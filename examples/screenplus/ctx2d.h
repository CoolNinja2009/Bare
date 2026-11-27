/* ctx2d.h: HTML canvas 2D for ports of Screen+'s pages, as close to Chrome's as a
 * page needs to look the same: paths (lines, arcs, ellipses, beziers, rects)
 * filled and stroked with miter/round/bevel joins, clip() to any path,
 * transforms, globalAlpha, source-over and lighter, solid colours and linear,
 * radial (two-circle) and conic gradients, drawImage of other canvases, and
 * text from TrueType fonts (stb_truetype) with per-character fallback, kerning,
 * textAlign and textBaseline (Blink's normalised typo ascent), measureText.
 *
 * Paths are covered on the CPU (exact area per pixel, nonzero), then sent as
 * runs of one coverage along a row; rectangles, glyphs and pictures are single
 * instances. The GPU (shaders/ctx2d.glsl, ctx2d_shader) paints them in one
 * instanced draw per run of the same target, blend and pictures. Glyphs are
 * cached in an atlas at quarter-pixel phases, as Skia places text; text under a
 * rotation, skew or a path clip is drawn from its outlines instead.
 *
 *     CxEngine e; cx_engine_init(&e, bare_shader(&ctx2d_shader));
 *     BareImage scene = bare_canvas(.width = w, .height = h);
 *     CxCtx c; cx_ctx_init(&c, &e, scene, w, h);
 *     cx_fill_color(&c, cx_rgba(255, 0, 0, 1)); cx_fill_rect(&c, 10, 10, 100, 50);
 *     cx_flush(&e);   // before anything reads the canvas (a pass), once a frame at least
 *
 * Needs stb_truetype.h on the include path (Bare's ${stb_SOURCE_DIR}). */
#ifndef SCREENPLUS_CTX2D_H
#define SCREENPLUS_CTX2D_H
#include "bare.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "stb_truetype.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define CX_TAU 6.283185307179586
#define CX_ATLAS 1024
#define CX_RAMPS 256
#define CX_PICS 6

typedef struct CxColor { uint8_t r, g, b, a; } CxColor;

/* U.rgba(c, a): rgb truncated as |0, alpha as toFixed(3), stored as Chrome
 * keeps it (8 bits, rounded). */
static inline CxColor cx_rgba(double r, double g, double b, double a) {
    double ch[3] = { r, g, b };
    CxColor c;
    uint8_t *o = &c.r;
    for (int i = 0; i < 3; i++) {
        double v = ch[i];
        v = v != v ? 0 : v;
        long t = (long)v;   /* toward zero, as |0 */
        o[i] = (uint8_t)(t < 0 ? 0 : t > 255 ? 255 : t);
    }
    a = a != a ? 0 : a < 0 ? 0 : a > 1 ? 1 : a;
    a = floor(a * 1000 + 0.5) / 1000;
    c.a = (uint8_t)floor(a * 255 + 0.5);
    return c;
}
static inline CxColor cx_rgbv(const double c[3], double a) { return cx_rgba(c[0], c[1], c[2], a); }

typedef struct CxMat { double a, b, c, d, e, f; } CxMat;   /* x' = a x + c y + e, y' = b x + d y + f */
static const CxMat CX_IDENTITY = { 1, 0, 0, 1, 0, 0 };
static inline CxMat cx_mul(CxMat m, CxMat n) {   /* m then n applied first: m * n */
    CxMat r = { m.a * n.a + m.c * n.b, m.b * n.a + m.d * n.b, m.a * n.c + m.c * n.d, m.b * n.c + m.d * n.d,
                m.a * n.e + m.c * n.f + m.e, m.b * n.e + m.d * n.f + m.f };
    return r;
}
static inline CxMat cx_inv(CxMat m) {
    double det = m.a * m.d - m.b * m.c;
    if (fabs(det) < 1e-12) return CX_IDENTITY;
    double id = 1 / det;
    CxMat r = { m.d * id, -m.b * id, -m.c * id, m.a * id, (m.c * m.f - m.d * m.e) * id, (m.b * m.e - m.a * m.f) * id };
    return r;
}
static inline void cx_apply(CxMat m, double x, double y, double *ox, double *oy) {
    *ox = m.a * x + m.c * y + m.e, *oy = m.b * x + m.d * y + m.f;
}

/* ---------- gradients ---------- */
enum { CX_SOLID = 0, CX_LINEAR = 1, CX_RADIAL = 2, CX_CONIC = 3 };
typedef struct CxGrad {
    int kind, n;
    double g[6];
    double t[8];
    CxColor c[8];
} CxGrad;
static inline CxGrad cx_linear(double x0, double y0, double x1, double y1) {
    CxGrad g; memset(&g, 0, sizeof g);
    g.kind = CX_LINEAR, g.g[0] = x0, g.g[1] = y0, g.g[2] = x1, g.g[3] = y1;
    return g;
}
static inline CxGrad cx_radial(double x0, double y0, double r0, double x1, double y1, double r1) {
    CxGrad g; memset(&g, 0, sizeof g);
    g.kind = CX_RADIAL, g.g[0] = x0, g.g[1] = y0, g.g[2] = r0, g.g[3] = x1, g.g[4] = y1, g.g[5] = r1;
    return g;
}
static inline CxGrad cx_conic(double start, double x, double y) {
    CxGrad g; memset(&g, 0, sizeof g);
    g.kind = CX_CONIC, g.g[0] = start, g.g[1] = x, g.g[2] = y;
    return g;
}
static inline void cx_stop(CxGrad *g, double t, CxColor c) {
    if (g->n >= 8) return;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    int i = g->n++;   /* stops at the same offset keep their order (canvas: insert after equals) */
    while (i > 0 && g->t[i - 1] > t) g->t[i] = g->t[i - 1], g->c[i] = g->c[i - 1], i--;
    g->t[i] = t, g->c[i] = c;
}

/* ---------- fonts ---------- */
typedef struct CxFace {
    stbtt_fontinfo info;
    unsigned char *data;
    double asc, desc;   /* OS/2 typo ascender, descender (positive), font units */
    int units;
} CxFace;
typedef struct CxFamily { CxFace *face[8]; int n; } CxFamily;

static inline CxFace *cx_face_load(const char *path, int index) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = size > 0 ? (unsigned char *)malloc((size_t)size) : NULL;
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) { fclose(f), free(data); return NULL; }
    fclose(f);
    int off = stbtt_GetFontOffsetForIndex(data, index);
    CxFace *fc = (CxFace *)calloc(1, sizeof *fc);
    if (!fc || off < 0 || !stbtt_InitFont(&fc->info, data, off)) { free(data), free(fc); return NULL; }
    fc->data = data;
    int a, d, g;
    if (!stbtt_GetFontVMetricsOS2(&fc->info, &a, &d, &g)) stbtt_GetFontVMetrics(&fc->info, &a, &d, &g);
    fc->asc = a, fc->desc = -d;
    int x0, y0, x1, y1;
    stbtt_GetFontBoundingBox(&fc->info, &x0, &y0, &x1, &y1);
    fc->units = ttUSHORT(fc->info.data + fc->info.head + 18);
    return fc;
}
static inline void cx_family_add(CxFamily *fam, CxFace *f) {
    if (f && fam->n < 8) fam->face[fam->n++] = f;
}
/* The face of the family that has the character (Chrome's per-character
 * fallback down the font list), its glyph; the first face if none does. */
static inline CxFace *cx_face_for(const CxFamily *fam, uint32_t cp, int *glyph) {
    for (int i = 0; i < fam->n; i++) {
        int g = stbtt_FindGlyphIndex(&fam->face[i]->info, (int)cp);
        if (g) { *glyph = g; return fam->face[i]; }
    }
    *glyph = 0;
    return fam->n ? fam->face[0] : NULL;
}

