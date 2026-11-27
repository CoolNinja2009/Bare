/* video.c: bare_video_load / bare_video_pixels, a video's frames as pixels for
 * the app to draw its own way.
 *
 * Each video has a thread that reads it a frame at a time (src/backend/media.h:
 * decoded on the GPU and shrunk there when it can), shrinks each frame to fit
 * the size asked for, and keeps a few in a ring just ahead of the time the app
 * last asked for. Behind that time frames are dropped; a jump back, or far
 * ahead, seeks. So memory stays a handful of small frames however long the
 * film, and a frame is usually waiting when it's due. */
#include "bare.h"
#include "backend/media.h"
#include "thread.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_VIDEOS 16
#define RING       24        /* frames kept */
#define AHEAD_S    0.4       /* decoded this far past the time asked for */
#define SEEK_S     1.5       /* a time further ahead than this seeks instead of decoding up to it */

typedef struct Frame { double t; unsigned char *px; } Frame;

typedef struct Video {
    bool used;
    char *path;
    int max_w, max_h;
    Lock lock;
    Cond cond;
    Thread thread;
    /* shared, under lock */
    double want;              /* the time the app last asked for */
    bool quit, ready, failed, ended;
    double duration;
    int vw, vh, fw, fh;       /* the video's size, a frame's size */
    float fps;
    Frame ring[RING];         /* by time */
    int count;
    unsigned char *spare[RING];
    int spares;
    /* the app's */
    unsigned char *out;
    int out_w, out_h;
    bool have_out;
} Video;

static Video g_videos[MAX_VIDEOS];

static Video *get(BareVideo v) { return v >= 1 && v <= MAX_VIDEOS && g_videos[v - 1].used ? &g_videos[v - 1] : NULL; }

/* ---- the decoding thread ----------------------------------------------------- */

typedef struct Shrink { unsigned char *dst; int w, h; } Shrink;

/* A decoded frame (BGRA) averaged down into dst (RGBA), box by box. */
static bool shrink(void *user, const unsigned char *px, int w, int h, int pitch, bool bgra, int delay_ms) {
    (void)delay_ms;
    Shrink *s = user;
    for (int y = 0; y < s->h; y++) {
        int y0 = (int)((long)y * h / s->h), y1 = (int)((long)(y + 1) * h / s->h);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < s->w; x++) {
            int x0 = (int)((long)x * w / s->w), x1 = (int)((long)(x + 1) * w / s->w);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned sum[3] = { 0, 0, 0 }, n = 0;
            for (int yy = y0; yy < y1; yy++) {
                const unsigned char *p = px + (ptrdiff_t)yy * pitch + (ptrdiff_t)x0 * 4;
                for (int xx = x0; xx < x1; xx++, p += 4) sum[0] += p[0], sum[1] += p[1], sum[2] += p[2], n++;
            }
            unsigned char *d = s->dst + ((size_t)y * (size_t)s->w + (size_t)x) * 4;
            d[0] = (unsigned char)(sum[bgra ? 2 : 0] / n);
            d[1] = (unsigned char)(sum[1] / n);
            d[2] = (unsigned char)(sum[bgra ? 0 : 2] / n);
            d[3] = 255;
        }
    }
    return true;
}

static void fit(Video *v) {
    double s = fmin((double)v->max_w / v->vw, (double)v->max_h / v->vh);
    if (s > 1) s = 1;
    v->fw = (int)fmax(1, round(v->vw * s));
    v->fh = (int)fmax(1, round(v->vh * s));
}

/* Drops frames behind the one showing at `t` (the last at or before it). Under lock. */
static void drop_old(Video *v, double t) {
    int keep = 0;
    while (keep + 1 < v->count && v->ring[keep + 1].t <= t) keep++;
    for (int i = 0; i < keep; i++) v->spare[v->spares++] = v->ring[i].px;
    memmove(v->ring, v->ring + keep, sizeof v->ring[0] * (size_t)(v->count - keep));
    v->count -= keep;
}

static void clear(Video *v) {
    for (int i = 0; i < v->count; i++) v->spare[v->spares++] = v->ring[i].px;
    v->count = 0;
}

