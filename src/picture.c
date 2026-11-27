/* picture.c: bare_picture, files loaded in the background, kept while drawn.
 *
 * A few worker threads decode, newest request first (what is on screen now),
 * and shrink every frame to the size asked for as it goes, so a 4K video
 * costs no more than its thumbnail. Work goes in two passes so everything
 * shows something soon: the first reads small files whole and gives big
 * ones and videos the system's cached thumbnail; the second reads those in
 * full (a moving picture's first frame showing as soon as it's decoded). stb_image reads PNG / JPEG / BMP / TGA and GIFs (one
 * frame at a time, so a long one costs only the frames kept); the system's
 * decoders (src/backend/media.h) read the rest and video, on the GPU when
 * they can. A moving picture's frames go to the GPU once as one strip
 * (bare_image_frames) and play by picking the frame, all stepping together
 * on one PICTURE_FPS clock; one not asked for in KEEP_S is freed. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L   /* clock_gettime */
#endif
#include "bare.h"
#include "backend/media.h"
#include "thread.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#if defined(_MSC_VER)
#  pragma warning(push, 0)
#  pragma warning(disable : 4244 4245 4456 4457 4701 4702 4706)
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wsign-compare"
#  pragma GCC diagnostic ignored "-Wunused-parameter"
#  pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "stb_image.h"
#if defined(_MSC_VER)
#  pragma warning(pop)
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

#define MAX_PICTURES 512
#define MAX_QUEUE    128
#define KEEP_S       10.0            /* a picture not asked for this long is freed */
#define PICTURE_FPS  24.0            /* moving pictures change frame at most this often, all at once */
#define RETRY_S      5.0             /* one that couldn't be read is tried again after this */
#define MAX_FILE     (256u << 20)    /* bigger files aren't read into memory */
#define GIF_BUDGET   (512u << 20)    /* a GIF whose frames would take more shows its first */
#define MAX_SIDE     4096            /* the largest picture kept when no size is asked for */
#define VIDEO_FPS    10.0f
#define MAX_WORKERS  4
#define BIG_FILE     (1u << 20)      /* a file this big shows the system's cached thumbnail while it's read */
#define IDLE_S       30              /* a decoding thread with nothing to do this long lets its GPU device go */
#define MAX_STRIP    16384           /* a moving picture's frames, stacked, up to this tall live on the GPU */

/* ---- threads: src/thread.h ------------------------------------------------- */

typedef struct Job {
    char *path;
    int width, height, frames;
    bool still;
    int slot;
    uint32_t gen;
    bool later;               /* the second pass: big files and videos, read in full */
    bool shown;               /* something was sent to show early */
} Job;

typedef struct Result {
    struct Result *next;
    int slot;
    uint32_t gen;
    bool early;               /* something to show while the rest is read */
    int w, h, count;          /* count 0: couldn't be read */
    unsigned char *rgba;      /* count frames of w x h */
    int *delays;              /* ms each */
} Result;

typedef struct Pic {
    char *path;
    int width, height, frames;
    bool still;
    uint32_t hash, gen;
    bool queued, done;
    BareImage image;
    unsigned char *rgba;
    int *delays;
    int w, h, count, total_ms, shown;
    double used, start, done_at;
} Pic;

static struct {
    bool started, quit;
    Lock lock;
    Cond cond;
    Thread threads[MAX_WORKERS];
    int workers;
    Job queue[MAX_QUEUE];
    int queued;
    Job later[MAX_PICTURES];  /* never fuller than the pictures waiting */
    int nlater;
    Result *results;
    Pic pics[MAX_PICTURES];
    int count;
    uint32_t next_gen;
    double swept;
} P;

/* ---- decoding (worker) ----------------------------------------------------- */

typedef struct Out {
    const Job *job;
    int want_w, want_h, max_frames;
    int w, h, count, cap;
    unsigned char *rgba;
    int *delays;
    bool early_sent;          /* a first picture went to the app already */
} Out;

static void deliver(Result *r);

/* Sends `rgba` (w x h) to show while the rest is read. */
static void send_early(Out *o, const unsigned char *rgba, int w, int h) {
    Result *r = calloc(1, sizeof *r);
    if (!r || !(r->rgba = malloc((size_t)w * h * 4))) { free(r); return; }
    memcpy(r->rgba, rgba, (size_t)w * h * 4);
    r->slot = o->job->slot, r->gen = o->job->gen, r->early = true;
    r->w = w, r->h = h, r->count = 1;
    o->early_sent = true;
    deliver(r);
}