/* UTF-8, one code point; advances *s */
static inline uint32_t cx_utf8(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    if (c >= 0xF0 && p[0] && p[1] && p[2]) c = ((c & 7) << 18) | ((p[0] & 63u) << 12) | ((p[1] & 63u) << 6) | (p[2] & 63u), p += 3;
    else if (c >= 0xE0 && p[0] && p[1]) c = ((c & 15) << 12) | ((p[0] & 63u) << 6) | (p[1] & 63u), p += 2;
    else if (c >= 0xC0 && p[0]) c = ((c & 31) << 6) | (p[0] & 63u), p += 1;
    *s = (const char *)p;
    return c;
}

/* ---------- engine ---------- */
typedef struct CxInst { float a[4], b[4], c[4], d[4]; } CxInst;
typedef struct CxBatch {
    BareImage target;
    int tw, th;
    BareBlend blend;
    BareImage pic[CX_PICS];
    int npics, first, count, npaints;
    float tab[255 * 4];
} CxBatch;

typedef struct CxGlyph {
    const CxFace *face;
    int glyph, size, phase;
    int x, y, w, h, ox, oy;   /* atlas corner, size; the mask's corner from the pen (pixels) */
    bool used;
} CxGlyph;

typedef struct CxEngine {
    BareShader shader;
    CxInst *inst;
    int ninst, capinst;
    CxBatch *batch;
    int nbatch, capbatch;
    BareImage ramp, atlas;
    unsigned char ramp_px[CX_RAMPS * 256 * 4];
    uint32_t ramp_key[CX_RAMPS];
    int ramp_frame[CX_RAMPS], frame;
    bool ramp_dirty, atlas_dirty;
    unsigned char *atlas_px;
    int shelf_x, shelf_y, shelf_h;
    CxGlyph *glyphs;
    int capglyphs, nglyphs;
    unsigned char lut[256];
    float *acc;
    size_t capacc;
    float *row;
    int caprow;
} CxEngine;

static inline void cx_engine_init(CxEngine *e, BareShader shader, double text_gamma) {
    memset(e, 0, sizeof *e);
    e->shader = shader;
    e->atlas_px = (unsigned char *)calloc((size_t)CX_ATLAS * CX_ATLAS * 4, 1);
    e->capglyphs = 8192;
    e->glyphs = (CxGlyph *)calloc((size_t)e->capglyphs, sizeof *e->glyphs);
    for (int i = 0; i < 256; i++) e->lut[i] = (unsigned char)floor(pow(i / 255.0, text_gamma) * 255 + 0.5);
    for (int i = 0; i < CX_RAMPS; i++) e->ramp_frame[i] = -1;
    e->ramp = bare_image_from_pixels(256, CX_RAMPS, e->ramp_px);
    e->atlas = bare_image_from_pixels(CX_ATLAS, CX_ATLAS, e->atlas_px);
    e->frame = 1;
}

/* ---------- context (one canvas's state) ---------- */
enum { CX_OVER = 0, CX_LIGHTER, CX_DST_OUT };
enum { CX_LEFT = 0, CX_CENTER, CX_RIGHT };
enum { CX_ALPHABETIC = 0, CX_TOP, CX_MIDDLE, CX_BOTTOM };
enum { CX_MITER = 0, CX_ROUND, CX_BEVEL };

typedef struct CxMask {   /* a clip(): coverage over a box of pixels, refcounted along the save stack */
    int x0, y0, w, h, refs;
    float *cov;
} CxMask;

typedef struct CxState {
    CxMat m;
    double alpha, line_width;
    int op, join, align, baseline;
    int fill_kind, stroke_kind;
    CxColor fill, stroke;
    CxGrad fill_grad, stroke_grad;
    const CxFamily *font;
    double px;
    double clip[4];   /* device rectangle every draw is clipped to */
    CxMask *mask;     /* a path clip, or NULL */
} CxState;

typedef struct CxPt { double x, y; } CxPt;
typedef struct CxCtx {
    CxEngine *e;
    BareImage target;
    int w, h;
    CxState s, stack[32];
    int depth;
    CxPt *pts;          /* the path, in device pixels: subpaths start at sub[i] */
    int npts, cappts;
    int *sub;           /* start index of each subpath; closed flag in closed[] */
    bool *closed;
    int nsub, capsub;
    bool has_cur;
} CxCtx;

static inline void cx_ctx_init(CxCtx *c, CxEngine *e, BareImage target, int w, int h) {
    memset(c, 0, sizeof *c);
    c->e = e, c->target = target, c->w = w, c->h = h;
    c->s.m = CX_IDENTITY, c->s.alpha = 1, c->s.line_width = 1;
    c->s.fill = c->s.stroke = cx_rgba(0, 0, 0, 1);
    c->s.px = 10;
    c->s.clip[0] = 0, c->s.clip[1] = 0, c->s.clip[2] = w, c->s.clip[3] = h;
}
static inline void cx_mask_release(CxMask *m) {
    if (m && --m->refs <= 0) free(m->cov), free(m);
}
static inline void cx_ctx_free(CxCtx *c) {
    while (c->depth > 0) cx_mask_release(c->stack[--c->depth].mask);
    cx_mask_release(c->s.mask);
    free(c->pts), free(c->sub), free(c->closed);
    memset(c, 0, sizeof *c);
}
/* resize: a new target, state back to a fresh canvas's (as setting canvas.width does) */
static inline void cx_ctx_reset(CxCtx *c, BareImage target, int w, int h) {
    CxEngine *e = c->e;
    cx_ctx_free(c);
    cx_ctx_init(c, e, target, w, h);
}

static inline void cx_save(CxCtx *c) {
    if (c->depth >= 32) return;
    c->stack[c->depth++] = c->s;
    if (c->s.mask) c->s.mask->refs++;
}
static inline void cx_restore(CxCtx *c) {
    if (c->depth <= 0) return;
    cx_mask_release(c->s.mask);
    c->s = c->stack[--c->depth];
}
static inline void cx_set_transform(CxCtx *c, double a, double b, double cc, double d, double e, double f) {
    CxMat m = { a, b, cc, d, e, f };
    c->s.m = m;
}
static inline void cx_transform(CxCtx *c, double a, double b, double cc, double d, double e, double f) {
    CxMat m = { a, b, cc, d, e, f };
    c->s.m = cx_mul(c->s.m, m);
}
static inline void cx_translate(CxCtx *c, double x, double y) { cx_transform(c, 1, 0, 0, 1, x, y); }
static inline void cx_scale(CxCtx *c, double x, double y) { cx_transform(c, x, 0, 0, y, 0, 0); }
static inline void cx_rotate(CxCtx *c, double a) { cx_transform(c, cos(a), sin(a), -sin(a), cos(a), 0, 0); }
static inline void cx_alpha(CxCtx *c, double a) { if (a >= 0 && a <= 1) c->s.alpha = a; }
static inline void cx_op(CxCtx *c, int op) { c->s.op = op; }
static inline void cx_fill_color(CxCtx *c, CxColor col) { c->s.fill_kind = CX_SOLID, c->s.fill = col; }
static inline void cx_stroke_color(CxCtx *c, CxColor col) { c->s.stroke_kind = CX_SOLID, c->s.stroke = col; }
static inline void cx_fill_grad(CxCtx *c, const CxGrad *g) { c->s.fill_kind = g->kind, c->s.fill_grad = *g; }
static inline void cx_stroke_grad(CxCtx *c, const CxGrad *g) { c->s.stroke_kind = g->kind, c->s.stroke_grad = *g; }
static inline void cx_line_width(CxCtx *c, double w) { if (w > 0 && w < 1e6) c->s.line_width = w; }
static inline void cx_line_join(CxCtx *c, int j) { c->s.join = j; }
static inline void cx_font(CxCtx *c, const CxFamily *f, double px) { c->s.font = f, c->s.px = px < 1 ? 1 : floor(px); }
static inline void cx_text_align(CxCtx *c, int a) { c->s.align = a; }
static inline void cx_text_baseline(CxCtx *c, int b) { c->s.baseline = b; }

