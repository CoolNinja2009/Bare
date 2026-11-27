/* The system decoders behind bare_sound_load and bare_video_load (src/backend/media_win32.c), without a window or a
 * GPU: a WAV written here decodes back to its samples; an MP4 made by the recorder (src/backend/record_win32.c) reads
 * back frame by frame, in order and at the right times, and seeks. Elsewhere than Windows: everything fails cleanly. */
#include "bare_test.h"
#include "backend/media.h"
#include "backend/record.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void temp_path(char *out, size_t cap, const char *name) {
    const char *tmp = getenv("TEMP");
    snprintf(out, cap, "%s/%s", tmp ? tmp : ".", name);
}

static void put16(FILE *f, unsigned v) { fputc((int)(v & 255), f); fputc((int)(v >> 8 & 255), f); }
static void put32(FILE *f, unsigned v) { put16(f, v & 0xffff); put16(f, v >> 16); }

/* 0.25 s of a 16-bit stereo WAV: left a 441 Hz sine, right its negative. */
static bool write_wav(const char *path, int rate, int frames) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fputs("RIFF", f); put32(f, 36u + (unsigned)frames * 4); fputs("WAVEfmt ", f);
    put32(f, 16); put16(f, 1); put16(f, 2); put32(f, (unsigned)rate); put32(f, (unsigned)rate * 4); put16(f, 4); put16(f, 16);
    fputs("data", f); put32(f, (unsigned)frames * 4);
    for (int i = 0; i < frames; i++) {
        int v = (int)lrint(16000 * sin(2 * 3.14159265358979 * 441 * i / rate));
        put16(f, (unsigned)(v & 0xffff));
        put16(f, (unsigned)(-v & 0xffff));
    }
    fclose(f);
    return true;
}

TEST(audio_decodes_a_wav) {
    char path[512];
    temp_path(path, sizeof path, "bare_test_media.wav");
    const int rate = 44100, frames = rate / 4;
    CHECK(write_wav(path, rate, frames));
    bare__media_thread_begin();
    float *s = NULL;
    int n = 0, ch = 0, hz = 0;
    bool ok = bare__media_audio(path, &s, &n, &ch, &hz);
    bare__media_thread_end();
#ifdef _WIN32
    CHECK(ok);
    CHECK(ch == 2);
    CHECK(hz == rate);
    CHECK(n == frames);
    if (ok && n == frames && ch == 2) {
        float worst = 0;
        for (int i = 0; i < frames; i++) {
            float want = (float)(16000 * sin(2 * 3.14159265358979 * 441 * i / rate) / 32768.0);
            worst = fmaxf(worst, fmaxf(fabsf(s[i * 2] - want), fabsf(s[i * 2 + 1] + want)));
        }
        CHECK(worst < 2.0f / 32768.0f);
    }
#else
    CHECK(!ok);
#endif
    free(s);
    remove(path);
}

#define VW 64
#define VH 48
#define VFRAMES 30

/* Frame i is grey level 20 + 7 i all over (after H.264, roughly). */
static bool write_mp4(const char *path) {
    BareRecorder *r = bare__record_open(path, VW, VH, 30);
    if (!r) return false;
    static unsigned char px[VH][VW][4];
    for (int f = 0; f < VFRAMES; f++) {
        memset(px, 20 + 7 * f, sizeof px);
        if (!bare__record_frame(r, px, VW * 4, true)) { bare__record_close(r); return false; }
    }
    return bare__record_close(r) == VFRAMES;
}

typedef struct Got { int grey, w, h; } Got;

static bool take(void *user, const unsigned char *px, int w, int h, int pitch, bool bgra, int delay_ms) {
    (void)bgra, (void)delay_ms;
    Got *g = user;
    g->w = w, g->h = h;
    const unsigned char *mid = px + (ptrdiff_t)(h / 2) * pitch + (ptrdiff_t)(w / 2) * 4;
    g->grey = mid[1];
    return true;
}

TEST(video_reads_and_seeks) {
    char path[512];
    temp_path(path, sizeof path, "bare_test_media.mp4");
    bool made = write_mp4(path);
    bare__media_thread_begin();
    BareMediaReader *r = made ? bare__media_reader_open(path, false, 0, 0) : NULL;
#ifdef _WIN32
    CHECK(made);
    CHECK(r != NULL);
    if (r) {
        double dur = 0;
        int w = 0, h = 0;
        float fps = 0;
        bare__media_reader_info(r, &dur, &w, &h, &fps);
        CHECK(w == VW && h == VH);
        CHECK_NEAR(dur, 1.0, 0.05);
        CHECK_NEAR(fps, 30.0, 0.5);
        /* every frame, in order, a thirtieth of a second apart, its grey near what was written */
        double t = -1, prev = -1;
        Got g = { 0 };
        int count = 0, worst = 0;
        while (bare__media_reader_next(r, take, &g, &t) == 1) {
            CHECK(t > prev);
            if (count < VFRAMES) worst = abs(g.grey - (20 + 7 * count)) > worst ? abs(g.grey - (20 + 7 * count)) : worst;
            prev = t;
            count++;
        }
        CHECK(count == VFRAMES);
        CHECK(worst <= 12);
        CHECK(bare__media_reader_next(r, take, &g, &t) == 0);   /* the end stays the end */
        /* back to the middle: reading on from the key frame before 0.5 s reaches frame 15 */
        CHECK(bare__media_reader_seek(r, 0.5));
        int grey15 = -1;
        for (int i = 0; i < VFRAMES && bare__media_reader_next(r, take, &g, &t) == 1; i++) {
            if (t >= 0.5 - 1e-3) { grey15 = g.grey; break; }
        }
        CHECK_NEAR(t, 0.5, 0.02);
        CHECK(abs(grey15 - (20 + 7 * 15)) <= 12);
        bare__media_reader_close(r);
    }
#else
    CHECK(r == NULL);
#endif
    bare__media_thread_end();
    remove(path);
}

TEST(missing_files_fail_cleanly) {
    bare__media_thread_begin();
    float *s = NULL;
    int n = 0, ch = 0, hz = 0;
    CHECK(!bare__media_audio("Z:/no/such/file.mp3", &s, &n, &ch, &hz));
    CHECK(s == NULL && n == 0);
    CHECK(bare__media_reader_open("Z:/no/such/file.mp4", false, 0, 0) == NULL);
    CHECK(bare__media_reader_next(NULL, take, NULL, NULL) == -1);
    bare__media_reader_close(NULL);
    bare__media_thread_end();
}

int main(void) {
    RUN(audio_decodes_a_wav);
    RUN(video_reads_and_seeks);
    RUN(missing_files_fail_cleanly);
    return test_report();
}
