/* ascii.c: drop a video or a song on the window and watch it as text.
 *
 * Videos: every frame is redrawn in characters, a denser one where the picture
 * is brighter (white, green or amber like an old terminal, or each character in
 * the colour under it). Sound (songs, or a video's soundtrack): a spectrum
 * drawn in characters, mirrored about the middle, from an FFT of what's
 * playing. The sound is the clock; the pictures keep up with it.
 *
 *     ascii                 then drop a file, or press Open
 *     ascii song.mp3        opens it straight away
 *
 * Keys: Space plays / pauses, Left / Right jump 5 s, V switches picture and sound.
 * Bare pieces: bare_sound_load / bare_sound_position, bare_video_load /
 * bare_video_pixels, bare_file_dropped, bare_file_dialog; the text is plain
 * bare_paragraph rows of spans in JetBrains Mono. */
#include "bare.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_COLS 320
#define MAX_ROWS 140
#define FFT_N    2048
#define PI_F     3.14159265f

/* Characters from empty to full. */
static const char RAMP[] = " .`'^\",:;Il!i~+_-?][}{1)(|/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$";
#define RAMP_N ((int)sizeof RAMP - 1)

typedef enum { EMPTY, LOADING, READY, FAILED } Stage;
typedef enum { VIEW_PICTURE, VIEW_SOUND } View;
typedef enum { TINT_WHITE, TINT_GREEN, TINT_AMBER, TINT_COLOUR } Tint;

static struct {
    Stage stage;
    int load_in;                 /* frames to show "Reading" before decoding (it blocks) */
    char path[2048], name[256];
    BareSound sound;
    BareVideo video;
    bool video_known;            /* the video opened or turned out not to be one */
    bool picked_view;            /* the user chose; don't switch for them */
    View view;
    Tint tint;
    float detail;                /* character size in pixels */
    bool playing;
    double pos, length;
    float lo, hi;                /* the picture's levels, eased frame to frame */
    float level[MAX_COLS], peak[MAX_COLS];
} A = { .detail = 10, .lo = 0, .hi = 1 };

static const BareColor MUTED = { 128, 128, 128, 255 };

/* ---- numbers ------------------------------------------------------------------ */

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

static float hash(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = a * 0x9E3779B1u ^ b * 0x85EBCA77u ^ c * 0xC2B2AE3Du;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return (float)(h >> 8) / 16777216.0f;
}

static void fft(float *re, float *im, int n) {   /* in place, radix 2 */
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float a = -2 * PI_F / (float)len, wr = cosf(a), wi = sinf(a);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                float *ur = &re[i + k], *ui = &im[i + k], *vr = &re[i + k + len / 2], *vi = &im[i + k + len / 2];
                float tr = *vr * cr - *vi * ci, ti = *vr * ci + *vi * cr;
                *vr = *ur - tr, *vi = *ui - ti;
                *ur += tr, *ui += ti;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr, cr = ncr;
            }
        }
    }
}

