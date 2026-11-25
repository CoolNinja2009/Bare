/* koi.c: a garden pond seen from above; Screen+'s "Koi Pond" wallpaper.
 *
 * A port of Screen+'s Koi Pond page (samples/KoiPond/index.html), shaders as
 * written there. The water is a height field (wave equation, ping-ponged half
 * floats) stepped at a fixed rate, so ripples spread the same at any frame
 * rate. The pond floor (pebbles, silt, boulders, algae) is drawn once per
 * size; every frame it gets caustics the ripples focus, then the koi and
 * their shadows go on top. The water pass refracts that through the surface
 * and mixes in the reflected sky, trees, sun or moon, glints and, at night,
 * glowing wakes. Lily pads, lotus flowers, petals, food and fireflies float
 * on top and ride the waves. Koi and floats are bare_draw: body strips and
 * instanced quads.
 *
 *   move   ripples follow the cursor; curious koi follow it
 *   click  scatters food: the koi rise and race for it
 *
 * A Screen+ app wallpaper: settings are the Customise panel (LivelyProperties.json
 * beside the exe), sent by Screen+ on stdin (see ../screenplus/screenplus_wallpaper.h);
 * by hand they're arguments: koi time=4 koi=20 rain=2
 *
 * Every canvas is bottom_up, as WebGL's, so the page's shaders and its pond
 * units (x 0..aspect, y 0..1 from the bottom) carry over unchanged. While
 * Screen+ has it paused it draws nothing. Not ported: music reaction. */
#include "bare.h"
#include "koi_shaders.h"
#include "../screenplus/screenplus_wallpaper.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TAU 6.283185307179586

/* ---- settings ---------------------------------------------------------------- */

static struct {
    int quality, fps, time, rain, koi, variety, lilies, drift, tint;
    double koi_size, koi_speed, flowers, petals, clarity, ripples, brightness;
    bool follow, cursor_ripples, trees, fireflies, glow;
} cfg = { .quality = 1, .fps = 1, .koi = 9, .lilies = 9, .koi_size = 100, .koi_speed = 100, .flowers = 35,
          .petals = 30, .clarity = 60, .ripples = 100, .brightness = 100,
          .follow = true, .cursor_ripples = true, .trees = true, .fireflies = true, .glow = true };

static bool want_quality, want_koi, want_variety, want_lilies;

static void set_property(const SpMessage *m) {
    static const struct { const char *name; char kind; void *at; bool *flag; } props[] = {
        { "quality", 'i', &cfg.quality, &want_quality }, { "fps", 'i', &cfg.fps, NULL },
        { "time", 'i', &cfg.time, NULL }, { "rain", 'i', &cfg.rain, NULL }, { "koi", 'i', &cfg.koi, &want_koi },
        { "variety", 'i', &cfg.variety, &want_variety }, { "lilies", 'i', &cfg.lilies, &want_lilies },
        { "drift", 'i', &cfg.drift, NULL }, { "tint", 'i', &cfg.tint, NULL },
        { "koiSize", 'd', &cfg.koi_size, NULL }, { "koiSpeed", 'd', &cfg.koi_speed, NULL },
        { "flowers", 'd', &cfg.flowers, NULL }, { "petals", 'd', &cfg.petals, NULL },
        { "clarity", 'd', &cfg.clarity, NULL }, { "ripples", 'd', &cfg.ripples, NULL },
        { "brightness", 'd', &cfg.brightness, NULL }, { "follow", 'b', &cfg.follow, NULL },
        { "cursorRipples", 'b', &cfg.cursor_ripples, NULL }, { "trees", 'b', &cfg.trees, NULL },
        { "fireflies", 'b', &cfg.fireflies, NULL }, { "glow", 'b', &cfg.glow, NULL },
    };
    for (size_t i = 0; i < sizeof props / sizeof props[0]; i++) {
        if (strcmp(m->name, props[i].name)) continue;
        if (props[i].kind == 'b') *(bool *)props[i].at = sp_bool(m);
        else if (props[i].kind == 'i') *(int *)props[i].at = (int)lround(sp_num(m));
        else *(double *)props[i].at = sp_num(m);
        if (props[i].flag) *props[i].flag = true;
    }
}

/* ---- constants ------------------------------------------------------------------ */

static const struct { double scale, max_px; int sim; } QUALITY[4] = {   /* scene buffer; water rows */
    { 0.5, 1.0e6, 256 }, { 0.7, 2.4e6, 320 }, { 0.85, 5e6, 400 }, { 1.0, 12e6, 480 },
};
static const float TINTS[4][2][3] = {   /* deep colour, shallow tint */
    { { 0.030f, 0.150f, 0.125f }, { 0.30f, 0.46f, 0.33f } },   /* Jade */
    { { 0.020f, 0.115f, 0.160f }, { 0.24f, 0.44f, 0.47f } },   /* Teal */
    { { 0.012f, 0.045f, 0.130f }, { 0.20f, 0.34f, 0.50f } },   /* Deep blue */
    { { 0.090f, 0.095f, 0.045f }, { 0.40f, 0.38f, 0.22f } },   /* Murky */
};

/* A look: the sky, the light and what the night brings. */
typedef struct Look {
    double top[3], hor[3], sun[3], sun_i, sun_pos[2], moon, amb, caus, bio, fly, refl, night;
} Look;
#define LOOK_N ((int)(sizeof(Look) / sizeof(double)))
enum { DAY, GOLDEN, DUSK, NIGHT };
static const Look KEYS[4] = {
    { { .30, .55, .86 }, { .80, .88, .95 }, { 1, .96, .88 }, 1, { .80, .90 }, 0, 1, 1, 0, 0, .20, 0 },
    { { .36, .42, .64 }, { 1, .64, .34 }, { 1, .72, .42 }, .95, { .86, .97 }, 0, .80, .75, 0, .15, .30, 0 },
    { { .10, .10, .26 }, { .78, .36, .34 }, { 1, .50, .38 }, .45, { .92, 1.02 }, 0, .48, .3, .5, .7, .32, .4 },
    { { .02, .04, .09 }, { .06, .11, .2 }, { .72, .82, 1 }, .6, { .24, .80 }, 1, .36, .14, 1, 1, .30, 1 },
};
static const struct { double hour; int key; } SCHEDULE[] = {
    { 0, NIGHT }, { 5, NIGHT }, { 6.3, GOLDEN }, { 8.5, DAY }, { 17, DAY }, { 18.6, GOLDEN }, { 19.8, DUSK },
    { 21, NIGHT }, { 24, NIGHT },
};
static const int VARIETY_WEIGHTS[7] = { 3, 2, 2, 2, 1, 1, 1 };   /* kohaku, sanke, showa, ogon, platinum, asagi, chagoi */
static const double OVERCAST[4] = { 0, 0.35, 0.6, 0.85 }, RAIN_RATE[4] = { 0, 6, 32, 95 };

