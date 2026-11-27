/* periodic.c: the periodic table of elements, glowing tiles on a dark page;
 * Screen+'s "Periodic Table" wallpaper.
 *
 * A port of the page (Mike Golus's CodePen "Periodic Table of Elements -
 * HTML/CSS", MIT, see LICENSE.txt), laid out as Blink lays out its CSS: an
 * 18-column grid in LayoutUnits (1/64 px), Arial's rounded ascents and
 * descents, half-leadings floored, flex centring, the key spread with
 * space-between. Every text glows (text-shadow 0 0 0.4vw currentColor): the
 * texts' coverage goes into a mask (four channels, so tiles next to each other
 * never share one), the mask is blurred, and each tile is painted in the
 * page's order (background, border, glow, text), so a later tile covers the
 * glow of an earlier one as it does in the page.
 *
 * And the page's interaction: a tile grows under the mouse; a click lifts it
 * to three times its size over a dimmed page, with its electron shells
 * turning; a click on the key dims every other category, a click elsewhere
 * undoes it. Transitions run with the page's durations and CSS's `ease`.
 *
 * A Screen+ app wallpaper (see ../screenplus/screenplus_wallpaper.h); it has no
 * settings. It only draws when something moves. */
#define C2D_ATLAS 2048
#include "bare.h"
#include "periodic_shaders.h"
#include "../screenplus/screenplus_wallpaper.h"
#include "../screenplus/canvas2d.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Element {
    int cat, col, row;
    const char *num, *sym, *name, *mass;
    int nshells, shells[7];   /* electrons, innermost shell first */
} Element;
#include "elements.h"

static const unsigned CAT_RGB[10] = { 0xecbe59, 0xdee955, 0xec77a3, 0xc686cc, 0xfd8572,
                                      0x4cddf3, 0x3aefb6, 0x52ee61, 0x759fff, 0xcccccc };
static const char *const KEY_TEXT[10] = { "Alkali Metals", "Alkaline Earth Metals", "Lanthanoids", "Aktinoids",
                                          "Transition Metals", "Post-Transition Metals", "Metalloids",
                                          "Other Nonmetals", "Noble Gasses", "Unknown" };
static const C2dColor BG = { 0x10 / 255.f, 0x13 / 255.f, 0x18 / 255.f, 1 };

