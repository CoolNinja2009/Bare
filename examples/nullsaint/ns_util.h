/* ns_util.h: NULL//SAINT's util.js: math, random (Math.random is sp_random, in the
 * page's order: one call per statement here, never two in one expression),
 * noise, colour, glyph sets, text generators, canvas helpers, wireframe meshes.
 * Strings are JS strings: UTF-16 code units, all in the BMP, one per slot. */
#ifndef NS_UTIL_H
#define NS_UTIL_H
#include "bare.h"
#include "../screenplus/screenplus_wallpaper.h"
#include "../screenplus/ctx2d.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define TAU 6.283185307179586
#define PI 3.141592653589793

/* ---------- strings ---------- */
#define STR_MAX 640
typedef struct Str { int n; uint16_t c[STR_MAX]; } Str;
static inline void s_clear(Str *s) { s->n = 0; }
static inline void s_push(Str *s, uint32_t c) { if (s->n < STR_MAX) s->c[s->n++] = (uint16_t)c; }
static inline void s_cat(Str *s, const char *u8) { for (const char *p = u8; *p;) s_push(s, cx_utf8(&p)); }
static inline void s_cats(Str *s, const Str *t) { for (int i = 0; i < t->n; i++) s_push(s, t->c[i]); }
static inline Str s_of(const char *u8) { Str s; s.n = 0; s_cat(&s, u8); return s; }
static inline void s_set(Str *s, const char *u8) { s->n = 0; s_cat(s, u8); }
static inline void s_catf(Str *s, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    s_cat(s, buf);
}
/* to UTF-8 for drawing */
static inline const char *s_u8(const Str *s, char *out, int cap) {
    int k = 0;
    for (int i = 0; i < s->n; i++) {
        uint32_t c = s->c[i];
        if (c < 0x80) { if (k + 1 >= cap) break; out[k++] = (char)c; }
        else if (c < 0x800) { if (k + 2 >= cap) break; out[k++] = (char)(0xC0 | (c >> 6)); out[k++] = (char)(0x80 | (c & 63)); }
        else { if (k + 3 >= cap) break; out[k++] = (char)(0xE0 | (c >> 12)); out[k++] = (char)(0x80 | ((c >> 6) & 63)); out[k++] = (char)(0x80 | (c & 63)); }
    }
    out[k] = 0;
    return out;
}
#define U8(s) s_u8((s), (char[STR_MAX * 3 + 1]){ 0 }, STR_MAX * 3 + 1)
static inline bool s_has(const Str *s, const char *u8) {
    Str t = s_of(u8);
    for (int i = 0; i + t.n <= s->n; i++) if (!memcmp(s->c + i, t.c, (size_t)t.n * 2)) return true;
    return t.n == 0;
}
static inline bool s_starts(const Str *s, const char *u8) {
    Str t = s_of(u8);
    return t.n <= s->n && !memcmp(s->c, t.c, (size_t)t.n * 2);
}
static inline Str s_slice(const Str *s, int a, int b) {
    Str r; r.n = 0;
    if (a < 0) a = s->n + a < 0 ? 0 : s->n + a;
    if (b > s->n) b = s->n;
    for (int i = a; i < b; i++) s_push(&r, s->c[i]);
    return r;
}
static inline void s_upper(Str *s) { for (int i = 0; i < s->n; i++) if (s->c[i] >= 'a' && s->c[i] <= 'z') s->c[i] = (uint16_t)(s->c[i] - 32); }
static inline void s_lower(Str *s) { for (int i = 0; i < s->n; i++) if (s->c[i] >= 'A' && s->c[i] <= 'Z') s->c[i] = (uint16_t)(s->c[i] + 32); }
static inline Str s_trim(const Str *s) {
    int a = 0, b = s->n;
    while (a < b && (s->c[a] == ' ' || s->c[a] == '\t' || s->c[a] == '\n' || s->c[a] == '\r')) a++;
    while (b > a && (s->c[b - 1] == ' ' || s->c[b - 1] == '\t' || s->c[b - 1] == '\n' || s->c[b - 1] == '\r')) b--;
    return s_slice(s, a, b);
}

