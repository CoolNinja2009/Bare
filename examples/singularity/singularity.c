/* singularity.c: a Schwarzschild black hole, ray traced in real time.
 *
 * A port of Screen+'s "Singularity" web wallpaper (samples/Singularity/index.html),
 * made to give the same picture pixel for pixel. Every pixel follows a photon
 * backwards along its curved path, collecting light from the accretion disk,
 * the jets and anything falling in; whatever escapes picks up the nebula and
 * the star field. The far side of the disk arches over the shadow, stars smear
 * into Einstein rings, and the side of the disk rushing towards you burns
 * brighter and bluer.
 *
 *   move   the camera drifts with the cursor, and the cursor is a small gravitational lens
 *   click  drops a star in: it spirals down, is torn into a streak and lights the disk up
 *   wheel  zoom
 *
 * Settings are the page's Lively properties, given as arguments:
 *   singularity quality=2 palette=3 bodies=3 cursorLens=false
 *
 * Four passes a frame, as in the page: the ray tracer draws into a smaller HDR
 * buffer and, at the same time, a 32-bit buffer of how far each ray was bent
 * (two targets, like WebGL's drawBuffers); a bloom chain halves it down and
 * adds it back up; the composite pass upscales the trace, draws the stars
 * where the bent rays point, adds bloom and tone maps at full resolution.
 *
 * The simulation below is the page's script in doubles, in the same order of
 * operations, so the GPU gets the same float32 uniforms. Math.hypot is V8's
 * algorithm (see len3). With SINGULARITY_SEED set, Math.random is replaced by
 * mulberry32 with that seed: a page whose Math.random is the same generator,
 * driven by the same clock (BARE_FIXED_DT), shows the same frames.
 *
 * Not ported: music reaction (the page gets audio levels from Lively; Bare
 * has no audio input), prefers-reduced-motion, and pausing when the host hides
 * the wallpaper (Bare already stops drawing while minimised). */
#include "bare.h"
#include "singularity_shaders.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PI 3.141592653589793

/* ---- settings (the page's cfg and its Lively properties) -------------------- */

static struct {
    int quality, fps, position, palette, bodies;
    double size, tilt, spin, brightness, nebula;
    bool parallax, cursor_lens, audio;
} cfg = { .quality = 1, .fps = 1, .position = 1, .palette = 0, .bodies = 2,
          .size = 100, .tilt = 10, .spin = 100, .brightness = 100, .nebula = 60,
          .parallax = true, .cursor_lens = true, .audio = true };

/* "name=value", as livelyPropertyListener takes them. */
static void set_property(const char *arg) {
    static const struct { const char *name; char kind; void *at; } props[] = {
        { "quality", 'i', &cfg.quality }, { "fps", 'i', &cfg.fps }, { "position", 'i', &cfg.position },
        { "palette", 'i', &cfg.palette }, { "bodies", 'i', &cfg.bodies }, { "size", 'd', &cfg.size },
        { "tilt", 'd', &cfg.tilt }, { "spin", 'd', &cfg.spin }, { "brightness", 'd', &cfg.brightness },
        { "nebula", 'd', &cfg.nebula }, { "parallax", 'b', &cfg.parallax },
        { "cursorLens", 'b', &cfg.cursor_lens }, { "audio", 'b', &cfg.audio },
    };
    const char *eq = strchr(arg, '=');
    if (!eq) return;
    for (size_t i = 0; i < sizeof props / sizeof props[0]; i++) {
        if (strlen(props[i].name) != (size_t)(eq - arg) || strncmp(arg, props[i].name, (size_t)(eq - arg))) continue;
        const char *v = eq + 1;
        char *end;
        double d = strtod(v, &end);
        bool number = end != v && *end == 0;
        if (props[i].kind == 'b') *(bool *)props[i].at = !strcmp(v, "true") || (number && d == 1);
        else if (number && props[i].kind == 'i') *(int *)props[i].at = (int)d;
        else if (number) *(double *)props[i].at = d;
    }
}

/* ---- constants --------------------------------------------------------------- */