static C2dColor cat_color(int cat, float a) {
    unsigned v = CAT_RGB[cat];
    return (C2dColor){ ((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f, a };
}
static C2dColor fade(C2dColor c, float a) { c.a *= a; return c; }

/* ---- Blink's numbers ----------------------------------------------------------- */

/* LayoutUnit: 1/64 px, made from floats by truncation; widths of text round up */
static double lu(double v) { return floor(v * 64 + 1e-7) / 64; }
static double lu_ceil(double v) { return ceil(v * 64 - 1e-6) / 64; }
static double lu_div(double v, int n) { long long r = llround(v * 64); return (double)(r / n) / 64; }

/* cubic-bezier(0.25, 0.1, 0.25, 1): CSS's `ease` */
static double ease(double x) {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    double lo = 0, hi = 1, t = x;
    for (int i = 0; i < 40; i++) {
        t = (lo + hi) / 2;
        double bx = 3 * (1 - t) * (1 - t) * t * 0.25 + 3 * (1 - t) * t * t * 0.25 + t * t * t;
        if (bx < x) lo = t; else hi = t;
    }
    return 3 * (1 - t) * (1 - t) * t * 0.1 + 3 * (1 - t) * t * t * 1.0 + t * t * t;
}

/* A line of Arial: its box's height and its baseline below the top. Blink takes
 * the font's ascent and descent rounded, halves the leading in LayoutUnits and
 * floors the half above the text to a whole pixel. */
typedef struct Line { float size; double h; int base; } Line;
static Line line_of(float size, float line_height) {
    int a = (int)floor(size * 1854.0 / 2048 + 0.5), d = (int)floor(size * 434.0 / 2048 + 0.5);
    double lh = lu((float)(size * line_height));
    Line l = { size, lh, a + (int)floor(lu_div(lh - (a + d), 2)) };
    return l;
}

/* ---- the layout ---------------------------------------------------------------- */

static BareFont arial;
/* Chrome's text is heavier than stb_truetype's unhinted coverage (its text
 * gamma and hinting): coverage goes through x^0.65; the glow (sigma half the
 * blur radius, as the CSS spec has it) at 0.85 matches its strength. */
#define GLOW_GAIN 0.85f
#define TEXT_GAMMA 0.65
static unsigned char text_lut[256];

static struct {
    int w, h;
    double colw, colx[19], rowy[11], rowh[11];
    Line num, sym, name, key, ph;
    float sigma;                          /* the glow's, at scale 1 */
    double kx, ky, kw, kh, krow, lab_x[10], lab_w[10];
} L;

static double text_w(C2d *c, float size, const char *t) { return lu_ceil(c2d_measure(c, arial, size, t)); }

static void layout(C2d *c, int w, int h) {
    L.w = w, L.h = h;
    double vw = w / 100.0;
    double cw = lu(0.85 * w), x0 = lu(0.05 * w) + lu(0.02 * w), y0 = lu(0.03 * w) + lu(0.02 * w);
    double exact = (cw - 17 * 5) / 18;
    L.colw = lu(exact);
    for (int i = 1; i <= 18; i++) L.colx[i] = x0 + (i - 1) * (L.colw + 5);
    for (int r = 1; r <= 10; r++) {   /* Blink's rows come out a 64th taller now and then */
        L.rowh[r] = r == 1 || r == 8 ? L.colw : lu_ceil(exact);
        L.rowy[r] = r == 1 ? y0 : L.rowy[r - 1] + L.rowh[r - 1] + 5;
    }
    L.num = line_of((float)(0.5 * vw), 1.15f);
    L.sym = line_of((float)(1.7 * vw), 1.15f);
    L.name = line_of((float)(0.7 * vw), 1.15f);
    L.key = line_of((float)(0.8 * vw), 1.5f);
    L.ph = line_of((float)(1.0 * vw), 1.15f);
    L.sigma = 0.5f * (float)(0.4 * vw);   /* Skia's sigma for a blur radius */
    /* the key: grid row 1, columns 3 to 16; a flex row of labels, space-between, centred down */
    L.kx = L.colx[3], L.ky = L.rowy[1], L.kw = L.colx[16] + L.colw - L.colx[3], L.kh = L.rowh[1];
    L.krow = L.ky + lu_div(L.kh - L.key.h, 2);
    double sum = 0;
    for (int i = 0; i < 10; i++) sum += L.lab_w[i] = text_w(c, L.key.size, KEY_TEXT[i]);
    double gap = lu_div(L.kw - sum, 9), x = L.kx;
    for (int i = 0; i < 10; i++) L.lab_x[i] = x, x += L.lab_w[i] + gap;
}

/* ---- what gets painted ------------------------------------------------------------ */

enum { IT_ELEMENT, IT_PLACEHOLDER, IT_LABEL };

typedef struct Item {
    int type, idx, ch;      /* ch: the mask channel of its glow */
    double x, y, w, h;      /* its box, before the scale */
    float s, alpha;         /* scale about the box's middle; opacity of the lot */
    float ls, wa;           /* the label's scale, the shells' list's opacity */
    float model;            /* the model's opacity (0: none) */
    double t_model;         /* seconds the model has turned */
} Item;

typedef struct Run { float size; const char *text; double x, y; float a; bool label, after_model; } Run;

/* The item's texts, in its own box's coordinates (before scaling); label centre out. */
static int runs_of(C2d *c, const Item *it, Run *r, double *lcx, double *lcy) {
    int n = 0;
    *lcx = *lcy = 0;
    if (it->type == IT_LABEL) {
        r[n++] = (Run){ L.key.size, KEY_TEXT[it->idx], it->x, L.krow + L.key.base, 1, false, false };
        return n;
    }
    double cx0 = it->x + 2, cy0 = it->y + 2, cw = it->w - 4, ch = it->h - 4;
    if (it->type == IT_PLACEHOLDER) {
        const char *t = it->idx ? "89-103" : "57-71";
        double tw = text_w(c, L.ph.size, t);
        r[n++] = (Run){ L.ph.size, t, cx0 + lu_div(cw - tw, 2), cy0 + lu_div(ch - L.ph.h, 2) + L.ph.base, 1, false, false };
        return n;
    }
    const Element *e = &ELEMENTS[it->idx];
    double sw = text_w(c, L.sym.size, e->sym), nw = text_w(c, L.name.size, e->name), lw = fmax(sw, nw);
    double lh = L.sym.h + L.name.h, lx = cx0 + lu_div(cw - lw, 2), ly = cy0 + lu_div(ch - lh, 2);
    *lcx = lx + lw / 2, *lcy = ly + lh / 2;
    r[n++] = (Run){ L.sym.size, e->sym, lx + lu_div(lw - sw, 2), ly + L.sym.base, 1, true, false };
    r[n++] = (Run){ L.name.size, e->name, lx + lu_div(lw - nw, 2), ly + L.sym.h + L.name.base, 1, true, false };
    r[n++] = (Run){ L.num.size, e->num, cx0 + 2, cy0 + 2 + L.num.base, 1, false, true };
    double mw = text_w(c, L.num.size, e->mass), mh = L.num.h + 4;
    r[n++] = (Run){ L.num.size, e->mass, cx0 + 2 + lu_div(cw - 4 - mw, 2), cy0 + ch - mh + 2 + L.num.base, 1, false, true };
    if (it->wa > 0) {
        static char shells[118][7][4];
        for (int i = 0; i < e->nshells; i++) {
            char *t = shells[it->idx][i];
            snprintf(t, 4, "%d", e->shells[i]);
            double ww = text_w(c, L.num.size, t);
            r[n++] = (Run){ L.num.size, t, cx0 + cw - 2 - ww, cy0 + 2 + i * L.num.h + L.num.base, it->wa, false, true };
        }
    }
    return n;
}

typedef struct Xf { double cx, cy, s, lcx, lcy, ls; } Xf;
static Xf xf_of(const Item *it, double lcx, double lcy) {
    return (Xf){ it->x + it->w / 2, it->y + it->h / 2, it->s, lcx, lcy, it->ls };
}
static void xf_pt(const Xf *x, double px, double py, bool label, double *ox, double *oy) {
    if (label) px = x->lcx + (px - x->lcx) * x->ls, py = x->lcy + (py - x->lcy) * x->ls;
    *ox = x->cx + (px - x->cx) * x->s, *oy = x->cy + (py - x->cy) * x->s;
}
static void xf_rect(C2d *c, const Xf *x, double x0, double y0, double x1, double y1, C2dColor col) {
    double a, b, d, e;
    xf_pt(x, x0, y0, false, &a, &b);
    xf_pt(x, x1, y1, false, &d, &e);
    c2d_rect(c, (float)a, (float)b, (float)d, (float)e, col);
}

static C2dColor item_color(const Item *it) {
    if (it->type == IT_ELEMENT) return cat_color(ELEMENTS[it->idx].cat, 1);
    if (it->type == IT_PLACEHOLDER) return cat_color(it->idx ? 3 : 2, 1);
    return cat_color(it->idx, 1);
}

/* the masks: each text's coverage, in the item's channel, at (ox, oy) off */
static void emit_masks(C2d *c, const Item *it, double ox, double oy) {
    Run r[16];
    double lcx, lcy;
    int n = runs_of(c, it, r, &lcx, &lcy);
    Xf x = xf_of(it, lcx, lcy);
    for (int i = 0; i < n; i++) {
        double px, py;
        xf_pt(&x, r[i].x, r[i].y, r[i].label, &px, &py);
        if (r[i].a > 0.004f)
            c2d_text_mask(c, arial, (float)(r[i].size * x.s * (r[i].label ? x.ls : 1)), r[i].text, (float)(px - ox), (float)(py - oy), it->ch);
    }
}

/* the item itself, in the page's order */
static void emit_item(C2d *c, const Item *it, float sigma) {
    Run r[16];
    double lcx, lcy;
    int n = runs_of(c, it, r, &lcx, &lcy);
    Xf x = xf_of(it, lcx, lcy);
    C2dColor col = item_color(it);
    float a = it->alpha;
    if (it->type != IT_LABEL) {   /* the square: snapped to pixels, then scaled */
        double X0 = floor(it->x + 0.5), Y0 = floor(it->y + 0.5), X1 = floor(it->x + it->w + 0.5), Y1 = floor(it->y + it->h + 0.5);
        if (it->type == IT_ELEMENT) xf_rect(c, &x, X0, Y0, X1, Y1, fade(BG, a));
        C2dColor b = fade(col, a);
        xf_rect(c, &x, X0, Y0, X1, Y0 + 2, b);
        xf_rect(c, &x, X0, Y1 - 2, X1, Y1, b);
        xf_rect(c, &x, X0, Y0 + 2, X0 + 2, Y1 - 2, b);
        xf_rect(c, &x, X1 - 2, Y0 + 2, X1, Y1 - 2, b);
    }
    /* the glow: the blurred mask over the box and its texts, three sigmas around */
    double gx0, gy0, gx1, gy1;
    xf_pt(&x, it->x, it->y, false, &gx0, &gy0);
    xf_pt(&x, it->x + it->w, it->y + it->h, false, &gx1, &gy1);
    for (int i = 0; i < n; i++) {
        double px, py, s = r[i].size * x.s * (r[i].label ? x.ls : 1);
        xf_pt(&x, r[i].x, r[i].y, r[i].label, &px, &py);
        gx0 = fmin(gx0, px), gx1 = fmax(gx1, px + c2d_measure(c, arial, (float)s, r[i].text));
        gy0 = fmin(gy0, py - s), gy1 = fmax(gy1, py + s * 0.3);
    }
    double m = ceil(3 * sigma) + 2;
    c2d_glow(c, (float)(gx0 - m), (float)(gy0 - m), (float)(gx1 + m), (float)(gy1 + m), it->ch, fade(col, a * GLOW_GAIN));
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1 && it->model > 0) {   /* the model: electron shells, outermost first */
            const Element *e = &ELEMENTS[it->idx];
            double pw = it->w - 4, ph = it->h - 4, cx, cy;
            xf_pt(&x, it->x + it->w / 2, it->y + it->h / 2, false, &cx, &cy);
            for (int k = 1; k <= e->nshells; k++) {
                double mg = (10 + 5.5 * (k - 1)) / 100 * 11 * pw;
                double ro = ((11 * pw - 2 * mg) + (11 * ph - 2 * mg)) / 4 * 0.1 * x.s;
                double dur = 40 - 6 * (k - 1), turn = it->t_model / dur;
                c2d_orbit(c, (float)cx, (float)cy, (float)ro, (float)(0.5 * x.s), e->shells[e->nshells - k],
                          (float)(0.07 * L.w / 100 * x.s), (float)(2 * 3.14159265358979 * (turn - floor(turn))),
                          fade(col, 0.25f * it->model * a));
            }
        }
        for (int i = 0; i < n; i++) {
            if (r[i].after_model != (pass == 1) || r[i].a <= 0.004f) continue;
            double px, py;
            xf_pt(&x, r[i].x, r[i].y, r[i].label, &px, &py);
            c2d_text(c, arial, (float)(r[i].size * x.s * (r[i].label ? x.ls : 1)), r[i].text, (float)px, (float)py, C2D_LEFT, 0,
                     fade(col, a * r[i].a));
        }
    }
}

