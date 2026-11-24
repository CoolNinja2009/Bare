/* text.c: fonts, shaping, line breaking, the glyph cache and atlas
 * (ARCHITECTURE §13).
 *
 * A text element is shaped once when declared, at its logical size; layout
 * wraps it at its final width (pass 3); emit turns its lines into glyph
 * quads. Glyphs are rasterized at physical size (sharp at any DPI) with three
 * horizontal sub-pixel positions, cached by (font, glyph, size, position) and
 * packed into one R8 atlas by a shelf packer. New glyphs become one upload
 * per frame covering everything that changed. */
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define ATLAS_START 1024
#define ATLAS_MAX   2048
#define SUBPIXELS   3

/* ---- UTF-8 ---------------------------------------------------------------- */

uint32_t bare__utf8_decode(const char *str, int len, int *i) {
    const unsigned char *s = (const unsigned char *)str;
    int at = *i;
    unsigned c = s[at];
    int need;
    uint32_t cp, min;
    if (c < 0x80) { *i = at + 1; return c; }
    if (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; min = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; min = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; min = 0x10000; }
    else { *i = at + 1; return 0xFFFD; }   /* stray continuation byte or invalid lead */
    if (at + need >= len) { *i = at + 1; return 0xFFFD; }   /* cut off by the end of the text */
    for (int k = 1; k <= need; k++) {
        unsigned b = s[at + k];
        if ((b & 0xC0) != 0x80) { *i = at + 1; return 0xFFFD; }   /* truncated: resync at the next byte */
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { *i = at + 1; return 0xFFFD; }
    *i = at + need + 1;
    return cp;
}

/* ---- fonts ---------------------------------------------------------------- */

const BareFontBackend *bare__font_backend(const BareContext *ctx) {
    return ctx->font_backend ? ctx->font_backend : bare_font_backend_stb();
}

static bool fonts_reserve_default(BareContext *ctx) {
    if (ctx->font_count > 0) return true;
    if (!bare__grow(&ctx->alloc, (void **)&ctx->fonts, &ctx->font_cap, 1, sizeof *ctx->fonts)) return false;
    memset(&ctx->fonts[0], 0, sizeof ctx->fonts[0]);
    ctx->fonts[0].alive = true;
    ctx->font_count = 1;
    return true;
}

/* The backend's handle for a font, loading the default font on first use. */
static void *font_handle(BareContext *ctx, BareFont id) {
    if (!fonts_reserve_default(ctx)) return NULL;
    if ((int)id >= ctx->font_count || !ctx->fonts[id].alive) id = 0;
    BareFontRec *f = &ctx->fonts[id];
    if (f->handle || f->failed) return f->handle;
    if (id == 0) {
#ifdef BARE_HAS_DEFAULT_FONT
        f->handle = bare__font_backend(ctx)->load(&ctx->alloc, bare__default_font_data, bare__default_font_size);
#endif
        if (!f->handle) {
            f->failed = true;
            bare__log(ctx, "no default font (built with BARE_DEFAULT_FONT=OFF?): load one with bare_font_load "
                           "and pass it as .font");
        }
    }
    return f->handle;
}

static BareFont add_font(BareContext *ctx, const void *data, int size, void *owned, size_t owned_size,
                         const char *what) {
    void *handle = data && size > 0 ? bare__font_backend(ctx)->load(&ctx->alloc, data, size) : NULL;
    if (!handle || !fonts_reserve_default(ctx) ||
        !bare__grow(&ctx->alloc, (void **)&ctx->fonts, &ctx->font_cap, ctx->font_count + 1, sizeof *ctx->fonts)) {
        bare__log(ctx, "%s: not a font Bare can read (TTF or OTF); using the default font", what);
        if (handle) bare__font_backend(ctx)->unload(handle);
        bare__free(&ctx->alloc, owned, owned_size);
        return 0;
    }
    BareFontRec *f = &ctx->fonts[ctx->font_count];
    memset(f, 0, sizeof *f);
    f->handle = handle;
    f->owned = owned;
    f->owned_size = owned_size;
    f->alive = true;
    return (BareFont)ctx->font_count++;
}

BareFont bare_font_from_memory(const void *data, int size) {
    BareContext *ctx = bare_ctx_current();
    if (!ctx) { bare__log(NULL, "bare_font_from_memory called before bare_window"); return 0; }
    return add_font(ctx, data, size, NULL, 0, "bare_font_from_memory");
}

BareFont bare_font_load(const char *path) {
    BareContext *ctx = bare_ctx_current();
    if (!ctx) { bare__log(NULL, "bare_font_load called before bare_window"); return 0; }
    FILE *fp = path ? fopen(path, "rb") : NULL;
    if (!fp) { bare__log(ctx, "bare_font_load: can't open \"%s\"", path ? path : "(null)"); return 0; }
    long size = -1;
    if (fseek(fp, 0, SEEK_END) == 0) size = ftell(fp);
    void *data = NULL;
    if (size > 0 && size < (64L << 20) && fseek(fp, 0, SEEK_SET) == 0) {
        data = bare__alloc(&ctx->alloc, (size_t)size);
        if (data && fread(data, 1, (size_t)size, fp) != (size_t)size) {
            bare__free(&ctx->alloc, data, (size_t)size);
            data = NULL;
        }
    }
    fclose(fp);
    if (!data) { bare__log(ctx, "bare_font_load: can't read \"%s\"", path); return 0; }
    return add_font(ctx, data, (int)size, data, (size_t)size, path);
}

/* ---- shaping and measuring -------------------------------------------------- */

enum { CH_OTHER, CH_SPACE, CH_NEWLINE, CH_HYPHEN };

static int char_class(const BareText *t, int g) {
    unsigned char c = (unsigned char)t->str[t->glyphs[g].cluster];
    if (c == '\n') return CH_NEWLINE;
    if (c == ' ' || c == '\t' || c == '\r') return CH_SPACE;
    if (c == '-') return CH_HYPHEN;
    return CH_OTHER;
}

static float align_t(BareAlign al) {
    return al == BARE_CENTER ? 0.5f : al == BARE_END ? 1.0f : 0.0f;
}

/* CSS text-transform for ASCII and Latin-1 letters. Both cases have the same
 * UTF-8 length there, so byte offsets (carets, clusters) stay valid. */
static void transform_case(char *s, int len, BareTransform tr) {
    bool up = tr == BARE_UPPERCASE;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            if (up && c >= 'a' && c <= 'z') s[i] = (char)(c - 32);
            if (!up && c >= 'A' && c <= 'Z') s[i] = (char)(c + 32);
        } else if (c == 0xC3 && i + 1 < len) {   /* U+00C0..U+00FF */
            unsigned char d = (unsigned char)s[i + 1];
            if (up && d >= 0xA0 && d <= 0xBE && d != 0xB7) s[i + 1] = (char)(d - 0x20);   /* not the division sign */
            if (!up && d >= 0x80 && d <= 0x9E && d != 0x97) s[i + 1] = (char)(d + 0x20);  /* not the times sign */
            i++;
        }
    }
}

