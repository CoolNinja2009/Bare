/* ns_config.h: config.js: the settings (Customise sends them), palettes, presets. */
#ifndef NS_CONFIG_H
#define NS_CONFIG_H
#include "ns_util.h"

enum { T_S, T_C, T_D, T_COL, T_TXT, T_FD };
/* key, type, default (number or string), min, max (dropdowns: item count) */
#define NS_KEYS(X) \
    X(preset, T_D, 0, NULL, 0, 8) X(theme, T_D, 0, NULL, 0, 12) \
    X(colA, T_COL, 0, "#ff2bd6", 0, 0) X(colB, T_COL, 0, "#00f0ff", 0, 0) X(colC, T_COL, 0, "#ffe600", 0, 0) \
    X(colBg, T_COL, 0, "#05010f", 0, 0) X(colBg2, T_COL, 0, "#2a0636", 0, 0) \
    X(hueCycle, T_S, 0, NULL, 0, 100) X(intensity, T_S, 100, NULL, 0, 250) X(chaos, T_S, 45, NULL, 0, 100) \
    X(speed, T_S, 100, NULL, 10, 300) X(quality, T_D, 1, NULL, 0, 3) X(fps, T_D, 1, NULL, 0, 3) X(boot, T_C, 1, NULL, 0, 0) \
    X(seed, T_TXT, 0, "NULLSAINT", 0, 0) X(safe, T_C, 0, NULL, 0, 0) \
    X(city, T_C, 1, NULL, 0, 0) X(cityDensity, T_S, 60, NULL, 10, 100) X(cityHeight, T_S, 60, NULL, 10, 100) \
    X(parallax, T_S, 50, NULL, 0, 100) X(moon, T_D, 1, NULL, 0, 6) X(moonSize, T_S, 50, NULL, 10, 100) X(signs, T_C, 1, NULL, 0, 0) \
    X(cars, T_S, 50, NULL, 0, 100) X(weather, T_S, 40, NULL, 0, 100) X(fog, T_S, 45, NULL, 0, 100) \
    X(searchlights, T_C, 1, NULL, 0, 0) X(lightning, T_C, 1, NULL, 0, 0) X(grid, T_C, 0, NULL, 0, 0) \
    X(glyphs, T_C, 1, NULL, 0, 0) X(glyphSet, T_D, 0, NULL, 0, 10) X(glyphCustom, T_TXT, 0, "NULLSAINT01", 0, 0) \
    X(glyphDensity, T_S, 35, NULL, 0, 100) X(glyphSpeed, T_S, 50, NULL, 5, 100) X(glyphSize, T_S, 16, NULL, 8, 40) \
    X(glyphRepel, T_S, 60, NULL, 0, 100) X(glyphColor, T_D, 1, NULL, 0, 5) \
    X(saint, T_C, 1, NULL, 0, 0) X(saintStyle, T_D, 0, NULL, 0, 4) X(saintPos, T_D, 0, NULL, 0, 5) \
    X(saintSize, T_S, 55, NULL, 15, 100) X(saintTrack, T_S, 80, NULL, 0, 100) X(saintAlpha, T_S, 75, NULL, 10, 100) \
    X(saintTalk, T_C, 1, NULL, 0, 0) X(saintWords, T_TXT, 0, "", 0, 0) X(saintBlink, T_C, 1, NULL, 0, 0) \
    X(girl, T_C, 0, NULL, 0, 0) X(girlMode, T_D, 0, NULL, 0, 2) X(girlPos, T_D, 1, NULL, 0, 3) X(girlSize, T_S, 45, NULL, 10, 100) \
    X(girlHair, T_D, 0, NULL, 0, 6) X(girlAcc, T_D, 0, NULL, 0, 4) X(girlUmbrella, T_C, 1, NULL, 0, 0) X(girlTalk, T_C, 1, NULL, 0, 0) \
    X(girlFlip, T_C, 0, NULL, 0, 0) X(girlFile, T_FD, 0, "", 0, 0) X(girlUrl, T_TXT, 0, "", 0, 0) \
    X(photo, T_D, 0, NULL, 0, 3) X(photoPlace, T_D, 0, NULL, 0, 3) X(photoMix, T_S, 55, NULL, 5, 100) X(photoDrift, T_S, 30, NULL, 0, 100) \
    X(photoTint, T_C, 1, NULL, 0, 0) X(photoFile, T_FD, 0, "", 0, 0) \
    X(ads, T_C, 1, NULL, 0, 0) X(adCount, T_S, 5, NULL, 0, 12) X(holo, T_C, 1, NULL, 0, 0) X(holoShape, T_D, 5, NULL, 0, 6) \
    X(holoSize, T_S, 40, NULL, 10, 100) \
    X(hud, T_C, 1, NULL, 0, 0) X(hudGlitch, T_C, 0, NULL, 0, 0) X(clock, T_C, 1, NULL, 0, 0) X(clock24, T_C, 1, NULL, 0, 0) \
    X(logs, T_C, 1, NULL, 0, 0) X(sys, T_C, 1, NULL, 0, 0) X(reticle, T_C, 1, NULL, 0, 0) X(ticker, T_C, 1, NULL, 0, 0) \
    X(popups, T_C, 1, NULL, 0, 0) X(popupRate, T_S, 30, NULL, 0, 100) X(operator_, T_TXT, 0, "OPERATOR", 0, 0) \
    X(messages, T_TXT, 0, "", 0, 0) X(hudScale, T_S, 100, NULL, 60, 180) \
    X(rgb, T_S, 45, NULL, 0, 100) X(scan, T_S, 45, NULL, 0, 100) X(curve, T_S, 20, NULL, 0, 100) X(grain, T_S, 30, NULL, 0, 100) \
    X(blocks, T_S, 35, NULL, 0, 100) X(tear, T_S, 30, NULL, 0, 100) X(vhs, T_S, 25, NULL, 0, 100) X(mosh, T_S, 30, NULL, 0, 100) \
    X(sort, T_S, 20, NULL, 0, 100) X(bloom, T_S, 60, NULL, 0, 100) X(vignette, T_S, 50, NULL, 0, 100) X(wave, T_S, 10, NULL, 0, 100) \
    X(pixelate, T_S, 0, NULL, 0, 100) X(posterize, T_S, 0, NULL, 0, 100) X(dither, T_C, 0, NULL, 0, 0) X(mask, T_C, 0, NULL, 0, 0) \
    X(ascii, T_D, 1, NULL, 0, 3) X(colorMode, T_D, 0, NULL, 0, 7) X(mirror, T_D, 0, NULL, 0, 4) \
    X(trail, T_S, 55, NULL, 0, 100) X(lens, T_S, 35, NULL, 0, 100) X(click, T_D, 4, NULL, 0, 6) X(clickPower, T_S, 60, NULL, 0, 100) \
    X(idleRot, T_C, 1, NULL, 0, 0) X(idleTime, T_S, 60, NULL, 10, 600) X(keys, T_C, 1, NULL, 0, 0) \
    X(audio, T_C, 1, NULL, 0, 0) X(audioGain, T_S, 100, NULL, 10, 400) X(audioRing, T_C, 1, NULL, 0, 0) \
    X(beatGlitch, T_C, 1, NULL, 0, 0) X(fakeBeat, T_C, 0, NULL, 0, 0) \
    X(sound, T_C, 0, NULL, 0, 0) X(soundVol, T_S, 40, NULL, 0, 100) X(drone, T_C, 1, NULL, 0, 0) \
    X(ghosts, T_C, 1, NULL, 0, 0) X(ghostCount, T_S, 2, NULL, 1, 6) \
    X(events, T_C, 1, NULL, 0, 0) X(evBreach, T_C, 1, NULL, 0, 0) X(evMelt, T_C, 1, NULL, 0, 0) X(evShuffle, T_C, 1, NULL, 0, 0) \
    X(evInvert, T_C, 1, NULL, 0, 0) X(evPanic, T_C, 1, NULL, 0, 0) X(evWorm, T_C, 1, NULL, 0, 0) X(evAscii, T_C, 1, NULL, 0, 0) \
    X(evBlackout, T_C, 1, NULL, 0, 0) X(evTakeover, T_C, 1, NULL, 0, 0) X(evSignal, T_C, 1, NULL, 0, 0) X(evMirror, T_C, 1, NULL, 0, 0) \
    X(evFreeze, T_C, 1, NULL, 0, 0)