static void clock_text(char *out, size_t cap, double s) {
    int t = (int)(s < 0 ? 0 : s);
    if (t >= 3600) snprintf(out, cap, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(out, cap, "%d:%02d", t / 60, t % 60);
}

/* ---- colours -------------------------------------------------------------------- */

static BareColor tint_color(float shade) {   /* the mono tints, shade 0..1 */
    static const float base[3][3] = { { 236, 236, 236 }, { 90, 255, 140 }, { 255, 186, 80 } };
    const float *b = base[A.tint == TINT_COLOUR ? 0 : A.tint];
    return BARE_RGB(b[0] * shade, b[1] * shade, b[2] * shade);
}

static BareColor ramp_color(float x) {   /* across the spectrum, for TINT_COLOUR */
    static const float stops[4][3] = { { 70, 210, 255 }, { 150, 120, 255 }, { 255, 100, 170 }, { 255, 190, 80 } };
    x = clamp01(x) * 2.999f;
    int i = (int)x;
    float f = x - (float)i;
    return BARE_RGB(stops[i][0] + (stops[i + 1][0] - stops[i][0]) * f, stops[i][1] + (stops[i + 1][1] - stops[i][1]) * f,
                    stops[i][2] + (stops[i + 1][2] - stops[i][2]) * f);
}

static bool same(BareColor a, BareColor b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

/* 0..255 to one of six levels: neighbours share a colour, so a row is a few spans, not one a character. */
static int level6(float x) {
    int l = (int)(x / 51.0f + 0.5f);
    return (l > 5 ? 5 : l < 0 ? 0 : l) * 51;
}

/* One row of characters at (x, y): a paragraph with a span for each run of one colour. */
static void row_text(const char *chars, const BareColor *colors, int n, float x, float y, float size) {
    int first = 0, last = n - 1;
    while (first < n && chars[first] == ' ') first++;
    while (last >= first && chars[last] == ' ') last--;
    if (first > last) return;
    float cw = size * 0.6f;
    bare_paragraph(.font = BARE_MONO, .size = size, .line_height = 1, .nowrap = true, .position = BARE_ABSOLUTE,
                   .x = x + first * cw, .y = y) {
        for (int c = first; c <= last;) {
            int e = c;
            char run[MAX_COLS + 1];
            while (e <= last && (same(colors[e], colors[c]) || chars[e] == ' ')) { run[e - c] = chars[e]; e++; }
            run[e - c] = 0;
            bare_span(run, .color = colors[c]);
            c = e;
        }
    }
}

/* ---- the picture ------------------------------------------------------------------ */

static void draw_picture(BareRect stage, float size) {
    int fw = 0, fh = 0;
    const unsigned char *px = bare_video_pixels(A.video, A.pos, &fw, &fh);
    if (!px || fw <= 0 || fh <= 0) return;
    float cw = size * 0.6f, lh = size;
    float room_w = stage.w - 32, room_h = stage.h - 32, aspect = (float)fw / (float)fh;
    float pw = room_w, ph = room_w / aspect;
    if (ph > room_h) ph = room_h, pw = room_h * aspect;
    int cols = (int)(pw / cw), rows = (int)(ph / lh);
    if (cols > MAX_COLS) cols = MAX_COLS;
    if (rows > MAX_ROWS) rows = MAX_ROWS;
    if (cols < 2 || rows < 2) return;
    float x0 = floorf((stage.w - cols * cw) * 0.5f), y0 = floorf((stage.h - rows * lh) * 0.5f);

    /* Each character is the average of the pixels under it. */
    static float lum[MAX_ROWS][MAX_COLS];
    static unsigned char rgb[MAX_ROWS][MAX_COLS][3];
    int hist[64] = { 0 };
    for (int r = 0; r < rows; r++) {
        int y0p = r * fh / rows, y1p = (r + 1) * fh / rows;
        if (y1p <= y0p) y1p = y0p + 1;
        for (int c = 0; c < cols; c++) {
            int x0p = c * fw / cols, x1p = (c + 1) * fw / cols;
            if (x1p <= x0p) x1p = x0p + 1;
            unsigned s[3] = { 0, 0, 0 }, n = 0;
            for (int y = y0p; y < y1p && y < fh; y++) {
                const unsigned char *p = px + ((size_t)y * (size_t)fw + (size_t)x0p) * 4;
                for (int x = x0p; x < x1p && x < fw; x++, p += 4) s[0] += p[0], s[1] += p[1], s[2] += p[2], n++;
            }
            if (!n) n = 1;
            for (int k = 0; k < 3; k++) rgb[r][c][k] = (unsigned char)(s[k] / n);
            float l = (0.2126f * s[0] + 0.7152f * s[1] + 0.0722f * s[2]) / (255.0f * (float)n);
            lum[r][c] = l;
            hist[(int)(clamp01(l) * 63.0f)]++;
        }
    }
    /* Levels: stretch the frame's darkest 1% to its brightest 1%, eased so cuts don't flash. */
    int total = rows * cols, acc = 0, lo_bin = 0, hi_bin = 63;
    for (int i = 0; i < 64; i++) { acc += hist[i]; if (acc <= total / 100) lo_bin = i; }
    acc = 0;
    for (int i = 63; i >= 0; i--) { acc += hist[i]; if (acc <= total / 100) hi_bin = i; }
    float lo = lo_bin / 63.0f, hi = fmaxf(hi_bin / 63.0f, lo + 0.15f);
    float k = A.playing ? 0.25f : 1.0f;
    A.lo += (lo - A.lo) * k, A.hi += (hi - A.hi) * k;

    char chars[MAX_COLS];
    BareColor colors[MAX_COLS];
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            float v = clamp01((lum[r][c] - A.lo) / fmaxf(A.hi - A.lo, 0.05f));
            v = powf(v, 0.9f);
            chars[c] = RAMP[(int)(v * (RAMP_N - 1) + 0.5f)];
            if (A.tint == TINT_COLOUR) {   /* the pixel's hue, bright enough to read */
                const unsigned char *p = rgb[r][c];
                float m = fmaxf(fmaxf(p[0], p[1]), fmaxf(p[2], 1.0f)), b = (110 + 145 * v) / m;
                colors[c] = BARE_RGB(level6(p[0] * b), level6(p[1] * b), level6(p[2] * b));
            } else {
                colors[c] = tint_color(v < 0.33f ? 0.55f : v < 0.66f ? 0.8f : 1.0f);
            }
        }
        row_text(chars, colors, cols, x0, y0 + r * lh, size);
    }
}

