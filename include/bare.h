/*
 * bare.h: the whole public API of Bare, a small native UI framework in C11.
 *
 * STATUS: design sketch (declarations only). Nothing here is implemented yet.
 * The milestone that delivers each part is noted as (M1)...(M9); see docs/ROADMAP.md.
 *
 * The simplest program:
 *
 *     #include "bare.h"
 *     int main(void) {
 *         bare_window(1920, 1080, "My App");
 *         while (bare_frame()) {
 *             bare_box(.width = 300, .height = 200, .radius = 16,
 *                      .color = BARE_RGB(40, 120, 255));
 *         }
 *         bare_quit();
 *     }
 *
 * A window with a column of rounded, shadowed cards and a button that springs
 * when hovered and pressed:
 *
 *     #include "bare.h"
 *     int main(void) {
 *         bare_window(800, 600, "Cards");
 *         int clicks = 0;
 *         while (bare_frame()) {
 *             bare_column(.width = BARE_GROW, .height = BARE_GROW, .padding = 32,
 *                         .gap = 16, .align = BARE_CENTER) {
 *                 for (int i = 0; i < 3; i++) {
 *                     bare_column(.id = BARE_IDI("card", i), .width = 360, .padding = 20,
 *                                 .gap = 6, .radius = 16, .color = BARE_RGB(36, 38, 46),
 *                                 .shadow = BARE_SHADOW_SOFT) {
 *                         bare_text("Card title", .size = 20);
 *                         bare_text("Rounded, shadowed, laid out by flexbox.",
 *                                   .color = BARE_RGB(160, 165, 180));
 *                     }
 *                 }
 *                 bare_row(.id = BARE_ID("button"), .pad = {12, 24, 12, 24}, .radius = 12,
 *                          .color = BARE_RGB(40, 120, 255), .cursor = BARE_CURSOR_HAND,
 *                          .hover = {.scale = 1.06f, .color = BARE_RGB(70, 145, 255)},
 *                          .press = {.scale = 0.94f},
 *                          .animate = BARE_SPRING_BOUNCY) {
 *                     bare_textf("Clicked %d times", clicks);
 *                     if (bare_this().clicked) clicks++;
 *                 }
 *             }
 *         }
 *         bare_quit();
 *     }
 *
 * (From M8 the button is one line: `if (bare_button("Save").clicked) save();`)
 *
 * Rules of the API:
 *   - Elements take named, optional parameters (C11 designated initializers).
 *     Every unset field has a sensible default, and 0 always means "default".
 *     Where the default isn't 0 (opacity, scale, grow, shrink, text size) and
 *     you really mean zero, write BARE_ZERO: `.opacity = BARE_ZERO`.
 *   - Units are logical pixels. Bare scales to the display's DPI.
 *   - Containers are blocks: `bare_row(...) { children }`. Don't `break`,
 *     `return` or `goto` out of a container block. Bare detects an unclosed
 *     container at the end of the frame, reports where it was opened, and closes it.
 *   - Strings passed to Bare are copied; they only need to live for the call.
 *   - Advanced users (custom backends, headless use, raw draw commands)
 *     include <bare/bare_core.h> and <bare/bare_backend.h>. Nobody else needs them.
 */
#ifndef BARE_H
#define BARE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Basic types                                                              */
/* ------------------------------------------------------------------------ */

typedef struct BareColor { uint8_t r, g, b, a; } BareColor;   /* sRGB, straight alpha */
typedef struct BareVec2  { float x, y; } BareVec2;
typedef struct BareRect  { float x, y, w, h; } BareRect;
typedef struct BareEdges { float top, right, bottom, left; } BareEdges;  /* CSS order */

#define BARE_RGB(r, g, b)      ((BareColor){ (uint8_t)(r), (uint8_t)(g), (uint8_t)(b), 255 })
#define BARE_RGBA(r, g, b, a)  ((BareColor){ (uint8_t)(r), (uint8_t)(g), (uint8_t)(b), (uint8_t)(a) })
#define BARE_HEX(rgb)          BARE_RGB(((rgb) >> 16) & 255, ((rgb) >> 8) & 255, (rgb) & 255)
#define BARE_WHITE             BARE_RGB(255, 255, 255)
#define BARE_BLACK             BARE_RGB(0, 0, 0)