/* The ray-traced buffer: share of the screen, pixel budget, max steps, step length factor. */
static const struct { double scale, max_px; int steps; double k; } QUALITY[4] = {
    { 0.5, 0.9e6, 110, 0.11 }, { 0.67, 2.4e6, 150, 0.085 }, { 0.85, 5e6, 200, 0.068 }, { 1.0, 12e6, 280, 0.052 },
};
/* Inner-edge temperature (K), glow tint, iridescent. */
static const struct { double t0, tint[3], spectral; } PALETTES[4] = {
    { 4300, { 1.0, 0.55, 0.22 }, 0 },    /* Ember */
    { 11000, { 0.55, 0.72, 1.0 }, 0 },   /* Blue giant */
    { 2500, { 1.0, 0.28, 0.12 }, 0 },    /* Crimson */
    { 6500, { 0.75, 0.55, 1.0 }, 1 },    /* Spectral */
};
#define FOCAL 1.5
#define R_IN 2.6
#define R_OUT 11.0
#define GM 60.0
#define FLOW_PERIOD 16.0
#define R_EAT 1.3     /* where pieces vanish */
#define TIDE 4.5      /* how much the tide is exaggerated */
enum { FRAGS = 12, MAX_BODIES = 4, MAX_LEVELS = 10 };
static const double BODY_COLORS[4][3] = {
    { 0.75, 0.85, 1.0 }, { 1.0, 0.95, 0.88 }, { 1.0, 0.82, 0.55 }, { 1.0, 0.58, 0.32 },
};
static const double BODY_EVERY[4][2] = { { 0, 0 }, { 80, 160 }, { 25, 55 }, { 8, 18 } };   /* never ... often */

/* ---- maths, as the page's helpers ------------------------------------------- */

typedef struct V3 { double e[3]; } V3;

static V3 v3(double x, double y, double z) { return (V3){ { x, y, z } }; }
static V3 add(V3 a, V3 b) { return v3(a.e[0] + b.e[0], a.e[1] + b.e[1], a.e[2] + b.e[2]); }
static V3 mul(V3 a, double s) { return v3(a.e[0] * s, a.e[1] * s, a.e[2] * s); }
static double dot(V3 a, V3 b) { return a.e[0] * b.e[0] + a.e[1] * b.e[1] + a.e[2] * b.e[2]; }
static V3 cross(V3 a, V3 b) {
    return v3(a.e[1] * b.e[2] - a.e[2] * b.e[1], a.e[2] * b.e[0] - a.e[0] * b.e[2], a.e[0] * b.e[1] - a.e[1] * b.e[0]);
}
/* Math.hypot as V8 computes it (scaled by the largest, Kahan-summed), so
 * lengths agree with the page to the last bit. */
static double hypot_v8(const double *v, int n) {
    double max = 0;
    for (int i = 0; i < n; i++) if (fabs(v[i]) > max) max = fabs(v[i]);
    if (max == 0) return 0;
    double sum = 0, compensation = 0;
    for (int i = 0; i < n; i++) {
        double x = fabs(v[i]) / max, summand = x * x - compensation, preliminary = sum + summand;
        compensation = (preliminary - sum) - summand;
        sum = preliminary;
    }
    return sqrt(sum) * max;
}
static double len(V3 a) { return hypot_v8(a.e, 3); }
static V3 norm(V3 a) {
    double l = len(a);
    return mul(a, 1 / (l != 0 ? l : 1));
}
static double clampd(double x, double a, double b) { return fmin(b, fmax(a, x)); }
static double ease(double dt, double rate) { return 1 - exp(-dt * rate); }
static double smooth(double a, double b, double x) {
    double t = clampd((x - a) / (b - a), 0, 1);
    return t * t * (3 - 2 * t);
}

/* Math.random: the system's generator, or mulberry32 when seeded for tests. */
static uint32_t seed;
static bool seeded;
static double random01(void) {
    if (!seeded) return (double)rand() / ((double)RAND_MAX + 1);
    uint32_t a = seed += 0x6D2B79F5u;
    uint32_t t = (a ^ (a >> 15)) * (1u | a);
    t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
    return (double)(t ^ (t >> 14)) / 4294967296.0;
}

/* ---- GPU resources ------------------------------------------------------------ */

static struct { BareShader scene, down, up, comp; } fx;
static BareImage scene_c, scene_d, display;
static struct { BareImage image; int w, h; } levels[MAX_LEVELS];
static int level_count;
static int W, H, SW, SH, built_quality = -1;   /* screen and traced buffer, physical pixels */

/* std140 blocks of the shaders. */
typedef struct SceneParams {
    float cam_rot[3][4];
    float cam_pos[3], focal;
    float phase[4];
    float tint[3], t0;
    float res[2], shift[2];
    float flow_w[2], shock[2];
    float step_k, esc_r, spectral, disk_gain;
    float jets, jet_phase, nebula, baa;
    int32_t steps, bodies, pad_[2];
    float frag[MAX_BODIES * FRAGS][4];
    float body_a[MAX_BODIES][4];
    float body_b[MAX_BODIES][4];
} SceneParams;

typedef struct CompParams {
    float cam_rot[3][4];
    float lens[3], focal;
    float wave[4];
    float res[2], hole[2];
    float scene_size[2], shift[2];
    float px_ang, star_s2, exposure, bloom_k;
} CompParams;