/* ---- painting with glows ------------------------------------------------------------ */

typedef struct Slot { BareImage m, t; int w, h; } Slot;

static void slot_fit(Slot *s, int w, int h) {
    if (s->m && s->w >= w && s->h >= h) return;
    if (s->m) bare_image_free(s->m), bare_image_free(s->t);
    s->w = w > s->w ? w : s->w, s->h = h > s->h ? h : s->h;
    s->m = bare_canvas(.width = s->w, .height = s->h);
    s->t = bare_canvas(.width = s->w, .height = s->h);
}
static void slot_free(Slot *s) {
    if (s->m) bare_image_free(s->m), bare_image_free(s->t);
    memset(s, 0, sizeof *s);
}

static BareShader blur_prog;

static void blur(BareImage src, BareImage dst, int dx, int dy, float sigma) {
    struct { float dir[4], w[64]; } p;
    memset(&p, 0, sizeof p);
    int r = (int)ceil(3 * sigma);
    if (r > 63) r = 63;
    if (r < 1) r = 1;
    double sum = 0;
    for (int i = 0; i <= r; i++) sum += (i ? 2 : 1) * exp(-(double)i * i / (2.0 * sigma * sigma));
    for (int i = 0; i <= r; i++) p.w[i] = (float)(exp(-(double)i * i / (2.0 * sigma * sigma)) / sum);
    p.dir[0] = (float)dx, p.dir[1] = (float)dy, p.dir[2] = (float)r;
    bare_draw(.target = dst, .shader = blur_prog, .inputs = { src }, .vertex_count = 6, BARE_PARAMS(p));
}