/* ---------- path ---------- */
static inline void cx__pt(CxCtx *c, double x, double y) {
    if (c->npts == c->cappts) {
        int cap = c->cappts ? c->cappts * 2 : 1024;
        CxPt *n = (CxPt *)realloc(c->pts, (size_t)cap * sizeof *n);
        if (!n) return;
        c->pts = n, c->cappts = cap;
    }
    c->pts[c->npts].x = x, c->pts[c->npts].y = y, c->npts++;
}
static inline void cx__sub(CxCtx *c) {
    if (c->nsub == c->capsub) {
        int cap = c->capsub ? c->capsub * 2 : 64;
        int *n = (int *)realloc(c->sub, (size_t)cap * sizeof *n);
        bool *b = (bool *)realloc(c->closed, (size_t)cap * sizeof *b);
        if (n) c->sub = n;
        if (b) c->closed = b;
        if (!n || !b) return;
        c->capsub = cap;
    }
    c->sub[c->nsub] = c->npts, c->closed[c->nsub] = false, c->nsub++;
}
static inline void cx_begin(CxCtx *c) { c->npts = 0, c->nsub = 0, c->has_cur = false; }
static inline void cx__move_dev(CxCtx *c, double x, double y) {
    /* a lone moveTo is replaced by the next one */
    if (c->nsub && c->sub[c->nsub - 1] == c->npts - 1 && !c->closed[c->nsub - 1]) c->npts--;
    else cx__sub(c);
    cx__pt(c, x, y);
    c->has_cur = true;
}
static inline void cx_move(CxCtx *c, double x, double y) {
    double dx, dy;
    cx_apply(c->s.m, x, y, &dx, &dy);
    cx__move_dev(c, dx, dy);
}
static inline void cx__line_dev(CxCtx *c, double x, double y) {
    if (!c->has_cur) { cx__move_dev(c, x, y); return; }
    if (c->nsub && c->closed[c->nsub - 1]) {   /* after closePath a new subpath starts at the closed one's start */
        CxPt st = c->pts[c->sub[c->nsub - 1]];
        cx__sub(c), cx__pt(c, st.x, st.y);
    }
    cx__pt(c, x, y);
}
static inline void cx_line(CxCtx *c, double x, double y) {
    double dx, dy;
    cx_apply(c->s.m, x, y, &dx, &dy);
    cx__line_dev(c, dx, dy);
}
static inline void cx_close(CxCtx *c) {
    if (c->nsub && c->npts > c->sub[c->nsub - 1]) c->closed[c->nsub - 1] = true;
}
static inline double cx__scale_of(CxMat m) { return sqrt(fabs(m.a * m.d - m.b * m.c)); }
static inline void cx_bezier(CxCtx *c, double x1, double y1, double x2, double y2, double x3, double y3) {
    if (!c->has_cur) cx_move(c, x1, y1);
    CxMat im = cx_inv(c->s.m);
    CxPt p0 = c->pts[c->npts - 1];
    double x0, y0;
    cx_apply(im, p0.x, p0.y, &x0, &y0);
    double sc = cx__scale_of(c->s.m);
    double len = (hypot(x1 - x0, y1 - y0) + hypot(x2 - x1, y2 - y1) + hypot(x3 - x2, y3 - y2)) * sc;
    int n = (int)ceil(sqrt(len * 2.0));
    n = n < 2 ? 2 : n > 200 ? 200 : n;
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        double x = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3;
        double y = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3;
        cx_line(c, x, y);
    }
}
static inline void cx_quad(CxCtx *c, double x1, double y1, double x2, double y2) {
    if (!c->has_cur) cx_move(c, x1, y1);
    CxMat im = cx_inv(c->s.m);
    CxPt p0 = c->pts[c->npts - 1];
    double x0, y0;
    cx_apply(im, p0.x, p0.y, &x0, &y0);
    double len = (hypot(x1 - x0, y1 - y0) + hypot(x2 - x1, y2 - y1)) * cx__scale_of(c->s.m);
    int n = (int)ceil(sqrt(len * 2.0));
    n = n < 2 ? 2 : n > 200 ? 200 : n;
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        cx_line(c, u * u * x0 + 2 * u * t * x1 + t * t * x2, u * u * y0 + 2 * u * t * y1 + t * t * y2);
    }
}
/* canvas ellipse(): the sweep as the spec has it, a line from the current point to its start */
static inline void cx_ellipse(CxCtx *c, double x, double y, double rx, double ry, double rot, double a0, double a1, bool ccw) {
    if (rx < 0 || ry < 0) return;
    double sweep;
    if (!ccw && a1 - a0 >= CX_TAU) sweep = CX_TAU;
    else if (ccw && a0 - a1 >= CX_TAU) sweep = -CX_TAU;
    else {
        double s0 = fmod(a0, CX_TAU);
        if (s0 < 0) s0 += CX_TAU;
        double d = a1 - a0;
        if (!ccw) { d = fmod(d, CX_TAU); if (d < 0) d += CX_TAU; }
        else { d = fmod(d, CX_TAU); if (d > 0) d -= CX_TAU; }
        sweep = d;
        (void)s0;
    }
    double sc = cx__scale_of(c->s.m) * (rx > ry ? rx : ry);
    double step = sc > 0.5 ? 2 * acos(1 - fmin(0.1 / sc, 1.0)) : 1;
    int n = (int)ceil(fabs(sweep) / step);
    n = n < 4 ? 4 : n > 720 ? 720 : n;
    double cr = cos(rot), sr = sin(rot);
    for (int i = 0; i <= n; i++) {
        double a = a0 + sweep * i / n;
        double ex = rx * cos(a), ey = ry * sin(a);
        double px = x + ex * cr - ey * sr, py = y + ex * sr + ey * cr;
        if (i == 0 && !c->has_cur) cx_move(c, px, py);
        else cx_line(c, px, py);
    }
}
static inline void cx_arc(CxCtx *c, double x, double y, double r, double a0, double a1, bool ccw) {
    cx_ellipse(c, x, y, r, r, 0, a0, a1, ccw);
}
static inline void cx_rect(CxCtx *c, double x, double y, double w, double h) {
    cx_move(c, x, y), cx_line(c, x + w, y), cx_line(c, x + w, y + h), cx_line(c, x, y + h), cx_close(c);
    cx_move(c, x, y);
}

/* ---------- coverage ---------- */
typedef struct CxRaster { int x0, y0, w, h; float *acc; } CxRaster;

static inline void cx__edge(CxRaster *r, double ax, double ay, double bx, double by, float sign) {
    ax -= r->x0, bx -= r->x0, ay -= r->y0, by -= r->y0;
    if (fabs(ay - by) < 1e-12) return;
    float dir = sign;
    if (ay > by) { double t = ax; ax = bx, bx = t, t = ay, ay = by, by = t; dir = -dir; }
    if (by <= 0 || ay >= r->h) return;
    double W = r->w;
    /* the part left of the window counts at its left edge; right of it, nowhere visible */
    if ((ax < 0) != (bx < 0)) {
        double ty = ay + (0 - ax) * (by - ay) / (bx - ax);
        if (ax < 0) { cx__edge(r, r->x0 + 0.0, ay + r->y0, r->x0 + 0.0, ty + r->y0, dir * 1); ax = 0, ay = ty; }
        else { cx__edge(r, r->x0 + 0.0, ty + r->y0, r->x0 + 0.0, by + r->y0, dir * 1); bx = 0, by = ty; }
        if (fabs(ay - by) < 1e-12) return;
    } else if (ax < 0 && bx < 0) ax = bx = 0;
    if ((ax > W) != (bx > W)) {   /* right of the window only touches cells past it: dropped */
        double ty = ay + (W - ax) * (by - ay) / (bx - ax);
        if (ax > W) ay = ty, ax = W;
        else by = ty, bx = W;
        if (fabs(ay - by) < 1e-12) return;
    } else if (ax > W && bx > W) return;
    double dxdy = (bx - ax) / (by - ay);
    double x = ax;
    int ys = (int)floor(ay > 0 ? ay : 0);
    if (ay < 0) x -= ay * dxdy;
    int ye = (int)ceil(by);
    if (ye > r->h) ye = r->h;
    int stride = r->w + 2;
    for (int y = ys; y < ye; y++) {
        float *line = r->acc + (size_t)y * stride;
        double dy = fmin(y + 1.0, by) - fmax((double)y, ay);
        double xn = x + dxdy * dy;
        float d = (float)(dy * dir);
        double x0 = x < xn ? x : xn, x1 = x < xn ? xn : x;
        if (x0 < 0) x0 = 0;
        if (x1 > W) x1 = W;
        double x0f = floor(x0);
        int x0i = (int)x0f;
        double x1c = ceil(x1);
        int x1i = (int)x1c;
        if (x1i <= x0i + 1) {
            double xm = 0.5 * (x + xn) - x0f;
            if (xm < 0) xm = 0;
            if (xm > 1) xm = 1;
            line[x0i] += d - d * (float)xm;
            line[x0i + 1] += d * (float)xm;
        } else {
            double s = 1.0 / (x1 - x0);
            double fx0 = x0 - x0f;
            double a0 = 0.5 * s * (1 - fx0) * (1 - fx0);
            double fx1 = x1 - x1c + 1;
            double am = 0.5 * s * fx1 * fx1;
            line[x0i] += d * (float)a0;
            if (x1i == x0i + 2) line[x0i + 1] += d * (float)(1 - a0 - am);
            else {
                double a1 = s * (1.5 - fx0);
                line[x0i + 1] += d * (float)(a1 - a0);
                for (int xi = x0i + 2; xi < x1i - 1; xi++) line[xi] += d * (float)s;
                double a2 = a1 + (x1i - x0i - 3) * s;
                line[x1i - 1] += d * (float)(1 - a2 - am);
            }
            line[x1i] += d * (float)am;
        }
        x = xn;
    }
}