/* The size to keep: what was asked, or the picture's own (no bigger than MAX_SIDE), keeping its shape. */
static void out_size(Out *o, int sw, int sh) {
    int w = o->want_w, h = o->want_h;
    if (w <= 0 && h <= 0) {
        w = sw, h = sh;
        if (w > MAX_SIDE || h > MAX_SIDE) {
            if (w >= h) h = (int)((int64_t)h * MAX_SIDE / w), w = MAX_SIDE;
            else w = (int)((int64_t)w * MAX_SIDE / h), h = MAX_SIDE;
        }
    } else if (w <= 0) {
        w = (int)((int64_t)sw * h / sh);
    } else if (h <= 0) {
        h = (int)((int64_t)sh * w / sw);
    }
    o->w = w < 1 ? 1 : w;
    o->h = h < 1 ? 1 : h;
}

/* Fills dst (dw x dh RGBA) from the middle of src that has dst's shape, averaging (up to 4 x 4 samples). */
static void cover(const unsigned char *src, int sw, int sh, int pitch, bool bgra, unsigned char *dst, int dw, int dh) {
    double cw = sw, ch = sh;
    if ((double)sw / sh > (double)dw / dh) cw = (double)sh * dw / dh;
    else ch = (double)sw * dh / dw;
    double x0 = (sw - cw) * 0.5, y0 = (sh - ch) * 0.5, sx = cw / dw, sy = ch / dh;
    int ri = bgra ? 2 : 0, bi = bgra ? 0 : 2;
    for (int y = 0; y < dh; y++) {
        int ya = (int)(y0 + y * sy), yb = (int)(y0 + (y + 1) * sy);
        if (yb <= ya) yb = ya + 1;
        if (yb > sh) yb = sh;
        if (ya >= sh) ya = sh - 1;
        int ystep = (yb - ya + 3) / 4;
        unsigned char *d = dst + (size_t)y * dw * 4;
        for (int x = 0; x < dw; x++, d += 4) {
            int xa = (int)(x0 + x * sx), xb = (int)(x0 + (x + 1) * sx);
            if (xb <= xa) xb = xa + 1;
            if (xb > sw) xb = sw;
            if (xa >= sw) xa = sw - 1;
            int xstep = (xb - xa + 3) / 4;
            unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int yy = ya; yy < yb; yy += ystep) {
                const unsigned char *row = src + (ptrdiff_t)yy * pitch;
                for (int xx = xa; xx < xb; xx += xstep, n++) {
                    const unsigned char *p = row + (size_t)xx * 4;
                    r += p[ri], g += p[1], b += p[bi], a += bgra ? 255 : p[3];
                }
            }
            d[0] = (unsigned char)(r / n), d[1] = (unsigned char)(g / n), d[2] = (unsigned char)(b / n);
            d[3] = (unsigned char)(a / n);
        }
    }
}

static bool add_frame(void *user, const unsigned char *px, int w, int h, int pitch, bool bgra, int delay_ms) {
    Out *o = user;
    if (w <= 0 || h <= 0) return false;
    if (!o->rgba) {
        out_size(o, w, h);
        size_t frame = (size_t)o->w * o->h * 4;
        int cap = o->max_frames;
        while (cap > 1 && frame * (size_t)cap > GIF_BUDGET) cap--;
        o->rgba = malloc(frame * (size_t)cap);
        o->delays = malloc(sizeof(int) * (size_t)cap);
        if (!o->rgba || !o->delays) return false;
        o->cap = cap;
    }
    if (o->count >= o->cap) return false;
    cover(px, w, h, pitch, bgra, o->rgba + (size_t)o->w * o->h * 4 * (size_t)o->count, o->w, o->h);
    o->delays[o->count++] = delay_ms;
    if (o->count == 1 && o->cap > 1 && o->job && !o->early_sent) send_early(o, o->rgba, o->w, o->h);   /* first frame */
    return o->count < o->cap;
}

