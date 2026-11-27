/* medusae.c: a soft-body jellyfish breathing in a drift of dust; Screen+'s
 * "Medusae" wallpaper.
 *
 * A port of the page (Jay Weeks's Particulate Medusae, see LICENSE.txt): the
 * same particle system (Particulate.js: Verlet steps at 30 Hz, two passes of
 * distance, axis and pin constraints, gravity and a point repulsor), worked
 * as the page works it, in doubles stored to 32-bit floats, so it moves as the
 * page's does; drawn as its three.js scene was: tentacle and contour lines,
 * the tail, mouth, gel and bulb skins (their shaders), the dust, then bloom,
 * lens dirt and a vignette, at 1.5 times the window's pixels and scaled down,
 * as the page's canvas was. Drag to turn the camera, wheel to zoom, click the
 * bulb to nudge it.
 *
 * A Screen+ app wallpaper (see ../screenplus/screenplus_wallpaper.h); it has no
 * settings. While Screen+ has it paused it draws nothing. */
#include "bare.h"
#include "medusae_shaders.h"
#include "../screenplus/screenplus_wallpaper.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.141592653589793

#define VEC(T) struct { T *v; int n, cap; }
#define PUSH(a, x) do { if ((a).n == (a).cap) { (a).cap = (a).cap ? (a).cap * 2 : 64; \
    (a).v = realloc((a).v, sizeof *(a).v * (size_t)(a).cap); if (!(a).v) abort(); } (a).v[(a).n++] = (x); } while (0)

typedef VEC(int) Ints;
typedef VEC(double) Doubles;

/* ---- the particle system (Particulate.js 0.3.2) ------------------------------------------ */

enum { C_DIST, C_AXIS, C_PIN };
typedef struct Con {
    int kind, n;          /* DIST: n pairs; AXIS: n points after the axis's two; PIN: 1 */
    uint16_t *idx;
    double min2, max2;    /* DIST */
    float pos[3];         /* PIN */
} Con;

static VEC(Con) cons;     /* every constraint made */
static Ints queue;        /* local constraints, in the order they're satisfied */
static Con pins[5];

static float *pos, *prev, *acc, *wts;
static int count;
static float gravity[3] = { 0, -2, 0 };
static float rep_pos[3] = { 20, 5, 0 };
static double rep_r2 = 50.0 * 50.0, rep_intensity = 0;

static int con_dist(double min, double max, const int *list, int n) {
    Con c = { C_DIST, n / 2, malloc(sizeof(uint16_t) * (size_t)n), min * min, max * max, { 0 } };
    for (int i = 0; i < n; i++) c.idx[i] = (uint16_t)list[i];
    PUSH(cons, c);
    return cons.n - 1;
}
static void set_distance(int k, double min, double max) { cons.v[k].min2 = min * min, cons.v[k].max2 = max * max; }

static void apply_dist(const Con *c) {
    float *b = pos;
    for (int j = 0; j < c->n; j++) {
        int g = 3 * c->idx[2 * j], jj = 3 * c->idx[2 * j + 1];
        double m = (double)b[jj] - b[g], n = (double)b[jj + 1] - b[g + 1], o = (double)b[jj + 2] - b[g + 2];
        if (!m && !n && !o) m = n = o = .1;
        double p = m * m + n * n + o * o, q = c->min2, r = c->max2;
        if (!(r > p && p > q)) {
            double s = q > p ? q : r, t = s / (p + s), u = t - .5;
            b[g] = (float)(b[g] - m * u), b[g + 1] = (float)(b[g + 1] - n * u), b[g + 2] = (float)(b[g + 2] - o * u);
            b[jj] = (float)(b[jj] + m * u), b[jj + 1] = (float)(b[jj + 1] + n * u), b[jj + 2] = (float)(b[jj + 2] + o * u);
        }
    }
}

static void apply_axis(const Con *c) {
    float *b = pos;
    int h = 3 * c->idx[0], n = 3 * c->idx[1];
    for (int j = 0; j < c->n; j++) {
        int k = 3 * c->idx[2 + j];
        double q = (double)b[k] - b[h], r = (double)b[k + 1] - b[h + 1], s = (double)b[k + 2] - b[h + 2];
        double t = (double)b[n] - b[h], u = (double)b[n + 1] - b[h + 1], v = (double)b[n + 2] - b[h + 2];
        double w = t * t + u * u + v * v, x = sqrt(w), y = 1 / x, z = t * y, A = u * y, B = v * y;
        double C = z * q + A * r + B * s;
        b[k] = (float)(b[h] + z * C), b[k + 1] = (float)(b[h + 1] + A * C), b[k + 2] = (float)(b[h + 2] + B * C);
    }
}

static void apply(const Con *c) {
    if (c->kind == C_DIST) apply_dist(c);
    else if (c->kind == C_AXIS) apply_axis(c);
    else {
        int f = 3 * c->idx[0];
        for (int i = 0; i < 3; i++) pos[f + i] = prev[f + i] = c->pos[i];
    }
}

static double clampd(double lo, double hi, double v) { return fmin(fmax(v, lo), hi); }

static void tick(double dt) {
    double d2 = dt * dt;
    for (int i = 0; i < count; i++) {   /* accumulateForces: gravity, then the repulsor */
        int b = 3 * i;
        for (int k = 0; k < 3; k++) acc[b + k] = (float)(0.0 + gravity[k]);
        double x = (double)pos[b] - rep_pos[0], y = (double)pos[b + 1] - rep_pos[1], z = (double)pos[b + 2] - rep_pos[2];
        double l = x * x + y * y + z * z, m = clampd(.001, 100, l - rep_r2 * rep_intensity), n = 1 / m;
        double o = clampd(0, 10, n * n * n);
        acc[b] = (float)(acc[b] + x * o), acc[b + 1] = (float)(acc[b + 1] + y * o), acc[b + 2] = (float)(acc[b + 2] + z * o);
    }
    for (int i = 0; i < count; i++)   /* integrate */
        for (int k = 0; k < 3; k++) {
            int a = 3 * i + k;
            double g = pos[a];
            pos[a] = (float)(g + ((g - prev[a]) + (double)acc[a] * wts[i] * d2));
            prev[a] = (float)g;
        }
    for (int it = 0; it < 2; it++) {
        for (int q = 0; q < queue.n; q++) apply(&cons.v[queue.v[q]]);
        for (int p = 0; p < 5; p++) apply(&pins[p]);
    }
}

