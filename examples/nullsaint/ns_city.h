/* ns_city.h: city.js: sky, sky object, parallax skyline, signs, traffic, rain, smog, lightning. */
#ifndef NS_CITY_H
#define NS_CITY_H
#include "ns_state.h"

static const double DEPTHS[4] = { 0.18, 0.38, 0.62, 1.0 };
typedef struct Win { double x, y, w, h, ph; Rgb c; } Win;
typedef struct Sign { double x, y, w, ph; int word; Rgb c; bool broken; } Sign;
typedef struct Antenna { double x, y, ph, sp; } Antenna;
typedef struct Layer {
    BareImage cv;
    int cw, ch;
    double depth, margin, base;
    Win windows[400];
    int nwin;
    Sign signs[64];
    int nsigns;
    Antenna ant[128];
    int nant;
} Layer;
typedef struct Star { double x, y, s, ph; bool glitch; } Star;
typedef struct Car { double d, x, y, sp, bob, len; int dir; Rgb c; } Car;
typedef struct Drop { double x, y, d, l; } Drop;
typedef struct Splash { double x, y, t; } Splash;
typedef struct Bolt { double pts[15][2]; double t; int branch; } Bolt;

static struct {
    Layer layers[4];
    Star stars[160];
    Car cars[64];
    int ncars;
    Drop drops[700];
    int ndrops;
    Splash splashes[60];
    int nsplash;
    Bolt bolts[16];
    int nbolts;
    char key[512];
} City;

static inline void city_key(char *out, size_t cap) {
    snprintf(out, cap, "%g|%g|%s|%g|%g|%g|%s|", F.W, F.H, U8(CS(seed)), C(cityDensity), C(cityHeight), C(theme), U8(CS(colA)));
    size_t n = strlen(out);
    snprintf(out + n, cap - n, "%s|%s|%s|%s", U8(CS(colB)), U8(CS(colC)), U8(CS(colBg)), U8(CS(colBg2)));
}

