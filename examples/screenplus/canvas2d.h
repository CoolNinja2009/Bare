/* canvas2d.h: the bits of HTML canvas 2D that ports of Screen+'s pages use,
 * drawn with shaders/canvas2d.glsl (add it to the example's bare_add_shaders;
 * it becomes canvas2d_shader).
 *
 *     C2d c;
 *     c2d_init(&c, bare_shader(&canvas2d_shader));
 *     c2d_rect(&c, 10, 10, 110, 60, C2D_RGBA(255, 0, 0, 1));          // fillRect (x0 y0 x1 y1)
 *     c2d_line(&c, 0, 0.5, 100, 0.5, 1, C2D_RGBA(0, 0, 0, 0.1));     // a stroked segment, butt ends
 *     c2d_text(&c, font, 12, "100", 26, 40, C2D_RIGHT, 4, C2D_RGBA(102, 102, 102, 1));
 *     c2d_flush(&c, canvas, w, h, true, black);                       // one bare_draw
 *
 * Coverage is per pixel and exact for rectangles (1 px lines at .5 are crisp,
 * as in a browser), from distance for slanted lines. Text is whole strings
 * rendered by bare_text_pixels into an atlas, cached by font, size and text;
 * pens are rounded to whole pixels. Colours are straight RGBA, blended as
 * canvas's source-over. */
#ifndef SCREENPLUS_CANVAS2D_H
#define SCREENPLUS_CANVAS2D_H
#include "bare.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct C2dColor { float r, g, b, a; } C2dColor;
#define C2D_RGBA(r, g, b, a) ((C2dColor){ (r) / 255.f, (g) / 255.f, (b) / 255.f, (float)(a) })

enum { C2D_LEFT, C2D_CENTER, C2D_RIGHT };

typedef struct C2dInst { float a[4], b[4], color[4], clip[4]; } C2dInst;

typedef struct C2dText {
    char text[120];
    BareFont font;
    float size, advance;
    int x, y, w, h, ox, oy;   /* in the atlas; the mask's corner from the pen */
} C2dText;

#ifndef C2D_ATLAS
#define C2D_ATLAS 1024   /* the text atlas, pixels square: define it bigger before including for lots of text */
#endif

typedef struct C2d {
    BareShader shader;
    C2dInst *inst;
    int n, cap;
    float clip[4];
    BareImage atlas, picture;
    unsigned char *atlas_px;
    int shelf_x, shelf_y, shelf_h;
    bool dirty;
    C2dText *texts;
    int ntexts, captexts;
    float pic[4];     /* kind 6 (c2d_glow): where the picture lies on the target: x, y, w, h */
    BareBlend blend;  /* 0: BARE_BLEND_ALPHA (canvas's source-over); c2d_mask's layers want BARE_BLEND_OVER */
    const unsigned char *lut;   /* text coverage remapped (a browser's text gamma), or NULL; set before any text */
} C2d;

static inline void c2d_init(C2d *c, BareShader shader) {
    memset(c, 0, sizeof *c);
    c->shader = shader;
    c->clip[0] = c->clip[1] = -1e9f, c->clip[2] = c->clip[3] = 1e9f;
}

static inline void c2d_clip(C2d *c, float x0, float y0, float x1, float y1) {
    c->clip[0] = x0, c->clip[1] = y0, c->clip[2] = x1, c->clip[3] = y1;
}
static inline void c2d_noclip(C2d *c) { c2d_clip(c, -1e9f, -1e9f, 1e9f, 1e9f); }

static inline void c2d__push(C2d *c, float a0, float a1, float a2, float a3, float kind, float width, float u, float v,
                             C2dColor col) {
    if (c->n == c->cap) {
        int cap = c->cap ? c->cap * 2 : 256;
        C2dInst *ni = realloc(c->inst, (size_t)cap * sizeof *ni);
        if (!ni) return;
        c->inst = ni, c->cap = cap;
    }
    C2dInst *i = &c->inst[c->n++];
    i->a[0] = a0, i->a[1] = a1, i->a[2] = a2, i->a[3] = a3;
    i->b[0] = kind, i->b[1] = width, i->b[2] = u, i->b[3] = v;
    i->color[0] = col.r, i->color[1] = col.g, i->color[2] = col.b, i->color[3] = col.a;
    memcpy(i->clip, c->clip, sizeof i->clip);
}

static inline void c2d_rect(C2d *c, float x0, float y0, float x1, float y1, C2dColor col) {
    if (x1 > x0 && y1 > y0) c2d__push(c, x0, y0, x1, y1, 0, 0, 0, 0, col);
}

/* A segment stroked `width` wide with butt ends; axis-aligned ones are rectangles. */
static inline void c2d_line(C2d *c, float x0, float y0, float x1, float y1, float width, C2dColor col) {
    float hw = width / 2;
    if (y0 == y1) c2d_rect(c, fminf(x0, x1), y0 - hw, fmaxf(x0, x1), y0 + hw, col);
    else if (x0 == x1) c2d_rect(c, x0 - hw, fminf(y0, y1), x0 + hw, fmaxf(y0, y1), col);
    else c2d__push(c, x0, y0, x1, y1, 1, width, 0, 0, col);
}

