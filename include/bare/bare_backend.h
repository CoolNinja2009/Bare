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

/* BareEvent lives in bare_core.h: the core consumes it (bare_ctx_event). */

typedef enum BareNativeKind {
    BARE_NATIVE_SDL_WINDOW = 1
} BareNativeKind;

typedef struct BarePlatform {
    void *self;
    /* Opens the window hidden; show_window reveals it after the first frame is drawn. */
    bool   (*open_window)(void *self, int width, int height, const char *title, bool transparent);
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
    /* Text fields: deliver TEXT and COMPOSITION events while on; `caret` is in
     * logical window pixels (the IME's window goes under it). Called again
     * when the caret moves. */
    void   (*start_text_input)(void *self, BareTextInputKind kind, BareRect caret);
    void   (*stop_text_input)(void *self);
    void  *(*native)(void *self, BareNativeKind what);
    /* The system's file dialog, without blocking: the answer comes back from
     * poll_events as a BARE_EVENT_FILE_CHOSEN with this `request`. kind: 0
     * open, 1 save, 2 folder. filters: "Markdown:md;markdown|Text:txt" or
     * NULL. NULL if the platform has none. */
    void   (*file_dialog)(void *self, uint64_t request, int kind, const char *filters, const char *path);
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