#define X_ENUM(k, t, d, s, mn, mx) K_##k,
enum { NS_KEYS(X_ENUM) K_COUNT };
typedef struct KeyDef { const char *name; int type; double def; const char *sdef; double min, max; } KeyDef;
#define X_DEF(k, t, d, s, mn, mx) { #k, t, d, s, mn, mx },
static const KeyDef KEYS[K_COUNT] = { NS_KEYS(X_DEF) };

static double CFG[K_COUNT];    /* numbers, checkboxes (0/1), dropdown indices */
static Str CFGS[K_COUNT];      /* colours and text */
#define C(k) (CFG[K_##k])
#define CB(k) (CFG[K_##k] != 0)
#define CS(k) (&CFGS[K_##k])

typedef struct Palette { const char *name, *bg, *bg2, *a, *b, *c, *text, *warn; } Palette;
static const Palette PALETTES[12] = {
    { "Neon Tokyo", "#05010f", "#2a0636", "#ff2bd6", "#00f0ff", "#ffe600", "#e8f7ff", "#ff2a3d" },
    { "Toxic Sludge", "#010702", "#0c2a08", "#7dff00", "#00ff9c", "#e1ff00", "#d6ffd0", "#ff4800" },
    { "Blood Moon", "#080000", "#3a0006", "#ff0033", "#ff7a00", "#ffd6d6", "#ffe6e6", "#ffffff" },
    { "Ice Ghost", "#00040b", "#0b2538", "#8ffcff", "#4d7cff", "#ffffff", "#eaffff", "#ff3c7f" },
    { "Vapor Mall", "#120024", "#ff4fa3", "#ff71ce", "#01cdfe", "#fffb96", "#fff2ff", "#b967ff" },
    { "Amber Terminal", "#060300", "#2b1300", "#ffb000", "#ff6a00", "#fff1c4", "#ffd88a", "#ff2200" },
    { "Rain Code", "#000300", "#001a06", "#00ff41", "#008f11", "#c8ffd4", "#b6ffc8", "#ff0040" },
    { "Ultraviolet", "#03000a", "#1d0047", "#a100ff", "#ff00a8", "#00ffd5", "#f2e6ff", "#ffef00" },
    { "Dead Channel", "#030303", "#1c1c1c", "#ffffff", "#9a9a9a", "#ff0040", "#f0f0f0", "#ff0040" },
    { "Acid Candy", "#0a0012", "#003b3b", "#ff3cac", "#2bff88", "#2b86c5", "#fff", "#ffe100" },
    { "Chrome Sunset", "#07000f", "#ff3d00", "#ff9e00", "#ff006e", "#8338ec", "#fff4e0", "#00f5d4" },
    { "Custom", "#05010f", "#2a0636", "#ff2bd6", "#00f0ff", "#ffe600", "#e8f7ff", "#ff2a3d" },
};
typedef struct Pal { Rgb bg, bg2, a, b, c, text, warn; } Pal;
static inline Pal palette(void) {
    int i = (int)C(theme);
    const Palette *base = &PALETTES[i >= 0 && i < 12 ? i : 0];
    bool custom = i == 11;
    Pal P;
    P.bg = custom ? hex2rgb(U8(CS(colBg))) : hex2rgb(base->bg);
    P.bg2 = custom ? hex2rgb(U8(CS(colBg2))) : hex2rgb(base->bg2);
    P.a = custom ? hex2rgb(U8(CS(colA))) : hex2rgb(base->a);
    P.b = custom ? hex2rgb(U8(CS(colB))) : hex2rgb(base->b);
    P.c = custom ? hex2rgb(U8(CS(colC))) : hex2rgb(base->c);
    P.text = hex2rgb(base->text), P.warn = hex2rgb(base->warn);
    return P;
}