/* Paints items (all at one scale) onto target (tw x th), their texts' glow
 * worked out over the region rx, ry, rw, rh of it. */
static void paint(C2d *c, Slot *sl, const Item *items, int n, int rx, int ry, int rw, int rh,
                  BareImage target, int tw, int th, bool clear) {
    if (rw < 1 || rh < 1 || !n) return;
    slot_fit(sl, rw, rh);
    c2d_atlas_trim(c, 0.6f);
    for (int i = 0; i < n; i++) emit_masks(c, &items[i], rx, ry);
    static const float none[4] = { 0, 0, 0, 0 };
    c->blend = BARE_BLEND_OVER;
    c2d_flush(c, sl->m, sl->w, sl->h, true, none);
    c->blend = 0;
    float sigma = L.sigma * items[0].s;
    blur(sl->m, sl->t, 1, 0, sigma);
    blur(sl->t, sl->m, 0, 1, sigma);
    c->pic[0] = (float)rx, c->pic[1] = (float)ry, c->pic[2] = (float)sl->w, c->pic[3] = (float)sl->h;
    c2d_picture(c, sl->m);
    for (int i = 0; i < n; i++) emit_item(c, &items[i], sigma);
    float bg[4] = { BG.r, BG.g, BG.b, 1 };
    c2d_flush(c, target, tw, th, clear, bg);
    c2d_picture(c, 0);
}