static void shape_ellipsis(BareContext *ctx, void *font, BareText *t, float spacing) {
    const BareFontBackend *fb = bare__font_backend(ctx);
    t->ell_count = fb->shape(font, t->size, "\xE2\x80\xA6", 3, t->ell, 3);
    if (t->ell_count != 1 || t->ell[0].glyph == 0) t->ell_count = fb->shape(font, t->size, "...", 3, t->ell, 3);
    t->ell_w = 0;
    for (int g = 0; g < t->ell_count; g++) {
        t->ell[g].cluster = 0;
        t->ell[g].advance += spacing;
        t->ell_w += t->ell[g].advance;
    }
}

BareText *bare__text_new(BareContext *ctx, const char *str, int len, const BareTextStyle *style) {
    BareText *t = bare__arena_alloc(&ctx->frame, sizeof *t, 16);
    char *copy = bare__arena_alloc(&ctx->frame, (size_t)len + 1, 1);
    if (!t || !copy) return NULL;
    memset(t, 0, sizeof *t);
    memcpy(copy, str, (size_t)len);
    if (style->transform) transform_case(copy, len, style->transform);
    t->str = copy;
    t->len = len;
    t->font = style->font;
    t->size = style->size > 0 ? style->size : 16.0f;
    t->line_height = t->size * (style->line_height > 0 ? style->line_height : 1.3f);
    t->align = style->align;
    t->nowrap = style->nowrap;
    t->ellipsis = style->ellipsis;
    t->underline = style->underline;
    t->strike = style->strike;
    t->max_lines = style->max_lines > 0 ? style->max_lines : 0;
    t->color = style->color;

    void *font = font_handle(ctx, t->font);
    if (!font) return t;
    const BareFontBackend *fb = bare__font_backend(ctx);
    float gap;
    fb->metrics(font, t->size, &t->ascent, &t->descent, &gap);
    if (len > 0) {
        t->glyphs = bare__arena_alloc(&ctx->frame, sizeof *t->glyphs * (size_t)len, 16);
        if (t->glyphs) t->glyph_count = fb->shape(font, t->size, copy, len, t->glyphs, len);
    }
    /* Letter spacing goes after every character, as in CSS, so carets and
     * hit tests (which walk the advances) agree with what's drawn. */
    float spacing = style->letter_spacing;
    if (spacing != 0)
        for (int g = 0; g < t->glyph_count; g++)
            if (copy[t->glyphs[g].cluster] != '\n') t->glyphs[g].advance += spacing;
    if (t->ellipsis) shape_ellipsis(ctx, font, t, spacing);

    /* Widest line as is, and the widest run that can't be broken. */
    float line = 0, ink = 0, word = 0;
    for (int g = 0; g < t->glyph_count; g++) {
        int c = char_class(t, g);
        float adv = t->glyphs[g].advance;
        if (c == CH_NEWLINE) {
            t->max_line = BARE__MAX(t->max_line, ink);
            line = ink = word = 0;
        } else if (c == CH_SPACE) {
            line += adv;
            word = 0;
        } else {
            line += adv;
            word += adv;
            ink = line;
            t->min_word = BARE__MAX(t->min_word, word);
            if (c == CH_HYPHEN) word = 0;
        }
    }
    t->max_line = BARE__MAX(t->max_line, ink);
    if (t->nowrap) t->min_word = t->max_line;
    if (t->nowrap && t->ellipsis) t->min_word = BARE__MIN(t->max_line, t->ell_w);   /* squeezable: it cuts */
    return t;
}

