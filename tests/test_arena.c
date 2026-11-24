#include "bare_test.h"
#include "internal.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* An allocator that counts calls, to prove the steady-state frame doesn't allocate. */
static int allocs;
static void *count_alloc(void *u, size_t n) { (void)u; allocs++; return malloc(n); }
static void *count_realloc(void *u, void *p, size_t o, size_t n) { (void)u; (void)o; allocs++; return realloc(p, n); }
static void count_free(void *u, void *p, size_t n) { (void)u; (void)n; free(p); }
static BareAllocator counting(void) {
    BareAllocator a = { count_alloc, count_realloc, count_free, NULL };
    return a;
}

TEST(arena_alignment_and_zeroing) {
    BareArena a;
    bare__arena_init(&a, bare__default_allocator(), 1024);
    for (size_t align = 1; align <= 64; align *= 2) {
        unsigned char *p = bare__arena_alloc(&a, 3, align);
        CHECK(p != NULL);
        CHECK(((uintptr_t)p & (align - 1)) == 0);
        CHECK(p[0] == 0 && p[1] == 0 && p[2] == 0);
    }
    bare__arena_free(&a);
}

TEST(arena_grows_for_big_allocations) {
    BareArena a;
    bare__arena_init(&a, bare__default_allocator(), 256);
    void *small = bare__arena_alloc(&a, 100, 8);
    void *big = bare__arena_alloc(&a, 10000, 8);
    CHECK(small && big);
    memset(big, 1, 10000);   /* must be writable in full */
    CHECK(a.block_count == 2);
    bare__arena_free(&a);
}

TEST(arena_reset_reuses_blocks) {
    BareArena a;
    bare__arena_init(&a, counting(), 512);
    allocs = 0;
    for (int frame = 0; frame < 3; frame++) {
        bare__arena_reset(&a);
        for (int i = 0; i < 40; i++) CHECK(bare__arena_alloc(&a, 64, 16) != NULL);
    }
    int warm = allocs;
    size_t blocks = a.block_count;
    for (int frame = 0; frame < 100; frame++) {
        bare__arena_reset(&a);
        for (int i = 0; i < 40; i++) CHECK(bare__arena_alloc(&a, 64, 16) != NULL);
    }
    CHECK(allocs == warm);            /* high-water mark reached: no more mallocs */
    CHECK(a.block_count == blocks);
    bare__arena_free(&a);
}

TEST(arena_strdup) {
    BareArena a;
    bare__arena_init(&a, bare__default_allocator(), 0);
    char *s = bare__arena_strdup(&a, "hello");
    CHECK(s && strcmp(s, "hello") == 0);
    bare__arena_free(&a);
}

TEST(grow_doubles) {
    BareAllocator al = bare__default_allocator();
    int *items = NULL, cap = 0;
    CHECK(bare__grow(&al, (void **)&items, &cap, 10, sizeof *items));
    CHECK(cap >= 10);
    int first = cap;
    CHECK(bare__grow(&al, (void **)&items, &cap, first + 1, sizeof *items));
    CHECK(cap == first * 2);
    bare__free(&al, items, (size_t)cap * sizeof *items);
}

TEST(allocation_overflow_is_rejected) {
    BareAllocator al = bare__default_allocator();
    CHECK(bare__heap_alloc(&al, SIZE_MAX) == NULL);
    void *p = bare__heap_alloc(&al, 16);
    CHECK(p != NULL);
    CHECK(bare__heap_realloc(&al, p, SIZE_MAX) == NULL);
    bare__heap_free(&al, p);
    void *items = NULL;
    int cap = 0;
    CHECK(!bare__grow(&al, &items, &cap, 100, SIZE_MAX));
    CHECK(items == NULL && cap == 0);
    BareArena a;
    bare__arena_init(&a, al, 0);
    CHECK(bare__arena_alloc(&a, SIZE_MAX, 16) == NULL);
    CHECK(bare__arena_alloc(&a, 32, 16) != NULL);
    CHECK(bare__arena_alloc(&a, SIZE_MAX - 32, 16) == NULL);
    bare__arena_free(&a);
}

int main(void) {
    RUN(allocation_overflow_is_rejected);
    RUN(arena_alignment_and_zeroing);
    RUN(arena_grows_for_big_allocations);
    RUN(arena_reset_reuses_blocks);
    RUN(arena_strdup);
    RUN(grow_doubles);
    return test_report();
}
