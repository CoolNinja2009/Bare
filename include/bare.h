/*
 * bare.h: the whole public API of Bare, a small native UI framework in C11.
 *
 * See docs/ROADMAP.md for what's planned.
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
 * (The button is also one line: `if (bare_button("Save").clicked) save();`)
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

/* .position = BARE_ABSOLUTE takes an element out of its parent's flex flow
 * (CSS position: absolute): it doesn't take up room or move its siblings, and
 * is placed on its parent instead, at .anchor (which point of the parent and
 * of the element meet; default the top left) moved by .x, .y (pixels, right
 * and down). Its size works as usual (BARE_GROW fills the parent, a percent is
 * of the parent). It's drawn in the order declared, so declare it last to put
 * it on top: a badge on a button, a label over a canvas, a popup. */
typedef enum BarePosition {
    BARE_POSITION_FLOW = 0,   /* default: laid out by the parent */
    BARE_ABSOLUTE
} BarePosition;

typedef enum BareAnchor {
    BARE_TOP_LEFT = 0, BARE_TOP, BARE_TOP_RIGHT,
    BARE_LEFT, BARE_MIDDLE, BARE_RIGHT,
    BARE_BOTTOM_LEFT, BARE_BOTTOM, BARE_BOTTOM_RIGHT
} BareAnchor;

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

typedef enum BareFit { BARE_FIT_STRETCH = 0, BARE_FIT_COVER } BareFit;   /* .image_fit */