/* Width of glyphs [first, end), not counting trailing spaces. */
static float run_width(const BareText *t, int first, int end) {
    while (end > first && char_class(t, end - 1) != CH_OTHER && char_class(t, end - 1) != CH_HYPHEN) end--;
    float w = 0;
    for (int g = first; g < end; g++) w += t->glyphs[g].advance;
    return w;
}

static void add_line(BareText *t, int first, int end) {
    BareTextLine *l = &t->lines[t->line_count++];
    l->first = first;
    l->count = end - first;
    l->width = run_width(t, first, end);
    l->cut = false;
}

/* Ends a line in the ellipsis, dropping characters until both fit `width`.
 * `always`: the text goes on after this line (.max_lines), so it gets one
 * even when the line itself fits. */
static void cut_line(BareText *t, BareTextLine *l, float width, bool always) {
    if (!always && l->width <= width + 0.01f) return;
    int end = l->first + l->count;
    while (end > l->first && run_width(t, l->first, end) + t->ell_w > width + 0.01f) end--;
    while (end > l->first && char_class(t, end - 1) == CH_SPACE) end--;   /* "word...", not "word ..." */
    l->count = end - l->first;
    l->width = run_width(t, l->first, end) + t->ell_w;
    l->cut = t->ell_count > 0;
}

/* Greedy line breaking: at spaces (which hang off the end of a line) and
 * after hyphens; a word wider than the line breaks where it overflows. */
