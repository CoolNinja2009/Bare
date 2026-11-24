/* input.c: events -> mouse, buttons, keys and per-element state
 * (ARCHITECTURE §8).
 *
 * Presses and clicks are recorded as chains: the element under the mouse and
 * all its ancestors. A press marks the chain where the button went down; on
 * release, the elements in both that chain and the one under the mouse now
 * are clicked (the DOM's rule: a click goes to the nearest common ancestor of
 * the down and up targets, and bubbles). So pressing a button's icon presses
 * the button, and dragging off and back on still clicks it. */
#include "internal.h"
#include <string.h>

static int press_slot(int button) {
    return button == BARE_MOUSE_LEFT ? BARE__PRESS_LEFT : button == BARE_MOUSE_RIGHT ? BARE__PRESS_RIGHT : -1;
}

static void chain_at(const BareContext *ctx, BareVec2 p, BareChain *out) {
    int i = bare__hit_find(ctx, p);
    if (i >= 0) bare__hit_chain(ctx, i, out);
    else out->count = 0;
}

static void release_all(BareContext *ctx) {
    memset(ctx->key_down, 0, sizeof ctx->key_down);
    for (int s = 0; s < BARE__PRESS_COUNT; s++) {
        ctx->held[s] = false;
        ctx->press[s].count = 0;
    }
    ctx->drag_id = 0;
}

static void button(BareContext *ctx, int b, bool down, BareVec2 p) {
    ctx->mouse = p;
    ctx->mouse_in = true;
    int s = press_slot(b);
    if (s < 0) return;
    if (down) {
        ctx->held[s] = true;
        int i = bare__hit_find(ctx, p);
        if (i >= 0) bare__hit_chain(ctx, i, &ctx->press[s]);
        else ctx->press[s].count = 0;
        if (i >= 0 && s == BARE__PRESS_LEFT && ctx->hits[i].bar) bare__scroll_press(ctx, i, p);
        return;
    }
    if (s == BARE__PRESS_LEFT) ctx->drag_id = 0;
    if (!ctx->held[s]) return;   /* the press started outside the window */
    BareChain now;
    chain_at(ctx, p, &now);
    BareChain *out = &ctx->clicked[s];
    for (int i = 0; i < ctx->press[s].count; i++) {
        BareId id = ctx->press[s].ids[i];
        if (bare__chain_has(&now, id) && !bare__chain_has(out, id) && out->count < BARE__MAX_DEPTH + 1)
            out->ids[out->count++] = id;
    }
    ctx->held[s] = false;
    ctx->press[s].count = 0;
}

void bare_ctx_event(BareContext *ctx, const BareEvent *e) {
    if (!ctx || !e) return;
    switch (e->kind) {
    case BARE_EVENT_MOUSE_MOVE:
        ctx->mouse = (BareVec2){ e->as.mouse.x, e->as.mouse.y };
        ctx->mouse_in = true;
        ctx->mouse_moved = true;
        if (ctx->drag_id && ctx->held[BARE__PRESS_LEFT]) bare__scroll_drag(ctx, ctx->mouse);
        break;
    case BARE_EVENT_MOUSE_LEAVE:
        ctx->mouse_in = false;
        break;
    case BARE_EVENT_MOUSE_BUTTON:
        button(ctx, e->as.button.button, e->as.button.down, (BareVec2){ e->as.button.x, e->as.button.y });
        break;
    case BARE_EVENT_WHEEL:
        bare__scroll_wheel(ctx, e->as.wheel.dx, e->as.wheel.dy, e->as.wheel.precise);   /* scroll.c */
        break;
    case BARE_EVENT_KEY: {
        int k = e->as.key.key;
        if (k <= BARE_KEY_NONE || k >= BARE_KEY_COUNT_) break;
        if (e->as.key.down) ctx->key_pressed[k] = 1;   /* first press and auto-repeats, like keydown */
        ctx->key_down[k] = e->as.key.down;
        break;
    }
    case BARE_EVENT_FOCUS:
        /* Keys released while another window had focus never arrive. */
        if (!e->as.focused) release_all(ctx);
        break;
    default:
        break;
    }
}

void bare__input_begin(BareContext *ctx) {
    ctx->cursor = BARE_CURSOR_DEFAULT;
    ctx->hover.count = 0;
    if (ctx->mouse_in) chain_at(ctx, ctx->mouse, &ctx->hover);
    ctx->hover_top = ctx->hover.count ? ctx->hover.ids[0] : 0;
}

bool bare__input_end(BareContext *ctx) {
    /* Another frame is needed when the UI moved under a still mouse (the
     * hover changed), or after a click or key press: the app may have
     * declared elements before it reacted to it. */
    bool again = false;
    BareId top = 0;
    if (ctx->mouse_in) {
        int i = bare__hit_find(ctx, ctx->mouse);
        if (i >= 0) top = ctx->hits[i].id;
    }
    if (top != ctx->hover_top) again = true;
    ctx->mouse_moved = false;
    for (int s = 0; s < BARE__PRESS_COUNT; s++) {
        if (ctx->clicked[s].count) again = true;
        ctx->clicked[s].count = 0;
    }
    for (int k = 0; k < BARE_KEY_COUNT_; k++) {
        if (ctx->key_pressed[k]) again = true;
        ctx->key_pressed[k] = 0;
    }
    return again;
}

BareCursor bare_ctx_cursor(const BareContext *ctx) { return ctx ? ctx->cursor : BARE_CURSOR_DEFAULT; }

/* ---- public queries on the current context ------------------------------ */

BareVec2 bare_mouse(void) {
    BareContext *ctx = bare_ctx_current();
    return ctx ? ctx->mouse : (BareVec2){ 0, 0 };
}

bool bare_key_down(BareKey key) {
    BareContext *ctx = bare_ctx_current();
    return ctx && key > BARE_KEY_NONE && key < BARE_KEY_COUNT_ && ctx->key_down[key];
}

bool bare_key_pressed(BareKey key) {
    BareContext *ctx = bare_ctx_current();
    return ctx && key > BARE_KEY_NONE && key < BARE_KEY_COUNT_ && ctx->key_pressed[key];
}