typedef enum BareCursor {
    BARE_CURSOR_DEFAULT = 0,
    BARE_CURSOR_HAND,
    BARE_CURSOR_TEXT,
    BARE_CURSOR_MOVE,
    BARE_CURSOR_RESIZE_EW,
    BARE_CURSOR_RESIZE_NS,
    BARE_CURSOR_NOT_ALLOWED,
    BARE_CURSOR_ARROW,    /* the normal arrow, even over a parent or style that set another */
    BARE_CURSOR_CROSSHAIR,
    BARE_CURSOR_RESIZE_NWSE,   /* corners: top-left to bottom-right */
    BARE_CURSOR_RESIZE_NESW
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

/* Built in: the default font is Inter (regular, semibold, bold, italic, bold
 * italic); BARE_MONO is JetBrains Mono (regular, bold), for code. */
#define BARE_MONO ((BareFont)1)
#define BARE_SANS ((BareFont)0xFFFFFFFFu)   /* the default sans (Inter) where 0 means another default (bare_editor) */

/* .weight: CSS font weights (100..900); the nearest face the font's family
 * has is used. BARE_REGULAR counts as set (it overrides a style's bold). */
enum { BARE_REGULAR = 400, BARE_MEDIUM = 500, BARE_SEMIBOLD = 600, BARE_BOLD = 700 };

BareFont  bare_font_load(const char *path);                       /* TTF/OTF; 0 (the default font) on failure */
BareFont  bare_font_from_memory(const void *data, int size);      /* data must outlive the font */
/* Makes `face` the bold / italic / ... of `family`, so text in `family` with
 * that .weight / .italic uses it:
 *     BareFont serif = bare_font_load("Serif.ttf");
 *     bare_font_add_face(serif, bare_font_load("Serif-Bold.ttf"), BARE_BOLD, false); */
void      bare_font_add_face(BareFont family, BareFont face, int weight, bool italic);
BareImage bare_image_load(const char *path);                      /* PNG, JPEG, BMP, GIF, TGA; 0 on failure */
BareImage bare_image_from_pixels(int width, int height, const void *rgba8);  /* pixels are copied */
/* Text into memory, as canvas 2D's fillText (for atlases and canvases): one line
 * of `text` in `font` at `size` pixels as an 8-bit coverage mask, *width x
 * *height bytes, malloc'd (free() it); NULL when nothing is visible. *x, *y:
 * the mask's top-left corner from the pen on the baseline (y down); *advance:
 * how far the pen moves. Pointers may be NULL. Needs the window open. */
BareImage bare_image_from_memory(const void *data, int size);     /* a file's bytes (PNG, JPEG, ...); 0 on failure */
/* A file's bytes to RGBA8 pixels in memory, malloc'd (free() them); NULL on failure. */
unsigned char *bare_image_decode(const void *data, int size, int *width, int *height);
unsigned char *bare_text_pixels(const char *text, BareFont font, float size, int *width, int *height, int *x, int *y,
                                float *advance);
void      bare_image_free(BareImage image);                       /* images and canvases */
BareVec2  bare_image_size(BareImage image);                       /* in pixels; {0, 0} if unknown */
/* New pixels for an image made by bare_image_from_pixels or bare_image_load,
 * same size (video frames, live previews): no new image, no new id. False if
 * the image isn't one of those. */
bool      bare_image_update(BareImage image, const void *rgba8);
/* An image of `frames` pictures of width x height, stacked top to bottom in
 * rgba8, kept on the GPU; it shows one at a time, picked by bare_image_frame
 * (no upload, no copy in memory). bare_image_size is one frame's. */
BareImage bare_image_frames(int width, int height, int frames, const void *rgba8);

/* Pixels as a WebGL texture, for 3D scenes and shaders: .mipmaps gives it a
 * full mip chain, remade after every bare_image_update, sampled trilinearly
 * (generateMipmap with LINEAR_MIPMAP_LINEAR); .srgb stores it as SRGB8_ALPHA8,
 * so shaders read linear values, decoded before filtering (three.js's
 * sRGBEncoding maps); .repeat wraps (REPEAT), .nearest skips filtering.
 *     BareImage wood = bare_texture(.width = w, .height = h, .rgba8 = px,
 *                                   .mipmaps = true, .srgb = true, .repeat = true); */
typedef struct BareTexture {
    int width, height;
    const void *rgba8;   /* width * height * 4 bytes, copied */
    bool mipmaps, srgb, repeat, nearest;
} BareTexture;
BareImage bare_texture_(const BareTexture *texture);
#define bare_texture(...) bare_texture_(&(BareTexture){ __VA_ARGS__ })
void      bare_image_frame(BareImage image, int frame);

/* A picture file, loaded in the background and kept while you draw it: call it
 * every frame, like the rest of the UI. 0 while it loads (a frame comes when
 * it's ready) or if it can't be read; a big file or a video may first show a
 * quick picture (the system's cached thumbnail, its first frame). Moving pictures move: every frame of a
 * GIF, a few seconds of a video (from a tenth of the way in). Reads PNG, JPEG,
 * BMP, TGA, GIF; on Windows also what the system can (WebP, HEIC, TIFF, ...)
 * and video (MP4, MKV, WebM, ... through Media Foundation). One not drawn for
 * 10 s is freed; one that
 * couldn't be read is tried again every 5 s while you ask for it.
 *     bare_box(.width = 240, .height = 135, .radius = 12,
 *              .image = bare_picture(path, .width = 480, .height = 270));
 * .width / .height: the pixels to keep, cropped to that shape from the middle
 * (one of them 0: the picture's shape; both 0: its own size). */
typedef struct BarePictureOptions {
    int width, height;
    bool still;          /* only the first frame (a video's from a tenth of the way in) */
    int frames;          /* at most this many frames of a moving picture (0: 40) */
} BarePictureOptions;
BareImage bare_picture_(const char *path, BarePictureOptions options);
#define bare_picture(path, ...) bare_picture_((path), (BarePictureOptions){ __VA_ARGS__ })
/* True once bare_picture with the same path and options found nothing it could read. */
bool      bare_picture_failed(const char *path, BarePictureOptions options);

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
 * documents the conventions they follow.
 *
 * A draw (bare_draw) is WebGL's drawArrays / drawArraysInstanced: triangles
 * from your own vertex data, run through a program (a .glsl file with a
 * vertex half too), into a canvas. */

typedef enum BareFormat {
    BARE_RGBA8 = 0,       /* default: 8-bit colour */
    BARE_RGBA16F,         /* half floats: HDR colour, simulation state */
    BARE_RG16F,           /* two half floats (e.g. velocity) */
    BARE_R16F,            /* one half float (e.g. pressure) */
    BARE_RGBA32F,         /* full floats, for data that must stay exact (half floats where unsupported) */
    BARE_DEPTH            /* a depth buffer for bare_draw's .depth (32-bit float where supported); as an
                             input it reads as depth in .r, 0 (near) to 1 (far) */
} BareFormat;

typedef struct BareCanvas {
    int width, height;    /* pixels (not scaled by DPI) */
    BareFormat format;
    bool nearest;         /* sample without filtering (default: bilinear) */
    bool repeat;          /* sampling outside 0..1 wraps (default: clamps to the edge) */
    bool bottom_up;       /* rows run bottom to top, as in WebGL (gl_FragCoord.y = 0 at the bottom):
                             .image shows it the right way up, so WebGL passes port unchanged */
    bool mipmaps;         /* keep a mip chain, rebuilt after every pass or draw into it, and sample it trilinear
                             with anisotropic filtering: fine detail stays crisp up close and calm far away or
                             at grazing angles (WebGL's generateMipmap + EXT_texture_filter_anisotropic).
                             Not for BARE_DEPTH; with .nearest only the full-size level is read */
} BareCanvas;

#define bare_canvas(...)  bare_canvas_(&(BareCanvas){ __VA_ARGS__ })

/* Compiled shader code; bare_add_shaders() generates one per .glsl file. */
typedef struct BareShaderCode {
    const unsigned char *spirv, *dxil, *msl;
    unsigned spirv_size, dxil_size, msl_size;
    int samplers;         /* inputs it reads */
    int uniform_buffers;  /* 0 or 1: whether it takes params */
    int no_uv;            /* 1: never reads v_uv (only gl_FragCoord) */
    const unsigned char *dxbc;  /* optional, preferred on D3D12: FXC's code, as browsers make for WebGL */
    unsigned dxbc_size;
    /* A program for bare_draw: the vertex half, as written ([0]) and with
     * gl_Position.y negated ([1], for bottom_up canvases). NULL for a pass shader. */
    const unsigned char *vert_spirv[2], *vert_dxil[2], *vert_msl[2], *vert_dxbc[2];
    unsigned vert_spirv_size[2], vert_dxil_size[2], vert_msl_size[2], vert_dxbc_size[2];
    int vert_inputs;            /* vertex attributes it reads (each one vec4 slot) */
    int vert_uniform_buffers;   /* 0 or 1: whether its vertex half reads the params */
    int vert_samplers;          /* inputs its vertex half reads (set 0, binding = the input's index) */
} BareShaderCode;

typedef uint32_t BareShader;   /* 0 = none */

BareShader bare_shader(const BareShaderCode *code);   /* code must outlive the shader (generated code is static) */
void       bare_shader_free(BareShader shader);

typedef enum BareBlend {
    BARE_BLEND_NONE = 0,  /* default: the shader's output replaces the target's pixels */
    BARE_BLEND_ADD,       /* added to them */
    BARE_BLEND_OVER,      /* premultiplied alpha over them */
    BARE_BLEND_ALPHA      /* straight alpha over them: colour as WebGL's blendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA),
                             alpha as a + a_dst (1 - a), so an opaque canvas stays opaque */
} BareBlend;

#define BARE_MAX_INPUTS 16   /* samplers per shader stage, as WebGL2 guarantees */
#define BARE_MAX_TARGETS 4

typedef struct BarePass {
    BareImage target;                   /* a canvas; not one of the inputs */
    BareShader shader;
    BareImage inputs[BARE_MAX_INPUTS];  /* bound to the shader's samplers 0, 1, ... */
    const void *params;                 /* the shader's uniform block (std140), copied; see BARE_PARAMS */
    int params_size;                    /* bytes, up to 4096 */
    BareBlend blend;
    /* More canvases the same size as .target, written at the same time by the
     * shader's outputs 1, 2, 3 (layout(location = 1) out ...), as WebGL's
     * drawBuffers: one pass, several results. */
    BareImage extra_targets[BARE_MAX_TARGETS - 1];
} BarePass;

#define bare_pass(...)    bare_pass_(&(BarePass){ __VA_ARGS__ }, __FILE__, __LINE__)
#define BARE_PARAMS(var)  .params = &(var), .params_size = (int)sizeof(var)

/* A draw: triangles from your vertex data into a canvas, as WebGL's
 * drawArrays(TRIANGLES or TRIANGLE_STRIP) and drawArraysInstanced.
 *
 *   typedef struct { float pos[4], color[4]; } Vert;       // each attribute a vec4 slot
 *   Vert tri[3] = { ... };
 *   bare_draw(.target = sky, .shader = shapes, .clear = true,
 *             BARE_VERTICES(tri, 3), BARE_PARAMS(p));
 *
 * Attributes are 16 bytes each (4 floats; declare them float, vec2, vec3 or
 * vec4): a vertex's at locations 0, 1, ..., then an instance's after them.
 * With no .vertices the program builds its vertices from gl_VertexIndex
 * (and gl_InstanceIndex). Clip space is WebGL's: in a bottom_up canvas, y = 1
 * is the top of the picture, as in a WebGL framebuffer. Vertex and instance
 * data are copied, up to 64 MB a draw.
 *
 * Depth (WebGL's DEPTH_TEST): give a depth canvas the target's size, and what's
 * behind what's already drawn stays hidden, whatever the order:
 *
 *   BareImage zbuf = bare_canvas(.width = w, .height = h, .format = BARE_DEPTH);
 *   bare_draw(.target = scene, .depth = zbuf, .clear = true, .clear_depth = true, ...);  // the ground
 *   bare_draw(.target = scene, .depth = zbuf, .depth_read_only = true, ...);           // soft particles
 *
 * Depth is gl_Position.z / w from 0 (near) to 1 (far), as Vulkan, Direct3D and
 * Metal have it (WebGL's -1..1 maps there with z = z * 0.5 + w * 0.5). */
typedef enum BareDepthTest {
    BARE_DEPTH_LESS = 0,     /* default: drawn where nearer than what's there */
    BARE_DEPTH_LESS_EQUAL,
    BARE_DEPTH_GREATER,      /* reversed depth (clear to BARE_ZERO) */
    BARE_DEPTH_GREATER_EQUAL,
    BARE_DEPTH_EQUAL,
    BARE_DEPTH_ALWAYS        /* never hidden; still written unless .depth_read_only */
} BareDepthTest;

typedef struct BareDraw {
    BareImage target;                   /* a canvas; not one of the inputs */
    BareShader shader;                  /* a program: its .glsl has a BARE_VERTEX half */
    BareImage inputs[BARE_MAX_INPUTS];  /* samplers 0, 1, ... of either half (vertex: set 0, fragment: set 2) */
    const void *params;                 /* the uniform block both halves may read (std140), copied */
    int params_size;
    BareBlend blend;
    BareImage extra_targets[BARE_MAX_TARGETS - 1];
    const void *vertices;               /* vertex_count * vertex_size bytes, or NULL */
    int vertex_size, vertex_count;      /* bytes a vertex (a multiple of 16), vertices to draw */
    const void *instances;              /* instance_count * instance_size bytes, or NULL */
    int instance_size, instance_count;  /* 0 instances: drawn once */
    bool strip;                         /* a triangle strip (default: a list, three vertices a triangle) */
    bool clear;                         /* clear the targets to .clear_color first */
    float clear_color[4];               /* straight RGBA, 0..1 (HDR canvases: any value) */
    BareImage depth;                    /* a BARE_DEPTH canvas the target's size: depth-tested (0 = none) */
    BareDepthTest depth_test;
    bool depth_read_only;               /* test against it, don't write it (blended things over a scene) */
    bool clear_depth;                   /* clear it to .depth_clear first (WebGL's DEPTH_BUFFER_BIT) */
    float depth_clear;                  /* 0 = 1 (far); BARE_ZERO for 0 (reversed depth) */
} BareDraw;

#define bare_draw(...)    bare_draw_(&(BareDraw){ __VA_ARGS__ }, __FILE__, __LINE__)
#define BARE_VERTICES(array, count)  .vertices = (array), .vertex_size = (int)sizeof((array)[0]), .vertex_count = (count)
#define BARE_INSTANCES(array, count) .instances = (array), .instance_size = (int)sizeof((array)[0]), .instance_count = (count)

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
    BarePosition position;      /* BARE_ABSOLUTE: placed on the parent by .anchor, .x, .y, out of the flow */
    BareAnchor anchor;
    float x, y;

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
    BareImage image;            /* drawn over .color, rounded by .radius; stretched to the box unless .image_fit */
    BareFit image_fit;          /* BARE_FIT_COVER: fill the box keeping the picture's shape, cropping the overflow */

    /* interaction */
    BareLook hover, press;      /* looks while hovered / pressed */
    BareLook focus;             /* look while it has keyboard focus (a focus ring: .focus = {.border_color = ...}) */
    BareCursor cursor;
    bool focusable;             /* takes keyboard focus: a click or Tab / Shift+Tab moves it here (see bare_focus) */
    bool passthrough;           /* the mouse goes through it (CSS pointer-events: none); children still get it */
    bool window_drag;           /* frameless windows: dragging here moves the window, double-click maximises
                                   (CSS app-region: drag). Children that react to the mouse (a hover or press
                                   look, a cursor, focusable) stay clickable */
    bool clip;                  /* clip children to the box, inside its border (CSS overflow: hidden) */
    BareScroll scroll;          /* scroll overflowing children (see BareScroll); implies .clip */

    /* animation */
    BareAnim animate;           /* how every animatable property moves to its new value (default: instantly) */
    BareAnim animate_layout;    /* overrides .animate for position and size */
} BareBox;