enum { NS = 14, MS = 20, MAX_KOI = 40, KOI_VERTS = (MS - 1) * 6 + 18, MAX_INST = 640,
       MAX_PADS = 30, MAX_PETALS = 220, MAX_PELLETS = 60, MAX_FLIES = 36, MAX_DROPS = 256 };

/* ---- helpers -------------------------------------------------------------------- */

static double clampd(double x, double a, double b) { return x < a ? a : x > b ? b : x; }
static double smooth(double a, double b, double x) {
    double t = clampd((x - a) / (b - a), 0, 1);
    return t * t * (3 - 2 * t);
}
static double lerp(double a, double b, double t) { return a + (b - a) * t; }
static double rnd(void) { return (double)rand() / ((double)RAND_MAX + 1); }
static Look mix_look(const Look *a, const Look *b, double t) {
    Look o;
    const double *pa = (const double *)a, *pb = (const double *)b;
    double *po = (double *)&o;
    for (int i = 0; i < LOOK_N; i++) po[i] = lerp(pa[i], pb[i], t);
    return o;
}
/* mulberry32: the same pond layout every time */
static uint32_t mseed;
static double mrnd(void) {
    uint32_t a = mseed += 0x6D2B79F5u;
    uint32_t t = (a ^ (a >> 15)) * (1u | a);
    t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
    return (double)(t ^ (t >> 14)) / 4294967296.0;
}

/* ---- the pond --------------------------------------------------------------------- */

typedef struct Koi {
    double seed, sz, spd, x, y, a, v, depth, td, next_depth, phase, burst, next_burst, wake_t, turn_s, flee, fx, fy;
    int variety;
    bool curious;
    double spine[NS * 2];
} Koi;
typedef struct Pad { double ax, ay, x, y, r, rot, vx, vy, vr, seed, hue, fseed; int fcol; } Pad;
typedef struct Petal { double x, y, rot, vr, vx, vy, age, life, sz; int col; bool leaf, landed; } Petal;
typedef struct Pellet { double x, y, vx, vy, age, seed; } Pellet;
typedef struct Fly { double x, y, vx, vy, ph, rate, fade; } Fly;

static struct {
    double aspect, clock;
    struct { double x, y, last; bool inside; } mouse;
    float drops[MAX_DROPS][4];
    int ndrops;
    Koi koi[MAX_KOI]; int nkoi;
    Pad pads[MAX_PADS]; int npads;
    Petal petals[MAX_PETALS]; int npetals;
    Pellet pellets[MAX_PELLETS]; int npellets;
    Fly flies[MAX_FLIES]; int nflies;
    double wind[2], wind_a, rain_acc, flash, next_bolt, bolt, idle_drop, autumn, overcast, sim_acc;
    Look look;
    bool have_look;
} P;

static void push(double x, double y, double s) {   /* drops shove floating things outward */
    const double R = 0.16, k = fabs(s) * 0.05;
    for (int i = 0; i < P.npads; i++) {
        Pad *f = &P.pads[i];
        double dx = f->x - x, dy = f->y - y, d = hypot(dx, dy);
        if (d < R && d > 1e-4) { double w = (1 - d / R) * k / (f->r * 18); f->vx += dx / d * w; f->vy += dy / d * w; }
    }
    for (int i = 0; i < P.npetals; i++) {
        Petal *f = &P.petals[i];
        double dx = f->x - x, dy = f->y - y, d = hypot(dx, dy);
        if (d < R && d > 1e-4) { double w = (1 - d / R) * k; f->vx += dx / d * w; f->vy += dy / d * w; }
    }
    for (int i = 0; i < P.npellets; i++) {
        Pellet *f = &P.pellets[i];
        double dx = f->x - x, dy = f->y - y, d = hypot(dx, dy);
        if (d < R && d > 1e-4) { double w = (1 - d / R) * k; f->vx += dx / d * w; f->vy += dy / d * w; }
    }
}

static void drop(double x, double y, double r, double s) {
    if (P.ndrops < MAX_DROPS) {
        float *d = P.drops[P.ndrops++];
        d[0] = (float)x, d[1] = (float)y, d[2] = (float)r, d[3] = (float)(s * cfg.ripples / 100);
    }
    if (fabs(s) > 0.2) push(x, y, s);
}

static int pick_variety(void) {
    if (cfg.variety > 0) return (int)clampd(cfg.variety - 1, 0, 6);
    double t = rnd() * 12;
    for (int i = 0; i < 7; i++) if ((t -= VARIETY_WEIGHTS[i]) < 0) return i;
    return 0;
}
static double koi_len(const Koi *k) { return 0.13 * cfg.koi_size / 100 * k->sz; }

static void make_koi(Koi *k) {
    memset(k, 0, sizeof *k);
    k->seed = rnd(), k->variety = pick_variety(), k->sz = 0.72 + 0.56 * rnd(), k->spd = 0.85 + 0.3 * rnd();
    k->x = 0.15 + rnd() * (P.aspect - 0.3), k->y = 0.15 + rnd() * 0.7, k->a = rnd() * TAU;
    k->depth = 0.2 + 0.6 * rnd(), k->phase = rnd() * 6.28;
    k->next_burst = 2 + rnd() * 6, k->curious = rnd() < 0.65;
    k->td = k->depth;
    double seg = koi_len(k) / (NS - 1);
    for (int i = 0; i < NS; i++) k->spine[i * 2] = k->x - cos(k->a) * seg * i, k->spine[i * 2 + 1] = k->y - sin(k->a) * seg * i;
}