/* the device box a draw can touch: the canvas, the clip rectangle, the mask */
static inline bool cx__window(const CxCtx *c, double bx0, double by0, double bx1, double by1, int *x0, int *y0, int *x1, int *y1) {
    const CxState *s = &c->s;
    double l = fmax(fmax(bx0, s->clip[0]), 0), t = fmax(fmax(by0, s->clip[1]), 0);
    double r = fmin(fmin(bx1, s->clip[2]), c->w), b = fmin(fmin(by1, s->clip[3]), c->h);
    if (s->mask) {
        l = fmax(l, s->mask->x0), t = fmax(t, s->mask->y0);
        r = fmin(r, s->mask->x0 + s->mask->w), b = fmin(b, s->mask->y0 + s->mask->h);
    }
    if (r <= l || b <= t) return false;
    *x0 = (int)floor(l), *y0 = (int)floor(t), *x1 = (int)ceil(r), *y1 = (int)ceil(b);
    return *x1 > *x0 && *y1 > *y0;
}
static inline bool cx__raster_begin(CxCtx *c, CxRaster *r, int x0, int y0, int x1, int y1) {
    CxEngine *e = c->e;
    r->x0 = x0, r->y0 = y0, r->w = x1 - x0, r->h = y1 - y0;
    size_t need = (size_t)(r->w + 2) * (size_t)r->h;
    if (need > e->capacc) {
        float *n = (float *)realloc(e->acc, need * sizeof *n);
        if (!n) return false;
        e->acc = n, e->capacc = need;
    }
    memset(e->acc, 0, need * sizeof *e->acc);
    r->acc = e->acc;
    if (r->w + 2 > e->caprow) {
        float *n = (float *)realloc(e->row, (size_t)(r->w + 2) * sizeof *n);
        if (!n) return false;
        e->row = n, e->caprow = r->w + 2;
    }
    return true;
}
static inline void cx__poly(CxRaster *r, const CxPt *p, int n, bool positive) {
    float sign = 1;
    if (positive) {
        double area = 0;
        for (int i = 0; i < n; i++) { const CxPt *a = &p[i], *b = &p[(i + 1) % n]; area += a->x * b->y - b->x * a->y; }
        sign = area < 0 ? -1.f : 1.f;
    }
    for (int i = 0; i < n; i++) cx__edge(r, p[i].x, p[i].y, p[(i + 1) % n].x, p[(i + 1) % n].y, sign);
}

/* ---------- emitting ---------- */
static inline CxBatch *cx__batch(CxEngine *e, BareImage target, int tw, int th, BareBlend blend, BareImage pic, int *slot,
                                 bool need_paint) {
    CxBatch *b = e->nbatch ? &e->batch[e->nbatch - 1] : NULL;
    int sl = -1;
    if (b && b->target == target && b->blend == blend && b->first + b->count == e->ninst && !(need_paint && b->npaints >= 63)) {
        if (!pic) sl = 0;
        else {
            for (int i = 0; i < b->npics; i++) if (b->pic[i] == pic) sl = i;
            if (sl < 0 && b->npics < CX_PICS) sl = b->npics, b->pic[b->npics++] = pic;
        }
    }
    if (sl < 0 || !b) {
        if (e->nbatch == e->capbatch) {
            int cap = e->capbatch ? e->capbatch * 2 : 64;
            CxBatch *n = (CxBatch *)realloc(e->batch, (size_t)cap * sizeof *n);
            if (!n) return NULL;
            e->batch = n, e->capbatch = cap;
        }
        b = &e->batch[e->nbatch++];
        b->target = target, b->tw = tw, b->th = th, b->blend = blend, b->npics = 0, b->first = e->ninst, b->count = 0, b->npaints = 0;
        sl = 0;
        if (pic) b->pic[b->npics++] = pic;
    }
    if (slot) *slot = sl;
    return b;
}
static inline CxInst *cx__inst(CxEngine *e) {
    if (e->ninst == e->capinst) {
        int cap = e->capinst ? e->capinst * 2 : 4096;
        CxInst *n = (CxInst *)realloc(e->inst, (size_t)cap * sizeof *n);
        if (!n) return NULL;
        e->inst = n, e->capinst = cap;
    }
    return &e->inst[e->ninst++];
}

static inline uint32_t cx__hash_grad(const CxGrad *g) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < g->n; i++) {
        uint32_t t = (uint32_t)(g->t[i] * 1e6);
        const uint8_t *p = (const uint8_t *)&t;
        for (int k = 0; k < 4; k++) h = (h ^ p[k]) * 16777619u;
        h = (h ^ g->c[i].r) * 16777619u, h = (h ^ g->c[i].g) * 16777619u;
        h = (h ^ g->c[i].b) * 16777619u, h = (h ^ g->c[i].a) * 16777619u;
    }
    return h ^ (uint32_t)g->n;
}
/* the ramp row of a gradient's stops: 256 straight colours, interpolated as Skia does */
static inline int cx__ramp(CxEngine *e, const CxGrad *g) {
    uint32_t key = cx__hash_grad(g);
    int free_row = -1;
    for (int i = 0; i < CX_RAMPS; i++) {
        if (e->ramp_frame[i] >= 0 && e->ramp_key[i] == key) { e->ramp_frame[i] = e->frame; return i; }
        if (free_row < 0 && e->ramp_frame[i] != e->frame) free_row = i;
    }
    if (free_row < 0) free_row = CX_RAMPS - 1;
    unsigned char *px = e->ramp_px + (size_t)free_row * 256 * 4;
    for (int k = 0; k < 256; k++) {
        double t = k / 255.0;
        double col[4];
        if (!g->n) col[0] = col[1] = col[2] = col[3] = 0;
        else if (t <= g->t[0]) col[0] = g->c[0].r, col[1] = g->c[0].g, col[2] = g->c[0].b, col[3] = g->c[0].a;
        else if (t >= g->t[g->n - 1]) {
            const CxColor *c = &g->c[g->n - 1];
            col[0] = c->r, col[1] = c->g, col[2] = c->b, col[3] = c->a;
        } else {
            int i = 0;
            while (i + 1 < g->n && g->t[i + 1] < t) i++;
            double span = g->t[i + 1] - g->t[i], f = span > 0 ? (t - g->t[i]) / span : 1;
            const CxColor *a = &g->c[i], *b = &g->c[i + 1];
            col[0] = a->r + (b->r - a->r) * f, col[1] = a->g + (b->g - a->g) * f;
            col[2] = a->b + (b->b - a->b) * f, col[3] = a->a + (b->a - a->a) * f;
        }
        for (int ch = 0; ch < 4; ch++) px[k * 4 + ch] = (unsigned char)floor(col[ch] + 0.5);
    }
    e->ramp_key[free_row] = key, e->ramp_frame[free_row] = e->frame, e->ramp_dirty = true;
    return free_row;
}
/* the paint for a draw: a premultiplied colour in col, or a table index */
static inline int cx__paint(CxCtx *c, CxBatch *b, bool stroke, float col[4]) {
    const CxState *s = &c->s;
    int kind = stroke ? s->stroke_kind : s->fill_kind;
    if (kind == CX_SOLID) {
        CxColor k = stroke ? s->stroke : s->fill;
        float a = k.a / 255.f * (float)s->alpha;
        col[0] = k.r / 255.f * a, col[1] = k.g / 255.f * a, col[2] = k.b / 255.f * a, col[3] = a;
        return -1;
    }
    const CxGrad *g = stroke ? &s->stroke_grad : &s->fill_grad;
    int row = cx__ramp(c->e, g);
    CxMat im = cx_inv(s->m);
    float *t = &b->tab[b->npaints * 16];
    t[0] = (float)im.a, t[1] = (float)im.b, t[2] = (float)im.c, t[3] = (float)im.d;
    t[4] = (float)im.e, t[5] = (float)im.f, t[6] = (float)kind, t[7] = (float)((row + 0.5) / CX_RAMPS);
    for (int i = 0; i < 4; i++) t[8 + i] = (float)g->g[i];
    t[12] = (float)g->g[4], t[13] = (float)g->g[5], t[14] = (float)s->alpha, t[15] = 0;
    col[0] = col[1] = col[2] = col[3] = 0;
    return b->npaints++;
}
static inline BareBlend cx__blend(int op) { return op == CX_LIGHTER ? BARE_BLEND_ADD : BARE_BLEND_OVER; }

