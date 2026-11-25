/* platform_sdl3.c: BarePlatform on SDL3 (ARCHITECTURE §10).
 *
 * Units: Bare's logical pixel = physical pixel / display scale. SDL reports
 * window and mouse coordinates in "window coordinates", which are pixels on
 * Windows and points on macOS, so everything goes through to_logical. */
#include "bare/bare_backend.h"
#include <SDL3/SDL.h>
#include <string.h>
#ifdef _WIN32
#include "../backdrop.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#endif

typedef struct TextChunk { struct TextChunk *next; int used, cap; char *data; } TextChunk;   /* data follows it */

typedef struct SdlPlatform {
    SDL_Window *window;
    Uint32      wake_event;
    SDL_Cursor *cursors[BARE_CURSOR_NOT_ALLOWED + 1];
    BareCursor  cursor;
    char       *clipboard;
    BareTextInputKind text_kind;   /* text input is on (and of this kind) */
    TextChunk  *text;              /* copies of this poll's text events */
    Uint32      file_event;        /* a dialog's answer, pushed from whatever thread SDL calls back on */
    char       *chosen;            /* the last answer's path, freed at the next poll */
    SDL_DialogFileFilter filters[16];
    char        filter_text[1024]; /* what `filters` point into: SDL reads them until the dialog closes */
    bool        frameless;
    int       (*drag_query)(void *user, float x, float y);
    void       *drag_user;
    void      (*live)(void *user, const BareEvent *e);
    void       *live_user;
#ifdef _WIN32
    BareBackdrop *backdrop;
    bool        backdrop_failed;
#endif
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

/* Frameless windows: the edges resize, the app's drag areas move the window
 * (SDL turns these into the system's own moves, so snapping and double-click
 * to maximise work). */
static SDL_HitTestResult hit_test(SDL_Window *win, const SDL_Point *p, void *data) {
    (void)data;
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    float k = to_logical();
    int edge = (int)(6 / k + 0.5f);
    if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED)) {
        bool l = p->x < edge, r = p->x >= w - edge, t = p->y < edge, b = p->y >= h - edge;
        if (t && l) return SDL_HITTEST_RESIZE_TOPLEFT;
        if (t && r) return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (b && l) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (b && r) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (t) return SDL_HITTEST_RESIZE_TOP;
        if (b) return SDL_HITTEST_RESIZE_BOTTOM;
        if (l) return SDL_HITTEST_RESIZE_LEFT;
        if (r) return SDL_HITTEST_RESIZE_RIGHT;
    }
    if (sdl.drag_query && sdl.drag_query(sdl.drag_user, p->x * k, p->y * k)) return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
}