static void make_pads(void) {   /* in a few clusters round the edges */
    mseed = 7331;
    double clusters[4][2] = { { 0.18, 0.22 }, { P.aspect - 0.25, 0.75 }, { P.aspect * 0.62, 0.12 }, { 0.12, 0.8 } };
    P.npads = (int)clampd(cfg.lilies, 0, MAX_PADS);
    for (int i = 0; i < P.npads; i++) {
        const double *c = clusters[i % 4];
        double a = mrnd() * TAU, d = 0.03 + mrnd() * 0.16;
        Pad *p = &P.pads[i];
        memset(p, 0, sizeof *p);
        p->ax = clampd(c[0] + cos(a) * d, 0.03, P.aspect - 0.03), p->ay = clampd(c[1] + sin(a) * d * 0.8, 0.03, 0.97);
        p->r = 0.032 + mrnd() * 0.04, p->rot = mrnd() * TAU;
        p->seed = mrnd(), p->hue = mrnd(), p->fseed = mrnd(), p->fcol = (int)floor(mrnd() * 4);
        p->x = p->ax, p->y = p->ay;
    }
}

static void sync_population(void) {
    if (want_koi) {
        int n = (int)clampd(cfg.koi, 0, MAX_KOI);
        while (P.nkoi < n) make_koi(&P.koi[P.nkoi++]);
        P.nkoi = n;
    }
    if (want_variety) for (int i = 0; i < P.nkoi; i++) P.koi[i].variety = pick_variety();
    if (want_lilies) make_pads();
    want_koi = want_variety = want_lilies = false;
}

static Look target_look(void) {
    if (cfg.time >= 1) return KEYS[(int)clampd(cfg.time - 1, 0, 3)];
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    double h = lt ? lt->tm_hour + lt->tm_min / 60.0 + lt->tm_sec / 3600.0 : 12;
    int i = 0, n = (int)(sizeof SCHEDULE / sizeof SCHEDULE[0]);
    while (i < n - 2 && h >= SCHEDULE[i + 1].hour) i++;
    return mix_look(&KEYS[SCHEDULE[i].key], &KEYS[SCHEDULE[i + 1].key],
                    smooth(0, 1, (h - SCHEDULE[i].hour) / (SCHEDULE[i + 1].hour - SCHEDULE[i].hour)));
}

/* ---- simulation ------------------------------------------------------------------ */

static void update_koi(double dt) {
    double sp = cfg.koi_speed / 100;
    for (int ki = 0; ki < P.nkoi; ki++) {
        Koi *k = &P.koi[ki];
        double L = koi_len(k);
        double base = 0.05 * sp * k->spd * sqrt(k->sz);
        double hx = cos(k->a), hy = sin(k->a);
        double sx = hx, sy = hy, sm = 1;
        double w = sin(P.clock * 0.23 + k->seed * 40) * 0.9 + sin(P.clock * 0.61 + k->seed * 91) * 0.5;
        sx += -hy * w * 0.45, sy += hx * w * 0.45;
        double m = 0.1 + L * 0.6;
        if (k->x < m) sx += 3 * pow((m - k->x) / m, 2);
        if (k->x > P.aspect - m) sx -= 3 * pow((k->x - P.aspect + m) / m, 2);
        if (k->y < m) sy += 3 * pow((m - k->y) / m, 2);
        if (k->y > 1 - m) sy -= 3 * pow((k->y - 1 + m) / m, 2);
        for (int oi = 0; oi < P.nkoi; oi++) {
            const Koi *o = &P.koi[oi];
            if (o == k) continue;
            double dx = k->x - o->x, dy = k->y - o->y, d = hypot(dx, dy), lim = (L + koi_len(o)) * 0.6;
            if (d < lim && d > 1e-4 && fabs(k->depth - o->depth) < 0.35) {
                double f = (lim - d) / lim * 3;
                sx += dx / d * f, sy += dy / d * f;
            }
        }
        k->next_depth -= dt;
        if (k->next_depth <= 0) k->td = 0.1 + rnd() * 0.75, k->next_depth = 6 + rnd() * 12;
        double td = k->td, depth_rate = 0.35;
        /* food first */
        int best = -1;
        double bd = 0.75;
        double mx = k->x + hx * L * 0.04, my = k->y + hy * L * 0.04;
        for (int i = 0; i < P.npellets; i++) {
            double d = hypot(P.pellets[i].x - mx, P.pellets[i].y - my);
            if (d < bd) bd = d, best = i;
        }
        if (best >= 0) {
            Pellet *b = &P.pellets[best];
            sx += (b->x - k->x) / fmax(bd, 0.02) * 4, sy += (b->y - k->y) / fmax(bd, 0.02) * 4;
            sm = bd < L * 0.5 ? 1.1 : 2.2, td = 0, depth_rate = 1.4;
            if (bd < L * 0.16 && k->depth < 0.15) {
                double bx = b->x, by = b->y;
                memmove(b, b + 1, (size_t)(P.npellets - best - 1) * sizeof *b);
                P.npellets--;
                drop(bx, by, 0.014, -0.55);
            }
        } else if (cfg.follow && P.mouse.inside && k->curious && P.clock - P.mouse.last < 25) {
            double dx = P.mouse.x - k->x, dy = P.mouse.y - k->y, d = hypot(dx, dy);
            if (d < 0.45 && d > 1e-4) {
                if (d > L * (0.9 + 1.4 * k->seed)) sx += dx / d * 1.3, sy += dy / d * 1.3, sm = 1.25;
                else sx += -dy / d * 1.2 - dx / d * 0.6, sy += dx / d * 1.2 - dy / d * 0.6, sm = 0.7;
                td = 0.12, depth_rate = 0.6;
            }
        }
        if (k->flee > 0) k->flee -= dt, sx += k->fx * 5, sy += k->fy * 5, sm = 3.2;
        k->next_burst -= dt;
        if (k->next_burst <= 0) k->burst = 1, k->next_burst = 3 + rnd() * 7;
        k->burst = fmax(0, k->burst - dt * 1.4);
        double da = atan2(sy, sx) - k->a;
        da = atan2(sin(da), cos(da));
        /* the turning circle stays at least 1.2 body lengths across, so a slow koi doesn't curl up */
        double max_turn = fmin(0.8 + (sm > 1.5 ? 1.2 : 0), fmax(0.2, k->v / (1.2 * L))) * dt;
        double turn = clampd(da, -max_turn, max_turn);
        k->a += turn;
        k->turn_s += (turn / fmax(dt, 1e-3) - k->turn_s) * fmin(1, dt * 4);
        double vt = base * sm * (1 + k->burst * 0.8);
        k->v += (vt - k->v) * fmin(1, dt * 1.6);
        k->x += cos(k->a) * k->v * dt;
        k->y += sin(k->a) * k->v * dt;
        k->x = clampd(k->x, -0.05, P.aspect + 0.05), k->y = clampd(k->y, -0.05, 1.05);
        k->depth += (td - k->depth) * fmin(1, dt * depth_rate);
        k->phase += dt * (2.5 + 9 * k->v / L);
        double seg = L / (NS - 1), *s = k->spine;
        s[0] = k->x, s[1] = k->y;
        for (int i = 1; i < NS; i++) {
            double dx = s[i * 2] - s[i * 2 - 2], dy = s[i * 2 + 1] - s[i * 2 - 1], d = hypot(dx, dy);
            if (d == 0) d = 1;
            s[i * 2] = s[i * 2 - 2] + dx / d * seg, s[i * 2 + 1] = s[i * 2 - 1] + dy / d * seg;
        }
        /* a wake when swimming near the surface */
        if (k->depth < 0.22) {
            k->wake_t -= dt;
            if (k->wake_t <= 0) {
                k->wake_t = 0.09;
                drop(k->x + hx * L * 0.1, k->y + hy * L * 0.1, 0.013, -0.07 * (1 - k->depth / 0.22) * (k->v / base));
            }
        }
        /* nudge pads it swims under */
        if (k->depth < 0.25)
            for (int i = 0; i < P.npads; i++) {
                Pad *p = &P.pads[i];
                double dx = p->x - k->x, dy = p->y - k->y;
                if (dx * dx + dy * dy < p->r * p->r)
                    p->vx += cos(k->a) * k->v * dt * 2, p->vy += sin(k->a) * k->v * dt * 2, p->vr += (rnd() - 0.5) * dt;
            }
    }
}

