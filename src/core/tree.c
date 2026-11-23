/* tree.c: the per-frame element tree, built while the app declares UI
 * (ARCHITECTURE §4). Elements are stored in pre-order, so walking the array
 * backwards visits children before parents. */
#include "internal.h"
#include <math.h>
#include <string.h>

/* 0 means "default"; BARE_ZERO (-0.0f) means a real zero. */
static float def(float v, float fallback) {
    if (v == 0.0f) return signbit(v) ? 0.0f : fallback;
    return v;
}

/* Glass is on when any field is set; the rest then take their defaults. */
static bool resolve_glass(BareGlass *g) {
    bool on = g->refraction != 0 || g->depth != 0 || g->blur != 0 || g->dispersion != 0 ||
              g->shine != 0 || g->merge != 0 || g->tint.a != 0;
    if (!on) return false;
    g->refraction = def(g->refraction, 1.0f);
    g->depth = def(g->depth, 20.0f);
    g->blur = def(g->blur, 2.0f);
    g->dispersion = def(g->dispersion, 1.0f);
    g->shine = def(g->shine, 1.0f);
    g->merge = BARE__MAX(g->merge, 0.0f);
    return true;
}

static void resolve(BareBox *b) {
    b->opacity = def(b->opacity, 1.0f);
    b->scale = def(b->scale, 1.0f);
    b->grow = def(b->grow, 1.0f);
    b->shrink = def(b->shrink, 1.0f);
    if (b->pad.top == 0 && b->pad.right == 0 && b->pad.bottom == 0 && b->pad.left == 0) {
        b->pad.top = b->pad.right = b->pad.bottom = b->pad.left = b->padding;
    }
    if (b->opacity < 0) b->opacity = 0;
    if (b->opacity > 1) b->opacity = 1;
    if (b->border < 0) b->border = 0;
    if (b->radius < 0) b->radius = 0;
}

/* Element IDs (ARCHITECTURE §4).
 *   explicit: the app's .id, made unique (and reported once) if it repeats;
 *   automatic: parent + call site + how many times that site ran under this
 *   parent, so an element from another line appearing or vanishing doesn't
 *   shift it. */
static BareId make_id(BareContext *ctx, int index, uint64_t parent_id) {
    BareElem *e = &ctx->elems[index];
    uint64_t id;
    if (e->box.id) {
        id = e->box.id;
    } else {
        uint64_t site = bare__hash_u64(parent_id, (uint64_t)(uintptr_t)e->file);
        site = bare__hash_u64(site, (uint64_t)e->line);
        int32_t *seen = bare__map_slot(&ctx->sites, site, NULL);
        int32_t n = seen ? (*seen)++ : 0;
        id = bare__hash_u64(site, (uint64_t)n);
    }
    if (!id) id = 1;

    bool found = false;
    int32_t *owner = bare__map_slot(&ctx->ids, id, &found);
    if (found && owner) {
        const BareElem *first = &ctx->elems[*owner - 1];
        int node = first->node;
        if (node >= 0 && !ctx->store.nodes[node].dup_warned) {
            ctx->store.nodes[node].dup_warned = true;
            bare__log(ctx, "duplicate element id at %s:%d (first used at %s:%d); give list items BARE_IDI(name, i)",
                      e->file, e->line, first->file, first->line);
        }
        /* Keep going with a derived, unique id so state doesn't get shared. */
        for (uint64_t salt = 1;; salt++) {
            uint64_t alt = bare__hash_u64(id, salt);
            int32_t *slot = bare__map_slot(&ctx->ids, alt ? alt : 1, &found);
            if (!found) { if (slot) *slot = index + 1; return alt ? alt : 1; }
        }
    }
    if (owner) *owner = index + 1;
    return id;
}

void bare__tree_begin(BareContext *ctx) {
    ctx->elem_count = 0;
    ctx->depth = 0;
    ctx->overflow = 0;
    int expect = BARE__MAX(ctx->elem_cap, 64);
    bare__map_init(&ctx->sites, &ctx->frame, expect);
    bare__map_init(&ctx->ids, &ctx->frame, expect);
    /* The root: the whole window, a column with no padding. */
    BareBox root = { .width = ctx->width, .height = ctx->height, .id = bare_id("bare.root") };
    int i = bare__tree_push(ctx, &root, "bare", 0);
    ctx->stack[ctx->depth++] = i;
}