static void city_build(CxEngine *e) {
    double W = F.W, H = F.H;
    Pal P = F.P;
    Str sd = CS(seed)->n ? *CS(seed) : s_of("NULLSAINT");
    Rng r = rng_new(str_seed(&sd));
    double margin = floor(W * 0.16 + 0.5);
    double dens = C(cityDensity) / 100, tall = C(cityHeight) / 100;
    for (int li = 0; li < 4; li++) {
        Layer *L = &City.layers[li];
        double depth = DEPTHS[li];
        int cw = (int)(W + margin * 2), ch = (int)H;
        if (L->cv) bare_image_free(L->cv);
        L->cv = bare_canvas(.width = cw, .height = ch);
        L->cw = cw, L->ch = ch;
        CxCtx x;
        cx_ctx_init(&x, e, L->cv, cw, ch);
        cx_clear(&x);
        double fogK = 1 - depth;
        Rgb body = mixc(scalec(P.bg, 0.6 + depth * 0.4), P.bg2, fogK * 0.55);
        Rgb edge = mixc(body, P.b, 0.25 + fogK * 0.1);
        L->nwin = L->nsigns = L->nant = 0;
        double base = H * (0.98 - (1 - depth) * 0.12);
        double px = -rng_range(&r, 0, 40);
        while (px < cw) {
            double r1 = rng_range(&r, 30, 110);
            double r2 = rng_range(&r, 0, 70);
            double bw = (r1 + r2 * depth) * (W / 1920) * (1.2 - dens * 0.5);
            double gap = rng_range(&r, 0, 22) * (1.2 - dens) * (W / 1920);
            double hmax = H * (0.18 + 0.55 * tall) * (0.45 + depth * 0.55);
            double bh = rng_range(&r, 0.25, 1) * hmax;
            if (rng_chance(&r, 0.08)) bh *= 1.45;
            double top = base - bh;
            cx_fill_color(&x, rgba(body, 1));
            cx_fill_rect(&x, px, top, bw, base - top + 4);
            int tiers = rng_chance(&r, 0.45) ? rng_int(&r, 1, 3) : 0;
            double tw = bw, tx = px, ty = top;
            for (int k = 0; k < tiers; k++) {
                tw *= rng_range(&r, 0.5, 0.8);
                tx += (bw - tw) / 2 * rng_range(&r, 0.6, 1.4) - (bw - tw) / 2 + (bw - tw) / 2;
                double th = rng_range(&r, 10, 60) * (0.5 + depth);
                tx = clampd(tx, px, px + bw - tw);
                cx_fill_rect(&x, tx, ty - th, tw, th + 1);
                ty -= th;
            }
            if (rng_chance(&r, 0.35)) {
                double ax = tx + tw / 2, ah = rng_range(&r, 20, 90) * (0.4 + depth);
                cx_stroke_color(&x, rgba(edge, 1)), cx_line_width(&x, fmax(1, depth * 2));
                cx_begin(&x), cx_move(&x, ax, ty), cx_line(&x, ax, ty - ah), cx_stroke(&x);
                Antenna *a = &L->ant[L->nant < 128 ? L->nant++ : 127];
                a->x = ax, a->y = ty - ah, a->ph = rng(&r) * 10, a->sp = rng_range(&r, 0.5, 2);
            }
            cx_fill_color(&x, rgba(edge, 0.5));
            cx_fill_rect(&x, px, top, fmax(1, depth * 2), base - top);
            if (depth > 0.5 && rng_chance(&r, 0.3)) {
                int pk = rng_pick(&r, 3);
                Rgb nc = pk == 0 ? P.a : pk == 1 ? P.b : P.c;
                cx_fill_color(&x, rgba(nc, 0.85));
                cx_fill_rect(&x, px + bw - 2, top, 2, base - top);
                if (rng_chance(&r, 0.5)) cx_fill_rect(&x, px, top, bw, 2);
            }
            double ws = fmax(2, (3 + depth * 4) * (W / 1920)), wg = ws * rng_range(&r, 1.4, 2.4);
            double lit = rng_range(&r, 0.1, 0.55);
            int tp = rng_pick(&r, 5);
            Rgb tint = tp == 0 ? P.c : tp == 1 ? P.b : tp == 2 ? P.a : tp == 3 ? rgb(255, 230, 190) : rgb(190, 230, 255);
            int style = rng_int(&r, 0, 3);
            for (double wy = top + wg; wy < base - wg; wy += wg * (style == 1 ? 1.6 : 1)) {
                if (style == 1) {
                    if (rng_chance(&r, lit)) {
                        cx_fill_color(&x, rgba(tint, 0.25 + 0.5 * depth));
                        cx_fill_rect(&x, px + ws, wy, bw - ws * 2, ws * 0.6);
                    }
                    continue;
                }
                for (double wx = px + ws; wx < px + bw - ws * 1.5; wx += wg) {
                    if (style == 2 && rng_chance(&r, 0.7)) continue;
                    if (rng_chance(&r, lit)) {
                        double a = (0.35 + rng(&r) * 0.55) * (0.4 + depth * 0.6);
                        cx_fill_color(&x, rgba(rng_chance(&r, 0.06) ? P.a : tint, a));
                        cx_fill_rect(&x, wx, wy, ws, ws * (style == 3 ? 0.4 : 1));
                        if (L->nwin < 400 && rng_chance(&r, 0.08)) {
                            Win *w = &L->windows[L->nwin++];
                            w->x = wx, w->y = wy, w->w = ws, w->h = ws, w->c = tint, w->ph = rng(&r) * 100;
                        }
                    }
                }
            }
            if (depth > 0.3 && rng_chance(&r, 0.18 + dens * 0.15) && bh > H * 0.12) {
                int word = rng_pick(&r, NITEMS(SIGNS));
                double sw = fmax(10, (14 + depth * 22) * (W / 1920));
                double sx = px + (rng_chance(&r, 0.5) ? -sw * 0.6 : bw - sw * 0.4);
                double sy = top + rng_range(&r, 10, fmax(12, bh * 0.4));
                int pc = rng_pick(&r, 4);
                Sign *s = &L->signs[L->nsigns < 64 ? L->nsigns++ : 63];
                s->x = sx, s->y = sy, s->w = sw, s->word = word;
                s->c = pc == 0 ? P.a : pc == 1 ? P.b : pc == 2 ? P.c : P.warn;
                s->ph = rng(&r) * 100;
                s->broken = rng_chance(&r, 0.3);
            }
            px += bw + gap;
        }
        cx_fill_color(&x, rgba(scalec(body, 0.8), 1));
        cx_fill_rect(&x, 0, base, cw, H - base);
        L->depth = depth, L->margin = margin, L->base = base;
        cx_ctx_free(&x);
    }
    for (int i = 0; i < 160; i++) {
        Star *s = &City.stars[i];
        s->x = rng(&r) * W;
        s->y = rng(&r) * H * 0.6;
        s->s = rng_range(&r, 0.5, 2);
        s->ph = rng(&r) * 10;
        s->glitch = rng_chance(&r, 0.05);
    }
    City.ncars = City.ndrops = City.nbolts = 0;
    city_key(City.key, sizeof City.key);
}