static unsigned char *read_file(const char *path, size_t *size) {
    FILE *f = NULL;
#ifdef _WIN32
    wchar_t wpath[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) > 0) f = _wfopen(wpath, L"rb");
#else
    f = fopen(path, "rb");
#endif
    if (!f) return NULL;
    unsigned char *data = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        if (n > 0 && (unsigned long)n <= MAX_FILE && fseek(f, 0, SEEK_SET) == 0 && (data = malloc((size_t)n))) {
            if (fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); data = NULL; }
            else *size = (size_t)n;
        }
    }
    fclose(f);
    return data;
}

/* Frames in a GIF and its size, without decoding it. */
static int gif_frames(const unsigned char *d, size_t n, int *w, int *h) {
    if (n < 13 || memcmp(d, "GIF8", 4)) return 0;
    *w = d[6] | d[7] << 8, *h = d[8] | d[9] << 8;
    size_t i = 13;
    if (d[10] & 0x80) i += 3u << ((d[10] & 7) + 1);
    int frames = 0;
    while (i < n) {
        unsigned char kind = d[i++];
        if (kind == 0x3B) break;
        if (kind == 0x21) {
            i++;   /* the extension's label */
        } else if (kind == 0x2C) {
            if (i + 9 > n) break;
            unsigned char packed = d[i + 8];
            i += 9;
            if (packed & 0x80) i += 3u << ((packed & 7) + 1);
            i++;   /* LZW code size */
            frames++;
        } else {
            break;
        }
        while (i < n && d[i]) i += 1u + d[i];   /* data sub-blocks */
        i++;
    }
    return frames;
}

/* A GIF's frames one at a time (stb_image's own loop keeps them all): every `total / keep`th is kept, lasting
 * as long as those it stands for. */
static void gif_stream(const unsigned char *data, size_t n, int total, Out *o) {
    stbi__context s;
    stbi__start_mem(&s, data, (int)n);
    stbi__gif *g = calloc(1, sizeof *g);
    if (!g) return;
    int keep = total < o->max_frames ? total : o->max_frames, comp = 0, k = 0;
    unsigned char *saved = NULL;   /* the frame "restore to previous" goes back to */
    for (int i = 0;; i++) {
        stbi_uc *px = stbi__gif_load_next(&s, g, &comp, 4, saved);
        if (!px || px == (stbi_uc *)&s) break;
        if (k < keep && i >= (int)((int64_t)k * total / keep)) {
            int had = o->count;
            add_frame(o, px, g->w, g->h, g->w * 4, false, 0);
            if (o->count == had) break;
            k++;
        }
        if (o->count) o->delays[o->count - 1] += g->delay > 10 ? g->delay : 100;   /* as browsers do */
        if ((g->eflags & 0x1c) != 0x0c) {
            size_t bytes = (size_t)g->w * g->h * 4;
            if (!saved) saved = malloc(bytes);
            if (!saved) break;
            memcpy(saved, px, bytes);
        }
    }
    STBI_FREE(g->out);
    free(saved);
    free(g);
}

static const char *const k_video[] = { ".mp4", ".m4v", ".mkv", ".webm", ".mov", ".avi", ".wmv", ".mpg", ".mpeg",
                                       ".ts", ".m2ts", ".flv", ".3gp", ".ogv" };

static bool is_video(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    for (size_t i = 0; i < sizeof k_video / sizeof *k_video; i++) {
        const char *a = dot, *b = k_video[i];
        while (*a && *b && (*a | 32) == *b) a++, b++;
        if (!*a && !*b) return true;
    }
    return false;
}

/* The system's cached thumbnail, sent to show while the file is read. */
static bool early_thumb(void *user, const unsigned char *px, int w, int h, int pitch, bool bgra, int delay_ms) {
    Out *o = user, t = { .want_w = o->want_w, .want_h = o->want_h, .max_frames = 1 };
    (void)delay_ms;
    add_frame(&t, px, w, h, pitch, bgra, 0);
    if (t.count) send_early(o, t.rgba, t.w, t.h);
    free(t.rgba);
    free(t.delays);
    return false;
}

static long long file_size(const char *path) {
#ifdef _WIN32
    wchar_t w[1024];
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024) <= 0 || !GetFileAttributesExW(w, GetFileExInfoStandard, &a))
        return -1;
    return (long long)a.nFileSizeHigh << 32 | a.nFileSizeLow;
