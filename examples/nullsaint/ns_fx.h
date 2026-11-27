/* ns_fx.h: fx.js (particles, shockwaves, cursor trail, reality tears, worms, confetti). */
#ifndef NS_FX_H
#define NS_FX_H
#include "ns_state.h"

typedef struct Part { double x, y, vx, vy, life, t, s, g, drag; Rgb c; uint16_t ch; int kind; } Part;
typedef struct Shock { double x, y, t, s; } Shock;
typedef struct TearPt { double x, y, w; } TearPt;
typedef struct RTear { TearPt pts[23]; double t, life, w; } RTear;
typedef struct Worm { double seg[91][2]; int nseg; double a, t, life, size, sp; } Worm;
typedef struct Block { double x, y, w, h, t, life; Rgb c; } Block;
typedef struct Dead { double x, y, s; Rgb c; } Dead;
typedef struct Ring { double x, y, t, p; } Ring;

#define MAX_PARTS 900
static struct {
    Part parts[MAX_PARTS];
    int nparts;
    Shock shocks[7];
    int nshocks;
    RTear tears[32];
    int ntears;
    Worm worms[16];
    int nworms;
    Block blocks[256];
    int nblocks;
    Dead dead[400];
    int ndead;
    Ring rings[64];
    int nrings;
    Set hexbox;
} X;

static void fx_init(void) { X.hexbox.n = 0; set_cat(&X.hexbox, &G_HEX); set_cat(&X.hexbox, &G_BOX); }
static Part part_new(void) { Part p; memset(&p, 0, sizeof p); p.life = 1, p.s = 2, p.c = WHITE, p.drag = 1; return p; }
static void fx_particle(const Part *p) { if (X.nparts < MAX_PARTS) X.parts[X.nparts++] = *p; }
static void fx_shock(double x, double y, double s) {
    Shock *k = &X.shocks[X.nshocks++];
    k->x = x, k->y = y, k->t = 0, k->s = s;
    if (X.nshocks > 6) memmove(X.shocks, X.shocks + 1, 6 * sizeof *X.shocks), X.nshocks = 6;
}
static void fx_ring(double x, double y, double p) {
    if (X.nrings >= 64) return;
    Ring *r = &X.rings[X.nrings++];
    r->x = x, r->y = y, r->t = 0, r->p = p;
}
static Rgb pick5(void) { Pal P = F.P; Rgb o[5] = { P.a, P.b, P.c, P.warn, P.text }; return o[pick_i(5)]; }
static void fx_burst(double x, double y, double power) {
    int n = (int)floor(60 + 140 * power + 0.5);
    for (int i = 0; i < n; i++) {
        double a = urand(0, TAU);
        double sp = urand(80, 900) * power * F.k;
        Part p = part_new();
        p.x = x, p.y = y, p.vx = cos(a) * sp, p.vy = sin(a) * sp;
        p.life = urand(0.6, 2);
        p.s = urand(2, 9) * F.k;
        p.g = 300 * F.k, p.drag = 0.8;
        p.c = pick5();
        p.ch = chance(0.45) ? X.hexbox.c[pick_i(X.hexbox.n)] : 0;
        p.kind = chance(0.2) ? 1 : 0;
        fx_particle(&p);
    }
}
static void fx_tear(double x, double y, double power) {
    if (X.ntears >= 32) return;
    RTear *tr = &X.tears[X.ntears];
    double len = (200 + 500 * power) * F.k;
    double a = urand(0, TAU);
    double px = x - cos(a) * len / 2, py = y - sin(a) * len / 2;
    const int n = 22;
    for (int i = 0; i <= n; i++) {
        tr->pts[i].x = px, tr->pts[i].y = py, tr->pts[i].w = urand(0.3, 1);
        a += urand(-0.6, 0.6) * 0.5;
        px += cos(a) * len / n, py += sin(a) * len / n;
    }
    tr->t = 0, tr->life = 3 + power * 3, tr->w = (6 + 20 * power) * F.k;
    X.ntears++;
}
static void fx_worm(void) {
    if (X.nworms >= 16) return;
    double W = F.W, H = F.H;
    int side = randi(0, 3);
    double x = side == 0 ? 0 : side == 1 ? W : urand(0, W);
    double y = side == 2 ? 0 : side == 3 ? H : urand(0, H);
    Worm *w = &X.worms[X.nworms];
    w->seg[0][0] = x, w->seg[0][1] = y, w->nseg = 1;
    w->a = atan2(H / 2 - y, W / 2 - x), w->t = 0;
    w->life = urand(5, 9);
    w->size = urand(10, 22) * F.k;
    w->sp = urand(180, 340) * F.k;
    X.nworms++;
}
static void fx_click(double x, double y) {
    double power = 0.3 + (C(clickPower) / 100) * 1.0;
    int mode = (int)C(click);
    if (mode == 0) return;
    if (mode == 4) mode = randi(1, 3);
    bool all = mode == 5;
    if (all || mode == 1) { fx_shock(x, y, power); fx_ring(x, y, power); }
    if (all || mode == 2) fx_burst(x, y, power);
    if (all || mode == 3) { fx_tear(x, y, power); fx_shock(x, y, power * 0.5); }
    glyphs_burst(x, y, power);
    if (!CB(safe)) flash(0.15 * power, F.P.text);
    director_kick(0.6 * power);
}

