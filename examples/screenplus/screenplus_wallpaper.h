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
 * Every property comes once at start, then each change.
 *
 *     SpArgs a = sp_args(argc, argv);         // also takes name=value arguments, for trying settings by hand
 *     bare_window_with(.title = "Aurora", .parent = a.parent);
 *     while (bare_frame()) {
 *         SpMessage m;
 *         while (sp_next(&m)) ...             // m.kind, m.name, m.value (text: 150, true, #C03030)
 *     }
 *
 * The reader thread sleeps in a blocking read and wakes Bare when a line
 * arrives, so a paused wallpaper costs nothing. Without a parent (started by
 * hand) nothing reads stdin. */
#ifndef SCREENPLUS_WALLPAPER_H
#define SCREENPLUS_WALLPAPER_H
#include "bare.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef enum SpKind { SP_PROPERTY, SP_PAUSE, SP_VOLUME } SpKind;

typedef struct SpMessage {
    SpKind kind;
    char name[64];
    char value[256];   /* a string's text, else the JSON as written: 150, 0.5, true */
} SpMessage;

typedef struct SpArgs {
    uintptr_t parent;  /* 0: started by hand, a normal window */
    int width, height; /* the wallpaper's size in pixels (0 if not given) */
    int nprops;        /* name=value arguments, queued as SP_PROPERTY messages */
} SpArgs;

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
    if (sp_q.count == SP_QUEUE) { sp_q.head = (sp_q.head + 1) % SP_QUEUE; sp_q.count--; }   /* full: the oldest goes */
    sp_q.items[(sp_q.head + sp_q.count++) % SP_QUEUE] = *m;
    sp__unlock();
}

static inline bool sp_next(SpMessage *out) {
    if (!sp_q.ready) return false;
    sp__lock();
    bool any = sp_q.count > 0;
    if (any) { *out = sp_q.items[sp_q.head]; sp_q.head = (sp_q.head + 1) % SP_QUEUE; sp_q.count--; }
    sp__unlock();
    return any;
}

/* ---- one line of Screen+'s JSON (its own fixed shape, not JSON in general) */

/* Copies the value after "key": into out: a string unescaped, anything else up to , or }. */
static inline bool sp__field(const char *line, const char *key, char *out, size_t cap) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(line, pat);
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
                else if (c == 'u') { c = '?'; for (int i = 0; i < 4 && p[1]; i++) p++; }
            }
            if (n + 1 < cap) out[n++] = c;
        }
    } else {
        for (; *p && *p != ',' && *p != '}' && *p != '\r' && *p != '\n'; p++)
            if (*p != ' ' && n + 1 < cap) out[n++] = *p;
    }
    out[n] = 0;
    return true;
}

static inline void sp__line(const char *line) {
    char type[16];
    SpMessage m = { 0 };
    if (!sp__field(line, "type", type, sizeof type) || !sp__field(line, "value", m.value, sizeof m.value)) return;
    if (!strcmp(type, "property")) {
        m.kind = SP_PROPERTY;
        if (!sp__field(line, "name", m.name, sizeof m.name)) return;
    } else if (!strcmp(type, "pause")) m.kind = SP_PAUSE;
    else if (!strcmp(type, "volume")) m.kind = SP_VOLUME;
    else return;
    sp__push(&m);
    bare_wake();
}

#ifdef _WIN32
static DWORD WINAPI sp__reader(LPVOID arg) {
#else
static void *sp__reader(void *arg) {
#endif
    (void)arg;
    static char line[8192];
    while (fgets(line, sizeof line, stdin)) sp__line(line);
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
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--screenplus-parent") && i + 1 < argc) a.parent = (uintptr_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--screenplus-size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &a.width, &a.height);
        else if (strchr(argv[i], '=')) {
            SpMessage m = { SP_PROPERTY };
            const char *eq = strchr(argv[i], '=');
            snprintf(m.name, sizeof m.name, "%.*s", (int)(eq - argv[i]), argv[i]);
            snprintf(m.value, sizeof m.value, "%s", eq + 1);
            sp__push(&m);
            a.nprops++;
        }
    }
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

#endif