#else
    struct stat st;
    return stat(path, &st) ? -1 : (long long)st.st_size;
#endif
}

static void decode_init(const Job *j, Out *o) {
    o->job = j;
    o->want_w = j->width, o->want_h = j->height;
    o->max_frames = j->still ? 1 : j->frames;
    o->early_sent = j->shown;
}

/* The first pass for a big file or a video: the system's cached thumbnail, if it has one. True: read the
 * rest in the second pass. */
static bool first_pass(Job *j) {
    if (j->later || (j->width <= 0 && j->height <= 0)) return false;
    if (!is_video(j->path) && file_size(j->path) < (long long)BIG_FILE) return false;
    Out o = { 0 };
    decode_init(j, &o);
    bare__media_thumb(j->path, j->width > j->height ? j->width : j->height, early_thumb, &o);
    j->shown = o.early_sent;
    return true;
}

static void decode(const Job *j, Out *o) {
    decode_init(j, o);
    bool video = is_video(j->path);
    size_t n = 0;
    unsigned char *data = video ? NULL : read_file(j->path, &n);
    if (video) {
        bare__media_video(j->path, o->max_frames, VIDEO_FPS, j->width, j->height, add_frame, o);
        return;
    }
    if (data && n <= 0x7fffffff) {
        int gw = 0, gh = 0, frames = j->still ? 0 : gif_frames(data, n, &gw, &gh);
        if (frames > 1) gif_stream(data, n, frames, o);
        if (!o->count) {
            int w = 0, h = 0, comp = 0;
            unsigned char *px = stbi_load_from_memory(data, (int)n, &w, &h, &comp, 4);
            if (px) add_frame(o, px, w, h, w * 4, false, 0);
            stbi_image_free(px);
        }
    }
    free(data);
    if (!o->count) bare__media_image(j->path, add_frame, o);   /* WebP, HEIC, TIFF, ... */
    if (!o->count) bare__media_video(j->path, o->max_frames, VIDEO_FPS, j->width, j->height, add_frame, o);
}

static void deliver(Result *r) {
    lock(&P.lock);
    r->next = P.results;
    P.results = r;
    unlock(&P.lock);
    bare_wake();
}

#ifdef _WIN32
static DWORD WINAPI worker(void *arg)
#else
static void *worker(void *arg)
#endif
{
    (void)arg;
    bare__media_thread_begin();
    for (;;) {
        lock(&P.lock);
        if (!P.quit && !P.queued && !P.nlater) {   /* nothing to do for IDLE_S: the decoders' GPU memory goes */
            cond_wait_ms(&P.cond, &P.lock, IDLE_S * 1000);
            if (!P.quit && !P.queued && !P.nlater) {
                unlock(&P.lock);
                bare__media_thread_idle();
                lock(&P.lock);
            }
        }
        while (!P.quit && !P.queued && !P.nlater) cond_wait(&P.cond, &P.lock);
        if (P.quit) { unlock(&P.lock); break; }
        Job j = P.queued ? P.queue[--P.queued] : P.later[--P.nlater];   /* newest first, first pass first */
        unlock(&P.lock);
        if (first_pass(&j)) {
            bool deferred = false;
            lock(&P.lock);
            if (P.nlater < MAX_PICTURES) {
                j.later = true;
                P.later[P.nlater++] = j;
                deferred = true;
            }
            unlock(&P.lock);
            if (deferred) continue;
        }
        Out o = { 0 };
        decode(&j, &o);
        Result *r = calloc(1, sizeof *r);
        if (r) {
            r->slot = j.slot, r->gen = j.gen;
            r->w = o.w, r->h = o.h, r->count = o.count, r->rgba = o.rgba, r->delays = o.delays;
            deliver(r);
        } else {
            free(o.rgba);
            free(o.delays);
        }
        free(j.path);
    }
    bare__media_thread_end();
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* Workers: one fewer than the processor's cores (it draws), 1 to MAX_WORKERS. */
static int worker_count(void) {
    int cores = 1;
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    cores = (int)si.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_ONLN)
    cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
    cores--;
    return cores < 1 ? 1 : cores > MAX_WORKERS ? MAX_WORKERS : cores;
}