static void sky(void) {
    CxCtx *ctx = F.ctx;
    double W = F.W, H = F.H, t = F.t;
    Pal P = F.P;
    CxGrad g = cx_linear(0, 0, 0, H);
    cx_stop(&g, 0, rgba(P.bg, 1));
    cx_stop(&g, 0.62, rgba(mixc(P.bg, P.bg2, 0.7), 1));
    cx_stop(&g, 0.85, rgba(P.bg2, 1));
    cx_stop(&g, 1, rgba(scalec(P.bg2, 0.6), 1));
    cx_fill_grad(ctx, &g), cx_fill_rect(ctx, 0, 0, W, H);
    cx_op(ctx, CX_LIGHTER);
    for (int i = 0; i < 3; i++) {
        double y = H * (0.12 + i * 0.09) + sin(t * 0.2 + i) * 20;
        CxGrad gg = cx_linear(0, y - 40, 0, y + 40);
        Rgb c = i == 0 ? P.a : i == 1 ? P.b : P.c;
        cx_stop(&gg, 0, rgba(c, 0)), cx_stop(&gg, 0.5, rgba(c, 0.035 + A.mid * 0.08)), cx_stop(&gg, 1, rgba(c, 0));
        cx_fill_grad(ctx, &gg), cx_fill_rect(ctx, 0, y - 40, W, 80);
    }
    cx_op(ctx, CX_OVER);
    for (int i = 0; i < 160; i++) {
        const Star *s = &City.stars[i];
        double tw = 0.4 + 0.6 * fabs(sin(t * 1.3 + s->ph));
        cx_fill_color(ctx, rgba(s->glitch ? P.a : P.text, tw * 0.8));
        double ox = s->glitch && sin(t * 7 + s->ph) > 0.9 ? urand(-20, 20) : 0;
        cx_fill_rect(ctx, s->x + ox - F.par * 0.1, s->y, s->s, s->s);
    }
}

