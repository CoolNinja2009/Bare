/* tree.c: the per-frame element tree, built while the app declares UI
 * (ARCHITECTURE §4). Elements are stored in pre-order, so walking the array
 * backwards visits children before parents. */
#include "internal.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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
int bare__tree_push(BareContext *ctx, const BareBox *in, const char *file, int line) {
    if (!bare__grow(&ctx->alloc, (void **)&ctx->elems, &ctx->elem_cap, ctx->elem_count + 1, sizeof *ctx->elems)) {
        bare__log(ctx, "out of memory adding an element at %s:%d", file, line);
        return -1;
    }
    int index = ctx->elem_count++;
    BareElem *e = &ctx->elems[index];
    memset(e, 0, sizeof *e);
    bare__style_box(ctx, in, &e->box, file, line);   /* its style chain under it */
    const BareBox *box = &e->box;
    const BareLook hover = box->hover, press = box->press;
    const BareCursor cursor = box->cursor;
    e->file = file;
    e->line = line;
    e->first_child = e->last_child = e->next_sibling = -1;
    e->node = -1;
    e->parent = ctx->depth ? ctx->stack[ctx->depth - 1] : -1;

    uint64_t parent_id = BARE__FNV_OFFSET;
    if (e->parent >= 0) {
        BareElem *p = &ctx->elems[e->parent];
        parent_id = p->id;
        if (p->last_child >= 0) ctx->elems[p->last_child].next_sibling = index;
        else p->first_child = index;
        p->last_child = index;
        p->child_count++;
    }
    e->id = make_id(ctx, index, parent_id);
    e->node = bare__store_get(ctx, e->id, NULL);
    if (e->node >= 0) ctx->store.nodes[e->node].last_frame = ctx->frame_index;

    /* Looks apply in the same frame the state is known: hover, then press on top. */
    bool hovered = bare__chain_has(&ctx->hover, e->id);
    if (hovered) bare__apply_look(&e->box, &hover);
    if (bare__chain_has(&ctx->press[BARE__PRESS_LEFT], e->id)) bare__apply_look(&e->box, &press);
    /* The innermost hovered element with a cursor wins; it's declared last. */
    if (hovered && cursor != BARE_CURSOR_DEFAULT) ctx->cursor = cursor;

    bare__resolve_box(&e->box);
    e->glass = bare__resolve_glass(&e->box.glass);
    e->glass_amount = e->glass ? 1.0f : 0.0f;   /* animation may fade it (anim.c) */
    e->opacity = e->box.opacity * (e->parent >= 0 ? ctx->elems[e->parent].opacity : 1.0f);
    return index;
}

void bare__tree_end(BareContext *ctx) {
    /* Everything but the root should be closed by now. */
    while (ctx->depth > 1) {
        BareElem *e = &ctx->elems[ctx->stack[ctx->depth - 1]];
        bare__log(ctx, "container opened at %s:%d was never closed (break/return inside its block?)",
                  e->file, e->line);
        bare__scroll_close(ctx, ctx->stack[ctx->depth - 1]);
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
    const BareElem *e = &ctx->elems[index];
    s.hovered = bare__chain_has(&ctx->hover, e->id);
    s.pressed = bare__chain_has(&ctx->press[BARE__PRESS_LEFT], e->id);
    s.clicked = bare__chain_has(&ctx->clicked[BARE__PRESS_LEFT], e->id);
    s.right_clicked = bare__chain_has(&ctx->clicked[BARE__PRESS_RIGHT], e->id);
    if (e->node >= 0) {
        s.rect = ctx->store.nodes[e->node].rect;   /* last frame's */
        s.scroll = ctx->store.nodes[e->node].scroll;
    }
    return s;
}

BareState bare_box_(const BareBox *box, const char *file, int line) {
    BareContext *ctx = need_frame("bare_box", file, line);
    if (!ctx) { BareState s; memset(&s, 0, sizeof s); return s; }
    int i = bare__tree_push(ctx, box, file, line);
    return state_of(ctx, i);
}

/* Text is a leaf element sized by its content; its colour comes from the
 * nearest .text_color unless the style sets one (emit.c). */
static BareState push_text(BareContext *ctx, const char *str, int len, const BareTextStyle *in,
                           const char *file, int line) {
    BareTextStyle merged;
    bare__style_text(ctx, in, &merged, file, line);
    const BareTextStyle *style = &merged;
    BareBox box;
    memset(&box, 0, sizeof box);
    box.id = style->id;
    box.text_color = style->color;
    box.animate = style->animate;
    box.animate_layout = BARE_INSTANT;   /* text glides with its parent, never on its own */
    int i = bare__tree_push(ctx, &box, file, line);
    if (i >= 0) ctx->elems[i].text = bare__text_new(ctx, str, len, style);
    return state_of(ctx, i);
}

BareState bare_text_(const char *text, const BareTextStyle *style, const char *file, int line) {
    BareContext *ctx = need_frame("bare_text", file, line);
    if (!ctx) { BareState s; memset(&s, 0, sizeof s); return s; }
    if (!text) text = "";
    return push_text(ctx, text, (int)strlen(text), style, file, line);
}

BareState bare_textf_(const char *file, int line, const char *fmt, ...) {
    BareContext *ctx = need_frame("bare_textf", file, line);
    BareState none;
    memset(&none, 0, sizeof none);
    if (!ctx || !fmt) return none;
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *buf = n >= 0 ? bare__arena_alloc(&ctx->frame, (size_t)n + 1, 1) : NULL;
    if (buf) vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    BareTextStyle style;
    memset(&style, 0, sizeof style);
    return push_text(ctx, buf ? buf : "", buf ? n : 0, &style, file, line);
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
    if (ctx->depth > 1) {
        bare__scroll_close(ctx, ctx->stack[ctx->depth - 1]);   /* its scrollbars go on top of its children */
        ctx->depth--;
    }
    return 0;
}

BareState bare_this(void) {
    BareContext *ctx = bare_ctx_current();
    if (!ctx || !ctx->in_frame || ctx->depth < 1) { BareState s; memset(&s, 0, sizeof s); return s; }
    return state_of(ctx, ctx->stack[ctx->depth - 1]);
}