typedef enum BareTransform { BARE_TRANSFORM_NONE = 0, BARE_UPPERCASE, BARE_LOWERCASE } BareTransform;

typedef struct BareTextStyle {
    BareId id;
    const struct BareTextStyle *style;   /* base style: fields left at 0 come from it */
    BareFont font;              /* 0 = default (Inter); BARE_MONO; or one you loaded */
    float size;                 /* 0 = default 16 */
    int weight;                 /* BARE_BOLD, BARE_SEMIBOLD, ... (0 = the font's own) */
    bool italic;
    BareColor color;            /* alpha 0 = inherited from the nearest .text_color (default white) */
    BareColor background;       /* behind the glyphs, rounded (a highlight, inline code); alpha 0 = none */
    float line_height;          /* multiple of size; 0 = default 1.3 */
    BareAlign align;            /* BARE_START / CENTER / END within the line box */
    bool nowrap;                /* default: wraps at spaces and after hyphens to fit its width */
    int max_lines;              /* cut after this many lines (0 = no limit) */
    bool ellipsis;              /* end cut text with "…" (with .max_lines, or .nowrap text narrower than itself) */
    bool underline, strike;     /* lines under / through the text, in its colour */
    float letter_spacing;       /* extra pixels after every character (may be negative) */
    BareTransform transform;    /* BARE_UPPERCASE / BARE_LOWERCASE (Latin) */
    BareShadow shadow;          /* drawn under the glyphs (spread is ignored) */
    int tab_size;               /* tab stops every this many spaces (0 = default 4) */
    float opacity;              /* 0 = default 1 */
    BarePosition position;      /* BARE_ABSOLUTE: see BareBox */
    BareAnchor anchor;
    float x, y;
    BareAnim animate;           /* how its colour changes (default: instantly; inherited colour follows the parent) */
} BareTextStyle;