/* Appends an element under the innermost open container; returns its index. */
int bare__tree_push(BareContext *ctx, const BareBox *box, const char *file, int line) {
    if (!bare__grow(&ctx->alloc, (void **)&ctx->elems, &ctx->elem_cap, ctx->elem_count + 1, sizeof *ctx->elems)) {
        bare__log(ctx, "out of memory adding an element at %s:%d", file, line);
        return -1;
    }
    int index = ctx->elem_count++;
    BareElem *e = &ctx->elems[index];
    memset(e, 0, sizeof *e);
    e->box = *box;
    resolve(&e->box);
    e->file = file;
    e->line = line;
    e->first_child = e->last_child = e->next_sibling = -1;
    e->node = -1;
    e->parent = ctx->depth ? ctx->stack[ctx->depth - 1] : -1;

    e->glass = resolve_glass(&e->box.glass);

    uint64_t parent_id = BARE__FNV_OFFSET;
    e->opacity = e->box.opacity;
    e->off = e->box.offset;
    if (e->parent >= 0) {
        BareElem *p = &ctx->elems[e->parent];
        parent_id = p->id;
        e->opacity *= p->opacity;
        e->off.x += p->off.x;
        e->off.y += p->off.y;
        if (p->last_child >= 0) ctx->elems[p->last_child].next_sibling = index;
        else p->first_child = index;
        p->last_child = index;
        p->child_count++;
    }
    e->id = make_id(ctx, index, parent_id);
    e->node = bare__store_get(ctx, e->id, NULL);
    if (e->node >= 0) ctx->store.nodes[e->node].last_frame = ctx->frame_index;
    return index;
}

void bare__tree_end(BareContext *ctx) {
    /* Everything but the root should be closed by now. */
    while (ctx->depth > 1) {
        BareElem *e = &ctx->elems[ctx->stack[ctx->depth - 1]];
        bare__log(ctx, "container opened at %s:%d was never closed (break/return inside its block?)",
                  e->file, e->line);
        ctx->depth--;
    }
    ctx->depth = 0;
}

/* ---- public element calls ----------------------------------------------- */

static BareContext *need_frame(const char *what, const char *file, int line) {
    BareContext *ctx = bare_ctx_current();
    if (!ctx || !ctx->in_frame) {
        bare__log(ctx, "%s at %s:%d called outside a frame (call bare_window, then bare_frame)", what, file, line);
        return NULL;
    }
    return ctx;
}

static BareState state_of(BareContext *ctx, int index) {
    BareState s;
    memset(&s, 0, sizeof s);
    if (index < 0) return s;
    int node = ctx->elems[index].node;
    if (node >= 0) s.rect = ctx->store.nodes[node].rect;   /* last frame's; hover/press arrive in M3 */
    return s;
}

BareState bare_box_(const BareBox *box, const char *file, int line) {
    BareContext *ctx = need_frame("bare_box", file, line);
    if (!ctx) { BareState s; memset(&s, 0, sizeof s); return s; }
    int i = bare__tree_push(ctx, box, file, line);
    return state_of(ctx, i);
}

int bare_open_(const BareBox *box, const char *file, int line) {
    BareContext *ctx = need_frame("bare_row/bare_column", file, line);
    if (!ctx) return 1;   /* still run the block once; its children log too */
    int i = bare__tree_push(ctx, box, file, line);
    if (i < 0 || ctx->depth >= BARE__MAX_DEPTH) {
        if (ctx->depth >= BARE__MAX_DEPTH)
            bare__log(ctx, "containers nested deeper than %d at %s:%d", BARE__MAX_DEPTH, file, line);
        ctx->overflow++;
        return 1;
    }
    ctx->stack[ctx->depth++] = i;
    return 1;
}

int bare_close_(void) {
    BareContext *ctx = bare_ctx_current();
    if (!ctx || !ctx->in_frame) return 0;
    if (ctx->overflow) { ctx->overflow--; return 0; }
    if (ctx->depth > 1) ctx->depth--;
    return 0;
}

BareState bare_this(void) {
    BareContext *ctx = bare_ctx_current();
    if (!ctx || !ctx->in_frame || ctx->depth < 1) { BareState s; memset(&s, 0, sizeof s); return s; }
    return state_of(ctx, ctx->stack[ctx->depth - 1]);
}
