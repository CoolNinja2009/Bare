/* aurora.c: soft drifting light over a starry night; a Screen+ live wallpaper.
 *
 * A port of Screen+'s "Aurora" web wallpaper (four blurred radial gradients on
 * CSS keyframes and a canvas of twinkling stars), made interactive:
 *
 *   move   the light leans towards the cursor and gathers under it
 *   click  a shooting star
 *
 * Settings are the Customise panel (LivelyProperties.json beside the exe),
 * sent by Screen+ on stdin (see ../screenplus/screenplus_wallpaper.h); by
 * hand they're arguments: aurora palette=2 speed=200 stars=150
 *
 * One fragment pass draws everything at full resolution. It draws at the
 * chosen frame rate and not at all while Screen+ has it paused. */
#include "bare.h"
#include "aurora_shaders.h"
#include "../screenplus/screenplus_wallpaper.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PI 3.14159265358979

/* ---- settings ---------------------------------------------------------------- */

static struct {
    int palette, fps;
    double speed, glow, brightness, stars;
    bool twinkle, shooting, follow;
} cfg = { .palette = 0, .fps = 0, .speed = 100, .glow = 100, .brightness = 100, .stars = 100,
          .twinkle = true, .shooting = true, .follow = true };

static void set_property(const SpMessage *m) {
    const char *n = m->name;
    if (!strcmp(n, "palette")) cfg.palette = (int)sp_num(m);
    else if (!strcmp(n, "fps")) cfg.fps = (int)sp_num(m);
    else if (!strcmp(n, "speed")) cfg.speed = sp_num(m);
    else if (!strcmp(n, "glow")) cfg.glow = sp_num(m);
    else if (!strcmp(n, "brightness")) cfg.brightness = sp_num(m);
    else if (!strcmp(n, "stars")) cfg.stars = sp_num(m);
    else if (!strcmp(n, "twinkle")) cfg.twinkle = sp_bool(m);
    else if (!strcmp(n, "shooting")) cfg.shooting = sp_bool(m);
    else if (!strcmp(n, "follow")) cfg.follow = sp_bool(m);
}

/* ---- the look ------------------------------------------------------------------ */

typedef struct Palette { unsigned bg, blob[4]; } Palette;
static const Palette PALETTES[] = {
    { 0x070a18, { 0x2878ff, 0x783cff, 0x00c8aa, 0xff46a0 } },   /* Aurora: the page's colours */
    { 0x030b10, { 0x18d080, 0x7440f0, 0x10a8c0, 0x58f0a0 } },   /* Northern lights */
    { 0x120714, { 0xff7a30, 0xb03c90, 0xffb040, 0xff3c64 } },   /* Sunset */
    { 0x050a14, { 0x5ab0ff, 0x9cc8ff, 0x3c6cff, 0xd8ecff } },   /* Ice */
    { 0x0c0505, { 0xff4020, 0xff9020, 0xa01830, 0xffcc60 } },   /* Ember */
};
#define NPALETTES (int)(sizeof PALETTES / sizeof PALETTES[0])

/* The page's blobs, in vw (size, centre) and their keyframes: translate(vw, vh) scale, period (s), opacity. */
static const struct {
    double size, cx, cy;        /* cy in vw from the top, or from the bottom when from_bottom */
    bool from_bottom;
    double dx, dy, scale, period, opacity;
    bool reverse;               /* animation-direction: alternate-reverse */
} BLOBS[4] = {
    { 55, 22.5, 17.5, false, 18, 12, 1.15, 38, .55, false },
    { 50, 83, 19, false, -14, 18, .90, 46, .55, false },
    { 60, 55, 0, true, -10, -14, 1.10, 52, .55, false },
    { 35, 11.5, 5.5, true, 18, 12, 1.15, 60, .35, true },
};

/* CSS ease-in-out: cubic-bezier(.42, 0, .58, 1). */
static double ease(double x) {
    double t = x;
    for (int i = 0; i < 6; i++) {   /* Newton on the curve's x(t) */
        double u = 1 - t;
        double bx = 3 * u * u * t * .42 + 3 * u * t * t * .58 + t * t * t;
        double d = 3 * u * u * .42 + 6 * u * t * (.58 - .42) + 3 * t * t * (1 - .58);
        if (fabs(d) < 1e-6) break;
        t -= (bx - x) / d;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
    }
    double u = 1 - t;
    return 3 * u * t * t + t * t * t;
}