/* A real zero for fields whose default isn't zero (negative zero, told apart
 * with signbit). Plain 0 in those fields means "default". */
#define BARE_ZERO              (-0.0f)

/* ------------------------------------------------------------------------ */
/* Element IDs (M2)                                                         */
/* ------------------------------------------------------------------------ */

/* Every element has a stable ID that keys its retained state (animations,
 * hover/press, scroll offset) across frames.
 *   - Not set (0): Bare derives one from the parent's ID, the call site
 *     (file + line) and how many times that call site ran under this parent.
 *     Fine for static UI.
 *   - Set it on anything whose position among its siblings can change (list
 *     items, conditionally shown elements) so its state follows it. */
typedef uint64_t BareId;

#define BARE_ID(name)          bare_id(name)
#define BARE_IDI(name, index)  bare_id_index((name), (uint32_t)(index))

BareId bare_id(const char *name);
BareId bare_id_index(const char *name, uint32_t index);

/* ------------------------------------------------------------------------ */
/* Layout values (M2)                                                       */
/* ------------------------------------------------------------------------ */

/* .width / .height take a number of pixels or one of these: */
#define BARE_FIT          0.0f                               /* default: wrap the content */
#define BARE_GROW         (-1.0f)                            /* fill the free space (weight: .grow) */
#define BARE_PERCENT(p)   (-2.0f - (float)(p) / 100.0f)      /* p% (0..100) of the parent's content box */

typedef enum BareDirection {
    BARE_COLUMN = 0,      /* default: children top to bottom */
    BARE_ROW,             /* children left to right */
    BARE_STACK            /* children on top of each other (later ones in front) */
} BareDirection;

typedef enum BareAlign {
    BARE_START = 0,       /* .align and .justify */
    BARE_CENTER,
    BARE_END,
    BARE_STRETCH,         /* .align only: children fill the cross axis */
    BARE_SPACE_BETWEEN,   /* .justify only */
    BARE_SPACE_AROUND,
    BARE_SPACE_EVENLY
} BareAlign;

typedef enum BareScroll {
    BARE_SCROLL_NONE = 0,
    BARE_SCROLL_X,
    BARE_SCROLL_Y,
    BARE_SCROLL_BOTH
} BareScroll;

/* ------------------------------------------------------------------------ */
/* Looks (M1, M4)                                                           */
/* ------------------------------------------------------------------------ */

typedef struct BareShadow {
    float x, y;           /* offset */
    float blur;           /* blur radius */
    float spread;         /* grows (or with < 0 shrinks) the shadow shape */
    BareColor color;      /* alpha 0 = no shadow */
} BareShadow;

#define BARE_SHADOW_SOFT   ((BareShadow){ 0, 8, 24, 0, { 0, 0, 0, 70 } })
#define BARE_SHADOW_SMALL  ((BareShadow){ 0, 2, 6, 0, { 0, 0, 0, 60 } })
#define BARE_SHADOW_LARGE  ((BareShadow){ 0, 20, 48, -4, { 0, 0, 0, 90 } })

/* Overrides applied while an element is hovered or pressed. A zero field
 * means "unchanged" (BARE_ZERO for a real zero). Changes are animated with the element's .animate. */
typedef struct BareLook {
    BareColor color;
    BareColor border_color;
    BareColor text_color;
    BareShadow shadow;
    float opacity;
    float scale;          /* visual only: scales about the centre, children included */
    float radius;
    BareVec2 offset;      /* visual nudge in pixels, e.g. {0, -2} to lift on hover */
} BareLook;

/* Liquid glass: the box refracts whatever was drawn behind it through a curved,
 * light-catching rim. Any non-zero field turns it on; other fields at 0 take
 * their defaults. `.glass = BARE_GLASS` is the usual start. The box's .color
 * and .border are drawn on top of the glass (a light .color frosts it), its
 * .shadow under it. */
typedef struct BareGlass {
    float refraction;     /* how strongly the rim bends the backdrop (default 1) */
    float depth;          /* width of the curved rim in pixels (default 20) */
    float blur;           /* frost, in pixels (default 2; BARE_ZERO = perfectly clear) */
    float dispersion;     /* colour fringing at the rim (default 1; BARE_ZERO = none) */
    float shine;          /* rim highlight (default 1; BARE_ZERO = none) */
    float merge;          /* melt into neighbouring glass siblings within this many pixels (default 0) */
    BareColor tint;       /* alpha = strength */
} BareGlass;

