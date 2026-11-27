/* voice.c: speech to text, shown two ways. A draws the voice as a hill of
 * ASCII characters, B as a grid of glowing dots; as each word is heard,
 * particles leave the top of the hill and fly into it, and the word condenses
 * out of a blur. Then "thinking", then done. A 14 s script that loops; click
 * the round button to start it again.
 *
 * Everything is plain Bare boxes and text, placed absolutely in each card and
 * computed from the clock every frame: no state but the words' measured widths.
 *
 *     voice            plays the loop
 *     voice --at 3.2   holds the script at 3.2 s (for captures)
 */
#include "bare.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CARD_W   640.0f
#define CARD_H   210.0f
#define LOOP     14.0

static const char *WORDS[] = {
    "Remind", "me", "to", "call", "Maya", "tomorrow", "morning,", "then",
    "book", "a", "quiet", "table", "for", "two", "by", "the", "river.",
};
#define NWORDS ((int)(sizeof WORDS / sizeof WORDS[0]))

/* The script, in seconds. */
#define T_TAP     0.7f     /* the button is pressed: listening */
#define T_FIRST   2.0f     /* the first word is heard */
#define T_STEP    0.29f    /* a word every this often */
#define T_QUIET   7.0f     /* speech ends: thinking */
#define T_DONE    9.8f     /* thinking ends */
#define T_FADE    13.0f    /* the text fades for the next loop */

/* The wave's grid inside a card: from x0 to x1, its bottom row at base. */
#define WAVE_X0   56.0f
#define WAVE_X1   584.0f
#define WAVE_BASE 196.0f
#define DOT_CELL  8.0f    /* B: one dot per 8 px */
#define DOT_ROWS  14
#define CH_W      7.2f    /* A: JetBrains Mono at 12 px is 7.2 px wide */
#define CH_H      12.0f
#define CH_ROWS   9
#define MAX_COLS  80

/* Text */
#define TEXT_X    26.0f
#define TEXT_Y    24.0f
#define TEXT_W    470.0f
#define TEXT_SIZE 20.0f
#define LINE_H    27.0f
#define WORD_GAP  5.4f

static float word_w[2][NWORDS], word_h[2][NWORDS];   /* measured last frame */

/* ---- numbers ------------------------------------------------------------- */

static float hash(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint32_t h = a * 0x9E3779B1u ^ b * 0x85EBCA77u ^ c * 0xC2B2AE3Du ^ d * 0x27D4EB2Fu;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return (float)(h >> 8) / 16777216.0f;
}

static float noise(float x, uint32_t seed) {   /* smooth value noise, 0..1 */
    float i = floorf(x), f = x - i;
    float a = hash((uint32_t)(int)i, seed, 7, 1), b = hash((uint32_t)(int)i + 1, seed, 7, 1);
    return a + (b - a) * f * f * (3 - 2 * f);
}

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float smooth(float a, float b, float x) { x = clamp01((x - a) / (b - a)); return x * x * (3 - 2 * x); }
static float ease_in_out(float x) { return x < 0.5f ? 4 * x * x * x : 1 - powf(-2 * x + 2, 3) / 2; }
static float opacity(float x) { return x <= 0.002f ? BARE_ZERO : x; }   /* 0 means "default" in Bare */
static uint8_t u8(float x) { return (uint8_t)(clamp01(x) * 255.0f + 0.5f); }

/* ---- the voice ----------------------------------------------------------- */

/* How loud, 0..1: rises at the tap, sinks to a murmur while thinking, then off. */
static float gain(float t) {
    return smooth(T_TAP, T_TAP + 0.8f, t) * (1 - 0.85f * smooth(T_QUIET - 0.1f, T_QUIET + 0.5f, t)) *
           (1 - smooth(T_DONE - 0.4f, T_DONE + 0.1f, t));
}

/* The hill's height over column c of n, in rows. */
static float height(int card, int c, int n, int rows, float t) {
    float x = (float)c / (float)(n - 1);
    float cx = 0.52f + 0.06f * sinf(t * 0.7f);
    float d = (x - cx) / 0.2f, w = (x - cx) / 0.45f;
    float shape = 0.82f * expf(-d * d) + 0.18f * expf(-w * w);
    float speech = 0.78f + 0.22f * sinf(t * 5.1f) * sinf(t * 2.3f + 0.7f);
    float jitter = 0.7f + 0.6f * noise(c * 0.35f + t * 3.0f, 11 + (uint32_t)card);
    return (float)rows * gain(t) * speech * shape * jitter;
}