/* What happened to an element. Results reflect input up to the start of this
 * frame against what was on screen last frame, so they are ready right away. */
typedef struct BareState {
    bool hovered;               /* the mouse is over it or one of its children */
    bool pressed;               /* a press started on it and the button is still down */
    bool clicked;               /* pressed and released over it (this frame); widgets: or Space / Enter while focused */
    bool double_clicked;        /* the click was the second of a double click (clicked is true too) */
    bool right_clicked;
    bool focused;               /* it has keyboard focus */
    bool changed;               /* bare_input: the text was edited this frame */
    bool submitted;             /* bare_input: Enter in a single-line field (this frame) */
    BareRect rect;              /* where it was last frame, logical pixels */
    BareVec2 scroll;            /* scroll boxes: how far the content is scrolled this frame */
} BareState;

/* Leaf box. Returns its state. */
#define bare_box(...)  bare_box_(&(BareBox){ __VA_ARGS__ }, __FILE__, __LINE__)

/* Containers: use as `bare_row(.gap = 8) { ... }`. */
#define bare_row(...)     BARE__CONTAINER(.dir = BARE_ROW, __VA_ARGS__)
#define bare_column(...)  BARE__CONTAINER(.dir = BARE_COLUMN, __VA_ARGS__)
#define bare_stack(...)   BARE__CONTAINER(.dir = BARE_STACK, __VA_ARGS__)    /* layers; .align places them */

/* Glass surfaces use the normal box API. A preset supplies only the look;
 * dimensions, position, children and interaction remain the app's choice.
 * Draw backdrop content first inside bare_glass_stack, then glass surfaces.
 * Adjacent surfaces with .glass.merge join as in ordinary Bare boxes. */
typedef enum BareGlassPreset {
    BARE_GLASS_CLEAR, BARE_GLASS_FROST, BARE_GLASS_LENS,
    BARE_GLASS_DOCK, BARE_GLASS_DROP
} BareGlassPreset;
BareState bare_glass_surface_(BareGlassPreset preset, const BareBox *box, const char *file, int line);
int bare_glass_layer_(BareGlassPreset preset, const BareBox *box, const char *file, int line);
#define bare_glass_stack(...) bare_stack(__VA_ARGS__)
#define bare_glass_surface(preset, ...) bare_glass_surface_((preset), &(BareBox){ __VA_ARGS__ }, __FILE__, __LINE__)
#define bare_glass_layer(preset, ...) \
    for (int v = bare_glass_layer_((preset), &(BareBox){ __VA_ARGS__ }, __FILE__, __LINE__); v; v = bare_close_())

/* State of the innermost open container (call inside its block). */
BareState bare_this(void);

/* Text: a leaf element as wide as its longest line, but no wider than its
 * parent allows (then it wraps; a row shrinks it down to its longest word).
 * The default font is Inter (Latin); pass .font = bare_font_load(...) for others. */
#define bare_text(str, ...)  bare_text_((str), &(BareTextStyle){ __VA_ARGS__ }, __FILE__, __LINE__)
#define bare_textf(...)      bare_textf_(__FILE__, __LINE__, __VA_ARGS__)   /* printf-style, default style */

/* Rich text: spans with their own font, size, weight, colour, background,
 * underline ... flowing and wrapping together as one text element. The
 * paragraph's options (line height, alignment, wrapping, max lines, shadow,
 * position) are for the whole; each span's override the paragraph's.
 *
 *     bare_paragraph(.size = 18) {
 *         bare_span("Hello ", .color = t->muted);
 *         bare_span("world", .weight = BARE_BOLD);
 *         bare_span("!", .italic = true);
 *     }
 *
 * Only spans go inside the block. */
#define bare_paragraph(...)   BARE__PARAGRAPH(BARE__CAT(bare__para_, BARE__UNIQUE), __VA_ARGS__)
#define bare_span(...)        bare_span_(BARE__ARG1(__VA_ARGS__), &(BareTextStyle){ BARE__AFTER1(__VA_ARGS__) .id = 0 })

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
/* Keyboard focus and text fields                                           */
/* ------------------------------------------------------------------------ */

/* A text field editing your buffer in place (`cap` bytes, the NUL included).
 * Everything a native field does is built in: typing (any layout, dead keys,
 * IME), the caret and selection by keyboard and mouse (double-click a word,
 * triple-click a line), every standard shortcut (word jumps, Home / End,
 * select all, cut / copy / paste, undo / redo, ...: see BareEditAction),
 * scrolling to the caret and a blinking caret that costs nothing when idle.
 *
 *     static char name[64];
 *     BareState s = bare_input(name, sizeof name, .placeholder = "Your name", .width = 280);
 *     if (s.submitted) save(name);      // Enter
 *
 * It looks like the theme's .input style; pass .style for your own (its
 * .focus look is the focus ring). The field's id defaults to its call site;
 * give fields in loops BARE_IDI. Your buffer stays the truth: change it any
 * time and the field follows (the caret goes to the end, undo starts over). */
typedef struct BareInput {
    BareId id;
    const BareBox *style;           /* the box (default: the theme's .input) */
    const BareTextStyle *text_style;/* its text (default: the theme's .body) */
    float width, height;            /* 0 = the style's; multi-line height defaults to .lines lines */
    int lines;                      /* multiline: visible lines when .height isn't set (0 = default 4) */
    const char *placeholder;        /* shown while empty */
    bool multiline;                 /* Enter adds a line (Shift+Enter always does); wraps and scrolls */
    bool password;                  /* shows dots; can't be cut or copied */
    bool tabs;                      /* multiline: Tab types a tab instead of moving focus */
    bool read_only;                 /* can be selected and copied, not changed */
    float size;                     /* text size (0 = the text style's) */
    BareFont font;
    BareColor color;                /* text colour (alpha 0 = the style's) */
    BareColor caret, selection;     /* alpha 0 = the theme's */
} BareInput;

#define bare_input(buf, cap, ...)  bare_input_((buf), (int)(cap), &(BareInput){ __VA_ARGS__ }, __FILE__, __LINE__)

/* Editing commands. Each is bound to the platform's usual keys (below; "Mod"
 * is Ctrl, Cmd on macOS) and can also be run directly, e.g. from a menu:
 * bare_edit(0, BARE_EDIT_UNDO) acts on the focused field. Movements extend
 * the selection when Shift is held. */