static void sky_object(void) {
    CxCtx *ctx = F.ctx;
    double W = F.W, H = F.H, t = F.t;
    Pal P = F.P;
    int kind = (int)C(moon);
    if (!kind) return;
    double R = fmin(W, H) * (0.06 + C(moonSize) / 100 * 0.22) * (1 + A.bass * 0.06);
    double cx = W * 0.72 - F.par * 0.15, cy = H * 0.3 - F.parY * 0.1;
    cx_save(ctx);
    if (kind == 1) {
        for (int i = 0; i < 18; i++) {
            double y0 = cy - R + (2 * R * i) / 18, sh = (2 * R) / 18;
            double n = noise2(i * 0.7, t * 0.4);
            double dx = (n > 0.72 ? (n - 0.72) * 600 : 0) * (i % 2 ? 1 : -1) * F.gi;
            cx_save(ctx), cx_begin(ctx), cx_rect(ctx, cx - R * 2, y0, R * 4, sh + 0.5), cx_clip(ctx);
            CxGrad g = cx_radial(cx + dx - R * 0.3, cy - R * 0.3, R * 0.1, cx + dx, cy, R);
            cx_stop(&g, 0, rgba(mixc(P.text, WHITE, 0.5), 0.95));
            cx_stop(&g, 0.7, rgba(mixc(P.text, P.b, 0.4), 0.8));
            cx_stop(&g, 1, rgba(P.b, 0.25));
            cx_fill_grad(ctx, &g), cx_begin(ctx), cx_arc(ctx, cx + dx, cy, R, 0, TAU, false), cx_fill(ctx);
            cx_restore(ctx);
        }
        Rng r = rng_new(42);
        cx_fill_color(ctx, rgba(P.bg, 0.18));
        for (int i = 0; i < 9; i++) {
            double a = rng(&r) * TAU, d = rng(&r) * R * 0.75;
            cx_begin(ctx), cx_arc(ctx, cx + cos(a) * d, cy + sin(a) * d, R * rng_range(&r, 0.04, 0.16), 0, TAU, false), cx_fill(ctx);
        }
        CxGrad hg = cx_radial(cx, cy, R, cx, cy, R * 2.4);
        cx_stop(&hg, 0, rgba(P.b, 0.18)), cx_stop(&hg, 1, rgba(P.b, 0));
        cx_fill_grad(ctx, &hg), cx_fill_rect(ctx, cx - R * 3, cy - R * 3, R * 6, R * 6);
    } else if (kind == 2) {
        CxGrad g = cx_linear(0, cy - R, 0, cy + R);
        cx_stop(&g, 0, rgba(P.c, 1)), cx_stop(&g, 0.55, rgba(P.a, 1)), cx_stop(&g, 1, rgba(P.warn, 1));
        cx_begin(ctx), cx_arc(ctx, cx, cy, R, 0, TAU, false), cx_fill_grad(ctx, &g), cx_fill(ctx);
        cx_op(ctx, CX_DST_OUT);
        for (int i = 0; i < 9; i++) {
            double yy = cy + R * (0.05 + i * 0.11) + fmod(t * 12, R * 0.11);
            cx_fill_rect(ctx, cx - R, yy, R * 2, 1 + i * R * 0.012);
        }
        cx_op(ctx, CX_OVER);
        CxGrad hg = cx_radial(cx, cy, R * 0.8, cx, cy, R * 2.2);
        cx_stop(&hg, 0, rgba(P.a, 0.25)), cx_stop(&hg, 1, rgba(P.a, 0));
        cx_fill_grad(ctx, &hg), cx_fill_rect(ctx, cx - R * 3, cy - R * 3, R * 6, R * 6);
    } else if (kind == 3) {
        cx_op(ctx, CX_LIGHTER);
        for (int i = 0; i < 40; i++) {
            double a = t * (0.4 + i * 0.01) + i * 0.6;
            cx_stroke_color(ctx, rgba(mixc(P.c, P.a, i / 40.0), 0.09));
            cx_line_width(ctx, R * 0.05);
            cx_begin(ctx), cx_ellipse(ctx, cx, cy, R * (1.3 + i * 0.025), R * (0.32 + i * 0.006), -0.25, a, a + 2.4, false), cx_stroke(ctx);
        }
        cx_op(ctx, CX_OVER);
        CxGrad hg = cx_radial(cx, cy, R * 0.6, cx, cy, R * 1.3);
        cx_stop(&hg, 0, cx_rgba(0, 0, 0, 1)), cx_stop(&hg, 0.75, cx_rgba(0, 0, 0, 1)), cx_stop(&hg, 1, rgba(P.c, 0.6));
        cx_fill_grad(ctx, &hg), cx_begin(ctx), cx_arc(ctx, cx, cy, R, 0, TAU, false), cx_fill(ctx);
        cx_stroke_color(ctx, rgba(P.c, 0.9)), cx_line_width(ctx, 2);
        cx_begin(ctx), cx_arc(ctx, cx, cy, R * 0.72, 0, TAU, false), cx_stroke(ctx);
    } else if (kind == 4) {
        double lx = clampd((F.mx - cx) / W, -0.5, 0.5), ly = clampd((F.my - cy) / H, -0.5, 0.5);
        double open = F.blink;
        cx_begin(ctx), cx_ellipse(ctx, cx, cy, R * 1.6, R * 0.8 * open, 0, 0, TAU, false);
        cx_fill_color(ctx, rgba(mixc(P.text, P.bg2, 0.3), 0.9)), cx_fill(ctx);
        cx_save(ctx), cx_clip(ctx);
        double ix = cx + lx * R * 1.4, iy = cy + ly * R * 0.8;
        CxGrad ig = cx_radial(ix, iy, R * 0.1, ix, iy, R * 0.62);
        cx_stop(&ig, 0, rgba(P.c, 1)), cx_stop(&ig, 0.5, rgba(P.a, 1)), cx_stop(&ig, 1, rgba(P.bg, 1));
        cx_fill_grad(ctx, &ig), cx_begin(ctx), cx_arc(ctx, ix, iy, R * 0.62, 0, TAU, false), cx_fill(ctx);
        cx_fill_color(ctx, cx_rgba(0, 0, 0, 1)), cx_begin(ctx), cx_arc(ctx, ix, iy, R * (0.18 + A.bass * 0.15), 0, TAU, false), cx_fill(ctx);
        cx_restore(ctx);
        cx_stroke_color(ctx, rgba(P.a, 0.8)), cx_line_width(ctx, 3);
        cx_begin(ctx), cx_ellipse(ctx, cx, cy, R * 1.6, R * 0.8 * open, 0, 0, TAU, false), cx_stroke(ctx);
    } else if (kind == 5) {
        double tilt = -0.35;
        cx_stroke_color(ctx, rgba(P.b, 0.6)), cx_line_width(ctx, R * 0.08);
        cx_begin(ctx), cx_ellipse(ctx, cx, cy, R * 1.9, R * 0.45, tilt, PI, TAU, false), cx_stroke(ctx);
        CxGrad g = cx_radial(cx - R * 0.4, cy - R * 0.4, R * 0.1, cx, cy, R);
        cx_stop(&g, 0, rgba(P.c, 1)), cx_stop(&g, 1, rgba(P.a, 0.9));
        cx_fill_grad(ctx, &g), cx_begin(ctx), cx_arc(ctx, cx, cy, R, 0, TAU, false), cx_fill(ctx);
        for (int i = 0; i < 6; i++) {
            cx_fill_color(ctx, rgba(P.bg, 0.08));
            cx_fill_rect(ctx, cx - R, cy - R + i * R * 0.33 + sin(t + i) * 4, R * 2, R * 0.1);
        }
        cx_stroke_color(ctx, rgba(P.b, 0.75)), cx_line_width(ctx, R * 0.08);
        cx_begin(ctx), cx_ellipse(ctx, cx, cy, R * 1.9, R * 0.45, tilt, 0, PI, false), cx_stroke(ctx);
    }
    cx_restore(ctx);
}

