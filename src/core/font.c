/* font.c: fonts, families, the glyph cache and the atlas (ARCHITECTURE §13).
 *
 * Every font is a face; faces group into families by weight and italic, so
 * text can ask for `.weight = BARE_BOLD, .italic = true` and get the nearest
 * face the family has (CSS font matching). The built-in faces have fixed ids:
 * 0 is Inter Regular (the default), 1 JetBrains Mono (BARE_MONO), then Inter's
 * SemiBold, Bold, Italic and Bold Italic and the mono's Bold. They load on
 * first use.
 *
 * Glyphs are rasterized at physical size (sharp at any DPI) with three
 * horizontal sub-pixel positions, cached by (face, glyph, size, position) and
 * packed into one R8 atlas by a shelf packer. New glyphs become one upload
 * per frame covering everything that changed. */
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ATLAS_START 1024
#define ATLAS_MAX   2048
#define SUBPIXELS   3

typedef struct Builtin { BareFont family; int weight; bool italic; const unsigned char *data; const int *size; } Builtin;

#ifdef BARE_HAS_DEFAULT_FONT
#  define DATA(name) bare__##name##_data, &bare__##name##_size
#else
#  define DATA(name) NULL, NULL
#endif

/* In id order. */
static const Builtin builtins[BARE__FONT_BUILTIN] = {
    { 0, 400, false, DATA(default_font) },
    { 1, 400, false, DATA(font_mono) },
    { 0, 600, false, DATA(font_sans_semibold) },
    { 0, 700, false, DATA(font_sans_bold) },
    { 0, 400, true,  DATA(font_sans_italic) },
    { 0, 700, true,  DATA(font_sans_bold_italic) },
    { 1, 700, false, DATA(font_mono_bold) },
};

const BareFontBackend *bare__font_backend(const BareContext *ctx) {
    return ctx->font_backend ? ctx->font_backend : bare_font_backend_stb();
}

static bool fonts_reserve(BareContext *ctx) {
    if (ctx->font_count > 0) return true;
    if (!bare__grow(&ctx->alloc, (void **)&ctx->fonts, &ctx->font_cap, BARE__FONT_BUILTIN, sizeof *ctx->fonts))
        return false;
    for (int i = 0; i < BARE__FONT_BUILTIN; i++) {
        BareFontRec *f = &ctx->fonts[i];
        memset(f, 0, sizeof *f);
        f->alive = true;
        f->family = builtins[i].family;
        f->weight = (int16_t)builtins[i].weight;
        f->italic = builtins[i].italic;
    }
    ctx->font_count = BARE__FONT_BUILTIN;
    return true;
}

static void *load_builtin(BareContext *ctx, BareFont id) {
    BareFontRec *f = &ctx->fonts[id];
    const Builtin *b = &builtins[id];
    if (b->data) f->handle = bare__font_backend(ctx)->load(&ctx->alloc, b->data, *b->size);
    if (!f->handle) {
        f->failed = true;
        if (id == 0)
            bare__log(ctx, "no default font (built with BARE_DEFAULT_FONT=OFF?): load one with bare_font_load "
                           "and pass it as .font");
    }
    return f->handle;
}

/* The backend's handle for a face, loading a built-in one on first use. A
 * face that can't load stands in with its family's root, then the default. */
void *bare__font_handle(BareContext *ctx, BareFont *id) {
    if (!fonts_reserve(ctx)) return NULL;
    BareFont f = *id;
    for (int tries = 0; tries < 3; tries++) {
        if (f >= (BareFont)ctx->font_count || !ctx->fonts[f].alive) f = 0;
        BareFontRec *r = &ctx->fonts[f];
        void *h = r->handle;
        if (!h && !r->failed && f < BARE__FONT_BUILTIN) h = load_builtin(ctx, f);
        if (h) { *id = f; return h; }
        if (f == 0) break;
        f = r->family != f ? r->family : 0;
    }
    *id = 0;
    return NULL;
}

/* CSS font matching, by weight: from 400 to 500 look up to 500 first, then
 * down, then up; below that down first; above it up first. */
static int weight_cost(int want, int have) {
    if (have == want) return 0;
    if (want >= 400 && want <= 500) {
        if (have > want && have <= 500) return have - want;
        if (have < want) return 1000 + (want - have);
        return 2000 + (have - want);
    }
    if (want < 400) return have < want ? want - have : 1000 + (have - want);
    return have > want ? have - want : 1000 + (want - have);
}

BareFont bare__font_face(BareContext *ctx, BareFont font, int weight, bool italic) {
    if (!fonts_reserve(ctx)) return font;
    if (font >= (BareFont)ctx->font_count || !ctx->fonts[font].alive) font = 0;
    if (weight <= 0 && !italic) return font;
    const BareFontRec *self = &ctx->fonts[font];
    if (weight <= 0) weight = self->weight;
    BareFont family = self->family, best = font;
    int best_cost = 1 << 30;
    for (int i = 0; i < ctx->font_count; i++) {
        const BareFontRec *f = &ctx->fonts[i];
        if (!f->alive || f->family != family || f->failed) continue;
        int cost = weight_cost(weight, f->weight) + (f->italic != italic ? 100000 : 0);
        if (cost < best_cost) { best_cost = cost; best = (BareFont)i; }
    }
    return best;
}