typedef enum BareEditAction {
    BARE_EDIT_NONE = 0,
    BARE_EDIT_LEFT, BARE_EDIT_RIGHT,                /* ← →: a character; a selection collapses to its edge */
    BARE_EDIT_WORD_LEFT, BARE_EDIT_WORD_RIGHT,      /* Ctrl+← → (macOS Option): a word */
    BARE_EDIT_LINE_START, BARE_EDIT_LINE_END,       /* Home / End (macOS Cmd+← →); Home is smart: first non-space, then column 0 */
    BARE_EDIT_UP, BARE_EDIT_DOWN,                   /* ↑ ↓: a line, keeping the column (single-line: start / end) */
    BARE_EDIT_PAGE_UP, BARE_EDIT_PAGE_DOWN,         /* a page of lines */
    BARE_EDIT_TEXT_START, BARE_EDIT_TEXT_END,       /* Ctrl+Home / End (macOS Cmd+↑ ↓) */
    BARE_EDIT_SELECT_ALL,                           /* Mod+A */
    BARE_EDIT_BACKSPACE, BARE_EDIT_DELETE,          /* the character before / after the caret, or the selection */
    BARE_EDIT_DELETE_WORD_LEFT, BARE_EDIT_DELETE_WORD_RIGHT,   /* Ctrl+Backspace / Delete (macOS Option) */
    BARE_EDIT_DELETE_LINE_LEFT,                     /* macOS Cmd+Backspace: to the line start */
    BARE_EDIT_CUT, BARE_EDIT_COPY, BARE_EDIT_PASTE, /* Mod+X / C / V; also Shift+Delete, Ctrl+Insert, Shift+Insert */
    BARE_EDIT_UNDO, BARE_EDIT_REDO,                 /* Mod+Z; Ctrl+Y, Mod+Shift+Z */
    BARE_EDIT_ENTER,                                /* Enter: submit (single-line) or a new line */
    BARE_EDIT_NEWLINE,                              /* Shift+Enter: a new line (multiline) */
    BARE_EDIT_TAB,                                  /* types a tab (multiline .tabs only; else Tab moves focus) */
    BARE_EDIT_BLUR,                                 /* Escape: leaves the field */
    BARE_EDIT_OVERWRITE,                            /* Insert: typing replaces instead of inserting */
    /* bare_editor only (see BareEditor for the keys) */
    BARE_EDIT_FIND, BARE_EDIT_REPLACE, BARE_EDIT_FIND_NEXT, BARE_EDIT_FIND_PREV, BARE_EDIT_GOTO_LINE,
    BARE_EDIT_SELECT_NEXT_MATCH, BARE_EDIT_SELECT_ALL_MATCHES,
    BARE_EDIT_ADD_CARET_UP, BARE_EDIT_ADD_CARET_DOWN,
    BARE_EDIT_MOVE_LINE_UP, BARE_EDIT_MOVE_LINE_DOWN, BARE_EDIT_COPY_LINE_UP, BARE_EDIT_COPY_LINE_DOWN,
    BARE_EDIT_DELETE_LINE, BARE_EDIT_LINE_BELOW, BARE_EDIT_LINE_ABOVE, BARE_EDIT_SELECT_LINE,
    BARE_EDIT_INDENT, BARE_EDIT_OUTDENT, BARE_EDIT_COMMENT,
    BARE_EDIT_ACTION_COUNT_
} BareEditAction;

/* Modifier bits for bare_edit_bind. BARE_MOD_SHORTCUT is Ctrl, or Cmd on macOS. */
enum { BARE_MOD_SHIFT = 1, BARE_MOD_CTRL = 2, BARE_MOD_ALT = 4, BARE_MOD_SUPER = 8, BARE_MOD_SHORTCUT = 16 };

void   bare_edit(BareId field, BareEditAction action);   /* field 0 = the focused one; runs when it's next declared */
/* Binds key + mods (exactly) to an action in every field, replacing what it
 * did; BARE_EDIT_NONE unbinds it. Movements bound this way also select with Shift. */
void   bare_edit_bind(int key, int mods, BareEditAction action);

/* Keyboard focus: clicking a .focusable element (or a field) focuses it,
 * clicking anything else or Escape blurs it, Tab / Shift+Tab walk the
 * focusable elements in the order they're declared. While a field has focus
 * the keys it uses don't reach bare_key_pressed / bare_key_down. */
void   bare_focus(BareId id);                  /* 0 blurs */
BareId bare_focused(void);                     /* 0 = nothing */

/* ------------------------------------------------------------------------ */
/* Documents and the code / text editor                                     */
/* ------------------------------------------------------------------------ */

/* A document: text Bare owns, made for editors (bare_editor). Stored as lines
 * (a loaded file isn't copied until a line changes), so a file of a million
 * lines opens at once; keeps its line endings (LF or CRLF) and a UTF-8 BOM;
 * invalid UTF-8 is shown as U+FFFD but saved back unchanged. It holds the
 * editor's selections (any number: multiple carets) and the undo history, so
 * an app can edit it like the user does. Use one editor per document. */
typedef struct BareDoc BareDoc;
typedef struct BareTextPos { int line, col; } BareTextPos;   /* col: byte offset in the line */

BareDoc    *bare_doc_create(void);
BareDoc    *bare_doc_load(const char *path);               /* NULL if it can't be read */
bool        bare_doc_save(BareDoc *doc, const char *path); /* in its line endings; true on success */
void        bare_doc_free(BareDoc *doc);
void        bare_doc_set_text(BareDoc *doc, const char *text, int len);   /* len < 0: up to the NUL; clears undo */
const char *bare_doc_text(BareDoc *doc, int *len);         /* all of it, '\n' breaks; valid until it changes */
int         bare_doc_lines(const BareDoc *doc);
const char *bare_doc_line(const BareDoc *doc, int line, int *len);   /* no line break, not NUL-terminated */
bool        bare_doc_modified(const BareDoc *doc);         /* differs from what was loaded or saved (undo-aware) */
uint64_t    bare_doc_version(const BareDoc *doc);          /* goes up with every change */
bool        bare_doc_crlf(const BareDoc *doc);             /* saves with \r\n (as it was loaded) */
void        bare_doc_set_crlf(BareDoc *doc, bool crlf);

/* Editing as the user would: each call is one undo step over every selection. */
void        bare_doc_insert(BareDoc *doc, const char *text);    /* replaces each selection */
void        bare_doc_wrap(BareDoc *doc, const char *before, const char *after);   /* around each selection */
void        bare_doc_toggle_prefix(BareDoc *doc, const char *prefix);   /* on the selected lines ("# ", "// ") */
void        bare_doc_select(BareDoc *doc, BareTextPos anchor, BareTextPos caret);   /* one selection */
BareTextPos bare_doc_caret(const BareDoc *doc);             /* the main caret */
int         bare_doc_selections(const BareDoc *doc);        /* how many carets */
const char *bare_doc_selected(BareDoc *doc, int *len);      /* the main selection's text; valid until it changes */
bool        bare_doc_undo(BareDoc *doc);
bool        bare_doc_redo(BareDoc *doc);

