/* screenplus_wallpaper.h: a Bare app as a Screen+ live wallpaper.
 *
 * Screen+ starts the program as
 *     app.exe --screenplus-parent <hwnd> --screenplus-size <w>x<h>
 * and the app opens its window inside that one (bare_window_with .parent), so
 * it sits behind the desktop icons from its first frame. The desktop's mouse
 * arrives as ordinary mouse input. Customise settings (the LivelyProperties.json
 * beside the exe), pause and volume arrive on stdin, one JSON line each:
 *     {"type":"property","name":"speed","value":150}
 *     {"type":"pause","value":true}
 *     {"type":"volume","value":40}
 * Every property comes once at start, then each change. The settings live in
 * Customise, never on the wallpaper: `app.exe --screenplus-properties` prints
 * them (LivelyProperties.json, built in by embed_properties.cmake) and exits,
 * so Screen+ finds them with no file beside the exe. Data a web wallpaper
 * gets from Lively's API comes the same way once asked for with sp_want:
 *     {"type":"audio","value":[64 numbers]}       the sound's spectrum (livelyAudioListener), ~30 a second
 *     {"type":"system","value":"<JSON>"}          livelySystemInformation's data, 4 a second
 *     {"type":"track","value":"<JSON>"}           livelyCurrentTrack's data (null: nothing playing), on change
 *
 *     SpArgs a = sp_args(argc, argv);         // also takes name=value arguments, for trying settings by hand
 *     bare_window_with(.title = "Aurora", .parent = a.parent);
 *     sp_want(SP_WANT_AUDIO);                 // optional
 *     while (bare_frame()) {
 *         SpMessage m;
 *         while (sp_next(&m)) ...             // m.kind, m.name, m.value (text: 150, true, #C03030),
 *     }                                       // m.bands (audio), m.text (system, track)
 *
 * The reader thread sleeps in a blocking read and wakes Bare when a line
 * arrives, so a paused wallpaper costs nothing. Without a parent (started by
 * hand) nothing reads stdin; --system=<JSON> and --track=<JSON> arguments
 * stand in for that data. */
#ifndef SCREENPLUS_WALLPAPER_H
#define SCREENPLUS_WALLPAPER_H
#include "bare.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef SP_PROPERTIES_H   /* the settings, built in by screenplus_embed_properties (embed_properties.cmake) */
#include SP_PROPERTIES_H
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

typedef enum SpKind { SP_PROPERTY, SP_PAUSE, SP_VOLUME, SP_AUDIO, SP_SYSTEM, SP_TRACK } SpKind;

typedef struct SpMessage {
    SpKind kind;
    char name[64];
    char value[256];   /* a string's text, else the JSON as written: 150, 0.5, true */
    float bands[64];   /* SP_AUDIO: the spectrum, low to high, 0..1 */
    int nbands;
    char *text;        /* SP_SYSTEM, SP_TRACK: the JSON text ("null" when no track plays); freed by the next sp_next */
} SpMessage;

typedef struct SpArgs {
    uintptr_t parent;  /* 0: started by hand, a normal window */
    int width, height; /* the wallpaper's size in pixels (0 if not given) */
    int nprops;        /* name=value arguments, queued as SP_PROPERTY messages */
} SpArgs;

enum { SP_WANT_AUDIO = 1, SP_WANT_SYSTEM = 2, SP_WANT_TRACK = 8 };

/* Math.random for ports of pages: the C library's generator, or mulberry32 when
 * SCREENPLUS_SEED is set, so a test can run the page with the same generator
 * and seed (and the same clock, BARE_FIXED_DT) and compare frames. */
static inline double sp_random(void) {
    static int seeded = -1;
    static uint32_t s;
    if (seeded < 0) {
        const char *e = getenv("SCREENPLUS_SEED");
        seeded = e && *e;
        if (seeded) s = (uint32_t)strtoul(e, NULL, 10);
        else srand((unsigned)time(NULL));
    }
    if (!seeded) return (double)rand() / ((double)RAND_MAX + 1);
    uint32_t a = s += 0x6D2B79F5u;
    uint32_t t = (a ^ (a >> 15)) * (1u | a);
    t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
    return (double)(t ^ (t >> 14)) / 4294967296.0;
}

/* Date.now(): wall-clock milliseconds, or SCREENPLUS_EPOCH + bare_time() when
 * that is set, for tests that run a page on a virtual clock. */
