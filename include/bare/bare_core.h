/*
 * bare_core.h: advanced header. The headless context and the draw list.
 *
 * Apps don't need this. It's for tests, custom backends and embedding Bare in
 * an existing render loop. See docs/ARCHITECTURE.md §9 and §14.
 */
#ifndef BARE_CORE_H
#define BARE_CORE_H

#include "../bare.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Allocation                                                               */
/* ------------------------------------------------------------------------ */

typedef struct BareAllocator {
    void *(*alloc)(void *user, size_t size);
    void *(*realloc)(void *user, void *ptr, size_t old_size, size_t new_size);
    void  (*free)(void *user, void *ptr, size_t size);
    void *user;
} BareAllocator;

/* ------------------------------------------------------------------------ */
/* Draw list: the core's only output. Physical pixels, already snapped.     */
/* ------------------------------------------------------------------------ */

typedef enum BareCmdKind {
    BARE_CMD_RECT,
    BARE_CMD_TEXT,
    BARE_CMD_IMAGE,
    BARE_CMD_CLIP_PUSH,
    BARE_CMD_CLIP_POP,
    BARE_CMD_GLASS
} BareCmdKind;

typedef struct BareColorF { float r, g, b, a; } BareColorF;  /* sRGB, straight alpha, opacity applied */

typedef struct BareRectCmd {
    BareRect   rect;
    BareColorF fill;
    BareColorF border_color;
    float      radius[4];          /* top-left, top-right, bottom-right, bottom-left */
    float      border;
    float      shadow[4];          /* offset x, offset y, blur, spread */
    BareColorF shadow_color;
} BareRectCmd;

typedef struct BareGlyph { BareRect dst, uv; } BareGlyph;

typedef struct BareTextCmd {
    uint32_t         atlas;        /* texture id */
    BareColorF       color;
    const BareGlyph *glyphs;
    int              count;
    float            blur;         /* text shadows: Gaussian blur radius, physical pixels (0 = sharp) */
} BareTextCmd;

typedef struct BareImageCmd {
    uint32_t   texture;
    BareRect   rect, uv;
    float      radius[4];
    BareColorF tint;
} BareImageCmd;

typedef struct BareClipCmd { BareRect rect; } BareClipCmd;  /* already intersected with the enclosing clip */

/* Liquid glass: refracts everything drawn before it. Up to BARE_GLASS_MAX_SHAPES
 * rounded boxes melt into one surface (smooth union over `merge` pixels).
 * Shapes aren't snapped to pixels: the SDF anti-aliases them, so they can move
 * smoothly. */
#define BARE_GLASS_MAX_SHAPES 8

typedef struct BareGlassShape { BareRect rect; float radius; } BareGlassShape;

typedef struct BareGlassCmd {
    const BareGlassShape *shapes;
    int        count;
    float      refraction, depth, blur, dispersion, shine, merge;   /* pixels where relevant */
    float      zoom;               /* magnification of the backdrop about each shape's centre */
    float      light;              /* rim light direction, radians clockwise from the top left */
    float      amount;             /* 0..1: how much glass there is (fading in or out); shadow excluded */
    BareColorF tint;
    float      shadow[4];          /* offset x, offset y, blur, spread */
    BareColorF shadow_color;
    float      opacity;
} BareGlassCmd;

typedef struct BareDrawCmd {
    BareCmdKind kind;
    union {
        BareRectCmd  rect;
        BareTextCmd  text;
        BareImageCmd image;
        BareClipCmd  clip;
        BareGlassCmd glass;
    } as;
} BareDrawCmd;

typedef enum BareTexFormat { BARE_TEX_R8 = 1, BARE_TEX_RGBA8 } BareTexFormat;

typedef struct BareTextureUpload {
    uint32_t      texture;         /* core-assigned id, never 0 */
    int           width, height;   /* full texture size; (re)created when it changes */
    BareTexFormat format;
    int           x, y, w, h;      /* region to upload */
    const void   *pixels;          /* region pixels, `stride` bytes per row */
    int           stride;
    bool          destroy;         /* free the texture instead */
} BareTextureUpload;

/* Images, canvases, shaders and passes the app asked for since the last draw
 * list, in call order. Run them before drawing the list's commands. */
typedef enum BareGpuOpKind {
    BARE_GPU_IMAGE_CREATE,     /* image: id, w, h, format, nearest, repeat, renderable */
    BARE_GPU_IMAGE_UPLOAD,     /* image: id, pixels (RGBA8, w * h * 4 bytes) */
    BARE_GPU_IMAGE_DESTROY,
    BARE_GPU_SHADER_CREATE,
    BARE_GPU_SHADER_DESTROY,
    BARE_GPU_PASS,
    BARE_GPU_DRAW              /* pass, with the draw fields */
} BareGpuOpKind;