void bare__text_wrap(BareContext *ctx, BareText *t, float width) {
    int n = t->glyph_count;
    t->line_count = 0;
    t->lines = bare__arena_alloc(&ctx->frame, sizeof *t->lines * (size_t)(n + 1), 16);
    if (!t->lines) return;
    float limit = t->nowrap ? INFINITY : width + 0.01f;   /* a text given its own width never wraps */
    int start = 0, brk = -1;
    float x = 0;
    for (int g = 0; g < n; g++) {
        int c = char_class(t, g);
        float adv = t->glyphs[g].advance;
        if (c == CH_NEWLINE) {
            add_line(t, start, g);
            start = g + 1;
            x = 0;
            brk = -1;
            continue;
        }
        if (c == CH_SPACE) {
            if (g > start && char_class(t, g - 1) != CH_SPACE) brk = g;
            x += adv;
            continue;
        }
        while (x + adv > limit && g > start) {
            if (brk > start) {
                add_line(t, start, brk);
                start = brk;
                while (start < g && char_class(t, start) == CH_SPACE) start++;
            } else {
                add_line(t, start, g);   /* no break opportunity: break inside the word */
                start = g;
            }
            brk = -1;
            x = 0;
            for (int k = start; k < g; k++) x += t->glyphs[k].advance;
        }
        x += adv;
        if (c == CH_HYPHEN) brk = g + 1;
    }
    add_line(t, start, n);

    if (t->max_lines > 0 && t->line_count > t->max_lines) {
        t->line_count = t->max_lines;
        if (t->ellipsis) cut_line(t, &t->lines[t->line_count - 1], width, true);
    }
    if (t->ellipsis && t->nowrap)   /* text-overflow: ellipsis on each line too long for the box */
        for (int l = 0; l < t->line_count; l++)
            if (!t->lines[l].cut) cut_line(t, &t->lines[l], width, false);
}

float bare__text_height(const BareText *t) {
    return (float)BARE__MAX(t->line_count, 1) * t->line_height;
}

/* ---- the glyph cache ---------------------------------------------------------- */

static uint64_t glyph_key(BareFont font, uint32_t glyph, float px, int sub) {
    uint64_t q = (uint64_t)(px * 64.0f + 0.5f) & 0x3FFFF;   /* 1/64 px steps up to 4096 px */
    return (uint64_t)(glyph & 0xFFFF) | (q << 16) | ((uint64_t)sub << 34) | ((uint64_t)(font & 0xFFF) << 36) |
           (1ull << 63);
}

static BareGlyphSlot *slot_find(BareContext *ctx, uint64_t key, bool *found) {
    *found = false;
    if (!ctx->glyph_cap) return NULL;
    uint32_t mask = (uint32_t)ctx->glyph_cap - 1;
    for (uint32_t i = (uint32_t)bare__hash_u64(BARE__FNV_OFFSET, key) & mask;; i = (i + 1) & mask) {
        BareGlyphSlot *s = &ctx->glyphs[i];
        if (s->key == key) { *found = true; return s; }
        if (s->key == 0) return s;
    }
}

static bool slots_grow(BareContext *ctx) {
    int cap = ctx->glyph_cap ? ctx->glyph_cap * 2 : 512;
    BareGlyphSlot *old = ctx->glyphs;
    int old_cap = ctx->glyph_cap;
    BareGlyphSlot *s = bare__alloc(&ctx->alloc, sizeof *s * (size_t)cap);
    if (!s) return false;
    memset(s, 0, sizeof *s * (size_t)cap);
    ctx->glyphs = s;
    ctx->glyph_cap = cap;
    for (int i = 0; i < old_cap; i++) {
        if (!old[i].key) continue;
        bool found;
        *slot_find(ctx, old[i].key, &found) = old[i];
    }
    bare__free(&ctx->alloc, old, sizeof *old * (size_t)old_cap);
    return true;
}

static void mark_dirty(BareContext *ctx, int x0, int y0, int x1, int y1) {
    int *d = ctx->dirty;
    if (d[2] <= d[0]) { d[0] = x0; d[1] = y0; d[2] = x1; d[3] = y1; return; }
    d[0] = BARE__MIN(d[0], x0);
    d[1] = BARE__MIN(d[1], y0);
    d[2] = BARE__MAX(d[2], x1);
    d[3] = BARE__MAX(d[3], y1);
}

