/*
 * bare.h: the whole public API of Bare, a small native UI framework in C11.
 *
 * Parts marked (M8)...(M9) aren't implemented yet; see docs/ROADMAP.md.
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
/* Transparent. Unlike an unset colour (all zero) it counts as set, so it
 * overrides a style's colour: `.style = &card, .color = BARE_CLEAR`. */
#define BARE_CLEAR             BARE_RGBA(255, 255, 255, 0)

/* A real zero for fields whose default isn't zero (negative zero, told apart
 * with signbit). Plain 0 in those fields means "default". */
#define BARE_ZERO              (-0.0f)

/* ------------------------------------------------------------------------ */
/* Element IDs                                                              */
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
/* Layout values                                                            */
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
    BARE_ALIGN_DEFAULT = 0,   /* start (0 lets a style's value through) */
    BARE_START,           /* .align and .justify */
    BARE_CENTER,
    BARE_END,
    BARE_STRETCH,         /* .align only: children fill the cross axis */
    BARE_SPACE_BETWEEN,   /* .justify only */
    BARE_SPACE_AROUND,
    BARE_SPACE_EVENLY
} BareAlign;

/* .scroll: which way a box scrolls content that overflows it (CSS overflow:
 * auto). The wheel scrolls the innermost scroll box under the mouse that can
 * still move that way (Shift turns the wheel sideways); a mouse wheel glides,
 * a touchpad follows the fingers. A thin scrollbar shows while scrolling or
 * while the mouse moves over the box, and can be dragged. */
typedef enum BareScroll {
    BARE_SCROLL_NONE = 0,
    BARE_SCROLL_X,
    BARE_SCROLL_Y,
    BARE_SCROLL_BOTH
} BareScroll;

/* ------------------------------------------------------------------------ */
/* Looks                                                                    */
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
 * .shadow under it.
 *
 * With .animate, glass settings animate like everything else, and turning
 * glass on or off is animated too: it condenses out of the backdrop and
 * dissolves back into it (a solid knob can turn to glass while pressed). */
typedef struct BareGlass {
    float refraction;     /* how strongly the rim bends the backdrop (default 1) */
    float depth;          /* width of the curved rim in pixels (default 20) */
    float blur;           /* frost, in pixels (default 2; BARE_ZERO = perfectly clear) */
    float dispersion;     /* colour fringing at the rim (default 1; BARE_ZERO = none) */
    float shine;          /* rim highlight (default 1; BARE_ZERO = none) */
    float merge;          /* melt into neighbouring glass siblings within this many pixels (default 0) */
    float zoom;           /* magnifies what's behind, like a loupe (default 1) */
    float light;          /* turns the light catching the rim, degrees clockwise from the top left (default 0) */
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
    BARE_CURSOR_NOT_ALLOWED,
    BARE_CURSOR_ARROW     /* the normal arrow, even over a parent or style that set another */
} BareCursor;

/* ------------------------------------------------------------------------ */
/* Animation                                                                */
/* ------------------------------------------------------------------------ */

typedef enum BareAnimKind {
    BARE_ANIM_NONE = 0,   /* default: changes are instant */
    BARE_ANIM_EASE,
    BARE_ANIM_SPRING
} BareAnimKind;

/* How an element moves to new values: position, size, colours, opacity,
 * radius, border, shadow, scale and offset (looks included). Interrupting an
 * animation continues from where it is: no jumps. An element's first frame
 * never animates. Positions are relative to the parent, so children ride
 * along with a moving parent. Give elements that change place a stable id
 * (BARE_IDI) so Bare can tell they moved. */
typedef struct BareAnim {
    BareAnimKind kind;
    float duration;       /* ease: seconds (0 = default 0.25; BARE_ZERO = instant) */
    float bezier[4];      /* ease: cubic-bezier x1, y1, x2, y2 as in CSS (all 0 = CSS `ease`) */
    float response;       /* spring: seconds, roughly the time to settle (0 = default 0.35) */
    float damping;        /* spring: 1 = critically damped (no overshoot), < 1 bounces (0 = default 1) */
} BareAnim;

