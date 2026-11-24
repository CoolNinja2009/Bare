/* focus.c: keyboard focus and the keyboard queue (ARCHITECTURE §8).
 *
 * One element at a time has focus, by id. A left press focuses the innermost
 * .focusable element under it (text fields are focusable) or blurs; Tab and
 * Shift+Tab walk the focusable elements of the last frame in declaration
 * (pre-) order; Escape blurs.
 *
 * Keys and text arrive between frames, in an order that matters ("a",
 * Backspace, "b"), so while a field has focus they're queued and the field
 * replays them when it's declared. A Tab that moves focus splits the queue:
 * what came before goes to the old field this frame, the Tab moves focus at
 * the start of the next frame, and the rest goes to the new field then. Keys
 * a focused field uses never reach bare_key_pressed, so a shortcut under the
 * field (Ctrl+Z in the app) doesn't fire while you type. */
#include "internal.h"
#include <string.h>

int bare__mods(const BareContext *ctx) {
    int m = 0;
    if (ctx->key_down[BARE_KEY_SHIFT]) m |= BARE_MOD_SHIFT;
    if (ctx->key_down[BARE_KEY_CTRL]) m |= BARE_MOD_CTRL;
    if (ctx->key_down[BARE_KEY_ALT]) m |= BARE_MOD_ALT;
    if (ctx->key_down[BARE_KEY_SUPER]) m |= BARE_MOD_SUPER;
    return m;
}

static int hit_of(const BareContext *ctx, BareId id) {
    if (!id) return -1;
    for (int i = 0; i < ctx->hit_count; i++)
        if (ctx->hits[i].id == id) return i;
    return -1;
}

void bare__focus_set(BareContext *ctx, BareId id, int how) {
    if (id == ctx->focus_id) return;
    ctx->focus_id = id;
    ctx->focus_how = (uint8_t)how;
    int h = hit_of(ctx, id);
    ctx->focus_kind = h >= 0 ? ctx->hits[h].focus : 0;   /* until it's declared again */
}

/* ---- the queue ------------------------------------------------------------ */

static BareKeyEv *push(BareContext *ctx, int kind) {
    if (!bare__grow(&ctx->alloc, (void **)&ctx->in, &ctx->in_cap, ctx->in_count + 1, sizeof *ctx->in)) return NULL;
    BareKeyEv *k = &ctx->in[ctx->in_count++];
    memset(k, 0, sizeof *k);
    k->kind = (uint8_t)kind;
    return k;
}

static int push_text(BareContext *ctx, const char *s, int len) {
    if (len < 0) len = 0;
    if (!bare__grow(&ctx->alloc, (void **)&ctx->in_text, &ctx->in_text_cap, ctx->in_text_len + len + 1, 1)) return -1;
    int at = ctx->in_text_len;
    if (len) memcpy(ctx->in_text + at, s, (size_t)len);
    ctx->in_text[at + len] = 0;
    ctx->in_text_len += len + 1;
    return at;
}

/* Whether a focused field uses this key press (so the app doesn't see it). */
static bool field_takes(BareContext *ctx, int key, int mods) {
    bool extend;
    if (key == BARE_KEY_TAB) return (ctx->focus_kind & BARE__FOCUS_TABS) && !(mods & BARE_MOD_SHIFT);
    return bare__edit_lookup(ctx, key, mods, &extend) != BARE_EDIT_NONE || bare__edit_types(ctx, key, mods);
}

static bool any_focusable(const BareContext *ctx) {
    for (int i = 0; i < ctx->hit_count; i++)
        if (ctx->hits[i].focus & BARE__FOCUS_ABLE) return true;
    return false;
}

/* After input.c recorded a key: queue it if focus or a field uses it, and
 * then the app's bare_key_pressed doesn't see it. */