/* ---- the sound ------------------------------------------------------------------- */

/* The spectrum at the playing position into A.level (0..1 a column), log spaced 40 Hz to 16 kHz. */
static void analyse(int cols, float dt) {
    int frames = 0, ch = 0, rate = 0;
    const float *s = bare_sound_samples(A.sound, &frames, &ch, &rate);
    static float re[FFT_N], im[FFT_N], mag[FFT_N / 2];
    long start = (long)(A.pos * rate) - FFT_N / 2;
    for (int i = 0; i < FFT_N; i++) {
        long f = start + i;
        float x = 0;
        if (s && f >= 0 && f < frames) x = ch == 2 ? 0.5f * (s[f * 2] + s[f * 2 + 1]) : s[f];
        re[i] = x * (0.5f - 0.5f * cosf(2 * PI_F * (float)i / (FFT_N - 1)));   /* Hann */
        im[i] = 0;
    }
    fft(re, im, FFT_N);
    for (int i = 0; i < FFT_N / 2; i++) mag[i] = sqrtf(re[i] * re[i] + im[i] * im[i]) * 4.0f / FFT_N;
    float lo = logf(40), hi = logf(16000);
    for (int c = 0; c < cols; c++) {
        float f0 = expf(lo + (hi - lo) * (float)c / (float)cols), f1 = expf(lo + (hi - lo) * (float)(c + 1) / (float)cols);
        float b0 = f0 * FFT_N / (float)(rate > 0 ? rate : 48000), b1 = f1 * FFT_N / (float)(rate > 0 ? rate : 48000);
        float m = 0;
        if (b1 - b0 < 1) {   /* narrower than a bin: read between bins */
            int i = (int)b0;
            float t = b0 - (float)i;
            if (i + 1 < FFT_N / 2) m = mag[i] * (1 - t) + mag[i + 1] * t;
        } else {
            for (int i = (int)b0; i <= (int)b1 && i < FFT_N / 2; i++) m = fmaxf(m, mag[i]);
        }
        float db = 20 * log10f(m + 1e-9f) + 3.0f * log2f(sqrtf(f0 * f1) / 1000.0f);   /* +3 dB an octave: highs read */
        float v = A.playing || dt == 0 ? clamp01((db + 62) / 56) : A.level[c];
        /* rise fast, fall slowly; the peaks fall slower still */
        A.level[c] = v > A.level[c] ? A.level[c] + (v - A.level[c]) * fminf(1, dt * 40) : A.level[c] * expf(-dt * 5);
        if (dt == 0) A.level[c] = v;
        A.peak[c] = fmaxf(A.peak[c] - dt * 0.4f, A.level[c]);
    }
}