/* ---- the jellyfish (App.Medusae) ------------------------------------------------------- */

#define SIZE 40.0
#define YOFF 20.0
#define SEGS 36          /* totalSegments: 3 * 4 * 3 */
#define RIBS 20
#define TAIL_RIBS 15
#define ARM_SEGS 100

typedef struct Rib { int start; double radius, radius_spine, radius_outer, yparam, ypos; int outer, inner, spine; } Rib;

static Doubles verts, uvs, weights;
static Ints links, tent_links, inner_links, bulb_faces, tail_faces, mouth_faces;
static Rib ribs[RIBS], tail_ribs[TAIL_RIBS];
static VEC(int) tentacles[3];
static double anim_time;

static int nverts(void) { return verts.n / 3; }
static void point(double x, double y, double z) { PUSH(verts, x); PUSH(verts, y); PUSH(verts, z); }
static void circle(int a, double b, double c) {
    double g = 2 * PI / a, h = 0;
    for (int i = 0; i < a; i++) { point(cos(h) * b, c, sin(h) * b); h += g; }
}
static void rib_uvs(double a, int b) {
    for (int f = 1; f < b; f++) {
        double d = (double)f / b, e = 2 * (d <= .5 ? d : 1 - d);
        PUSH(uvs, e); PUSH(uvs, a);
    }
    PUSH(uvs, 0.0); PUSH(uvs, a);
}
static double vdist(int a, int b) {
    double x = verts.v[3 * a] - verts.v[3 * b], y = verts.v[3 * a + 1] - verts.v[3 * b + 1], z = verts.v[3 * a + 2] - verts.v[3 * b + 2];
    return sqrt(x * x + y * y + z * z);
}
static void l_line(Ints *c, int a, int b) { for (int f = 0; f < b - 1; f++) { PUSH(*c, a + f); PUSH(*c, a + f + 1); } }
static void l_loop(Ints *c, int a, int b) { l_line(c, a, b); PUSH(*c, a); PUSH(*c, a + b - 1); }
static void l_rings(Ints *d, int a, int b, int c) { for (int g = 0; g < c; g++) { PUSH(*d, a + g); PUSH(*d, b + g); } }
static void l_radial(Ints *d, int a, int b, int c) { for (int f = 0; f < c; f++) { PUSH(*d, a); PUSH(*d, b + f); } }
static void f_radial(Ints *d, int a, int b, int c) {
    for (int g = 0; g < c - 1; g++) { PUSH(*d, a); PUSH(*d, b + g + 1); PUSH(*d, b + g); }
    PUSH(*d, a); PUSH(*d, b); PUSH(*d, b + c - 1);
}
static void f_quad6(Ints *d, int e, int f, int g, int h) { int q[6] = { e, f, g, g, h, e }; for (int i = 0; i < 6; i++) PUSH(*d, q[i]); }
static void f_rings(Ints *d, int a, int b, int c) {
    for (int i = 0; i < c - 1; i++) f_quad6(d, a + i, a + i + 1, b + i + 1, b + i);
    f_quad6(d, a + c - 1, a, b, b + c - 1);
}
static void f_quad2(Ints *e, int a, int b, int c, int d) {
    int q[12] = { a, b, c, c, d, a, d, c, b, b, a, d };
    for (int i = 0; i < 12; i++) PUSH(*e, q[i]);
}
static void add_links(Ints *to, int k) { const Con *c = &cons.v[k]; for (int i = 0; i < 2 * c->n; i++) PUSH(*to, c->idx[i]); }
static void add_list(Ints *to, const Ints *l) { for (int i = 0; i < l->n; i++) PUSH(*to, l->v[i]); }
static void enqueue(int k) { PUSH(queue, k); }
static int dist_list(double min, double max, Ints *l) { int k = con_dist(min, max, l->v, l->n); free(l->v); *l = (Ints){ 0 }; return k; }
static void queue_weights(int a, int b, double c) {
    int g = a + b;
    while (weights.n < g) PUSH(weights, 1.0);
    for (int d = a; d < g; d++) weights.v[d] = c;
}

static int inner_rib(int a, double b) {
    Ints f = { 0 };
    for (int g = 0; g < 4; g++) {
        int A = 3 * g, G = SEGS / 3;
        for (int h = 0; h < 3; h++) { int e = A + G * h, ff = A + G * (h + 1); PUSH(f, a + e % SEGS); PUSH(f, a + ff % SEGS); }
    }
    return dist_list(.8 * b, b, &f);
}

static double rib_fn(double a) { return sin(PI - .55 * PI * a * 1.8) + log(100 * a + 2) / 3; }
static double tail_fn(double a) { return sin(.25 * a * PI + .5 * PI) * (1 - .9 * a); }

static void create_core(void) {
    double E[8] = { YOFF + SIZE, YOFF, YOFF - SIZE, YOFF - ARM_SEGS * 1.0, YOFF - 120 * 1.5 * 1.5, 1.5 * SIZE, .5 * -SIZE, -SIZE };
    for (int i = 0; i < 8; i++) { point(0, E[i], 0); PUSH(uvs, 0.0); PUSH(uvs, 0.0); }
    enqueue(con_dist(0, .5 * SIZE, (int[]){ 0, 5 }, 2));
    enqueue(con_dist(.5 * SIZE, .7 * SIZE, (int[]){ 5, 6 }, 2));
    enqueue(con_dist(0, .5 * SIZE, (int[]){ 2, 7 }, 2));
    enqueue(con_dist(SIZE, 2 * SIZE, (int[]){ 5, 7 }, 2));
    Con x = { C_AXIS, 3, malloc(sizeof(uint16_t) * 5), 0, 0, { 0 } };
    uint16_t xi[5] = { 0, 1, 5, 6, 7 };
    memcpy(x.idx, xi, sizeof xi);
    PUSH(cons, x);
    enqueue(cons.n - 1);
    f_radial(&bulb_faces, 5, 8, SEGS);
}