static bool atlas_resize(BareContext *ctx, int size) {
    unsigned char *a = bare__alloc(&ctx->alloc, (size_t)size * (size_t)size);
    if (!a) return false;
    memset(a, 0, (size_t)size * (size_t)size);
    for (int y = 0; y < ctx->atlas_h; y++)
        memcpy(a + (size_t)y * (size_t)size, ctx->atlas + (size_t)y * (size_t)ctx->atlas_w, (size_t)ctx->atlas_w);
    bare__free(&ctx->alloc, ctx->atlas, (size_t)ctx->atlas_w * (size_t)ctx->atlas_h);
    ctx->atlas = a;
    ctx->atlas_w = ctx->atlas_h = size;
    if (!ctx->atlas_id) ctx->atlas_id = bare__image_reserve(ctx, size, size);
    mark_dirty(ctx, 0, 0, size, size);   /* a new GPU texture: send all of it */
    return true;
}

/* Shelf packing: the shortest shelf the glyph fits on, or a new shelf below. */
static bool atlas_place(BareContext *ctx, int w, int h, int *ox, int *oy) {
    int pw = w + 1, ph = h + 1;   /* a 1-pixel gutter so bilinear sampling never bleeds */
    for (;;) {
        int best = -1, bottom = 0;
        for (int i = 0; i < ctx->shelf_count; i++) {
            BareShelf *s = &ctx->shelves[i];
            bottom = BARE__MAX(bottom, s->y + s->h);
            if (s->h >= ph && s->x + pw <= ctx->atlas_w && (best < 0 || s->h < ctx->shelves[best].h)) best = i;
        }
        bool wasteful = best >= 0 && ctx->shelves[best].h > ph + ph / 2;
        if (best >= 0 && !(wasteful && bottom + ph <= ctx->atlas_h)) {
            BareShelf *s = &ctx->shelves[best];
            *ox = s->x;
            *oy = s->y;
            s->x += pw;
            return true;
        }
        if (bottom + ph <= ctx->atlas_h && pw <= ctx->atlas_w) {
            if (!bare__grow(&ctx->alloc, (void **)&ctx->shelves, &ctx->shelf_cap, ctx->shelf_count + 1,
                            sizeof *ctx->shelves))
                return false;
            ctx->shelves[ctx->shelf_count++] = (BareShelf){ bottom, ph, 0 };
            continue;
        }
        if (ctx->atlas_w >= ATLAS_MAX || !atlas_resize(ctx, ctx->atlas_w * 2)) return false;
    }
}

/* Starts over when the atlas filled up last frame (glyphs skipped then are
 * drawn now). */
static void atlas_reset(BareContext *ctx) {
    if (ctx->glyphs) memset(ctx->glyphs, 0, sizeof *ctx->glyphs * (size_t)ctx->glyph_cap);
    ctx->glyph_used = 0;
    ctx->shelf_count = 0;
    if (ctx->atlas) memset(ctx->atlas, 0, (size_t)ctx->atlas_w * (size_t)ctx->atlas_h);
    mark_dirty(ctx, 0, 0, ctx->atlas_w, ctx->atlas_h);
    ctx->atlas_full = false;
}

void bare__text_begin_emit(BareContext *ctx) {
    if (ctx->atlas_full) atlas_reset(ctx);
}

/* The cached glyph, rasterizing it on first use. NULL when it can't be drawn. */
static const BareGlyphSlot *glyph_get(BareContext *ctx, void *font, BareFont font_id, uint32_t glyph, float px,
                                      int sub) {
    uint64_t key = glyph_key(font_id, glyph, px, sub);
    bool found;
    BareGlyphSlot *slot = slot_find(ctx, key, &found);
    if (found) return slot;
    if (ctx->atlas_full) return NULL;
    if ((ctx->glyph_used + 1) * 10 > ctx->glyph_cap * 7) {
        if (!slots_grow(ctx)) return NULL;
        slot = slot_find(ctx, key, &found);
    }
    if (!ctx->atlas && !atlas_resize(ctx, ATLAS_START)) return NULL;

    BareGlyphBitmap bm;
    ctx->raster_count++;
    if (!bare__font_backend(ctx)->rasterize(font, glyph, px, (float)sub / SUBPIXELS, &bm)) return NULL;
    BareGlyphSlot s = { key, 0, 0, (int16_t)bm.width, (int16_t)bm.height, (int16_t)bm.left, (int16_t)bm.top };
    if (bm.width > 0 && bm.height > 0) {
        int x, y;
        if (bm.width >= ATLAS_MAX || bm.height >= ATLAS_MAX || !atlas_place(ctx, bm.width, bm.height, &x, &y)) {
            if (bm.width < ATLAS_MAX && bm.height < ATLAS_MAX) {
                ctx->atlas_full = true;
                bare__log(ctx, "the glyph atlas is full (%dx%d): starting it over next frame", ctx->atlas_w,
                          ctx->atlas_h);
            }
            return NULL;
        }
        for (int r = 0; r < bm.height; r++)
            memcpy(ctx->atlas + (size_t)(y + r) * (size_t)ctx->atlas_w + (size_t)x,
                   bm.pixels + (size_t)r * (size_t)bm.stride, (size_t)bm.width);
        mark_dirty(ctx, x, y, x + bm.width, y + bm.height);
        s.x = (int16_t)x;
        s.y = (int16_t)y;
    }
    *slot = s;
    ctx->glyph_used++;
    return slot;
}