#define BARE_GLASS          ((BareGlass){ .refraction = 1.0f })
#define BARE_GLASS_FROSTED  ((BareGlass){ .refraction = 0.6f, .blur = 18.0f, .tint = { 255, 255, 255, 30 } })

typedef enum BareCursor {
    BARE_CURSOR_DEFAULT = 0,
    BARE_CURSOR_HAND,
    BARE_CURSOR_TEXT,
    BARE_CURSOR_MOVE,
    BARE_CURSOR_RESIZE_EW,
    BARE_CURSOR_RESIZE_NS,
    BARE_CURSOR_NOT_ALLOWED
} BareCursor;

/* ------------------------------------------------------------------------ */
/* Animation (M4)                                                           */
/* ------------------------------------------------------------------------ */

typedef enum BareAnimKind {
    BARE_ANIM_NONE = 0,   /* default: changes are instant */
    BARE_ANIM_EASE,
    BARE_ANIM_SPRING
} BareAnimKind;

typedef struct BareAnim {
    BareAnimKind kind;
    float duration;       /* ease: seconds */
    float bezier[4];      /* ease: cubic-bezier x1, y1, x2, y2 (as in CSS) */
    float response;       /* spring: seconds, roughly the time to settle */
    float damping;        /* spring: 1 = critically damped (no overshoot), < 1 bounces */
} BareAnim;

/* Curves for BARE_EASE */
#define BARE_LINEAR                  0.0f, 0.0f, 1.0f, 1.0f
#define BARE_EASE_IN                 0.42f, 0.0f, 1.0f, 1.0f
#define BARE_EASE_OUT                0.0f, 0.0f, 0.58f, 1.0f
#define BARE_EASE_IN_OUT             0.42f, 0.0f, 0.58f, 1.0f
#define BARE_BEZIER(x1, y1, x2, y2)  (x1), (y1), (x2), (y2)

#define BARE_EASE(secs, curve)  ((BareAnim){ .kind = BARE_ANIM_EASE, .duration = (secs), .bezier = { curve } })
#define BARE_SPRING(resp, damp) ((BareAnim){ .kind = BARE_ANIM_SPRING, .response = (resp), .damping = (damp) })

#define BARE_SPRING_SMOOTH  BARE_SPRING(0.35f, 1.00f)   /* settles without overshoot */
#define BARE_SPRING_SNAPPY  BARE_SPRING(0.20f, 0.85f)   /* quick, a hint of overshoot */
#define BARE_SPRING_BOUNCY  BARE_SPRING(0.45f, 0.55f)   /* playful wobble */

/* ------------------------------------------------------------------------ */
/* Fonts and images (M5)                                                    */
/* ------------------------------------------------------------------------ */

typedef uint32_t BareFont;    /* 0 = the built-in default font */
typedef uint32_t BareImage;   /* 0 = no image */

BareFont  bare_font_load(const char *path);                       /* TTF/OTF; 0 on failure */
BareFont  bare_font_from_memory(const void *data, int size);      /* data must outlive the font */
BareImage bare_image_load(const char *path);                      /* PNG/JPG; 0 on failure */
BareImage bare_image_from_pixels(int width, int height, const void *rgba8);  /* pixels are copied */
void      bare_image_free(BareImage image);

/* ------------------------------------------------------------------------ */
/* Elements                                                                 */
/* ------------------------------------------------------------------------ */