static void new_petal(Petal *p, bool landed, double age) {
    p->x = rnd() * P.aspect, p->y = rnd(), p->rot = rnd() * TAU, p->vr = (rnd() - 0.5) * 0.3, p->vx = p->vy = 0;
    p->age = age, p->life = 40 + rnd() * 30, p->leaf = cfg.drift == 1, p->sz = 0.7 + rnd() * 0.6;
    p->col = (int)floor(rnd() * 4), p->landed = landed;
}

static void update_floats(double dt) {
    P.wind_a += (rnd() - 0.5) * dt * 0.3;
    double ws = 0.004 + 0.006 * P.overcast;
    P.wind[0] = cos(P.wind_a) * ws, P.wind[1] = sin(P.wind_a) * ws;
    double damp = exp(-dt * 0.8);
    for (int i = 0; i < P.npads; i++) {   /* anchored by their stems */
        Pad *p = &P.pads[i];
        p->vx += ((p->ax - p->x) * 0.25 + P.wind[0] * 0.3) * dt, p->vy += ((p->ay - p->y) * 0.25 + P.wind[1] * 0.3) * dt;
        p->vx *= damp, p->vy *= damp, p->vr *= damp;
        p->x += p->vx * dt, p->y += p->vy * dt, p->rot += p->vr * dt;
    }
    for (int i = 0; i < P.npads; i++)
        for (int j = i + 1; j < P.npads; j++) {
            Pad *a = &P.pads[i], *b = &P.pads[j];
            double dx = b->x - a->x, dy = b->y - a->y, d = hypot(dx, dy), lim = (a->r + b->r) * 0.92;
            if (d < lim && d > 1e-5) {
                double f = (lim - d) * 0.5 / d;
                a->x -= dx * f, a->y -= dy * f, b->x += dx * f, b->y += dy * f, a->vr += f * 0.05, b->vr -= f * 0.05;
            }
        }
    /* petals or leaves fall onto the water */
    double rate = pow(cfg.petals / 100, 1.5) * 1.1;
    if (rnd() < rate * dt && P.npetals < MAX_PETALS) new_petal(&P.petals[P.npetals++], false, 0);
    for (int i = P.npetals - 1; i >= 0; i--) {
        Petal *p = &P.petals[i];
        p->age += dt;
        if (!p->landed && p->age > 0.5) p->landed = true, drop(p->x, p->y, 0.008, -0.18);
        p->vx = (p->vx + P.wind[0] * dt * 0.5) * damp, p->vy = (p->vy + P.wind[1] * dt * 0.5) * damp;
        p->x += p->vx * dt + P.wind[0] * dt, p->y += p->vy * dt + P.wind[1] * dt, p->rot += p->vr * dt;
        if (p->age > p->life || p->x < -0.05 || p->x > P.aspect + 0.05 || p->y < -0.05 || p->y > 1.05) {
            memmove(p, p + 1, (size_t)(P.npetals - i - 1) * sizeof *p);
            P.npetals--;
        }
    }
    for (int i = P.npellets - 1; i >= 0; i--) {
        Pellet *p = &P.pellets[i];
        p->age += dt;
        p->vx *= damp, p->vy *= damp;
        p->x += p->vx * dt + P.wind[0] * dt * 0.5, p->y += p->vy * dt + P.wind[1] * dt * 0.5;
        if (p->age > 45) {
            memmove(p, p + 1, (size_t)(P.npellets - i - 1) * sizeof *p);
            P.npellets--;
        }
    }
    /* fireflies at night */
    int nf = cfg.fireflies ? (int)lround(36 * P.look.fly) : 0;
    while (P.nflies < nf) {
        Fly *f = &P.flies[P.nflies++];
        *f = (Fly){ rnd() * P.aspect, rnd(), 0, 0, rnd() * 20, 0.6 + rnd() * 0.8, 0 };
    }
    for (int i = P.nflies - 1; i >= 0; i--) {
        Fly *f = &P.flies[i];
        bool keep = i < nf;
        f->fade = clampd(f->fade + (keep ? dt : -dt) * 0.8, 0, 1);
        if (!keep && f->fade <= 0) {
            memmove(f, f + 1, (size_t)(P.nflies - i - 1) * sizeof *f);
            P.nflies--;
            continue;
        }
        f->vx += (rnd() - 0.5) * dt * 0.06, f->vy += (rnd() - 0.5) * dt * 0.06;
        f->vx *= exp(-dt * 0.6), f->vy *= exp(-dt * 0.6);
        if (f->x < 0.05) f->vx += dt * 0.02;
        if (f->x > P.aspect - 0.05) f->vx -= dt * 0.02;
        if (f->y < 0.05) f->vy += dt * 0.02;
        if (f->y > 0.95) f->vy -= dt * 0.02;
        f->x += f->vx * dt, f->y += f->vy * dt, f->ph += dt * f->rate;
    }
}