#ifdef _WIN32
static DWORD WINAPI worker(LPVOID arg)
#else
static void *worker(void *arg)
#endif
{
    Video *v = arg;
    bare__media_thread_begin();
    bool gpu = true;
    BareMediaReader *r = bare__media_reader_open(v->path, true, v->max_w, v->max_h);
    if (!r) gpu = false, r = bare__media_reader_open(v->path, false, 0, 0);
    lock(&v->lock);
    if (r) {
        bare__media_reader_info(r, &v->duration, &v->vw, &v->vh, &v->fps);
        if (v->vw > 0 && v->vh > 0) fit(v), v->ready = true;
    }
    if (!v->ready) v->failed = true;
    double at = -1;           /* the reader's place: the last frame it gave */
    int errors = 0;
    while (v->ready && !v->quit) {
        double want = v->want;
        double first = v->count ? v->ring[0].t : at;
        double last = v->count ? v->ring[v->count - 1].t : at;
        if (want < first - 0.05 || want > last + SEEK_S) {   /* a jump: start reading from there */
            clear(v);
            unlock(&v->lock);
            bare__media_reader_seek(r, want);
            lock(&v->lock);
            at = want - 1e-3;
            v->ended = false;
            continue;
        }
        drop_old(v, want);
        if (v->ended || v->count == RING || (v->count && v->ring[v->count - 1].t >= want + AHEAD_S)) {
            cond_wait_ms(&v->cond, &v->lock, 50);
            continue;
        }
        unsigned char *px = v->spares ? v->spare[--v->spares] : malloc((size_t)v->fw * (size_t)v->fh * 4);
        if (!px) { cond_wait_ms(&v->cond, &v->lock, 50); continue; }
        Shrink s = { px, v->fw, v->fh };
        unlock(&v->lock);
        double t = 0;
        int got = bare__media_reader_next(r, shrink, &s, &t);
        if (got < 0 && gpu && at < 0) {   /* no GPU decoder for this one after all: the CPU */
            bare__media_reader_close(r);
            gpu = false;
            r = bare__media_reader_open(v->path, false, 0, 0);
            if (r) bare__media_reader_seek(r, want);
            lock(&v->lock);
            v->spare[v->spares++] = px;
            if (!r) v->failed = true, v->ready = false;
            continue;
        }
        lock(&v->lock);
        if (got <= 0) {
            v->spare[v->spares++] = px;
            if (got == 0 || ++errors > 8) v->ended = true;
            continue;
        }
        errors = 0;
        at = t;
        if (v->count && t <= v->ring[v->count - 1].t) {   /* out of order (a seek's leftovers): keep the order */
            v->spare[v->spares++] = px;
            continue;
        }
        v->ring[v->count++] = (Frame){ t, px };
        if (t >= v->want - 0.1) bare_wake();   /* the frame the app is waiting for may be this one */
    }
    unlock(&v->lock);
    bare__media_reader_close(r);
    bare__media_thread_end();
    bare_wake();
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ---- the app's side ---------------------------------------------------------- */

BareVideo bare_video_load_(const char *path, BareVideoOptions o) {
    if (!path || !*path) return 0;
    int slot = -1;
    for (int i = 0; i < MAX_VIDEOS && slot < 0; i++) if (!g_videos[i].used) slot = i;
    if (slot < 0) return 0;
    Video *v = &g_videos[slot];
    memset(v, 0, sizeof *v);
    size_t n = strlen(path);
    v->path = malloc(n + 1);
    if (!v->path) return 0;
    memcpy(v->path, path, n + 1);
    v->max_w = o.width > 0 ? o.width : 320;
    v->max_h = o.height > 0 ? o.height : 180;
    lock_init(&v->lock);
    cond_init(&v->cond);
    v->used = true;
#ifdef _WIN32
    v->thread = CreateThread(NULL, 0, worker, v, 0, NULL);
    bool started = v->thread != NULL;
#else
    bool started = pthread_create(&v->thread, NULL, worker, v) == 0;
#endif
    if (!started) {
        free(v->path);
        memset(v, 0, sizeof *v);
        return 0;
    }
    return (BareVideo)(slot + 1);
}

const unsigned char *bare_video_pixels(BareVideo video, double seconds, int *width, int *height) {
    Video *v = get(video);
    if (width) *width = 0;
    if (height) *height = 0;
    if (!v) return NULL;
    lock(&v->lock);
    if (v->want != seconds) {
        v->want = seconds;
        cond_one(&v->cond);
    }
    const Frame *f = NULL;
    for (int i = 0; i < v->count && v->ring[i].t <= seconds + 1e-3; i++) f = &v->ring[i];
    if (!f && v->count && v->ring[0].t - seconds < 0.25) f = &v->ring[0];   /* just after a seek */
    if (f) {
        size_t bytes = (size_t)v->fw * (size_t)v->fh * 4;
        if (!v->out || v->out_w != v->fw || v->out_h != v->fh) {
            free(v->out);
            v->out = malloc(bytes);
            v->out_w = v->fw, v->out_h = v->fh;
        }
        if (v->out) memcpy(v->out, f->px, bytes), v->have_out = true;
    }
    unlock(&v->lock);
    if (!v->have_out || !v->out) return NULL;
    if (width) *width = v->out_w;
    if (height) *height = v->out_h;
    return v->out;
}

BareVideoInfo bare_video_info(BareVideo video) {
    BareVideoInfo info = { 0 };
    Video *v = get(video);
    if (!v) { info.failed = true; return info; }
    lock(&v->lock);
    info.ready = v->ready;
    info.failed = v->failed;
    info.duration = v->duration;
    info.width = v->vw, info.height = v->vh;
    info.fps = v->fps;
    unlock(&v->lock);
    return info;
}

void bare_video_free(BareVideo video) {
    Video *v = get(video);
    if (!v) return;
    lock(&v->lock);
    v->quit = true;
    cond_all(&v->cond);
    unlock(&v->lock);
#ifdef _WIN32
    WaitForSingleObject(v->thread, INFINITE);
    CloseHandle(v->thread);
#else
    pthread_join(v->thread, NULL);
#endif
    clear(v);
    for (int i = 0; i < v->spares; i++) free(v->spare[i]);
    free(v->out);
    free(v->path);
    memset(v, 0, sizeof *v);
}

/* bare_quit: every video's thread stops. */
void bare__videos_stop(void) {
    for (int i = 0; i < MAX_VIDEOS; i++) if (g_videos[i].used) bare_video_free((BareVideo)(i + 1));
}