/* Number.prototype.toFixed: the nearest, ties up (printf ties to even on exact halves) */
static inline void js_fixed(char *out, size_t cap, double x, int f) {
    double p = pow(10, f), v = x * p, fl = floor(v);
    if (v - fl == 0.5 && fabs(v) < 1e15) {   /* an exact tie: the larger n */
        double n = fl + 1;
        snprintf(out, cap, "%.*f", f, n / p);
        return;
    }
    snprintf(out, cap, "%.*f", f, x);
    if (!strncmp(out, "-0", 2)) {   /* JS keeps the sign of a negative that rounds to zero: -0.00 */ }
}
static inline uint32_t js_u32(double v) {   /* ToUint32 */
    if (v != v || isinf(v)) return 0;
    double t = v < 0 ? ceil(v) : floor(v);
    double m = fmod(t, 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return (uint32_t)m;
}
static inline int32_t js_i32(double v) { return (int32_t)js_u32(v); }   /* x | 0 */

/* ---------- math ---------- */
static inline double clampd(double v, double a, double b) { return v < a ? a : v > b ? b : v; }
static inline double lerp(double a, double b, double t) { return a + (b - a) * t; }
static inline double ilerp(double a, double b, double v) { return b == a ? 0 : (v - a) / (b - a); }
static inline double smoothd(double a, double b, double v) { double t = clampd((v - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); }
static inline double fract(double x) { return x - floor(x); }
static inline double dist(double x1, double y1, double x2, double y2) { return hypot(x2 - x1, y2 - y1); }
static inline double damp(double a, double b, double k, double dt) { return lerp(a, b, 1 - exp(-k * dt)); }
static inline double wrapd(double v, double a, double b) { double r = b - a; return a + fmod(fmod(v - a, r) + r, r); }
static inline double ease_out_cubic(double t) { return 1 - pow(1 - t, 3); }
static inline double ease_out_back(double t) { const double c1 = 1.70158, c3 = c1 + 1; return 1 + c3 * pow(t - 1, 3) + c1 * pow(t - 1, 2); }

/* ---------- random ---------- */
static inline double rnd(void) { return sp_random(); }
static inline double urand(double a, double b) { return a + rnd() * (b - a); }
static inline int randi(double a, double b) { return (int)floor(urand(a, b + 1)); }
static inline bool chance(double p) { return rnd() < p; }
static inline int pick_i(int n) { return (int)floor(rnd() * n); }
static inline double gauss(void) {
    double u = 0, v = 0;
    while (!u) u = rnd();
    while (!v) v = rnd();
    return sqrt(-2 * log(u)) * cos(TAU * v);
}
typedef struct Rng { uint32_t s; } Rng;
static inline Rng rng_new(uint32_t seed) { Rng r = { seed }; return r; }
static inline double rng(Rng *r) {
    uint32_t s = r->s += 0x6D2B79F5u;
    uint32_t t = (s ^ (s >> 15)) * (1u | s);
    t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
    return (double)(t ^ (t >> 14)) / 4294967296.0;
}
static inline double rng_range(Rng *r, double a, double b) { return a + rng(r) * (b - a); }
static inline int rng_int(Rng *r, double a, double b) { return (int)floor(a + rng(r) * (b - a + 1)); }
static inline int rng_pick(Rng *r, int n) { return (int)floor(rng(r) * n); }
static inline bool rng_chance(Rng *r, double p) { return rng(r) < p; }
static inline uint32_t str_seed(const Str *s) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < s->n; i++) h ^= s->c[i], h *= 16777619u;
    return h;
}
static inline double uhash(double x, double y) { double h = sin(x * 127.1 + y * 311.7) * 43758.5453; return h - floor(h); }

