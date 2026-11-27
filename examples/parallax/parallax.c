/* parallax.c: a paper-theatre sea at night (a lighthouse, waves, clouds and
 * birds hung on ropes, coloured lights) whose layers move apart with the mouse;
 * Screen+'s "Parallax.js" wallpaper.
 *
 * A port of the parallax.js demo page (Matthew Wagerfield and Claudio
 * Guglieri, MIT, see LICENSE.txt): its CSS laid out here as Blink lays it out,
 * breakpoints included (min-width 400, 600, 900 and 1200 px), its keyframe
 * animations run here (waves circling, boards swinging, the lighthouse
 * rocking, the lights flickering) and parallax.js's mouse mode: every layer
 * offset against the pointer by its depth, smoothed by friction. Each box is
 * one draw of shaders/sprite.glsl.
 *
 * A Screen+ app wallpaper; it has no settings.
 * While Screen+ has it paused it draws nothing. */
#include "bare.h"
#include "parallax_shaders.h"
#include "../screenplus/screenplus_wallpaper.h"
#include "../screenplus/mips.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846

/* ---- pictures --------------------------------------------------------------------- */

enum { BACKGROUND, BIRDS, CLOUD1, CLOUD2, CLOUD3, CLOUD4, ORANGE, PURPLE, LIGHTHOUSE, ROPE, PAINT, PLAIN, NPICS };
static const char *const FILES[NPICS] = { "background.jpg", "board-birds.png", "board-cloud-1.png", "board-cloud-2.png",
                                          "board-cloud-3.png", "board-cloud-4.png", "light-orange.png", "light-purple.png",
                                          "lighthouse.png", "rope.png", "wave-paint.png", "wave-plain.png" };
static Mips pics[NPICS];
static int pic_w[NPICS], pic_h[NPICS];