/* Where an infinite alternate animation is after `s` seconds: 0..1, eased. */
static double alternate(double s, double period, bool reverse) {
    double ph = fmod(s, 2 * period) / period;
    double p = ph < 1 ? ph : 2 - ph;
    return ease(reverse ? 1 - p : p);
}

static double rnd(void) { return rand() / (RAND_MAX + 1.0); }
static double approach(double v, double target, double dt, double seconds) { return v + (target - v) * (1 - exp(-dt / seconds)); }
static void rgb(unsigned c, float *out) {
    out[0] = ((c >> 16) & 255) / 255.f, out[1] = ((c >> 8) & 255) / 255.f, out[2] = (c & 255) / 255.f;
}

typedef struct Params {
    float res[4];
    float blob[4][4];
    float col[4][4];
    float bg[4];
    float stars[4];
    float cursor[4];
    float cursor_col[4];
    float shoot[4];
    float shoot_len[4];
} Params;

/* ---- the app -------------------------------------------------------------------- */

static struct {
    double drift, twinkle;          /* animation clocks: drift runs at the chosen speed */
    double mx, my, last_move;       /* the cursor (px), when it last moved (clock s) */
    double lean_x, lean_y, gather;  /* eased cursor effects */
    double shoot_at, shoot_x, shoot_y, shoot_dir, next_shoot;
    bool shooting;
} st;

static void shoot(double x, double y, double w, double h) {
    st.shooting = true;
    st.shoot_at = st.twinkle;
    st.shoot_x = x, st.shoot_y = y;
    /* downwards, away from the nearer side so it crosses the screen */
    double side = x < w / 2 ? 1 : -1;
    st.shoot_dir = side > 0 ? (.12 + .22 * rnd()) * PI : (1 - .12 - .22 * rnd()) * PI;
    (void)h;
}

