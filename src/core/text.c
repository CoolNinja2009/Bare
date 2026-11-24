/* text.c: shaping, line breaking and drawing text (ARCHITECTURE §13).
 *
 * A text element is shaped once when declared, at its logical size; layout
 * wraps it at its final width (pass 3); emit turns its lines into glyph
 * quads. A text is one or more runs (bare_span), each with its own face,
 * size, colour, background and lines; runs flow together and wrap as one
 * paragraph. Lines are as tall as their tallest run (CSS line boxes: every
 * run's line height split evenly above and below it, the paragraph's own
 * line as the minimum, so empty lines keep their height). */
#include "internal.h"
#include <math.h>
#include <string.h>

#define SUBPIXELS 3

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

/* What a span changes: the fields it sets win over the paragraph's. */
static BareTextStyle span_style(const BareTextStyle *base, const BareTextStyle *s) {
    BareTextStyle o = *base;
    if (!s) return o;
    if (s->font) o.font = s->font;
    if (s->size > 0) o.size = s->size;
    if (s->color.a || s->color.r || s->color.g || s->color.b) o.color = s->color;
    if (s->background.a) o.background = s->background;
    if (s->weight) o.weight = s->weight;
    if (s->italic) o.italic = true;
    if (s->underline) o.underline = true;
    if (s->strike) o.strike = true;
    if (s->letter_spacing != 0) o.letter_spacing = s->letter_spacing;
    if (s->transform) o.transform = s->transform;
    return o;
}

static void set_run(BareContext *ctx, BareTextRun *r, int start, const BareTextStyle *st, float lh_mult) {
    memset(r, 0, sizeof *r);
    r->start = start;
    r->font = bare__font_face(ctx, st->font, st->weight, st->italic);
    r->size = st->size > 0 ? st->size : 16.0f;
    r->lh = r->size * lh_mult;
    r->color = st->color;
    r->background = st->background;
    r->underline = st->underline;
    r->strike = st->strike;
    r->spacing = st->letter_spacing;
    void *h = bare__font_handle(ctx, &r->font);
    float gap;
    if (h) bare__font_backend(ctx)->metrics(h, r->size, &r->ascent, &r->descent, &gap);
}

/* Tabs advance to the next stop, every `tab_size` spaces of the paragraph's
 * font, counted from the start of the line as written. */
static void tab_stops(BareContext *ctx, BareText *t, int tab_size) {
    if (!memchr(t->str, '\t', (size_t)t->len)) return;
    BareFont f = t->font;
    void *h = bare__font_handle(ctx, &f);
    BareShapedGlyph sp;
    float space = t->size * 0.25f;
    if (h && bare__font_backend(ctx)->shape(h, t->size, " ", 1, &sp, 1) == 1 && sp.advance > 0) space = sp.advance;
    float stop = space * (float)(tab_size > 0 ? tab_size : 4);
    float x = 0;
    for (int g = 0; g < t->glyph_count; g++) {
        char c = t->str[t->glyphs[g].cluster];
        if (c == '\n') { x = 0; continue; }
        if (c == '\t') t->glyphs[g].advance = stop - fmodf(x, stop);
        x += t->glyphs[g].advance;
    }
}