void bare__focus_event(BareContext *ctx, const BareEvent *e) {
    bool field = ctx->focus_id && (ctx->focus_kind & BARE__FOCUS_FIELD);
    if (e->kind == BARE_EVENT_TEXT || e->kind == BARE_EVENT_COMPOSITION) {
        if (!field) return;
        int at = push_text(ctx, e->as.text.utf8 ? e->as.text.utf8 : "", e->as.text.utf8 ? e->as.text.len : 0);
        BareKeyEv *k = at >= 0 ? push(ctx, e->kind == BARE_EVENT_TEXT ? BARE__IN_TEXT : BARE__IN_COMPOSE) : NULL;
        if (!k) return;
        k->text = at;
        k->len = e->as.text.utf8 ? e->as.text.len : 0;
        k->cursor = e->as.text.cursor;
        return;
    }
    if (e->kind != BARE_EVENT_KEY || !e->as.key.down) return;
    int key = e->as.key.key;
    if (key == BARE_KEY_SHIFT || key == BARE_KEY_CTRL || key == BARE_KEY_ALT || key == BARE_KEY_SUPER) return;
    int mods = (e->as.key.mods | bare__mods(ctx)) & 15;
    bool navigates = (key == BARE_KEY_TAB && !(mods & (BARE_MOD_CTRL | BARE_MOD_ALT | BARE_MOD_SUPER)) &&
                      (ctx->focus_id || any_focusable(ctx))) ||
                     (key == BARE_KEY_ESCAPE && ctx->focus_id);
    bool takes = field && field_takes(ctx, key, mods);
    if (!navigates && !takes) return;
    BareKeyEv *k = push(ctx, BARE__IN_KEY);
    if (k) {
        k->key = key;
        k->mods = (uint8_t)mods;
        k->repeat = e->as.key.repeat;
    }
    ctx->key_pressed[key] = 0;   /* input.c set it; the field (or focus) has it instead */
}

/* bare_edit(id, action) from the app: queued like a key, for the field to run. */
void bare_edit(BareId field, BareEditAction action) {
    BareContext *ctx = bare_ctx_current();
    if (!ctx || action <= BARE_EDIT_NONE || action >= BARE_EDIT_ACTION_COUNT_) return;
    if (field && field != ctx->focus_id) bare__focus_set(ctx, field, 3);
    if (!ctx->focus_id) return;
    BareKeyEv *k = push(ctx, BARE__IN_ACTION);
    if (k) k->key = action;
}

/* ---- each frame ----------------------------------------------------------- */

static void drop_front(BareContext *ctx, int n) {
    if (n <= 0) return;
    if (n >= ctx->in_count) { ctx->in_count = 0; ctx->in_text_len = 0; return; }
    memmove(ctx->in, ctx->in + n, sizeof *ctx->in * (size_t)(ctx->in_count - n));
    ctx->in_count -= n;
}

static bool moves_focus(const BareContext *ctx, const BareKeyEv *k) {
    if (k->kind != BARE__IN_KEY || k->key != BARE_KEY_TAB) return false;
    return !((ctx->focus_kind & BARE__FOCUS_TABS) && !(k->mods & BARE_MOD_SHIFT));
}

static void tab(BareContext *ctx, bool back) {
    int n = ctx->hit_count, cur = hit_of(ctx, ctx->focus_id);
    for (int step = 1; step <= n; step++) {
        int i = cur < 0 ? (back ? n - step : step - 1) : ((cur + (back ? -step : step)) % n + n) % n;
        if (!(ctx->hits[i].focus & BARE__FOCUS_ABLE)) continue;
        bare__focus_set(ctx, ctx->hits[i].id, 2);
        bare__scroll_into_view(ctx, i);
        return;
    }
}

void bare__focus_begin(BareContext *ctx) {
    ctx->focus_declared = false;
    ctx->field_count = 0;
    ctx->ti_kind = BARE_TEXT_INPUT_NONE;
    /* Navigation keys at the head of the queue act now; the first one that
     * comes after typing waits for the next frame. */
    while (ctx->in_count > 0) {
        const BareKeyEv *k = &ctx->in[0];
        if (moves_focus(ctx, k)) {
            tab(ctx, (k->mods & BARE_MOD_SHIFT) != 0);
        } else if (k->kind == BARE__IN_KEY && k->key == BARE_KEY_ESCAPE && !(ctx->focus_kind & BARE__FOCUS_FIELD)) {
            bare__focus_set(ctx, 0, 0);
        } else {
            break;
        }
        drop_front(ctx, 1);
    }
    int take = 0;
    while (take < ctx->in_count && !moves_focus(ctx, &ctx->in[take])) take++;
    ctx->in_take = take;
}