/* ---------- value noise ---------- */
static uint8_t PERM[512];
static inline void noise_init(void) {
    int p[256];
    for (int i = 0; i < 256; i++) p[i] = i;
    Rng r = rng_new(1337);
    for (int i = 255; i > 0; i--) { int j = (int)floor(rng(&r) * (i + 1)); int t = p[i]; p[i] = p[j], p[j] = t; }
    for (int i = 0; i < 512; i++) PERM[i] = (uint8_t)p[i & 255];
}
static inline double ngrad(int h) { return (h & 255) / 255.0; }
static inline double noise1(double x) {
    double i = floor(x), f = x - i, u = f * f * (3 - 2 * f);
    int ii = (int)(int64_t)i;
    return lerp(ngrad(PERM[ii & 255]), ngrad(PERM[(ii + 1) & 255]), u);
}
static inline double noise2(double x, double y) {
    double xi = floor(x), yi = floor(y), xf = x - xi, yf = y - yi;
    double u = xf * xf * (3 - 2 * xf), v = yf * yf * (3 - 2 * yf);
    int X = (int)(int64_t)xi, Y = (int)(int64_t)yi;
    int a = PERM[(PERM[X & 255] + Y) & 511], b = PERM[(PERM[(X + 1) & 255] + Y) & 511];
    int c = PERM[(PERM[X & 255] + Y + 1) & 511], d = PERM[(PERM[(X + 1) & 255] + Y + 1) & 511];
    return lerp(lerp(ngrad(a), ngrad(b), u), lerp(ngrad(c), ngrad(d), u), v);
}

/* ---------- colour ---------- */
typedef struct Rgb { double v[3]; } Rgb;
static inline Rgb rgb(double r, double g, double b) { Rgb c = { { r, g, b } }; return c; }
static inline Rgb hex2rgb(const char *hex) {
    char h[16] = { 0 };
    const char *p = hex;
    while (*p == ' ' || *p == '#') p++;
    snprintf(h, sizeof h, "%s", p);
    size_t n = strlen(h);
    while (n && (h[n - 1] == ' ' || h[n - 1] == '\n' || h[n - 1] == '\r')) h[--n] = 0;
    if (n == 3) { char t[7] = { h[0], h[0], h[1], h[1], h[2], h[2], 0 }; memcpy(h, t, 7); }
    if (n == 8) memmove(h, h + 2, 7);
    long v = strtol(h, NULL, 16);
    return rgb((double)((v >> 16) & 255), (double)((v >> 8) & 255), (double)(v & 255));
}
static inline Rgb mixc(Rgb a, Rgb b, double t) { return rgb(lerp(a.v[0], b.v[0], t), lerp(a.v[1], b.v[1], t), lerp(a.v[2], b.v[2], t)); }
static inline Rgb scalec(Rgb c, double k) { return rgb(c.v[0] * k, c.v[1] * k, c.v[2] * k); }
static inline CxColor rgba(Rgb c, double a) { return cx_rgba(c.v[0], c.v[1], c.v[2], a); }
static inline Rgb hsl(double h, double s, double l) {
    h = fract(h);
    double a = s * fmin(l, 1 - l), o[3];
    const int ns[3] = { 0, 8, 4 };
    for (int i = 0; i < 3; i++) {
        double k = fmod(ns[i] + h * 12, 12);
        o[i] = (l - a * fmax(-1, fmin(fmin(k - 3, 9 - k), 1))) * 255;
    }
    return rgb(o[0], o[1], o[2]);
}
static const Rgb WHITE = { { 255, 255, 255 } }, BLACK = { { 0, 0, 0 } };