static void searchlights(void) {
    CxCtx *ctx = F.ctx;
    double W = F.W, H = F.H, t = F.t;
    cx_save(ctx), cx_op(ctx, CX_LIGHTER);
    for (int i = 0; i < 4; i++) {
        double bx = W * (0.12 + i * 0.26), by = H * 0.85;
        double ang = -PI / 2 + sin(t * (0.21 + i * 0.07) + i * 2) * 0.55;
        double len = H * 1.2, spread = 0.05;
        CxGrad g = cx_linear(bx, by, bx + cos(ang) * len, by + sin(ang) * len);
        Rgb c = i % 2 ? F.P.b : F.P.a;
        cx_stop(&g, 0, rgba(c, 0.16)), cx_stop(&g, 1, rgba(c, 0));
        cx_fill_grad(ctx, &g);
        cx_begin(ctx), cx_move(ctx, bx, by);
        cx_line(ctx, bx + cos(ang - spread) * len, by + sin(ang - spread) * len);
        cx_line(ctx, bx + cos(ang + spread) * len, by + sin(ang + spread) * len);
        cx_close(ctx), cx_fill(ctx);
    }
    cx_restore(ctx);
}

static void traffic(double dmin, double dmax) {
    CxCtx *ctx = F.ctx;
    double W = F.W, H = F.H, dt = F.dt;
    Pal P = F.P;
    int want = (int)floor(C(cars) * 0.6 + 0.5);
    while (City.ncars < want && City.ncars < 64) {
        Car *c = &City.cars[City.ncars++];
        c->d = urand(0.15, 1);
        c->dir = chance(0.5) ? 1 : -1;
        c->x = urand(0, W);
        c->y = H * urand(0.2, 0.72);
        c->sp = urand(60, 240) * (0.3 + c->d);
        if (chance(0.5)) c->c = P.warn;
        else { int k = pick_i(4); c->c = k == 0 ? P.a : k == 1 ? P.b : k == 2 ? P.c : WHITE; }
        c->bob = urand(0, 10);
        c->len = urand(20, 90) * c->d;
    }
    if (City.ncars > want) City.ncars = want;
    cx_save(ctx), cx_op(ctx, CX_LIGHTER);
    for (int i = 0; i < City.ncars; i++) {
        Car *c = &City.cars[i];
        if (c->d < dmin || c->d >= dmax) continue;
        c->x += c->dir * c->sp * dt * F.speed;
        if (c->x > W + 120) c->x = -120;
        if (c->x < -120) c->x = W + 120;
        double y = c->y + sin(F.t * 0.8 + c->bob) * 6 * c->d - F.parY * c->d * 0.3;
        double x = c->x - F.par * c->d;
        double s = 1.5 + c->d * 3;
        CxGrad g = cx_linear(x, y, x - c->dir * c->len, y);
        cx_stop(&g, 0, rgba(c->c, 0.9)), cx_stop(&g, 1, rgba(c->c, 0));
        cx_fill_grad(ctx, &g), cx_fill_rect(ctx, fmin(x, x - c->dir * c->len), y - s * 0.3, c->len, s * 0.6);
        cx_fill_color(ctx, rgba(WHITE, 0.95)), cx_fill_rect(ctx, x - s / 2, y - s / 2, s, s);
    }
    cx_restore(ctx);
}