typedef struct BareGpuImage {
    uint32_t    id;
    int         width, height;
    BareFormat  format;
    bool        nearest, repeat;
    bool        renderable;    /* a canvas: passes draw into it */
    bool        mipmaps;       /* a canvas with a mip chain, rebuilt after each pass or draw into it */
    const void *pixels;        /* IMAGE_UPLOAD */
} BareGpuImage;

typedef struct BareGpuShader {
    uint32_t              id;
    const BareShaderCode *code;
} BareGpuShader;

typedef struct BareGpuPass {
    uint32_t    target, shader;
    uint32_t    extra_targets[BARE_MAX_TARGETS - 1];   /* the shader's outputs 1..3; 0 = none */
    uint32_t    inputs[BARE_MAX_INPUTS];
    int         input_count;   /* inputs after the last non-zero one are unbound */
    const void *params;
    int         params_size;
    BareBlend   blend;
    /* BARE_GPU_DRAW only */
    const void *vertices, *instances;            /* NULL: none */
    int         vertex_size, vertex_count, instance_size, instance_count;
    bool        strip, clear, flip;              /* flip: the target is bottom_up (gl_Position.y negated) */
    float       clear_color[4];
    uint32_t    depth;                           /* a BARE_DEPTH canvas, 0 = none */
    BareDepthTest depth_test;
    bool        depth_write, clear_depth;
    float       depth_clear;                     /* the value itself (1 = far) */
} BareGpuPass;

typedef struct BareGpuOp {
    BareGpuOpKind kind;
    union {
        BareGpuImage  image;
        BareGpuShader shader;
        BareGpuPass   pass;
    } as;
} BareGpuOp;

typedef struct BareDrawList {
    const BareGpuOp         *ops;      int op_count;       /* first, in order */
    const BareTextureUpload *uploads;  int upload_count;   /* then these (the glyph atlas) */
    const BareDrawCmd       *cmds;     int cmd_count;
    int        width, height;          /* framebuffer, physical pixels */
    float      scale;                  /* physical / logical */
    BareColorF clear;
} BareDrawList;

/* ------------------------------------------------------------------------ */
/* Context                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct BareContext BareContext;

/* ------------------------------------------------------------------------ */
/* Input: what a platform reports, fed to the core with bare_ctx_event       */
/* ------------------------------------------------------------------------ */

typedef enum BareEventKind {
    BARE_EVENT_NONE = 0,
    BARE_EVENT_MOUSE_MOVE,
    BARE_EVENT_MOUSE_BUTTON,
    BARE_EVENT_MOUSE_LEAVE,    /* the pointer left the window */
    BARE_EVENT_WHEEL,
    BARE_EVENT_KEY,
    BARE_EVENT_RESIZE,         /* size or DPI scale changed */
    BARE_EVENT_EXPOSE,         /* the window needs redrawing */
    BARE_EVENT_FOCUS,
    BARE_EVENT_QUIT,
    BARE_EVENT_TEXT,           /* typed text (UTF-8), after the layout, dead keys and IME did their work */
    BARE_EVENT_COMPOSITION,    /* the IME's text being composed (empty: composition over); replaces the last one */
    BARE_EVENT_FILE_CHOSEN     /* a file dialog's answer (BarePlatform.file_dialog) */
} BareEventKind;

enum { BARE_MOUSE_LEFT = 1, BARE_MOUSE_MIDDLE = 2, BARE_MOUSE_RIGHT = 3 };

/* Logical pixels one wheel notch scrolls (Chrome on Windows: 100). */
#define BARE_WHEEL_NOTCH 100.0f

typedef struct BareEvent {
    BareEventKind kind;
    double time;
    union {
        struct { float x, y; } mouse;                       /* logical pixels */
        struct { int button; bool down; float x, y; } button;
        struct { float dx, dy; bool precise; } wheel;       /* + = right / up; precise = pixels (touchpads), else notches */
        struct { int key; bool down, repeat; int mods; } key;   /* key: a BareKey; mods: BARE_MOD_* bits */
        /* TEXT and COMPOSITION. The bytes only need to live until bare_ctx_event
         * returns. cursor: byte offset of the IME's cursor in the composition. */
        struct { const char *utf8; int len; int cursor; } text;
        struct { float w, h, scale; } size;
        struct { uint64_t request; const char *path; } file;   /* path NULL: cancelled; valid until the next poll */
        bool focused;
    } as;
} BareEvent;

/* ------------------------------------------------------------------------ */
/* Fonts: loading, shaping and rasterizing (ARCHITECTURE §13)               */
/* ------------------------------------------------------------------------ */

/* The default backend is stb_truetype. The interface is shaped so FreeType +
 * HarfBuzz can replace it without touching layout or emit; tests pass a fake. */