/* ---- the wave: B, glowing dots ------------------------------------------- */

static void dot(float x, float y, float size, BareColor color, float glow) {
    bare_box(.position = BARE_ABSOLUTE, .width = size, .height = size, .radius = size * 0.5f, .color = color,
             .offset = { x - size * 0.5f, y - size * 0.5f },
             .shadow = { 0, 0, glow > 0 ? 9.0f : 0.0f, 0, { 255, 255, 255, u8(glow) } });
}

static void dots(float t) {
    int n = (int)((WAVE_X1 - WAVE_X0) / DOT_CELL) + 1;
    float g = gain(t);
    if (g < 0.003f) return;
    uint32_t tick = (uint32_t)(t * 14.0f);   /* the dots flicker 14 times a second */

    BareColor dim = BARE_RGBA(150, 150, 150, u8(0.75f * fminf(1, g * 4)));
    for (int c = 0; c < n; c++) {
        float hc = height(1, c, n, DOT_ROWS, t);
        float x = WAVE_X0 + c * DOT_CELL;
        for (int r = 0; r < DOT_ROWS; r++) {
            float e = hc - (float)r, h = hash((uint32_t)c, (uint32_t)r, tick, 1), h2 = hash((uint32_t)c, (uint32_t)r, tick, 2);
            int kind = 0;   /* 0 nothing, 1 a dim pair, 2 a bright dot */
            if (e >= 1)     kind = h < 0.3f + 0.45f * clamp01((e - 1) / 4) ? 2 : h < 0.92f ? 1 : 0;
            else if (e > 0) kind = h < 0.35f * e ? 2 : h < 0.75f ? 1 : 0;
            else if (e > -2) kind = h < 0.22f * (1 + e / 2) ? 1 : 0;
            if (r == 0 && kind == 0) kind = h < 0.12f * g ? 2 : 1;   /* the baseline is always there */
            float y = WAVE_BASE - r * DOT_CELL;
            if (kind == 2) {
                float b = 0.75f + 0.25f * h2;
                dot(x, y, 3.4f + 2.6f * h2, BARE_RGBA(255, 255, 255, u8(b * fminf(1, g * 3))), 0.6f * b * g);
            } else if (kind == 1) {   /* a tiny colon */
                bare_box(.position = BARE_ABSOLUTE, .width = 1.4f, .height = 1.4f, .color = dim, .offset = { x - 0.7f, y - 2.4f });
                bare_box(.position = BARE_ABSOLUTE, .width = 1.4f, .height = 1.4f, .color = dim, .offset = { x - 0.7f, y + 1.0f });
            }
        }
    }
}

/* ---- the wave: A, ASCII -------------------------------------------------- */

/* Characters from light to dense. */
static const char RAMP[] = ".:-=+*?/|1ijltfrxnuvczJYCLUQ0OZmwqpdbkhao#MW&8%B@$";
#define RAMP_N ((int)sizeof RAMP - 1)