static void create_rib(int a, int c) {
    double i = (double)a / c, l = SIZE + YOFF - i * SIZE, o = rib_fn(i) * 15;
    int m = a * SEGS + 8;
    circle(SEGS, o, l);
    rib_uvs(i, SEGS);
    Ints t = { 0 };
    l_loop(&t, m, SEGS);
    double u = 2 * PI * o / SEGS;
    int v = dist_list(.9 * u, u, &t);
    double x = 2 * PI * o / 3;
    int y = inner_rib(m, x);
    int q = -1;
    double s = 0;
    bool z = a == 0, A = a == c - 1;
    if (z || A) {
        int r = z ? 5 : 7;
        s = z ? 1.25 * o : o;
        Ints rl = { 0 };
        l_radial(&rl, r, m, SEGS);
        q = dist_list(.5 * o, s, &rl);
        enqueue(q);
        add_links(z ? &links : &inner_links, q);
    }
    add_links(&inner_links, v);
    add_links(&inner_links, y);
    enqueue(v), enqueue(y);
    ribs[a] = (Rib){ m, o, s, 0, i, l, v, y, q };
}

static void create_skin(int a, int b) {
    double f = vdist(ribs[a].start, ribs[b].start);
    Ints r = { 0 };
    l_rings(&r, ribs[a].start, ribs[b].start, SEGS);
    int g = dist_list(.5 * f, f, &r);
    enqueue(g);
    add_links(&links, g);
    f_rings(&bulb_faces, ribs[a].start, ribs[b].start, SEGS);
}

static void create_tail_rib(int a, int b) {
    const Rib *c = &ribs[RIBS - 1];
    double l = (double)a / b, m = c->ypos - l * SIZE * .8, o = tail_fn(l), q = o * c->radius, r = q + l * 20;
    int n = nverts();
    circle(SEGS, q, m);
    rib_uvs(l, SEGS);
    Ints u = { 0 };
    l_loop(&u, n, SEGS);
    double v = 2 * PI * r / SEGS;
    int x = dist_list(.9 * v, 1.5 * v, &u);
    double y = 2 * PI * q / 3;
    int z = inner_rib(n, y), s = -1;
    if (a == b - 1) {
        Ints rl = { 0 };
        l_radial(&rl, 6, n, SEGS);
        s = dist_list(.8 * q, q, &rl);
        enqueue(s);
        add_links(&inner_links, s);
    }
    enqueue(x), enqueue(z);
    if (a > 2) add_links(&links, x);
    tail_ribs[a] = (Rib){ n, q, 0, r, 1 - l, m, x, z, s };
}

static void create_tail_skin(int a, int b) {
    const Rib *d = a < 0 ? &ribs[RIBS - 1] : &tail_ribs[a], *e = &tail_ribs[b];
    double f = vdist(d->start, e->start);
    Ints r = { 0 };
    l_rings(&r, d->start, e->start, SEGS);
    int g = dist_list(.5 * f, f, &r);
    enqueue(g);
    add_links(&inner_links, g);
    f_rings(&tail_faces, d->start, e->start, SEGS);
}

static const Rib *rib_at(int a) {
    int t = TAIL_RIBS - a - 1;
    return t >= 0 ? &tail_ribs[t] : &ribs[RIBS - a + TAIL_RIBS - 1];
}

static void create_mouth_arm(double a, int b, int c, int d, int e, int f) {
    double v = (double)d / e, A = YOFF, K = 1, L = 2.4 * K;
    const Rib *B = rib_at(b), *C = rib_at(c);
    int E = ((int)floor(SEGS * v + .5) + f) % SEGS, F = B->start + E, G = C->start + E;
    double H = vdist(F, G);
    int I = ARM_SEGS, J = (int)floor(a * I + .5);
    double M = 20 + (I - J) * K;
    int N = nverts(), O = N + J - 1, P = N + J;
    Ints Q = { 0 }, R = { 0 }, T = { 0 }, U = { 0 };
    PUSH(Q, F); PUSH(Q, N); l_line(&Q, N, J);
    PUSH(R, G); PUSH(R, P); l_line(&R, P, J);
    double V = 2 * PI * v, W = cos(V), X = sin(V), x = .5 * PI, q = 0;
    for (int Y = 0; Y < J; Y++) { double u = (double)Y / (J - 1); point(0, A - Y * K, 0); PUSH(uvs, u); PUSH(uvs, 0.0); }
    int *S = malloc(sizeof(int) * (size_t)J);
    for (int Y = 0; Y < J; Y++) {
        double u = (double)Y / (J - 1);
        int m = N + Y, o = P + Y;
        q = H * (.25 * sin(x + 10 * u) + .75) * (.25 * sin(x + 20 * u) + .75) * (.15 * sin(x + 26 * u) + .85) * sin(x + .45 * PI * u);
        point(W * q, A - Y * K, X * q);
        PUSH(uvs, u); PUSH(uvs, 1.0);
        S[Y] = con_dist(q, q, (int[]){ m, o }, 2);
        if (Y > 10) { PUSH(T, m - 10); PUSH(T, o); }
        if (Y > 1) { PUSH(U, m - 1); PUSH(U, o); }
        if (Y > 1) f_quad2(&mouth_faces, m - 1, o - 1, o, m);
    }
    int Z = con_dist(.25 * K, K, Q.v, Q.n), D = con_dist(.25 * L, L, R.v, R.n);
    int _ = con_dist(.5 * q, INFINITY, T.v, T.n), aa = con_dist(0, M, (int[]){ O, 3 }, 2);
    enqueue(Z), enqueue(D), enqueue(_), enqueue(aa);
    for (int Y = 0; Y < J; Y++) enqueue(S[Y]);
    free(S);
    queue_weights(N, 2 * J, .5);
    add_list(&links, &Q); add_list(&links, &R);
    add_list(&tent_links, &U); add_list(&tent_links, &T);
    add_list(&inner_links, &Q); add_list(&inner_links, &R); add_list(&inner_links, &U); add_list(&inner_links, &T);
    add_links(&inner_links, aa);
    free(Q.v), free(R.v), free(T.v), free(U.v);
}