static bool start(void) {
    if (P.started) return true;
    lock_init(&P.lock);
    cond_init(&P.cond);
    for (int want = worker_count(); P.workers < want; P.workers++) {
#ifdef _WIN32
        HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (!t) break;
        SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
        P.threads[P.workers] = t;
#else
        if (pthread_create(&P.threads[P.workers], NULL, worker, NULL)) break;
#endif
    }
    if (!P.workers) return false;
    P.started = true;
    return true;
}

/* ---- the app's side ---------------------------------------------------------- */

static uint32_t hash_of(const char *s, int w, int h, int frames, bool still) {
    uint32_t x = 2166136261u;
    for (; *s; s++) x = (x ^ (unsigned char)*s) * 16777619u;
    x = (x ^ (uint32_t)w) * 16777619u;
    x = (x ^ (uint32_t)h) * 16777619u;
    x = (x ^ (uint32_t)frames) * 16777619u;
    return (x ^ (uint32_t)still) * 16777619u;
}

static void drop(int i) {
    Pic *p = &P.pics[i];
    if (p->image) bare_image_free(p->image);
    free(p->path);
    free(p->rgba);
    free(p->delays);
    P.pics[i] = P.pics[--P.count];
}

/* Picks up decoded pictures. */
static void take_results(void) {
    lock(&P.lock);
    Result *r = P.results;
    P.results = NULL;
    unlock(&P.lock);
    while (r) {
        Result *next = r->next;
        Pic *p = NULL;
        for (int i = 0; i < P.count; i++)
            if (P.pics[i].gen == r->gen) { p = &P.pics[i]; break; }
        if (p && !p->done && r->early) {   /* to show until the rest comes */
            if (p->image && (p->w != r->w || p->h != r->h)) bare_image_free(p->image), p->image = 0;
            if (p->image) bare_image_update(p->image, r->rgba);
            else p->image = bare_image_from_pixels(r->w, r->h, r->rgba);
            if (p->image) p->w = r->w, p->h = r->h, p->count = 1;
        } else if (p && !p->done) {
            p->done = true;
            p->queued = false;
            p->done_at = bare_time();
            /* A moving picture's frames go to the GPU once, as one strip, and leave memory; playing only picks
             * which one shows. One too tall for a strip keeps its frames here and uploads each as it shows. */
            bool strip = r->count > 1 && (int64_t)r->h * r->count <= MAX_STRIP;
            if (r->count > 0) {
                if (p->image && (strip || p->w != r->w || p->h != r->h)) bare_image_free(p->image), p->image = 0;
                if (strip) p->image = bare_image_frames(r->w, r->h, r->count, r->rgba);
                else if (p->image) bare_image_update(p->image, r->rgba);
                else p->image = bare_image_from_pixels(r->w, r->h, r->rgba);
                p->w = r->w, p->h = r->h, p->count = r->count, p->shown = 0, p->start = bare_time(), p->total_ms = 0;
                if (r->count > 1) {
                    if (!strip) p->rgba = r->rgba, r->rgba = NULL;
                    p->delays = r->delays, r->delays = NULL;
                    for (int k = 0; k < p->count; k++) p->total_ms += p->delays[k];
                }
            }
        }
        free(r->rgba);
        free(r->delays);
        free(r);
        r = next;
    }
}

static void enqueue(Pic *p) {
    size_t n = strlen(p->path) + 1;
    char *copy = malloc(n);
    if (!copy) return;
    memcpy(copy, p->path, n);
    lock(&P.lock);
    if (P.queued == MAX_QUEUE) {   /* the oldest request goes; it is asked again if still drawn */
        Job *old = &P.queue[0];
        for (int i = 0; i < P.count; i++) if (P.pics[i].gen == old->gen) P.pics[i].queued = false;
        free(old->path);
        memmove(&P.queue[0], &P.queue[1], sizeof(Job) * (MAX_QUEUE - 1));
        P.queued--;
    }
    P.queue[P.queued++] = (Job){ copy, p->width, p->height, p->frames, p->still, 0, p->gen, false, false };
    cond_one(&P.cond);
    unlock(&P.lock);
    p->queued = true;
}