/* ---- transitions ----------------------------------------------------------------- */

typedef struct Tween { double from, to, t0, dur; } Tween;
static double tw_at(const Tween *t, double now) {
    if (t->dur <= 0 || now >= t->t0 + t->dur) return t->to;
    return t->from + (t->to - t->from) * ease((now - t->t0) / t->dur);
}
static void tw_set(Tween *t, double to, double now, double dur) {
    if (to == t->to) return;
    t->from = tw_at(t, now), t->to = to, t->t0 = now, t->dur = dur;
}
static void tw_init(Tween *t, double v) { t->from = t->to = v, t->t0 = 0, t->dur = 0; }
static bool tw_busy(const Tween *t, double now) { return t->dur > 0 && now < t->t0 + t->dur; }

typedef struct State {
    Tween scale, shift, ls, wa, overlay, opacity;
    int z, z_next;          /* z-index; it drops 100 ms late */
    double z_at, t_active;
} State;

static State st[NELEMENTS];
static Tween ph_op[2], gap_op, lab_op[10];
static int hovered = -1, hovered_label = -1, active = -1, category = -1;

static int z_now(int i, double now) {
    State *s = &st[i];
    if (s->z_next != s->z && now >= s->z_at) s->z = s->z_next;
    return s->z;
}

static void retarget(double now) {
    for (int i = 0; i < NELEMENTS; i++) {
        State *s = &st[i];
        bool act = i == active, hov = i == hovered && active < 0;
        double sc = act ? 3 : hov ? 1.35 : 1;
        if (sc != s->scale.to) tw_set(&s->scale, sc, now, act ? 0.5 : 0.1);
        tw_set(&s->shift, act, now, 0.2);
        if ((act ? 0.75 : 1) != s->ls.to) tw_set(&s->ls, act ? 0.75 : 1, now, act ? 0.5 : 0.2);
        if ((act ? 1 : 0) != s->wa.to) tw_set(&s->wa, act ? 1 : 0, now, act ? 0.5 : 0.2);
        tw_set(&s->overlay, act ? 0.75 : 0, now, 0.5);
        tw_set(&s->opacity, category < 0 || ELEMENTS[i].cat == category ? 1 : 0.15, now, 0.5);
        int z = act ? 3 : hov ? 2 : 0;
        z_now(i, now);
        if (z > s->z) s->z = s->z_next = z;
        else if (z < s->z && z != s->z_next) s->z_next = z, s->z_at = now + 0.1;
        if (act && s->t_active < 0) s->t_active = now;
        if (!act) s->t_active = -1;
    }
    for (int k = 0; k < 2; k++)
        tw_set(&ph_op[k], category < 0 || category == 2 + k ? 1 : 0.15, now, 0.5);
    tw_set(&gap_op, category < 0 || category == 2 || category == 3 ? 1 : 0.5, now, 0.5);
    for (int k = 0; k < 10; k++)
        tw_set(&lab_op[k], k == hovered_label ? 1 : category >= 0 && k != category ? 0.65 : 0.85, now, 0.12);
}