static void create_tentacle_group(int a, int b) {
    const Rib *d = rib_at(6 + 4 * a);
    double e = 1 - (double)a / b, f = 120, g = f * e * .25 + .75 * f;
    for (int h = 0; h < g; h++) {
        double k = d->radius * (.25 * sin(.25 * h) + .5), l = -h * 1.5 + YOFF, r = h / g;
        int m = nverts();
        circle(SEGS, k, l);
        for (int i = 0; i < SEGS; i++) { PUSH(uvs, 0.0); PUSH(uvs, 0.0); }
        queue_weights(m, SEGS, r * r * r * 1.25);
        PUSH(tentacles[a], m);
        Ints rl = { 0 };
        if (h > 0) {
            l_rings(&rl, tentacles[a].v[h - 1], tentacles[a].v[h], SEGS);
            int i = dist_list(.5 * 1.5, 1.5, &rl);
            enqueue(i);
            add_links(&tent_links, i);
            add_links(&inner_links, i);
        } else {
            l_rings(&rl, d->start, m, SEGS);
            int fi = dist_list(.5 * 1.5, 1.5, &rl);
            enqueue(fi);
            add_links(&tent_links, fi);
        }
    }
    Ints rl = { 0 };
    l_radial(&rl, 4, tentacles[a].v[tentacles[a].n - 1], SEGS);
    enqueue(dist_list(.5 * 180, 180, &rl));
}

static void update_ribs(Rib *r, int n, double b) {
    for (int k = 0; k < n; k++) {
        Rib *c = &r[k];
        double d = c->radius + c->yparam * b * 15;
        double e = (c->radius_outer ? c->radius_outer : c->radius) + c->yparam * b * 15;
        double f = (c->radius_spine ? c->radius_spine : c->radius) + c->yparam * b * 15;
        double g = 2 * PI * e / SEGS, h = 2 * PI * d / 3;
        set_distance(c->outer, .9 * g, g);
        set_distance(c->inner, .8 * h, h);
        if (c->spine >= 0) set_distance(c->spine, .8 * d, f);
    }
}

static void create_medusae(void) {
    create_core();
    for (int b = 0; b < RIBS; b++) { create_rib(b, RIBS); if (b > 0) create_skin(b - 1, b); }
    for (int b = 0; b < TAIL_RIBS; b++) { create_tail_rib(b, TAIL_RIBS); create_tail_skin(b - 1, b); }
    for (int f = 0; f < 3; f++) create_mouth_arm(1, 0, 4, f, 3, 0);
    for (int f = 0; f < 3; f++) create_mouth_arm(.8, 1, 8, f, 3, 3);
    for (int f = 0; f < 6; f++) create_mouth_arm(.5, 7, 9, f, 6, 0);
    for (int a = 0; a < 3; a++) create_tentacle_group(a, 3);

    count = nverts();
    pos = malloc(sizeof(float) * 3 * (size_t)count), prev = malloc(sizeof(float) * 3 * (size_t)count);
    acc = calloc(3 * (size_t)count, sizeof(float)), wts = malloc(sizeof(float) * (size_t)count);
    for (int i = 0; i < 3 * count; i++) pos[i] = prev[i] = (float)verts.v[i];
    for (int i = 0; i < count; i++) wts[i] = i < weights.n ? (float)weights.v[i] : 1;
    for (int i = 0; i < 4; i++) wts[i] = 0;
    double pin_y[5] = { YOFF + SIZE, YOFF, YOFF - SIZE, YOFF - ARM_SEGS * 1.0, YOFF - 120 * 1.5 * 1.5 };
    for (int i = 0; i < 5; i++) {
        pins[i] = (Con){ C_PIN, 1, malloc(sizeof(uint16_t)), 0, 0, { 0, (float)pin_y[i], 0 } };
        pins[i].idx[0] = (uint16_t)i;
    }
}

static double time_phase(double a) { return .5 * (sin(a * PI - .5 * PI) + 1); }

/* ---- the camera (TrackballControls: rotate .75, zoom .75, damping .2, no pan) ------------- */

typedef struct { double x, y, z; } V3;
static V3 v3(double x, double y, double z) { return (V3){ x, y, z }; }
static V3 add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V3 sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 mul(V3 a, double s) { return v3(a.x * s, a.y * s, a.z * s); }
static double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static double len(V3 a) { return sqrt(dot(a, a)); }
static V3 set_len(V3 a, double l) { double n = len(a); return n ? mul(a, l / n) : a; }
static V3 norm(V3 a) { return set_len(a, 1); }
static V3 rotate(V3 v, V3 axis, double angle) {   /* applyQuaternion(setFromAxisAngle(axis, angle)) */
    double s = sin(angle / 2), w = cos(angle / 2);
    V3 q = mul(axis, s), t = mul(cross(q, v), 2);
    return add(add(v, mul(t, w)), cross(q, t));
}

static struct {
    V3 pos, up, rs, re;        /* the camera; _rotateStart, _rotateEnd */
    double zoom_start;         /* _zoomStart.y (_zoomEnd.y stays 0) */
    double min_d, max_d;
    bool rotating;
} cam;

static V3 on_ball(double px, double py, double w, double h) {
    V3 b = v3((px - .5 * w) / (.5 * w), (.5 * h - py) / (.5 * h), 0);
    double f = len(b);
    if (f > 1) b = norm(b);
    else b.z = sqrt(1 - f * f);
    V3 q = cam.pos;
    return add(add(set_len(cam.up, b.y), set_len(cross(cam.up, q), b.x)), set_len(q, b.z));
}