/* A spectrum of `cols` columns, mirrored about the middle row; idle shows a slow made-up one. */
static void draw_sound(BareRect stage, float size, float dt, bool idle, float t) {
    float cw = size * 0.6f, lh = size;
    int cols = (int)((stage.w - 32) / cw), rows = (int)((stage.h - 32) / lh);
    if (cols > MAX_COLS) cols = MAX_COLS;
    if (rows > MAX_ROWS) rows = MAX_ROWS;
    if (cols < 2 || rows < 2) return;
    if (idle) {
        for (int c = 0; c < cols; c++) {
            float x = (float)c / (float)cols;
            float v = 0.18f + 0.12f * sinf(x * 9 + t * 1.3f) * sinf(x * 23 - t * 0.7f) + 0.08f * sinf(x * 51 + t * 2.1f);
            A.level[c] = clamp01(v * (1 - 0.6f * fabsf(x - 0.5f) * 2));
            A.peak[c] = A.level[c];
        }
    } else {
        analyse(cols, dt);
    }
    float x0 = floorf((stage.w - cols * cw) * 0.5f), y0 = floorf((stage.h - rows * lh) * 0.5f);
    float half = rows * 0.5f;
    uint32_t tick = (uint32_t)(t * 15);
    char chars[MAX_COLS];
    BareColor colors[MAX_COLS];
    for (int r = 0; r < rows; r++) {
        float d = fabsf((float)r + 0.5f - half) / half;   /* 0 at the middle, 1 at the edge */
        for (int c = 0; c < cols; c++) {
            float L = A.level[c], P = A.peak[c], h = hash((uint32_t)c, (uint32_t)r, tick);
            char ch = ' ';
            float shade = 0.55f;
            if (d < L) {   /* inside the bar: densest at the middle */
                float u = (L - d) / fmaxf(L, 1e-3f);
                float k = clamp01(0.18f + 0.55f * u + 0.35f * L + 0.18f * (h - 0.5f));
                ch = RAMP[1 + (int)(k * (RAMP_N - 2))];
                shade = k > 0.7f ? 1.0f : k > 0.45f ? 0.8f : 0.55f;
            } else if (d < L + 1.0f / half && h < 0.6f) {
                ch = h < 0.3f ? '.' : ':';
            } else if (P > 0.02f && fabsf(d - P) < 0.5f / half) {
                ch = '-';
                shade = 0.8f;
            } else if (d < 0.5f / half && h < 0.5f) {
                ch = '.';   /* the middle line, even in silence */
            }
            chars[c] = ch;
            if (A.tint == TINT_COLOUR) {
                BareColor col = ramp_color((float)c / (float)cols);
                colors[c] = BARE_RGB(col.r * shade, col.g * shade, col.b * shade);
            } else {
                colors[c] = tint_color(shade);
            }
            if (idle) colors[c] = BARE_RGB(colors[c].r * 0.45f, colors[c].g * 0.45f, colors[c].b * 0.45f);
        }
        row_text(chars, colors, cols, x0, y0 + r * lh, size);
    }
}

/* ---- opening, playing -------------------------------------------------------------- */

static void close_media(void) {
    if (A.sound) bare_sound_free(A.sound);
    if (A.video) bare_video_free(A.video);
    A.sound = 0, A.video = 0;
    A.playing = false;
    A.pos = A.length = 0;
}

static void open_path(const char *path) {
    close_media();
    snprintf(A.path, sizeof A.path, "%s", path);
    const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\');
    const char *name = slash > back ? slash : back;
    snprintf(A.name, sizeof A.name, "%s", name ? name + 1 : path);
    A.stage = LOADING;
    A.load_in = 2;   /* draw "Reading" first: decoding the sound blocks */
    A.video_known = false;
    A.picked_view = false;
    memset(A.level, 0, sizeof A.level);
    memset(A.peak, 0, sizeof A.peak);
}

static void play(bool on) {
    if (A.stage != READY) return;
    if (on && A.pos >= A.length - 0.05) A.pos = 0;
    A.playing = on;
    if (!A.sound) return;
    if (on) bare_sound_play_at(A.sound, 1, A.pos);
    else bare_sound_stop(A.sound);
}

static void seek(double to) {
    A.pos = to < 0 ? 0 : to > A.length ? A.length : to;
    if (A.playing && A.sound) {
        bare_sound_stop(A.sound);
        bare_sound_play_at(A.sound, 1, A.pos);
    }
}

static void finish_loading(void) {
    A.video = bare_video_load(A.path, .width = 480, .height = 480);
    A.sound = bare_sound_load(A.path);
    A.length = A.sound ? bare_sound_length(A.sound) : 0;
    A.view = VIEW_SOUND;
    A.stage = READY;
    if (!A.video && !A.sound) A.stage = FAILED;
    else if (A.sound) play(true);
}