/* Curves for BARE_EASE */
#define BARE_LINEAR                  0.0f, 0.0f, 1.0f, 1.0f
#define BARE_EASE_IN                 0.42f, 0.0f, 1.0f, 1.0f
#define BARE_EASE_OUT                0.0f, 0.0f, 0.58f, 1.0f
#define BARE_EASE_IN_OUT             0.42f, 0.0f, 0.58f, 1.0f
#define BARE_BEZIER(x1, y1, x2, y2)  (x1), (y1), (x2), (y2)

#define BARE_EASE(secs, curve)  ((BareAnim){ .kind = BARE_ANIM_EASE, .duration = (secs), .bezier = { curve } })
#define BARE_SPRING(resp, damp) ((BareAnim){ .kind = BARE_ANIM_SPRING, .response = (resp), .damping = (damp) })

/* No animation, overriding a style's (or, as .animate_layout, keeping layout
 * instant while looks animate). */
#define BARE_INSTANT        ((BareAnim){ .kind = BARE_ANIM_EASE, .duration = BARE_ZERO })

#define BARE_SPRING_SMOOTH  BARE_SPRING(0.35f, 1.00f)   /* settles without overshoot */
#define BARE_SPRING_SNAPPY  BARE_SPRING(0.20f, 0.85f)   /* quick, a hint of overshoot */
#define BARE_SPRING_BOUNCY  BARE_SPRING(0.45f, 0.55f)   /* playful wobble */

/* ------------------------------------------------------------------------ */
/* Fonts and images                                                         */
/* ------------------------------------------------------------------------ */

typedef uint32_t BareFont;    /* 0 = the built-in default font */
typedef uint32_t BareImage;   /* 0 = no image */

BareFont  bare_font_load(const char *path);                       /* TTF/OTF; 0 (the default font) on failure */
BareFont  bare_font_from_memory(const void *data, int size);      /* data must outlive the font */
BareImage bare_image_load(const char *path);                      /* PNG, JPEG, BMP, GIF, TGA; 0 on failure */
BareImage bare_image_from_pixels(int width, int height, const void *rgba8);  /* pixels are copied */
void      bare_image_free(BareImage image);                       /* images and canvases */
BareVec2  bare_image_size(BareImage image);                       /* in pixels; {0, 0} if unknown */

/* ------------------------------------------------------------------------ */
/* Canvas: your own GPU shaders (what WebGL is to a <canvas>)               */
/* ------------------------------------------------------------------------ */

/* A canvas is an image that shaders draw into. Show it with .image like any
 * other image; use it as a shader input; draw into it every frame for
 * simulations, effects and generated backgrounds.
 *
 *   BareImage sky = bare_canvas(.width = 512, .height = 512);
 *   bare_pass(.target = sky, .shader = clouds, BARE_PARAMS(p));
 *   bare_box(.width = BARE_GROW, .height = BARE_GROW, .image = sky);
 *
 * A pass runs a fragment shader once for every pixel of its target. Passes run
 * on the GPU in the order they're called, before the frame they're called in is
 * drawn (or the next one, when called between frames). Shaders are GLSL 450,
 * compiled at build time by the bare_add_shaders() CMake function, which also
 * documents the conventions they follow. */

typedef enum BareFormat {
    BARE_RGBA8 = 0,       /* default: 8-bit colour */
    BARE_RGBA16F,         /* half floats: HDR colour, simulation state */
    BARE_RG16F,           /* two half floats (e.g. velocity) */
    BARE_R16F             /* one half float (e.g. pressure) */
} BareFormat;

typedef struct BareCanvas {
    int width, height;    /* pixels (not scaled by DPI) */
    BareFormat format;
    bool nearest;         /* sample without filtering (default: bilinear) */
    bool repeat;          /* sampling outside 0..1 wraps (default: clamps to the edge) */
} BareCanvas;