typedef struct BareBox {
    BareId id;                  /* 0 = automatic (see BareId) */
    const struct BareBox *style;/* (M7) base style; fields left at 0 come from it */

    /* size: pixels, BARE_FIT (default), BARE_GROW or BARE_PERCENT(p) */
    float width, height;
    float min_width, max_width, min_height, max_height;   /* 0 = no limit */
    float grow;                 /* share of free space for BARE_GROW (0 = default 1) */
    float shrink;               /* share of overflow taken back (0 = default 1; fixed sizes never shrink) */

    /* children */
    BareDirection dir;          /* set by bare_row / bare_column */
    BareAlign align;            /* cross axis (default BARE_START) */
    BareAlign justify;          /* main axis (default BARE_START) */
    float gap;                  /* space between children */
    float padding;              /* all four sides */
    BareEdges pad;              /* per side; replaces .padding when any side is set */

    /* looks */
    BareColor color;            /* background; alpha 0 = none */
    BareColor border_color;
    float border;               /* border width, drawn inside the box */
    float radius;               /* corner radius */
    BareShadow shadow;
    float opacity;              /* 0 = default 1; multiplies into the children */
    float scale;                /* 0 = default 1; visual only (doesn't affect layout) */
    BareVec2 offset;            /* visual move in pixels, children included; doesn't affect layout.
                                   Not snapped to whole pixels, so slow motion stays smooth */
    BareGlass glass;            /* liquid glass (see BareGlass) */
    BareColor text_color;       /* inherited by text inside; alpha 0 = inherit */
    BareImage image;            /* (M5) fills the box (BARE_FIT = the image's own size) */

    /* interaction (M3, M6) */
    BareLook hover, press;      /* looks while hovered / pressed */
    BareCursor cursor;
    bool passthrough;           /* ignore the mouse (CSS pointer-events: none) */
    bool clip;                  /* (M6) clip children to the box */
    BareScroll scroll;          /* (M6) scroll overflowing children; implies .clip */

    /* animation (M4) */
    BareAnim animate;           /* how every animatable property moves to its new value */
    BareAnim animate_layout;    /* overrides .animate for position and size */
} BareBox;

typedef struct BareTextStyle {
    BareId id;
    BareFont font;              /* 0 = default */
    float size;                 /* 0 = default 16 */
    BareColor color;            /* alpha 0 = inherited from the nearest .text_color */
    float line_height;          /* multiple of size; 0 = default 1.3 */
    BareAlign align;            /* BARE_START / CENTER / END within the line box */
    bool nowrap;                /* default: wraps at the available width */
} BareTextStyle;

/* What happened to an element. Results reflect input up to the start of this
 * frame against what was on screen last frame, so they are ready right away. */
typedef struct BareState {
    bool hovered;               /* the mouse is over it or one of its children */
    bool pressed;               /* a press started on it and the button is still down */
    bool clicked;               /* pressed and released over it (this frame) */
    bool right_clicked;
    BareRect rect;              /* where it was last frame, logical pixels */
    BareVec2 scroll;            /* (M6) current scroll offset */
} BareState;

/* Leaf box. Returns its state. */
#define bare_box(...)  bare_box_(&(BareBox){ __VA_ARGS__ }, __FILE__, __LINE__)

/* Containers: use as `bare_row(.gap = 8) { ... }`. */
#define bare_row(...)     BARE__CONTAINER(.dir = BARE_ROW, __VA_ARGS__)
#define bare_column(...)  BARE__CONTAINER(.dir = BARE_COLUMN, __VA_ARGS__)
#define bare_stack(...)   BARE__CONTAINER(.dir = BARE_STACK, __VA_ARGS__)    /* layers; .align places them */

/* State of the innermost open container (call inside its block). */
BareState bare_this(void);

/* Text (M5; until then layout uses a stand-in measurer). */
#define bare_text(str, ...)  bare_text_((str), &(BareTextStyle){ __VA_ARGS__ }, __FILE__, __LINE__)
#define bare_textf(...)      bare_textf_(__FILE__, __LINE__, __VA_ARGS__)   /* printf-style, default style */

/* (M7) Reusable styles are plain BareBox values:
 *     static const BareBox card = { .radius = 16, .padding = 20, .color = {36, 38, 46, 255} };
 *     bare_column(.style = &card, .width = 300) { ... }
 * (In static initializers write colours as braces; BARE_RGB is a compound literal.) */

/* ------------------------------------------------------------------------ */
/* Widgets (M8): one call each, built only from the elements above          */
/* ------------------------------------------------------------------------ */

#define bare_button(label, ...)          bare_button_((label), &(BareBox){ __VA_ARGS__ }, __FILE__, __LINE__)
#define bare_checkbox(label, value, ...) bare_checkbox_((label), (value), &(BareBox){ __VA_ARGS__ }, __FILE__, __LINE__)
#define bare_slider(value, lo, hi, ...)  bare_slider_((value), (lo), (hi), &(BareBox){ __VA_ARGS__ }, __FILE__, __LINE__)
/* Lists: a bare_column(.scroll = BARE_SCROLL_Y) of items with BARE_IDI ids;
 * bare_list_item gives the standard hover/selected look. */
