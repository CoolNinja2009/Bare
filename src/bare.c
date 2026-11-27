/* bare.c: the simple API (bare_window, bare_frame, bare_run, bare_quit) on top
 * of the core context and the backend chosen at build time (ARCHITECTURE §3). */
#include "bare/bare_backend.h"
#include "backend/record.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void bare__pictures_stop(void);   /* picture.c */
void bare__videos_stop(void);     /* video.c */
void bare__sound_virtual(bool on); /* sound: plays follow the fixed clock, unheard */

BarePlatform bare__sdl3_platform(void);
BareRenderer bare__sdl3gpu_renderer(void);

#define MAX_EVENTS 256

static struct {
    BareContext  *ctx;
    BarePlatform  platform;
    BareRenderer  renderer;
    bool          open, in_frame, quit, shown, continuous, dirty;
    float         width, height, scale;
    float         zoom;               /* BareWindowOptions.zoom: logical pixels = the system's / zoom */
    double        start, time, prev_time;
    int           capture_frames;     /* BARE_CAPTURE: frames left before quitting */
    const char   *record_path;        /* BARE_RECORD: the video being written */
    int           record_fps;
    long          record_frames;      /* frames left to record before quitting */
    BareRecorder *recorder;           /* opened on the first frame, when its size is known */
    double        fixed_dt;           /* BARE_FIXED_DT: seconds per frame, whatever the clock says */
    const char   *script;             /* BARE_EVENTS: input to play, by frame (see play_script) */
    long          frames;             /* frames begun */
    BareTextInputKind text_input;     /* what the platform was last told */
    BareRect      text_rect;
    BareEvent     events[MAX_EVENTS];
    BareId        chosen_id;          /* a file dialog's answer, shown for one frame */
    char          chosen[4096];
    int           chosen_frames;
    char          dropped[8][4096];   /* files dropped on the window, one handed out a frame */
    int           drops, drop_shown;
    void        (*run_frame)(void *user);   /* bare_run's frame, drawn from move/resize loops too */
    void         *run_user;
    bool          in_live;
    BareImage     backdrop;           /* bare_window_backdrop's image */
    int           backdrop_w, backdrop_h;
} app;

static const char *clip_get(void *user) { (void)user; return app.platform.clipboard_get(app.platform.self); }
static void clip_set(void *user, const char *text) { (void)user; app.platform.clipboard_set(app.platform.self, text); }

static int drag_query(void *user, float x, float y) {
    (void)user;
    return app.ctx && bare_ctx_window_drag(app.ctx, (BareVec2){ x / app.zoom, y / app.zoom });
}

/* BARE_RECORD=out.mp4: the app runs on a fixed clock (BARE_RECORD_FPS frames
 * a second, default 60) for BARE_RECORD_SECONDS (default 10), every frame is
 * read back and encoded, then it quits. The clock is the video's, not the
 * wall's, so the video is smooth however slowly frames are made, and the same
 * on every run. Input is ignored, as with BARE_FIXED_DT; BARE_EVENTS still
 * plays. */
static void record_sink(void *user, const void *pixels, int width, int height, int pitch, bool bgra) {
    (void)user;
    if (!app.record_path) return;
    if (!app.recorder) {
        app.recorder = bare__record_open(app.record_path, width & ~1, height & ~1, app.record_fps);
        if (!app.recorder) { app.record_path = NULL; return; }
        fprintf(stderr, "bare: recording %ld frames (%dx%d, %d fps) to %s\n", app.record_frames,
                width & ~1, height & ~1, app.record_fps, app.record_path);
    }
    if (!bare__record_frame(app.recorder, pixels, pitch, bgra)) app.quit = true;
}

static void record_start(void) {
    const char *path = getenv("BARE_RECORD");
    if (!path || !*path) return;
    if (!app.renderer.read_frames) {
        fprintf(stderr, "bare: BARE_RECORD: this renderer can't read frames back\n");
        return;
    }
    const char *fps = getenv("BARE_RECORD_FPS"), *secs = getenv("BARE_RECORD_SECONDS");
    app.record_fps = fps && atoi(fps) > 0 ? atoi(fps) : 60;
    double seconds = secs && atof(secs) > 0 ? atof(secs) : 10.0;
    app.record_frames = (long)ceil(seconds * app.record_fps);
    app.record_path = path;
    if (app.fixed_dt <= 0) app.fixed_dt = 1.0 / app.record_fps;
    app.renderer.read_frames(app.renderer.self, record_sink, NULL);
}