static void draw_layer(const Layer *L) {
    CxCtx *ctx = F.ctx;
    double t = F.t;
    Pal P = F.P;
    double ox = -L->margin - F.par * L->depth, oy = -F.parY * L->depth * 0.4;
    cx_draw_image(ctx, L->cv, L->cw, L->ch, ox, oy);
    for (int i = 0; i < L->nwin; i++) {
        const Win *w = &L->windows[i];
        double n = noise1(t * 0.7 + w->ph);
        if (n > 0.62) { cx_fill_color(ctx, rgba(w->c, (n - 0.62) * 2.2 * (0.5 + L->depth * 0.5))); cx_fill_rect(ctx, w->x + ox, w->y + oy, w->w, w->h); }
        else if (n < 0.12) { cx_fill_color(ctx, rgba(P.bg, 0.9)); cx_fill_rect(ctx, w->x + ox, w->y + oy, w->w, w->h); }
    }
    for (int i = 0; i < L->nant; i++) {
        const Antenna *a = &L->ant[i];
        if (sin(t * a->sp * 3 + a->ph) > 0.6) { cx_fill_color(ctx, rgba(P.warn, 0.95)); cx_fill_rect(ctx, a->x + ox - 1.5, a->y + oy - 1.5, 3, 3); }
    }
    if (CB(signs))
        for (int i = 0; i < L->nsigns; i++) {
            const Sign *s = &L->signs[i];
            double flick = s->broken ? (noise1(t * 6 + s->ph) > 0.35 ? 1 : 0.15) : 0.85 + 0.15 * sin(t * 30 + s->ph);
            Str txt = s_of(SIGNS[s->word]);
            int n = txt.n;
            double sh = s->w * 1.1 * n + s->w * 0.4;
            double x = s->x + ox, y = s->y + oy;
            cx_fill_color(ctx, rgba(scalec(s->c, 0.12), 0.85)), cx_fill_rect(ctx, x, y, s->w, sh);
            cx_stroke_color(ctx, rgba(s->c, 0.7 * flick)), cx_line_width(ctx, 1.5), cx_stroke_rect(ctx, x, y, s->w, sh);
            cx_fill_color(ctx, rgba(s->c, flick));
            ufont(ctx, s->w * 0.85, F_JP_B), cx_text_align(ctx, CX_CENTER), cx_text_baseline(ctx, CX_TOP);
            for (int k = 0; k < n; k++) {
                Str ch = s_slice(&txt, k, k + 1);
                fill_str(ctx, &ch, x + s->w / 2, y + s->w * 0.25 + k * s->w * 1.1);
            }
            cx_fill_color(ctx, rgba(s->c, 0.06 * flick)), cx_fill_rect(ctx, x - s->w * 0.6, y - s->w * 0.3, s->w * 2.2, sh + s->w * 0.6);
        }
}

