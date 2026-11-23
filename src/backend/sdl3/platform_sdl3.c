/* platform_sdl3.c: BarePlatform on SDL3 (ARCHITECTURE §10).
 *
 * Units: Bare's logical pixel = physical pixel / display scale. SDL reports
 * window and mouse coordinates in "window coordinates", which are pixels on
 * Windows and points on macOS, so everything goes through to_logical. */
#include "bare/bare_backend.h"
#include <SDL3/SDL.h>
#include <string.h>

typedef struct SdlPlatform {
    SDL_Window *window;
    Uint32      wake_event;
    SDL_Cursor *cursors[BARE_CURSOR_NOT_ALLOWED + 1];
    BareCursor  cursor;
    char       *clipboard;
} SdlPlatform;

static SdlPlatform sdl;

/* Logical pixels per window coordinate. */
static float to_logical(void) {
    float density = SDL_GetWindowPixelDensity(sdl.window);
    float scale = SDL_GetWindowDisplayScale(sdl.window);
    if (density <= 0) density = 1;
    if (scale <= 0) scale = 1;
    return density / scale;
}

static bool open_window(void *self, int width, int height, const char *title) {
    (void)self;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_Log("bare: SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    /* Create at the requested logical size on the primary display. */
    float content = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (content <= 0) content = 1;
    sdl.window = SDL_CreateWindow(title ? title : "Bare", (int)(width * content), (int)(height * content),
                                  SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!sdl.window) {
        SDL_Log("bare: SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    sdl.wake_event = SDL_RegisterEvents(1);
    return true;
}

static void show_window(void *self) { (void)self; SDL_ShowWindow(sdl.window); }

static void close_window(void *self) {
    (void)self;
    for (int i = 0; i <= BARE_CURSOR_NOT_ALLOWED; i++) {
        if (sdl.cursors[i]) SDL_DestroyCursor(sdl.cursors[i]);
    }
    SDL_free(sdl.clipboard);
    if (sdl.window) SDL_DestroyWindow(sdl.window);
    SDL_Quit();
    memset(&sdl, 0, sizeof sdl);
}

static void window_size(void *self, float *w, float *h, float *scale) {
    (void)self;
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(sdl.window, &pw, &ph);
    float s = SDL_GetWindowDisplayScale(sdl.window);
    if (s <= 0) s = 1;
    *w = pw / s;
    *h = ph / s;
    *scale = s;
}

/* Translates one SDL event; returns false for events Bare doesn't use. */
static bool translate(const SDL_Event *e, BareEvent *out) {
    memset(out, 0, sizeof *out);
    out->time = e->common.timestamp / 1e9;
    float k = to_logical();
    switch (e->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        out->kind = BARE_EVENT_QUIT;
        return true;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        out->kind = BARE_EVENT_RESIZE;
        window_size(NULL, &out->as.size.w, &out->as.size.h, &out->as.size.scale);
        return true;
    case SDL_EVENT_WINDOW_EXPOSED:
        out->kind = BARE_EVENT_EXPOSE;
        return true;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        out->kind = BARE_EVENT_FOCUS;
        out->as.focused = e->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        out->kind = BARE_EVENT_MOUSE_MOVE;
        out->as.mouse.x = e->motion.x * k;
        out->as.mouse.y = e->motion.y * k;
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        out->kind = BARE_EVENT_MOUSE_BUTTON;
        out->as.button.button = e->button.button;   /* 1 left, 2 middle, 3 right */
        out->as.button.down = e->button.down;
        out->as.button.x = e->button.x * k;
        out->as.button.y = e->button.y * k;
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        out->kind = BARE_EVENT_WHEEL;
        out->as.wheel.dx = e->wheel.x;
        out->as.wheel.dy = e->wheel.y;
        out->as.wheel.precise = false;
        return true;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        out->kind = BARE_EVENT_KEY;
        out->as.key.key = (int)e->key.scancode;    /* mapped to BareKey in M3 */
        out->as.key.down = e->key.down;
        out->as.key.repeat = e->key.repeat;
        out->as.key.mods = e->key.mod;
        return true;
    default:
        if (e->type == sdl.wake_event) {
            out->kind = BARE_EVENT_EXPOSE;
            return true;
        }
        return false;
    }
}

static int poll_events(void *self, BareEvent *out, int max, double wait) {
    (void)self;
    int n = 0;
    SDL_Event e;
    if (wait != 0 && max > 0) {
        Sint32 ms = wait < 0 ? -1 : (Sint32)(wait * 1000.0);
        /* Block until something Bare cares about arrives (or the timeout). */
        while (n == 0) {
            if (!SDL_WaitEventTimeout(&e, ms)) return 0;
            if (translate(&e, &out[n])) n++;
            if (ms >= 0) break;
        }
    }
    while (n < max && SDL_PollEvent(&e)) {
        if (translate(&e, &out[n])) n++;
    }
    return n;
}

static void wake(void *self) {
    (void)self;
    SDL_Event e;
    SDL_zero(e);
    e.type = sdl.wake_event;
    SDL_PushEvent(&e);
}

static double now(void *self) { (void)self; return SDL_GetTicksNS() / 1e9; }

static void set_cursor(void *self, BareCursor cursor) {
    (void)self;
    if (cursor == sdl.cursor || cursor < 0 || cursor > BARE_CURSOR_NOT_ALLOWED) return;
    static const SDL_SystemCursor map[] = {
        SDL_SYSTEM_CURSOR_DEFAULT, SDL_SYSTEM_CURSOR_POINTER, SDL_SYSTEM_CURSOR_TEXT, SDL_SYSTEM_CURSOR_MOVE,
        SDL_SYSTEM_CURSOR_EW_RESIZE, SDL_SYSTEM_CURSOR_NS_RESIZE, SDL_SYSTEM_CURSOR_NOT_ALLOWED,
    };
    if (!sdl.cursors[cursor]) sdl.cursors[cursor] = SDL_CreateSystemCursor(map[cursor]);
    if (sdl.cursors[cursor]) SDL_SetCursor(sdl.cursors[cursor]);
    sdl.cursor = cursor;
}

static void set_title(void *self, const char *title) { (void)self; SDL_SetWindowTitle(sdl.window, title); }

static const char *clipboard_get(void *self) {
    (void)self;
    SDL_free(sdl.clipboard);
    sdl.clipboard = SDL_GetClipboardText();
    return sdl.clipboard ? sdl.clipboard : "";
}

static void clipboard_set(void *self, const char *text) { (void)self; SDL_SetClipboardText(text); }

static void *native(void *self, BareNativeKind what) {
    (void)self;
    return what == BARE_NATIVE_SDL_WINDOW ? sdl.window : NULL;
}

BarePlatform bare__sdl3_platform(void) {
    BarePlatform p = {
        .self = &sdl,
        .open_window = open_window,
        .show_window = show_window,
        .close_window = close_window,
        .poll_events = poll_events,
        .wake = wake,
        .window_size = window_size,
        .time = now,
        .set_cursor = set_cursor,
        .set_title = set_title,
        .clipboard_get = clipboard_get,
        .clipboard_set = clipboard_set,
        .native = native,
        .run = NULL,
    };
    return p;
}
