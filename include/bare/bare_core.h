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

typedef struct BareDrawList {
    const BareTextureUpload *uploads;  int upload_count;   /* process first */
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
    BARE_EVENT_QUIT
} BareEventKind;

enum { BARE_MOUSE_LEFT = 1, BARE_MOUSE_MIDDLE = 2, BARE_MOUSE_RIGHT = 3 };

typedef struct BareEvent {
    BareEventKind kind;
    double time;
    union {
        struct { float x, y; } mouse;                       /* logical pixels */
        struct { int button; bool down; float x, y; } button;
        struct { float dx, dy; bool precise; } wheel;       /* + = right / up; precise = pixels, else lines */
        struct { int key; bool down, repeat; int mods; } key;   /* key: a BareKey */
        struct { float w, h, scale; } size;
        bool focused;
    } as;
} BareEvent;

typedef struct BareConfig {
    BareAllocator allocator;       /* zero = libc */
    size_t frame_arena_block;      /* zero = 256 KB */
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

void bare_ctx_set_clear(BareContext *ctx, BareColor color);

/* Logs from the core (user mistakes, warnings). Default: stderr. */
typedef void (*BareLogFn)(void *user, const char *message);
void bare_ctx_set_log(BareContext *ctx, BareLogFn fn, void *user);

#ifdef __cplusplus
}
#endif

#endif /* BARE_CORE_H */