static void scatter_food(double x, double y) {
    int n = 5 + (int)floor(rnd() * 4);
    for (int i = 0; i < n && P.npellets < MAX_PELLETS; i++) {
        double a = rnd() * TAU, d = rnd() * 0.035;
        P.pellets[P.npellets++] = (Pellet){ x + cos(a) * d, y + sin(a) * d, cos(a) * 0.01, sin(a) * 0.01, 0, rnd() };
    }
    drop(x, y, 0.028, -1.4);
    for (int i = 0; i < P.nkoi; i++) {   /* the splash startles the koi right under it */
        Koi *k = &P.koi[i];
        double dx = k->x - x, dy = k->y - y, d = hypot(dx, dy);
        if (d < 0.07 && d > 1e-4) k->flee = 0.8, k->fx = dx / d, k->fy = dy / d;
    }
}

static void update(double dt) {
    P.clock += dt;
    Look goal = target_look();
    P.look = P.have_look ? mix_look(&P.look, &goal, 1 - exp(-dt * 1.2)) : goal;
    P.have_look = true;
    int rain = (int)clampd(cfg.rain, 0, 3);
    P.overcast += (OVERCAST[rain] - P.overcast) * (1 - exp(-dt * 0.5));
    P.autumn += (cfg.drift - P.autumn) * (1 - exp(-dt * 0.5));
    P.rain_acc += RAIN_RATE[rain] * dt;
    while (P.rain_acc >= 1) P.rain_acc -= 1, drop(rnd() * P.aspect, rnd(), 0.0065, -(0.3 + rnd() * 0.25));
    if (rain == 3) {   /* storm lightning: a double flicker */
        P.next_bolt -= dt;
        if (P.next_bolt <= 0) P.bolt = 0, P.next_bolt = 8 + rnd() * 14;
    }
    if (P.bolt >= 0) {
        P.bolt += dt;
        double b = P.bolt;
        P.flash = b < 0.08 ? 1 : b < 0.16 ? 0.2 : b < 0.26 ? 0.8 : fmax(0, 0.8 - (b - 0.26) * 2.5);
        if (b > 0.6) P.bolt = -1, P.flash = 0;
    }
    /* water striders and the odd fish rising: the pond is never quite still */
    P.idle_drop -= dt;
    if (P.idle_drop <= 0) P.idle_drop = 1.5 + rnd() * 4, drop(rnd() * P.aspect, rnd(), 0.009, -0.22);
    update_koi(dt);
    update_floats(dt);
}

/* ---- GPU ------------------------------------------------------------------------- */

typedef struct SimParams { float aspect, damp; int32_t drops; float pad; float drop[32][4]; } SimParams;
typedef struct BaseParams { float aspect, pad, seed[2]; } BaseParams;
typedef struct FloorParams {
    float sim_texel[2], aspect, time, caus, amb, fog, nk;
    float sun[3], pad0, deep[3], pad1, shallow[3], pad2;
} FloorParams;
typedef struct WaterParams {
    float sim_texel[2], sun_pos[2], aspect, time, nk, refr, refl, sun_i, moon, bio;
    float night, flash, exposure, trees, autumn, overcast, amb, pad0;
    float top[3], pad1, hor[3], pad2, sun[3], pad3, l[3], pad4;
} WaterParams;
typedef struct KoiParams {
    float aspect, shadow, shadow_off[2], shadow_k, time, caus, amb, sun[3], fog, deep[3], pad;
} KoiParams;
typedef struct FloatParams {
    float aspect, shadow, nk, shadow_k, shadow_off[2], sim_texel[2], sun[3], amb, l[3], pad;
} FloatParams;
typedef struct KoiVert { float pos_uv[4], info[4]; } KoiVert;
typedef struct Inst { float a[4], b[4]; } Inst;

static struct { BareShader sim, base, floor, water, koi, flt; } fx;
static BareImage sim[2], base_img, scene, display;
static int W, H, SW, SH, simW, simH, built_quality = -1;
static KoiVert koi_data[MAX_KOI * KOI_VERTS];
static Inst inst[MAX_INST];

static void free_images(void) {
    BareImage *all[] = { &sim[0], &sim[1], &base_img, &scene, &display };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++)
        if (*all[i]) bare_image_free(*all[i]), *all[i] = 0;
}

static void resize(int w, int h) {
    W = w, H = h;
    double old = P.aspect;
    P.aspect = (double)W / H;
    if (fabs(old - P.aspect) > 1e-3) {
        make_pads();
        for (int i = 0; i < P.nkoi; i++) P.koi[i].x = clampd(P.koi[i].x, 0.05, P.aspect - 0.05);
    }
    int q = (int)clampd(cfg.quality, 0, 3);
    built_quality = cfg.quality;
    double s = QUALITY[q].scale;
    if ((double)W * H * s * s > QUALITY[q].max_px) s = sqrt(QUALITY[q].max_px / ((double)W * H));
    SW = (int)fmax(1, lround(W * s)), SH = (int)fmax(1, lround(H * s));
    simH = QUALITY[q].sim, simW = (int)fmax(1, lround(simH * P.aspect));
    free_images();
    for (int i = 0; i < 2; i++) sim[i] = bare_canvas(.width = simW, .height = simH, .format = BARE_RG16F, .bottom_up = true);
    base_img = bare_canvas(.width = SW, .height = SH, .bottom_up = true);
    scene = bare_canvas(.width = SW, .height = SH, .bottom_up = true);
    display = bare_canvas(.width = W, .height = H, .bottom_up = true);
    BaseParams bp = { (float)P.aspect, 0, { 13.7f, 4.2f } };   /* the floor's fixed part */
    bare_pass(.target = base_img, .shader = fx.base, BARE_PARAMS(bp));
}