static void ascii(float t) {
    int n = (int)((WAVE_X1 - WAVE_X0) / CH_W) + 1;
    float g = gain(t);
    if (g < 0.003f || n > MAX_COLS) return;
    uint32_t tick = (uint32_t)(t * 12.0f);
    float hc[MAX_COLS];
    for (int c = 0; c < n; c++) hc[c] = height(0, c, n, CH_ROWS, t);
    float fade = fminf(1, g * 3);
    const BareColor ink[4] = { BARE_RGBA(0, 0, 0, 0), BARE_RGBA(150, 150, 150, u8(0.8f * fade)),
                               BARE_RGBA(215, 215, 215, u8(fade)), BARE_RGBA(255, 255, 255, u8(fade)) };

    for (int r = 0; r < CH_ROWS; r++) {
        char row[MAX_COLS + 1];
        int level[MAX_COLS], first = -1, last = -1;
        for (int c = 0; c < n; c++) {
            float e = hc[c] - (float)r, h = hash((uint32_t)c, (uint32_t)r, tick, 3);
            int i = -1;
            if (e > 0)        i = (int)(clamp01(0.3f + 0.12f * e + 0.36f * (h - 0.5f)) * (RAMP_N - 1));
            else if (e > -2)  i = h < 0.13f * (1 + e / 2) ? (int)(h * 40) % 5 : -1;
            if (r == 0 && i < 0 && h < 0.9f) i = 9 + (int)(h * 100) % 22;   /* letters along the bottom */
            row[c] = i < 0 ? ' ' : RAMP[i];
            level[c] = i < 0 ? 0 : i > RAMP_N * 0.72f ? 3 : i > RAMP_N * 0.4f ? 2 : 1;
            if (r == 0 && e <= 1 && level[c]) level[c] = 1;
            if (i >= 0) { if (first < 0) first = c; last = c; }
        }
        if (first < 0) continue;
        /* One paragraph a row, a span for each run of one brightness, and a soft glow under it. */
        bare_paragraph(.font = BARE_MONO, .size = 12, .line_height = 1, .nowrap = true, .position = BARE_ABSOLUTE,
                       .x = WAVE_X0 + first * CH_W - CH_W * 0.5f, .y = WAVE_BASE - r * CH_H - CH_H * 0.5f,
                       .shadow = { 0, 0, 5, 0, { 255, 255, 255, u8(0.35f * g) } }) {
            for (int c = first; c <= last;) {
                int e = c;
                char run[MAX_COLS + 1];
                while (e <= last && (level[e] == level[c] || row[e] == ' ')) { run[e - c] = row[e]; e++; }
                run[e - c] = 0;
                bare_span(run, .color = level[c] ? ink[level[c]] : ink[1]);
                c = e;
            }
        }
    }
}

/* ---- particles: the hill sends each word to where it's written ---------- */

#define PER_WORD 14

static void particles(int card, float t) {
    int n = card ? (int)((WAVE_X1 - WAVE_X0) / DOT_CELL) + 1 : (int)((WAVE_X1 - WAVE_X0) / CH_W) + 1;
    int rows = card ? DOT_ROWS : CH_ROWS;
    float cell_w = card ? DOT_CELL : CH_W, cell_h = card ? DOT_CELL : CH_H;
    float wx = TEXT_X, wy = TEXT_Y;
    for (int i = 0; i < NWORDS; i++) {
        /* where word i is written (the same flow as words()) */
        if (wx > TEXT_X && wx + word_w[card][i] > TEXT_X + TEXT_W) { wx = TEXT_X; wy += LINE_H; }
        float tx = wx, ty = wy, tw = word_w[card][i], th = word_h[card][i];
        wx += word_w[card][i] + WORD_GAP;
        if (tw <= 0) continue;
        float ts = T_FIRST + i * T_STEP;
        if (t < ts || t > ts + 1.6f) continue;
        for (int k = 0; k < PER_WORD; k++) {
            uint32_t s = (uint32_t)(i * 64 + k);
            float t0 = ts + 0.3f * hash(s, 1, 0, (uint32_t)card), dur = 0.5f + 0.35f * hash(s, 2, 0, (uint32_t)card);
            if (t < t0 || t > t0 + dur + 0.35f) continue;
            /* from the top of the hill when it left */
            int c = (int)(n * (0.5f + 0.32f * (hash(s, 3, 0, (uint32_t)card) - 0.5f)));
            float sx = WAVE_X0 + c * cell_w;
            float sy = WAVE_BASE - height(card, c, n, rows, t0) * cell_h * (0.55f + 0.45f * hash(s, 4, 0, (uint32_t)card));
            /* to a point on the word */
            float ex = tx + tw * (0.05f + 0.9f * hash(s, 5, 0, (uint32_t)card));
            float ey = ty + th * (0.25f + 0.55f * hash(s, 6, 0, (uint32_t)card));
            float bend = (hash(s, 7, 0, (uint32_t)card) - 0.5f) * 60;
            float cx = (sx + ex) * 0.5f + bend, cy = (sy + ey) * 0.5f - 20;
            float size = card ? 2.6f + 1.6f * hash(s, 8, 0, 0) : 2.4f;
            float rad = card ? size * 0.5f : 0.4f;
            /* a short streak: the dot and where it just was */
            for (int j = 3; j >= 0; j--) {
                float p = clamp01((t - j * 0.016f - t0) / dur);
                float landed = clamp01((t - t0 - dur) / 0.35f);
                float q = ease_in_out(p), u = 1 - q;
                float x = u * u * sx + 2 * u * q * cx + q * q * ex, y = u * u * sy + 2 * u * q * cy + q * q * ey;
                float a = (1 - j * 0.24f) * (1 - landed) * (p > 0 ? 1 : 0);
                if (a <= 0 || (j > 0 && (p >= 1 || p <= 0))) continue;
                float sz = size * (1 - j * 0.16f) * (1 - 0.5f * landed);
                bare_box(.position = BARE_ABSOLUTE, .width = sz, .height = sz, .radius = card ? sz * 0.5f : rad,
                         .color = BARE_RGBA(255, 255, 255, u8(a)), .offset = { x - sz * 0.5f, y - sz * 0.5f },
                         .shadow = { 0, 0, j == 0 ? 5.0f : 0.0f, 0, { 255, 255, 255, u8(j == 0 ? 0.5f * a : 0) } });
            }
        }
    }
}