/* A polyline stroked as one path (lineTo after moveTo). */
static inline void c2d_polyline(C2d *c, const float *xy, int points, float width, C2dColor col) {
    for (int i = 0; i + 1 < points; i++) c2d_line(c, xy[i * 2], xy[i * 2 + 1], xy[i * 2 + 2], xy[i * 2 + 3], width, col);
}

/* A triangle filled, grown by `grow` pixels with round corners: fill() and a
 * stroke() as wide as twice that in the same colour, the way pages draw meshes. */
static inline void c2d_triangle(C2d *c, float x0, float y0, float x1, float y1, float x2, float y2, float grow,
                                C2dColor col) {
    c2d__push(c, x0, y0, x1, y1, 4, grow, x2, y2, col);
}

/* drawImage: the picture set with c2d_picture, stretched over the rectangle. */
static inline void c2d_picture(C2d *c, BareImage picture) { c->picture = picture; }
static inline void c2d_image(C2d *c, float x, float y, float w, float h, float alpha) {
    c2d__push(c, x, y, w, h, 3, 0, 0, 0, (C2dColor){ 1, 1, 1, alpha });
}

/* A blurred mask (one channel of the picture, which lies at c->pic on the
 * target) tinting the rectangle: a text-shadow's glow, drawn with its text. */
static inline void c2d_glow(C2d *c, float x0, float y0, float x1, float y1, int channel, C2dColor col) {
    if (x1 > x0 && y1 > y0) c2d__push(c, x0, y0, x1, y1, 6, (float)channel, 0, 0, col);
}
/* A ring (the border of a circle of radius r, `width` wide) with n dots of radius dot
 * on its inner edge, the first at the top, the lot turned `angle` radians clockwise;
 * the dots at 0.75 over the ring, all at col: the page's electron orbitals. */
static inline void c2d_orbit(C2d *c, float cx, float cy, float r, float width, int n, float dot, float angle, C2dColor col) {
    c2d__push(c, cx, cy, r, width, 8, (float)n, dot, angle, col);
}
static inline const C2dText *c2d__text(C2d *c, BareFont font, float size, const char *text) {
    for (int i = 0; i < c->ntexts; i++)
        if (c->texts[i].font == font && c->texts[i].size == size && !strcmp(c->texts[i].text, text)) return &c->texts[i];
    if (strlen(text) >= sizeof c->texts[0].text) return NULL;
    if (!c->atlas_px && !(c->atlas_px = calloc((size_t)C2D_ATLAS * C2D_ATLAS, 4))) return NULL;
    if (c->ntexts == c->captexts) {
        int cap = c->captexts ? c->captexts * 2 : 32;
        C2dText *nt = realloc(c->texts, (size_t)cap * sizeof *nt);
        if (!nt) return NULL;
        c->texts = nt, c->captexts = cap;
    }
    C2dText t;
    memset(&t, 0, sizeof t);
    snprintf(t.text, sizeof t.text, "%s", text);
    t.font = font, t.size = size;
    unsigned char *mask = bare_text_pixels(text, font, size, &t.w, &t.h, &t.ox, &t.oy, &t.advance);
    if (mask) {
        if (c->shelf_x + t.w + 2 > C2D_ATLAS) c->shelf_x = 0, c->shelf_y += c->shelf_h + 2, c->shelf_h = 0;
        if (c->shelf_y + t.h > C2D_ATLAS || t.w > C2D_ATLAS) {   /* full: start over */
            memset(c->atlas_px, 0, (size_t)C2D_ATLAS * C2D_ATLAS * 4);
            c->shelf_x = c->shelf_y = c->shelf_h = 0;
            c->ntexts = 0;
        }
        t.x = c->shelf_x, t.y = c->shelf_y;
        for (int r = 0; r < t.h && t.y + r < C2D_ATLAS; r++)
            for (int k = 0; k < t.w; k++) {
                unsigned char *p = c->atlas_px + ((size_t)(t.y + r) * C2D_ATLAS + t.x + k) * 4;
                p[0] = p[1] = p[2] = p[3] = c->lut ? c->lut[mask[r * t.w + k]] : mask[r * t.w + k];
            }
        free(mask);
        c->shelf_x += t.w + 2;   /* two pixels apart: turned text is filtered */
        if (t.h > c->shelf_h) c->shelf_h = t.h;
        c->dirty = true;
    }
    c->texts[c->ntexts] = t;
    return &c->texts[c->ntexts++];
}

/* Starts the text atlas over once it is more than `frac` full, so one draw's
 * text never outgrows it (only between flushes: nothing queued may use it). */
static inline void c2d_atlas_trim(C2d *c, float frac) {
    if (c->n || !c->atlas_px || c->shelf_y + c->shelf_h < frac * C2D_ATLAS) return;
    memset(c->atlas_px, 0, (size_t)C2D_ATLAS * C2D_ATLAS * 4);
    c->shelf_x = c->shelf_y = c->shelf_h = 0;
    c->ntexts = 0;
    c->dirty = true;
}