#define bare_canvas(...)  bare_canvas_(&(BareCanvas){ __VA_ARGS__ })

/* Compiled shader code; bare_add_shaders() generates one per .glsl file. */
typedef struct BareShaderCode {
    const unsigned char *spirv, *dxil, *msl;
    unsigned spirv_size, dxil_size, msl_size;
    int samplers;         /* inputs it reads */
    int uniform_buffers;  /* 0 or 1: whether it takes params */
} BareShaderCode;

typedef uint32_t BareShader;   /* 0 = none */

BareShader bare_shader(const BareShaderCode *code);   /* code must outlive the shader (generated code is static) */
void       bare_shader_free(BareShader shader);

typedef enum BareBlend {
    BARE_BLEND_NONE = 0,  /* default: the shader's output replaces the target's pixels */
    BARE_BLEND_ADD,       /* added to them */
    BARE_BLEND_OVER       /* premultiplied alpha over them */
} BareBlend;

#define BARE_MAX_INPUTS 8

typedef struct BarePass {
    BareImage target;                   /* a canvas; not one of the inputs */
    BareShader shader;
    BareImage inputs[BARE_MAX_INPUTS];  /* bound to the shader's samplers 0, 1, ... */
    const void *params;                 /* the shader's uniform block (std140), copied; see BARE_PARAMS */
    int params_size;                    /* bytes, up to 1024 */
    BareBlend blend;
} BarePass;

#define bare_pass(...)    bare_pass_(&(BarePass){ __VA_ARGS__ }, __FILE__, __LINE__)
#define BARE_PARAMS(var)  .params = &(var), .params_size = (int)sizeof(var)

/* ------------------------------------------------------------------------ */
/* Elements                                                                 */
/* ------------------------------------------------------------------------ */

typedef struct BareBox {
    BareId id;                  /* 0 = automatic (see BareId) */
    const struct BareBox *style;/* base style: fields left at 0 come from it (see "Styles" below) */

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
    BareColor text_color;       /* inherited by text inside; alpha 0 = inherit (white at the top) */
    BareImage image;            /* drawn over .color, stretched to the box, rounded by .radius */

    /* interaction */
    BareLook hover, press;      /* looks while hovered / pressed */
    BareCursor cursor;
    bool passthrough;           /* the mouse goes through it (CSS pointer-events: none); children still get it */
    bool clip;                  /* clip children to the box, inside its border (CSS overflow: hidden) */
    BareScroll scroll;          /* scroll overflowing children (see BareScroll); implies .clip */

    /* animation */
    BareAnim animate;           /* how every animatable property moves to its new value (default: instantly) */
    BareAnim animate_layout;    /* overrides .animate for position and size */
} BareBox;

typedef struct BareTextStyle {
    BareId id;
    const struct BareTextStyle *style;   /* base style: fields left at 0 come from it */
    BareFont font;              /* 0 = default */
    float size;                 /* 0 = default 16 */
    BareColor color;            /* alpha 0 = inherited from the nearest .text_color (default white) */
    float line_height;          /* multiple of size; 0 = default 1.3 */
    BareAlign align;            /* BARE_START / CENTER / END within the line box */
    bool nowrap;                /* default: wraps at spaces and after hyphens to fit its width */
    BareAnim animate;           /* how its colour changes (default: instantly; inherited colour follows the parent) */
} BareTextStyle;

/* What happened to an element. Results reflect input up to the start of this
 * frame against what was on screen last frame, so they are ready right away. */
typedef struct BareState {
    bool hovered;               /* the mouse is over it or one of its children */
    bool pressed;               /* a press started on it and the button is still down */
    bool clicked;               /* pressed and released over it (this frame) */
    bool right_clicked;
    BareRect rect;              /* where it was last frame, logical pixels */
    BareVec2 scroll;            /* scroll boxes: how far the content is scrolled this frame */
} BareState;

/* Leaf box. Returns its state. */
#define bare_box(...)  bare_box_(&(BareBox){ __VA_ARGS__ }, __FILE__, __LINE__)