static void load_pictures(void) {
    for (int i = 0; i < NPICS; i++) {
        FILE *f = fopen(sp_asset(FILES[i]), "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *bytes = n > 0 ? malloc((size_t)n) : NULL, *rgba = NULL;
        if (bytes && fread(bytes, 1, (size_t)n, f) == (size_t)n) rgba = bare_image_decode(bytes, (int)n, &pic_w[i], &pic_h[i]);
        free(bytes);
        fclose(f);
        if (!rgba) continue;
        for (int p = 0; p < pic_w[i] * pic_h[i]; p++)   /* premultiplied, as the compositor filters */
            for (int k = 0; k < 3; k++) rgba[p * 4 + k] = (unsigned char)((rgba[p * 4 + k] * rgba[p * 4 + 3] + 127) / 255);
        pics[i] = make_mips(rgba, pic_w[i], pic_h[i]);
        free(rgba);
    }
}

/* ---- 2D transforms, as CSS matrix(a, b, c, d, e, f) ------------------------------- */

typedef struct M { double a, b, c, d, e, f; } M;
static M mul(M p, M q) {
    return (M){ p.a * q.a + p.c * q.b, p.b * q.a + p.d * q.b, p.a * q.c + p.c * q.d, p.b * q.c + p.d * q.d,
                p.a * q.e + p.c * q.f + p.e, p.b * q.e + p.d * q.f + p.f };
}
static M tr(double x, double y) { return (M){ 1, 0, 0, 1, x, y }; }
static M sc(double s) { return (M){ s, 0, 0, s, 0, 0 }; }
static M rot(double deg) {   /* rotateZ: clockwise on screen */
    double r = deg * PI / 180;
    return (M){ cos(r), sin(r), -sin(r), cos(r), 0, 0 };
}
static M about(double ox, double oy, M t) { return mul(mul(tr(ox, oy), t), tr(-ox, -oy)); }   /* transform-origin */

/* ---- drawing ------------------------------------------------------------------------ */

typedef struct Params {
    float m[4], t[4], img[4], opt[4], color[4], sizes[4], lv[MAX_LEVELS][4];
} Params;

static BareShader sprite;
static BareImage canvas, blank;
static int cw, ch;
static bool first;

/* A box (w x h) under transform x, painted with picture pic at (ix, iy, iw, ih) in it (pic -1: a colour,
 * -2: the colour fading out downwards). */
static void box(M x, double w, double h, int pic, double ix, double iy, double iw, double ih, bool repeat, double opacity,
                const float rgba[4]) {
    bool fade = pic == -2;
    Params p;
    memset(&p, 0, sizeof p);
    p.m[0] = (float)x.a, p.m[1] = (float)x.b, p.m[2] = (float)x.c, p.m[3] = (float)x.d;
    p.t[0] = (float)x.e, p.t[1] = (float)x.f, p.t[2] = (float)w, p.t[3] = (float)h;
    p.img[0] = (float)ix, p.img[1] = (float)iy, p.img[2] = (float)iw, p.img[3] = (float)ih;
    p.opt[0] = (float)opacity, p.opt[1] = repeat ? 1.f : 0.f;
    p.sizes[2] = (float)cw, p.sizes[3] = (float)ch;
    BareImage in = blank;
    if (pic < 0 || !pics[pic].levels) {
        p.opt[2] = fade ? 2.f : 1.f;
        if (rgba) for (int k = 0; k < 4; k++) p.color[k] = k < 3 ? rgba[k] * rgba[3] : rgba[3];
    } else {
        Mips *mp = &pics[pic];
        in = mp->atlas;
        p.opt[3] = (float)mp->levels;
        p.sizes[0] = (float)mp->aw, p.sizes[1] = (float)mp->ah;
        memcpy(p.lv, mp->lv, sizeof p.lv);
    }
    if (first)
        bare_draw(.target = canvas, .shader = sprite, .inputs = { in }, .vertex_count = 6, .blend = BARE_BLEND_OVER,
                  BARE_PARAMS(p), .clear = true, .clear_color = { 0, 0, 0, 1 });
    else
        bare_draw(.target = canvas, .shader = sprite, .inputs = { in }, .vertex_count = 6, .blend = BARE_BLEND_OVER,
                  BARE_PARAMS(p));
    first = false;
}

/* ---- the CSS ------------------------------------------------------------------------ */

/* cubic-bezier(0.455, 0.03, 0.515, 0.955), easeInOutSine */
static double ease(double x) {
    const double x1 = 0.455, y1 = 0.03, x2 = 0.515, y2 = 0.955;
    double lo = 0, hi = 1, t = x;
    for (int i = 0; i < 40; i++) {
        t = (lo + hi) / 2;
        double bx = 3 * (1 - t) * (1 - t) * t * x1 + 3 * (1 - t) * t * t * x2 + t * t * t;
        if (bx < x) lo = t; else hi = t;
    }
    return 3 * (1 - t) * (1 - t) * t * y1 + 3 * (1 - t) * t * t * y2 + t * t * t;
}

/* progress of an infinite `alternate` animation with that easing; -1 before its delay */
static double alternate(double t, double delay, double dur) {
    t -= delay;
    if (t < 0) return -1;
    double it = floor(t / dur), x = t / dur - it;
    return ease(fmod(it, 2) == 1 ? 1 - x : x);
}

static double phase(double t, double dur) {   /* the lights: 1 .4 .8 .4 1, linear */
    t -= 0.1;
    if (t < 0) return 1;
    static const double v[5] = { 1, 0.4, 0.8, 0.4, 1 };
    double x = (t / dur - floor(t / dur)) * 4;
    int i = (int)floor(x);
    if (i > 3) i = 3;
    return v[i] + (v[i + 1] - v[i]) * (x - i);
}

typedef struct Wave { double bottom, height, after; } Wave;
/* the breakpoints: base, min-width 400, 600, 900, 1200 */
static const double BG_BOTTOM[5] = { 96, 96, 96, 160, 160 };
static const double LH[5][3] = { { 128, 224, 64 }, { 128, 224, 64 }, { 192, 336, 128 }, { 208, 364, 128 }, { 224, 392, 128 } };
static const double LIGHT[5] = { 240, 240, 280, 320, 400 };
static const double LIGHT_BOTTOM[5][3] = { { 80, 112, 96 }, { 80, 112, 96 }, { 144, 192, 160 }, { 160, 208, 176 }, { 160, 208, 176 } };
enum { W30, W40, W50, W60, W80, W100 };
static const Wave WAVES[5][6] = {
    { { 100, 48, 112 }, { 80, 64, 96 }, { 60, 80, 80 }, { 40, 96, 64 }, { 0, 128, 32 }, { -40, 160, 0 } },
    { { 100, 48, 112 }, { 80, 64, 96 }, { 60, 80, 80 }, { 40, 96, 64 }, { 0, 128, 32 }, { -40, 160, 0 } },
    { { 150, 72, 168 }, { 120, 96, 144 }, { 90, 120, 120 }, { 60, 144, 96 }, { 0, 192, 48 }, { -60, 240, 0 } },
    { { 170, 82, 190.4 }, { 136, 109, 163.2 }, { 102, 136, 136 }, { 68, 163, 108.8 }, { -1, 218, 54.4 }, { -68, 272, 0 } },
    { { 180, 86, 201.6 }, { 144, 115, 172.8 }, { 108, 144, 144 }, { 72, 173, 115.2 }, { -1, 230, 57.6 }, { -72, 288, 0 } },
};
static const double WAVE_DUR[6] = { 7.11111, 6.66667, 6.22222, 5.77778, 4.88889, 4 };
enum { R10, R30, R60 };
static const double ROPES[5][3][2] = {   /* top (%), hanger scale */
    { { 20, 0.25 }, { 13.33333, 0.30556 }, { 3.33333, 0.38889 } },
    { { 10, 0.3 }, { 2.22222, 0.36667 }, { -9.44444, 0.46667 } },
    { { 20, 0.5 }, { 13.33333, 0.61111 }, { 3.33333, 0.77778 } },
    { { 10, 0.5 }, { 3.33333, 0.62222 }, { -6.66667, 0.80556 } },
    { { 5, 0.6 }, { -1.66667, 0.75556 }, { -11.66667, 0.98889 } },
};
static const double ROPE_SCALE[3] = { 1, 1.05556, 1.13889 };
static const double HANGER[10][2] = { { 0, 0 }, { 28, 10 }, { 46, 20 }, { 59, 30 }, { 66.5, 40 }, { 69.5, 50 }, { 66.5, 60 },
                                      { 59, 70 }, { 46, 80 }, { 28, 90 } };   /* position-n: top, left (%) */
static const double SWING[6] = { 0, 4, 3.5, 3, 2.5, 2 };

static double W, H, T;
static int K;

static void wave(M layer, int which, bool paint) {
    const Wave *wv = &WAVES[K][which];
    double h = wv->height, w = 3 * W, x = -W, y = H - wv->bottom - h;
    M m = mul(layer, tr(x, y));
    double t = T - 0.1;
    if (t >= 0) {   /* rotateZ(a) translate3d(0, 10%, 0) rotateZ(-a): round a circle */
        double a = 2 * PI * (t / WAVE_DUR[which] - floor(t / WAVE_DUR[which]));
        m = mul(m, tr(-sin(a) * 0.1 * h, cos(a) * 0.1 * h));
    }
    int pic = paint ? PAINT : PLAIN;
    double th = 1.01 * h, tw = pic_h[pic] ? (double)pic_w[pic] / pic_h[pic] * th : th;   /* background-size: auto 101% */
    box(m, w, h, pic, (w - tw) / 2, h - th, tw, th, true, 1, NULL);   /* center bottom, repeat-x */
    static const float black[4] = { 0, 0, 0, 1 };
    if (wv->after > 0) box(mul(m, tr(0, h)), w, wv->after, -1, 0, 0, 0, 0, false, 1, black);   /* :after */
}

static void light(M layer, int pic, double left, double bottom, double dur) {
    double s = LIGHT[K];
    box(mul(layer, tr(left * W - s / 2, H - bottom - s / 2)), s, s, pic, 0, 0, s, s, false, phase(T, dur), NULL);
}

typedef struct Hang { int position, pic, swing; } Hang;
static void rope(M layer, int which, const Hang *hangs, int n) {
    double w = 1.2 * W, h = pic_w[ROPE] ? w * pic_h[ROPE] / pic_w[ROPE] : 0.15 * W;
    M ul = mul(layer, tr(-0.1 * W, ROPES[K][which][0] / 100 * H));
    ul = mul(ul, about(w / 2, h / 2, sc(ROPE_SCALE[which])));
    box(ul, w, h, ROPE, 0, 0, w, h, false, 1, NULL);
    for (int i = 0; i < n; i++) {
        const Hang *g = &hangs[i];
        M m = mul(ul, tr(HANGER[g->position][1] / 100 * w, HANGER[g->position][0] / 100 * h));
        m = mul(m, sc(ROPES[K][which][1]));
        m = mul(m, tr(-140, -4));
        double p = alternate(T, 0, SWING[g->swing]);
        m = mul(m, about(140, 0, rot(10 - 20 * p)));
        box(m, 280, 280, g->pic, 0, 0, 280, 280, false, 1, NULL);
    }
}

static void lighthouse(M layer) {
    double w = LH[K][0], h = LH[K][1], bottom = LH[K][2];
    M m = mul(layer, tr(0.8 * W - w / 2, H - bottom - h));
    double p = alternate(T, 0.1, 4);
    if (p >= 0) m = mul(m, about(0.5 * w, 0.9 * h, mul(tr((0.15 - 0.3 * p) * w, 0), rot(10 - 20 * p))));
    box(m, w, h, LIGHTHOUSE, 0, 0, w, h, false, 1, NULL);
}

int main(int argc, char **argv) {
    SpArgs sp = sp_args(argc, argv);
    if (!bare_window_with(.width = sp.width ? sp.width : 1280, .height = sp.height ? sp.height : 720, .title = "Parallax.js", .parent = sp.parent)) return 1;
    bare_background(BARE_BLACK);
    sprite = bare_shader(&sprite_shader);
    static const unsigned char px[4] = { 0, 0, 0, 0 };
    blank = bare_image_from_pixels(1, 1, px);
    load_pictures();
    bool paused = false, moved = false;
    double clock = 0, last = -1, vx = 0, vy = 0;
    BareVec2 last_mouse = { -1e9f, -1e9f };

    while (bare_frame()) {
        SpMessage m;
        while (sp_next(&m))
            if (m.kind == SP_PAUSE) paused = sp_bool(&m);
        double now = bare_time();
        BareVec2 win = bare_window_size();
        int w = (int)fmax(1, floor(win.x + 0.5)), h = (int)fmax(1, floor(win.y + 0.5));
        if (w != cw || h != ch) {
            if (canvas) bare_image_free(canvas);
            canvas = bare_canvas(.width = w, .height = h);
            cw = w, ch = h;
        }
        bare_box(.width = BARE_GROW, .height = BARE_GROW, .image = canvas);
        if (paused) { bare_continuous(false); last = -1; continue; }
        if (last >= 0) clock += now - last;
        last = now;

        /* parallax.js, mouse mode: the pointer against the middle of the window, scalars 25 and 15,
         * friction 0.1 a frame, layers moved the other way by their depth */
        BareVec2 mo = bare_mouse();
        if (mo.x != last_mouse.x || mo.y != last_mouse.y) {
            if (last_mouse.x > -1e8f) moved = true;
            last_mouse = mo;
        }
        double ix = moved ? (mo.x - w / 2.0) / (w / 2.0) : 0, iy = moved ? (mo.y - h / 2.0) / (h / 2.0) : 0;
        vx += (ix * w * 0.25 - vx) * 0.1;
        vy += (iy * h * 0.15 - vy) * 0.1;

        W = w, H = h, T = clock;
        K = w >= 1200 ? 4 : w >= 900 ? 3 : w >= 600 ? 2 : w >= 400 ? 1 : 0;
        first = true;
#define LAYER(depth) tr(-vx * (depth), -vy * (depth))
        {   /* the background: 110% wide from -5%, from -5% down to its bottom offset; cover, at 50% 100% */
            double bw = 1.1 * W, bh = 1.05 * H - BG_BOTTOM[K];
            double s = fmax(bw / pic_w[BACKGROUND], bh / pic_h[BACKGROUND]), iw = pic_w[BACKGROUND] * s, ih = pic_h[BACKGROUND] * s;
            box(mul(LAYER(0.1), tr(-0.05 * W, -0.05 * H)), bw, bh, BACKGROUND, (bw - iw) * 0.5, bh - ih, iw, ih, false, 1, NULL);
        }
        light(LAYER(0.1), ORANGE, 0.30, LIGHT_BOTTOM[K][0], 14);
        light(LAYER(0.1), PURPLE, 0.45, LIGHT_BOTTOM[K][1], 12);
        light(LAYER(0.1), ORANGE, 0.60, LIGHT_BOTTOM[K][2], 16);
        static const Hang r10[] = { { 2, CLOUD2, 1 }, { 4, CLOUD1, 3 }, { 8, BIRDS, 5 } };
        rope(LAYER(0.15), R10, r10, 3);
        static const Hang r30[] = { { 1, CLOUD1, 3 }, { 5, CLOUD4, 1 } };
        rope(LAYER(0.30), R30, r30, 2);
        wave(LAYER(0.30), W30, true);
        wave(LAYER(0.40), W40, false);
        wave(LAYER(0.50), W50, true);
        lighthouse(LAYER(0.60));
        static const Hang r60[] = { { 3, BIRDS, 5 }, { 6, CLOUD2, 2 }, { 8, CLOUD3, 4 } };
        rope(LAYER(0.60), R60, r60, 3);
        wave(LAYER(0.60), W60, false);
        wave(LAYER(0.80), W80, false);
        wave(LAYER(1.00), W100, true);
#undef LAYER
        {   /* #about is hidden, but not its :after: a shade over the top 5%, black 0.8 to transparent */
            static const float shade[4] = { 0, 0, 0, 0.8f };
            box(tr(0, 0), W, 0.05 * H, -2, 0, 0, 0, 0, false, 1, shade);
        }
        bare_continuous(true);
    }
    bare_quit();
}