/* The video opens on its own thread: once it says what it is, pick the view and the length. */
static void check_video(void) {
    if (!A.video || A.video_known) return;
    BareVideoInfo info = bare_video_info(A.video);
    if (info.failed) {
        bare_video_free(A.video);
        A.video = 0;
        A.video_known = true;
        if (!A.sound) A.stage = FAILED;
    } else if (info.ready) {
        A.video_known = true;
        if (info.duration > A.length) A.length = info.duration;
        if (!A.picked_view) A.view = VIEW_PICTURE;
        if (!A.sound && !A.playing) play(true);
    }
}

static void advance(float dt) {
    if (!A.playing) return;
    if (A.sound) {
        double p = bare_sound_position(A.sound);
        if (p >= 0) { A.pos = p; return; }
        if (A.pos < A.length - 0.5 && A.video) { A.pos += dt; return; }   /* the sound ended early: the picture goes on */
    } else {
        A.pos += dt;
    }
    if (A.pos >= A.length && A.length > 0) { A.pos = A.length; A.playing = false; }
}

/* ---- the window --------------------------------------------------------------------- */

static int segmented(const char *id, const char *const *labels, int n, int selected) {
    bare_row(.id = BARE_ID(id), .radius = 9, .color = BARE_RGB(28, 28, 28), .padding = 3, .gap = 2) {
        for (int i = 0; i < n; i++) {
            bool on = i == selected;
            bare_row(.id = BARE_IDI(id, i), .pad = { 5, 11, 5, 11 }, .radius = 7,
                     .color = on ? BARE_RGB(62, 62, 62) : BARE_CLEAR, .cursor = BARE_CURSOR_HAND,
                     .hover = { .color = on ? BARE_RGB(70, 70, 70) : BARE_RGB(40, 40, 40) },
                     .animate = BARE_EASE(0.15f, BARE_EASE_OUT)) {
                bare_text(labels[i], .size = 13, .color = on ? BARE_RGB(240, 240, 240) : MUTED);
                if (bare_this().clicked) selected = i;
            }
        }
    }
    return selected;
}

static void open_dialog(void) {
    bare_file_dialog(BARE_ID("open"), BARE_FILE_OPEN,
                     "Video and sound:mp4;mkv;mov;webm;avi;wmv;m4v;mp3;wav;flac;m4a;aac;ogg;opus;wma|Video:mp4;mkv;mov;"
                     "webm;avi;wmv;m4v|Sound:mp3;wav;flac;m4a;aac;ogg;opus;wma", NULL);
}

static void top_bar(void) {
    bare_row(.width = BARE_GROW, .align = BARE_CENTER, .gap = 14) {
        bare_text("ascii", .font = BARE_MONO, .size = 18, .weight = BARE_BOLD);
        if (A.stage != EMPTY) bare_text(A.name, .size = 14, .color = MUTED, .nowrap = true, .ellipsis = true);
        bare_box(.width = BARE_GROW);
        if (A.stage == READY && A.video && A.sound) {
            static const char *const views[] = { "Picture", "Sound" };
            int v = segmented("view", views, 2, (int)A.view);
            if (v != (int)A.view) A.view = (View)v, A.picked_view = true;
        }
        static const char *const tints[] = { "White", "Green", "Amber", "Colour" };
        A.tint = (Tint)segmented("tint", tints, 4, (int)A.tint);
        bare_row(.gap = 8, .align = BARE_CENTER) {
            bare_text("Detail", .size = 13, .color = MUTED);
            float fine = 20 - A.detail;   /* right = more detail = smaller characters */
            if (bare_slider(&fine, 4, 14, .width = 110)) A.detail = roundf(20 - fine);
        }
        if (bare_button("Open", .style = &bare_theme()->primary).clicked) open_dialog();
    }
}

static void transport(void) {
    bool ready = A.stage == READY;
    bare_row(.width = BARE_GROW, .align = BARE_CENTER, .gap = 14, .opacity = ready ? 1 : 0.35f) {
        bare_stack(.id = BARE_ID("play"), .width = 42, .height = 42, .radius = 21, .color = BARE_RGB(44, 44, 44),
                   .align = BARE_CENTER, .justify = BARE_CENTER, .cursor = BARE_CURSOR_HAND,
                   .hover = { .color = BARE_RGB(58, 58, 58) }, .press = { .scale = 0.92f },
                   .animate = BARE_EASE(0.12f, BARE_EASE_OUT)) {
            bare_text(A.playing ? "||" : ">", .font = BARE_MONO, .size = 16, .weight = BARE_BOLD);
            if (bare_this().clicked) play(!A.playing);
        }
        char now[32], all[32];
        clock_text(now, sizeof now, A.pos);
        clock_text(all, sizeof all, A.length);
        bare_text(now, .font = BARE_MONO, .size = 13, .color = MUTED);
        float at = (float)A.pos;
        if (bare_slider(&at, 0, (float)(A.length > 0 ? A.length : 1), .width = BARE_GROW) && ready) seek(at);
        bare_text(all, .font = BARE_MONO, .size = 13, .color = MUTED);
    }
}