static void fx_draw(void) {
    CxCtx *ctx = F.ctx;
    double dt = F.dt, W = F.W, H = F.H, t = F.t;
    Pal P = F.P;
    if (chance(dt * 4 * F.gi * (0.3 + C(chaos) / 100)) && X.nblocks < 256) {
        Block *b = &X.blocks[X.nblocks];
        b->x = urand(0, W);
        b->y = urand(0, H);
        b->w = urand(10, 220) * F.k;
        b->h = urand(2, 30) * F.k;
        b->t = 0;
        b->life = urand(0.05, 0.4);
        Rgb o[6] = { P.a, P.b, P.c, P.warn, WHITE, BLACK };
        b->c = o[pick_i(6)];
        X.nblocks++;
    }
    for (int i = X.nblocks - 1; i >= 0; i--) {
        Block *b = &X.blocks[i];
        b->t += dt;
        if (b->t > b->life) { memmove(b, b + 1, (size_t)(X.nblocks - i - 1) * sizeof *b), X.nblocks--; continue; }
        cx_fill_color(ctx, rgba(b->c, 0.85)), cx_fill_rect(ctx, b->x, b->y, b->w, b->h);
    }
    if (F.rot > 0.2 && X.ndead < 400 && chance(dt * 20 * F.rot)) {
        Dead *d = &X.dead[X.ndead];
        d->x = urand(0, W);
        d->y = urand(0, H);
        Rgb o[4] = { P.a, P.b, P.c, WHITE };
        d->c = o[pick_i(4)];
        d->s = randi(1, 4) * F.k;
        X.ndead++;
    }
    if (F.rot < 0.05 && X.ndead) X.ndead = X.ndead - 20 > 0 ? X.ndead - 20 : 0;
    for (int i = 0; i < X.ndead; i++) { Dead *d = &X.dead[i]; cx_fill_color(ctx, rgba(d->c, 1)), cx_fill_rect(ctx, d->x, d->y, d->s, d->s); }

    double tr = C(trail) / 100;
    if (tr > 0 && I.speed > 200 * F.k && I.inside) {
        int n = (int)fmin(6, floor(I.speed / (400 * F.k)) + 1);
        for (int i = 0; i < n; i++) {
            Part p = part_new();
            p.x = F.mx + urand(-8, 8) * F.k;
            p.y = F.my + urand(-8, 8) * F.k;
            p.vx = -I.vx * 0.05 * F.k + urand(-30, 30);
            p.vy = -I.vy * 0.05 * F.k + urand(-30, 30);
            p.life = urand(0.3, 0.9) * (0.4 + tr);
            p.s = urand(2, 7) * F.k;
            Rgb o[3] = { P.a, P.b, P.c };
            p.c = o[pick_i(3)];
            p.drag = 0.5, p.kind = 2;
            fx_particle(&p);
        }
    }

    cx_save(ctx), cx_op(ctx, CX_LIGHTER);
    cx_text_align(ctx, CX_CENTER), cx_text_baseline(ctx, CX_MIDDLE);
    for (int i = X.nparts - 1; i >= 0; i--) {
        Part *p = &X.parts[i];
        p->t += dt;
        if (p->t > p->life) { memmove(p, p + 1, (size_t)(X.nparts - i - 1) * sizeof *p), X.nparts--; continue; }
        p->vx *= pow(1 - p->drag * 0.9, dt * 2), p->vy *= pow(1 - p->drag * 0.9, dt * 2);
        p->vy += p->g * dt;
        p->x += p->vx * dt, p->y += p->vy * dt;
        double a = 1 - p->t / p->life;
        if (p->ch) {
            ufont(ctx, p->s * 2.4, F_MONO_B), cx_fill_color(ctx, rgba(p->c, a));
            Str s;
            s.n = 1, s.c[0] = p->ch;
            fill_str(ctx, &s, p->x, p->y);
        } else if (p->kind == 1) {
            cx_stroke_color(ctx, rgba(p->c, a)), cx_line_width(ctx, 1);
            cx_begin(ctx), cx_move(ctx, p->x, p->y), cx_line(ctx, p->x - p->vx * 0.05, p->y - p->vy * 0.05), cx_stroke(ctx);
        } else {
            cx_fill_color(ctx, rgba(p->c, a)), cx_fill_rect(ctx, p->x - p->s / 2, p->y - p->s / 2, p->s, p->s);
        }
    }
    cx_restore(ctx);

    for (int i = X.nrings - 1; i >= 0; i--) {
        Ring *r = &X.rings[i];
        r->t += dt;
        if (r->t > 1.2) { memmove(r, r + 1, (size_t)(X.nrings - i - 1) * sizeof *r), X.nrings--; continue; }
        double rad = ease_out_cubic(r->t / 1.2) * 400 * F.k * (0.5 + r->p);
        cx_stroke_color(ctx, rgba(P.b, (1 - r->t / 1.2) * 0.9)), cx_line_width(ctx, 2 * F.k);
        cx_begin(ctx);
        for (int k = 0; k <= 6; k++) {
            double a = (k / 6.0) * TAU + r->t;
            double px = r->x + cos(a) * rad, py = r->y + sin(a) * rad;
            k ? cx_line(ctx, px, py) : cx_move(ctx, px, py);
        }
        cx_stroke(ctx);
        cx_stroke_color(ctx, rgba(P.a, (1 - r->t / 1.2) * 0.6));
        cx_begin(ctx), cx_arc(ctx, r->x, r->y, rad * 0.7, 0, TAU, false), cx_stroke(ctx);
        ufont(ctx, 12 * F.k, F_MONO), cx_fill_color(ctx, rgba(P.text, 1 - r->t / 1.2));
        Str s = s_of("IMPACT "), h = uhex(js_i32(r->x * 31 + r->y), 4);
        s_cats(&s, &h);
        fill_str(ctx, &s, r->x + rad * 0.72, r->y);
    }

    for (int i = X.ntears - 1; i >= 0; i--) {
        RTear *tr2 = &X.tears[i];
        tr2->t += dt;
        if (tr2->t > tr2->life) { memmove(tr2, tr2 + 1, (size_t)(X.ntears - i - 1) * sizeof *tr2), X.ntears--; continue; }
        double open = fmin(1, tr2->t * 6) * (1 - smoothd(tr2->life * 0.6, tr2->life, tr2->t));
        const TearPt *pts = tr2->pts;
        const int np = 23;
        cx_begin(ctx);
        for (int k = 0; k < np; k++) {
            double w = tr2->w * open * pts[k].w * sin(((double)k / (np - 1)) * PI);
            k ? cx_line(ctx, pts[k].x, pts[k].y - w) : cx_move(ctx, pts[k].x, pts[k].y);
        }
        for (int k = np - 1; k >= 0; k--) {
            double w = tr2->w * open * pts[k].w * sin(((double)k / (np - 1)) * PI);
            cx_line(ctx, pts[k].x, pts[k].y + w);
        }
        cx_close(ctx);
        cx_fill_color(ctx, cx_rgba(0, 0, 0, 1)), cx_fill(ctx);
        cx_save(ctx), cx_clip(ctx);
        for (int k = 0; k < 40; k++) {
            Rgb o[3] = { P.a, P.b, P.text };
            Rgb c = o[pick_i(3)];
            double al = urand(0.2, 0.8);
            cx_fill_color(ctx, rgba(c, al));
            const TearPt *p = &pts[pick_i(np)];
            double rx = urand(-30, 30);
            double ry = urand(-tr2->w, tr2->w);
            double rw = urand(2, 20);
            cx_fill_rect(ctx, p->x + rx, p->y + ry, rw, 2);
        }
        cx_restore(ctx);
        cx_save(ctx), cx_op(ctx, CX_LIGHTER);
        cx_stroke_color(ctx, rgba(P.c, 0.9 * open)), cx_line_width(ctx, 2 * F.k);
        cx_stroke(ctx);
        cx_restore(ctx);
        if (chance(dt * 30 * open)) {
            const TearPt *p = &pts[pick_i(np)];
            Part q = part_new();
            q.x = p->x, q.y = p->y;
            q.vx = urand(-60, 60);
            q.vy = urand(-60, 60);
            q.life = 0.8, q.s = 3 * F.k, q.c = P.c, q.drag = 0.4;
            fx_particle(&q);
        }
    }

    for (int i = X.nworms - 1; i >= 0; i--) {
        Worm *w = &X.worms[i];
        w->t += dt;
        if (w->t > w->life) { memmove(w, w + 1, (size_t)(X.nworms - i - 1) * sizeof *w), X.nworms--; continue; }
        double hx = w->seg[w->nseg - 1][0], hy = w->seg[w->nseg - 1][1];
        double ta = atan2(F.my - hy, F.mx - hx);
        double da = ta - w->a;
        while (da > PI) da -= TAU;
        while (da < -PI) da += TAU;
        w->a += da * dt * 0.8 + sin(t * 5 + i) * dt * 2;
        double nx = hx + cos(w->a) * w->sp * dt, ny = hy + sin(w->a) * w->sp * dt;
        if (dist(nx, ny, hx, hy) > 0) w->seg[w->nseg][0] = nx, w->seg[w->nseg][1] = ny, w->nseg++;
        if (w->nseg > 90) memmove(w->seg, w->seg + 1, (size_t)(w->nseg - 1) * sizeof w->seg[0]), w->nseg--;
        for (int k = 0; k < w->nseg; k += 2) {
            double kk = (double)k / w->nseg;
            double sz = w->size * (0.3 + kk);
            cx_fill_color(ctx, rgba(kk > 0.95 ? P.warn : mixc(P.a, P.c, fmod(k * 0.13, 1)), 0.85 * kk));
            cx_fill_rect(ctx, floor(w->seg[k][0] / sz + 0.5) * sz, floor(w->seg[k][1] / sz + 0.5) * sz, sz, sz);
        }
        ufont(ctx, 12 * F.k, F_MONO_B), cx_fill_color(ctx, rgba(P.warn, 1));
        Str eat = s_of("EAT"), ce = corrupt(&eat, 0.3);
        fill_str(ctx, &ce, nx + 14 * F.k, ny - 14 * F.k);
    }

    for (int i = X.nshocks - 1; i >= 0; i--) {
        X.shocks[i].t += dt;
        if (X.shocks[i].t > 1.6) memmove(&X.shocks[i], &X.shocks[i + 1], (size_t)(X.nshocks - i - 1) * sizeof X.shocks[0]), X.nshocks--;
    }
}
static void fx_shock_uniform(float out[24]) {
    memset(out, 0, 24 * sizeof *out);
    for (int i = 0; i < X.nshocks; i++) {
        out[i * 4] = (float)(X.shocks[i].x / F.W), out[i * 4 + 1] = (float)(1 - X.shocks[i].y / F.H);
        out[i * 4 + 2] = (float)X.shocks[i].t, out[i * 4 + 3] = (float)X.shocks[i].s;
    }
}
#endif