static void controls_update(void) {
    V3 q = cam.pos;
    double c = acos(dot(cam.rs, cam.re) / len(cam.rs) / len(cam.re));
    if (c > 0) {
        V3 a = norm(cross(cam.rs, cam.re));
        c *= .75;
        q = rotate(q, a, -c), cam.up = rotate(cam.up, a, -c), cam.re = rotate(cam.re, a, -c);
        cam.rs = rotate(cam.rs, a, c * (.2 - 1));
    }
    double z = 1 + (0 - cam.zoom_start) * .75;
    if (z != 1 && z > 0) { q = mul(q, z); cam.zoom_start += (0 - cam.zoom_start) * .2; }
    cam.pos = q;
    if (dot(q, q) > cam.max_d * cam.max_d) cam.pos = set_len(q, cam.max_d);
    if (dot(q, q) < cam.min_d * cam.min_d) cam.pos = set_len(q, cam.min_d);
}

/* column-major 4 x 4 */
static void mat_mul(double o[16], const double a[16], const double b[16]) {
    double r[16];
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 4; k++) r[c * 4 + k] = a[k] * b[c * 4] + a[4 + k] * b[c * 4 + 1] + a[8 + k] * b[c * 4 + 2] + a[12 + k] * b[c * 4 + 3];
    memcpy(o, r, sizeof r);
}
static void to_f(float o[16], const double a[16]) { for (int i = 0; i < 16; i++) o[i] = (float)a[i]; }
static void view_matrix(double v[16], V3 *x_out, V3 *y_out, V3 *z_out) {
    V3 z = norm(cam.pos), x = norm(cross(cam.up, z)), y = cross(z, x);
    double m[16] = { x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0, -dot(x, cam.pos), -dot(y, cam.pos), -dot(z, cam.pos), 1 };
    memcpy(v, m, sizeof m);
    if (x_out) *x_out = x, *y_out = y, *z_out = z;
}
static const double FOV = 30, CAM_NEAR = 5, CAM_FAR = 3500;
static void proj_matrix(double p[16], double aspect) {
    double top = CAM_NEAR * tan(PI / 180 * .5 * FOV), bottom = -top, left = aspect * bottom, right = aspect * top;
    double m[16] = { 2 * CAM_NEAR / (right - left), 0, 0, 0, 0, 2 * CAM_NEAR / (top - bottom), 0, 0,
                     (right + left) / (right - left), (top + bottom) / (top - bottom), -(CAM_FAR + CAM_NEAR) / (CAM_FAR - CAM_NEAR), -1,
                     0, 0, -2 * CAM_FAR * CAM_NEAR / (CAM_FAR - CAM_NEAR), 0 };
    memcpy(p, m, sizeof m);
}
static void item_matrix(double m[16], double s) {   /* the group at y 20, the mesh scaled */
    double t[16] = { s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, 0, YOFF, 0, 1 };
    memcpy(m, t, sizeof t);
}

/* ---- lens dirt (LensDirtPass: 200 quads) ------------------------------------------------- */

#define QUADS 200
typedef struct { float c01[4], c23[4], uv[4], alpha[4]; } DirtQuad;
static DirtQuad dirt[QUADS];
static int dirt_index;

static void dirt_init(void) {
    int h = 0, g = 0;
    for (int i = 0; i < QUADS; i++) {
        float e = 1.0f / 10;
        dirt[i].uv[0] = (float)(e * h), dirt[i].uv[1] = (float)(e * g), dirt[i].uv[2] = (float)(e * (h + 1)), dirt[i].uv[3] = (float)(e * (g + 1));
        if (++h == 10) { h = 0; if (++g == 10) g = 0; }
    }
}
static void dirt_set_group(int a, double b, double c, double d, double scale) {
    int i = QUADS, j = dirt_index, k = j;
    for (int l = 0; l < a; l++) {
        double e = b + (sp_random() - .5) * d, f = c + (sp_random() - .5) * d, rot = sp_random() * PI * 2, s = .15 * sp_random() * scale;
        double cs = cos(rot), sn = sin(rot), cx[4] = { -1, 1, 1, -1 }, cy[4] = { -1, -1, 1, 1 };
        float *o[2] = { dirt[k].c01, dirt[k].c23 };
        for (int n = 0; n < 4; n++) {
            double x = cx[n] * s, y = cy[n] * s;
            o[n / 2][(n % 2) * 2] = (float)(cs * x - sn * y + e);
            o[n / 2][(n % 2) * 2 + 1] = (float)(sn * x + cs * y + f);
        }
        dirt[k].alpha[0] = 1;
        k = j + l;
        if (k >= i) j = dirt_index = 0;
    }
    dirt_index = k;
}

/* ---- pictures -------------------------------------------------------------------------- */

static unsigned char *load_rgba(const char *name, int *w, int *h) {
    FILE *f = fopen(sp_asset(name), "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n), *px = NULL;
    if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) px = bare_image_decode(buf, (int)n, w, h);
    free(buf);
    fclose(f);
    return px;
}

/* the dust's 64 x 64 dot and its mipmaps (2 x 2 averages), side by side: 128 x 64 */
static BareImage dust_atlas(void) {
    int w, h;
    unsigned char *px = load_rgba("dust.png", &w, &h);
    if (!px || w != 64 || h != 64) { free(px); return 0; }
    static unsigned char atlas[64 * 128 * 4];
    memset(atlas, 0, sizeof atlas);
    unsigned char *lv = px;
    int x0 = 0;
    for (int s = 64; s >= 1; s /= 2) {
        for (int y = 0; y < s; y++) memcpy(atlas + ((size_t)y * 128 + x0) * 4, lv + (size_t)y * s * 4, (size_t)s * 4);
        if (s == 1) break;
        int t = s / 2;
        unsigned char *next = malloc((size_t)t * t * 4);
        for (int y = 0; y < t; y++)
            for (int x = 0; x < t; x++)
                for (int c = 0; c < 4; c++) {
                    int sum = lv[((2 * y) * s + 2 * x) * 4 + c] + lv[((2 * y) * s + 2 * x + 1) * 4 + c] +
                              lv[((2 * y + 1) * s + 2 * x) * 4 + c] + lv[((2 * y + 1) * s + 2 * x + 1) * 4 + c];
                    next[(y * t + x) * 4 + c] = (unsigned char)((sum + 2) / 4);
                }
        if (lv != px) free(lv);
        lv = next;
        x0 += s;
    }
    if (lv != px) free(lv);
    free(px);
    return bare_image_from_pixels(128, 64, atlas);
}