static inline void cx__put(CxEngine *e, CxBatch *b, float x0, float y0, float x1, float y1, int kind, float cov, int paint,
                           int slot, const float col[4], const float d[4]) {
    CxInst *i = cx__inst(e);
    if (!i) return;
    i->a[0] = x0, i->a[1] = y0, i->a[2] = x1, i->a[3] = y1;
    i->b[0] = (float)kind, i->b[1] = cov, i->b[2] = (float)paint, i->b[3] = (float)slot;
    memcpy(i->c, col, sizeof i->c);
    if (d) memcpy(i->d, d, sizeof i->d);
    else memset(i->d, 0, sizeof i->d);
    b->count++;
}

/* rows of coverage (accumulated) to runs, times the clip rectangle and mask */
static inline void cx__emit(CxCtx *c, CxRaster *r, bool stroke) {
    CxEngine *e = c->e;
    const CxState *s = &c->s;
    if (s->op == CX_DST_OUT) return;   /* not drawn (pages here only use it in non-default settings) */
    CxBatch *b = cx__batch(e, c->target, c->w, c->h, cx__blend(s->op), 0, NULL, (stroke ? s->stroke_kind : s->fill_kind) != CX_SOLID);
    if (!b) return;
    float col[4];
    int paint = cx__paint(c, b, stroke, col);
    int stride = r->w + 2;
    const CxMask *mk = s->mask;
    for (int y = 0; y < r->h; y++) {
        int py = r->y0 + y;
        double fy = fmin(py + 1.0, s->clip[3]) - fmax((double)py, s->clip[1]);
        if (fy <= 0) continue;
        if (fy > 1) fy = 1;
        const float *line = r->acc + (size_t)y * stride;
        const float *mrow = mk && py >= mk->y0 && py < mk->y0 + mk->h ? mk->cov + (size_t)(py - mk->y0) * mk->w : NULL;
        if (mk && !mrow) continue;
        float sum = 0;
        int run_q = 0, run_x = 0;
        for (int x = 0; x <= r->w; x++) {
            int q = 0;
            if (x < r->w) {
                sum += line[x];
                float v = sum < 0 ? -sum : sum;
                if (v > 1) v = 1;
                int px = r->x0 + x;
                double fx = fmin(px + 1.0, s->clip[2]) - fmax((double)px, s->clip[0]);
                if (fx > 1) fx = 1;
                double k = v * fy * (fx > 0 ? fx : 0);
                if (mrow) k *= (px >= mk->x0 && px < mk->x0 + mk->w) ? mrow[px - mk->x0] : 0;
                q = (int)(k * 255 + 0.5);
            }
            if (q != run_q || x == r->w) {
                if (run_q > 0)
                    cx__put(e, b, (float)(r->x0 + run_x), (float)py, (float)(r->x0 + x), (float)(py + 1), 0, run_q / 255.f, paint, 0, col, NULL);
                run_q = q, run_x = x;
            }
        }
    }
}

static inline void cx__path_bounds(const CxPt *p, int n, double grow, double *x0, double *y0, double *x1, double *y1) {
    *x0 = *y0 = 1e30, *x1 = *y1 = -1e30;
    for (int i = 0; i < n; i++) {
        if (p[i].x < *x0) *x0 = p[i].x;
        if (p[i].y < *y0) *y0 = p[i].y;
        if (p[i].x > *x1) *x1 = p[i].x;
        if (p[i].y > *y1) *y1 = p[i].y;
    }
    *x0 -= grow, *y0 -= grow, *x1 += grow, *y1 += grow;
}

/* covers the current path's fill into r (already begun) */
static inline void cx__fill_into(CxCtx *c, CxRaster *r) {
    for (int k = 0; k < c->nsub; k++) {
        int a = c->sub[k], z = k + 1 < c->nsub ? c->sub[k + 1] : c->npts;
        if (z - a >= 2) cx__poly(r, c->pts + a, z - a, false);
    }
}
static inline void cx_fill(CxCtx *c) {
    if (!c->npts) return;
    double bx0, by0, bx1, by1;
    cx__path_bounds(c->pts, c->npts, 1, &bx0, &by0, &bx1, &by1);
    int x0, y0, x1, y1;
    if (!cx__window(c, bx0, by0, bx1, by1, &x0, &y0, &x1, &y1)) return;
    CxRaster r;
    if (!cx__raster_begin(c, &r, x0, y0, x1, y1)) return;
    cx__fill_into(c, &r);
    cx__emit(c, &r, false);
}

/* strokes: each segment a quad, each join a wedge, all the same way round, so
 * their union (nonzero) is the stroke; computed in user space, as the pen is */