/* ---- the text ------------------------------------------------------------ */

static void words(int card, float t) {
    float out = 1 - smooth(T_FADE, T_FADE + 0.5f, t);
    float first = smooth(T_FIRST + 0.4f, T_FIRST + 0.6f, t) * out;
    /* the placeholder: before the tap, while listening, and again as the loop restarts */
    float ph = 1 - first;
    if (t > T_FADE) ph = smooth(T_FADE + 0.5f, T_FADE + 0.9f, t);
    if (ph > 0.01f)
        bare_text(t < T_TAP || t > T_FADE ? "Tap the key and speak" : "Listening...", .size = TEXT_SIZE,
                  .color = BARE_RGBA(120, 120, 120, u8(ph)), .position = BARE_ABSOLUTE, .x = TEXT_X, .y = TEXT_Y);

    float x = TEXT_X, y = TEXT_Y;
    for (int i = 0; i < NWORDS; i++) {
        if (x > TEXT_X && x + word_w[card][i] > TEXT_X + TEXT_W) { x = TEXT_X; y += LINE_H; }
        /* a word condenses out of a blur as its particles land */
        float ts = T_FIRST + i * T_STEP;
        float p = smooth(ts + 0.45f, ts + 1.25f, t);
        float a = p * p * out, blur = clamp01(1 - p) * clamp01(p * 4) * out;
        /* Every word is declared from the start (all but invisible) so it's measured
         * before its particles need to know where it is. */
        BareState s = bare_text(WORDS[i], .id = BARE_IDI(card ? "b" : "a", i), .size = TEXT_SIZE, .nowrap = true,
                                .color = BARE_RGBA(232, 232, 232, a > 0.004f ? u8(a) : 1),
                                .shadow = { 0, 0, 3 + 7 * blur, 0, { 255, 255, 255, u8(0.85f * blur) } },
                                .position = BARE_ABSOLUTE, .x = x, .y = y);
        word_w[card][i] = s.rect.w;
        word_h[card][i] = s.rect.h;
        x += s.rect.w + WORD_GAP;
    }
}

/* ---- status and the button ----------------------------------------------- */

static void status(float t) {
    float on = smooth(T_TAP, T_TAP + 0.25f, t) * (1 - smooth(T_DONE - 0.2f, T_DONE + 0.1f, t));
    if (on <= 0.01f) return;
    char label[16];
    if (t < T_QUIET) snprintf(label, sizeof label, "0:%02d", (int)(t - T_TAP));
    else snprintf(label, sizeof label, "thinking");
    bare_row(.position = BARE_ABSOLUTE, .anchor = BARE_TOP_RIGHT, .x = -24, .y = 27, .gap = 8, .align = BARE_CENTER,
             .opacity = opacity(on)) {
        bare_box(.width = 6, .height = 6, .radius = 3, .color = BARE_RGB(230, 230, 230));
        bare_text(label, .size = 16, .color = BARE_RGB(120, 120, 120));
    }
}

static void mic(float a) {
    if (a <= 0.01f) return;
    bare_stack(.position = BARE_ABSOLUTE, .width = 48, .height = 48, .opacity = opacity(a)) {
        BareColor ink = BARE_RGB(235, 235, 235);
        bare_box(.position = BARE_ABSOLUTE, .x = 19.5f, .y = 12, .width = 9, .height = 15, .radius = 4.5f,
                 .border = 1.8f, .border_color = ink);
        bare_stack(.position = BARE_ABSOLUTE, .x = 16, .y = 21, .width = 16, .height = 9, .clip = true) {   /* the cup */
            bare_box(.position = BARE_ABSOLUTE, .y = -9, .width = 16, .height = 18, .radius = 8, .border = 1.8f,
                     .border_color = ink);
        }
        bare_box(.position = BARE_ABSOLUTE, .x = 23.1f, .y = 29.5f, .width = 1.8f, .height = 5, .color = ink);
    }
}