static void step_sim(double dt) {
    double hz = 120.0 * simH / 360;
    P.sim_acc += dt * hz;
    int steps = (int)floor(P.sim_acc);
    P.sim_acc -= steps;
    if (steps > 8) steps = 8, P.sim_acc = 0;
    double rmin = 2.3 / simH;
    for (int i = 0; i < steps; i++) {
        static SimParams sp;
        sp.aspect = (float)P.aspect;
        sp.damp = (float)pow(0.992, 120 / hz);
        int n = P.ndrops < 32 ? P.ndrops : 32;
        for (int j = 0; j < n; j++) {
            memcpy(sp.drop[j], P.drops[j], sizeof sp.drop[j]);
            sp.drop[j][2] = (float)fmax(rmin, P.drops[j][2]);
        }
        memmove(P.drops, P.drops + n, (size_t)(P.ndrops - n) * sizeof P.drops[0]);
        P.ndrops -= n;
        sp.drops = n;
        bare_pass(.target = sim[1], .shader = fx.sim, .inputs = { sim[0] }, BARE_PARAMS(sp));
        BareImage t = sim[0];
        sim[0] = sim[1], sim[1] = t;
    }
}

static void put_koi(int *o, double x, double y, double u, double v, const Koi *k, int part) {
    KoiVert *d = &koi_data[(*o)++];
    d->pos_uv[0] = (float)x, d->pos_uv[1] = (float)y, d->pos_uv[2] = (float)u, d->pos_uv[3] = (float)v;
    d->info[0] = (float)k->seed, d->info[1] = (float)k->variety, d->info[2] = (float)k->depth, d->info[3] = (float)part;
}

static void quad(int *o, double rx, double ry, double dx, double dy, double len, double w, const Koi *k, int part) {
    double nx = -dy * w, ny = dx * w, ex = rx + dx * len, ey = ry + dy * len;
    put_koi(o, rx - nx, ry - ny, 0, -1, k, part), put_koi(o, rx + nx, ry + ny, 0, 1, k, part);
    put_koi(o, ex - nx, ey - ny, 1, -1, k, part), put_koi(o, ex - nx, ey - ny, 1, -1, k, part);
    put_koi(o, rx + nx, ry + ny, 0, 1, k, part), put_koi(o, ex + nx, ey + ny, 1, 1, k, part);
}

static int by_depth(const void *a, const void *b) {   /* deepest first */
    double da = (*(Koi *const *)a)->depth, db = (*(Koi *const *)b)->depth;
    return da < db ? 1 : da > db ? -1 : 0;
}

static int build_koi(void) {
    Koi *sorted[MAX_KOI];   /* not const: MSVC warns passing an array of const pointers to qsort */
    for (int i = 0; i < P.nkoi; i++) sorted[i] = &P.koi[i];
    qsort(sorted, (size_t)P.nkoi, sizeof sorted[0], by_depth);
    int o = 0;
    double px[MS], py[MS], tx[MS], ty[MS], hw[MS];
    for (int ki = 0; ki < P.nkoi; ki++) {
        const Koi *k = sorted[ki];
        double L = koi_len(k), sc = 1 - 0.12 * k->depth;
        const double *s = k->spine;
        double amp = (0.045 + 0.05 * k->burst + 0.02 * fmin(1, fabs(k->turn_s))) * L;
        for (int m = 0; m < MS; m++) {
            double u = pow((double)m / (MS - 1), 1.25);
            double f = u * (NS - 1);
            int i = (int)fmin(NS - 2, floor(f));
            double t = f - i;
            double ax = s[i * 2], ay = s[i * 2 + 1], bx = s[i * 2 + 2], by = s[i * 2 + 3];
            double dx = ax - bx, dy = ay - by, dl = hypot(dx, dy);
            if (dl == 0) dl = 1;
            dx /= dl, dy /= dl;
            double lat = sin(k->phase - u * 5.5) * amp * (0.2 + 0.8 * u * u);
            px[m] = lerp(ax, bx, t) - dy * lat, py[m] = lerp(ay, by, t) + dx * lat;
            tx[m] = dx, ty[m] = dy;
            double pr = u < 0.3 ? sqrt(fmax(0, 1 - pow((0.3 - u) / 0.3, 2))) : 1 - 0.8 * smooth(0.3, 1, u);
            hw[m] = L * 0.105 * sc * pr;
        }
        /* pectoral fins (under the body) */
        int fm = 6;
        double flap = 0.95 + 0.35 * sin(P.clock * 2.2 + k->seed * 10) - 0.25 * k->burst;
        for (int side = -1; side <= 1; side += 2) {
            double nx = -ty[fm] * side, ny = tx[fm] * side;
            double dx = -tx[fm] * cos(flap) + nx * sin(flap), dy = -ty[fm] * cos(flap) + ny * sin(flap);
            quad(&o, px[fm] + nx * hw[fm] * 0.6, py[fm] + ny * hw[fm] * 0.6, dx, dy, 0.17 * L * sc, 0.075 * L * sc, k, 2);
        }
        /* tail */
        int e = MS - 1;
        double sw = sin(k->phase - 6.1) * 0.5, bdx = -tx[e], bdy = -ty[e];
        double cdx = bdx * cos(sw) - bdy * sin(sw), cdy = bdx * sin(sw) + bdy * cos(sw);
        quad(&o, px[e] - bdx * L * 0.02, py[e] - bdy * L * 0.02, cdx, cdy, 0.3 * L * sc, 0.2 * L * sc, k, 1);
        /* body */
        for (int m = 0; m < MS - 1; m++) {
            double u0 = pow((double)m / (MS - 1), 1.25), u1 = pow((double)(m + 1) / (MS - 1), 1.25);
            int n = m + 1;
            double l0x = px[m] + ty[m] * hw[m], l0y = py[m] - tx[m] * hw[m], r0x = px[m] - ty[m] * hw[m], r0y = py[m] + tx[m] * hw[m];
            double l1x = px[n] + ty[n] * hw[n], l1y = py[n] - tx[n] * hw[n], r1x = px[n] - ty[n] * hw[n], r1y = py[n] + tx[n] * hw[n];
            put_koi(&o, l0x, l0y, u0, -1, k, 0), put_koi(&o, r0x, r0y, u0, 1, k, 0), put_koi(&o, l1x, l1y, u1, -1, k, 0);
            put_koi(&o, l1x, l1y, u1, -1, k, 0), put_koi(&o, r0x, r0y, u0, 1, k, 0), put_koi(&o, r1x, r1y, u1, 1, k, 0);
        }
    }
    return o;
}

static void put_inst(int *n, double x, double y, double rot, double s, int type, double seed, double a, double ex) {
    if (*n >= MAX_INST) return;
    Inst *d = &inst[(*n)++];
    d->a[0] = (float)x, d->a[1] = (float)y, d->a[2] = (float)rot, d->a[3] = (float)s;
    d->b[0] = (float)type, d->b[1] = (float)seed, d->b[2] = (float)a, d->b[3] = (float)ex;
}