static void resize(int w, int h) {
    W = w;
    H = h;
    int q = (int)clampd(cfg.quality, 0, 3);
    double s = fmin(QUALITY[q].scale, sqrt(QUALITY[q].max_px / ((double)W * H)));
    SW = (int)fmax(8, floor(W * s + 0.5));
    SH = (int)fmax(8, floor(H * s + 0.5));
    built_quality = cfg.quality;

    if (scene_c) bare_image_free(scene_c), bare_image_free(scene_d), bare_image_free(display);
    for (int i = 0; i < level_count; i++) bare_image_free(levels[i].image);
    scene_c = bare_canvas(.width = SW, .height = SH, .format = BARE_RGBA16F, .bottom_up = true);
    scene_d = bare_canvas(.width = SW, .height = SH, .format = BARE_RGBA32F, .bottom_up = true);   /* stars exact to the pixel at 8K */
    display = bare_canvas(.width = W, .height = H, .nearest = true, .bottom_up = true);
    level_count = 0;
    for (int lw = SW >> 1, lh = SH >> 1; level_count < MAX_LEVELS && lw >= 8 && lh >= 8; lw >>= 1, lh >>= 1) {
        levels[level_count].image = bare_canvas(.width = lw, .height = lh, .format = BARE_RGBA16F,
                                                 .bottom_up = true);
        levels[level_count].w = lw;
        levels[level_count++].h = lh;
    }
}

/* ---- simulation state --------------------------------------------------------- */

static double clock_s, disk_time, zoom = 1, zoom_target = 1, lens, audio, flare;
static bool shock_on;
static double shock_r, shock_a, wave_age = -1, last_drop = -1, next_body;
static struct { double x, y; } cam;
static struct { double x, y, nx, ny, moved; bool inside; } mouse = { -1, -1, 0, 0, -1e9, false };
static double inner_w, inner_h;   /* the window in logical pixels (innerWidth, innerHeight) */

typedef struct Frag { double o, j[2]; V3 p, v; bool alive; } Frag;
typedef struct Body {
    V3 c, v;
    double size, age, free, len0;
    Frag frags[FRAGS];
    const double *color;
    int eaten;
} Body;
static Body bodies[MAX_BODIES];
static int body_count;

static void remove_body(int i) {
    memmove(&bodies[i], &bodies[i + 1], (size_t)(body_count - i - 1) * sizeof bodies[0]);
    body_count--;
}

/* ---- camera and the hole's place on screen (shared by the renderer and clicks) */

typedef struct Camera { V3 pos, f, r, u; double D; } Camera;

static Camera camera(void) {
    double yaw = cam.x * 0.24 + 0.04 * sin(clock_s * 0.043);
    double pitch = clampd(cfg.tilt * PI / 180 + cam.y * 0.12 + 0.015 * sin(clock_s * 0.029), -1.3, 1.3);
    double D = 31 * (100 / clampd(cfg.size, 30, 300)) * zoom;
    double cp = cos(pitch);
    Camera c;
    c.pos = v3(D * cp * sin(yaw), D * sin(pitch), -D * cp * cos(yaw));
    c.f = norm(mul(c.pos, -1));
    c.r = norm(cross(v3(0, 1, 0), c.f));
    c.u = cross(c.f, c.r);
    c.D = D;
    return c;
}

static double hole_shift(void) {
    int p = cfg.position;
    double k = p == 0 ? -1 : p == 2 ? 1 : 0;
    return k * 0.2 * ((double)W / H);
}

/* ---- large bodies, and their spaghettification ---------------------------------
 * A body is a rubble pile: FRAGS pieces in a chain. Far out it falls as one;
 * inside its tidal radius every piece falls on its own orbit, the tide
 * (exaggerated a little) pulls the near end away from the far end, and the pile
 * stretches into a stream that winds into the hole. */

static double tidal_radius(const Body *b) { return 6.5 + 6 * b->size; }