static inline int key_of(const char *name) {
    for (int i = 0; i < K_COUNT; i++) {
        const char *n = KEYS[i].name;
        if (!strcmp(n, name) || (i == K_operator_ && !strcmp(name, "operator"))) return i;
    }
    return -1;
}
static inline void cfg_defaults(void) {
    for (int i = 0; i < K_COUNT; i++) {
        CFG[i] = KEYS[i].def;
        if (KEYS[i].sdef) s_set(&CFGS[i], KEYS[i].sdef);
    }
}
/* coerce: sliders clamp, checkboxes true/1, dropdowns an index, the rest text */
static inline bool cfg_coerce(int k, const char *v, double *num, Str *str) {
    const KeyDef *d = &KEYS[k];
    switch (d->type) {
    case T_S: { double x = atof(v); *num = clampd(x != x ? 0 : x, d->min, d->max); return true; }
    case T_C: *num = !strcmp(v, "true") || !strcmp(v, "1"); return true;
    case T_D: { long x = strtol(v, NULL, 10); *num = clampd((double)x, 0, d->max - 1); return true; }
    case T_FD: {
        const char *s = v, *p;
        for (p = v; *p; p++) if (*p == '/' || *p == '\\') s = p + 1;
        s_set(str, s);
        return false;
    }
    default: s_set(str, v); return false;
    }
}