typedef struct BareShapedGlyph {
    uint32_t glyph;
    float    advance;              /* pixels at the size shaped, kerning with the next glyph included */
    float    x_offset, y_offset;
    uint32_t cluster;              /* byte offset of its first character in the text */
} BareShapedGlyph;

typedef struct BareGlyphBitmap {
    int width, height;             /* 0 for blank glyphs (space) */
    int left, top;                 /* top-left corner relative to the pen on the baseline (y down) */
    const unsigned char *pixels;   /* R8 coverage, `stride` bytes per row; valid until the next call */
    int stride;
} BareGlyphBitmap;

typedef struct BareFontBackend {
    void *(*load)(const BareAllocator *alloc, const void *data, int size);    /* NULL on failure */
    /* ascent and descent are distances above and below the baseline (both >= 0). */
    void  (*metrics)(void *font, float px, float *ascent, float *descent, float *line_gap);
    /* Writes at most `cap` glyphs (cap >= len) and returns how many. */
    int   (*shape)(void *font, float px, const char *utf8, int len, BareShapedGlyph *out, int cap);
    /* shift_x: sub-pixel offset 0..1 of the pen. */
    bool  (*rasterize)(void *font, uint32_t glyph, float px, float shift_x, BareGlyphBitmap *out);
    void  (*unload)(void *font);
} BareFontBackend;

/* stb_truetype, the default. */
const BareFontBackend *bare_font_backend_stb(void);

typedef struct BareConfig {
    BareAllocator allocator;       /* zero = libc */
    size_t frame_arena_block;      /* zero = 256 KB */
    const BareFontBackend *fonts;  /* NULL = stb_truetype */
} BareConfig;

BareContext *bare_ctx_create(const BareConfig *config);   /* config may be NULL */
void         bare_ctx_destroy(BareContext *ctx);

/* The public bare_* element calls act on the current context. */
void         bare_ctx_make_current(BareContext *ctx);
BareContext *bare_ctx_current(void);

/* width/height in logical pixels; scale = physical / logical; time in seconds. */
void bare_ctx_begin(BareContext *ctx, float width, float height, float scale, double time);
/* Ends the frame and returns its draw list (valid until the next begin).
 * *still_animating (optional) tells the caller whether to keep drawing. */
const BareDrawList *bare_ctx_end(BareContext *ctx, bool *still_animating);

/* Feeds one input event; call between bare_ctx_end and the next
 * bare_ctx_begin. Element states in the next frame reflect it, hit-tested
 * against what was drawn in the last frame. */
void bare_ctx_event(BareContext *ctx, const BareEvent *event);

/* The mouse cursor the UI asks for (the hovered element's .cursor), valid
 * after bare_ctx_end. */
BareCursor bare_ctx_cursor(const BareContext *ctx);
/* Whether a point (logical pixels) is in a .window_drag area of the last
 * frame, with nothing that reacts to the mouse over it. */
bool bare_ctx_window_drag(const BareContext *ctx, BareVec2 p);

/* When nothing is animating the caller may sleep until input arrives, but no
 * later than this time (on the clock passed to bare_ctx_begin): something is
 * timed to change then (a scrollbar fading out). Negative: no such time.
 * Valid after bare_ctx_end. */
double bare_ctx_wake_time(const BareContext *ctx);

void bare_ctx_set_clear(BareContext *ctx, BareColor color);

/* Text input: whether a text field has focus (the platform should deliver
 * TEXT and COMPOSITION events and show an IME there), what kind it is, and the
 * caret's rect in logical window pixels (where the IME's candidate window
 * goes). Valid after bare_ctx_end. */
typedef enum BareTextInputKind { BARE_TEXT_INPUT_NONE = 0, BARE_TEXT_INPUT_LINE, BARE_TEXT_INPUT_MULTILINE,
                                 BARE_TEXT_INPUT_PASSWORD } BareTextInputKind;
BareTextInputKind bare_ctx_text_input(const BareContext *ctx, BareRect *caret);

/* The clipboard text fields use. Without one (headless) the context keeps
 * its own. get returns a string valid until the next call. */
void bare_ctx_set_clipboard(BareContext *ctx, const char *(*get)(void *user), void (*set)(void *user, const char *text),
                            void *user);

/* Editing keys follow macOS (Cmd shortcuts, Option word jumps) or Windows /
 * Linux; the default is the platform built for. Tests switch it. */
void bare_ctx_set_mac_keys(BareContext *ctx, bool mac);

/* Logs from the core (user mistakes, warnings). Default: stderr. */
typedef void (*BareLogFn)(void *user, const char *message);
void bare_ctx_set_log(BareContext *ctx, BareLogFn fn, void *user);

#ifdef __cplusplus
}
#endif

#endif /* BARE_CORE_H */