static inline void cx__stroke_into(CxCtx *c, CxRaster *r) {
    CxMat m = c->s.m, im = cx_inv(m);
    double hw = c->s.line_width / 2;
    for (int k = 0; k < c->nsub; k++) {
        int a = c->sub[k], z = k + 1 < c->nsub ? c->sub[k + 1] : c->npts;
        int n = z - a;
        if (n < 2) continue;
        bool closed = c->closed[k];
        CxPt *u = (CxPt *)malloc((size_t)(n + 1) * sizeof *u);
        if (!u) return;
        int m2 = 0;
        for (int i = 0; i < n; i++) {
            double x, y;
            cx_apply(im, c->pts[a + i].x, c->pts[a + i].y, &x, &y);
            if (m2 && fabs(x - u[m2 - 1].x) < 1e-9 && fabs(y - u[m2 - 1].y) < 1e-9) continue;
            u[m2].x = x, u[m2].y = y, m2++;
        }
        if (closed && m2 > 1 && fabs(u[0].x - u[m2 - 1].x) < 1e-9 && fabs(u[0].y - u[m2 - 1].y) < 1e-9) m2--;
        if (m2 < 2) { free(u); continue; }
        int segs = closed ? m2 : m2 - 1;
        for (int i = 0; i < segs; i++) {
            CxPt p0 = u[i], p1 = u[(i + 1) % m2];
            double dx = p1.x - p0.x, dy = p1.y - p0.y, len = hypot(dx, dy);
            if (len < 1e-12) continue;
            double nx = -dy / len * hw, ny = dx / len * hw;
            CxPt q[4] = { { p0.x + nx, p0.y + ny }, { p1.x + nx, p1.y + ny }, { p1.x - nx, p1.y - ny }, { p0.x - nx, p0.y - ny } };
            for (int j = 0; j < 4; j++) { double x, y; cx_apply(m, q[j].x, q[j].y, &x, &y); q[j].x = x, q[j].y = y; }
            cx__poly(r, q, 4, true);
        }
        int first = closed ? 0 : 1, last = closed ? m2 : m2 - 1;
        for (int i = first; i < last; i++) {
            CxPt p = u[i], pa = u[(i - 1 + m2) % m2], pb = u[(i + 1) % m2];
            double ax = p.x - pa.x, ay = p.y - pa.y, bx = pb.x - p.x, by = pb.y - p.y;
            double la = hypot(ax, ay), lb = hypot(bx, by);
            if (la < 1e-12 || lb < 1e-12) continue;
            ax /= la, ay /= la, bx /= lb, by /= lb;
            double cross = ax * by - ay * bx;
            if (fabs(cross) < 1e-9 && ax * bx + ay * by > 0) continue;
            double side = cross > 0 ? -1 : 1;   /* the outer side of the turn */
            CxPt n0 = { p.x - ay * hw * side, p.y + ax * hw * side }, n1 = { p.x - by * hw * side, p.y + bx * hw * side };
            CxPt w[40];
            int nw = 0;
            w[nw++] = p, w[nw++] = n0;
            int join = c->s.join;
            if (join == CX_ROUND) {
                double a0 = atan2(n0.y - p.y, n0.x - p.x), a1 = atan2(n1.y - p.y, n1.x - p.x);
                double d = a1 - a0;
                while (d > CX_TAU / 2) d -= CX_TAU;
                while (d < -CX_TAU / 2) d += CX_TAU;
                int steps = (int)ceil(fabs(d) / 0.2);
                if (steps > 36) steps = 36;
                for (int s2 = 1; s2 < steps; s2++) {
                    double aa = a0 + d * s2 / steps;
                    w[nw].x = p.x + cos(aa) * hw, w[nw].y = p.y + sin(aa) * hw, nw++;
                }
            } else if (join == CX_MITER) {
                double cosang = -(ax * bx + ay * by);   /* the angle between the segments */
                double theta = acos(fmax(-1.0, fmin(1.0, cosang)));
                double ratio = 1 / sin(theta / 2);
                if (ratio <= 10) {
                    double mx = (n0.x - p.x) + (n1.x - p.x), my = (n0.y - p.y) + (n1.y - p.y), ml = hypot(mx, my);
                    if (ml > 1e-12) { w[nw].x = p.x + mx / ml * hw * ratio, w[nw].y = p.y + my / ml * hw * ratio, nw++; }
                }
            }
            w[nw++] = n1;
            for (int j = 0; j < nw; j++) { double x, y; cx_apply(m, w[j].x, w[j].y, &x, &y); w[j].x = x, w[j].y = y; }
            cx__poly(r, w, nw, true);
        }
        free(u);
    }
}
static inline void cx_stroke(CxCtx *c) {
    if (!c->npts) return;
    double grow = c->s.line_width * cx__scale_of(c->s.m) * 6 + 2;
    double bx0, by0, bx1, by1;
    cx__path_bounds(c->pts, c->npts, grow, &bx0, &by0, &bx1, &by1);
    int x0, y0, x1, y1;
    if (!cx__window(c, bx0, by0, bx1, by1, &x0, &y0, &x1, &y1)) return;
    CxRaster r;
    if (!cx__raster_begin(c, &r, x0, y0, x1, y1)) return;
    cx__stroke_into(c, &r);
    cx__emit(c, &r, true);
}

static inline bool cx__axis(CxMat m) { return fabs(m.b) < 1e-12 && fabs(m.c) < 1e-12; }

/* clip(): a rectangle stays a rectangle; anything else becomes a mask */
static inline void cx_clip(CxCtx *c) {
    CxState *s = &c->s;
    int real = 0, which = -1;
    for (int k = 0; k < c->nsub; k++) {
        int z = k + 1 < c->nsub ? c->sub[k + 1] : c->npts;
        if (z - c->sub[k] >= 2) real++, which = k;
    }
    int rz = which >= 0 ? (which + 1 < c->nsub ? c->sub[which + 1] : c->npts) : 0;
    if (real == 1 && rz - c->sub[which] == 4 && cx__axis(s->m)) {
        const CxPt *p = c->pts + c->sub[which];
        bool rect = true;
        for (int i = 0; i < 4 && rect; i++) {
            const CxPt *q = &p[(i + 1) % 4];
            if (!(fabs(p[i].x - q->x) < 1e-9 || fabs(p[i].y - q->y) < 1e-9)) rect = false;
        }
        if (rect) {
            double bx0, by0, bx1, by1;
            cx__path_bounds(p, 4, 0, &bx0, &by0, &bx1, &by1);
            s->clip[0] = fmax(s->clip[0], bx0), s->clip[1] = fmax(s->clip[1], by0);
            s->clip[2] = fmin(s->clip[2], bx1), s->clip[3] = fmin(s->clip[3], by1);
            return;
        }
    }
    double bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    if (c->npts) cx__path_bounds(c->pts, c->npts, 1, &bx0, &by0, &bx1, &by1);
    int x0, y0, x1, y1;
    CxMask *nm = (CxMask *)calloc(1, sizeof *nm);
    if (!nm) return;
    nm->refs = 1;
    if (!c->npts || !cx__window(c, bx0, by0, bx1, by1, &x0, &y0, &x1, &y1)) {
        nm->x0 = 0, nm->y0 = 0, nm->w = 0, nm->h = 0;
    } else {
        CxRaster r;
        if (!cx__raster_begin(c, &r, x0, y0, x1, y1)) { free(nm); return; }
        cx__fill_into(c, &r);
        nm->x0 = x0, nm->y0 = y0, nm->w = r.w, nm->h = r.h;
        nm->cov = (float *)malloc((size_t)r.w * r.h * sizeof *nm->cov);
        if (!nm->cov) { free(nm); return; }
        const CxMask *old = s->mask;
        for (int y = 0; y < r.h; y++) {
            float sum = 0;
            const float *line = r.acc + (size_t)y * (r.w + 2);
            for (int x = 0; x < r.w; x++) {
                sum += line[x];
                float v = sum < 0 ? -sum : sum;
                if (v > 1) v = 1;
                int px = x0 + x, py = y0 + y;
                if (old) v *= (px >= old->x0 && px < old->x0 + old->w && py >= old->y0 && py < old->y0 + old->h)
                                  ? old->cov[(size_t)(py - old->y0) * old->w + (px - old->x0)] : 0;
                nm->cov[(size_t)y * r.w + x] = v;
            }
        }
    }
    cx_mask_release(s->mask);
    s->mask = nm;
}