static void build_floats(int *n_shadow, int *n_inst) {
    int n = 0;
    for (int i = 0; i < P.npads; i++) {
        const Pad *p = &P.pads[i];
        put_inst(&n, p->x, p->y, p->rot, p->r, 0, p->seed, 1, p->hue);
    }
    for (int i = 0; i < P.npads; i++) {
        const Pad *p = &P.pads[i];
        if (p->fseed * 100 < cfg.flowers)
            put_inst(&n, p->x + cos(p->rot + 2) * p->r * 0.25, p->y + sin(p->rot + 2) * p->r * 0.25, p->rot * 1.7,
                     p->r * 0.5, 1, p->seed, 1, p->fcol);
    }
    for (int i = 0; i < P.npetals; i++) {
        const Petal *p = &P.petals[i];
        double land = smooth(0, 0.5, p->age), fade = 1 - smooth(p->life - 8, p->life, p->age);
        put_inst(&n, p->x, p->y, p->rot, (p->leaf ? 0.016 : 0.008) * p->sz * (1 + 0.8 * (1 - land)), p->leaf ? 3 : 2,
                 p->rot, land * fade, p->col);
    }
    for (int i = 0; i < P.npellets; i++) {
        const Pellet *p = &P.pellets[i];
        put_inst(&n, p->x, p->y, 0, 0.0045, 4, p->seed, 1 - smooth(40, 45, p->age), 0);
    }
    *n_shadow = n;
    for (int i = 0; i < P.nflies; i++) {
        const Fly *f = &P.flies[i];
        double blink = pow(fmax(0, sin(f->ph * 2.1)), 3) * 0.85 + 0.08;
        put_inst(&n, f->x, f->y, 0, 0.06, 5, 0, blink * f->fade * P.look.fly * 1.6, 0);
    }
    *n_inst = n;
}

static void set3(float *o, const double *v) { o[0] = (float)v[0], o[1] = (float)v[1], o[2] = (float)v[2]; }

static void draw(double dt) {
    const Look *T = &P.look;
    int ti = (int)clampd(cfg.tint, 0, 3);
    double clar = clampd(cfg.clarity, 0, 100) / 100;
    double fog = lerp(3.6, 0.8, clar);
    double sun_i = T->sun_i, amb = T->amb * (1 - 0.25 * P.overcast) * (1 + P.flash * 0.5);
    double top[3], hor[3];
    for (int i = 0; i < 3; i++) {
        top[i] = lerp(T->top[i], (T->top[0] + T->top[1] + T->top[2]) / 3 * 0.9, P.overcast);
        hor[i] = lerp(T->hor[i], (T->hor[0] + T->hor[1] + T->hor[2]) / 3 * 0.95, P.overcast);
    }
    double Lx = (T->sun_pos[0] - 0.5) * 1.4, Ly = (T->sun_pos[1] - 0.5) * 1.4, Lz = 0.75;
    double ll = sqrt(Lx * Lx + Ly * Ly + Lz * Lz);
    Lx /= ll, Ly /= ll, Lz /= ll;
    double sh_off[2] = { -Lx / Lz * 0.035, -Ly / Lz * 0.035 };
    double sh_k = 0.34 * sun_i * (1 - P.overcast * 0.7) * (0.3 + 0.7 * T->amb);
    double caus = T->caus * (1 - P.overcast * 0.75);
    double nk = 2.6 * simH / 360;
    float sim_texel[2] = { 1.0f / simW, 1.0f / simH };

    step_sim(dt);

    /* 1. the pond floor, shadows and koi into the scene buffer */
    FloorParams fp = { { sim_texel[0], sim_texel[1] }, (float)P.aspect, (float)P.clock, (float)caus, (float)amb,
                       (float)fog, (float)nk, { 0 }, 0, { 0 }, 0, { 0 }, 0 };
    set3(fp.sun, T->sun);
    memcpy(fp.deep, TINTS[ti][0], sizeof fp.deep);
    memcpy(fp.shallow, TINTS[ti][1], sizeof fp.shallow);
    bare_pass(.target = scene, .shader = fx.floor, .inputs = { base_img, sim[0] }, BARE_PARAMS(fp));

    int n_shadow, n_inst;
    build_floats(&n_shadow, &n_inst);
    FloatParams flp = { (float)P.aspect, 1, (float)nk, (float)(sh_k * 1.1), { (float)(sh_off[0] * 0.8), (float)(sh_off[1] * 0.8) },
                        { sim_texel[0], sim_texel[1] }, { 0 }, (float)amb, { (float)Lx, (float)Ly, (float)Lz }, 0 };
    set3(flp.sun, T->sun);
    if (n_shadow && sh_k > 0.01)
        bare_draw(.target = scene, .shader = fx.flt, .inputs = { sim[0] }, .vertex_count = 6,
                  BARE_INSTANCES(inst, n_shadow), .blend = BARE_BLEND_ALPHA, BARE_PARAMS(flp));
    int nkv = build_koi();
    if (nkv) {
        KoiParams kp = { (float)P.aspect, 0, { (float)sh_off[0], (float)sh_off[1] }, (float)sh_k, (float)P.clock,
                         (float)caus, (float)amb, { 0 }, (float)fog, { 0 }, 0 };
        set3(kp.sun, T->sun);
        memcpy(kp.deep, TINTS[ti][0], sizeof kp.deep);
        if (sh_k > 0.01) {
            kp.shadow = 1;
            bare_draw(.target = scene, .shader = fx.koi, BARE_VERTICES(koi_data, nkv), .blend = BARE_BLEND_ALPHA,
                      BARE_PARAMS(kp));
        }
        kp.shadow = 0;
        bare_draw(.target = scene, .shader = fx.koi, BARE_VERTICES(koi_data, nkv), .blend = BARE_BLEND_ALPHA,
                  BARE_PARAMS(kp));
    }

    /* 2. the water surface at full resolution */
    WaterParams wp;
    memset(&wp, 0, sizeof wp);
    wp.sim_texel[0] = sim_texel[0], wp.sim_texel[1] = sim_texel[1];
    wp.sun_pos[0] = (float)T->sun_pos[0], wp.sun_pos[1] = (float)T->sun_pos[1];
    wp.aspect = (float)P.aspect, wp.time = (float)P.clock, wp.nk = (float)nk, wp.refr = 0.03f;
    wp.refl = (float)(T->refl * (1 + P.overcast * 0.3)), wp.sun_i = (float)sun_i, wp.moon = (float)T->moon;
    wp.bio = (float)(cfg.glow ? T->bio : 0), wp.night = (float)T->night, wp.flash = (float)P.flash;
    wp.exposure = (float)(clampd(cfg.brightness, 10, 400) / 100), wp.trees = cfg.trees ? 1.f : 0.f;
    wp.autumn = (float)P.autumn, wp.overcast = (float)P.overcast, wp.amb = (float)amb;
    set3(wp.top, top), set3(wp.hor, hor), set3(wp.sun, T->sun);
    wp.l[0] = (float)Lx, wp.l[1] = (float)Ly, wp.l[2] = (float)Lz;
    bare_pass(.target = display, .shader = fx.water, .inputs = { scene, sim[0] }, BARE_PARAMS(wp));

    /* 3. what floats on it, then the fireflies */
    flp.shadow = 0;
    if (n_shadow)
        bare_draw(.target = display, .shader = fx.flt, .inputs = { sim[0] }, .vertex_count = 6,
                  BARE_INSTANCES(inst, n_shadow), .blend = BARE_BLEND_ALPHA, BARE_PARAMS(flp));
    if (n_inst > n_shadow)
        bare_draw(.target = display, .shader = fx.flt, .inputs = { sim[0] }, .vertex_count = 6,
                  BARE_INSTANCES(inst + n_shadow, n_inst - n_shadow), .blend = BARE_BLEND_ADD, BARE_PARAMS(flp));
}