enum { BARE_FIND_CASE = 1, BARE_FIND_WORD = 2 };            /* match case, whole words */
/* The next match of `query` (one line, no breaks) at or after `from`. */
bool        bare_doc_find(const BareDoc *doc, const char *query, int flags, BareTextPos from,
                          BareTextPos *start, BareTextPos *end);

/* A highlighter colours one line at a time: it reads the line and the state
 * the previous line ended in (*state, 0 at the top; e.g. "inside a block
 * comment"), writes spans, sorted and not overlapping (gaps are plain), sets
 * *state for the next line and returns how many spans it wrote (at most
 * `cap`). Bare keeps each line's end state and re-runs it only from an edit
 * down, and only as far as is shown. */
typedef struct BareHighlight {
    int start, end;             /* bytes in the line */
    BareColor color;            /* alpha 0 = the editor's text colour */
    BareColor background;
    int weight;                 /* BARE_BOLD ... */
    bool italic, underline, strike;
} BareHighlight;
typedef int (*BareHighlighter)(void *user, const char *line, int len, int *state, BareHighlight *out, int cap);

/* What Enter puts at the start of the new line (the default: the current
 * line's indentation). Writes at most `cap` bytes, returns how many. */
typedef int (*BareIndenter)(void *user, const char *line, int len, int col, char *out, int cap);

/* The editor: what a code editor or a notes app needs, built in. Only the
 * lines on screen are laid out and drawn, so huge files scroll at full rate.
 *   - every bare_input shortcut, plus: Ctrl+D selects the next match, Ctrl+
 *     Shift+L all of them, Alt+click adds a caret, Ctrl+Alt+Up / Down add one
 *     above / below, Alt+drag selects a column; edits go to every caret
 *   - Alt+Up / Down move lines, Shift+Alt+Up / Down copy them, Ctrl+Shift+K
 *     deletes a line, Ctrl+Enter / Ctrl+Shift+Enter open a line below / above,
 *     Ctrl+L selects a line, Tab / Shift+Tab indent and outdent, Ctrl+/
 *     toggles .comment
 *   - Ctrl+F find (match case, whole word), Ctrl+H replace, F3 / Shift+F3,
 *     Enter / Shift+Enter in the find bar, Ctrl+G go to line; matches shown
 *   - auto-indent, .auto_close pairs, bracket matching, line numbers, soft
 *     wrap, visible whitespace, a ruler, the current line highlighted
 * (macOS: Cmd for Ctrl.)
 *
 *     static BareDoc *doc;
 *     if (!doc) doc = bare_doc_load("notes.txt");
 *     bare_editor(doc, .line_numbers = true, .wrap = true);
 */
typedef struct BareEditor {
    BareId id;                      /* default: from the call site */
    const BareBox *style;           /* the box (default: the theme's .input look) */
    float width, height;            /* default: BARE_GROW both */
    BareFont font;                  /* default: BARE_MONO */
    float size;                     /* text size (default 14) */
    float line_height;              /* multiple of size (default 1.5) */
    int tab_size;                   /* columns (default 4) */
    bool spaces;                    /* Tab inserts spaces */
    bool line_numbers, wrap, highlight_line, whitespace, read_only;
    float ruler;                    /* a line at this column (0 = none) */
    const char *comment;            /* Ctrl+/ toggles this line prefix, e.g. "// " */
    const char *auto_close;         /* pairs typed together, e.g. "()[]{}\"\"" */
    BareHighlighter highlight;  void *highlight_user;
    BareIndenter indent;        void *indent_user;
    BareColor color;                /* text (alpha 0 = the theme's) */
    BareColor caret, selection;     /* alpha 0 = the theme's */
    int id_;                        /* internal: keeps the macro's braces non-empty */
} BareEditor;

/* (doc, ...) */
#define bare_editor(...) \
    bare_editor_(BARE__ARG1(__VA_ARGS__), &(BareEditor){ BARE__AFTER1(__VA_ARGS__) .id_ = 0 }, __FILE__, __LINE__)

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
    BareColor selection;        /* selected text's background */
    BareBox panel;              /* a large flat surface */
    BareBox card;               /* a raised surface with a shadow */
    BareBox button;             /* a neutral button: hover and press looks, hand cursor */
    BareBox primary;            /* the accent button */
    BareBox row;                /* a list row that lights up under the mouse */
    BareBox input;              /* text fields (bare_input); its .focus look is the focus ring */
    BareTextStyle title, heading, body, caption;
} BareTheme;

const BareTheme *bare_theme(void);              /* the current theme (dark until you set one) */
void bare_set_theme(const BareTheme *theme);    /* copied: tweak a copy of a built-in for your own */
const BareTheme *bare_theme_dark(void);
const BareTheme *bare_theme_light(void);

/* ------------------------------------------------------------------------ */
/* Widgets: one call each, built only from the elements above              */
/* ------------------------------------------------------------------------ */

/* Each takes a BareBox of options: .style replaces the theme's look, fields
 * set on the call override it. All are focusable (Tab), and pressed by Space
 * (buttons and list items by Enter too).
 *
 *     if (bare_button("Save", .style = &bare_theme()->primary).clicked) save();
 *     bare_checkbox("Wrap lines", &wrap);                // true when toggled
 *     bare_slider(&volume, 0, 100, .width = 240);        // true when moved; drag, or arrows
 *     for (int i = 0; i < n; i++)
 *         if (bare_list_item(names[i], i == sel, .id = BARE_IDI("item", i)).clicked) sel = i;
 */
/* (label, ...) */
#define bare_button(...)     bare_button_(BARE__ARG1(__VA_ARGS__), &(BareBox){ BARE__AFTER1(__VA_ARGS__) .dir = 0 }, __FILE__, __LINE__)
/* (label, bool *value, ...) */
#define bare_checkbox(...)     bare_checkbox_(BARE__ARG1(__VA_ARGS__), BARE__ARG2(__VA_ARGS__), &(BareBox){ BARE__AFTER2(__VA_ARGS__) .dir = 0 },                    __FILE__, __LINE__)
/* (float *value, lo, hi, ...) */
#define bare_slider(...)     bare_slider_(BARE__ARG1(__VA_ARGS__), BARE__ARG2(__VA_ARGS__), BARE__ARG3(__VA_ARGS__),                  &(BareBox){ BARE__AFTER3(__VA_ARGS__) .dir = 0 }, __FILE__, __LINE__)
/* Lists: a bare_column(.scroll = BARE_SCROLL_Y) of items with BARE_IDI ids
 * (bare_visible for long ones); bare_list_item gives the hover and selected
 * looks and fills the list's width. */