static void record_stop(void) {
    if (!app.recorder) return;
    long frames = bare__record_close(app.recorder);
    app.recorder = NULL;
    fprintf(stderr, "bare: recorded %ld frames (%.2f s) to %s\n", frames, (double)frames / app.record_fps,
            app.record_path ? app.record_path : "the video");
}

static bool window_open(int width, int height, const char *title, unsigned flags, uintptr_t parent) {
    if (app.open) return true;
    if (!(app.zoom > 0)) app.zoom = 1;
    app.platform = bare__sdl3_platform();
    app.renderer = bare__sdl3gpu_renderer();
    if (!app.platform.open_window(app.platform.self, (int)(width * app.zoom + 0.5f), (int)(height * app.zoom + 0.5f),
                                  title, flags)) return false;
    if (app.platform.set_drag_query) app.platform.set_drag_query(app.platform.self, drag_query, NULL);
    /* Before the renderer, so the swap chain starts at the host's size. */
    if (parent && (!app.platform.set_parent || !app.platform.set_parent(app.platform.self, parent))) {
        app.platform.close_window(app.platform.self);
        return false;
    }
    if (!app.renderer.init(app.renderer.self, &app.platform)) {
        app.platform.close_window(app.platform.self);
        return false;
    }
    app.ctx = bare_ctx_create(NULL);
    if (!app.ctx) {
        app.renderer.shutdown(app.renderer.self);
        app.platform.close_window(app.platform.self);
        return false;
    }
    bare_ctx_make_current(app.ctx);
    bare_ctx_set_clipboard(app.ctx, clip_get, clip_set, NULL);
    app.platform.window_size(app.platform.self, &app.width, &app.height, &app.scale);
    app.width /= app.zoom, app.height /= app.zoom, app.scale *= app.zoom;
    app.start = app.prev_time = app.platform.time(app.platform.self);
    const char *capture = getenv("BARE_CAPTURE");
    /* BARE_CAPTURE: save frame BARE_CAPTURE_FRAMES (default 2) to a .bmp and quit. */
    const char *frames = getenv("BARE_CAPTURE_FRAMES");
    app.capture_frames = capture && *capture ? (frames && atoi(frames) > 0 ? atoi(frames) : 2) : 0;
    /* BARE_FIXED_DT: frame n starts at n * dt seconds, so a capture of something
     * animated is the same picture on every run (and comparable to a browser
     * driven by the same clock). */
    const char *fixed = getenv("BARE_FIXED_DT");
    app.fixed_dt = fixed ? atof(fixed) : 0;
    if (app.fixed_dt < 0) app.fixed_dt = 0;
    app.script = getenv("BARE_EVENTS");
    record_start();
    bare__sound_virtual(app.fixed_dt > 0);
    app.open = true;
    app.dirty = true;
    return true;
}

bool bare_window(int width, int height, const char *title) {
    return window_open(width, height, title, 0, 0);
}

bool bare_window_transparent(int width, int height, const char *title) {
    return window_open(width, height, title, BARE_WINDOW_TRANSPARENT, 0);
}

bool bare_window_with_(const BareWindowOptions *o) {
    unsigned flags = (o->transparent ? BARE_WINDOW_TRANSPARENT : 0) | (o->frameless ? BARE_WINDOW_FRAMELESS : 0);
    if (!app.open) app.zoom = o->zoom > 0 ? o->zoom : 1;
    if (!window_open(o->width > 0 ? o->width : 1280, o->height > 0 ? o->height : 800, o->title, flags, o->parent)) return false;
    if ((o->min_width > 0 || o->min_height > 0) && app.platform.set_min_size)
        app.platform.set_min_size(app.platform.self, (int)(o->min_width * app.zoom + 0.5f),
                                  (int)(o->min_height * app.zoom + 0.5f));
    return true;
}