static bool button(int card, float t) {
    float press = smooth(T_TAP - 0.12f, T_TAP, t) * (1 - smooth(T_TAP, T_TAP + 0.2f, t));
    float listening = smooth(T_TAP, T_TAP + 0.15f, t) * (1 - smooth(T_QUIET - 0.1f, T_QUIET + 0.1f, t));
    float thinking = smooth(T_QUIET - 0.1f, T_QUIET + 0.1f, t) * (1 - smooth(T_DONE - 0.1f, T_DONE + 0.1f, t));
    bool clicked = false;
    bare_stack(.id = BARE_IDI("button", card), .position = BARE_ABSOLUTE, .anchor = BARE_BOTTOM_RIGHT, .x = -18, .y = -18,
               .width = 48, .height = 48, .radius = 24, .color = BARE_RGB(58, 58, 58), .scale = 1 - 0.1f * press,
               .cursor = BARE_CURSOR_HAND, .hover = { .color = BARE_RGB(70, 70, 70) }, .press = { .scale = 0.9f },
               .animate = BARE_EASE(0.15f, BARE_EASE_OUT)) {
        clicked = bare_this().clicked;
        mic(1 - listening - thinking);
        if (listening > 0.01f)   /* stop */
            bare_box(.position = BARE_ABSOLUTE, .x = 17.5f, .y = 17.5f, .width = 13, .height = 13, .radius = 3.5f,
                     .color = BARE_RGB(235, 235, 235), .opacity = opacity(listening), .scale = 0.6f + 0.4f * listening);
        if (thinking > 0.01f)    /* three dots taking turns */
            for (int i = 0; i < 3; i++) {
                float pulse = 0.35f + 0.65f * (0.5f + 0.5f * sinf((float)(t * 6.0) - i * 0.9f));
                bare_box(.position = BARE_ABSOLUTE, .x = 13.5f + i * 8.5f, .y = 21.75f, .width = 4.5f, .height = 4.5f,
                         .radius = 2.25f, .color = BARE_RGBA(235, 235, 235, u8(pulse)), .opacity = opacity(thinking));
            }
    }
    return clicked;
}

/* ---- the page ------------------------------------------------------------ */

static void badge(const char *letter) {
    bare_stack(.width = 26, .height = 26, .radius = 13, .color = BARE_RGB(236, 236, 236), .align = BARE_CENTER,
               .justify = BARE_CENTER) {
        bare_text(letter, .size = 14, .weight = BARE_BOLD, .color = BARE_RGB(24, 24, 24));
    }
}

static bool card(int k, float t) {
    bool clicked = false;
    bare_stack(.id = BARE_IDI("card", k), .width = CARD_W, .height = CARD_H, .radius = 26,
               .color = BARE_RGB(37, 37, 37), .border = 1, .border_color = BARE_RGBA(255, 255, 255, 14), .clip = true,
               .shadow = BARE_SHADOW_SOFT) {
        words(k, t);
        if (k) dots(t); else ascii(t);
        particles(k, t);
        status(t);
        clicked = button(k, t);
    }
    return clicked;
}

int main(int argc, char **argv) {
    double hold = argc > 2 && !strcmp(argv[1], "--at") ? atof(argv[2]) : -1;
    bare_window(760, 620, "Bare: voice");
    bare_background(BARE_RGB(17, 17, 17));
    bare_continuous(true);
    double start = 0;
    while (bare_frame()) {
        float t = hold >= 0 ? (float)hold : (float)fmod(bare_time() - start, LOOP);
        bare_column(.width = BARE_GROW, .height = BARE_GROW, .align = BARE_CENTER, .justify = BARE_CENTER, .gap = 14) {
            for (int k = 0; k < 2; k++) {
                if (k) bare_box(.height = 10);
                badge(k ? "B" : "A");
                if (card(k, t) && hold < 0) start = bare_time() - (T_TAP - 0.15);   /* tap: listen again */
            }
        }
    }
    bare_quit();
}