static void fog(double y, double strength, Rgb c) {
    CxCtx *ctx = F.ctx;
    double W = F.W, H = F.H, t = F.t;
    if (strength <= 0) return;
    CxGrad g = cx_linear(0, y - H * 0.15, 0, y + H * 0.1);
    cx_stop(&g, 0, rgba(c, 0)), cx_stop(&g, 0.6, rgba(c, 0.35 * strength)), cx_stop(&g, 1, rgba(c, 0.1 * strength));
    cx_fill_grad(ctx, &g), cx_fill_rect(ctx, 0, y - H * 0.15, W, H * 0.25);
    for (int i = 0; i < 5; i++) {
        double bx = wrapd(i * W * 0.3 + t * 12 * (1 + i * 0.2), -W * 0.3, W * 1.3);
        CxGrad rg = cx_radial(bx, y, 0, bx, y, W * 0.2);
        cx_stop(&rg, 0, rgba(c, 0.12 * strength)), cx_stop(&rg, 1, rgba(c, 0));
        cx_fill_grad(ctx, &rg), cx_fill_rect(ctx, bx - W * 0.2, y - W * 0.2, W * 0.4, W * 0.4);
    }
}

static void city_splash(double x, double y) {
    if (City.nsplash < 60) { Splash *s = &City.splashes[City.nsplash++]; s->x = x, s->y = y, s->t = 0; }
}
static void rain(void) {
    CxCtx *ctx = F.ctx;
    double W = F.W, H = F.H, dt = F.dt;
    int want = (int)floor(C(weather) * 6 * (W / 1920) + 0.5);
    if (want > 700) want = 700;
    while (City.ndrops < want) {
        Drop *d = &City.drops[City.ndrops++];
        d->x = urand(-W * 0.2, W * 1.1);
        d->y = urand(-H, H);
        d->d = urand(0.3, 1);
        d->l = urand(8, 26);
    }
    if (City.ndrops > want) City.ndrops = want;
    double wind = 0.25 + I.vx * 0.0004;
    cx_stroke_color(ctx, rgba(mixc(F.P.text, F.P.b, 0.4), 0.35));
    cx_line_width(ctx, 1);
    cx_begin(ctx);
    for (int i = 0; i < City.ndrops; i++) {
        Drop *d = &City.drops[i];
        double sp = (700 + 700 * d->d) * dt * F.speed;
        d->y += sp, d->x += sp * wind;
        if (d->y > H) {
            d->y = urand(-80, -10);
            d->x = urand(-W * 0.2, W * 1.05);
            if (chance(0.15)) city_splash(d->x, H * urand(0.86, 0.99));
        }
        cx_move(ctx, d->x, d->y), cx_line(ctx, d->x - d->l * wind * d->d, d->y - d->l * d->d);
    }
    cx_stroke(ctx);
}
static void splashes(void) {
    CxCtx *ctx = F.ctx;
    cx_stroke_color(ctx, rgba(F.P.b, 0.4)), cx_line_width(ctx, 1);
    for (int i = City.nsplash - 1; i >= 0; i--) {
        Splash *s = &City.splashes[i];
        s->t += F.dt * 3;
        if (s->t > 1) { memmove(&City.splashes[i], &City.splashes[i + 1], (size_t)(City.nsplash - i - 1) * sizeof *s); City.nsplash--; continue; }
        cx_alpha(ctx, 1 - s->t);
        cx_begin(ctx), cx_ellipse(ctx, s->x, s->y, 2 + s->t * 10, 1 + s->t * 2, 0, 0, TAU, false), cx_stroke(ctx);
    }
    cx_alpha(ctx, 1);
}