void bare_window_minimize(void) { if (app.open && app.platform.minimize) app.platform.minimize(app.platform.self); }
void bare_window_maximize(void) { if (app.open && app.platform.maximize) app.platform.maximize(app.platform.self); }
bool bare_window_maximized(void) {
    return app.open && app.platform.maximized && app.platform.maximized(app.platform.self);
}
void bare_window_close(void) { app.quit = true; if (app.open) app.platform.wake(app.platform.self); }

BareImage bare_window_backdrop(void) {
    if (!app.open || !app.platform.backdrop) return 0;
    int w = 0, h = 0;
    const void *pixels = app.platform.backdrop(app.platform.self, &w, &h);
    if (pixels && w > 0 && h > 0) {
        if (app.backdrop && (w != app.backdrop_w || h != app.backdrop_h)) {
            bare_image_free(app.backdrop);
            app.backdrop = 0;
        }
        if (app.backdrop) bare_image_update(app.backdrop, pixels);
        else app.backdrop = bare_image_from_pixels(w, h, pixels);
        app.backdrop_w = w, app.backdrop_h = h;
    }
    return app.backdrop;
}

static bool is_input(BareEventKind k) {
    return k == BARE_EVENT_MOUSE_MOVE || k == BARE_EVENT_MOUSE_BUTTON || k == BARE_EVENT_MOUSE_LEAVE ||
           k == BARE_EVENT_WHEEL || k == BARE_EVENT_KEY || k == BARE_EVENT_TEXT || k == BARE_EVENT_COMPOSITION;
}

/* Applies one platform event to the app state. */
static void apply(const BareEvent *event) {
    /* The system's logical pixels to the app's (a played script is in the app's already). */
    BareEvent zoomed = *event;
    const BareEvent *e = &zoomed;
    if (app.zoom != 1 && event->time != -1) {
        float z = app.zoom;
        if (e->kind == BARE_EVENT_MOUSE_MOVE) zoomed.as.mouse.x /= z, zoomed.as.mouse.y /= z;
        if (e->kind == BARE_EVENT_MOUSE_BUTTON) zoomed.as.button.x /= z, zoomed.as.button.y /= z;
        if (e->kind == BARE_EVENT_WHEEL && e->as.wheel.precise) zoomed.as.wheel.dx /= z, zoomed.as.wheel.dy /= z;
    }
    /* With a fixed clock nothing else may move the app either: the real mouse
     * over the window would make every capture different. */
    if ((app.fixed_dt > 0 || app.script) && is_input(e->kind) && e->time != -1) return;
    switch (e->kind) {
    case BARE_EVENT_QUIT: app.quit = true; break;
    case BARE_EVENT_FILE_CHOSEN: {
        const char *p = e->as.file.path ? e->as.file.path : "";
        size_t n = strlen(p);
        if (n >= sizeof app.chosen) n = 0;   /* a path that long can't be opened anyway */
        memcpy(app.chosen, p, n);
        app.chosen[n] = 0;
        app.chosen_id = e->as.file.request;
        app.chosen_frames = 1;   /* the coming frame sees it; it is gone after */
        app.dirty = true;
        return;
    }
    case BARE_EVENT_FILE_DROPPED: {
        const char *p = e->as.file.path ? e->as.file.path : "";
        size_t n = strlen(p);
        if (!n || n >= sizeof app.dropped[0] || app.drops >= 8) return;
        memcpy(app.dropped[app.drops], p, n + 1);
        app.drops++;
        app.dirty = true;
        return;
    }
    case BARE_EVENT_RESIZE:
        app.width = e->as.size.w / app.zoom;
        app.height = e->as.size.h / app.zoom;
        app.scale = e->as.size.scale * app.zoom;
        app.dirty = true;
        break;
    default: app.dirty = true; break;
    }
    bare_ctx_event(app.ctx, e);   /* mouse, buttons, wheel, keys, focus */
}