static bool open_window(void *self, int width, int height, const char *title, unsigned options) {
    (void)self;
    /* Bare draws the IME's composition inline in the field; the OS still
     * shows the candidate list. */
    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_Log("bare: SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    /* Create at the requested logical size on the primary display. */
    float content = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (content <= 0) content = 1;
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    if (options & BARE_WINDOW_TRANSPARENT) flags |= SDL_WINDOW_TRANSPARENT;
    if (options & BARE_WINDOW_FRAMELESS) flags |= SDL_WINDOW_BORDERLESS;
    sdl.window = SDL_CreateWindow(title ? title : "Bare", (int)(width * content), (int)(height * content), flags);
    if (!sdl.window) {
        SDL_Log("bare: SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    sdl.frameless = (options & BARE_WINDOW_FRAMELESS) != 0;
    if (sdl.frameless) {
        SDL_SetWindowHitTest(sdl.window, hit_test, NULL);
#ifdef _WIN32
        /* Windows 11 rounds and shadows a frameless window when asked. */
        HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(sdl.window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        if (hwnd) {
            int round = 2;   /* DWMWCP_ROUND */
            DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
        }
#endif
    }
    sdl.wake_event = SDL_RegisterEvents(1);
    return true;
}

static void show_window(void *self) { (void)self; SDL_ShowWindow(sdl.window); }

static void close_window(void *self) {
    (void)self;
#ifdef _WIN32
    bare__backdrop_stop(sdl.backdrop);
#endif
    for (int i = 0; i <= BARE_CURSOR_NOT_ALLOWED; i++) {
        if (sdl.cursors[i]) SDL_DestroyCursor(sdl.cursors[i]);
    }
    SDL_free(sdl.clipboard);
    while (sdl.text) { TextChunk *next = sdl.text->next; SDL_free(sdl.text); sdl.text = next; }
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

/* SDL keycode (what the key types on the user's layout) -> BareKey; falls
 * back to the physical key for letters and digits on non-Latin layouts. */
static int map_key(SDL_Keycode k, SDL_Scancode sc) {
    if (k >= 'a' && k <= 'z') return BARE_KEY_A + (int)(k - 'a');
    if (k >= '0' && k <= '9') return BARE_KEY_0 + (int)(k - '0');
    if (k >= SDLK_F1 && k <= SDLK_F12) return BARE_KEY_F1 + (int)(k - SDLK_F1);
    switch (k) {
    case SDLK_ESCAPE: return BARE_KEY_ESCAPE;
    case SDLK_RETURN: case SDLK_KP_ENTER: return BARE_KEY_ENTER;
    case SDLK_SPACE: return BARE_KEY_SPACE;
    case SDLK_TAB: return BARE_KEY_TAB;
    case SDLK_BACKSPACE: return BARE_KEY_BACKSPACE;
    case SDLK_DELETE: return BARE_KEY_DELETE;
    case SDLK_INSERT: return BARE_KEY_INSERT;
    case SDLK_HOME: return BARE_KEY_HOME;
    case SDLK_END: return BARE_KEY_END;
    case SDLK_PAGEUP: return BARE_KEY_PAGE_UP;
    case SDLK_PAGEDOWN: return BARE_KEY_PAGE_DOWN;
    case SDLK_LEFT: return BARE_KEY_LEFT;
    case SDLK_RIGHT: return BARE_KEY_RIGHT;
    case SDLK_UP: return BARE_KEY_UP;
    case SDLK_DOWN: return BARE_KEY_DOWN;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return BARE_KEY_SHIFT;
    case SDLK_LCTRL: case SDLK_RCTRL: return BARE_KEY_CTRL;
    case SDLK_LALT: case SDLK_RALT: return BARE_KEY_ALT;
    case SDLK_LGUI: case SDLK_RGUI: return BARE_KEY_SUPER;
    case SDLK_MINUS: return BARE_KEY_MINUS;
    case SDLK_EQUALS: return BARE_KEY_EQUALS;
    case SDLK_COMMA: return BARE_KEY_COMMA;
    case SDLK_PERIOD: return BARE_KEY_PERIOD;
    case SDLK_SLASH: return BARE_KEY_SLASH;
    case SDLK_SEMICOLON: return BARE_KEY_SEMICOLON;
    case SDLK_APOSTROPHE: return BARE_KEY_APOSTROPHE;
    case SDLK_BACKSLASH: return BARE_KEY_BACKSLASH;
    case SDLK_LEFTBRACKET: return BARE_KEY_LEFT_BRACKET;
    case SDLK_RIGHTBRACKET: return BARE_KEY_RIGHT_BRACKET;
    case SDLK_GRAVE: return BARE_KEY_GRAVE;
    default: break;
    }
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return BARE_KEY_A + (int)(sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return BARE_KEY_1 + (int)(sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0) return BARE_KEY_0;
    return BARE_KEY_NONE;
}

static int map_mods(SDL_Keymod m) {
    int out = 0;
    if (m & SDL_KMOD_SHIFT) out |= BARE_MOD_SHIFT;
    if (m & SDL_KMOD_CTRL) out |= BARE_MOD_CTRL;
    if (m & SDL_KMOD_ALT) out |= BARE_MOD_ALT;
    if (m & SDL_KMOD_GUI) out |= BARE_MOD_SUPER;
    return out;
}

/* Text event strings are SDL's until the next poll: copies live in chunks
 * that never move, reused from the next poll_events call on. */
static const char *keep_text(const char *s, int *len) {
    int n = s ? (int)SDL_strlen(s) : 0;
    *len = n;
    TextChunk *c = sdl.text;
    while (c && c->cap - c->used < n + 1) c = c->next;
    if (!c) {
        int cap = n + 1 > 4096 ? n + 1 : 4096;
        c = SDL_malloc(sizeof *c + (size_t)cap);
        if (!c) { *len = 0; return ""; }
        c->data = (char *)(c + 1);
        c->used = 0;
        c->cap = cap;
        c->next = sdl.text;
        sdl.text = c;
    }
    char *at = c->data + c->used;
    if (n) SDL_memcpy(at, s, (size_t)n);
    at[n] = 0;
    c->used += n + 1;
    return at;
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
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        out->kind = BARE_EVENT_MOUSE_LEAVE;
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
        {
            float flip = e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
            float x = e->wheel.x * flip, y = e->wheel.y * flip;
            /* A mouse wheel moves in whole notches; touchpads (and free-spinning
             * wheels) report fractions of one, many times a second. Those are
             * followed directly instead of glided. */
            bool fine = x != SDL_floorf(x) || y != SDL_floorf(y);
            float unit = fine ? BARE_WHEEL_NOTCH : 1.0f;
            out->as.wheel.dx = x * unit;
            out->as.wheel.dy = y * unit;
            out->as.wheel.precise = fine;
        }
        return true;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        out->kind = BARE_EVENT_KEY;
        out->as.key.key = map_key(e->key.key, e->key.scancode);
        if (out->as.key.key == BARE_KEY_NONE) return false;
        out->as.key.down = e->key.down;
        out->as.key.repeat = e->key.repeat;
        out->as.key.mods = map_mods(e->key.mod);
        return true;
    case SDL_EVENT_TEXT_INPUT:
        out->kind = BARE_EVENT_TEXT;
        out->as.text.utf8 = keep_text(e->text.text, &out->as.text.len);
        out->as.text.cursor = out->as.text.len;
        return out->as.text.len > 0;
    case SDL_EVENT_TEXT_EDITING: {
        out->kind = BARE_EVENT_COMPOSITION;
        const char *t = keep_text(e->edit.text, &out->as.text.len);
        out->as.text.utf8 = t;
        /* SDL's cursor counts characters; Bare's is a byte offset. */
        int cursor = out->as.text.len;
        for (int i = 0, chars = 0; e->edit.start >= 0 && i <= out->as.text.len; i++) {
            if (i == out->as.text.len || (t[i] & 0xC0) != 0x80) {
                if (chars++ == e->edit.start) { cursor = i; break; }
            }
        }
        out->as.text.cursor = cursor;
        return true;
    }
    default:
        if (e->type == sdl.file_event && sdl.file_event) {
            SDL_free(sdl.chosen);
            sdl.chosen = e->user.data2;
            out->kind = BARE_EVENT_FILE_CHOSEN;
            SDL_memcpy(&out->as.file.request, e->user.data1, sizeof(uint64_t));
            SDL_free(e->user.data1);
            out->as.file.path = sdl.chosen;
            return true;
        }
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
    for (TextChunk *c = sdl.text; c; c = c->next) c->used = 0;   /* the last batch has been handled */
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

static void start_text_input(void *self, BareTextInputKind kind, BareRect caret) {
    (void)self;
    if (kind != sdl.text_kind) {
        if (sdl.text_kind) SDL_StopTextInput(sdl.window);
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER,
                              kind == BARE_TEXT_INPUT_PASSWORD ? SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN
                                                                : SDL_TEXTINPUT_TYPE_TEXT);
        SDL_SetBooleanProperty(props, SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN, kind == BARE_TEXT_INPUT_MULTILINE);
        SDL_StartTextInputWithProperties(sdl.window, props);
        SDL_DestroyProperties(props);
        sdl.text_kind = kind;
    }
    float k = to_logical();   /* logical -> window coordinates */
    SDL_Rect r = { (int)(caret.x / k), (int)(caret.y / k), (int)(caret.w / k) + 1, (int)(caret.h / k) + 1 };
    SDL_SetTextInputArea(sdl.window, &r, 0);
}

static void stop_text_input(void *self) {
    (void)self;
    if (sdl.text_kind) SDL_StopTextInput(sdl.window);
    sdl.text_kind = BARE_TEXT_INPUT_NONE;
}

static void *native(void *self, BareNativeKind what) {
    (void)self;
    return what == BARE_NATIVE_SDL_WINDOW ? sdl.window : NULL;
}

static void minimize(void *self) { (void)self; SDL_MinimizeWindow(sdl.window); }

static bool maximized(void *self) {
    (void)self;
    return (SDL_GetWindowFlags(sdl.window) & SDL_WINDOW_MAXIMIZED) != 0;
}

static void maximize(void *self) {
    if (maximized(self)) SDL_RestoreWindow(sdl.window);
    else SDL_MaximizeWindow(sdl.window);
}

static void set_min_size(void *self, int width, int height) {
    (void)self;
    float k = to_logical();
    SDL_SetWindowMinimumSize(sdl.window, (int)(width / k), (int)(height / k));
}

/* SDL sends EXPOSED with data1 = 1 from a timer while Windows runs its modal
 * move/size loop (SDL_OnWindowLiveResizeUpdate); an event watch sees it at once. */
static bool SDLCALL live_watch(void *user, SDL_Event *e) {
    (void)user;
    if (!sdl.live) return true;
    BareEvent out;
    memset(&out, 0, sizeof out);
    if (e->type == SDL_EVENT_WINDOW_EXPOSED && e->window.data1 == 1) {
        out.kind = BARE_EVENT_EXPOSE;
        sdl.live(sdl.live_user, &out);
    } else if (e->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        out.kind = BARE_EVENT_RESIZE;
        window_size(NULL, &out.as.size.w, &out.as.size.h, &out.as.size.scale);
        sdl.live(sdl.live_user, &out);
    }
    return true;
}

static void set_live(void *self, void (*live)(void *user, const BareEvent *e), void *user) {
    (void)self;
    if (live && !sdl.live) SDL_AddEventWatch(live_watch, NULL);
    else if (!live && sdl.live) SDL_RemoveEventWatch(live_watch, NULL);
    sdl.live = live;
    sdl.live_user = user;
}

static void set_drag_query(void *self, int (*query)(void *user, float x, float y), void *user) {
    (void)self;
    sdl.drag_query = query;
    sdl.drag_user = user;
}

#ifdef _WIN32
static void backdrop_wake(void *user) { wake(user); }
#endif

static const void *backdrop(void *self, int *width, int *height) {
    (void)self;
#ifdef _WIN32
    if (!sdl.backdrop && !sdl.backdrop_failed) {
        void *hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(sdl.window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        sdl.backdrop = bare__backdrop_start(hwnd, backdrop_wake, &sdl);
        sdl.backdrop_failed = !sdl.backdrop;
    }
    return bare__backdrop_take(sdl.backdrop, width, height);
#else
    (void)width; (void)height;
    return NULL;
#endif
}

/* SDL may call back on another thread: the answer travels as an event. */
static void dialog_done(void *user, const char *const *files, int filter) {
    (void)filter;
    SDL_Event e;
    SDL_zero(e);
    e.type = sdl.file_event;
    e.user.data1 = user;   /* the request id, on the heap */
    e.user.data2 = files && files[0] ? SDL_strdup(files[0]) : NULL;
    if (!SDL_PushEvent(&e)) {
        SDL_free(e.user.data1);
        SDL_free(e.user.data2);
    }
}

static void file_dialog(void *self, uint64_t request, int kind, const char *filters, const char *path) {
    (void)self;
    if (!sdl.file_event) sdl.file_event = SDL_RegisterEvents(1);
    uint64_t *id = SDL_malloc(sizeof *id);
    if (!sdl.file_event || !id) { SDL_free(id); return; }
    *id = request;
    /* "Name:ext;ext|Name:ext" -> SDL's filters, kept until the dialog closes */
    int n = 0;
    SDL_strlcpy(sdl.filter_text, filters ? filters : "", sizeof sdl.filter_text);
    for (char *s = sdl.filter_text; *s && n < 16;) {
        char *end = SDL_strchr(s, '|'), *colon;
        if (end) *end = 0;
        colon = SDL_strchr(s, ':');
        if (colon) {
            *colon = 0;
            sdl.filters[n].name = s;
            sdl.filters[n].pattern = colon + 1;
            n++;
        }
        if (!end) break;
        s = end + 1;
    }
    if (kind == 1) SDL_ShowSaveFileDialog(dialog_done, id, sdl.window, n ? sdl.filters : NULL, n, path);
    else if (kind == 2) SDL_ShowOpenFolderDialog(dialog_done, id, sdl.window, path, false);
    else SDL_ShowOpenFileDialog(dialog_done, id, sdl.window, n ? sdl.filters : NULL, n, path, false);
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
        .start_text_input = start_text_input,
        .stop_text_input = stop_text_input,
        .native = native,
        .file_dialog = file_dialog,
        .minimize = minimize,
        .maximize = maximize,
        .maximized = maximized,
        .set_min_size = set_min_size,
        .set_drag_query = set_drag_query,
        .set_live = set_live,
        .backdrop = backdrop,
        .run = NULL,
    };
    return p;
}