/* ---- the app -------------------------------------------------------------------- */

int main(int argc, char **argv) {
    srand((unsigned)time(NULL));
    SpArgs sa = sp_args(argc, argv);
    if (!bare_window_with(.width = 1280, .height = 720, .title = "Koi Pond", .parent = sa.parent)) return 1;
    bare_background(BARE_RGBA(11, 38, 34, 255));
    fx.sim = bare_shader(&sim_shader);
    fx.base = bare_shader(&base_shader);
    fx.floor = bare_shader(&floor_shader);
    fx.water = bare_shader(&water_shader);
    fx.koi = bare_shader(&koi_shader);
    fx.flt = bare_shader(&float_shader);

    P.aspect = 16.0 / 9, P.wind_a = rnd() * 6.28, P.next_bolt = 6, P.bolt = -1, P.idle_drop = 2;
    P.mouse.x = P.mouse.y = -1, P.mouse.last = -100;
    bool started = false, paused = false, was_pressed = false;
    double prev = -1, last_draw = -1e9;
    BareVec2 last_mouse = { -1e9f, -1e9f };

    while (bare_frame()) {
        SpMessage msg;
        while (sp_next(&msg)) {
            if (msg.kind == SP_PROPERTY) set_property(&msg);
            else if (msg.kind == SP_PAUSE) paused = sp_bool(&msg);
        }
        double now = bare_time();
        BareVec2 win = bare_window_size();
        float scale = bare_pixel_scale();
        int w = (int)clampd(floor(win.x * scale + 0.5), 1, 16384), h = (int)clampd(floor(win.y * scale + 0.5), 1, 16384);
        if (win.x >= 1 && win.y >= 1 && (w != W || h != H || cfg.quality != built_quality)) resize(w, h);
        if (!started) {   /* the settings have arrived: the pond fills, and doesn't start bare */
            P.autumn = cfg.drift, P.overcast = OVERCAST[(int)clampd(cfg.rain, 0, 3)];
            want_koi = want_lilies = true;
            sync_population();
            int n = (int)lround(60 * pow(cfg.petals / 100, 1.5));
            for (int i = 0; i < n && P.npetals < MAX_PETALS; i++) new_petal(&P.petals[P.npetals++], true, 1 + rnd() * 30);
            for (int i = 0; i < 3; i++) drop(0.2 + rnd() * (P.aspect - 0.4), 0.2 + rnd() * 0.6, 0.03, -1.2);
            started = true;
        }
        sync_population();

        BareState page = bare_box(.width = BARE_GROW, .height = BARE_GROW, .image = display,
                                  .color = BARE_RGBA(11, 38, 34, 255));
        if (paused) {   /* the picture stays as it is; nothing runs until Screen+ resumes it */
            bare_continuous(false);
            prev = -1;
            continue;
        }

        /* input, in pond units (y up), as the page's listeners */
        BareVec2 m = bare_mouse();
        if ((m.x != last_mouse.x || m.y != last_mouse.y) && page.hovered && last_mouse.x > -1e8f && win.y > 0) {
            double x = m.x / win.y, y = 1 - m.y / win.y;
            if (P.mouse.inside && cfg.cursor_ripples) {
                double d = hypot(x - P.mouse.x, y - P.mouse.y);
                if (d < 0.3) {
                    int n = (int)floor(d / 0.012);
                    for (int i = 1; i <= n; i++) {
                        double t = (double)i / n;
                        drop(lerp(P.mouse.x, x, t), lerp(P.mouse.y, y, t), 0.012, -0.16);
                    }
                }
            }
            if (!P.mouse.inside || hypot(x - P.mouse.x, y - P.mouse.y) >= 0.012 || !cfg.cursor_ripples)
                P.mouse.x = x, P.mouse.y = y;
            P.mouse.inside = true, P.mouse.last = P.clock;
        }
        last_mouse = m;
        if (!page.hovered) P.mouse.inside = false;
        if (page.pressed && !was_pressed && win.y > 0) scatter_food(m.x / win.y, 1 - m.y / win.y);
        was_pressed = page.pressed;

        double interval = cfg.fps == 0 ? 1 / 30.0 : cfg.fps == 1 ? 1 / 60.0 : 0;
        double since = now - last_draw;
        if (!interval || since >= interval - 0.0015) {
            last_draw = interval ? now - fmod(since, interval) : now;
            double dt = prev < 0 ? 0 : fmin(0.1, fmax(0, now - prev));
            prev = now;
            update(dt);
            draw(dt);
        }
        bare_continuous(!interval);
        if (interval) bare_wake_in((float)fmax(0, last_draw + interval - 0.0015 - now));
    }
    bare_quit();
}