/* ---- drawing ---------------------------------------------------------------- */

typedef struct Pen {
    BareContext *ctx; void *font; const BareText *t;
    float k, vscale, px, base, x; bool snap;
    BareGlyph *out; int count;
} Pen;

static void put_glyph(Pen *p, const BareShapedGlyph *sg, bool blank) {
    BareContext *ctx = p->ctx;
    float x = p->x + sg->x_offset * p->k;
    p->x += sg->advance * p->k;
    if (blank) return;
    int sub = 0;
    if (p->snap) {   /* whole pixels plus one of three sub-pixel phases */
        float ix = floorf(x);
        sub = (int)((x - ix) * SUBPIXELS + 0.5f);
        x = ix;
        if (sub == SUBPIXELS) { sub = 0; x += 1; }
    }
    const BareGlyphSlot *s = glyph_get(ctx, p->font, p->t->font, sg->glyph, p->px, sub);
    if (!s) { ctx->text_redraw = ctx->text_redraw || ctx->atlas_full; return; }
    if (s->w <= 0 || s->h <= 0) return;
    BareGlyph *o = &p->out[p->count++];
    o->dst = (BareRect){ x + (float)s->left * p->vscale, p->base + (sg->y_offset * p->k) + (float)s->top * p->vscale,
                         (float)s->w * p->vscale, (float)s->h * p->vscale };
    o->uv = (BareRect){ (float)s->x, (float)s->y, (float)s->w, (float)s->h };   /* pixels until end_frame */
}

/* Underline and line-through: a bar per line in the text's colour, at least
 * a physical pixel thick, on whole pixels when the text is. */
static void line_bar(BareContext *ctx, float x0, float x1, float centre, float thick, bool snap, BareColorF color) {
    if (snap) {
        thick = fmaxf(1.0f, roundf(thick));
        centre = roundf(centre - thick * 0.5f) + thick * 0.5f;
        x0 = roundf(x0);
        x1 = roundf(x1);
    }
    bare__emit_fill(ctx, (BareRect){ x0, centre - thick * 0.5f, x1 - x0, thick }, color);
}