/* (label, bool selected, ...) */
#define bare_list_item(...)     bare_list_item_(BARE__ARG1(__VA_ARGS__), BARE__ARG2(__VA_ARGS__), &(BareBox){ BARE__AFTER2(__VA_ARGS__) .dir = 0 },                     __FILE__, __LINE__)

/* ------------------------------------------------------------------------ */
/* Window, frame loop, input                                                */
/* ------------------------------------------------------------------------ */

/* Opens the window (logical pixels). One window is current; no handle needed. */
bool  bare_window(int width, int height, const char *title);
/* Request an alpha-composited window. Use a transparent bare_background() to
 * let the desktop show through; compositor support varies by platform. */
bool  bare_window_transparent(int width, int height, const char *title);

/* A window with every option. bare_window(w, h, title) is
 * bare_window_with(.width = w, .height = h, .title = title).
 *
 *     bare_window_with(.width = 1200, .height = 800, .title = "Notes", .frameless = true,
 *                      .min_width = 640, .min_height = 420);
 *
 * Frameless windows have no system title bar or border: draw your own, mark
 * it .window_drag, and put buttons on it that call bare_window_minimize /
 * maximize / close. The edges still resize, and on Windows 11 the corners
 * stay round with the system's shadow. */
typedef struct BareWindowOptions {
    int width, height;          /* logical pixels (0 = 1280 x 800) */
    const char *title;
    bool transparent;           /* alpha-composited (see bare_window_transparent) */
    bool frameless;
    int min_width, min_height;  /* logical pixels (0 = no limit) */
    float zoom;                 /* everything this much bigger (0 = 1): 0.8 is the same layout in a window 80% the size */
    /* Another program's window to live inside (Windows: an HWND), filling it
     * and following its size; no title bar, never in the taskbar. A live
     * wallpaper host passes one (Screen+: --screenplus-parent). 0 = none. */
    uintptr_t parent;
} BareWindowOptions;
#define bare_window_with(...) bare_window_with_(&(BareWindowOptions){ __VA_ARGS__ })
bool  bare_window_with_(const BareWindowOptions *options);
void  bare_window_minimize(void);
void  bare_window_maximize(void);       /* toggles between maximised and restored */
bool  bare_window_maximized(void);
void  bare_window_close(void);          /* as the close button: bare_frame returns false */

/* The window as a pane of glass: the live desktop behind it, the size of the
 * window (physical pixels), new pictures arriving by themselves. Draw it
 * first, behind everything, and .glass bends what's really behind the
 * window, not a blur of it:
 *
 *     bare_stack(.width = BARE_GROW, .height = BARE_GROW, .image = bare_window_backdrop()) {
 *         bare_box(.width = 400, .height = 240, .radius = 40, .glass = BARE_GLASS);
 *     }
 *
 * 0 until the first picture arrives (and where unsupported: Windows 10 2004
 * and later only, so far), so keep a background colour for that. The window
 * is left out of screenshots and screen sharing while this runs (that is how
 * it can see behind itself). */
BareImage bare_window_backdrop(void);

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

/* The system's file dialogs. They don't block: the app keeps drawing, and
 * the answer arrives in a later frame, when bare_file_chosen(id) returns the
 * path (for that one frame; "" if the user cancelled, NULL otherwise).
 * filters: "Markdown:md;markdown|Text:txt" (NULL: any file); path: where it
 * opens, or the suggested name to save as.
 *
 *     if (bare_button("Open").clicked) bare_file_dialog(BARE_ID("open"), BARE_FILE_OPEN, "Text:txt", NULL);
 *     const char *p = bare_file_chosen(BARE_ID("open"));
 *     if (p && *p) load(p);
 */
typedef enum BareFileDialog { BARE_FILE_OPEN, BARE_FILE_SAVE, BARE_FILE_FOLDER } BareFileDialog;
void        bare_file_dialog(BareId id, BareFileDialog kind, const char *filters, const char *path);
const char *bare_file_chosen(BareId id);
/* A file dragged from the desktop and dropped on the window: its path, for
 * the frame after the drop (NULL otherwise). Several at once arrive a frame
 * apart. */
const char *bare_file_dropped(void);
float bare_pixel_scale(void);          /* physical pixels per logical pixel (devicePixelRatio) */
void  bare_continuous(bool on);         /* redraw every frame even when idle (games, video) */
void  bare_wake(void);                  /* thread-safe: ask for a frame (e.g. data arrived) */
void  bare_wake_in(float seconds);      /* a frame this many seconds from now (a timed fade); idle until then */

double bare_time(void);                 /* seconds since bare_window */
float  bare_dt(void);                   /* seconds since the previous frame */

BareVec2 bare_mouse(void);              /* logical pixels, window coordinates */
/* How far the wheel turned this frame while the mouse was over the window, as
 * the DOM's wheel deltaX / deltaY: logical pixels, a notch is 100, + = right /
 * down (toward the end of a page). Scroll boxes still scroll; this is for apps
 * that use the wheel themselves (zoom, a 3D camera). */
BareVec2 bare_wheel(void);

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
    BARE_KEY_SEMICOLON, BARE_KEY_APOSTROPHE, BARE_KEY_BACKSLASH, BARE_KEY_LEFT_BRACKET, BARE_KEY_RIGHT_BRACKET,
    BARE_KEY_GRAVE,
    BARE_KEY_COUNT_,
    BARE_KEY_SHORTCUT = 1000    /* bare_key_down only: Ctrl, or Cmd on macOS */
} BareKey;

bool bare_key_down(BareKey key);        /* held now */
bool bare_key_pressed(BareKey key);     /* went down this frame, or auto-repeated while held (like keydown) */

const char *bare_clipboard(void);       /* valid until the next frame */
void        bare_set_clipboard(const char *text);

/* Short sounds held in memory (clicks, cues), played on the default output.
 * Samples are 32-bit floats, interleaved when stereo; they're copied. Several
 * can play at once. The device opens on the first play and closes again when
 * nothing has played for a while.
 *     BareSound tick = bare_sound(samples, frames, 1, 48000);
 *     if (bare_button("OK").clicked) bare_sound_play(tick, 0.5f); */
