/* picture.c: bare_picture, files loaded in the background, kept while drawn.
 *
 * One worker thread decodes, newest request first (what is on screen now),
 * and shrinks every frame to the size asked for as it goes, so a 4K video
 * costs no more than its thumbnail. stb_image reads PNG / JPEG / BMP / TGA and
 * every frame of a GIF; the system's decoders (src/backend/media.h) read the
 * rest and video. Moving pictures keep their frames here and play by
 * bare_image_update while they are drawn; one not asked for in KEEP_S is
 * freed. */
#include "bare.h"
#include "backend/media.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
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
#define RETRY_S      5.0             /* one that couldn't be read is tried again after this */
#define MAX_FILE     (256u << 20)    /* bigger files aren't read into memory */
#define GIF_BUDGET   (512u << 20)    /* a GIF whose frames would take more shows its first */
#define MAX_SIDE     4096            /* the largest picture kept when no size is asked for */
#define VIDEO_FPS    10.0f

/* ---- threads --------------------------------------------------------------- */

#ifdef _WIN32
typedef SRWLOCK Lock;
static void lock_init(Lock *l) { InitializeSRWLock(l); }
static void lock(Lock *l) { AcquireSRWLockExclusive(l); }
static void unlock(Lock *l) { ReleaseSRWLockExclusive(l); }
#else
typedef pthread_mutex_t Lock;
static void lock_init(Lock *l) { pthread_mutex_init(l, NULL); }
static void lock(Lock *l) { pthread_mutex_lock(l); }
static void unlock(Lock *l) { pthread_mutex_unlock(l); }
#endif

typedef struct Job {
    char *path;
    int width, height, frames;
    bool still;
    int slot;
    uint32_t gen;
} Job;

typedef struct Result {
    struct Result *next;
    int slot;
    uint32_t gen;
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
#ifdef _WIN32
    HANDLE thread, event;
#else
    pthread_t thread;
    pthread_cond_t cond;
#endif
    Job queue[MAX_QUEUE];
    int queued;
    Result *results;
    Pic pics[MAX_PICTURES];
    int count;
    uint32_t next_gen;
    double swept;
} P;

/* ---- decoding (worker) ----------------------------------------------------- */

typedef struct Out {
    int want_w, want_h, max_frames;
    int w, h, count, cap;
    unsigned char *rgba;
    int *delays;
} Out;

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

static void decode(const Job *j, Out *o) {
    o->want_w = j->width, o->want_h = j->height;
    o->max_frames = j->still ? 1 : j->frames;
    if (is_video(j->path)) {
        bare__media_video(j->path, o->max_frames, VIDEO_FPS, add_frame, o);
        return;
    }
    size_t n = 0;
    unsigned char *data = read_file(j->path, &n);
    if (data && n <= 0x7fffffff) {
        int gw = 0, gh = 0, frames = j->still ? 0 : gif_frames(data, n, &gw, &gh);
        if (frames > 1 && (uint64_t)gw * gh * 4 * frames <= GIF_BUDGET) {
            int *delays = NULL, w = 0, h = 0, z = 0, comp = 0;
            unsigned char *px = stbi_load_gif_from_memory(data, (int)n, &delays, &w, &h, &z, &comp, 4);
            if (px && z > 0) {
                /* evenly thinned to max_frames; each kept frame lasts as long as those it stands for */
                int keep = z < o->max_frames ? z : o->max_frames;
                for (int k = 0; k < keep; k++) {
                    int a = (int)((int64_t)k * z / keep), b = (int)((int64_t)(k + 1) * z / keep), ms = 0;
                    for (int i = a; i < b; i++) ms += delays && delays[i] > 10 ? delays[i] : 100;   /* as browsers do */
                    if (!add_frame(o, px + (size_t)w * h * 4 * a, w, h, w * 4, false, ms) && o->count < keep) break;
                }
            }
            stbi_image_free(px);
            free(delays);
        }
        if (!o->count) {
            int w = 0, h = 0, comp = 0;
            unsigned char *px = stbi_load_from_memory(data, (int)n, &w, &h, &comp, 4);
            if (px) add_frame(o, px, w, h, w * 4, false, 0);
            stbi_image_free(px);
        }
    }
    free(data);
    if (!o->count) bare__media_image(j->path, add_frame, o);   /* WebP, HEIC, TIFF, ... */
    if (!o->count) bare__media_video(j->path, o->max_frames, VIDEO_FPS, add_frame, o);
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
        Job j = { 0 };
        bool have = false;
        lock(&P.lock);
#ifndef _WIN32
        while (!P.quit && !P.queued) pthread_cond_wait(&P.cond, &P.lock);
#endif
        if (P.quit) { unlock(&P.lock); break; }
        if (P.queued) { j = P.queue[--P.queued]; have = true; }   /* newest first */
        unlock(&P.lock);
        if (!have) {
#ifdef _WIN32
            WaitForSingleObject(P.event, INFINITE);
#endif
            continue;
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

static bool start(void) {
    if (P.started) return true;
    lock_init(&P.lock);
#ifdef _WIN32
    P.event = CreateEventW(NULL, FALSE, FALSE, NULL);
    P.thread = P.event ? CreateThread(NULL, 0, worker, NULL, 0, NULL) : NULL;
    if (!P.thread) return false;
    SetThreadPriority(P.thread, THREAD_PRIORITY_BELOW_NORMAL);
#else
    pthread_cond_init(&P.cond, NULL);
    if (pthread_create(&P.thread, NULL, worker, NULL)) return false;
#endif
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
        if (p && !p->done) {
            p->done = true;
            p->queued = false;
            p->done_at = bare_time();
            if (r->count > 0) {
                p->image = bare_image_from_pixels(r->w, r->h, r->rgba);
                p->w = r->w, p->h = r->h, p->count = r->count, p->shown = 0, p->start = bare_time();
                if (r->count > 1) {
                    p->rgba = r->rgba, p->delays = r->delays;
                    r->rgba = NULL, r->delays = NULL;
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
    P.queue[P.queued++] = (Job){ copy, p->width, p->height, p->frames, p->still, 0, p->gen };
    unlock(&P.lock);
    p->queued = true;
#ifdef _WIN32
    SetEvent(P.event);
#else
    pthread_cond_signal(&P.cond);
#endif
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
        bare_image_update(p->image, p->rgba + (size_t)p->w * p->h * 4 * (size_t)k);
        p->shown = k;
    }
    bare_wake_in((float)(end - at) / 1000.0f);
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
        return 0;
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
    unlock(&P.lock);
#ifdef _WIN32
    SetEvent(P.event);
    bool ended = WaitForSingleObject(P.thread, 2000) == WAIT_OBJECT_0;   /* a decoder may be slow to give up */
    CloseHandle(P.thread);
    if (ended) CloseHandle(P.event);
#else
    pthread_cond_signal(&P.cond);
    pthread_join(P.thread, NULL);
    bool ended = true;
#endif
    if (!ended) return;   /* the thread still uses the queue: leave it to the process's end */
    while (P.queued) free(P.queue[--P.queued].path);
    take_results();
    while (P.count) {
        drop(P.count - 1);
    }
    memset(&P, 0, sizeof P);
}