/* measureText(text).width */
static inline float c2d_measure(C2d *c, BareFont font, float size, const char *text) {
    const C2dText *t = c2d__text(c, font, size, text);
    return t ? t->advance : 0;
}

/* fillText at (x, y): y is the alphabetic baseline plus `baseline` pixels
 * (textBaseline 'middle' is (ascent - descent) / 2 of the font, rounded as the
 * browser rounds its metrics: c2d_middle). Wider than max_width (> 0): squeezed, as canvas does. */
static inline void c2d_text_max(C2d *c, BareFont font, float size, const char *text, float x, float y, int align,
                                float baseline, float max_width, C2dColor col) {
    const C2dText *t = c2d__text(c, font, size, text);
    if (!t || !t->w) return;
    float sx = max_width > 0 && t->advance > max_width ? max_width / t->advance : 1;
    float pen = x - (align == C2D_CENTER ? t->advance * sx / 2 : align == C2D_RIGHT ? t->advance * sx : 0);
    float px = floorf(pen + 0.5f), py = floorf(y + baseline + 0.5f);
    c2d__push(c, px + t->ox * sx, py + t->oy, t->w * sx, (float)t->h, 2, 0, (float)t->x, (float)t->y, col);
}
/* A text's coverage into a mask layer: premultiplied colour (c2d_mask_color(k):
 * channel k alone), flushed with c->blend = BARE_BLEND_OVER, so masks in
 * different channels add up. */
static inline void c2d_text_mask(C2d *c, BareFont font, float size, const char *text, float x, float y, int channel) {
    const C2dText *t = c2d__text(c, font, size, text);
    if (!t || !t->w) return;
    C2dColor m = { (float)(channel == 0), (float)(channel == 1), (float)(channel == 2), (float)(channel == 3) };
    c2d__push(c, floorf(x + 0.5f) + t->ox, floorf(y + 0.5f) + t->oy, (float)t->w, (float)t->h, 7, 0, (float)t->x, (float)t->y, m);
}
static inline void c2d_text(C2d *c, BareFont font, float size, const char *text, float x, float y, int align,
                            float baseline, C2dColor col) {
    c2d_text_max(c, font, size, text, x, y, align, baseline, 0, col);
}
/* c2d_text turned by `angle` radians (clockwise on screen) about (px, py): CSS's rotate(). */
static inline void c2d_text_turned(C2d *c, BareFont font, float size, const char *text, float x, float y, int align,
                                   float baseline, float angle, float px, float py, C2dColor col) {
    const C2dText *t = c2d__text(c, font, size, text);
    if (!t || !t->w) return;
    float pen = x - (align == C2D_CENTER ? t->advance / 2 : align == C2D_RIGHT ? t->advance : 0);
    c2d__push(c, floorf(pen + 0.5f) + t->ox, floorf(y + baseline + 0.5f) + t->oy, (float)t->w, (float)t->h, 5, angle,
              (float)t->x, (float)t->y, col);
    c->inst[c->n - 1].clip[0] = px, c->inst[c->n - 1].clip[1] = py;
}
static inline float c2d_middle(float ascent_em, float descent_em, float size) {
    return (floorf(ascent_em * size + 0.5f) - floorf(descent_em * size + 0.5f)) / 2;
}

/* Draws everything since the last flush into target (w x h pixels). */
static inline void c2d_flush(C2d *c, BareImage target, int w, int h, bool clear, const float clear_rgba[4]) {
    if (c->dirty) {
        if (!c->atlas) c->atlas = bare_image_from_pixels(C2D_ATLAS, C2D_ATLAS, c->atlas_px);
        else bare_image_update(c->atlas, c->atlas_px);
        c->dirty = false;
    }
    if (!c->atlas) {   /* the shader always samples an atlas: one blank pixel will do */
        static const unsigned char blank[4] = { 0, 0, 0, 0 };
        c->atlas = bare_image_from_pixels(1, 1, blank);
    }
    float res[8] = { (float)w, (float)h, C2D_ATLAS, C2D_ATLAS, c->pic[0], c->pic[1], c->pic[2], c->pic[3] };
    BareBlend blend = c->blend ? c->blend : BARE_BLEND_ALPHA;
    BareImage pic = c->picture ? c->picture : c->atlas;
    if (clear && !c->n) c2d__push(c, 0, 0, 0, 0, 0, 0, 0, 0, (C2dColor){ 0, 0, 0, 0 });   /* a draw needs an instance */
    if (clear)
        bare_draw(.target = target, .shader = c->shader, .inputs = { c->atlas, pic }, .vertex_count = 6,
                  BARE_INSTANCES(c->inst, c->n), .blend = blend, BARE_PARAMS(res), .clear = true,
                  .clear_color = { clear_rgba[0], clear_rgba[1], clear_rgba[2], clear_rgba[3] });
    else if (c->n)
        bare_draw(.target = target, .shader = c->shader, .inputs = { c->atlas, pic }, .vertex_count = 6,
                  BARE_INSTANCES(c->inst, c->n), .blend = blend, BARE_PARAMS(res));
    c->n = 0;
}

#endif