int main(int argc, char **argv) {
    srand((unsigned)time(NULL));
    SpArgs sp = sp_args(argc, argv);
    if (!bare_window_with(.width = 1280, .height = 720, .title = "Aurora", .parent = sp.parent)) return 1;
    bare_background(BARE_BLACK);
    BareShader shader = bare_shader(&aurora_shader);
    BareImage canvas = 0;
    int cw = 0, ch = 0;
    bool paused = false, was_pressed = false;
    double last_draw = -1e9;
    BareVec2 last_mouse = { -1e9f, -1e9f };
    st.next_shoot = 8 + 20 * rnd();
    st.mx = st.my = -1;
    st.last_move = -1e9;

    while (bare_frame()) {
        SpMessage m;
        while (sp_next(&m)) {
            if (m.kind == SP_PROPERTY) set_property(&m);
            else if (m.kind == SP_PAUSE) paused = sp_bool(&m);
        }
        double now = bare_time();
        BareVec2 win = bare_window_size();
        float scale = bare_pixel_scale();
        int w = (int)fmax(1, floor(win.x * scale + .5)), h = (int)fmax(1, floor(win.y * scale + .5));
        if (w != cw || h != ch) {
            if (canvas) bare_image_free(canvas);
            canvas = bare_canvas(.width = w, .height = h);
            cw = w, ch = h, last_draw = -1e9;
        }
        BareState page = bare_box(.width = BARE_GROW, .height = BARE_GROW, .image = canvas, .color = BARE_BLACK);

        if (paused) {   /* the picture stays as it is; nothing runs until Screen+ resumes it */
            bare_continuous(false);
            continue;
        }

        /* input: the desktop's mouse, forwarded by Screen+ */
        BareVec2 mo = bare_mouse();
        if (mo.x != last_mouse.x || mo.y != last_mouse.y) {
            st.mx = mo.x * scale, st.my = mo.y * scale;
            st.last_move = st.twinkle;
        }
        last_mouse = mo;
        if (page.pressed && !was_pressed && cfg.shooting) shoot(mo.x * scale, mo.y * scale, w, h);
        was_pressed = page.pressed;

        double interval = cfg.fps == 0 ? 1 / 30.0 : cfg.fps == 1 ? 1 / 60.0 : 0;
        double since = now - last_draw;
        if (interval && since < interval - .0015) {
            bare_continuous(false);
            bare_wake_in((float)(interval - .0015 - since));
            continue;
        }
        last_draw = interval ? now - fmod(since, interval) : now;
        double step = fmin(.1, since);

        st.drift += step * cfg.speed / 100;
        st.twinkle += step;

        /* the cursor: lean towards it while it's moving about, fade back when it rests */
        bool active = cfg.follow && st.mx >= 0 && st.twinkle - st.last_move < 6;
        double cx = w / 2.0, cy = h / 2.0;
        st.lean_x = approach(st.lean_x, active ? (st.mx - cx) / w : 0, step, 1.6);
        st.lean_y = approach(st.lean_y, active ? (st.my - cy) / h : 0, step, 1.6);
        st.gather = approach(st.gather, active ? 1 : 0, step, active ? .5 : 2.5);

        /* shooting stars: now and then, or on a click */
        if (cfg.shooting && !st.shooting && st.twinkle >= st.next_shoot) {
            shoot(w * (.1 + .8 * rnd()), h * (.05 + .3 * rnd()), w, h);
            st.next_shoot = st.twinkle + 12 + 26 * rnd();
        }
        double life = 1.3, age = st.twinkle - st.shoot_at;
        if (st.shooting && age > life) st.shooting = false;

        const Palette *pal = &PALETTES[cfg.palette < 0 || cfg.palette >= NPALETTES ? 0 : cfg.palette];
        double vw = w / 100.0, vh = h / 100.0;
        Params p = { 0 };
        p.res[0] = (float)w, p.res[1] = (float)h, p.res[2] = scale, p.res[3] = (float)st.twinkle;
        for (int i = 0; i < 4; i++) {
            double e = alternate(st.drift, BLOBS[i].period, BLOBS[i].reverse);
            double s = BLOBS[i].size * vw * (1 + (BLOBS[i].scale - 1) * e);
            double bx = BLOBS[i].cx * vw + BLOBS[i].dx * vw * e;
            double by = (BLOBS[i].from_bottom ? h - BLOBS[i].cy * vw : BLOBS[i].cy * vw) + BLOBS[i].dy * vh * e;
            double k = .05 + .035 * i;   /* nearer blobs lean further: a little depth */
            bx += st.lean_x * w * k, by += st.lean_y * h * k;
            p.blob[i][0] = (float)bx, p.blob[i][1] = (float)by;
            p.blob[i][2] = (float)(s * .495);   /* farthest-corner gradient, transparent at 70% */
            p.blob[i][3] = (float)fmin(1, BLOBS[i].opacity * cfg.glow / 100);
            rgb(pal->blob[i], p.col[i]);
            p.col[i][3] = 80 * scale;           /* the page's blur(80px) */
        }
        rgb(pal->bg, p.bg);
        p.bg[3] = (float)(cfg.brightness / 100);
        double cell = 94.9 * scale / sqrt(fmax(cfg.stars / 100, .01));   /* the page's one star per 9000 px² */
        p.stars[0] = (float)fmax(4, cell);
        p.stars[1] = cfg.stars > 0 ? 1.f : 0.f;
        p.stars[2] = cfg.twinkle ? 1.f : 0.f;
        p.stars[3] = 1;
        p.cursor[0] = (float)st.mx, p.cursor[1] = (float)st.my;
        p.cursor[2] = (float)st.gather, p.cursor[3] = (float)(18 * vw);
        float c0[3], c1[3];
        rgb(pal->blob[0], c0), rgb(pal->blob[2], c1);
        for (int i = 0; i < 3; i++) p.cursor_col[i] = (c0[i] + c1[i]) * .5f;
        if (st.shooting) {
            double speed = .9 * fmax(w, h), dist = age * speed;
            p.shoot[0] = (float)(st.shoot_x + cos(st.shoot_dir) * dist);
            p.shoot[1] = (float)(st.shoot_y + sin(st.shoot_dir) * dist);
            p.shoot[2] = (float)st.shoot_dir;
            p.shoot[3] = (float)sin(PI * fmin(1, age / life));
            p.shoot_len[0] = (float)(.16 * fmax(w, h) * fmin(1, age * 3));
            p.shoot_len[1] = 1.3f * scale;
        }
        bare_pass(.target = canvas, .shader = shader, BARE_PARAMS(p));

        bare_continuous(!interval);
        if (interval) bare_wake_in((float)fmax(0, last_draw + interval - .0015 - now));
    }
    bare_quit();
}
