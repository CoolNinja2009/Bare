/* ns_state.h: the frame (main.js's F), input, audio and system info (input.js),
 * and the modules' entry points. */
#ifndef NS_STATE_H
#define NS_STATE_H
#include "ns_config.h"
#include <time.h>

typedef struct Glitch {
    double rgb, scan, curve, grain, blocks, tear, vhs, mosh, melt, sort, bloom, vig, wave, pix, post, invert;
    double flash[4];
    double shuffle, ascii;
    int colorMode, mirror;
    double stat, bars, zoom, swirl, freeze, hue;
    bool banner_on;
    Str banner_text, banner_sub;
    bool banner_has_sub;
    double banner_a;
    Rgb banner_c;
    double hudShake, seedJump, lens;
} Glitch;

typedef struct Frame {
    CxCtx *ctx;
    double W, H, cssW, cssH, k, t, dt, speed;
    Pal P;
    double gi, rot, par, parY, mx, my, blink;
    int frame;
    double flash_a;
    Rgb flash_c;
    Glitch *G;
    double now_ms;   /* performance.now() */
} Frame;
static Frame F;

typedef struct Click { double x, y; } Click;
typedef struct Input {
    double x, y, sx, sy, vx, vy, speed;
    bool down, inside;
    double lastMove, idle;
    Click clicks[16];
    int nclicks;
    int keys[16];
    int nkeys;
} Input;
static Input I;

typedef struct Audio {
    float raw[128], spec[64];   /* Float32Array */
    double bass, mid, high, level, beat, avgBass, lastData;
    int beatCount;
    bool live, kick;
} Audio;
static Audio A;

typedef struct Sys {
    double cpu, gpu, ram, ramTotal, net;   /* NAN: not given */
    bool live;
    double fcpu, fgpu, fram;
} Sys;
static Sys SYS = { NAN, NAN, NAN, NAN, NAN, false, 30, 40, 60 };

/* modules */
static void flash(double a, Rgb c);
static void director_kick(double a);
static void saint_say(const Str *text);
static void saint_say_u8(const char *text);
static void hud_spawn_popup(double W, double Hh, const double *x, const double *y, const Str *text);
static void fx_shock(double x, double y, double s);
static void fx_ring(double x, double y, double p);
static void fx_burst(double x, double y, double power);
static void fx_worm(void);
static void fx_click(double x, double y);
static void glyphs_inject(const Str *word);
static void glyphs_burst(double x, double y, double power);

/* new Date(ms) fields, local or UTC */
static inline void ns_time(double ms, bool utc, struct tm *out) {
    time_t tt = (time_t)floor(ms / 1000);
#ifdef _WIN32
    if (utc) gmtime_s(out, &tt);
    else localtime_s(out, &tt);
#else
    if (utc) gmtime_r(&tt, out);
    else localtime_r(&tt, out);
#endif
}

/* ---------- input ---------- */
static inline void input_update(double dt) {
    double px = I.sx, py = I.sy;
    I.sx = damp(I.sx, I.x, 14, dt), I.sy = damp(I.sy, I.y, 14, dt);
    double ivx = (I.sx - px) / fmax(dt, 1e-3), ivy = (I.sy - py) / fmax(dt, 1e-3);
    I.vx = damp(I.vx, ivx, 10, dt), I.vy = damp(I.vy, ivy, 10, dt);
    I.speed = hypot(I.vx, I.vy);
    I.idle = (F.now_ms - I.lastMove) / 1000;
}

/* ---------- audio ---------- */
static inline void audio_update(double dt, double t) {
    bool fresh = F.now_ms - A.lastData < 1500;
    A.live = fresh && CB(audio);
    double gain = C(audioGain) / 100;
    for (int i = 0; i < 64; i++) {
        double v = 0;
        if (A.live) v = fmax(A.raw[i], A.raw[i + 64]) * gain;
        else if (CB(fakeBeat)) {
            double bt = fract(t * 2.0);
            v = (i < 8 ? exp(-bt * 8) * (1 - i / 10.0) : 0) + 0.15 * noise2(i * 0.3, t * 2) * (1 - i / 64.0);
        }
        A.spec[i] = (float)damp(A.spec[i], clampd(v, 0, 1.5), v > A.spec[i] ? 30 : 8, dt);
    }
    double b = 0, m = 0, h = 0;
    for (int i = 0; i < 6; i++) b += A.spec[i];
    for (int i = 6; i < 24; i++) m += A.spec[i];
    for (int i = 24; i < 64; i++) h += A.spec[i];
    b /= 6, m /= 18, h /= 40;
    A.bass = b, A.mid = m, A.high = h;
    A.level = damp(A.level, (b + m + h) / 3, 6, dt);
    A.avgBass = damp(A.avgBass, b, 1.5, dt);
    A.kick = false;
    if (b > 0.25 && b > A.avgBass * 1.45 && A.beat < 0.3) A.beat = 1, A.beatCount++, A.kick = true;
    A.beat = fmax(0, A.beat - dt * 3.5);
}

/* ---------- system info ---------- */
static inline void sys_update(double dt) {
    double g = gauss();
    SYS.fcpu = clampd(SYS.fcpu + g * 8 * dt * 10 * 0.2, 3, 99);
    g = gauss();
    SYS.fgpu = clampd(SYS.fgpu + g * 8 * dt * 10 * 0.2, 5, 99);
    g = gauss();
    SYS.fram = clampd(SYS.fram + g * dt * 2, 20, 95);
}
static inline double sys_cpu(void) { return SYS.cpu == SYS.cpu ? SYS.cpu : SYS.fcpu; }
static inline double sys_gpu(void) { return SYS.gpu == SYS.gpu ? SYS.gpu : SYS.fgpu; }
static inline double sys_ram(void) { return SYS.ram == SYS.ram && SYS.ramTotal == SYS.ramTotal && SYS.ramTotal ? 100 * (1 - SYS.ram / SYS.ramTotal) : SYS.fram; }
#endif