/* Containers: use as `bare_row(.gap = 8) { ... }`. */
#define bare_row(...)     BARE__CONTAINER(.dir = BARE_ROW, __VA_ARGS__)
#define bare_column(...)  BARE__CONTAINER(.dir = BARE_COLUMN, __VA_ARGS__)
#define bare_stack(...)   BARE__CONTAINER(.dir = BARE_STACK, __VA_ARGS__)    /* layers; .align places them */

/* State of the innermost open container (call inside its block). */
BareState bare_this(void);

/* Text: a leaf element as wide as its longest line, but no wider than its
 * parent allows (then it wraps; a row shrinks it down to its longest word).
 * The default font is Inter (Latin); pass .font = bare_font_load(...) for others. */
#define bare_text(str, ...)  bare_text_((str), &(BareTextStyle){ __VA_ARGS__ }, __FILE__, __LINE__)
#define bare_textf(...)      bare_textf_(__FILE__, __LINE__, __VA_ARGS__)   /* printf-style, default style */

/* Long lists: inside a scroll box, declare only the items on screen. Items
 * are `size` pixels long along the box's direction (its .gap is added); Bare
 * keeps room for the rest, so scrolling and the scrollbar behave as if all
 * `count` were there. One element per item, and no `break` out of the loop.
 *
 *     bare_column(.height = BARE_GROW, .scroll = BARE_SCROLL_Y) {
 *         bare_visible(i, 10000, 32) {
 *             bare_row(.id = BARE_IDI("row", i), .height = 32) { bare_textf("Row %d", i); }
 *         }
 *     }
 *
 * In a box that doesn't scroll along its direction every item is declared. */
#define bare_visible(i, count, size) \
    for (int i = bare_visible_((count), (float)(size), __FILE__, __LINE__); bare_visible_more_(i); i++)

/* ------------------------------------------------------------------------ */
/* Styles and themes                                                        */
/* ------------------------------------------------------------------------ */

/* Reusable styles (CSS classes) are plain BareBox / BareTextStyle values:
 *
 *     static const BareBox card = { .radius = 16, .padding = 20, .color = { 36, 38, 46, 255 } };
 *     bare_column(.style = &card, .width = 300) { ... }
 *
 * Every field the element leaves at 0 comes from its style; what it sets wins
 * (BARE_ZERO, BARE_CLEAR, BARE_START, BARE_CURSOR_ARROW and BARE_INSTANT set a
 * "nothing" value on purpose). A style can have a .style of its own (a chain,
 * most specific last). Padding (.padding and .pad), .shadow, .offset, .glass and
 * animations are taken whole; .hover and .press field by field, so a style's
 * hover colour and an element's hover scale combine. .dir and .id never come
 * from a style. A style only has to exist while elements using it are
 * declared (a static, a global or a local in the frame all work).
 * (In static initializers write colours as braces; BARE_RGB is a compound literal.) */

/* A theme: colours plus ready-made styles. The current theme also gives the
 * window background and the default text colour.
 *
 *     const BareTheme *t = bare_theme();
 *     bare_column(.style = &t->card) { bare_text("Hello", .style = &t->heading); }
 *     bare_set_theme(bare_theme_light());
 *
 * Elements using a theme's styles animate to the new theme's colours. */
typedef struct BareTheme {
    BareColor background;       /* the window */
    BareColor surface;          /* panels and cards */
    BareColor text, muted;      /* body text, secondary text */
    BareColor accent;           /* links, primary buttons, selection */
    BareColor border;           /* hairlines and dividers */
    BareBox panel;              /* a large flat surface */
    BareBox card;               /* a raised surface with a shadow */
    BareBox button;             /* a neutral button: hover and press looks, hand cursor */
    BareBox primary;            /* the accent button */
    BareBox row;                /* a list row that lights up under the mouse */
    BareTextStyle title, heading, body, caption;
} BareTheme;