static void spawn(V3 p, double rp, double size) {
    double r0 = len(p);
    V3 ph = mul(p, 1 / r0);
    /* orbital plane: mostly turning with the disk, tilted at random */
    double ax0 = (random01() - 0.5) * 0.9;
    double ax1 = random01() < 0.75 ? -1 : 1;
    double ax2 = (random01() - 0.5) * 0.9;
    V3 ax = v3(ax0, ax1, ax2);
    ax = add(ax, mul(ph, -dot(ax, ph)));
    if (len(ax) < 1e-3) ax = cross(ph, fabs(ph.e[0]) < 0.9 ? v3(1, 0, 0) : v3(0, 0, 1));
    V3 th = norm(cross(norm(ax), ph));
    /* a near-parabolic fall that would swing past the hole at rp */
    double v = sqrt(2 * GM / r0) * 0.97, vt = fmin(sqrt(2 * GM * rp) / r0, v * 0.95);
    Body b;
    memset(&b, 0, sizeof b);
    for (int i = 0; i < FRAGS; i++) {
        Frag *f = &b.frags[i];
        f->o = (double)i / (FRAGS - 1) * 2 - 1;
        f->j[0] = random01() - 0.5;
        f->j[1] = random01() - 0.5;
        f->p = p;
        f->alive = true;
    }
    if (body_count >= MAX_BODIES) remove_body(0);
    b.c = p;
    b.v = add(mul(ph, -sqrt(v * v - vt * vt)), mul(th, vt));
    b.size = size;
    b.color = BODY_COLORS[(int)floor(random01() * 4)];
    b.free = -1;
    b.len0 = 1;
    bodies[body_count++] = b;
}

static double chain_length(const Body *b) {
    double L = 0;
    for (int i = 1; i < FRAGS; i++) {
        const Frag *f0 = &b->frags[i - 1], *f1 = &b->frags[i];
        if (f0->alive && f1->alive) L += len(add(f1->p, mul(f0->p, -1)));
    }
    return L;
}

/* While whole, the pile lies along the line to the hole, drawn out more as the tide grows. */
static void lay_pile(Body *b) {
    double r = len(b->c);
    V3 n = mul(b->c, 1 / r);
    V3 e1 = cross(n, b->v);
    e1 = len(e1) > 1e-6 ? norm(e1) : norm(cross(n, fabs(n.e[1]) < 0.9 ? v3(0, 1, 0) : v3(1, 0, 0)));
    V3 e2 = cross(n, e1);
    double s = b->size * (0.55 + 0.9 * fmin(1, pow(tidal_radius(b) / r, 3)));
    for (int i = 0; i < FRAGS; i++) {
        Frag *f = &b->frags[i];
        f->p = add(add(b->c, mul(n, f->o * s)),
                   add(mul(e1, f->j[0] * b->size * 0.25), mul(e2, f->j[1] * b->size * 0.25)));
    }
}

static void drop(double client_x, double client_y) {
    if (clock_s - last_drop < 0.25) return;
    last_drop = clock_s;
    Camera c = camera();
    double px = (client_x - inner_w / 2) / inner_h - hole_shift();
    double py = (inner_h / 2 - client_y) / inner_h - 0;
    V3 d = norm(add(add(mul(c.r, px), mul(c.u, py)), mul(c.f, FOCAL)));
    V3 p = v3(0, 0, 0);
    bool found = false;
    if (fabs(d.e[1]) > 1e-4) {   /* where the click meets the disk plane */
        double t = -c.pos.e[1] / d.e[1];
        V3 hp = add(c.pos, mul(d, t));
        double xz[2] = { hp.e[0], hp.e[2] };
        double r = hypot_v8(xz, 2);
        double sign = c.pos.e[1] != 0 ? (c.pos.e[1] > 0 ? 1 : -1) : 1;
        if (t > 0 && r > 3.6 && r < 22) p = v3(hp.e[0], 0.3 * sign, hp.e[2]), found = true;
    }
    if (!found) {   /* otherwise the point of the click ray nearest the hole */
        V3 q = add(c.pos, mul(d, fmax(0, -dot(c.pos, d))));
        double r = len(q);
        p = r > 1e-3 ? mul(q, clampd(r, 4.5, 18) / r) : v3(6, 0.5, 0);
    }
    double rp = 1.5 + random01() * 1.5;
    double size = 0.14 + random01() * 0.1;
    spawn(p, rp, size);
}

static void eat(Body *b) {   /* one piece crossed the horizon */
    b->eaten++;
    flare = fmin(flare + 0.075 * (0.6 + 1.6 * b->size), 2.4);
    if (b->eaten == 1) shock_on = true, shock_r = R_IN, shock_a = fmin(1, 0.5 + b->size);
    if (b->eaten == 4) wave_age = 0;
}