BareText *bare__text_runs(BareContext *ctx, const char *str, int len, const BareTextStyle *style,
                          const BareSpanIn *spans, int span_count) {
    BareText *t = bare__arena_alloc(&ctx->frame, sizeof *t, 16);
    char *copy = bare__arena_alloc(&ctx->frame, (size_t)len + 1, 1);
    int nr = span_count > 0 ? span_count : 1;
    BareTextRun *runs = bare__arena_alloc(&ctx->frame, sizeof *runs * (size_t)nr, 16);
    if (!t || !copy || !runs) return NULL;
    memset(t, 0, sizeof *t);
    memcpy(copy, str, (size_t)len);
    t->str = copy;
    t->len = len;
    t->size = style->size > 0 ? style->size : 16.0f;
    float lh_mult = style->line_height > 0 ? style->line_height : 1.3f;
    t->line_height = t->size * lh_mult;
    t->align = style->align;
    t->nowrap = style->nowrap;
    t->ellipsis = style->ellipsis;
    t->max_lines = style->max_lines > 0 ? style->max_lines : 0;
    t->color = style->color;
    t->shadow = style->shadow;
    t->font = bare__font_face(ctx, style->font, style->weight, style->italic);
    t->runs = runs;
    t->run_count = nr;

    void *font = bare__font_handle(ctx, &t->font);
    const BareFontBackend *fb = bare__font_backend(ctx);
    float gap;
    if (font) fb->metrics(font, t->size, &t->ascent, &t->descent, &gap);

    /* Runs: one for plain text; one per span otherwise. */
    if (span_count <= 0) {
        set_run(ctx, &runs[0], 0, style, lh_mult);
        if (style->transform) transform_case(copy, len, style->transform);
    } else {
        for (int i = 0; i < span_count; i++) {
            BareTextStyle st = span_style(style, &spans[i].style);
            set_run(ctx, &runs[i], spans[i].start, &st, lh_mult);
            int end = i + 1 < span_count ? spans[i + 1].start : len;
            if (st.transform) transform_case(copy + spans[i].start, end - spans[i].start, st.transform);
        }
    }
    t->uniform = true;
    for (int i = 0; i < nr; i++)
        if (fabsf(runs[i].lh - t->line_height) > 0.01f || fabsf(runs[i].ascent - t->ascent) > 0.01f ||
            fabsf(runs[i].descent - t->descent) > 0.01f)
            t->uniform = false;

    if (len > 0) {
        t->glyphs = bare__arena_alloc(&ctx->frame, sizeof *t->glyphs * (size_t)len, 16);
        if (nr > 1) t->grun = bare__arena_alloc(&ctx->frame, sizeof *t->grun * (size_t)len, 16);
        if (t->glyphs && (nr == 1 || t->grun)) {
            for (int i = 0; i < nr; i++) {
                int a = runs[i].start, b = i + 1 < nr ? runs[i + 1].start : len;
                BareFont f = runs[i].font;
                void *h = bare__font_handle(ctx, &f);
                if (!h || b <= a) continue;
                int n = fb->shape(h, runs[i].size, copy + a, b - a, t->glyphs + t->glyph_count, len - t->glyph_count);
                /* Letter spacing goes after every character, as in CSS, so carets
                 * and hit tests (which walk the advances) agree with what's drawn. */
                for (int g = t->glyph_count; g < t->glyph_count + n; g++) {
                    t->glyphs[g].cluster += (uint32_t)a;
                    if (runs[i].spacing != 0 && copy[t->glyphs[g].cluster] != '\n')
                        t->glyphs[g].advance += runs[i].spacing;
                    if (t->grun) t->grun[g] = (uint16_t)i;
                }
                t->glyph_count += n;
            }
        }
    }
    tab_stops(ctx, t, style->tab_size);
    if (t->ellipsis && font) shape_ellipsis(ctx, font, t, style->letter_spacing);

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

BareText *bare__text_new(BareContext *ctx, const char *str, int len, const BareTextStyle *style) {
    return bare__text_runs(ctx, str, len, style, NULL, 0);
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
    memset(l, 0, sizeof *l);
    l->first = first;
    l->count = end - first;
    l->width = run_width(t, first, end);
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

/* Line boxes: each line as tall as what's on it, the paragraph's own line
 * height at least. */
static void line_metrics(BareText *t) {
    float lead = (t->line_height - (t->ascent + t->descent)) * 0.5f;
    float sa = lead + t->ascent, sd = lead + t->descent, y = 0;
    for (int l = 0; l < t->line_count; l++) {
        BareTextLine *line = &t->lines[l];
        float a = sa, d = sd;
        if (!t->uniform && t->grun) {
            int last = -1;
            for (int g = line->first; g < line->first + line->count; g++) {
                if (t->grun[g] == last) continue;
                last = t->grun[g];
                const BareTextRun *r = &t->runs[last];
                float rl = (r->lh - (r->ascent + r->descent)) * 0.5f;
                a = fmaxf(a, rl + r->ascent);
                d = fmaxf(d, rl + r->descent);
            }
        } else if (!t->uniform) {
            const BareTextRun *r = &t->runs[0];
            float rl = (r->lh - (r->ascent + r->descent)) * 0.5f;
            a = fmaxf(a, rl + r->ascent);
            d = fmaxf(d, rl + r->descent);
        }
        line->y = y;
        line->asc = a;
        line->h = a + d;
        y += line->h;
    }
    t->height = t->line_count ? y : t->line_height;
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
    line_metrics(t);
}

float bare__text_height(const BareText *t) {
    return t->line_count ? t->height : t->line_height;
}

int bare__text_line_at_y(const BareText *t, float y) {
    int lo = 0, hi = t->line_count - 1;
    if (hi < 0) return 0;
    while (lo < hi) {   /* the last line starting at or above y */
        int mid = (lo + hi + 1) / 2;
        if (t->lines[mid].y <= y) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

/* ---- drawing ---------------------------------------------------------------- */

static BareColorF color_f(BareColor c, float opacity) {
    return (BareColorF){ c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (c.a / 255.0f) * opacity };
}

static bool same_color(BareColorF a, BareColorF b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

/* Glyphs and their colour changes, collected before becoming commands. */
typedef struct Pen {
    BareContext *ctx; const BareText *t;
    void **handles; BareFont *faces;   /* per run */
    float k, vscale, base, x; bool snap;
    BareGlyph *out; int count;
    int *seg_at; BareColorF *seg_color; int seg_count;
} Pen;

static void put_glyph(Pen *p, const BareShapedGlyph *sg, int run, bool blank, BareColorF color) {
    BareContext *ctx = p->ctx;
    float x = p->x + sg->x_offset * p->k;
    p->x += sg->advance * p->k;
    if (blank || !p->handles[run]) return;
    int sub = 0;
    if (p->snap) {   /* whole pixels plus one of three sub-pixel phases */
        float ix = floorf(x);
        sub = (int)((x - ix) * SUBPIXELS + 0.5f);
        x = ix;
        if (sub == SUBPIXELS) { sub = 0; x += 1; }
    }
    float px = p->t->runs[run].size * ctx->scale;   /* rasterized at physical size */
    const BareGlyphSlot *s = bare__glyph_get(ctx, p->handles[run], p->faces[run], sg->glyph, px, sub);
    if (!s) { ctx->text_redraw = ctx->text_redraw || ctx->atlas_full; return; }
    if (s->w <= 0 || s->h <= 0) return;
    if (p->seg_count == 0 || !same_color(p->seg_color[p->seg_count - 1], color)) {
        p->seg_at[p->seg_count] = p->count;
        p->seg_color[p->seg_count++] = color;
    }
    BareGlyph *o = &p->out[p->count++];
    o->dst = (BareRect){ x + (float)s->left * p->vscale, p->base + (sg->y_offset * p->k) + (float)s->top * p->vscale,
                         (float)s->w * p->vscale, (float)s->h * p->vscale };
    o->uv = (BareRect){ (float)s->x, (float)s->y, (float)s->w, (float)s->h };   /* pixels until end_frame */
}

static void push_text(BareContext *ctx, const BareGlyph *g, int count, BareColorF color, float blur) {
    if (count <= 0) return;
    if (!bare__grow(&ctx->alloc, (void **)&ctx->cmds, &ctx->cmd_cap, ctx->cmd_count + 1, sizeof *ctx->cmds)) return;
    BareDrawCmd *cmd = &ctx->cmds[ctx->cmd_count++];
    cmd->kind = BARE_CMD_TEXT;
    cmd->as.text = (BareTextCmd){ ctx->atlas_id, color, g, count, blur };
}

/* A filled, rounded rect (run backgrounds), snapped when the text is. */
static void fill(BareContext *ctx, BareRect r, float radius, bool snap, BareColorF color) {
    if (snap) {
        float x0 = roundf(r.x), y0 = roundf(r.y), x1 = roundf(r.x + r.w), y1 = roundf(r.y + r.h);
        r = (BareRect){ x0, y0, x1 - x0, y1 - y0 };
    }
    if (r.w <= 0 || r.h <= 0 || color.a <= 0) return;
    if (!bare__grow(&ctx->alloc, (void **)&ctx->cmds, &ctx->cmd_cap, ctx->cmd_count + 1, sizeof *ctx->cmds)) return;
    BareDrawCmd *cmd = &ctx->cmds[ctx->cmd_count++];
    memset(cmd, 0, sizeof *cmd);
    cmd->kind = BARE_CMD_RECT;
    cmd->as.rect.rect = r;
    cmd->as.rect.fill = color;
    for (int i = 0; i < 4; i++) cmd->as.rect.radius[i] = fminf(radius, fminf(r.w, r.h) * 0.5f);
}

/* Underline and line-through: a bar in the run's colour, at least a physical
 * pixel thick, on whole pixels when the text is. */
static void line_bar(BareContext *ctx, float x0, float x1, float centre, float thick, bool snap, BareColorF color) {
    if (snap) {
        thick = fmaxf(1.0f, roundf(thick));
        centre = roundf(centre - thick * 0.5f) + thick * 0.5f;
        x0 = roundf(x0);
        x1 = roundf(x1);
    }
    bare__emit_fill(ctx, (BareRect){ x0, centre - thick * 0.5f, x1 - x0, thick }, color);
}

static int run_of(const BareText *t, int g) { return t->grun ? t->grun[g] : 0; }

/* A run's background, underline and strike on one line: the pieces of the
 * line belonging to one run, ink only (hanging spaces excluded). */
static void run_decorations(BareContext *ctx, const BareText *t, int l, float x0, float base, float k, bool snap,
                            BareColorF ink, float opacity, bool under) {
    const BareTextLine *line = &t->lines[l];
    int end = line->first + line->count;
    float pen = x0;
    for (int g = line->first; g < end;) {
        int r = run_of(t, g), e = g;
        float a = pen, b = pen;
        while (e < end && run_of(t, e) == r) {
            pen += t->glyphs[e].advance * k;
            if (char_class(t, e) != CH_SPACE || e + 1 < end) b = pen;
            e++;
        }
        const BareTextRun *run = &t->runs[r];
        BareColorF c = run->color.a ? color_f(run->color, opacity) : ink;
        if (under && run->background.a) {
            float pad = run->size * 0.12f * k;
            fill(ctx, (BareRect){ a - pad, base - (run->ascent + run->size * 0.08f) * k, b - a + 2 * pad,
                                  (run->ascent + run->descent + run->size * 0.16f) * k },
                 run->size * 0.2f * k, snap, color_f(run->background, opacity));
        }
        if (!under) {
            float thick = fmaxf(run->size / 14.0f * k, 1.0f);
            if (run->underline) line_bar(ctx, a, b, base + run->size * 0.11f * k, thick, snap, c);
            if (run->strike) line_bar(ctx, a, b, base - run->size * 0.28f * k, thick, snap, c);
        }
        g = e;
    }
}

void bare__text_emit(BareContext *ctx, const BareText *t, BareRect r, float vscale, bool snap, BareColorF color,
                     float opacity) {
    if (!t || !t->line_count) return;
    int cap = t->glyph_count + t->line_count * 3;   /* room for an ellipsis per line */
    Pen p;
    memset(&p, 0, sizeof p);
    p.out = bare__arena_alloc(&ctx->frame, sizeof *p.out * (size_t)(cap + 1), 16);
    p.seg_at = bare__arena_alloc(&ctx->frame, sizeof *p.seg_at * (size_t)(cap + 1), 16);
    p.seg_color = bare__arena_alloc(&ctx->frame, sizeof *p.seg_color * (size_t)(cap + 1), 16);
    p.handles = bare__arena_alloc(&ctx->frame, sizeof *p.handles * (size_t)t->run_count, 16);
    p.faces = bare__arena_alloc(&ctx->frame, sizeof *p.faces * (size_t)t->run_count, 16);
    if (!p.out || !p.seg_at || !p.seg_color || !p.handles || !p.faces) return;
    bool decorated = false, backgrounds = false;
    for (int i = 0; i < t->run_count; i++) {
        p.faces[i] = t->runs[i].font;
        p.handles[i] = bare__font_handle(ctx, &p.faces[i]);
        decorated = decorated || t->runs[i].underline || t->runs[i].strike;
        backgrounds = backgrounds || t->runs[i].background.a;
    }
    BareFont ell_face = t->font;
    void *ell_font = t->ell_count ? bare__font_handle(ctx, &ell_face) : NULL;

    float k = r.w > 0 ? ctx->scale * vscale : 0;      /* logical -> physical, visual scale included */
    if (k <= 0) return;
    float box_w = r.w / k;
    p.ctx = ctx; p.t = t; p.k = k; p.vscale = vscale; p.snap = snap;
    for (int pass = 0; pass < 2; pass++) {   /* backgrounds under everything, then the glyphs */
        if (pass == 0 && !backgrounds) continue;
        for (int l = 0; l < t->line_count; l++) {
            const BareTextLine *line = &t->lines[l];
            float base = r.y + (line->y + line->asc) * k;
            if (snap) base = roundf(base);
            float x0 = r.x + (box_w - line->width) * align_t(t->align) * k;
            if (pass == 0) { run_decorations(ctx, t, l, x0, base, k, snap, color, opacity, true); continue; }
            p.base = base;
            p.x = x0;
            for (int g = line->first; g < line->first + line->count; g++) {   /* spaces and controls draw nothing */
                int run = run_of(t, g);
                BareColorF c = t->runs[run].color.a ? color_f(t->runs[run].color, opacity) : color;
                put_glyph(&p, &t->glyphs[g], run, (unsigned char)t->str[t->glyphs[g].cluster] <= ' ', c);
            }
            if (line->cut && ell_font) {
                void *h0 = p.handles[0];
                BareFont f0 = p.faces[0];
                p.handles[0] = ell_font;
                p.faces[0] = ell_face;
                for (int g = 0; g < t->ell_count; g++) put_glyph(&p, &t->ell[g], 0, false, color);
                p.handles[0] = h0;
                p.faces[0] = f0;
            }
        }
    }
    /* Shadow first: the same glyphs moved and blurred, in one colour. */
    if (p.count && t->shadow.color.a) {
        BareGlyph *sh = bare__arena_alloc(&ctx->frame, sizeof *sh * (size_t)p.count, 16);
        if (sh) {
            float dx = t->shadow.x * k, dy = t->shadow.y * k;
            if (snap) { dx = roundf(dx); dy = roundf(dy); }
            for (int i = 0; i < p.count; i++) {
                sh[i] = p.out[i];
                sh[i].dst.x += dx;
                sh[i].dst.y += dy;
            }
            push_text(ctx, sh, p.count, color_f(t->shadow.color, opacity), fmaxf(t->shadow.blur, 0.0f) * k);
        }
    }
    for (int s = 0; s < p.seg_count; s++) {
        int end = s + 1 < p.seg_count ? p.seg_at[s + 1] : p.count;
        push_text(ctx, p.out + p.seg_at[s], end - p.seg_at[s], p.seg_color[s], 0);
    }
    if (!decorated) return;
    for (int l = 0; l < t->line_count; l++) {
        const BareTextLine *line = &t->lines[l];
        if (line->width <= 0) continue;
        float base = r.y + (line->y + line->asc) * k;
        float x0 = r.x + (box_w - line->width) * align_t(t->align) * k;
        run_decorations(ctx, t, l, x0, base, k, snap, color, opacity, false);
    }
}

/* ---- rich text: bare_paragraph and bare_span --------------------------------------- */

int bare_paragraph_(const BareTextStyle *style, const char *file, int line) {
    BareContext *ctx = bare_ctx_current();
    if (!ctx || !ctx->in_frame) {
        bare__log(ctx, "bare_paragraph at %s:%d called outside a frame", file, line);
        return 1;
    }
    if (ctx->para_elem >= 0) {
        bare__log(ctx, "bare_paragraph at %s:%d is inside another one: its spans join that one", file, line);
        ctx->para_depth++;
        return 1;
    }
    ctx->para_elem = bare__text_push(ctx, "", 0, style, file, line);
    if (ctx->para_elem < 0) return 1;
    bare__style_text(ctx, style, &ctx->para_style, file, line);
    ctx->para_len = 0;
    ctx->para_span_count = 0;
    ctx->para_file = file;
    ctx->para_line = line;
    return 1;
}

int bare_paragraph_end_(void) {
    BareContext *ctx = bare_ctx_current();
    if (!ctx || !ctx->in_frame) return 0;
    if (ctx->para_depth > 0) { ctx->para_depth--; return 0; }
    int i = ctx->para_elem;
    ctx->para_elem = -1;
    if (i < 0) return 0;
    BareText *t = bare__text_runs(ctx, ctx->para_buf ? ctx->para_buf : "", ctx->para_len, &ctx->para_style,
                                  ctx->para_spans, ctx->para_span_count);
    if (t) ctx->elems[i].text = t;
    return 0;
}

void bare_span_(const char *str, const BareTextStyle *style) {
    BareContext *ctx = bare_ctx_current();
    if (!ctx || !ctx->in_frame) return;
    if (ctx->para_elem < 0) {
        bare__log(ctx, "bare_span outside a bare_paragraph block: drawn as its own text");
        bare__text_push(ctx, str ? str : "", str ? (int)strlen(str) : 0, style, "bare_span", 0);
        return;
    }
    int n = str ? (int)strlen(str) : 0;
    if (!bare__grow(&ctx->alloc, (void **)&ctx->para_buf, &ctx->para_cap, ctx->para_len + n + 1, 1) ||
        !bare__grow(&ctx->alloc, (void **)&ctx->para_spans, &ctx->para_span_cap, ctx->para_span_count + 1,
                    sizeof *ctx->para_spans))
        return;
    BareSpanIn *s = &ctx->para_spans[ctx->para_span_count++];
    s->start = ctx->para_len;
    bare__style_text(ctx, style, &s->style, ctx->para_file, ctx->para_line);
    memcpy(ctx->para_buf + ctx->para_len, str, (size_t)n);
    ctx->para_len += n;
    ctx->para_buf[ctx->para_len] = 0;
}