static bool busy(double now) {
    for (int i = 0; i < NELEMENTS; i++) {
        State *s = &st[i];
        if (tw_busy(&s->scale, now) || tw_busy(&s->shift, now) || tw_busy(&s->ls, now) || tw_busy(&s->wa, now) ||
            tw_busy(&s->overlay, now) || tw_busy(&s->opacity, now) || s->z != s->z_next || s->t_active >= 0)
            return true;
    }
    for (int k = 0; k < 10; k++) if (tw_busy(&lab_op[k], now)) return true;
    return tw_busy(&ph_op[0], now) || tw_busy(&ph_op[1], now) || tw_busy(&gap_op, now);
}

static Item element_item(int i, double now, bool lifted) {
    const Element *e = &ELEMENTS[i];
    State *s = &st[i];
    Item it = { IT_ELEMENT, i, ((e->row - 1) % 2) * 2 + (e->col - 1) % 2,
                L.colx[e->col], L.rowy[e->row], L.colw, L.rowh[e->row], 1, (float)tw_at(&s->opacity, now), 1, 0, 0, 0 };
    if (lifted) {
        double sh = tw_at(&s->shift, now);
        it.x += sh * (e->col == 1 ? L.colw : e->col == 18 ? -L.colw : 0);
        it.y += sh * (e->row == 1 ? L.rowh[1] : e->row == 10 ? -L.rowh[10] : 0);
        it.s = (float)tw_at(&s->scale, now), it.ls = (float)tw_at(&s->ls, now), it.wa = (float)tw_at(&s->wa, now);
        it.ch = 0;
        if (s->t_active >= 0) it.model = (float)ease(now - s->t_active), it.t_model = now - s->t_active;
    }
    return it;
}

/* the page beneath the lifted tiles: the placeholders, the tiles, the gap's bracket */
static void paint_table(C2d *c, Slot *sl, BareImage table, double now) {
    static Item items[NELEMENTS + 2];
    int n = 0;
    for (int k = 0; k < 2; k++)
        items[n++] = (Item){ IT_PLACEHOLDER, k, ((6 + k - 1) % 2) * 2 + 0, L.colx[3], L.rowy[6 + k], L.colw, L.rowh[6 + k],
                             1, 0.5f * (float)tw_at(&ph_op[k], now), 1, 0, 0, 0 };
    for (int i = 0; i < NELEMENTS; i++)
        if (z_now(i, now) < 3) items[n++] = element_item(i, now, false);
    paint(c, sl, items, n, 0, 0, L.w, L.h, table, L.w, L.h, true);
    /* .gap::before: borders left and bottom, white at 0.2 */
    double gw = L.colw, gh = L.rowh[8], left = L.colx[3] + lu(gw * 0.5) - 1;
    double X0 = floor(left + 0.5), Y0 = floor(L.rowy[8] + 0.5);
    double X1 = floor(left + lu(gw * 0.5) + 2 + 0.5), Y1 = floor(L.rowy[8] + 2 * gh + 8 + 0.5);
    C2dColor wc = { 1, 1, 1, 0.2f * (float)tw_at(&gap_op, now) };
    c2d_rect(c, (float)X0, (float)Y0, (float)X0 + 2, (float)Y1 - 2, wc);
    c2d_rect(c, (float)X0, (float)Y1 - 2, (float)X1, (float)Y1, wc);
    c2d_flush(c, table, L.w, L.h, false, NULL);
}