#define bare_list_item(label, selected, ...) bare_list_item_((label), (selected), &(BareBox){ __VA_ARGS__ }, __FILE__, __LINE__)

/* ------------------------------------------------------------------------ */
/* Window, frame loop, input                                                */
/* ------------------------------------------------------------------------ */

/* Opens the window (logical pixels). One window is current; no handle needed. */
bool  bare_window(int width, int height, const char *title);

/* Ends the previous frame (layout, animate, draw) and starts the next one.
 * Returns false when the window is closed. When nothing is animating and no
 * input arrives it sleeps, so an idle app uses no CPU (see bare_continuous). */
bool  bare_frame(void);

/* Callback form of the loop: calls frame(user) once per frame until the window
 * closes. Works on every backend; required where the platform owns the main
 * loop (sokol_app, the web). Call bare_window first. */
void  bare_run(void (*frame)(void *user), void *user);

void  bare_quit(void);                  /* closes the window, frees everything */

void  bare_background(BareColor color); /* window clear colour (default: dark grey) */
void  bare_title(const char *title);
BareVec2 bare_window_size(void);
void  bare_continuous(bool on);         /* redraw every frame even when idle (games, video) */
void  bare_wake(void);                  /* thread-safe: ask for a frame (e.g. data arrived) */

double bare_time(void);                 /* seconds since bare_window */
float  bare_dt(void);                   /* seconds since the previous frame */

BareVec2 bare_mouse(void);

typedef enum BareKey {
    BARE_KEY_NONE = 0,
    BARE_KEY_ESCAPE, BARE_KEY_ENTER, BARE_KEY_SPACE, BARE_KEY_TAB, BARE_KEY_BACKSPACE,
    BARE_KEY_LEFT, BARE_KEY_RIGHT, BARE_KEY_UP, BARE_KEY_DOWN,
    BARE_KEY_A, /* ... letters, digits, F-keys follow in M3 */
    BARE_KEY_COUNT_
} BareKey;

bool bare_key_down(BareKey key);
bool bare_key_pressed(BareKey key);     /* went down this frame */

const char *bare_clipboard(void);       /* valid until the next frame */
void        bare_set_clipboard(const char *text);

/* ------------------------------------------------------------------------ */
/* Internals the macros above expand to. Not for direct use.                */
/* ------------------------------------------------------------------------ */

#define BARE__CAT2(a, b) a##b
#define BARE__CAT(a, b)  BARE__CAT2(a, b)
/* Each container's loop variable needs its own name, or containers nested on
 * one line shadow each other (-Wshadow, C4456). __COUNTER__ isn't C11 but
 * GCC, Clang and MSVC all have it. */
#ifdef __COUNTER__
#  define BARE__UNIQUE __COUNTER__
#else
#  define BARE__UNIQUE __LINE__
#endif
#define BARE__CONTAINER(...) BARE__CONTAINER_(BARE__CAT(bare__open_, BARE__UNIQUE), __VA_ARGS__)
#define BARE__CONTAINER_(v, ...) \
    for (int v = bare_open_(&(BareBox){ __VA_ARGS__ }, __FILE__, __LINE__); v; v = bare_close_())

BareState bare_box_(const BareBox *box, const char *file, int line);
int       bare_open_(const BareBox *box, const char *file, int line);   /* returns 1 */
int       bare_close_(void);                                            /* returns 0 */
BareState bare_text_(const char *text, const BareTextStyle *style, const char *file, int line);
#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
BareState bare_textf_(const char *file, int line, const char *fmt, ...);
BareState bare_button_(const char *label, const BareBox *box, const char *file, int line);
bool      bare_checkbox_(const char *label, bool *value, const BareBox *box, const char *file, int line);
bool      bare_slider_(float *value, float lo, float hi, const BareBox *box, const char *file, int line);
BareState bare_list_item_(const char *label, bool selected, const BareBox *box, const char *file, int line);

#ifdef __cplusplus
}
#endif

#endif /* BARE_H */
