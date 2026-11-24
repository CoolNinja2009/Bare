/* bare.c: the simple API (bare_window, bare_frame, bare_run, bare_quit) on top
 * of the core context and the backend chosen at build time (ARCHITECTURE §3). */
#include "bare/bare_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

BarePlatform bare__sdl3_platform(void);
BareRenderer bare__sdl3gpu_renderer(void);

#define MAX_EVENTS 256

static struct {
    BareContext  *ctx;
    BarePlatform  platform;
    BareRenderer  renderer;
    bool          open, in_frame, quit, shown, continuous, dirty;
    float         width, height, scale;
    double        start, time, prev_time;
    int           capture_frames;     /* BARE_CAPTURE: frames left before quitting */
    double        fixed_dt;           /* BARE_FIXED_DT: seconds per frame, whatever the clock says */
    const char   *script;             /* BARE_EVENTS: input to play, by frame (see play_script) */
    long          frames;             /* frames begun */
    BareTextInputKind text_input;     /* what the platform was last told */
    BareRect      text_rect;
    BareEvent     events[MAX_EVENTS];
    BareId        chosen_id;          /* a file dialog's answer, shown for one frame */
    char          chosen[4096];
    int           chosen_frames;
} app;

static const char *clip_get(void *user) { (void)user; return app.platform.clipboard_get(app.platform.self); }
static void clip_set(void *user, const char *text) { (void)user; app.platform.clipboard_set(app.platform.self, text); }

static bool window_open(int width, int height, const char *title, bool transparent) {
    if (app.open) return true;
    app.platform = bare__sdl3_platform();
    app.renderer = bare__sdl3gpu_renderer();
    if (!app.platform.open_window(app.platform.self, width, height, title, transparent)) return false;
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
    app.open = true;
    app.dirty = true;
    return true;
}

bool bare_window(int width, int height, const char *title) {
    return window_open(width, height, title, false);
}

bool bare_window_transparent(int width, int height, const char *title) {
    return window_open(width, height, title, true);
}

static bool is_input(BareEventKind k) {
    return k == BARE_EVENT_MOUSE_MOVE || k == BARE_EVENT_MOUSE_BUTTON || k == BARE_EVENT_MOUSE_LEAVE ||
           k == BARE_EVENT_WHEEL || k == BARE_EVENT_KEY || k == BARE_EVENT_TEXT || k == BARE_EVENT_COMPOSITION;
}

/* Applies one platform event to the app state. */
static void apply(const BareEvent *e) {
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
    case BARE_EVENT_RESIZE:
        app.width = e->as.size.w;
        app.height = e->as.size.h;
        app.scale = e->as.size.scale;
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
        if (ti && app.platform.start_text_input) app.platform.start_text_input(app.platform.self, ti, caret);
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
    if (app.capture_frames > 0 && --app.capture_frames == 0) app.quit = true;
}

/* BARE_EVENTS="2:move:640:360; 3:down:640:360; 4:up:640:360; 5:wheel:-100":
 * input played at the start of frame N (logical pixels; wheel in the DOM's
 * deltaY), so tests and captures can hover, click and scroll. */
static void play_script(void) {
    for (const char *p = app.script; p && *p;) {
        while (*p == ' ' || *p == ';') p++;
        char kind[8] = { 0 };
        long frame = 0;
        float x = 0, y = 0;
        int used = 0;
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
    double wait = (app.dirty || app.capture_frames) ? 0.0 : -1.0;
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

const char *bare_file_chosen(BareId id) {
    return id && app.chosen_frames > 0 && app.chosen_id == id ? app.chosen : NULL;
}

void bare_run(void (*frame)(void *user), void *user) {
    if (!app.open || !frame) return;
    if (app.platform.run) {
        /* M9: the platform calls back once per frame. */
        app.platform.run(app.platform.self, frame, user);
        return;
    }
    while (bare_frame()) frame(user);
}

void bare_quit(void) {
    if (!app.open) return;
    if (app.in_frame) bare_ctx_end(app.ctx, NULL);
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
