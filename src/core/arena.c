/* arena.c: allocator plumbing and the bump arena (ARCHITECTURE §12). */
#include "internal.h"
#include <stdlib.h>
#include <string.h>

static void *libc_alloc(void *user, size_t size) { (void)user; return malloc(size); }
static void *libc_realloc(void *user, void *ptr, size_t old_size, size_t new_size) {
    (void)user; (void)old_size;
    return realloc(ptr, new_size);
}
static void libc_free(void *user, void *ptr, size_t size) { (void)user; (void)size; free(ptr); }

BareAllocator bare__default_allocator(void) {
    BareAllocator a = { libc_alloc, libc_realloc, libc_free, NULL };
    return a;
}

void *bare__alloc(const BareAllocator *a, size_t size) { return a->alloc(a->user, size); }
void *bare__realloc(const BareAllocator *a, void *ptr, size_t old_size, size_t new_size) {
    return a->realloc(a->user, ptr, old_size, new_size);
}
void bare__free(const BareAllocator *a, void *ptr, size_t size) {
    if (ptr) a->free(a->user, ptr, size);
}

/* For libraries (stb) whose free and realloc don't pass the size: the size
 * rides in a 16-byte header in front of the block. */
#define SIZED_HEADER 16

void *bare__heap_alloc(const BareAllocator *a, size_t size) {
    unsigned char *p = bare__alloc(a, size + SIZED_HEADER);
    if (!p) return NULL;
    memcpy(p, &size, sizeof size);
    return p + SIZED_HEADER;
}

void *bare__heap_realloc(const BareAllocator *a, void *ptr, size_t size) {
    if (!ptr) return bare__heap_alloc(a, size);
    unsigned char *base = (unsigned char *)ptr - SIZED_HEADER;
    size_t old;
    memcpy(&old, base, sizeof old);
    unsigned char *p = bare__realloc(a, base, old + SIZED_HEADER, size + SIZED_HEADER);
    if (!p) return NULL;
    memcpy(p, &size, sizeof size);
    return p + SIZED_HEADER;
}

void bare__heap_free(const BareAllocator *a, void *ptr) {
    if (!ptr) return;
    unsigned char *base = (unsigned char *)ptr - SIZED_HEADER;
    size_t size;
    memcpy(&size, base, sizeof size);
    bare__free(a, base, size + SIZED_HEADER);
}

bool bare__grow(const BareAllocator *a, void **items, int *cap, int need, size_t item_size) {
    if (need <= *cap) return true;
    int new_cap = *cap ? *cap : 64;
    while (new_cap < need) new_cap *= 2;
    void *p = bare__realloc(a, *items, (size_t)*cap * item_size, (size_t)new_cap * item_size);
    if (!p) return false;
    *items = p;
    *cap = new_cap;
    return true;
}

/* ---- arena -------------------------------------------------------------- */

#define BLOCK_HEADER ((sizeof(BareArenaBlock) + 15) & ~(size_t)15)

static unsigned char *block_data(BareArenaBlock *b) { return (unsigned char *)b + BLOCK_HEADER; }

void bare__arena_init(BareArena *arena, BareAllocator alloc, size_t block_size) {
    memset(arena, 0, sizeof *arena);
    arena->alloc = alloc;
    arena->block_size = block_size ? block_size : 256 * 1024;
}

static BareArenaBlock *new_block(BareArena *arena, size_t min_size) {
    size_t size = BARE__MAX(arena->block_size, min_size);
    BareArenaBlock *b = bare__alloc(&arena->alloc, BLOCK_HEADER + size);
    if (!b) return NULL;
    b->next = NULL;
    b->size = size;
    b->used = 0;
    arena->block_count++;
    return b;
}

/* Where `size` bytes aligned to `align` would start in `b`, or NULL if they don't fit. */
static unsigned char *fit(BareArenaBlock *b, size_t size, size_t align) {
    uintptr_t base = (uintptr_t)block_data(b);
    uintptr_t p = (base + b->used + (align - 1)) & ~(uintptr_t)(align - 1);
    if (p + size > base + b->size) return NULL;
    return (unsigned char *)p;
}

void *bare__arena_alloc(BareArena *arena, size_t size, size_t align) {
    BARE_ASSERT(align && (align & (align - 1)) == 0);
    if (size == 0) size = 1;
    if (!arena->current) {
        if (!arena->first && !(arena->first = new_block(arena, size + align))) return NULL;
        arena->current = arena->first;
    }
    unsigned char *p = fit(arena->current, size, align);
    while (!p) {
        /* Reuse the next kept block if it fits, else insert a new one after current. */
        BareArenaBlock *next = arena->current->next;
        if (next) {
            next->used = 0;
            if ((p = fit(next, size, align)) != NULL) { arena->current = next; break; }
        }
        BareArenaBlock *b = new_block(arena, size + align);
        if (!b) return NULL;
        b->next = next;
        arena->current->next = b;
        arena->current = b;
        p = fit(b, size, align);
    }
    arena->current->used = (size_t)(p - block_data(arena->current)) + size;
    memset(p, 0, size);
    return p;
}

char *bare__arena_strdup(BareArena *arena, const char *s) {
    size_t n = strlen(s) + 1;
    char *d = bare__arena_alloc(arena, n, 1);
    if (d) memcpy(d, s, n);
    return d;
}

void bare__arena_reset(BareArena *arena) {
    for (BareArenaBlock *b = arena->first; b; b = b->next) {
        b->used = 0;
#ifndef NDEBUG
        memset(block_data(b), 0xCD, b->size);   /* catch use of last frame's data */
#endif
    }
    arena->current = arena->first;
}

void bare__arena_free(BareArena *arena) {
    BareArenaBlock *b = arena->first;
    while (b) {
        BareArenaBlock *next = b->next;
        bare__free(&arena->alloc, b, BLOCK_HEADER + b->size);
        b = next;
    }
    arena->first = arena->current = NULL;
}