static void step_bodies(double dt) {
    for (int i = body_count - 1; i >= 0; i--) {
        Body *b = &bodies[i];
        b->age += dt;
        if (b->free < 0) {   /* still whole: it falls as one */
            for (double left = dt; left > 1e-6;) {
                double r = len(b->c), h = fmin(left, 0.004 + 0.003 * r * sqrt(r)), k = -GM / (r * r * r);
                for (int j = 0; j < 3; j++) { b->v.e[j] += b->c.e[j] * k * h; b->c.e[j] += b->v.e[j] * h; }
                left -= h;
            }
            lay_pile(b);
            if (len(b->c) < tidal_radius(b)) {   /* the tide wins: every piece on its own from here */
                b->free = 0;
                b->len0 = fmax(chain_length(b), 1e-3);
                for (int k = 0; k < FRAGS; k++) b->frags[k].v = mul(b->v, 1 - 0.1 * b->frags[k].o);   /* the near end already pulls ahead */
            }
            if (b->age > 50) remove_body(i);
            continue;
        }
        b->free += dt;
        /* Gravity at the pile's middle: the tide is each piece's difference from
         * it, exaggerated for the first moments so the stretching reads clearly. */
        V3 m = v3(0, 0, 0);
        int n = 0;
        for (int k = 0; k < FRAGS; k++) if (b->frags[k].alive) m = add(m, b->frags[k].p), n++;
        m = mul(m, 1 / fmax(n, 1));
        double rm = fmax(len(m), 1e-3);
        V3 am = mul(m, -GM / (pow(fmax(rm, 4), 2) * rm));
        double tide = (TIDE - 1) * fmax(0, 1 - b->free / 4);
        bool any = false;
        for (int k = 0; k < FRAGS; k++) {
            Frag *f = &b->frags[k];
            if (!f->alive) continue;
            for (double left = dt; left > 1e-6;) {
                double r = len(f->p);
                if (r < R_EAT) break;
                double h = fmin(left, 0.004 + 0.003 * r * sqrt(r)), kk = -GM / (r * r * r);
                double drag = 0.06 + 0.2 * smooth(5, 2, r);   /* debris that skims the hole loses its orbit and winds in */
                for (int j = 0; j < 3; j++) {
                    double g = f->p.e[j] * kk;
                    f->v.e[j] += (g + (g - am.e[j]) * tide - f->v.e[j] * drag) * h;
                    f->p.e[j] += f->v.e[j] * h;
                }
                left -= h;
            }
            double r = len(f->p);
            if (r < R_EAT) f->alive = false, eat(b);
            else if (r > 34) f->alive = false;   /* flung out of the scene */
        }
        for (int k = 0; k < FRAGS; k++) any |= b->frags[k].alive;
        if (b->age > 50 || !any) remove_body(i);
    }
}

/* Fragments, bounding spheres, colours and tube radii for the shader. */
static int pack_bodies(SceneParams *sp, double D) {
    double sig_min = 1.2 * D / (FOCAL * SH);   /* never thinner than a traced pixel, so streams stay smooth */
    int nb = 0;
    memset(sp->frag, 0, sizeof sp->frag);
    for (int bi = 0; bi < body_count; bi++) {
        const Body *b = &bodies[bi];
        V3 m = v3(0, 0, 0);
        int n = 0;
        double r_min = 1e9, R = 0;
        for (int k = 0; k < FRAGS; k++) {
            if (!b->frags[k].alive) continue;
            m = add(m, b->frags[k].p);
            n++;
            r_min = fmin(r_min, len(b->frags[k].p));
        }
        if (!n) continue;
        m = mul(m, 1.0 / n);
        for (int k = 0; k < FRAGS; k++)
            if (b->frags[k].alive) R = fmax(R, len(add(b->frags[k].p, mul(m, -1))));
        /* spaghettification: the longer the stream, the thinner it gets */
        double sig0 = 0.5 * b->size;
        double sig = b->free < 0 ? sig0 : sig0 * pow(b->len0 / fmax(chain_length(b), b->len0), 0.75);
        double tube = fmax(sig, sig_min);
        double fade = fmin(1, b->age / 0.6) * clampd((50 - b->age) / 3, 0, 1) * sig / tube;
        for (int k = 0; k < FRAGS; k++) {
            const Frag *f = &b->frags[k];
            if (!f->alive) continue;
            double r = len(f->p);
            double w = fade * (0.7 + 2.2 * clampd((7 - r) / 5.5, 0, 1)) * (1 - smooth(26, 34, r));   /* heats up as it nears the hole */
            float *o = sp->frag[nb * FRAGS + k];
            o[0] = (float)f->p.e[0], o[1] = (float)f->p.e[1], o[2] = (float)f->p.e[2], o[3] = (float)w;
        }
        double hot = 0.5 * clampd((6 - r_min) / 4.5, 0, 1);
        const double *c = b->color;
        float *a = sp->body_a[nb], *bb = sp->body_b[nb];
        a[0] = (float)m.e[0], a[1] = (float)m.e[1], a[2] = (float)m.e[2], a[3] = (float)R;
        bb[0] = (float)(c[0] + (1 - c[0]) * hot);
        bb[1] = (float)(c[1] + (0.93 - c[1]) * hot);
        bb[2] = (float)(c[2] + (0.86 - c[2]) * hot);
        bb[3] = (float)tube;
        if (++nb == MAX_BODIES) break;
    }
    return nb;
}