static void stage_view(float dt, float t) {
    bare_stack(.id = BARE_ID("stage"), .width = BARE_GROW, .height = BARE_GROW, .radius = 14,
               .color = BARE_RGB(13, 13, 13), .border = 1, .border_color = BARE_RGBA(255, 255, 255, 16),
               .clip = true) {
        BareRect r = bare_this().rect;   /* last frame's: the room to fill */
        float size = A.detail;
        if (A.stage == READY && A.view == VIEW_PICTURE && A.video) draw_picture(r, size);
        else if (A.stage == READY && A.sound) draw_sound(r, size, dt, false, t);
        else draw_sound(r, size, dt, true, t);

        if (A.stage != READY || (!A.sound && A.view == VIEW_SOUND && !A.video)) {
            bare_column(.position = BARE_ABSOLUTE, .anchor = BARE_MIDDLE, .align = BARE_CENTER, .gap = 10,
                        .pad = { 26, 34, 26, 34 }, .radius = 16, .color = BARE_RGBA(13, 13, 13, 225)) {
                if (A.stage == LOADING) {
                    bare_text("Reading", .size = 22, .weight = BARE_SEMIBOLD);
                    bare_text(A.name, .size = 14, .color = MUTED);
                } else {
                    bare_text(A.stage == FAILED ? "That file can't be read" : "Drop a video or a song here",
                              .size = 22, .weight = BARE_SEMIBOLD);
                    bare_text(A.stage == FAILED ? A.name : "MP4, MKV, MOV, WebM, MP3, WAV, FLAC, M4A ...", .size = 14,
                              .color = MUTED);
                    bare_box(.height = 6);
                    if (bare_button("Open a file", .style = &bare_theme()->primary).clicked) open_dialog();
                }
            }
        } else if (A.view == VIEW_PICTURE && A.video && !bare_video_pixels(A.video, A.pos, NULL, NULL)) {
            bare_text("...", .font = BARE_MONO, .size = 18, .color = MUTED, .position = BARE_ABSOLUTE,
                      .anchor = BARE_MIDDLE);
        }
    }
}

int main(int argc, char **argv) {
    bare_window(1200, 780, "Bare: ASCII");
    bare_background(BARE_RGB(8, 8, 8));
    if (argc > 1) open_path(argv[1]);
    while (bare_frame()) {
        float dt = bare_dt(), t = (float)bare_time();
        const char *dropped = bare_file_dropped(), *chosen = bare_file_chosen(BARE_ID("open"));
        if (dropped) open_path(dropped);
        else if (chosen && *chosen) open_path(chosen);
        if (A.stage == LOADING && --A.load_in <= 0) finish_loading();
        check_video();

        if (A.stage == READY) {
            if (bare_key_pressed(BARE_KEY_SPACE)) play(!A.playing);
            if (bare_key_pressed(BARE_KEY_LEFT)) seek(A.pos - 5);
            if (bare_key_pressed(BARE_KEY_RIGHT)) seek(A.pos + 5);
            if (bare_key_pressed(BARE_KEY_V) && A.video && A.sound)
                A.view = A.view == VIEW_PICTURE ? VIEW_SOUND : VIEW_PICTURE, A.picked_view = true;
        }
        advance(dt);

        bare_column(.width = BARE_GROW, .height = BARE_GROW, .padding = 18, .gap = 14) {
            top_bar();
            stage_view(dt, t);
            transport();
        }
        /* Moving while playing or loading; the empty screen's idle spectrum at a gentle 20 fps; else asleep. */
        bare_continuous(A.playing || A.stage == LOADING);
        if (!A.playing && A.stage != READY) bare_wake_in(0.05f);
    }
    close_media();
    bare_quit();
}