static inline void cx_fill_rect(CxCtx *c, double x, double y, double w, double h) {
    CxState *s = &c->s;
    if (w < 0) x += w, w = -w;
    if (h < 0) y += h, h = -h;
    if (w == 0 || h == 0 || s->op == CX_DST_OUT) return;
    if (!cx__axis(s->m) || s->mask) {
        int nsub = c->nsub, npts = c->npts;   /* fillRect leaves the path alone */
        CxPt *save = NULL;
        int *ssub = NULL;
        bool *sclosed = NULL;
        if (npts) {
            save = (CxPt *)malloc((size_t)npts * sizeof *save), ssub = (int *)malloc((size_t)nsub * sizeof *ssub);
            sclosed = (bool *)malloc((size_t)nsub * sizeof *sclosed);
            if (save && ssub && sclosed) memcpy(save, c->pts, (size_t)npts * sizeof *save), memcpy(ssub, c->sub, (size_t)nsub * sizeof *ssub),
                memcpy(sclosed, c->closed, (size_t)nsub * sizeof *sclosed);
        }
        bool cur = c->has_cur;
        cx_begin(c), cx_rect(c, x, y, w, h), cx_fill(c);
        cx_begin(c);
        if (save && ssub && sclosed) {
            for (int i = 0; i < npts; i++) cx__pt(c, save[i].x, save[i].y);
            for (int i = 0; i < nsub; i++) { cx__sub(c); c->sub[i] = ssub[i], c->closed[i] = sclosed[i]; }
            c->npts = npts, c->has_cur = cur;
        }
        free(save), free(ssub), free(sclosed);
        return;
    }
    double x0, y0, x1, y1;
    cx_apply(s->m, x, y, &x0, &y0), cx_apply(s->m, x + w, y + h, &x1, &y1);
    if (x0 > x1) { double t = x0; x0 = x1, x1 = t; }
    if (y0 > y1) { double t = y0; y0 = y1, y1 = t; }
    x0 = fmax(fmax(x0, s->clip[0]), 0), y0 = fmax(fmax(y0, s->clip[1]), 0);
    x1 = fmin(fmin(x1, s->clip[2]), c->w), y1 = fmin(fmin(y1, s->clip[3]), c->h);
    if (x1 <= x0 || y1 <= y0) return;
    CxEngine *e = c->e;
    CxBatch *b = cx__batch(e, c->target, c->w, c->h, cx__blend(s->op), 0, NULL, s->fill_kind != CX_SOLID);
    if (!b) return;
    float col[4];
    int paint = cx__paint(c, b, false, col);
    cx__put(e, b, (float)x0, (float)y0, (float)x1, (float)y1, 0, 1, paint, 0, col, NULL);
}
static inline void cx_stroke_rect(CxCtx *c, double x, double y, double w, double h) {
    int nsub = c->nsub, npts = c->npts;
    (void)nsub;
    if (npts) {   /* keep it simple: strokeRect on an empty path (as the pages use it) */ }
    cx_begin(c);
    cx_move(c, x, y), cx_line(c, x + w, y), cx_line(c, x + w, y + h), cx_line(c, x, y + h), cx_close(c);
    cx_stroke(c);
    cx_begin(c);
}

/* clearRect over the whole canvas (a new canvas, or the page clearing one) */
static inline void cx_clear(CxCtx *c) {
    CxEngine *e = c->e;
    CxBatch *b = cx__batch(e, c->target, c->w, c->h, BARE_BLEND_NONE, 0, NULL, false);
    if (!b) return;
    float col[4] = { 0, 0, 0, 0 };
    cx__put(e, b, 0, 0, (float)c->w, (float)c->h, 0, 1, -1, 0, col, NULL);
}

/* drawImage(canvas, x, y[, w, h]) and the 9-argument form's source rectangle;
 * a picture is premultiplied (a canvas cx drew), sampled bilinearly */
static inline void cx_draw_image_sub(CxCtx *c, BareImage pic, int pw, int ph, double sx, double sy, double sw, double sh, double dx,
                                     double dy, double dw, double dh) {
    CxState *s = &c->s;
    if (!pic || dw <= 0 || dh <= 0 || s->op == CX_DST_OUT) return;
    double x0, y0, x1, y1;
    cx_apply(s->m, dx, dy, &x0, &y0), cx_apply(s->m, dx + dw, dy + dh, &x1, &y1);
    double u0 = sx / pw, v0 = sy / ph, u1 = (sx + sw) / pw, v1 = (sy + sh) / ph;
    double cl = fmax(fmax(x0, s->clip[0]), 0), ct = fmax(fmax(y0, s->clip[1]), 0);
    double cr = fmin(fmin(x1, s->clip[2]), c->w), cb = fmin(fmin(y1, s->clip[3]), c->h);
    if (cr <= cl || cb <= ct) return;
    /* crop the uv with the rectangle */
    double nu0 = u0 + (u1 - u0) * (cl - x0) / (x1 - x0), nu1 = u0 + (u1 - u0) * (cr - x0) / (x1 - x0);
    double nv0 = v0 + (v1 - v0) * (ct - y0) / (y1 - y0), nv1 = v0 + (v1 - v0) * (cb - y0) / (y1 - y0);
    CxEngine *e = c->e;
    int slot = 0;
    CxBatch *b = cx__batch(e, c->target, c->w, c->h, cx__blend(s->op), pic, &slot, false);
    if (!b) return;
    float col[4] = { 0, 0, 0, (float)s->alpha };
    float d[4] = { (float)nu0, (float)nv0, (float)nu1, (float)nv1 };
    cx__put(e, b, (float)cl, (float)ct, (float)cr, (float)cb, 2, 1, -1, slot, col, d);
}
static inline void cx_draw_image(CxCtx *c, BareImage pic, int pw, int ph, double x, double y) {
    cx_draw_image_sub(c, pic, pw, ph, 0, 0, pw, ph, x, y, pw, ph);
}

/* ---------- text ---------- */
static inline double cx__round64(double v) { return floor(v * 64 + 0.5) / 64; }
/* where the baseline lies below y for the current textBaseline (Blink: the
 * primary font's typo ascent and descent, normalised to the em) */
static inline double cx__baseline(const CxCtx *c) {
    const CxState *s = &c->s;
    if (!s->font || !s->font->n || s->baseline == CX_ALPHABETIC) return 0;
    const CxFace *f = s->font->face[0];
    double h = f->asc + f->desc;
    double asc = h > 0 ? cx__round64(s->px * f->asc / h) : s->px * 0.8;
    double desc = h > 0 ? cx__round64(s->px * f->desc / h) : s->px * 0.2;
    if (s->baseline == CX_TOP) return asc;
    if (s->baseline == CX_MIDDLE) return (asc - desc) / 2;
    return -desc;
}

typedef struct CxRun { const CxFace *face; int glyph; double x; } CxRun;   /* a laid-out glyph, pen x from the start */
/* lays the text out (advances and kerning in each face), returns its width */
static inline double cx__layout(const CxCtx *c, const char *text, CxRun *out, int cap, int *n) {
    const CxState *s = &c->s;
    double pen = 0;
    int k = 0;
    const CxFace *pf = NULL;
    int pg = 0;
    if (!s->font || !s->font->n) { if (n) *n = 0; return 0; }
    for (const char *p = text; *p;) {
        uint32_t cp = cx_utf8(&p);
        int g;
        CxFace *f = cx_face_for(s->font, cp, &g);
        if (!f) continue;
        double scale = stbtt_ScaleForMappingEmToPixels(&f->info, (float)s->px);
        if (pf == f && pg) pen += stbtt_GetGlyphKernAdvance(&f->info, pg, g) * scale;
        if (out && k < cap) out[k].face = f, out[k].glyph = g, out[k].x = pen, k++;
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&f->info, g, &adv, &lsb);
        pen += adv * scale;
        pf = f, pg = g;
    }
    if (n) *n = k;
    return pen;
}
static inline double cx_measure(const CxCtx *c, const char *text) { return cx__layout(c, text, NULL, 0, NULL); }