static void city_strike(void) {
    double W = F.W, H = F.H;
    if (City.nbolts >= 16) return;
    Bolt *b = &City.bolts[City.nbolts];
    double x = urand(W * 0.1, W * 0.9), y = 0;
    double tx = x + urand(-W * 0.15, W * 0.15), ty = H * urand(0.45, 0.75);
    int n = 14;
    for (int i = 0; i <= n; i++) {
        double k = (double)i / n;
        double j = i && i < n ? urand(-40, 40) : 0;
        b->pts[i][0] = lerp(x, tx, k) + j, b->pts[i][1] = lerp(y, ty, k);
    }
    b->t = 0, b->branch = randi(0, n - 3);
    City.nbolts++;
    flash(0.25, F.P.b);
}
static void bolts(void) {
    CxCtx *ctx = F.ctx;
    Pal P = F.P;
    for (int i = City.nbolts - 1; i >= 0; i--) {
        Bolt *b = &City.bolts[i];
        b->t += F.dt * 2.5;
        if (b->t > 1) { memmove(&City.bolts[i], &City.bolts[i + 1], (size_t)(City.nbolts - i - 1) * sizeof *b); City.nbolts--; continue; }
        double a = (1 - b->t) * (rnd() > 0.3 ? 1 : 0.2);
        cx_save(ctx), cx_op(ctx, CX_LIGHTER);
        const double ws[3] = { 8, 3, 1 };
        const Rgb cs[3] = { P.b, P.text, WHITE };
        for (int k = 0; k < 3; k++) {
            cx_stroke_color(ctx, rgba(cs[k], a * (ws[k] > 4 ? 0.25 : 0.9))), cx_line_width(ctx, ws[k]);
            cx_begin(ctx);
            for (int p = 0; p < 15; p++) p ? cx_line(ctx, b->pts[p][0], b->pts[p][1]) : cx_move(ctx, b->pts[p][0], b->pts[p][1]);
            cx_stroke(ctx);
        }
        cx_fill_color(ctx, rgba(P.c, a)), ufont(ctx, 11, F_MONO);
        for (int k = 0; k < 15; k += 3) {
            Str h = uhex(randi(0, 65535), 4);
            fill_str(ctx, &h, b->pts[k][0] + 6, b->pts[k][1]);
        }
        cx_restore(ctx);
    }
}

static void grid(void) {
    CxCtx *ctx = F.ctx;
    double W = F.W, H = F.H, t = F.t;
    double hy = H * 0.8;
    cx_save(ctx), cx_begin(ctx), cx_rect(ctx, 0, hy, W, H - hy), cx_clip(ctx);
    cx_fill_color(ctx, rgba(scalec(F.P.bg, 0.6), 0.9)), cx_fill_rect(ctx, 0, hy, W, H - hy);
    cx_stroke_color(ctx, rgba(F.P.a, 0.6)), cx_line_width(ctx, 1.5);
    double sp = fmod(t * 0.6, 1);
    for (int i = 0; i < 18; i++) {
        double k = (i + sp) / 18, y = hy + (H - hy) * k * k;
        cx_alpha(ctx, k), cx_begin(ctx), cx_move(ctx, 0, y), cx_line(ctx, W, y), cx_stroke(ctx);
    }
    cx_alpha(ctx, 1);
    double vx = W / 2 - F.par * 0.5;
    for (int i = -20; i <= 20; i++) { cx_begin(ctx), cx_move(ctx, vx + i * 12, hy), cx_line(ctx, vx + i * W * 0.12, H), cx_stroke(ctx); }
    cx_restore(ctx);
}

static void holo_boards(void);
static void city_draw(CxEngine *e) {
    char key[512];
    city_key(key, sizeof key);
    if (strcmp(key, City.key)) city_build(e);
    double H = F.H;
    Pal P = F.P;
    sky();
    sky_object();
    if (CB(searchlights)) searchlights();
    if (!CB(city)) { if (C(weather)) rain(); return; }
    Rgb fogC = mixc(P.bg2, P.a, 0.15);
    double fogK = C(fog) / 100;
    draw_layer(&City.layers[0]);
    traffic(0, 0.35);
    fog(City.layers[0].base - H * 0.05, fogK * 0.9, fogC);
    draw_layer(&City.layers[1]);
    traffic(0.35, 0.6);
    fog(City.layers[1].base - H * 0.04, fogK * 0.7, fogC);
    holo_boards();
    draw_layer(&City.layers[2]);
    traffic(0.6, 0.85);
    fog(City.layers[2].base, fogK * 0.5, fogC);
    if (CB(lightning) && (chance(F.dt * 0.04 * (0.3 + C(chaos) / 50)) || (A.kick && A.bass > 0.7 && chance(0.25)))) city_strike();
    bolts();
    draw_layer(&City.layers[3]);
    traffic(0.85, 1.01);
    if (CB(grid)) grid();
    fog(H * 0.98, fogK * 0.6, fogC);
    if (C(weather)) rain(), splashes();
}
#endif