static void end_frame(void) {
    bool animating = false;
    const BareDrawList *list = bare_ctx_end(app.ctx, &animating);
    app.renderer.render(app.renderer.self, list);
    app.platform.set_cursor(app.platform.self, bare_ctx_cursor(app.ctx));   /* no-op when unchanged */
    /* Text input on while a field has focus, the IME window following the caret. */
    BareRect caret;
    BareTextInputKind ti = bare_ctx_text_input(app.ctx, &caret);
    bool moved = caret.x != app.text_rect.x || caret.y != app.text_rect.y || caret.h != app.text_rect.h;
    if (ti != app.text_input || (ti && moved)) {
        BareRect at = { caret.x * app.zoom, caret.y * app.zoom, caret.w * app.zoom, caret.h * app.zoom };
        if (ti && app.platform.start_text_input) app.platform.start_text_input(app.platform.self, ti, at);
        else if (!ti && app.platform.stop_text_input) app.platform.stop_text_input(app.platform.self);
        app.text_input = ti;
        app.text_rect = caret;
    }
    if (!app.shown) {
        app.platform.show_window(app.platform.self);   /* no blank flash before the first frame */
        app.shown = true;
    }
    app.in_frame = false;
    app.dirty = animating || app.continuous;
    if (app.chosen_frames > 0 && --app.chosen_frames == 0) app.chosen_id = 0;
    if (app.drop_shown) {   /* the dropped file this frame saw is done; the next one waits for the next frame */
        memmove(app.dropped[0], app.dropped[1], sizeof app.dropped[0] * (size_t)(app.drops - 1));
        app.drops--;
        app.drop_shown = 0;
        if (app.drops) app.dirty = true;
    }
    if (app.capture_frames > 0 && --app.capture_frames == 0) app.quit = true;
    if (app.record_path) {
        app.dirty = true;   /* every frame is drawn, animating or not */
        if (--app.record_frames <= 0) app.quit = true;
    }
}

/* BARE_EVENTS="2:move:640:360; 3:down:640:360; 4:up:640:360; 5:wheel:-100":
 * input played at the start of frame N (logical pixels; wheel in the DOM's
 * deltaY), so tests and captures can hover, click and scroll.
 * "6:drop:C:/clips/a.mp4" drops a file on the window (the path runs to the ';'). */
static void play_script(void) {
    for (const char *p = app.script; p && *p;) {
        while (*p == ' ' || *p == ';') p++;
        char kind[8] = { 0 };
        long frame = 0;
        float x = 0, y = 0;
        int used = 0;
        if (sscanf(p, "%ld:%7[a-z]:%n", &frame, kind, &used) == 2 && !strcmp(kind, "drop") && used) {
            const char *path = p + used, *end = strchr(path, ';');
            size_t n = end ? (size_t)(end - path) : strlen(path);
            p = path + n;
            if (frame != app.frames + 1 || !n || n >= sizeof app.dropped[0] || app.drops >= 8) continue;
            memcpy(app.dropped[app.drops], path, n);
            app.dropped[app.drops++][n] = 0;
            continue;
        }
        if (sscanf(p, "%ld:%7[a-z]:%f%n", &frame, kind, &x, &used) < 2) break;
        p += used;
        if (*p == ':') { p++; sscanf(p, "%f%n", &y, &used); p += used; }
        while (*p && *p != ';') p++;
        if (frame != app.frames + 1) continue;
        BareEvent e;
        memset(&e, 0, sizeof e);
        e.time = -1;   /* played, not from the platform */
        if (!strcmp(kind, "move")) {
            e.kind = BARE_EVENT_MOUSE_MOVE;
            e.as.mouse.x = x, e.as.mouse.y = y;
        } else if (!strcmp(kind, "down") || !strcmp(kind, "up")) {
            e.kind = BARE_EVENT_MOUSE_BUTTON;
            e.as.button.button = 1;
            e.as.button.down = kind[0] == 'd';
            e.as.button.x = x, e.as.button.y = y;
        } else if (!strcmp(kind, "wheel")) {
            e.kind = BARE_EVENT_WHEEL;
            e.as.wheel.dy = -x;
            e.as.wheel.precise = true;
        } else if (!strcmp(kind, "leave")) {
            e.kind = BARE_EVENT_MOUSE_LEAVE;
        } else {
            continue;
        }
        apply(&e);
    }
}