/* ---- update and draw ---------------------------------------------------------- */

static void update(double dt) {
    clock_s += dt;
    disk_time += dt * clampd(cfg.spin, 0, 1000) / 100 * 1;
    bool follow = cfg.parallax && mouse.inside;
    cam.x += ((follow ? mouse.nx : 0) - cam.x) * ease(dt, 1.4);
    cam.y += ((follow ? mouse.ny : 0) - cam.y) * ease(dt, 1.4);
    zoom += (zoom_target - zoom) * ease(dt, 3);
    bool lens_on = cfg.cursor_lens && mouse.inside && clock_s - mouse.moved < 2.5;
    lens += ((lens_on ? 1 : 0) - lens) * ease(dt, lens_on ? 6 : 1.5);
    audio += (0 - audio) * ease(dt, 4);   /* no audio feed in Bare: stays at rest */
    step_bodies(dt);
    flare *= exp(-dt / 2.4);
    if (shock_on) {
        shock_r += dt * 3;
        shock_a *= exp(-dt / 1.8);
        if (shock_r > R_OUT + 2) shock_on = false;
    }
    if (wave_age >= 0 && (wave_age += dt) > 6) wave_age = -1;
    /* Now and then a large body wanders in from beyond the screen on an orbit that grazes the hole. */
    if (clock_s > next_body) {
        int k = (int)clampd(cfg.bodies, 0, 3);
        bool every = k > 0;
        double e0 = BODY_EVERY[k][0], e1 = BODY_EVERY[k][1];
        next_body = clock_s + (every ? e0 + random01() * (e1 - e0) : 5);
        if (every && body_count < 3) {
            double a = random01() * PI * 2, r0 = 21 + random01() * 5;
            double y = (random01() - 0.5) * 0.7;
            V3 p = mul(norm(v3(cos(a), y, sin(a))), r0);
            if (dot(norm(p), norm(camera().pos)) > 0.3) p = v3(-p.e[0], p.e[1], -p.e[2]);   /* not right in front of the camera */
            double rp = 1.8 + random01() * 1.6;
            double size = 0.28 + random01() * 0.32;
            spawn(p, rp, size);
        }
    }
}

/* The camera matrix (columns: right, up, forward) stored by rows, which the
 * shaders declare row_major: then M * v compiles to one dot product per
 * component, as ANGLE compiles WebGL's, and rounds the same. */
static void put_rot(float out[3][4], const Camera *c) {
    const V3 *cols[3] = { &c->r, &c->u, &c->f };
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) out[i][j] = (float)cols[j]->e[i];
        out[i][3] = 0;
    }
}