static void item_region(C2d *c, const Item *it, int *rx, int *ry, int *rw, int *rh) {
    double cx = it->x + it->w / 2, cy = it->y + it->h / 2, hw = it->w / 2 * it->s, hh = it->h / 2 * it->s;
    double m = ceil(3 * L.sigma * it->s) + 4 + 0.6 * it->w * it->s;   /* names can stick out */
    (void)c;
    int x0 = (int)floor(cx - hw - m), y0 = (int)floor(cy - hh - m), x1 = (int)ceil(cx + hw + m), y1 = (int)ceil(cy + hh + m);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > L.w) x1 = L.w;
    if (y1 > L.h) y1 = L.h;
    *rx = x0, *ry = y0, *rw = x1 - x0, *rh = y1 - y0;
}

int main(int argc, char **argv) {
    SpArgs sp = sp_args(argc, argv);
    if (!bare_window_with(.width = sp.width ? sp.width : 1280, .height = sp.height ? sp.height : 720,
                          .title = "Periodic Table", .parent = sp.parent)) return 1;
    bare_background(BARE_RGB(0x10, 0x13, 0x18));
    static const char *const fonts[] = {
        "C:/Windows/Fonts/arial.ttf",   /* the page's sans-serif; elsewhere Arial's metric twin */
        "/usr/share/fonts/TTF/LiberationSans-Regular.ttf", "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    };
    for (size_t i = 0; i < sizeof fonts / sizeof *fonts && !arial; i++) {
        FILE *f = fopen(fonts[i], "rb");
        if (f) fclose(f), arial = bare_font_load(fonts[i]);
    }
    blur_prog = bare_shader(&blur_shader);
    for (int i = 0; i < 256; i++) text_lut[i] = (unsigned char)floor(pow(i / 255.0, TEXT_GAMMA) * 255 + 0.5);
    C2d c;
    c2d_init(&c, bare_shader(&canvas2d_shader));
    c.lut = text_lut;
    for (int i = 0; i < NELEMENTS; i++) {
        State *s = &st[i];
        tw_init(&s->scale, 1), tw_init(&s->shift, 0), tw_init(&s->ls, 1), tw_init(&s->wa, 0);
        tw_init(&s->overlay, 0), tw_init(&s->opacity, 1);
        s->t_active = -1;
    }
    tw_init(&ph_op[0], 1), tw_init(&ph_op[1], 1), tw_init(&gap_op, 1);
    for (int k = 0; k < 10; k++) tw_init(&lab_op[k], 0.85);

    BareImage canvas = 0, table = 0;
    Slot s_table = { 0 }, s_key = { 0 }, s_sprite = { 0 };
    int cw = 0, ch = 0;
    bool paused = false, was_busy = true, table_dirty = true, was_fading = false;
    int excluded = -2;

    while (bare_frame()) {
        SpMessage m;
        while (sp_next(&m))
            if (m.kind == SP_PAUSE) paused = sp_bool(&m);
        BareVec2 win = bare_window_size();
        int w = (int)fmax(1, floor(win.x + 0.5)), h = (int)fmax(1, floor(win.y + 0.5));
        bool dirty = false;
        if (w != cw || h != ch) {
            if (canvas) bare_image_free(canvas), bare_image_free(table);
            canvas = bare_canvas(.width = w, .height = h);
            table = bare_canvas(.width = w, .height = h);
            slot_free(&s_table);
            cw = w, ch = h;
            layout(&c, w, h);
            table_dirty = dirty = true;
        }
        BareState box = bare_box(.width = BARE_GROW, .height = BARE_GROW, .image = canvas);
        if (paused) { bare_continuous(false); continue; }
        double now = bare_time();

        /* the mouse: the tile or key label under it; while a tile is lifted, a
         * page-wide input over everything takes the click that puts it back */
        BareVec2 mo = bare_mouse();
        int hv = -1, hl = -1;
        if (active < 0) {
            for (int i = 0; i < NELEMENTS; i++) {
                const Element *e = &ELEMENTS[i];
                if (mo.x >= L.colx[e->col] && mo.x < L.colx[e->col] + L.colw && mo.y >= L.rowy[e->row] &&
                    mo.y < L.rowy[e->row] + L.rowh[e->row]) hv = i;
            }
            for (int k = 0; k < 10 && hv < 0; k++)
                if (mo.x >= L.lab_x[k] && mo.x < L.lab_x[k] + L.lab_w[k] && mo.y >= L.krow && mo.y < L.krow + L.key.h) hl = k;
        }
        bool changed = hv != hovered || hl != hovered_label;
        static bool dbg_done;   /* testing: PERIODIC_CLICK=n starts with tile n lifted, PERIODIC_KEY=k with category k */
        if (!dbg_done) {
            dbg_done = true;
            if (getenv("PERIODIC_CLICK")) active = atoi(getenv("PERIODIC_CLICK")), changed = true;
            if (getenv("PERIODIC_KEY")) category = atoi(getenv("PERIODIC_KEY")), changed = true;
        }
        hovered = hv, hovered_label = hl;
        if (box.clicked) {
            if (active >= 0) active = -1;
            else if (hovered >= 0) active = hovered, hovered = -1;
            else if (hovered_label >= 0) category = hovered_label;
            else category = -1;
            changed = true;
        }
        if (changed) retarget(now), dirty = true;

        bool b = busy(now);
        if (!(dirty || b || was_busy)) { bare_continuous(false); continue; }
        was_busy = b;

        /* the table again when its tiles' opacities move or a tile leaves or joins it */
        int ex = -1;
        bool fading = tw_busy(&ph_op[0], now) || tw_busy(&ph_op[1], now) || tw_busy(&gap_op, now);
        for (int i = 0; i < NELEMENTS; i++) {
            if (z_now(i, now) == 3) ex = i;
            if (tw_busy(&st[i].opacity, now)) fading = true;
        }
        if (table_dirty || fading || was_fading || ex != excluded) paint_table(&c, &s_table, table, now);
        table_dirty = false, excluded = ex, was_fading = fading;

        /* the page: the table, the lifted tile's overlay, the key, the lifted tiles */
        c2d_picture(&c, table);
        c2d_image(&c, 0, 0, (float)w, (float)h, 1);
        for (int i = 0; i < NELEMENTS; i++) {
            double ov = tw_at(&st[i].overlay, now) * tw_at(&st[i].opacity, now);
            if (ov > 0) c2d_rect(&c, 0, 0, (float)w, (float)h, fade(BG, (float)ov));
        }
        c2d_flush(&c, canvas, w, h, false, NULL);
        c2d_picture(&c, 0);
        Item labels[10];
        for (int k = 0; k < 10; k++)
            labels[k] = (Item){ IT_LABEL, k, 0, L.lab_x[k], L.krow, L.lab_w[k], L.key.h, 1, (float)tw_at(&lab_op[k], now), 1, 0, 0, 0 };
        double km = ceil(3 * L.sigma) + 4 + L.key.size;
        int kx0 = (int)fmax(0, floor(L.kx - km)), ky0 = (int)fmax(0, floor(L.krow - km));
        paint(&c, &s_key, labels, 10, kx0, ky0, (int)fmin(w, ceil(L.kx + L.kw + km)) - kx0,
              (int)fmin(h, ceil(L.krow + L.key.h + km)) - ky0, canvas, w, h, false);
        for (int z = 2; z <= 3; z++)
            for (int i = 0; i < NELEMENTS; i++) {
                if (z_now(i, now) != z) continue;
                Item it = element_item(i, now, true);
                int rx, ry, rw, rh;
                item_region(&c, &it, &rx, &ry, &rw, &rh);
                paint(&c, &s_sprite, &it, 1, rx, ry, rw, rh, canvas, w, h, false);
            }
        bare_continuous(true);
    }
    bare_quit();
}