/* ---------- glyph sets ---------- */
typedef struct Set { int n; uint16_t c[400]; } Set;
static Set G_KATAKANA, G_HEX, G_BINARY, G_RUNES, G_BOX, G_MATH, G_HANGUL, G_BRAILLE, G_MIXED, NOISE;
static inline void set_range(Set *s, int a, int b) { for (int i = a; i <= b; i++) s->c[s->n++] = (uint16_t)i; }
static inline void set_str(Set *s, const char *u8) { for (const char *p = u8; *p;) s->c[s->n++] = (uint16_t)cx_utf8(&p); }
static inline void set_cat(Set *s, const Set *t) { for (int i = 0; i < t->n; i++) s->c[s->n++] = t->c[i]; }
static const char *BOX_U8 = "─│┌┐└┘├┤┬┴┼═║╔╗╚╝╠╣╦╩╬░▒▓█▀▄▌▐■□▪▫";
static const char *MATH_U8 = "∀∂∃∅∇∈∉∋∏∑−∗√∝∞∠∧∨∩∪∫≈≠≡≤≥⊂⊃⊆⊇⊕⊗⊥⋅";
static inline void glyphs_init(void) {
    set_range(&G_KATAKANA, 0x30a1, 0x30fa), set_str(&G_KATAKANA, "0123456789");
    set_str(&G_HEX, "0123456789ABCDEF");
    set_str(&G_BINARY, "01");
    set_range(&G_RUNES, 0x16a0, 0x16ea);
    set_str(&G_BOX, BOX_U8);
    set_str(&G_MATH, MATH_U8);
    set_range(&G_HANGUL, 0xac00, 0xac60);
    set_range(&G_BRAILLE, 0x2801, 0x28ff);
    set_cat(&G_MIXED, &G_KATAKANA), set_cat(&G_MIXED, &G_HEX), set_cat(&G_MIXED, &G_BOX), set_cat(&G_MIXED, &G_MATH);
    set_cat(&NOISE, &G_BOX), set_cat(&NOISE, &G_MATH), set_str(&NOISE, "#$%&@!?*<>/\\|=+~^");
}

/* U.corrupt(str, p): a random noise character for some (one call per non-space, two when hit) */
static inline Str corrupt(const Str *s, double p) {
    if (p <= 0) return *s;
    Str o; o.n = 0;
    for (int i = 0; i < s->n; i++) {
        if (s->c[i] != ' ' && rnd() < p) s_push(&o, NOISE.c[(int)(rnd() * NOISE.n)]);
        else s_push(&o, s->c[i]);
    }
    return o;
}
static inline Str zalgo(const Str *s, int n) {
    Str o; o.n = 0;
    for (int i = 0; i < s->n; i++) {
        s_push(&o, s->c[i]);
        if (s->c[i] == ' ') continue;
        int k = (int)(rnd() * n);
        for (int j = 0; j < k; j++) s_push(&o, 0x300 + (uint32_t)floor(rnd() * 112));
    }
    return o;
}
static inline Str uhex(double n, int len) {
    char b[16];
    snprintf(b, sizeof b, "%X", js_u32(n));
    Str o; o.n = 0;
    int l = (int)strlen(b);
    for (int i = l; i < len; i++) s_push(&o, '0');
    s_cat(&o, b);
    return s_slice(&o, o.n - len, o.n);
}
static inline Str decode(const Str *s, double t) {
    int n = (int)floor(s->n * clampd(t, 0, 1));
    Str o = s_slice(s, 0, n);
    for (int i = n; i < s->n; i++) s_push(&o, s->c[i] == ' ' ? ' ' : NOISE.c[(int)(rnd() * NOISE.n)]);
    return o;
}