static void draw(void) {
    Camera c = camera();
    double sh = hole_shift();
    int qi = (int)clampd(cfg.quality, 0, 3), pi = (int)clampd(cfg.palette, 0, 3);
    double t1 = fmod(disk_time, FLOW_PERIOD), t2 = fmod(disk_time + FLOW_PERIOD / 2, FLOW_PERIOD);
    double w1 = 1 - fabs(2 * t1 / FLOW_PERIOD - 1);

    /* 1. trace the rays into the low-resolution HDR buffers */
    static SceneParams sp;
    put_rot(sp.cam_rot, &c);
    for (int i = 0; i < 3; i++) sp.cam_pos[i] = (float)c.pos.e[i], sp.tint[i] = (float)PALETTES[pi].tint[i];
    sp.focal = (float)FOCAL;
    sp.res[0] = (float)SW, sp.res[1] = (float)SH;
    sp.shift[0] = (float)sh, sp.shift[1] = 0;
    sp.steps = QUALITY[qi].steps;
    sp.step_k = (float)QUALITY[qi].k;
    sp.esc_r = (float)(c.D * 1.25 + 8);
    sp.baa = (float)(0.75 * c.D / (FOCAL * SH));
    sp.phase[0] = (float)t1, sp.phase[1] = (float)t2;
    sp.phase[2] = (float)fmod(floor(disk_time / FLOW_PERIOD), 64);
    sp.phase[3] = (float)fmod(floor(disk_time / FLOW_PERIOD + 0.5), 64);
    sp.flow_w[0] = (float)w1, sp.flow_w[1] = (float)(1 - w1);
    sp.t0 = (float)PALETTES[pi].t0;
    sp.spectral = (float)PALETTES[pi].spectral;
    sp.disk_gain = (float)((1 + 0.55 * flare) * (1 + 0.6 * audio));
    sp.shock[0] = (float)(shock_on ? shock_r : 0), sp.shock[1] = (float)(shock_on ? shock_a : 0);
    sp.jets = (float)(flare * 0.75);
    sp.jet_phase = (float)fmod(clock_s * 9, PI * 2000);
    sp.nebula = (float)(clampd(cfg.nebula, 0, 200) / 100);
    sp.bodies = pack_bodies(&sp, c.D);
    if (getenv("SINGULARITY_ANGLE_LAYOUT")) {   /* TEMPORARY: ANGLE's register per uniform */
        static float g[79][4];
        memset(g, 0, sizeof g);
        memcpy(g[0], sp.res, 8); memcpy(g[1], sp.cam_pos, 12);
        for (int i = 0; i < 3; i++) memcpy(g[2 + i], sp.cam_rot[i], 12);
        g[5][0] = sp.focal; memcpy(g[6], sp.shift, 8); memcpy(g[7], &sp.steps, 4);
        g[8][0] = sp.step_k; g[9][0] = sp.esc_r; memcpy(g[10], sp.phase, 16); memcpy(g[11], sp.flow_w, 8);
        g[12][0] = sp.t0; g[13][0] = sp.spectral; memcpy(g[14], sp.tint, 12); g[15][0] = sp.disk_gain;
        memcpy(g[16], sp.shock, 8); g[17][0] = sp.jets; g[18][0] = sp.jet_phase; g[19][0] = sp.nebula;
        g[20][0] = 1.0f; memcpy(g[21], &sp.bodies, 4);
        memcpy(g[22], sp.frag, sizeof sp.frag); memcpy(g[70], sp.body_a, sizeof sp.body_a);
        memcpy(g[74], sp.body_b, sizeof sp.body_b); g[78][0] = sp.baa;
        bare_pass(.target = scene_c, .extra_targets = { scene_d }, .shader = fx.scene, .params = g, .params_size = (int)sizeof g);
    } else
    bare_pass(.target = scene_c, .extra_targets = { scene_d }, .shader = fx.scene, BARE_PARAMS(sp));
    if (getenv("SINGULARITY_DUMP")) {   /* TEMPORARY */
        FILE *f = fopen(getenv("SINGULARITY_DUMP"), "wb");
        if (f) { fwrite(&sp, sizeof sp, 1, f); fclose(f); }
    }

    /* 2. bloom: progressive downsample, then additive upsample */
    BareImage src = scene_c;
    int sw = SW, shh = SH;
    for (int i = 0; i < level_count; i++) {
        float texel[2] = { (float)(1.0 / sw), (float)(1.0 / shh) };
        bare_pass(.target = levels[i].image, .shader = fx.down, .inputs = { src }, BARE_PARAMS(texel));
        src = levels[i].image, sw = levels[i].w, shh = levels[i].h;
    }
    for (int i = level_count - 1; i > 0; i--) {
        float texel[2] = { (float)(1.0 / levels[i].w), (float)(1.0 / levels[i].h) };
        bare_pass(.target = levels[i - 1].image, .shader = fx.up, .inputs = { levels[i].image },
                  .blend = BARE_BLEND_ADD, BARE_PARAMS(texel));
    }

    /* 3. full resolution: lensing by the cursor and the wave, stars, bloom, tone mapping */
    CompParams cp;
    memset(&cp, 0, sizeof cp);
    put_rot(cp.cam_rot, &c);
    cp.focal = (float)FOCAL;
    cp.res[0] = (float)W, cp.res[1] = (float)H;
    cp.scene_size[0] = (float)SW, cp.scene_size[1] = (float)SH;
    cp.shift[0] = (float)sh, cp.shift[1] = 0;
    cp.hole[0] = (float)(W / 2.0 + sh * H), cp.hole[1] = (float)(H / 2.0 + 0 * H);
    cp.lens[0] = (float)(mouse.x * W / inner_w);
    cp.lens[1] = (float)(H - mouse.y * H / inner_h);
    cp.lens[2] = (float)(0.037 * H * lens);
    double wave = wave_age >= 0 ? 0.015 * H * exp(-wave_age / 2) * fmin(1, wave_age * 4) : 0;
    cp.wave[0] = (float)(wave_age * 0.42 * (W > H ? W : H));
    cp.wave[1] = (float)wave, cp.wave[2] = (float)(0.07 * H), cp.wave[3] = (float)(0.05 * H);
    cp.px_ang = (float)(1 / (FOCAL * H));
    cp.star_s2 = (float)(0.6 * pow(fmax(1, H / 1080.0), 2));
    cp.exposure = (float)(clampd(cfg.brightness, 0, 400) / 100);
    cp.bloom_k = (float)(0.3 / fmax(1, level_count));
    bare_pass(.target = display, .shader = fx.comp,
              .inputs = { scene_c, scene_d, level_count ? levels[0].image : scene_c }, BARE_PARAMS(cp));
    const char *probe = getenv("SINGULARITY_PROBE");   /* TEMPORARY */
    if (probe) {
        static BareShader ps;
        if (!ps) ps = bare_shader(&probe_shader);
        float pp[4] = { (float)W, (float)H, (float)atoi(probe), 0 };
        bare_pass(.target = display, .shader = ps,
                  .inputs = { scene_c, scene_d, level_count ? levels[0].image : scene_c }, BARE_PARAMS(pp));
    }
}