static inline double sp_now_ms(void) {
    const char *e = getenv("SCREENPLUS_EPOCH");
    if (e && *e) return atof(e) + bare_time() * 1000.0;
#ifdef _WIN32
    FILETIME ft;   /* 100 ns since 1601 */
    GetSystemTimeAsFileTime(&ft);
    return (double)(((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10000ULL) - 11644473600000.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

static inline double sp_num(const SpMessage *m) { return atof(m->value); }
static inline bool sp_bool(const SpMessage *m) {
    return !strcmp(m->value, "true") || (m->value[0] && m->value[0] != '0' && strcmp(m->value, "false") && atof(m->value) != 0);
}

/* ---- queue: the reader thread pushes, the main thread pops ---------------- */

#define SP_QUEUE 256
static struct {
    SpMessage items[SP_QUEUE];
    int head, count;
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
    bool ready;
    char *last_text;   /* the text sp_next handed out last */
    uintptr_t parent;
} sp_q;

static inline void sp__lock(void) {
#ifdef _WIN32
    EnterCriticalSection(&sp_q.lock);
#else
    pthread_mutex_lock(&sp_q.lock);
#endif
}
static inline void sp__unlock(void) {
#ifdef _WIN32
    LeaveCriticalSection(&sp_q.lock);
#else
    pthread_mutex_unlock(&sp_q.lock);
#endif
}

static inline void sp__push(const SpMessage *m) {
    sp__lock();
    SpMessage *last = sp_q.count ? &sp_q.items[(sp_q.head + sp_q.count - 1) % SP_QUEUE] : NULL;
    if (m->kind == SP_AUDIO && last && last->kind == SP_AUDIO) {   /* not read yet: only the newest counts */
        *last = *m;
        sp__unlock();
        return;
    }
    if (sp_q.count == SP_QUEUE) {   /* full: the oldest goes */
        free(sp_q.items[sp_q.head].text);
        sp_q.head = (sp_q.head + 1) % SP_QUEUE;
        sp_q.count--;
    }
    sp_q.items[(sp_q.head + sp_q.count++) % SP_QUEUE] = *m;
    sp__unlock();
}

static inline bool sp_next(SpMessage *out) {
    if (!sp_q.ready) return false;
    free(sp_q.last_text);
    sp_q.last_text = NULL;
    sp__lock();
    bool any = sp_q.count > 0;
    if (any) { *out = sp_q.items[sp_q.head]; sp_q.head = (sp_q.head + 1) % SP_QUEUE; sp_q.count--; }
    sp__unlock();
    if (any) sp_q.last_text = out->text;
    return any;
}

/* ---- one line of Screen+'s JSON (its own fixed shape, not JSON in general) */

/* The value after "key": in json: a string unescaped into out (\uXXXX as UTF-8),
 * anything else as written up to , } or ]. Also reads one level of Lively's
 * data: sp_json(m.text, "Title", title, sizeof title). */
static inline bool sp_json(const char *json, const char *key, char *out, size_t cap) {
    char pat[72];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = json ? strstr(json, pat) : NULL;
    if (!p || !cap) return false;
    p += strlen(pat);
    while (*p == ' ') p++;
    size_t n = 0;
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++) {
            char c = *p;
            if (c == '\\' && p[1]) {
                c = *++p;
                if (c == 'n') c = '\n';
                else if (c == 't') c = '\t';
                else if (c == 'r') c = '\r';
                else if (c == 'u') {
                    unsigned u = 0;
                    for (int i = 0; i < 4 && p[1]; i++) {
                        char h = *++p;
                        u = u * 16 + (unsigned)(h >= 'a' ? h - 'a' + 10 : h >= 'A' ? h - 'A' + 10 : h - '0');
                    }
                    char enc[4];
                    int k;
                    if (u < 0x80) enc[0] = (char)u, k = 1;
                    else if (u < 0x800) enc[0] = (char)(0xC0 | u >> 6), enc[1] = (char)(0x80 | (u & 63)), k = 2;
                    else enc[0] = (char)(0xE0 | u >> 12), enc[1] = (char)(0x80 | (u >> 6 & 63)),
                         enc[2] = (char)(0x80 | (u & 63)), k = 3;
                    for (int i = 0; i < k; i++)
                        if (n + 1 < cap) out[n++] = enc[i];
                    continue;
                }
            }
            if (n + 1 < cap) out[n++] = c;
        }
    } else {
        for (; *p && *p != ',' && *p != '}' && *p != ']' && *p != '\r' && *p != '\n'; p++)
            if (*p != ' ' && n + 1 < cap) out[n++] = *p;
    }
    out[n] = 0;
    return true;
}

/* A string value too long for a fixed buffer (a track's Thumbnail): malloc'd, or NULL. */
static inline char *sp_json_alloc(const char *json, const char *key) {
    char pat[72];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *at = json ? strstr(json, pat) : NULL;
    if (!at) return NULL;
    const char *p = at + strlen(pat);
    while (*p == ' ') p++;
    if (*p++ != '"') return NULL;
    size_t n = 0;
    while (p[n] && p[n] != '"') n += p[n] == '\\' && p[n + 1] ? 2 : 1;
    char *out = malloc(n + 1);
    if (out && !sp_json(at, key, out, n + 1)) { free(out); return NULL; }
    return out;
}

/* Base64 (a data: URL's prefix is skipped) to bytes, malloc'd; *size gets their count. */
static inline unsigned char *sp_base64(const char *s, int *size) {
    *size = 0;
    if (!s) return NULL;
    const char *comma = strncmp(s, "data:", 5) ? NULL : strchr(s, ',');
    if (comma) s = comma + 1;
    size_t len = strlen(s);
    unsigned char *out = malloc(len / 4 * 3 + 3);
    if (!out) return NULL;
    unsigned acc = 0;
    int bits = 0, n = 0;
    for (; *s; s++) {
        int v = *s >= 'A' && *s <= 'Z' ? *s - 'A' : *s >= 'a' && *s <= 'z' ? *s - 'a' + 26
              : *s >= '0' && *s <= '9' ? *s - '0' + 52 : *s == '+' || *s == '-' ? 62 : *s == '/' || *s == '_' ? 63 : -1;
        if (v < 0) continue;
        acc = acc << 6 | (unsigned)v, bits += 6;
        if (bits >= 8) bits -= 8, out[n++] = (unsigned char)(acc >> bits & 255);
    }
    *size = n;
    return out;
}

static inline void sp__line(const char *line) {
    char type[16];
    SpMessage m;
    memset(&m, 0, sizeof m);
    if (!sp_json(line, "type", type, sizeof type)) return;
    if (!strcmp(type, "audio")) {
        const char *p = strstr(line, "\"value\":[");
        if (!p) return;
        p += 9;
        m.kind = SP_AUDIO;
        while (*p && *p != ']' && m.nbands < 64) {
            char *end;
            m.bands[m.nbands++] = (float)strtod(p, &end);
            if (end == p) break;
            p = end;
            while (*p == ',' || *p == ' ') p++;
        }
    } else if (!strcmp(type, "system") || !strcmp(type, "track")) {
        m.kind = type[0] == 's' ? SP_SYSTEM : SP_TRACK;
        m.text = sp_json_alloc(line, "value");
        if (!m.text) return;
    } else {
        if (!sp_json(line, "value", m.value, sizeof m.value)) return;
        if (!strcmp(type, "property")) {
            m.kind = SP_PROPERTY;
            if (!sp_json(line, "name", m.name, sizeof m.name)) return;
        } else if (!strcmp(type, "pause")) m.kind = SP_PAUSE;
        else if (!strcmp(type, "volume")) m.kind = SP_VOLUME;
        else return;
    }
    sp__push(&m);
    bare_wake();
}

#ifdef _WIN32
static DWORD WINAPI sp__reader(LPVOID arg) {
#else
static void *sp__reader(void *arg) {
#endif
    (void)arg;
    size_t cap = 8192, n = 0;
    char *line = malloc(cap);
    int c;
    while (line && (c = getchar()) != EOF) {   /* lines can be long: a track carries its picture */
        if (c == '\n') {
            line[n] = 0;
            sp__line(line);
            n = 0;
            continue;
        }
        if (n + 1 >= cap) {
            char *bigger = cap < ((size_t)64 << 20) ? realloc(line, cap * 2) : NULL;
            if (!bigger) { n = 0; continue; }   /* absurdly long: dropped */
            line = bigger, cap *= 2;
        }
        line[n++] = (char)c;
    }
    free(line);
    return 0;   /* Screen+ closed the pipe: it ends the program itself */
}

static inline SpArgs sp_args(int argc, char **argv) {
    SpArgs a = { 0 };
    if (!sp_q.ready) {
#ifdef _WIN32
        InitializeCriticalSection(&sp_q.lock);
#else
        pthread_mutex_init(&sp_q.lock, NULL);
#endif
        sp_q.ready = true;
    }
    for (int i = 1; i < argc; i++)   /* Screen+ asks for the settings (Customise) and nothing else */
        if (!strcmp(argv[i], "--screenplus-properties")) {
#ifdef SP_PROPERTIES_H
            fputs(sp_properties_json, stdout);
#else
            fputs("{}", stdout);
#endif
            fflush(stdout);
            exit(0);
        }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--screenplus-parent") && i + 1 < argc) a.parent = (uintptr_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--screenplus-size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &a.width, &a.height);
        else if (!strncmp(argv[i], "--system=", 9) || !strncmp(argv[i], "--track=", 8)) {   /* data by hand */
            SpMessage m;
            memset(&m, 0, sizeof m);
            m.kind = argv[i][2] == 's' ? SP_SYSTEM : SP_TRACK;
            const char *v = strchr(argv[i], '=') + 1;
            m.text = malloc(strlen(v) + 1);
            if (m.text) strcpy(m.text, v), sp__push(&m);
        } else if (!strncmp(argv[i], "--audio=", 8)) {   /* one spectrum by hand: comma-separated bands */
            char line[4096];
            snprintf(line, sizeof line, "{\"type\":\"audio\",\"value\":[%s]}", argv[i] + 8);
            sp__line(line);
        } else if (strchr(argv[i], '=')) {
            SpMessage m;
            memset(&m, 0, sizeof m);
            m.kind = SP_PROPERTY;
            const char *eq = strchr(argv[i], '=');
            snprintf(m.name, sizeof m.name, "%.*s", (int)(eq - argv[i]), argv[i]);
            snprintf(m.value, sizeof m.value, "%s", eq + 1);
            sp__push(&m);
            a.nprops++;
        }
    }
    sp_q.parent = a.parent;
    if (a.parent) {
#ifdef _WIN32
        HANDLE t = CreateThread(NULL, 0, sp__reader, NULL, 0, NULL);
        if (t) CloseHandle(t);
#else
        pthread_t t;
        if (!pthread_create(&t, NULL, sp__reader, NULL)) pthread_detach(t);
#endif
    }
    return a;
}

/* Asks Screen+ for data (SP_WANT_*), after bare_window_with: the window says so
 * with the property "ScreenPlus.Wants", which Screen+ reads in its first seconds. */
#ifdef _WIN32
static BOOL CALLBACK sp__mark(HWND hwnd, LPARAM wants) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) SetPropW(hwnd, L"ScreenPlus.Wants", (HANDLE)(INT_PTR)wants);
    return TRUE;
}
#endif
static inline void sp_want(int wants) {
#ifdef _WIN32
    if (sp_q.parent) EnumChildWindows((HWND)sp_q.parent, sp__mark, (LPARAM)wants);   /* Bare's window is a child */
#else
    (void)wants;   /* Screen+ runs app wallpapers on Windows only */
#endif
}

/* A file the app ships beside it: <exe folder>/assets/<name>, else the source
 * tree's (ASSETS_DIR, set by the build), else assets/<name> from the working folder. */
static inline const char *sp_asset(const char *name) {
    static char path[4200];
    char dir[4096] = "";
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, dir, sizeof dir);
    if (n == 0 || n >= sizeof dir) dir[0] = 0;
    char *slash = strrchr(dir, '\\');
#else
    ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
    dir[n > 0 ? n : 0] = 0;
    char *slash = strrchr(dir, '/');
#endif
    FILE *f;
    if (slash) {
        slash[1] = 0;
        snprintf(path, sizeof path, "%s%s", dir, name);   /* beside the program, as a .spf carries them */
        if ((f = fopen(path, "rb"))) { fclose(f); return path; }
        snprintf(path, sizeof path, "%sassets/%s", dir, name);
        if ((f = fopen(path, "rb"))) { fclose(f); return path; }
    }
#ifdef ASSETS_DIR
    snprintf(path, sizeof path, "%s/%s", ASSETS_DIR, name);
    if ((f = fopen(path, "rb"))) { fclose(f); return path; }
#endif
    snprintf(path, sizeof path, "assets/%s", name);
    return path;
}

#endif