/* ---- drawing --------------------------------------------------------------------------- */

typedef struct { float projection[16], model_view[16], view[16], diffuse[4], diffuse_b[4], misc[4]; } MeshParams;
typedef struct { float projection[16], model_view[16], diffuse[4], misc[4], size[4]; } LineParams;
typedef struct { float projection[16], model_view[16], misc[4], size[4]; } DustParams;
typedef struct { float misc[4], inc[4], color[4], k[28]; } PostParams;
typedef struct { float misc[4]; } DirtParams;
typedef struct { float p0[4], q0[4], p1[4], q1[4], p2[4], q2[4]; } Tri;
typedef struct { float a[4], ap[4], b[4], bp[4]; } Seg;

static void hex(float o[4], unsigned c, float a) {
    o[0] = ((c >> 16) & 255) / 255.f, o[1] = ((c >> 8) & 255) / 255.f, o[2] = (c & 255) / 255.f, o[3] = a;
}

static Tri *tris;
static Seg *segs;

static int build_tris(const Ints *faces) {
    int n = faces->n / 3;
    for (int t = 0; t < n; t++) {
        float *o[6] = { tris[t].p0, tris[t].q0, tris[t].p1, tris[t].q1, tris[t].p2, tris[t].q2 };
        for (int v = 0; v < 3; v++) {
            int i = faces->v[3 * t + v];
            for (int k = 0; k < 3; k++) o[2 * v][k] = pos[3 * i + k], o[2 * v + 1][k] = prev[3 * i + k];
            o[2 * v][3] = (float)uvs.v[2 * i], o[2 * v + 1][3] = (float)uvs.v[2 * i + 1];
        }
    }
    return n;
}
static int build_segs(const Ints *l) {
    int n = l->n / 2;
    for (int s = 0; s < n; s++) {
        int a = l->v[2 * s], b = l->v[2 * s + 1];
        for (int k = 0; k < 3; k++) {
            segs[s].a[k] = pos[3 * a + k], segs[s].ap[k] = prev[3 * a + k];
            segs[s].b[k] = pos[3 * b + k], segs[s].bp[k] = prev[3 * b + k];
        }
        segs[s].a[3] = segs[s].ap[3] = segs[s].b[3] = segs[s].bp[3] = 0;
    }
    return n;
}

/* the bulb under a click: three's Raycaster against its front faces (world: y + 20, scaled .95) */
static bool raycast_bulb(V3 o, V3 d, V3 *hit) {
    double best = INFINITY;
    for (int t = 0; t + 2 < bulb_faces.n; t += 3) {
        V3 p[3];
        for (int v = 0; v < 3; v++) {
            int i = bulb_faces.v[t + v];
            p[v] = v3(pos[3 * i] * .95, pos[3 * i + 1] * .95 + YOFF, pos[3 * i + 2] * .95);
        }
        V3 e1 = sub(p[1], p[0]), e2 = sub(p[2], p[0]), n = cross(e1, e2);
        double DdN = dot(d, n);
        if (DdN >= 0) continue;   /* back faces (and edge-on) */
        DdN = -DdN;
        V3 diff = sub(o, p[0]);
        double DdQxE2 = -dot(d, cross(diff, e2));   /* sign -1 */
        if (DdQxE2 < 0) continue;
        double DdE1xQ = -dot(d, cross(e1, diff));
        if (DdE1xQ < 0 || DdQxE2 + DdE1xQ > DdN) continue;
        double QdN = dot(diff, n);   /* -sign * dot */
        if (QdN < 0) continue;
        double tt = QdN / DdN;
        if (tt < best) best = tt, *hit = add(o, mul(d, tt));
    }
    return best < INFINITY;
}