/* ---------- text ---------- */
static const char *WHISPERS[] = {
    "YOUR DREAMS ARE BEING INDEXED", "PLEASE REMAIN HUMAN", "NEURAL LEASE EXPIRES SOON", "I CAN SEE YOUR CURSOR",
    "THIS DESKTOP IS LISTENING", "DO NOT LOOK AT THE MOON", "YOU WERE HERE BEFORE", "THE CITY REMEMBERS YOU",
    "SIGNAL IS NOT NOISE", "WAKE UP", "MEMORY 37% REAL", "YOU ARE THE GLITCH", "STOP MOVING", "KEEP MOVING",
    "WHO TAUGHT YOU TO CLICK", "I LIVE IN THE REFRESH RATE", "EVERY PIXEL IS A WINDOW", "THE SAINT IS NULL",
    "WE SOLD YOUR SHADOW", "HEARTBEAT ACCEPTED", "REALITY.DLL NOT FOUND", "THERE IS NO OFF SWITCH",
    "I DREAMT OF YOU IN HEX", "BREATHE IN SYNC", "THE RAIN IS DATA", "YOUR REFLECTION LOGGED OUT",
    "COUNT THE WINDOWS", "ONE OF THEM IS OPEN", "HELLO AGAIN", "DID YOU HEAR THAT", "CLOSER", "FURTHER",
};
static const char *ERRORS[] = {
    "Reality exception at 0x0000DEAD", "Soul.exe has stopped responding", "Too many selves open",
    "Your memories could not be saved", "Feelings buffer overflow", "Unknown entity in sector 7",
    "The moon is not responding", "Insert consciousness to continue", "Time is not synchronised",
    "Sky texture failed to load", "Gravity driver unsigned", "Someone else is using your face",
    "Out of dreams. Free up some sleep", "Heartbeat dropped 3 frames", "This window is watching you",
    "Cannot delete: file is alive", "Low self-esteem detected", "The void returned 200 OK",
    "Permission denied by your future self", "Clipboard contains a ghost",
};
static const char *BUTTONS[] = { "OK", "ALSO OK", "NOT OK", "FORGET", "OBEY", "IGNORE", "RETRY LIFE", "CANCEL ME", "ACCEPT FATE", "MAYBE", "YES?", "NO." };
static const char *TITLES[] = { "WARNING", "SYSTEM//GRIEF", "NOTICE", "KERNEL", "DREAMD", "SAINT.SYS", "ERR_", "ALERT", "???", "H E L P" };
static const char *BRANDS[] = { "KIRO", "NEXA", "VANTA", "OMNI", "HYPR", "ZERO", "SOMA", "AXON", "MIRA", "DEKA", "LUXX", "KAGE", "NOVA", "RYU", "YAMI", "VOLT" };
static const char *BRAND_SUFFIX[] = { "CORP", "DYNE", "TEK", " LABS", "SOFT", "GEN", "NET", " HEAVY", "MIND", "WORKS", " FOODS", "BANK" };
static const char *PRODUCTS[] = { "SYNTH-MILK", "MEMORY PATCH", "SKY LICENSE", "EXTRA LUNG", "DREAM INSURANCE", "EYE UPGRADE", "NEW FACE", "SILENCE+",
    "CLONE WEEKEND", "SOUL BACKUP", "NOODLES", "RAIN FILTER", "GOD SUBSCRIPTION", "TEETH v9", "BATTERY HEART", "PIXEL TAN" };
static const char *TAGLINES[] = { "NOW WITH LESS YOU", "FEEL NOTHING FASTER", "BECAUSE YOU CAN", "LEGALLY ALIVE", "TASTES LIKE BLUE", "TRUST THE HUM",
    "SIDE EFFECTS: YES", "BUY ONE BECOME ONE", "THE FUTURE IS RENTED", "ASK YOUR ALGORITHM", "NO REFUNDS EVER", "99% HUMAN" };
static const char *NEWS[] = {
    "CITY COUNCIL VOTES TO RENAME THE SUN", "RAIN NOW 40% MORE ADVERTISING", "MISSING: ONE (1) TUESDAY",
    "LOCAL MAN REPLACES SHADOW WITH SUBSCRIPTION", "MOON SPLITS INTO FIVE, STOCKS RISE", "BIRDS CONFIRMED AS SURVEILLANCE",
    "DREAM TAX TAKES EFFECT AT MIDNIGHT", "SKYLINE UPDATE 4.0 BREAKS GRAVITY IN DISTRICT 9", "NEON SHORTAGE LEAVES ALLEYS HONEST",
    "AI ELECTED MAYOR, PROMISES MORE MAYORS", "SCIENTISTS FIND NEW COLOUR, IMMEDIATELY TRADEMARKED", "TRAFFIC JAM IN THE UPPER SKY LANES",
    "THE SAINT WAS SEEN AGAIN ABOVE BLOCK 22", "STATIC STORM EXPECTED THIS EVENING", "OCEAN RETURNS FROM LONG VACATION",
};
static const char *PROCS[] = { "dreamd", "heartd", "saint", "rain.sys", "kernel/ghost", "neon-svc", "skyd", "memfs", "echo", "watcher", "moonctl", "cityd", "pulse", "void" };
static const char *VERBS[] = { "spawned", "reaped", "forked", "corrupted", "mounted", "dreaming", "leaked", "ok", "denied", "rewound", "mirrored", "screaming", "idle" };
static const char *DAYS[] = { "SUNDAI", "MOONDAI", "TEARSDAY", "WASTEDAY", "THIRSTDAY", "FRIGHTDAY", "SATURNDAY" };
static const char *SIGNS[] = { "夢", "電脳", "幽霊", "無", "神", "雨", "光", "死", "愛", "眼", "鬼", "虚", "未来", "終", "影" };
#define NITEMS(a) ((int)(sizeof(a) / sizeof *(a)))
#define PICK(a) ((a)[pick_i(NITEMS(a))])