typedef uint32_t BareSound;   /* 0 = none */
BareSound bare_sound(const float *samples, int frames, int channels, int rate);
void      bare_sound_play(BareSound sound, float volume);   /* volume 0..1 */
void      bare_sound_free(BareSound sound);

/* Music and soundtracks: a whole file's sound, decoded into memory (about 23 MB
 * a minute of stereo at 48 kHz). Reads what the system can (Windows: MP3, AAC,
 * WMA, FLAC, WAV, ... and the sound of a video: MP4, MKV, ...; elsewhere WAV).
 * 0 if it can't. It blocks while it decodes (a second or so for a song).
 *
 *     BareSound song = bare_sound_load("song.mp3");
 *     bare_sound_play_at(song, 1, 30);                // from 0:30
 *     double at = bare_sound_position(song);          // seconds; keep pictures in step with this
 *     bare_sound_stop(song);                          // a pause: play_at(position) resumes */
BareSound bare_sound_load(const char *path);
void      bare_sound_play_at(BareSound sound, float volume, double seconds);
void      bare_sound_loop(BareSound sound, float volume);    /* over and over without a gap, until stopped */
void      bare_sound_volume(BareSound sound, float volume);  /* every play of it, from now on (0..1) */
void      bare_sound_stop(BareSound sound);             /* every play of it */
/* Seconds into its latest play that is still playing; -1 when it isn't playing. */
double    bare_sound_position(BareSound sound);
double    bare_sound_length(BareSound sound);           /* seconds */
/* Its samples, 32-bit floats, interleaved when stereo: to draw a waveform or
 * a spectrum. Valid until bare_sound_free; NULL for 0. */
const float *bare_sound_samples(BareSound sound, int *frames, int *channels, int *rate);

/* ------------------------------------------------------------------------ */
/* Video frames for your own drawing                                        */
/* ------------------------------------------------------------------------ */

/* A video's pictures as pixels, for drawing your own way (ASCII art, effects,
 * analysis): what a <video> drawn into a canvas and read back with
 * getImageData is in a browser. Frames are decoded on a thread of its own, on
 * the GPU when it can, shrunk to fit inside .width x .height (keeping the
 * video's shape), and kept only a moment ahead of the time you ask for, so a
 * film costs no more memory than a clip. Ask for any time, in any order:
 * a jump seeks. Windows only so far (elsewhere it fails).
 *
 *     BareVideo v = bare_video_load("clip.mp4", .width = 160, .height = 90);
 *     int w, h;
 *     const unsigned char *rgba = bare_video_pixels(v, seconds, &w, &h);
 *     if (rgba) { ... w * h pixels, 4 bytes each, rows top first ... }
 *
 * Pair it with bare_sound_load(same path) and use bare_sound_position as the
 * clock to keep the pictures with the sound. */
typedef uint32_t BareVideo;   /* 0 = none */
typedef struct BareVideoOptions {
    int width, height;          /* the most pixels a frame has across and down (0: 320 x 180) */
} BareVideoOptions;
BareVideo bare_video_load_(const char *path, BareVideoOptions options);
#define bare_video_load(path, ...) bare_video_load_((path), (BareVideoOptions){ __VA_ARGS__ })

/* The frame showing at `seconds`, RGBA; valid until the next call for this
 * video. Until that frame is decoded (just after a jump), the last one
 * returned; NULL before the first. */
const unsigned char *bare_video_pixels(BareVideo video, double seconds, int *width, int *height);

typedef struct BareVideoInfo {
    bool ready;                 /* opened: the fields below are known */
    bool failed;                /* not a video, or it can't be read */
    double duration;            /* seconds (0 if the file doesn't say) */
    int width, height;          /* the video's own size */
    float fps;
} BareVideoInfo;
BareVideoInfo bare_video_info(BareVideo video);
void          bare_video_free(BareVideo video);

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
#define BARE__PARAGRAPH(v, ...)     for (int v = bare_paragraph_(&(BareTextStyle){ __VA_ARGS__ }, __FILE__, __LINE__); v; v = bare_paragraph_end_())
/* Calls with no options, like bare_button("Save") or bare_span("text"):
 * the leading arguments are picked out of __VA_ARGS__ and the options after
 * them go into the braces, ended by a field the call never sets (so the
 * braces are never empty, which C11 forbids). BARE__X re-scans for MSVC's
 * traditional preprocessor. */
#define BARE__X(x)                   x
#define BARE__ARG1(...)              BARE__X(BARE__ARG1_(__VA_ARGS__, 0))
#define BARE__ARG1_(a, ...)          a
#define BARE__ARG2(...)              BARE__X(BARE__ARG2_(__VA_ARGS__, 0, 0))
#define BARE__ARG2_(a, b, ...)       b
#define BARE__ARG3(...)              BARE__X(BARE__ARG3_(__VA_ARGS__, 0, 0, 0))
#define BARE__ARG3_(a, b, c, ...)    c
#define BARE__AFTER1(...)            BARE__X(BARE__AFTER1_(__VA_ARGS__, ))
#define BARE__AFTER1_(a, ...)        __VA_ARGS__
#define BARE__AFTER2(...)            BARE__X(BARE__AFTER2_(__VA_ARGS__, ))
#define BARE__AFTER2_(a, b, ...)     __VA_ARGS__
#define BARE__AFTER3(...)            BARE__X(BARE__AFTER3_(__VA_ARGS__, ))
#define BARE__AFTER3_(a, b, c, ...)  __VA_ARGS__

BareState bare_box_(const BareBox *box, const char *file, int line);
BareImage bare_canvas_(const BareCanvas *canvas);
void      bare_pass_(const BarePass *pass, const char *file, int line);
void      bare_draw_(const BareDraw *draw, const char *file, int line);
int       bare_open_(const BareBox *box, const char *file, int line);   /* returns 1 */
int       bare_close_(void);                                            /* returns 0 */
BareState bare_text_(const char *text, const BareTextStyle *style, const char *file, int line);
int       bare_paragraph_(const BareTextStyle *style, const char *file, int line);   /* returns 1 */
int       bare_paragraph_end_(void);                                                 /* returns 0 */
void      bare_span_(const char *text, const BareTextStyle *style);
int       bare_visible_(int count, float size, const char *file, int line);   /* the first item on screen */
int       bare_visible_more_(int i);
BareState bare_input_(char *buf, int cap, const BareInput *input, const char *file, int line);
BareState bare_editor_(BareDoc *doc, const BareEditor *editor, const char *file, int line);
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
