/* internal.h: shared by the core's files. Never included by apps or backends. */
#ifndef BARE_INTERNAL_H
#define BARE_INTERNAL_H

#include "bare/bare_core.h"
#include <stddef.h>

#ifndef BARE_ASSERT
#  include <assert.h>
#  define BARE_ASSERT(x) assert(x)
#endif

#define BARE__MIN(a, b) ((a) < (b) ? (a) : (b))
#define BARE__MAX(a, b) ((a) > (b) ? (a) : (b))

/* ---- allocation --------------------------------------------------------- */

BareAllocator bare__default_allocator(void);
void *bare__alloc(const BareAllocator *a, size_t size);
void *bare__realloc(const BareAllocator *a, void *ptr, size_t old_size, size_t new_size);
void  bare__free(const BareAllocator *a, void *ptr, size_t size);

/* Grows *items (each `item_size` bytes) so it holds at least `need`; doubles.
 * Used for persistent arrays that grow only when the UI grows. */
bool bare__grow(const BareAllocator *a, void **items, int *cap, int need, size_t item_size);

/* ---- arena -------------------------------------------------------------- */

typedef struct BareArenaBlock {
    struct BareArenaBlock *next;
    size_t size, used;
} BareArenaBlock;

typedef struct BareArena {
    BareAllocator   alloc;
    BareArenaBlock *first, *current;
    size_t          block_size;
    size_t          block_count;    /* blocks ever allocated (tests check reuse) */
} BareArena;

void  bare__arena_init(BareArena *arena, BareAllocator alloc, size_t block_size);
void *bare__arena_alloc(BareArena *arena, size_t size, size_t align);   /* zeroed */
char *bare__arena_strdup(BareArena *arena, const char *s);
void  bare__arena_reset(BareArena *arena);                              /* keeps blocks */
void  bare__arena_free(BareArena *arena);

/* ---- ids and maps ------------------------------------------------------- */

#define BARE__FNV_OFFSET 0xcbf29ce484222325ull
uint64_t bare__hash_bytes(uint64_t h, const void *data, size_t size);
uint64_t bare__hash_str(uint64_t h, const char *s);
uint64_t bare__hash_u64(uint64_t h, uint64_t v);

/* Per-frame map in the frame arena: key (never 0) -> int32. */
typedef struct BareMap {
    uint64_t  *keys;
    int32_t   *vals;
    int        cap, count;
    BareArena *arena;
} BareMap;

void     bare__map_init(BareMap *m, BareArena *arena, int cap);
/* Finds or inserts `key` (inserted values start at 0); NULL when out of memory. */
int32_t *bare__map_slot(BareMap *m, uint64_t key, bool *found);

/* ---- retained state ----------------------------------------------------- */

typedef struct BareNode {
    BareId   id;               /* 0 = free slot */
    int32_t  next_free;
    uint64_t last_frame;       /* frame it was last declared in */
    BareRect rect;             /* where it was drawn last frame (visual: offset and scale), logical pixels */
    BareVec2 scroll;           /* wheel-driven scroll offset (applied to layout in M6) */
    uint32_t anim;             /* animation set index + 1; 0 = never animated (anim.c) */
    bool     dup_warned;       /* a duplicate of this explicit ID was reported once */
} BareNode;

typedef struct BareStore {
    BareNode *nodes; int node_count, node_cap;
    int       free_head;
    uint64_t *keys;  int32_t *vals; int cap, used;   /* id -> node index */
} BareStore;

int  bare__store_get(BareContext *ctx, BareId id, bool *created);   /* node index, -1 on failure */
int  bare__store_find(const BareContext *ctx, BareId id);            /* node index, -1 if absent */
void bare__store_evict(BareContext *ctx, uint64_t frame);            /* drop nodes not seen in `frame` */
int  bare__store_live(const BareContext *ctx);
void bare__store_free(BareContext *ctx);

/* ---- element tree ------------------------------------------------------- */