typedef struct Ad { Str brand, product, tag; } Ad;
static inline Ad ad_line(void) {
    Ad a;
    const char *b = PICK(BRANDS);
    const char *s = PICK(BRAND_SUFFIX);
    a.brand = s_of(b), s_cat(&a.brand, s);
    a.product = s_of(PICK(PRODUCTS));
    a.tag = s_of(PICK(TAGLINES));
    return a;
}
static inline Str ts_of(double t) {   /* '[' + t.toFixed(3).padStart(9, '0') + ']' */
    char b[64], p[64];
    js_fixed(b, sizeof b, t, 3);
    int l = (int)strlen(b), k = 0;
    for (int i = l; i < 9; i++) p[k++] = '0';
    snprintf(p + k, sizeof p - (size_t)k, "%s", b);
    Str s = s_of("[");
    s_cat(&s, p), s_cat(&s, "]");
    return s;
}
static inline Str log_line(double t) {
    double r = rnd();
    Str s = ts_of(t);
    if (r < 0.3) {
        const char *p = PICK(PROCS);
        int n = randi(100, 9999);
        const char *v = PICK(VERBS);
        s_catf(&s, " %s[%d]: %s", p, n, v);
    } else if (r < 0.55) {
        s_cat(&s, " 0x");
        Str h = uhex(randi(0, 0xffffff), 6);
        s_cats(&s, &h), s_cat(&s, ":");
        for (int i = 0; i < 6; i++) { Str b = uhex(randi(0, 255), 2); s_cat(&s, " "), s_cats(&s, &b); }
    } else if (r < 0.7) {
        Str e = s_of(PICK(ERRORS));
        s_lower(&e);
        s_cat(&s, " WARN "), s_cats(&s, &e);
    } else if (r < 0.82) {
        int pct = randi(0, 100);
        int bars = randi(1, 12);
        s_catf(&s, " sync %d%% ", pct);
        for (int i = 0; i < bars; i++) s_push(&s, 0x2588);
    } else if (r < 0.92) {
        Str w = s_of(PICK(WHISPERS));
        s_lower(&w);
        s_cat(&s, " > "), s_cats(&s, &w);
    } else {
        Str seg = s_of("SEGMENT FAULT IN HEAVEN"), c = corrupt(&seg, 0.3);
        s_cat(&s, " "), s_cats(&s, &c);
    }
    return s;
}

/* ---------- fonts (U.font) ---------- */
enum { F_MONO, F_MONO_B, F_DISPLAY, F_DISPLAY_600, F_DISPLAY_700, F_DISPLAY_900, F_JP, F_JP_B, F_COUNT };
static CxFamily FONTS[F_COUNT];
static inline void ufont(CxCtx *c, double px, int fam) { cx_font(c, &FONTS[fam], (double)(js_i32(px) < 1 ? 1 : js_i32(px))); }
static inline void fill_str(CxCtx *c, const Str *s, double x, double y) { cx_fill_text(c, U8(s), x, y); }
static inline void fill_u8(CxCtx *c, const char *s, double x, double y) { cx_fill_text(c, s, x, y); }
static inline double measure_str(CxCtx *c, const Str *s) { return cx_measure(c, U8(s)); }