/* The frame of a moving picture for now, and a wake for the next one. */
static void play(Pic *p) {
    if (p->count < 2 || p->total_ms <= 0) return;
    double t = (bare_time() - p->start) * 1000.0;
    int at = (int)(t - (double)p->total_ms * (int64_t)(t / p->total_ms)), k = 0, end = 0;
    for (; k < p->count; k++) {
        end += p->delays[k];
        if (at < end) break;
    }
    if (k >= p->count) k = p->count - 1;
    if (k != p->shown) {
        if (p->rgba) bare_image_update(p->image, p->rgba + (size_t)p->w * p->h * 4 * (size_t)k);
        else bare_image_frame(p->image, k);
        p->shown = k;
    }
    /* Every moving picture steps on one shared clock: a window of them would otherwise wake for each one's own
     * frame times, out of step, and draw everything far more often than any of them moves. */
    double now = bare_time(), next = now + (end - at) / 1000.0;
    next = ceil(next * PICTURE_FPS) / PICTURE_FPS;
    bare_wake_in((float)(next - now));
}

BareImage bare_picture_(const char *path, BarePictureOptions o) {
    if (!path || !*path || !start()) return 0;
    if (o.frames <= 0) o.frames = 40;
    if (o.still) o.frames = 1;
    take_results();
    double now = bare_time();
    if (now - P.swept > 1.0) {   /* free what isn't drawn any more */
        P.swept = now;
        for (int i = P.count - 1; i >= 0; i--)
            if (now - P.pics[i].used > KEEP_S && !P.pics[i].queued) drop(i);
    }
    uint32_t h = hash_of(path, o.width, o.height, o.frames, o.still);
    Pic *p = NULL;
    for (int i = 0; i < P.count; i++) {
        Pic *q = &P.pics[i];
        if (q->hash == h && q->width == o.width && q->height == o.height && q->frames == o.frames &&
            q->still == o.still && !strcmp(q->path, path)) { p = q; break; }
    }
    if (!p) {
        if (P.count == MAX_PICTURES) {   /* full: the longest unused goes */
            int old = -1;
            for (int i = 0; i < P.count; i++)
                if (!P.pics[i].queued && (old < 0 || P.pics[i].used < P.pics[old].used)) old = i;
            if (old < 0) return 0;
            drop(old);
        }
        size_t n = strlen(path) + 1;
        char *copy = malloc(n);
        if (!copy) return 0;
        memcpy(copy, path, n);
        p = &P.pics[P.count++];
        *p = (Pic){ .path = copy, .width = o.width, .height = o.height, .frames = o.frames, .still = o.still,
                    .hash = h, .gen = ++P.next_gen };
    }
    p->used = now;
    if (p->done && !p->image && now - p->done_at > RETRY_S) p->done = false;   /* the file may be there now */
    if (!p->done) {
        if (!p->queued) enqueue(p);
        return p->image;   /* what shows early, or 0 */
    }
    play(p);
    return p->image;
}

bool bare_picture_failed(const char *path, BarePictureOptions o) {
    if (!path || !*path) return true;
    if (o.frames <= 0) o.frames = 40;
    if (o.still) o.frames = 1;
    for (int i = 0; i < P.count; i++) {
        const Pic *q = &P.pics[i];
        if (q->width == o.width && q->height == o.height && q->frames == o.frames && q->still == o.still &&
            !strcmp(q->path, path))
            return q->done && !q->image;
    }
    return false;
}

void bare__pictures_stop(void) {
    if (!P.started) return;
    lock(&P.lock);
    P.quit = true;
    cond_all(&P.cond);
    unlock(&P.lock);
    bool ended = true;
#ifdef _WIN32
    /* a decoder may be slow to give up */
    ended = WaitForMultipleObjects((DWORD)P.workers, P.threads, TRUE, 2000) == WAIT_OBJECT_0;
    for (int i = 0; i < P.workers; i++) CloseHandle(P.threads[i]);
#else
    for (int i = 0; i < P.workers; i++) pthread_join(P.threads[i], NULL);
#endif
    if (!ended) return;   /* a thread still uses the queue: leave it to the process's end */
    while (P.queued) free(P.queue[--P.queued].path);
    while (P.nlater) free(P.later[--P.nlater].path);
    take_results();
    while (P.count) {
        drop(P.count - 1);
    }
    memset(&P, 0, sizeof P);
}