typedef struct BareElem {
    BareId      id;
    int32_t     node;               /* retained node index, -1 if none */
    int32_t     parent, first_child, last_child, next_sibling, child_count;
    const char *file;               /* call site, for messages */
    int         line;
    BareBox     box;                /* resolved: defaults applied, BARE_ZERO -> 0 */
    float       opacity;            /* effective (times ancestors) */
    float       vscale;             /* visual transform (offset and scale, with ancestors'): */
    BareVec2    vt;                 /*   drawn at rect * vscale + vt (layout.c, after the passes) */
    bool        glass;              /* box.glass is on (fields resolved) */
    float       fit[2];             /* preferred (max-content) size per axis, layout passes 1/4 */
    float       minc[2];            /* min-content size per axis: shrinking stops here */
    bool        frozen;             /* layout scratch: size settled at a min/max while growing */
    bool        moving;             /* its position or size is animating (or an ancestor's): drawn unsnapped */
    BareRect    rect;               /* layout result, then (after bare__animate) the animated rect */
    BareRect    target;             /* the layout result, kept by bare__animate */
} BareElem;

#define BARE__MAX_DEPTH 256

/* ---- animation (easing.c, spring.c, anim.c) ------------------------------ */

/* One float per animatable property. x and y are relative to the parent, so a
 * moving parent carries its children. Colours are in 0..255 units. */
enum {
    BARE__CH_X, BARE__CH_Y, BARE__CH_W, BARE__CH_H,          /* layout: .animate_layout */
    BARE__CH_COLOR = 4,                                      /* r g b a, then the same for: */
    BARE__CH_BORDER_COLOR = 8,
    BARE__CH_TEXT_COLOR = 12,
    BARE__CH_SHADOW_COLOR = 16,
    BARE__CH_BORDER = 20, BARE__CH_RADIUS, BARE__CH_OPACITY, BARE__CH_SCALE,
    BARE__CH_OFFSET_X, BARE__CH_OFFSET_Y,
    BARE__CH_SHADOW_X, BARE__CH_SHADOW_Y, BARE__CH_SHADOW_BLUR, BARE__CH_SHADOW_SPREAD,
    BARE__CH_COUNT
};

typedef struct BareChannel {
    float value, velocity;      /* what's drawn; units per second */
    float target;
    float from, t;              /* easing: where it started and progress 0..1 */
} BareChannel;

typedef struct BareAnimSet {
    BareChannel ch[BARE__CH_COUNT];
    uint64_t    active;         /* bit per channel still moving */
    BareId      parent;         /* the parent x/y were relative to last frame */
    BareVec2    abs;            /* last animated position, window coordinates */
    bool        ready;          /* channels hold values (a new set snaps to its targets) */
    int32_t     next_free;
} BareAnimSet;

float bare__bezier(const float b[4], float x);                      /* CSS cubic-bezier: progress -> eased */
void  bare__spring_step(float *x, float *v, float response, float damping, float dt);
void  bare__animate(BareContext *ctx);                              /* layout targets -> visual values */
void  bare__anim_release(BareContext *ctx, uint32_t anim);          /* a node holding it was evicted */
void  bare__anim_free(BareContext *ctx);

/* ---- images, shaders and passes (gpu.c) ---------------------------------- */

typedef struct BareImageRec {
    int        width, height;
    BareFormat format;
    bool       alive, canvas;
    int32_t    next_free;
} BareImageRec;

typedef struct BareShaderRec {
    const BareShaderCode *code;
    bool                  alive;
    int32_t               next_free;
} BareShaderRec;

/* Ops in call order, with the bytes they point to (params, pixels) copied in.
 * While queued, pointers hold offsets into `data`; they become real when the
 * queue is handed to the renderer. */
typedef struct BareGpuQueue {
    BareGpuOp     *ops;  int count, cap;
    unsigned char *data; int data_size, data_cap;
} BareGpuQueue;

/* ---- interaction (hit.c, input.c) ---------------------------------------- */

/* One element as last drawn, for hit testing. Same order and indices as the
 * elements of the frame it was built from (paint order). */