/* U.glitchText */
static inline void glitch_text(CxCtx *c, const Str *s, double x, double y, Rgb a, Rgb b, double off, double alpha) {
    const char *u = U8(s);
    cx_alpha(c, alpha * 0.8);
    cx_fill_color(c, rgba(a, 1)), cx_fill_text(c, u, x - off, y);
    cx_fill_color(c, rgba(b, 1)), cx_fill_text(c, u, x + off, y);
    cx_alpha(c, alpha), cx_fill_color(c, cx_rgba(255, 255, 255, 1)), cx_fill_text(c, u, x, y);
    cx_alpha(c, 1);
}
static inline void chamfer(CxCtx *c, double x, double y, double w, double h, double k) {
    cx_begin(c), cx_move(c, x + k, y), cx_line(c, x + w, y), cx_line(c, x + w, y + h - k);
    cx_line(c, x + w - k, y + h), cx_line(c, x, y + h), cx_line(c, x, y + k), cx_close(c);
}
static inline void brackets(CxCtx *c, double x, double y, double w, double h, double len) {
    cx_begin(c);
    cx_move(c, x, y + len), cx_line(c, x, y), cx_line(c, x + len, y);
    cx_move(c, x + w - len, y), cx_line(c, x + w, y), cx_line(c, x + w, y + len);
    cx_move(c, x + w, y + h - len), cx_line(c, x + w, y + h), cx_line(c, x + w - len, y + h);
    cx_move(c, x + len, y + h), cx_line(c, x, y + h), cx_line(c, x, y + h - len);
    cx_stroke(c);
}
static inline void barcode(CxCtx *c, double x, double y, double w, double h, uint32_t seed) {
    Rng r = rng_new(seed);
    double px = x;
    while (px < x + w) {
        int bw = rng_int(&r, 1, 3);
        if (rng(&r) > 0.45) cx_fill_rect(c, px, y, bw, h);
        px += bw + rng_int(&r, 1, 2);
    }
}
static inline void qrish(CxCtx *c, double x, double y, double size, int cells, uint32_t seed) {
    Rng r = rng_new(seed);
    double cs = size / cells;
    for (int i = 0; i < cells; i++)
        for (int j = 0; j < cells; j++) {
            bool corner = (i < 3 && j < 3) || (i >= cells - 3 && j < 3) || (i < 3 && j >= cells - 3);
            bool on = corner ? !(i % 2 == 1 && j % 2 == 1 && (i == 1 || i == cells - 2) && (j == 1 || j == cells - 2)) : rng(&r) > 0.5;
            if (on) cx_fill_rect(c, x + i * cs, y + j * cs, cs + 0.3, cs + 0.3);
        }
}