const BareTheme *bare_theme(void);              /* the current theme (dark until you set one) */
void bare_set_theme(const BareTheme *theme);    /* copied: tweak a copy of a built-in for your own */
const BareTheme *bare_theme_dark(void);
const BareTheme *bare_theme_light(void);

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

void  bare_background(BareColor color); /* window clear colour (default: the theme's background) */
void  bare_title(const char *title);
BareVec2 bare_window_size(void);
float bare_pixel_scale(void);          /* physical pixels per logical pixel (devicePixelRatio) */
void  bare_continuous(bool on);         /* redraw every frame even when idle (games, video) */
void  bare_wake(void);                  /* thread-safe: ask for a frame (e.g. data arrived) */

double bare_time(void);                 /* seconds since bare_window */
float  bare_dt(void);                   /* seconds since the previous frame */

BareVec2 bare_mouse(void);              /* logical pixels, window coordinates */

/* Keys by what they type on the user's keyboard layout (Ctrl+Z is Z on
 * AZERTY too); left and right modifiers count as one. */
typedef enum BareKey {
    BARE_KEY_NONE = 0,
    BARE_KEY_ESCAPE, BARE_KEY_ENTER, BARE_KEY_SPACE, BARE_KEY_TAB, BARE_KEY_BACKSPACE,
    BARE_KEY_DELETE, BARE_KEY_INSERT, BARE_KEY_HOME, BARE_KEY_END, BARE_KEY_PAGE_UP, BARE_KEY_PAGE_DOWN,
    BARE_KEY_LEFT, BARE_KEY_RIGHT, BARE_KEY_UP, BARE_KEY_DOWN,
    BARE_KEY_SHIFT, BARE_KEY_CTRL, BARE_KEY_ALT, BARE_KEY_SUPER,   /* SUPER: Windows / Command key */
    BARE_KEY_A, BARE_KEY_B, BARE_KEY_C, BARE_KEY_D, BARE_KEY_E, BARE_KEY_F, BARE_KEY_G, BARE_KEY_H,
    BARE_KEY_I, BARE_KEY_J, BARE_KEY_K, BARE_KEY_L, BARE_KEY_M, BARE_KEY_N, BARE_KEY_O, BARE_KEY_P,
    BARE_KEY_Q, BARE_KEY_R, BARE_KEY_S, BARE_KEY_T, BARE_KEY_U, BARE_KEY_V, BARE_KEY_W, BARE_KEY_X,
    BARE_KEY_Y, BARE_KEY_Z,
    BARE_KEY_0, BARE_KEY_1, BARE_KEY_2, BARE_KEY_3, BARE_KEY_4,
    BARE_KEY_5, BARE_KEY_6, BARE_KEY_7, BARE_KEY_8, BARE_KEY_9,
    BARE_KEY_F1, BARE_KEY_F2, BARE_KEY_F3, BARE_KEY_F4, BARE_KEY_F5, BARE_KEY_F6,
    BARE_KEY_F7, BARE_KEY_F8, BARE_KEY_F9, BARE_KEY_F10, BARE_KEY_F11, BARE_KEY_F12,
    BARE_KEY_MINUS, BARE_KEY_EQUALS, BARE_KEY_COMMA, BARE_KEY_PERIOD, BARE_KEY_SLASH,
    BARE_KEY_COUNT_
} BareKey;

bool bare_key_down(BareKey key);        /* held now */
bool bare_key_pressed(BareKey key);     /* went down this frame, or auto-repeated while held (like keydown) */

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
BareImage bare_canvas_(const BareCanvas *canvas);
void      bare_pass_(const BarePass *pass, const char *file, int line);
int       bare_open_(const BareBox *box, const char *file, int line);   /* returns 1 */
int       bare_close_(void);                                            /* returns 0 */
BareState bare_text_(const char *text, const BareTextStyle *style, const char *file, int line);
int       bare_visible_(int count, float size, const char *file, int line);   /* the first item on screen */
int       bare_visible_more_(int i);
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
