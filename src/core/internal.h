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

/* ---- ids ---------------------------------------------------------------- */

#define BARE__FNV_OFFSET 0xcbf29ce484222325ull
uint64_t bare__hash_bytes(uint64_t h, const void *data, size_t size);
uint64_t bare__hash_str(uint64_t h, const char *s);
uint64_t bare__hash_u64(uint64_t h, uint64_t v);

/* ---- element tree ------------------------------------------------------- */

typedef struct BareElem {
    BareId      id;
    int32_t     parent, first_child, last_child, next_sibling, child_count;
    const char *file;               /* call site, for messages */
    int         line;
    BareBox     box;                /* resolved: defaults applied, BARE_ZERO -> 0 */
    float       opacity;            /* effective (times ancestors) */
    BareVec2    off;                /* effective visual offset (plus ancestors'), logical pixels */
    bool        glass;              /* box.glass is on (fields resolved) */
    float       fit_w, fit_h;       /* content size (layout) */
    BareRect    rect;               /* layout result, logical pixels */
} BareElem;

#define BARE__MAX_DEPTH 256

struct BareContext {
    BareAllocator alloc;
    BareArena     frame;            /* reset every frame */

    BareElem     *elems;   int elem_count, elem_cap;    /* pre-order */
    int           stack[BARE__MAX_DEPTH]; int depth;     /* open containers */
    int           overflow;         /* opens beyond MAX_DEPTH, ignored */

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

/* layout.c */
void bare__layout(BareContext *ctx);

/* emit.c */
void bare__emit(BareContext *ctx);

#endif /* BARE_INTERNAL_H */