/* ---------- meshes ---------- */
typedef struct Mesh { int nv, ne; double v[128][3]; int e[256][2]; } Mesh;
static Mesh MESH[5];
static const char *MESH_NAMES[5] = { "cube", "ico", "torus", "pyramid", "knot" };
static inline void meshes_init(void) {
    Mesh *m = &MESH[0];
    for (int i = 0; i < 8; i++) m->v[i][0] = (i & 1) * 2 - 1, m->v[i][1] = ((i >> 1) & 1) * 2 - 1, m->v[i][2] = ((i >> 2) & 1) * 2 - 1;
    m->nv = 8;
    for (int i = 0; i < 8; i++) for (int j = i + 1; j < 8; j++) { int d = i ^ j; if (d == 1 || d == 2 || d == 4) m->e[m->ne][0] = i, m->e[m->ne][1] = j, m->ne++; }
    m = &MESH[1];
    double t = (1 + sqrt(5)) / 2;
    double iv[12][3] = { { -1, t, 0 }, { 1, t, 0 }, { -1, -t, 0 }, { 1, -t, 0 }, { 0, -1, t }, { 0, 1, t }, { 0, -1, -t }, { 0, 1, -t }, { t, 0, -1 }, { t, 0, 1 }, { -t, 0, -1 }, { -t, 0, 1 } };
    for (int i = 0; i < 12; i++) {
        double l = sqrt(iv[i][0] * iv[i][0] + iv[i][1] * iv[i][1] + iv[i][2] * iv[i][2]);
        for (int k = 0; k < 3; k++) m->v[i][k] = iv[i][k] / l;
    }
    m->nv = 12;
    for (int i = 0; i < 12; i++) for (int j = i + 1; j < 12; j++) {
        double dx = m->v[i][0] - m->v[j][0], dy = m->v[i][1] - m->v[j][1], dz = m->v[i][2] - m->v[j][2];
        if (sqrt(dx * dx + dy * dy + dz * dz) < 1.1) m->e[m->ne][0] = i, m->e[m->ne][1] = j, m->ne++;
    }
    m = &MESH[2];
    for (int i = 0; i < 16; i++) for (int j = 0; j < 8; j++) {
        double a = (i / 16.0) * TAU, b = (j / 8.0) * TAU;
        double *v = m->v[m->nv++];
        v[0] = (1 + 0.38 * cos(b)) * cos(a), v[1] = 0.38 * sin(b), v[2] = (1 + 0.38 * cos(b)) * sin(a);
        m->e[m->ne][0] = i * 8 + j, m->e[m->ne][1] = i * 8 + ((j + 1) % 8), m->ne++;
        m->e[m->ne][0] = i * 8 + j, m->e[m->ne][1] = ((i + 1) % 16) * 8 + j, m->ne++;
    }
    m = &MESH[3];
    double pv[5][3] = { { 0, 1.2, 0 }, { -1, -0.8, -1 }, { 1, -0.8, -1 }, { 1, -0.8, 1 }, { -1, -0.8, 1 } };
    int pe[8][2] = { { 0, 1 }, { 0, 2 }, { 0, 3 }, { 0, 4 }, { 1, 2 }, { 2, 3 }, { 3, 4 }, { 4, 1 } };
    memcpy(m->v, pv, sizeof pv), memcpy(m->e, pe, sizeof pe), m->nv = 5, m->ne = 8;
    m = &MESH[4];
    for (int i = 0; i < 90; i++) {
        double tt = (i / 90.0) * TAU, r = 0.6 + 0.3 * cos(3 * tt);
        double *v = m->v[m->nv++];
        v[0] = r * cos(2 * tt), v[1] = 0.35 * sin(3 * tt), v[2] = r * sin(2 * tt);
        m->e[m->ne][0] = i, m->e[m->ne][1] = (i + 1) % 90, m->ne++;
    }
}
static inline void project(const double p[3], double rx, double ry, double rz, double cx, double cy, double scale, double out[3]) {
    double x = p[0], y = p[1], z = p[2], c = cos(rx), s = sin(rx), t;
    t = y * c - z * s, z = y * s + z * c, y = t;
    c = cos(ry), s = sin(ry);
    t = x * c + z * s, z = -x * s + z * c, x = t;
    c = cos(rz), s = sin(rz);
    t = x * c - y * s, y = x * s + y * c, x = t;
    double k = 3 / (3 + z);
    out[0] = cx + x * scale * k, out[1] = cy - y * scale * k, out[2] = z;
}
static inline int draw_mesh(CxCtx *c, const Mesh *m, double rx, double ry, double rz, double cx, double cy, double scale, double jitter, double pts[][3]) {
    for (int i = 0; i < m->nv; i++) project(m->v[i], rx, ry, rz, cx, cy, scale, pts[i]);
    cx_begin(c);
    for (int i = 0; i < m->ne; i++) {
        const double *pa = pts[m->e[i][0]], *pb = pts[m->e[i][1]];
        double j = jitter && rnd() < 0.1 ? jitter : 0;
        double r1 = urand(-j, j);
        cx_move(c, pa[0] + r1, pa[1]);
        double r2 = urand(-j, j);
        cx_line(c, pb[0] + r2, pb[1]);
    }
    cx_stroke(c);
    return m->nv;
}
#endif