/* ---- the app ------------------------------------------------------------------- */

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) set_property(argv[i]);
    const char *s = getenv("SINGULARITY_SEED");
    seeded = s && *s;
    if (seeded) seed = (uint32_t)strtoul(s, NULL, 10);
    else srand((unsigned)time(NULL));
    next_body = 6 + random01() * 6;   /* the page's first draw of Math.random */

    if (!bare_window(1280, 720, "Singularity")) return 1;
    bare_background(BARE_BLACK);
    fx.scene = bare_shader(&scene_shader);
    if (getenv("SINGULARITY_DXBC")) {   /* TEMPORARY: try another DXBC for the scene */
        static BareShaderCode alt;
        static unsigned char blob[1 << 20];
        FILE *f = fopen(getenv("SINGULARITY_DXBC"), "rb");
        alt = scene_shader;
        alt.dxbc = blob;
        alt.dxbc_size = (unsigned)fread(blob, 1, sizeof blob, f);
        fclose(f);
        fx.scene = bare_shader(&alt);
    }
    fx.down = bare_shader(&down_shader);
    fx.up = bare_shader(&up_shader);
    fx.comp = bare_shader(&comp_shader);
    if (getenv("SINGULARITY_COMP_DXBC")) {   /* TEMPORARY: try another DXBC for the composite */
        static BareShaderCode alt2;
        static unsigned char blob2[1 << 20];
        FILE *f = fopen(getenv("SINGULARITY_COMP_DXBC"), "rb");
        alt2 = comp_shader;
        alt2.dxbc = blob2;
        alt2.dxbc_size = (unsigned)fread(blob2, 1, sizeof blob2, f);
        fclose(f);
        fx.comp = bare_shader(&alt2);
    }

    double prev = -1, last_draw = -1e9;
    BareVec2 last_mouse = { -1e9, -1e9 };
    bool was_pressed = false;
    while (bare_frame()) {
        double now = bare_time() * 1000;
        BareVec2 win = bare_window_size();
        float scale = bare_pixel_scale();
        inner_w = win.x, inner_h = win.y;

        /* Buffers follow the window in physical pixels (the page's resize()). */
        int w = (int)clampd(floor(win.x * scale + 0.5), 1, 16384), h = (int)clampd(floor(win.y * scale + 0.5), 1, 16384);
        if (win.x >= 1 && win.y >= 1 && (w != W || h != H || cfg.quality != built_quality)) resize(w, h);

        /* The page: one element filling the window, showing the composite. */
        BareState page = bare_box(.width = BARE_GROW, .height = BARE_GROW, .image = display, .color = BARE_BLACK);

        /* Input, as the page's listeners see it. */
        BareVec2 m = bare_mouse();
        if ((m.x != last_mouse.x || m.y != last_mouse.y) && page.hovered) {   /* mousemove */
            mouse.x = m.x, mouse.y = m.y;
            mouse.inside = m.x >= 0 && m.y >= 0 && m.x < inner_w && m.y < inner_h;
            mouse.nx = clampd(m.x / inner_w * 2 - 1, -1, 1);
            mouse.ny = clampd(1 - m.y / inner_h * 2, -1, 1);
            mouse.moved = clock_s;
        }
        last_mouse = m;
        if (!page.hovered) mouse.inside = false;   /* mouseout */
        if (page.pressed && !was_pressed) drop(m.x, m.y);   /* mousedown */
        was_pressed = page.pressed;
        float wheel = bare_wheel().y;
        if (wheel != 0) zoom_target = clampd(zoom_target * exp(wheel * 0.0011), 0.55, 1.9);

        /* The page's frame-rate cap: 30 or 60 fps, or every refresh. Passes
         * called now still run before this frame is drawn. */
        double interval = cfg.fps == 0 ? 1000.0 / 30 : cfg.fps == 1 ? 1000.0 / 60 : 0;
        double since = now - last_draw;
        if (!interval || since >= interval - 1.5) {
            last_draw = interval ? now - fmod(since, interval) : now;
            update(prev < 0 ? 0 : fmin(0.1, fmax(0, (now - prev) / 1000)));
            prev = now;
            draw();
        }
        bare_continuous(!interval);
        if (interval) bare_wake_in((float)fmax(0, (last_draw + interval - 1.5 - now) / 1000));
    }
    bare_quit();
}