int main(int argc, char **argv) {
    SpArgs sp = sp_args(argc, argv);
    if (!bare_window_with(.width = sp.width ? sp.width : 1280, .height = sp.height ? sp.height : 720, .title = "Medusae", .parent = sp.parent)) return 1;
    bare_background(BARE_BLACK);
    BareShader mesh = bare_shader(&mesh_shader), line = bare_shader(&line_shader), dust = bare_shader(&dust_shader);
    BareShader post = bare_shader(&post_shader), dirt_prog = bare_shader(&dirt_shader);

    create_medusae();
    dirt_init();
    tris = malloc(sizeof(Tri) * (size_t)(mouth_faces.n / 3 + bulb_faces.n / 3 + tail_faces.n / 3 + 1));
    segs = malloc(sizeof(Seg) * (size_t)(tent_links.n / 2 + links.n / 2 + 1));
    /* Math.random as the page spends it: the lens dirt's picture first, then 8000 dust motes in -150..150 */
    for (int i = 0; i < 1216; i++) sp_random();
    float (*motes)[4] = malloc(sizeof(float[4]) * 8000);
    for (int i = 0; i < 8000; i++) {
        for (int k = 0; k < 3; k++) motes[i][k] = (float)(sp_random() * 300 - 150);
        motes[i][3] = 1;
    }
    for (int i = 0; i < 24; i++) sp_random();   /* and the rest of its start */
    BareImage dust_img = dust_atlas();
    int dw, dh;
    unsigned char *dpx = load_rgba("lensdirt.png", &dw, &dh);
    BareImage dirt_img = dpx ? bare_image_from_pixels(dw, dh, dpx) : 0;
    free(dpx);
    PostParams conv;
    memset(&conv, 0, sizeof conv);
    {   /* ConvolutionShader.buildKernel(8): 25 taps */
        double k[25], sum = 0;
        for (int c = 0; c < 25; c++) { double x = c - 12; k[c] = exp(-(x * x) / (2 * 8.0 * 8.0)); sum += k[c]; }
        for (int c = 0; c < 25; c++) conv.k[c] = (float)(k[c] / sum);
    }

    const char *dump = getenv("MEDUSAE_DUMP");
    int dump_frame = getenv("MEDUSAE_DUMP_FRAME") ? atoi(getenv("MEDUSAE_DUMP_FRAME")) : -1;
    bool fixed = getenv("BARE_FIXED_DT") != NULL;

    BareImage scene = 0, depth = 0, bloom_x = 0, bloom_y = 0, final = 0, out = 0;
    bloom_x = bare_canvas(.width = 512, .height = 512, .bottom_up = true);
    bloom_y = bare_canvas(.width = 512, .height = 512, .bottom_up = true);
    int cw = 0, ch = 0, rw = 0, rh = 0;
    bool paused = false, started = false, was_down = false, dragged = false;
    double acc_ms = 0, last_date = 0, dust_time = 0, last_nudge = NAN;
    const double step_ms = 1.0 / 30 * 1e3;
    long frame = 0;
    int nudge_index = 0;
    BareVec2 down_at = { 0, 0 };

    while (bare_frame()) {
        SpMessage m;
        while (sp_next(&m))
            if (m.kind == SP_PAUSE) paused = sp_bool(&m);
        BareVec2 win = bare_window_size();
        int w = (int)fmax(1, floor(win.x + 0.5)), h = (int)fmax(1, floor(win.y + 0.5));
        if (w != cw || h != ch) {   /* onWindowResize: the camera from the height, the canvas at 1.5x */
            cw = w, ch = h;
            rw = (int)(w * 1.5), rh = (int)(h * 1.5);
            if (scene) bare_image_free(scene), bare_image_free(depth), bare_image_free(final), bare_image_free(out);
            scene = bare_canvas(.width = rw, .height = rh, .bottom_up = true);
            final = bare_canvas(.width = rw, .height = rh, .bottom_up = true);
            depth = bare_canvas(.width = rw, .height = rh, .format = BARE_DEPTH);
            out = bare_canvas(.width = w, .height = h, .bottom_up = true);
            double e = h / 1e3;
            if (!started) cam.pos = v3(400 * e, 300 * e, 0), cam.up = v3(0, 1, 0);
            cam.min_d = 200 * e, cam.max_d = 1200 * e;
        }
        BareState box = bare_box(.width = BARE_GROW, .height = BARE_GROW, .image = out);
        if (paused) { bare_continuous(false); started = false; continue; }

        /* the clock: Date.now(); on BARE_FIXED_DT the page's test clock exactly (frame n at n * 1000 / 60) */
        double date = 1767225600000.0 + (fixed ? frame * (1000.0 / 60) : bare_time() * 1000);
        frame++;

        /* the mouse: TrackballControls and the page's click (a press and release without a move) */
        BareVec2 mo = bare_mouse();
        bool down = box.pressed;
        if (down && !was_down) {
            dragged = false, down_at = mo;
            cam.rs = cam.re = on_ball(mo.x, mo.y, w, h), cam.rotating = true;
        } else if (down && (mo.x != down_at.x || mo.y != down_at.y)) {
            dragged = true, down_at = mo;
            cam.re = on_ball(mo.x, mo.y, w, h);
        } else if (!down && was_down) {
            cam.rotating = false;
            double c = date - last_nudge;
            if (!dragged && !(250 > c)) {
                if (c > 800) nudge_index = 0;
                if (nudge_index <= 2) {
                    double nx = mo.x / w * 2 - 1, ny = -(mo.y / h) * 2 + 1, t = tan(PI / 180 * .5 * FOV);
                    V3 xa, ya, za;
                    double vm[16];
                    view_matrix(vm, &xa, &ya, &za);
                    V3 dir = norm(add(add(mul(xa, nx * t * w / h), mul(ya, ny * t)), mul(za, -1)));
                    V3 k;
                    if (raycast_bulb(cam.pos, dir, &k)) {
                        k = add(k, mul(norm(k), 15));
                        rep_intensity = (nudge_index + 1) / 3.0 + .5;
                        rep_pos[0] = (float)k.x, rep_pos[1] = (float)k.y, rep_pos[2] = (float)k.z;
                        dirt_set_group(5 * nudge_index, nx, ny, .8, (double)rh / rw);
                        last_nudge = date, nudge_index++;
                    }
                }
            }
        }
        was_down = down;
        BareVec2 wh = bare_wheel();
        if (wh.y) cam.zoom_start += .01 * (-wh.y * 1.2 / 40);

        /* Looper: 30 Hz physics steps, the frame between the last two */
        if (!started) { started = true; last_date = date; acc_ms = 0; }
        double b = fmin(step_ms, date - last_date);
        last_date = date;
        acc_ms += b;
        int steps = (int)floor(acc_ms / step_ms);
        if (steps > 0) acc_ms -= steps * step_ms;
        for (; steps > 0; steps--) {
            rep_intensity *= .8;
            anim_time += .001 * step_ms;
            double c = time_phase(anim_time);
            update_ribs(ribs, RIBS, c);
            update_ribs(tail_ribs, TAIL_RIBS, c);
            tick(.001 * step_ms);
            for (int q = 0; q < QUADS; q++) dirt[q].alpha[0] = (float)(dirt[q].alpha[0] * .995);
        }
        double sp_ = acc_ms / step_ms;
        if (dump && frame - 1 == dump_frame) {
            FILE *f = fopen(dump, "wb");
            if (f) { fwrite(&count, 4, 1, f); fwrite(&anim_time, 8, 1, f); fwrite(pos, 4, 3 * (size_t)count, f); fwrite(prev, 4, 3 * (size_t)count, f); fclose(f); }
            return 0;
        }

        /* preRender: the camera, then the scene */
        controls_update();
        dust_time += .005 * step_ms;
        double P[16], V[16], M[16], MV[16];
        proj_matrix(P, (double)w / h);
        view_matrix(V, NULL, NULL, NULL);
        float spf = (float)sp_;

        LineParams lp;
        to_f(lp.projection, P);
        item_matrix(M, 1), mat_mul(MV, V, M), to_f(lp.model_view, MV);
        lp.size[0] = (float)rw, lp.size[1] = (float)rh, lp.size[2] = lp.size[3] = 0;
        /* tentacles: normal blending */
        hex(lp.diffuse, 0x997199, .25f);
        lp.misc[0] = 2000, lp.misc[1] = spf, lp.misc[2] = 0, lp.misc[3] = 0;
        int n = build_segs(&tent_links);
        bare_draw(.target = scene, .shader = line, .vertex_count = 6, .instances = segs, .instance_size = sizeof(Seg), .instance_count = n,
                  .blend = BARE_BLEND_ALPHA, BARE_PARAMS(lp), .clear = true,
                  .clear_color = { 0x0A / 255.f, 0x06 / 255.f, 0x0E / 255.f, 1 });
        /* the hood's contour: added */
        hex(lp.diffuse, 0xFFDDE9, .35f);
        lp.misc[0] = 1200, lp.misc[2] = 1;
        n = build_segs(&links);
        bare_draw(.target = scene, .shader = line, .vertex_count = 6, .instances = segs, .instance_size = sizeof(Seg), .instance_count = n,
                  .blend = BARE_BLEND_ADD, BARE_PARAMS(lp));

        MeshParams mp;
        to_f(mp.projection, P), to_f(mp.view, V);
        mp.misc[1] = spf, mp.misc[2] = (float)anim_time, mp.misc[3] = 0;
        /* tail and mouth */
        item_matrix(M, .95), mat_mul(MV, V, M), to_f(mp.model_view, MV);
        hex(mp.diffuse, 0xE4BAEE, .75f), hex(mp.diffuse_b, 0x241138, 20), mp.misc[0] = 2;
        n = build_tris(&tail_faces);
        bare_draw(.target = scene, .shader = mesh, .vertex_count = 3, .instances = tris, .instance_size = sizeof(Tri), .instance_count = n,
                  .blend = BARE_BLEND_ALPHA, BARE_PARAMS(mp), .depth = depth, .depth_test = BARE_DEPTH_LESS_EQUAL, .clear_depth = true);
        item_matrix(M, 1), mat_mul(MV, V, M), to_f(mp.model_view, MV);
        hex(mp.diffuse, 0xEFA5F0, .65f), hex(mp.diffuse_b, 0x4A67CE, 3);
        n = build_tris(&mouth_faces);
        bare_draw(.target = scene, .shader = mesh, .vertex_count = 3, .instances = tris, .instance_size = sizeof(Tri), .instance_count = n,
                  .blend = BARE_BLEND_ALPHA, BARE_PARAMS(mp), .depth = depth, .depth_test = BARE_DEPTH_LESS_EQUAL);
        /* gel (added, no depth), then the bulb */
        n = build_tris(&bulb_faces);
        item_matrix(M, 1.05), mat_mul(MV, V, M), to_f(mp.model_view, MV);
        hex(mp.diffuse, 0x415AB5, .25f), mp.misc[0] = 1;
        bare_draw(.target = scene, .shader = mesh, .vertex_count = 3, .instances = tris, .instance_size = sizeof(Tri), .instance_count = n,
                  .blend = BARE_BLEND_ADD, BARE_PARAMS(mp));
        item_matrix(M, .95), mat_mul(MV, V, M), to_f(mp.model_view, MV);
        hex(mp.diffuse, 0xFFAAD2, .75f), hex(mp.diffuse_b, 0x70256C, 1), mp.misc[0] = 0;
        bare_draw(.target = scene, .shader = mesh, .vertex_count = 3, .instances = tris, .instance_size = sizeof(Tri), .instance_count = n,
                  .blend = BARE_BLEND_ALPHA, BARE_PARAMS(mp), .depth = depth, .depth_test = BARE_DEPTH_LESS_EQUAL);
        /* dust */
        if (dust_img) {
            DustParams dp;
            to_f(dp.projection, P), to_f(dp.model_view, V);
            dp.misc[0] = (float)dust_time, dp.misc[1] = 300, dp.misc[2] = 48, dp.misc[3] = 150;
            dp.size[0] = (float)rw, dp.size[1] = (float)rh, dp.size[2] = .95f, dp.size[3] = 0;
            bare_draw(.target = scene, .shader = dust, .inputs = { dust_img }, .vertex_count = 6, BARE_INSTANCES(motes, 8000),
                      .blend = BARE_BLEND_ADD, BARE_PARAMS(dp));
        }

        /* BloomPass(.8, 25, 8, 512), LensDirtPass, VignetteShader, then the browser's scaling */
        conv.misc[0] = 0, conv.misc[1] = 512, conv.misc[2] = 512;
        conv.inc[0] = .001953125f, conv.inc[1] = 0;
        bare_draw(.target = bloom_x, .shader = post, .inputs = { scene }, .vertex_count = 6, BARE_PARAMS(conv));
        conv.inc[0] = 0, conv.inc[1] = .001953125f;
        bare_draw(.target = bloom_y, .shader = post, .inputs = { bloom_x }, .vertex_count = 6, BARE_PARAMS(conv));
        PostParams pp = { { 1, (float)rw, (float)rh, 0 }, { 0 }, { 0 }, { 0 } };
        bare_draw(.target = scene, .shader = post, .inputs = { bloom_y }, .vertex_count = 6, .blend = BARE_BLEND_ADD, BARE_PARAMS(pp));
        float amax = 0;
        for (int q = 0; q < QUADS; q++) amax = fmaxf(amax, dirt[q].alpha[0]);
        if (dirt_img && amax > 1e-4f) {
            DirtParams dp = { { (float)rh / rw, 0, 0, 0 } };
            bare_draw(.target = scene, .shader = dirt_prog, .inputs = { dirt_img }, .vertex_count = 6, BARE_INSTANCES(dirt, QUADS),
                      .blend = BARE_BLEND_ADD, BARE_PARAMS(dp));
        }
        pp.misc[0] = 2;
        hex(pp.color, 0x07070C, 1.25f);
        for (int k = 0; k < 3; k++) pp.color[k] *= .5f;
        bare_draw(.target = final, .shader = post, .inputs = { scene }, .vertex_count = 6, BARE_PARAMS(pp));
        pp.misc[0] = 3, pp.misc[1] = (float)w, pp.misc[2] = (float)h;
        bare_draw(.target = out, .shader = post, .inputs = { final }, .vertex_count = 6, BARE_PARAMS(pp));
        bare_continuous(true);
    }
    bare_quit();
}