void bare__text_emit(BareContext *ctx, const BareText *t, BareRect r, float vscale, bool snap, BareColorF color) {
    if (!t || !t->glyph_count || !t->line_count || color.a <= 0) return;
    void *font = font_handle(ctx, t->font);
    if (!font) return;
    int cap = t->glyph_count + t->line_count * 3;   /* room for an ellipsis per line */
    BareGlyph *out = bare__arena_alloc(&ctx->frame, sizeof *out * (size_t)cap, 16);
    if (!out) return;

    float k = r.w > 0 ? ctx->scale * vscale : 0;      /* logical -> physical, visual scale included */
    float lead = (t->line_height - (t->ascent + t->descent)) * 0.5f;   /* half-leading, as in CSS */
    float box_w = k > 0 ? r.w / k : 0;
    Pen p = { ctx, font, t, k, vscale, t->size * ctx->scale, 0, 0, snap, out, 0 };   /* rasterized at physical size */
    for (int l = 0; l < t->line_count; l++) {
        const BareTextLine *line = &t->lines[l];
        p.base = r.y + ((float)l * t->line_height + lead + t->ascent) * k;
        if (snap) p.base = roundf(p.base);
        p.x = r.x + (box_w - line->width) * align_t(t->align) * k;
        for (int g = line->first; g < line->first + line->count; g++)   /* spaces and controls draw nothing */
            put_glyph(&p, &t->glyphs[g], (unsigned char)t->str[t->glyphs[g].cluster] <= ' ');
        if (line->cut)
            for (int g = 0; g < t->ell_count; g++) put_glyph(&p, &t->ell[g], false);
    }
    if (p.count) {
        if (!bare__grow(&ctx->alloc, (void **)&ctx->cmds, &ctx->cmd_cap, ctx->cmd_count + 1, sizeof *ctx->cmds)) return;
        BareDrawCmd *cmd = &ctx->cmds[ctx->cmd_count++];
        cmd->kind = BARE_CMD_TEXT;
        cmd->as.text = (BareTextCmd){ ctx->atlas_id, color, out, p.count };
    }
    if (!t->underline && !t->strike) return;
    float thick = fmaxf(t->size / 14.0f * k, 1.0f);
    for (int l = 0; l < t->line_count; l++) {
        const BareTextLine *line = &t->lines[l];
        if (line->width <= 0) continue;
        float base = r.y + ((float)l * t->line_height + lead + t->ascent) * k;
        float x0 = r.x + (box_w - line->width) * align_t(t->align) * k, x1 = x0 + line->width * k;
        if (t->underline) line_bar(ctx, x0, x1, base + t->size * 0.11f * k, thick, snap, color);
        if (t->strike) line_bar(ctx, x0, x1, base - t->size * 0.28f * k, thick, snap, color);
    }
}

/* After emit: atlas uvs become 0..1 (the atlas may have grown mid-frame) and
 * what changed in the atlas becomes this frame's upload. */
void bare__text_end_frame(BareContext *ctx) {
    BareDrawList *l = &ctx->list;
    l->uploads = NULL;
    l->upload_count = 0;
    if (!ctx->atlas) return;
    float iw = 1.0f / (float)ctx->atlas_w, ih = 1.0f / (float)ctx->atlas_h;
    for (int i = 0; i < ctx->cmd_count; i++) {
        BareDrawCmd *c = &ctx->cmds[i];
        if (c->kind != BARE_CMD_TEXT) continue;
        BareGlyph *g = (BareGlyph *)c->as.text.glyphs;
        for (int j = 0; j < c->as.text.count; j++) {
            g[j].uv.x *= iw; g[j].uv.w *= iw;
            g[j].uv.y *= ih; g[j].uv.h *= ih;
        }
    }
    int *d = ctx->dirty;
    if (d[2] <= d[0] || d[3] <= d[1]) return;
    BareTextureUpload *u = &ctx->upload;
    u->texture = ctx->atlas_id;
    u->width = ctx->atlas_w;
    u->height = ctx->atlas_h;
    u->format = BARE_TEX_R8;
    u->x = d[0];
    u->y = d[1];
    u->w = d[2] - d[0];
    u->h = d[3] - d[1];
    u->pixels = ctx->atlas + (size_t)d[1] * (size_t)ctx->atlas_w + (size_t)d[0];
    u->stride = ctx->atlas_w;
    u->destroy = false;
    l->uploads = u;
    l->upload_count = 1;
    d[0] = d[1] = d[2] = d[3] = 0;
}

void bare__text_free(BareContext *ctx) {
    const BareFontBackend *fb = bare__font_backend(ctx);
    for (int i = 0; i < ctx->font_count; i++) {
        BareFontRec *f = &ctx->fonts[i];
        if (f->handle) fb->unload(f->handle);
        bare__free(&ctx->alloc, f->owned, f->owned_size);
    }
    bare__free(&ctx->alloc, ctx->fonts, sizeof *ctx->fonts * (size_t)ctx->font_cap);
    bare__free(&ctx->alloc, ctx->glyphs, sizeof *ctx->glyphs * (size_t)ctx->glyph_cap);
    bare__free(&ctx->alloc, ctx->atlas, (size_t)ctx->atlas_w * (size_t)ctx->atlas_h);
    bare__free(&ctx->alloc, ctx->shelves, sizeof *ctx->shelves * (size_t)ctx->shelf_cap);
}