static BareFont add_font(BareContext *ctx, const void *data, int size, void *owned, size_t owned_size,
                         const char *what) {
    void *handle = data && size > 0 ? bare__font_backend(ctx)->load(&ctx->alloc, data, size) : NULL;
    if (!handle || !fonts_reserve(ctx) ||
        !bare__grow(&ctx->alloc, (void **)&ctx->fonts, &ctx->font_cap, ctx->font_count + 1, sizeof *ctx->fonts)) {
        bare__log(ctx, "%s: not a font Bare can read (TTF or OTF); using the default font", what);
        if (handle) bare__font_backend(ctx)->unload(handle);
        bare__free(&ctx->alloc, owned, owned_size);
        return 0;
    }
    BareFont id = (BareFont)ctx->font_count++;
    BareFontRec *f = &ctx->fonts[id];
    memset(f, 0, sizeof *f);
    f->handle = handle;
    f->owned = owned;
    f->owned_size = owned_size;
    f->alive = true;
    f->family = id;   /* its own family until added to another */
    f->weight = 400;
    return id;
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

void bare_font_add_face(BareFont family, BareFont face, int weight, bool italic) {
    BareContext *ctx = bare_ctx_current();
    if (!ctx || !fonts_reserve(ctx)) return;
    if ((int)family >= ctx->font_count || (int)face >= ctx->font_count || !ctx->fonts[family].alive ||
        !ctx->fonts[face].alive || face < BARE__FONT_BUILTIN) {
        bare__log(ctx, "bare_font_add_face: %u and %u must be fonts you loaded", family, face);
        return;
    }
    BareFontRec *f = &ctx->fonts[face];
    f->family = ctx->fonts[family].family;
    f->weight = (int16_t)(weight > 0 ? weight : 400);
    f->italic = italic;
}

/* ---- text into memory ------------------------------------------------------------ */

/* Two walks over the shaped run: the first finds the box every glyph covers
 * (pen positions kept fractional, as the glyph cache does), the second adds
 * each glyph's coverage into it. */
unsigned char *bare_text_pixels(const char *text, BareFont font, float size, int *width, int *height, int *x,
                                int *y, float *advance) {
    if (width) *width = 0;
    if (height) *height = 0;
    if (x) *x = 0;
    if (y) *y = 0;
    if (advance) *advance = 0;
    BareContext *ctx = bare_ctx_current();
    if (!ctx || !text || !(size > 0)) return NULL;
    void *h = bare__font_handle(ctx, &font);
    if (!h) return NULL;
    const BareFontBackend *fb = bare__font_backend(ctx);
    int len = (int)strlen(text);
    if (!len) return NULL;
    BareShapedGlyph *g = malloc((size_t)len * sizeof *g);
    if (!g) return NULL;
    int n = fb->shape(h, size, text, len, g, len);
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    float pen = 0;
    for (int pass = 0; pass < 2; pass++) {
        unsigned char *out = NULL;
        int w = x1 - x0, hh = y1 - y0;
        if (pass == 1) {
            if (w <= 0 || hh <= 0) break;
            out = calloc((size_t)w * (size_t)hh, 1);
            if (!out) break;
        }
        pen = 0;
        for (int i = 0; i < n; i++) {
            float fx = floorf(pen);
            BareGlyphBitmap bm;
            if (fb->rasterize(h, g[i].glyph, size, pen - fx, &bm) && bm.width > 0) {
                int gx = (int)fx + bm.left + (int)lroundf(g[i].x_offset), gy = bm.top + (int)lroundf(g[i].y_offset);
                if (pass == 0) {
                    if (gx < x0) x0 = gx;
                    if (gy < y0) y0 = gy;
                    if (gx + bm.width > x1) x1 = gx + bm.width;
                    if (gy + bm.height > y1) y1 = gy + bm.height;
                } else {
                    for (int r = 0; r < bm.height; r++) {
                        unsigned char *d = out + (size_t)(gy - y0 + r) * (size_t)w + (gx - x0);
                        const unsigned char *s = bm.pixels + (size_t)r * (size_t)bm.stride;
                        for (int c = 0; c < bm.width; c++) {
                            int v = d[c] + s[c];
                            d[c] = (unsigned char)(v > 255 ? 255 : v);
                        }
                    }
                }
            }
            pen += g[i].advance;
        }
        if (pass == 1) {
            free(g);
            if (width) *width = w;
            if (height) *height = hh;
            if (x) *x = x0;
            if (y) *y = y0;
            if (advance) *advance = pen;
            return out;
        }
    }
    free(g);
    if (advance) *advance = pen;
    return NULL;
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

const BareGlyphSlot *bare__glyph_get(BareContext *ctx, void *font, BareFont font_id, uint32_t glyph, float px,
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
    bare__free(&ctx->alloc, ctx->para_buf, (size_t)ctx->para_cap);
    bare__free(&ctx->alloc, ctx->para_spans, sizeof *ctx->para_spans * (size_t)ctx->para_span_cap);
}
