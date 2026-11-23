/* store.c: hash maps for IDs (ARCHITECTURE §4).
 *
 * BareMap: a per-frame map (key -> int) living in the frame arena; used for
 * call-site counters and duplicate-ID detection.
 *
 * BareStore: retained nodes, keyed by element ID, kept across frames. Nodes
 * live in a dense pool so their indices stay valid while the hash table grows;
 * the table uses linear probing with backward-shift deletion (no tombstones).
 * Key 0 means "empty" in both (IDs are never 0). */
#include "internal.h"
#include <string.h>

static uint32_t mix(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33;
    return (uint32_t)k;
}

/* ---- per-frame map ------------------------------------------------------ */

void bare__map_init(BareMap *m, BareArena *arena, int cap) {
    int c = 64;
    while (c < cap * 2) c *= 2;
    m->arena = arena;
    m->cap = c;
    m->count = 0;
    m->keys = bare__arena_alloc(arena, sizeof *m->keys * (size_t)c, 8);
    m->vals = bare__arena_alloc(arena, sizeof *m->vals * (size_t)c, 4);
}

static int32_t *map_find(BareMap *m, uint64_t key, bool insert, bool *found) {
    if (!m->keys) return NULL;
    uint32_t mask = (uint32_t)m->cap - 1;
    for (uint32_t i = mix(key) & mask;; i = (i + 1) & mask) {
        if (m->keys[i] == key) { if (found) *found = true; return &m->vals[i]; }
        if (m->keys[i] == 0) {
            if (found) *found = false;
            if (!insert) return NULL;
            m->keys[i] = key;
            m->vals[i] = 0;
            m->count++;
            return &m->vals[i];
        }
    }
}

int32_t *bare__map_slot(BareMap *m, uint64_t key, bool *found) {
    if (key == 0) key = 1;
    if ((m->count + 1) * 2 > m->cap) {
        /* Grow into a fresh arena block; the old table is simply abandoned. */
        BareMap bigger;
        bare__map_init(&bigger, m->arena, m->cap);
        if (!bigger.keys || !bigger.vals) return NULL;
        for (int i = 0; i < m->cap; i++) {
            if (m->keys[i]) *map_find(&bigger, m->keys[i], true, NULL) = m->vals[i];
        }
        *m = bigger;
    }
    return map_find(m, key, true, found);
}

/* ---- retained store ----------------------------------------------------- */

static bool table_grow(BareContext *ctx) {
    BareStore *s = &ctx->store;
    int cap = s->cap ? s->cap * 2 : 256;
    uint64_t *keys = bare__alloc(&ctx->alloc, sizeof *keys * (size_t)cap);
    int32_t *vals = bare__alloc(&ctx->alloc, sizeof *vals * (size_t)cap);
    if (!keys || !vals) {
        bare__free(&ctx->alloc, keys, sizeof *keys * (size_t)cap);
        bare__free(&ctx->alloc, vals, sizeof *vals * (size_t)cap);
        return false;
    }
    memset(keys, 0, sizeof *keys * (size_t)cap);
    uint32_t mask = (uint32_t)cap - 1;
    for (int i = 0; i < s->cap; i++) {
        if (!s->keys[i]) continue;
        uint32_t j = mix(s->keys[i]) & mask;
        while (keys[j]) j = (j + 1) & mask;
        keys[j] = s->keys[i];
        vals[j] = s->vals[i];
    }
    bare__free(&ctx->alloc, s->keys, sizeof *s->keys * (size_t)s->cap);
    bare__free(&ctx->alloc, s->vals, sizeof *s->vals * (size_t)s->cap);
    s->keys = keys;
    s->vals = vals;
    s->cap = cap;
    return true;
}

int bare__store_find(const BareContext *ctx, BareId id) {
    const BareStore *s = &ctx->store;
    if (!s->cap) return -1;
    uint32_t mask = (uint32_t)s->cap - 1;
    for (uint32_t i = mix(id) & mask; s->keys[i]; i = (i + 1) & mask) {
        if (s->keys[i] == id) return s->vals[i];
    }
    return -1;
}

int bare__store_get(BareContext *ctx, BareId id, bool *created) {
    BareStore *s = &ctx->store;
    if (created) *created = false;
    if (s->cap) {
        uint32_t mask = (uint32_t)s->cap - 1;
        for (uint32_t i = mix(id) & mask; s->keys[i]; i = (i + 1) & mask) {
            if (s->keys[i] == id) return s->vals[i];
        }
    }
    if ((s->used + 1) * 10 > s->cap * 7 && !table_grow(ctx)) return -1;

    /* A node from the free list, or a new one at the end of the pool. */
    int index;
    if (s->free_head >= 0) {
        index = s->free_head;
        s->free_head = s->nodes[index].next_free;
    } else {
        if (!bare__grow(&ctx->alloc, (void **)&s->nodes, &s->node_cap, s->node_count + 1, sizeof *s->nodes))
            return -1;
        index = s->node_count++;
    }
    BareNode *n = &s->nodes[index];
    memset(n, 0, sizeof *n);
    n->id = id;
    n->next_free = -1;

    uint32_t mask = (uint32_t)s->cap - 1;
    uint32_t i = mix(id) & mask;
    while (s->keys[i]) i = (i + 1) & mask;
    s->keys[i] = id;
    s->vals[i] = index;
    s->used++;
    if (created) *created = true;
    return index;
}

static void table_remove(BareStore *s, BareId id) {
    uint32_t mask = (uint32_t)s->cap - 1;
    uint32_t i = mix(id) & mask;
    while (s->keys[i] != id) {
        if (!s->keys[i]) return;
        i = (i + 1) & mask;
    }
    /* Backward-shift: pull later entries of the same probe run into the hole. */
    for (uint32_t j = (i + 1) & mask; s->keys[j]; j = (j + 1) & mask) {
        uint32_t home = mix(s->keys[j]) & mask;
        bool movable = (j > i) ? (home <= i || home > j) : (home <= i && home > j);
        if (movable) {
            s->keys[i] = s->keys[j];
            s->vals[i] = s->vals[j];
            i = j;
        }
    }
    s->keys[i] = 0;
    s->used--;
}

void bare__store_evict(BareContext *ctx, uint64_t frame) {
    BareStore *s = &ctx->store;
    for (int i = 0; i < s->node_count; i++) {
        BareNode *n = &s->nodes[i];
        if (n->id == 0 || n->last_frame == frame) continue;
        table_remove(s, n->id);
        n->id = 0;
        n->next_free = s->free_head;
        s->free_head = i;
    }
}

int bare__store_live(const BareContext *ctx) { return ctx->store.used; }

void bare__store_free(BareContext *ctx) {
    BareStore *s = &ctx->store;
    bare__free(&ctx->alloc, s->keys, sizeof *s->keys * (size_t)s->cap);
    bare__free(&ctx->alloc, s->vals, sizeof *s->vals * (size_t)s->cap);
    bare__free(&ctx->alloc, s->nodes, sizeof *s->nodes * (size_t)s->node_cap);
    memset(s, 0, sizeof *s);
    s->free_head = -1;
}