bool bare_frame(void) {
    if (!app.open) return false;
    if (app.platform.run) {
        /* The platform owns the loop; the while-loop form can't work here. */
        fprintf(stderr, "bare: this backend owns the main loop; use bare_run(frame, user) instead of bare_frame()\n");
        return false;
    }
    if (app.in_frame) end_frame();
    if (app.quit) return false;

    /* Sleep until something happens unless a redraw is already needed. In
     * capture mode never block: there's no user to wake us. */
    double wait = (app.dirty || app.capture_frames || app.record_path) ? 0.0 : -1.0;
    double wake = bare_ctx_wake_time(app.ctx);   /* something timed, like a scrollbar fading out */
    if (wait < 0 && wake >= 0) {
        double now = app.platform.time(app.platform.self) - app.start;
        wait = wake > now ? wake - now : 0.0;
    }
    int n = app.platform.poll_events(app.platform.self, app.events, MAX_EVENTS, wait);
    for (int i = 0; i < n; i++) apply(&app.events[i]);
    if (app.quit) return false;
    if (app.script) play_script();

    app.prev_time = app.time;
    app.time = app.fixed_dt > 0 ? (double)app.frames * app.fixed_dt : app.platform.time(app.platform.self) - app.start;
    app.frames++;
    bare_ctx_begin(app.ctx, app.width, app.height, app.scale, app.time);
    app.in_frame = true;
    return true;
}

void bare_file_dialog(BareId id, BareFileDialog kind, const char *filters, const char *path) {
    if (!app.open || !app.platform.file_dialog) return;
    app.platform.file_dialog(app.platform.self, id, (int)kind, filters, path);
}

const char *bare_file_dropped(void) {
    if (!app.open || !app.in_frame || !app.drops) return NULL;
    app.drop_shown = 1;
    return app.dropped[0];
}

const char *bare_file_chosen(BareId id) {
    return id && app.chosen_frames > 0 && app.chosen_id == id ? app.chosen : NULL;
}

/* A frame from inside Windows' move/resize loop (bare_run only: the while-loop
 * form is stuck in bare_frame's poll then). No new input arrives there. */
static void live(void *user, const BareEvent *e) {
    (void)user;
    if (e->kind == BARE_EVENT_RESIZE) { apply(e); return; }
    if (!app.run_frame || app.in_frame || app.in_live || app.quit || !app.shown) return;
    app.in_live = true;
    app.prev_time = app.time;
    app.time = app.fixed_dt > 0 ? (double)app.frames * app.fixed_dt : app.platform.time(app.platform.self) - app.start;
    app.frames++;
    bare_ctx_begin(app.ctx, app.width, app.height, app.scale, app.time);
    app.in_frame = true;
    app.run_frame(app.run_user);
    if (app.in_frame) end_frame();
    app.in_live = false;
}

void bare_run(void (*frame)(void *user), void *user) {
    if (!app.open || !frame) return;
    if (app.platform.run) {
        /* M9: the platform calls back once per frame. */
        app.platform.run(app.platform.self, frame, user);
        return;
    }
    app.run_frame = frame;
    app.run_user = user;
    if (app.platform.set_live) app.platform.set_live(app.platform.self, live, NULL);
    while (bare_frame()) frame(user);
    if (app.platform.set_live) app.platform.set_live(app.platform.self, NULL, NULL);
    app.run_frame = NULL;
}

void bare_quit(void) {
    if (!app.open) return;
    if (app.in_frame) bare_ctx_end(app.ctx, NULL);
    bare__pictures_stop();
    bare__videos_stop();
    record_stop();
    bare_ctx_destroy(app.ctx);
    app.renderer.shutdown(app.renderer.self);
    app.platform.close_window(app.platform.self);
    memset(&app, 0, sizeof app);
}

void bare_background(BareColor color) {
    if (app.ctx) bare_ctx_set_clear(app.ctx, color);
}

void bare_title(const char *title) {
    if (app.open) app.platform.set_title(app.platform.self, title);
}

BareVec2 bare_window_size(void) { return (BareVec2){ app.width, app.height }; }
float    bare_pixel_scale(void) { return app.scale > 0 ? app.scale : 1.0f; }
void bare_continuous(bool on) { app.continuous = on; }
void bare_wake(void) { if (app.open) app.platform.wake(app.platform.self); }
double bare_time(void) { return app.time; }
float bare_dt(void) { return (float)(app.time - app.prev_time); }

const char *bare_clipboard(void) { return app.open ? app.platform.clipboard_get(app.platform.self) : ""; }
void bare_set_clipboard(const char *text) { if (app.open) app.platform.clipboard_set(app.platform.self, text); }