/* presets: the page's, applied over the defaults (seed, operator, messages and words kept) */
typedef struct PresetVal { int k; double v; } PresetVal;
static const PresetVal PRESET_2[] = { { K_intensity, 45 }, { K_chaos, 12 }, { K_rgb, 25 }, { K_blocks, 10 }, { K_tear, 10 }, { K_mosh, 10 }, { K_sort, 5 }, { K_grain, 20 },
    { K_weather, 70 }, { K_glyphDensity, 20 }, { K_popups, 0 }, { K_theme, 3 }, { K_speed, 70 }, { K_evPanic, 0 }, { K_evInvert, 0 }, { K_evBreach, 0 }, { -1, 0 } };
static const PresetVal PRESET_3[] = { { K_intensity, 230 }, { K_chaos, 100 }, { K_rgb, 100 }, { K_blocks, 90 }, { K_tear, 85 }, { K_vhs, 70 }, { K_mosh, 75 }, { K_sort, 70 },
    { K_wave, 40 }, { K_grain, 70 }, { K_popupRate, 90 }, { K_glyphDensity, 70 }, { K_theme, 2 }, { K_speed, 140 }, { -1, 0 } };
static const PresetVal PRESET_4[] = { { K_theme, 5 }, { K_curve, 75 }, { K_scan, 90 }, { K_mask, 1 }, { K_vhs, 55 }, { K_grain, 55 }, { K_bloom, 80 }, { K_rgb, 55 },
    { K_colorMode, 0 }, { K_glyphSet, 1 }, { K_dither, 0 }, { -1, 0 } };
static const PresetVal PRESET_5[] = { { K_theme, 6 }, { K_glyphDensity, 80 }, { K_glyphSet, 0 }, { K_saintStyle, 2 }, { K_saintSize, 85 }, { K_ascii, 1 }, { K_mosh, 50 },
    { K_colorMode, 0 }, { -1, 0 } };
static const PresetVal PRESET_6[] = { { K_intensity, 35 }, { K_chaos, 5 }, { K_city, 1 }, { K_glyphs, 0 }, { K_ads, 0 }, { K_holo, 0 }, { K_popups, 0 }, { K_rgb, 15 },
    { K_blocks, 5 }, { K_tear, 5 }, { K_mosh, 5 }, { K_sort, 0 }, { K_grain, 15 }, { K_events, 0 }, { K_theme, 8 }, { -1, 0 } };
static const PresetVal PRESET_7[] = { { K_safe, 1 }, { K_intensity, 40 }, { K_chaos, 10 }, { K_evInvert, 0 }, { K_evPanic, 0 }, { K_evBlackout, 0 }, { K_evSignal, 0 },
    { K_beatGlitch, 0 }, { K_blocks, 15 }, { K_tear, 10 }, { -1, 0 } };
static const PresetVal *PRESETS[8] = { NULL, NULL, PRESET_2, PRESET_3, PRESET_4, PRESET_5, PRESET_6, PRESET_7 };

static inline void apply_preset(int i) {
    if (i <= 0 || i > 7) return;
    for (int k = 0; k < K_COUNT; k++) {
        if (k == K_preset || k == K_seed || k == K_operator_ || k == K_messages || k == K_saintWords) continue;
        CFG[k] = KEYS[k].def;
        if (KEYS[k].sdef) s_set(&CFGS[k], KEYS[k].sdef);
    }
    if (PRESETS[i]) for (const PresetVal *p = PRESETS[i]; p->k >= 0; p++) CFG[p->k] = p->v;
}

/* NS.set: returns what changed for the listeners: -1 nothing, -2 everything ('*'), else the key */
static inline int cfg_set(const char *name, const char *value) {
    int k = key_of(name);
    if (k < 0) return -1;
    double num = 0;
    Str str;
    str.n = 0;
    bool isnum = cfg_coerce(k, value, &num, &str);
    if (isnum) {
        if (CFG[k] == num && k != K_preset) return -1;
        CFG[k] = num;
        if (k == K_preset && num > 0) { apply_preset((int)num); return -2; }
    } else {
        if (CFGS[k].n == str.n && !memcmp(CFGS[k].c, str.c, (size_t)str.n * 2)) return -1;
        CFGS[k] = str;
    }
    return k;
}
#endif