static inline const CxGlyph *cx__glyph(CxEngine *e, const CxFace *f, int glyph, int size, int phase) {
    uint32_t h = (uint32_t)(uintptr_t)f * 2654435761u ^ (uint32_t)glyph * 40503u ^ (uint32_t)size * 977u ^ (uint32_t)phase * 31u;
    int cap = e->capglyphs;
    for (int i = 0; i < cap; i++) {
        CxGlyph *g = &e->glyphs[(h + (uint32_t)i) % (uint32_t)cap];
        if (!g->used) {
            if (e->nglyphs > cap * 3 / 4) return NULL;
            float scale = stbtt_ScaleForMappingEmToPixels(&f->info, (float)size);
            float shift = phase / 4.f;
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBoxSubpixel(&f->info, glyph, scale, scale, shift, 0, &x0, &y0, &x1, &y1);
            int w = x1 - x0, hh = y1 - y0;
            if (w > 0 && hh > 0) {
                if (e->shelf_x + w + 1 > CX_ATLAS) e->shelf_x = 0, e->shelf_y += e->shelf_h + 1, e->shelf_h = 0;
                if (e->shelf_y + hh + 1 > CX_ATLAS) return NULL;
                unsigned char *tmp = (unsigned char *)malloc((size_t)w * hh);
                if (!tmp) return NULL;
                stbtt_MakeGlyphBitmapSubpixel(&f->info, tmp, w, hh, w, scale, scale, shift, 0, glyph);
                for (int y = 0; y < hh; y++)
                    for (int x = 0; x < w; x++) {
                        unsigned char v = e->lut[tmp[y * w + x]];
                        unsigned char *px = e->atlas_px + ((size_t)(e->shelf_y + y) * CX_ATLAS + e->shelf_x + x) * 4;
                        px[0] = px[1] = px[2] = px[3] = v;
                    }
                free(tmp);
                g->x = e->shelf_x, g->y = e->shelf_y;
                e->shelf_x += w + 1;
                if (hh > e->shelf_h) e->shelf_h = hh;
                e->atlas_dirty = true;
            }
            g->face = f, g->glyph = glyph, g->size = size, g->phase = phase;
            g->w = w > 0 ? w : 0, g->h = hh > 0 ? hh : 0, g->ox = x0, g->oy = y0, g->used = true;
            e->nglyphs++;
            return g;
        }
        if (g->face == f && g->glyph == glyph && g->size == size && g->phase == phase) return g;
    }
    return NULL;
}

/* a glyph from its outline, through the path rasteriser (any transform, any clip) */
static inline void cx__glyph_path(CxCtx *c, const CxFace *f, int glyph, double ox, double oy) {
    stbtt_vertex *v = NULL;
    int n = stbtt_GetGlyphShape(&f->info, glyph, &v);
    if (n <= 0) return;
    double scale = stbtt_ScaleForMappingEmToPixels(&f->info, (float)c->s.px);
    double cx0 = 0, cy0 = 0;
    for (int i = 0; i < n; i++) {
        double x = ox + v[i].x * scale, y = oy - v[i].y * scale;
        if (v[i].type == STBTT_vmove) { if (c->nsub) cx_close(c); cx_move(c, x, y); }
        else if (v[i].type == STBTT_vline) cx_line(c, x, y);
        else if (v[i].type == STBTT_vcurve) cx_quad(c, ox + v[i].cx * scale, oy - v[i].cy * scale, x, y);
        else if (v[i].type == STBTT_vcubic)
            cx_bezier(c, ox + v[i].cx * scale, oy - v[i].cy * scale, ox + v[i].cx1 * scale, oy - v[i].cy1 * scale, x, y);
        cx0 = x, cy0 = y;
    }
    (void)cx0, (void)cy0;
    cx_close(c);
    stbtt_FreeShape(&f->info, v);
}

static inline void cx_fill_text(CxCtx *c, const char *text, double x, double y) {
    CxState *s = &c->s;
    if (!text || !*text || !s->font || s->op == CX_DST_OUT) return;
    CxRun runs[512];
    int n = 0;
    double width = cx__layout(c, text, runs, 512, &n);
    double sx = s->align == CX_CENTER ? x - width / 2 : s->align == CX_RIGHT ? x - width : x;
    double by = y + cx__baseline(c);
    CxEngine *e = c->e;
    bool translate_only = fabs(s->m.a - 1) < 1e-9 && fabs(s->m.d - 1) < 1e-9 && cx__axis(s->m);
    if (!translate_only || s->mask) {
        /* outlines, as one path, so overlapping glyphs cover once */
        int nsub = c->nsub, npts = c->npts;
        (void)nsub, (void)npts;
        cx_begin(c);
        for (int i = 0; i < n; i++) cx__glyph_path(c, runs[i].face, runs[i].glyph, sx + runs[i].x, by);
        cx_fill(c);
        cx_begin(c);
        return;
    }
    CxBatch *b = cx__batch(e, c->target, c->w, c->h, cx__blend(s->op), 0, NULL, s->fill_kind != CX_SOLID);
    if (!b) return;
    float col[4];
    int paint = cx__paint(c, b, false, col);
    double dx = s->m.e, dy = s->m.f;
    double base = floor(by + dy + 0.5);
    for (int i = 0; i < n; i++) {
        double gx = sx + runs[i].x + dx;
        double q = floor(gx * 4 + 0.5) / 4;   /* the nearest quarter pixel */
        double xi = floor(q);
        int phase = (int)((q - xi) * 4 + 0.5) & 3;
        const CxGlyph *g = cx__glyph(e, runs[i].face, runs[i].glyph, (int)s->px, phase);
        if (!g) {   /* atlas full: this one from its outline */
            CxMat keep = s->m;
            s->m = CX_IDENTITY;
            cx_begin(c), cx__glyph_path(c, runs[i].face, runs[i].glyph, xi + phase / 4.0, base), cx_fill(c), cx_begin(c);
            s->m = keep;
            b = cx__batch(e, c->target, c->w, c->h, cx__blend(s->op), 0, NULL, s->fill_kind != CX_SOLID);
            if (!b) return;
            paint = cx__paint(c, b, false, col);
            continue;
        }
        if (!g->w || !g->h) continue;
        double qx0 = xi + g->ox, qy0 = base + g->oy, qx1 = qx0 + g->w, qy1 = qy0 + g->h;
        double cl = fmax(fmax(qx0, s->clip[0]), 0), ct = fmax(fmax(qy0, s->clip[1]), 0);
        double cr = fmin(fmin(qx1, s->clip[2]), c->w), cb = fmin(fmin(qy1, s->clip[3]), c->h);
        if (cr <= cl || cb <= ct) continue;
        float d[4] = { (float)(g->x - qx0), (float)(g->y - qy0), 0, 0 };
        cx__put(e, b, (float)cl, (float)ct, (float)cr, (float)cb, 1, 1, paint, 0, col, d);
    }
}

/* ---------- flush ---------- */
typedef struct CxParams { float res[4]; float tab[255 * 4]; } CxParams;

static inline void cx_flush(CxEngine *e) {
    if (e->ramp_dirty) bare_image_update(e->ramp, e->ramp_px), e->ramp_dirty = false;
    if (e->atlas_dirty) bare_image_update(e->atlas, e->atlas_px), e->atlas_dirty = false;
    static CxParams p;
    for (int i = 0; i < e->nbatch; i++) {
        CxBatch *b = &e->batch[i];
        if (!b->count) continue;
        p.res[0] = (float)b->tw, p.res[1] = (float)b->th, p.res[2] = CX_ATLAS, p.res[3] = CX_ATLAS;
        memcpy(p.tab, b->tab, (size_t)b->npaints * 16 * sizeof(float));
        BareImage pics[CX_PICS];
        for (int k = 0; k < CX_PICS; k++) pics[k] = k < b->npics ? b->pic[k] : e->ramp;
        bare_draw(.target = b->target, .shader = e->shader, .blend = b->blend,
                  .inputs = { e->ramp, e->atlas, pics[0], pics[1], pics[2], pics[3], pics[4], pics[5] },
                  .params = &p, .params_size = (int)sizeof p,
                  .instances = e->inst + b->first, .instance_size = (int)sizeof(CxInst), .instance_count = b->count,
                  .vertex_count = 6);
    }
    e->nbatch = 0, e->ninst = 0;
}
/* once a frame, after the last flush: gradient rows of this frame stay, others may go */
static inline void cx_frame_end(CxEngine *e) {
    e->frame++;
    if (e->nglyphs > e->capglyphs / 2 || e->shelf_y > CX_ATLAS * 7 / 8) {   /* full: start the atlas again next frame */
        memset(e->glyphs, 0, (size_t)e->capglyphs * sizeof *e->glyphs);
        e->nglyphs = 0, e->shelf_x = e->shelf_y = e->shelf_h = 0;
        memset(e->atlas_px, 0, (size_t)CX_ATLAS * CX_ATLAS * 4);
        e->atlas_dirty = true;
    }
}
#endif