typedef struct BareHit {
    BareId   id;
    int32_t  parent;
    int32_t  node;
    BareRect rect;              /* visual, logical pixels */
    float    radius;
    BareRect clip;              /* intersection of clipping ancestors */
    bool     passthrough;
    uint8_t  scroll;            /* BareScroll */
} BareHit;

/* An element and its ancestors (innermost first), by ID. */
typedef struct BareChain { BareId ids[BARE__MAX_DEPTH + 1]; int count; } BareChain;

enum { BARE__PRESS_LEFT, BARE__PRESS_RIGHT, BARE__PRESS_COUNT };

struct BareContext {
    BareAllocator alloc;
    BareArena     frame;            /* reset every frame */
    BareStore     store;            /* retained nodes */

    BareElem     *elems;   int elem_count, elem_cap;    /* pre-order */
    int           stack[BARE__MAX_DEPTH]; int depth;     /* open containers */
    int           overflow;         /* opens beyond MAX_DEPTH, ignored */
    BareMap       sites;            /* (parent, call site) -> times seen this frame */
    BareMap       ids;              /* id -> element index + 1, this frame */

    BareHit      *hits;    int hit_count, hit_cap;      /* last frame, for this frame's input */
    BareVec2      mouse;
    bool          mouse_in;          /* the pointer is over the window */
    bool          held[BARE__PRESS_COUNT];
    BareChain     press[BARE__PRESS_COUNT];   /* where each button went down */
    BareChain     hover;             /* computed at begin */
    BareChain     clicked[BARE__PRESS_COUNT]; /* since the last frame ended */
    BareId        hover_top;         /* innermost hovered element this frame (0 = none) */
    uint8_t       key_down[BARE_KEY_COUNT_], key_pressed[BARE_KEY_COUNT_];
    BareCursor    cursor;

    BareImageRec  *images;  int image_count, image_cap, image_free;   /* by id - 1 */
    BareShaderRec *shaders; int shader_count, shader_cap, shader_free;
    BareGpuQueue   gpu_pending, gpu_published;

    BareAnimSet  *anims;   int anim_count, anim_cap, anim_free;   /* pool; nodes point in by index + 1 */
    bool          anim_active;      /* some channel was still moving at the end of the last frame */
    bool          anim_started;     /* anim_time is valid */
    double        anim_time;        /* time of the last animation step */
    float         anim_frame;       /* last measured frame interval while animating */

    BareDrawCmd  *cmds;    int cmd_count, cmd_cap;
    BareDrawList  list;

    float         width, height, scale;
    double        time;
    uint64_t      frame_index;
    bool          in_frame;
    BareColor     clear;

    BareLogFn     log;
    void         *log_user;
};

void bare__log(BareContext *ctx, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* tree.c */
int  bare__tree_push(BareContext *ctx, const BareBox *box, const char *file, int line);
void bare__tree_begin(BareContext *ctx);
void bare__tree_end(BareContext *ctx);

/* layout.c: the flexbox passes; then (after bare__animate) visual transforms (vscale, vt) */
void bare__layout(BareContext *ctx);
void bare__transforms(BareContext *ctx);

/* hit.c */
void bare__hits_build(BareContext *ctx);
int  bare__hit_find(const BareContext *ctx, BareVec2 p);              /* topmost hit index, -1 */
void bare__hit_chain(const BareContext *ctx, int index, BareChain *out);
bool bare__chain_has(const BareChain *c, BareId id);
BareRect bare__visual_rect(const BareElem *e);

/* gpu.c */
void bare__gpu_publish(BareContext *ctx);       /* pending ops -> the draw list */
void bare__gpu_free(BareContext *ctx);
const BareImageRec *bare__image(const BareContext *ctx, BareImage id);   /* NULL unless alive */

/* input.c */
void bare__input_begin(BareContext *ctx);       /* hover chain from the mouse */
bool bare__input_end(BareContext *ctx);         /* clears one-frame input; true = draw another frame */

/* emit.c */
void bare__emit(BareContext *ctx);

#endif /* BARE_INTERNAL_H */