/* What the focused field took is gone; what's left waits for the next frame. */
void bare__focus_end(BareContext *ctx) {
    drop_front(ctx, ctx->in_take);
    ctx->in_take = 0;
    if (ctx->focus_id && !ctx->focus_declared) {   /* it went away */
        ctx->focus_id = 0;
        ctx->focus_kind = 0;
    }
    if (!ctx->focus_id || !(ctx->focus_kind & BARE__FOCUS_FIELD)) {   /* nothing to replay text into */
        int keep = 0;
        for (int i = 0; i < ctx->in_count; i++)
            if (ctx->in[i].kind == BARE__IN_KEY) ctx->in[keep++] = ctx->in[i];
        ctx->in_count = keep;
        if (!keep) ctx->in_text_len = 0;
    }
}

/* A left press: focus follows it to the innermost focusable element. */
void bare__focus_press(BareContext *ctx, int hit) {
    if (hit >= 0 && ctx->hits[hit].bar) return;   /* scrollbars don't take focus */
    BareId to = 0;
    for (int i = hit; i >= 0; i = ctx->hits[i].parent) {
        if (ctx->hits[i].focus & BARE__FOCUS_ABLE) { to = ctx->hits[i].id; break; }
    }
    if (to != ctx->focus_id) {
        ctx->in_count = 0;   /* typed before the click: for the old field, which is gone */
        ctx->in_text_len = 0;
        ctx->in_take = 0;
    }
    bare__focus_set(ctx, to, 1);
}

void bare__focus_free(BareContext *ctx) {
    bare__free(&ctx->alloc, ctx->in, sizeof *ctx->in * (size_t)ctx->in_cap);
    bare__free(&ctx->alloc, ctx->in_text, (size_t)ctx->in_text_cap);
    bare__free(&ctx->alloc, ctx->binds, sizeof *ctx->binds * (size_t)ctx->bind_cap);
    bare__free(&ctx->alloc, ctx->fields, sizeof *ctx->fields * (size_t)ctx->field_cap);
    bare__free(&ctx->alloc, ctx->clip_own, ctx->clip_own_size);
}

/* ---- clipboard ------------------------------------------------------------- */

void bare_ctx_set_clipboard(BareContext *ctx, const char *(*get)(void *user), void (*set)(void *user, const char *text),
                            void *user) {
    ctx->clip_get = get;
    ctx->clip_set = set;
    ctx->clip_user = user;
}

const char *bare__clipboard_get(BareContext *ctx) {
    if (ctx->clip_get) {
        const char *s = ctx->clip_get(ctx->clip_user);
        return s ? s : "";
    }
    return ctx->clip_own ? ctx->clip_own : "";
}

void bare__clipboard_set(BareContext *ctx, const char *text) {
    if (!text) text = "";
    if (ctx->clip_set) { ctx->clip_set(ctx->clip_user, text); return; }
    size_t n = strlen(text) + 1;
    char *s = bare__alloc(&ctx->alloc, n);
    if (!s) return;
    memcpy(s, text, n);
    bare__free(&ctx->alloc, ctx->clip_own, ctx->clip_own_size);
    ctx->clip_own = s;
    ctx->clip_own_size = n;
}

/* ---- public ---------------------------------------------------------------- */

void bare_ctx_set_mac_keys(BareContext *ctx, bool mac) {
    ctx->mac_keys = mac;
    ctx->bind_count = 0;   /* the platform's defaults reload (bare_edit_bind changes are dropped) */
}

BareTextInputKind bare_ctx_text_input(const BareContext *ctx, BareRect *caret) {
    if (!ctx) return BARE_TEXT_INPUT_NONE;
    if (caret) *caret = ctx->ti_rect;
    return ctx->ti_kind;
}

void bare_focus(BareId id) {
    BareContext *ctx = bare_ctx_current();
    if (ctx) bare__focus_set(ctx, id, 3);
}

BareId bare_focused(void) {
    BareContext *ctx = bare_ctx_current();
    return ctx ? ctx->focus_id : 0;
}
