/*
 * bare_backend.h: advanced header. The platform and renderer interfaces a
 * backend implements. See docs/ARCHITECTURE.md §10.
 */
#ifndef BARE_BACKEND_H
#define BARE_BACKEND_H

#include "bare_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum BareEventKind {
    BARE_EVENT_NONE = 0,
    BARE_EVENT_MOUSE_MOVE,
    BARE_EVENT_MOUSE_BUTTON,
    BARE_EVENT_WHEEL,
    BARE_EVENT_KEY,
    BARE_EVENT_RESIZE,         /* size or DPI scale changed */
    BARE_EVENT_EXPOSE,         /* the window needs redrawing */
    BARE_EVENT_FOCUS,
    BARE_EVENT_QUIT
} BareEventKind;

typedef struct BareEvent {
    BareEventKind kind;
    double time;
    union {
        struct { float x, y; } mouse;                       /* logical pixels */
        struct { int button; bool down; float x, y; } button;
        struct { float dx, dy; bool precise; } wheel;       /* precise = touchpad pixels, else lines */
        struct { int key; bool down, repeat; int mods; } key;
        struct { float w, h, scale; } size;
        bool focused;
    } as;
} BareEvent;

typedef enum BareNativeKind {
    BARE_NATIVE_SDL_WINDOW = 1
} BareNativeKind;

typedef struct BarePlatform {
    void *self;
    /* Opens the window hidden; show_window reveals it after the first frame is drawn. */
    bool   (*open_window)(void *self, int width, int height, const char *title);
    void   (*show_window)(void *self);
    void   (*close_window)(void *self);
    /* Fills `out` with up to `max` events. If none are queued, blocks up to
     * `wait` seconds (0 = don't block, < 0 = until something happens). */
    int    (*poll_events)(void *self, BareEvent *out, int max, double wait);
    void   (*wake)(void *self);                              /* thread-safe */
    void   (*window_size)(void *self, float *width, float *height, float *scale);
    double (*time)(void *self);                              /* seconds, monotonic */
    void   (*set_cursor)(void *self, BareCursor cursor);
    void   (*set_title)(void *self, const char *title);
    const char *(*clipboard_get)(void *self);
    void   (*clipboard_set)(void *self, const char *text);
    void  *(*native)(void *self, BareNativeKind what);
    /* Platforms that own the main loop set this; others leave it NULL. */
    void   (*run)(void *self, void (*frame)(void *ctx), void *ctx);
} BarePlatform;

typedef struct BareRenderer {
    void *self;
    bool (*init)(void *self, const BarePlatform *platform);
    void (*render)(void *self, const BareDrawList *list);    /* uploads, draw, present */
    void (*shutdown)(void *self);
} BareRenderer;

#ifdef __cplusplus
}
#endif

#endif /* BARE_BACKEND_H */
