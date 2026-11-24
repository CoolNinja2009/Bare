/* scroll.c: scroll containers (ARCHITECTURE §6, §8).
 *
 * A scroll box's offset is retained state on its node: wheel and scrollbar
 * drags move the target (`scroll_to`), and the drawn offset follows it on a
 * critically damped spring stepped at the *start* of each frame, so the app
 * (BareState.scroll, bare_visible) and layout see the value this frame draws.
 * Layout doesn't move children by it: it is applied as a translation of the
 * children (layout.c, bare__transforms), snapped to whole physical pixels so
 * scrolled text stays sharp, and so children with .animate_layout don't chase
 * the scroll on their springs.
 *
 * Scrollbars are two elements Bare adds to a scroll box when it closes, after
 * its children (so they're drawn and hit on top): a rail (the thumb's hit
 * area, placed by layout outside the flex flow) holding the visible bar, which
 * fades and widens through the animation system like any element. */
#include "internal.h"
#include <math.h>

#define SPRING_RESPONSE 0.15f   /* seconds: a wheel notch glides for about this long */
#define MAX_DT          (1.0f / 20.0f)
#define SHOW_TIME       1.0     /* seconds a scrollbar stays after scrolling or a mouse move */
#define BAR_THIN        6.0f
#define BAR_WIDE        10.0f   /* while hovered or dragged */
#define BAR_MIN         24.0f   /* shortest thumb */

bool bare__scrolls(uint8_t scroll, int axis) {
    return axis == 0 ? (scroll == BARE_SCROLL_X || scroll == BARE_SCROLL_BOTH)
                     : (scroll == BARE_SCROLL_Y || scroll == BARE_SCROLL_BOTH);
}

static float *axis_of(BareVec2 *v, int a) { return a ? &v->y : &v->x; }
static float get(BareVec2 v, int a) { return a ? v.y : v.x; }

static float max_scroll(const BareNode *n, int a) {
    return fmaxf(get(n->content, a) - get(n->view, a), 0.0f);
}

void bare__want_frame_at(BareContext *ctx, double time) {
    if (ctx->wake_at < 0 || time < ctx->wake_at) ctx->wake_at = time;
}

double bare_ctx_wake_time(const BareContext *ctx) { return ctx ? ctx->wake_at : -1.0; }

/* ---- stepping ------------------------------------------------------------- */

/* Coming out of idle the clock has run on while nothing moved: the first step
 * is one typical frame, not the whole sleep (as in anim.c). */
static float step_dt(BareContext *ctx) {
    float raw = ctx->scroll_started ? (float)(ctx->time - ctx->scroll_time) : 0.0f;
    ctx->scroll_time = ctx->time;
    ctx->scroll_started = true;
    if (!(raw > 0)) return 0;
    if (ctx->scroll_frame <= 0) ctx->scroll_frame = 1.0f / 60.0f;
    if (!ctx->scroll_active) return fminf(raw, ctx->scroll_frame);
    if (raw < MAX_DT) ctx->scroll_frame = raw;
    return fminf(raw, MAX_DT);
}

/* Returns true while the axis is still moving. */
static bool step_axis(float *x, float *v, float to, float dt) {
    float d = *x - to;
    bare__spring_step(&d, v, SPRING_RESPONSE, 1.0f, dt);
    if (fabsf(d) < 0.05f && fabsf(*v) < 1.0f) {
        *x = to;
        *v = 0;
        return false;
    }
    *x = to + d;
    return true;
}

void bare__scroll_begin(BareContext *ctx) {
    ctx->wake_at = -1;
    float dt = step_dt(ctx);
    if (!ctx->scroll_active) return;
    bool any = false;
    for (int i = 0; i < ctx->store.node_count; i++) {
        BareNode *n = &ctx->store.nodes[i];
        if (!n->id || !n->scroll_flags) continue;
        n->scroll_seen = ctx->time;
        n->scroll_flags &= (uint8_t)~BARE__SCROLL_POKED;
        bool mx = step_axis(&n->scroll.x, &n->scroll_v.x, n->scroll_to.x, dt);
        bool my = step_axis(&n->scroll.y, &n->scroll_v.y, n->scroll_to.y, dt);
        if (mx || my) any = true;
        else n->scroll_flags &= (uint8_t)~BARE__SCROLL_MOVING;
    }
    ctx->scroll_active = any;
}

static void poke(BareContext *ctx, BareNode *n) {
    n->scroll_flags |= BARE__SCROLL_MOVING | BARE__SCROLL_POKED;
    ctx->scroll_active = true;
}

/* ---- input ---------------------------------------------------------------- */

/* Each axis goes to the innermost scroll box under the mouse that can still
 * move that way; one at its end hands the wheel to the next one out (as
 * browsers chain scrolling). A notch moves the target and the offset glides
 * after it; a touchpad's pixels move both, so the content follows the fingers. */
void bare__scroll_wheel(BareContext *ctx, float dx, float dy, bool precise) {
    if (!ctx->mouse_in) return;
    if (ctx->key_down[BARE_KEY_SHIFT] && dx == 0) {   /* Shift + wheel scrolls sideways */
        dx = -dy;
        dy = 0;
    }
    float k = precise ? 1.0f : BARE_WHEEL_NOTCH;
    float d[2] = { dx * k, -dy * k };   /* + = toward the end of the content */
    int top = bare__hit_find(ctx, ctx->mouse);
    for (int a = 0; a < 2; a++) {
        if (d[a] == 0) continue;
        for (int i = top; i >= 0; i = ctx->hits[i].parent) {
            const BareHit *h = &ctx->hits[i];
            if (!bare__scrolls(h->scroll, a) || h->node < 0) continue;
            BareNode *n = &ctx->store.nodes[h->node];
            float max = max_scroll(n, a);
            float *to = axis_of(&n->scroll_to, a);
            float nt = fminf(fmaxf(*to + d[a], 0.0f), max);
            if (nt == *to) continue;   /* at its end that way: hand off */
            if (precise) {
                float *v = axis_of(&n->scroll, a);
                *v = fminf(fmaxf(*v + (nt - *to), 0.0f), max);
            }
            *to = nt;
            poke(ctx, n);
            break;
        }
    }
}

/* Scrolls every scroll box around a hit (from the inside out) just enough
 * that the hit shows, as focusing with Tab does in a browser. */
void bare__scroll_into_view(BareContext *ctx, int hit) {
    if (hit < 0 || hit >= ctx->hit_count) return;
    BareRect r = ctx->hits[hit].rect;
    for (int i = ctx->hits[hit].parent; i >= 0; i = ctx->hits[i].parent) {
        const BareHit *h = &ctx->hits[i];
        if (!h->scroll || h->node < 0) continue;
        BareNode *n = &ctx->store.nodes[h->node];
        float d[2] = { 0, 0 };
        for (int a = 0; a < 2; a++) {
            if (!bare__scrolls(h->scroll, a)) continue;
            float lo = a ? h->rect.y : h->rect.x, len = a ? h->rect.h : h->rect.w;
            float s = a ? r.y : r.x, e = s + (a ? r.h : r.w);
            if (e - s > len || s < lo) d[a] = s - lo;
            else if (e > lo + len) d[a] = e - (lo + len);
            float *to = axis_of(&n->scroll_to, a);
            float nt = fminf(fmaxf(*to + d[a], 0.0f), max_scroll(n, a));
            d[a] = nt - *to;
            *to = nt;
        }
        if (d[0] != 0 || d[1] != 0) poke(ctx, n);
        r.x -= d[0];   /* where it will be inside the next box out */
        r.y -= d[1];
    }
}

/* The thumb of one axis, from the box's start along it: the track is the box
 * inset at both ends (and short of the other bar's corner). */
void bare__scroll_thumb(float view, float content, float scroll, float other, float *pos, float *len) {
    float track = view - 2 * BARE__BAR_INSET - other;
    *pos = *len = 0;
    if (content <= view + 0.5f || track <= 0) return;
    float l = fmaxf(track * view / content, fminf(BAR_MIN, track));
    float t = fminf(fmaxf(scroll / (content - view), 0.0f), 1.0f);
    *pos = BARE__BAR_INSET + (track - l) * t;
    *len = l;
}

/* Grabbing a thumb: remember where along it, so dragging keeps that point
 * under the mouse. */
void bare__scroll_press(BareContext *ctx, int hit, BareVec2 p) {
    const BareHit *rail = &ctx->hits[hit];
    if (!rail->bar || rail->parent < 0) return;
    const BareHit *box = &ctx->hits[rail->parent];
    if (box->node < 0) return;
    const BareNode *n = &ctx->store.nodes[box->node];
    int a = rail->bar - 1;
    float k = get(n->view, a) > 0 ? (a ? box->rect.h / n->view.y : box->rect.w / n->view.x) : 1.0f;
    if (!(k > 0)) return;
    ctx->drag_id = box->id;
    ctx->drag_axis = a;
    ctx->drag_grab = ((a ? p.y - rail->rect.y : p.x - rail->rect.x)) / k;
}

void bare__scroll_drag(BareContext *ctx, BareVec2 p) {
    int ni = bare__store_find(ctx, ctx->drag_id);
    if (ni < 0) { ctx->drag_id = 0; return; }
    BareNode *n = &ctx->store.nodes[ni];
    int a = ctx->drag_axis;
    float view = get(n->view, a), content = get(n->content, a);
    if (view <= 0) return;
    float k = (a ? n->rect.h : n->rect.w) / view;
    if (!(k > 0)) return;
    float other = (n->bars & (1u << (1 - a))) ? BARE__BAR_HIT : 0.0f;
    float pos, len;
    bare__scroll_thumb(view, content, 0, other, &pos, &len);
    float track = view - 2 * BARE__BAR_INSET - other;
    if (len <= 0 || track - len <= 0) return;
    float at = ((a ? p.y - n->rect.y : p.x - n->rect.x)) / k - ctx->drag_grab;   /* thumb start */
    float max = content - view;
    float s = fminf(fmaxf((at - BARE__BAR_INSET) / (track - len), 0.0f), 1.0f) * max;
    *axis_of(&n->scroll_to, a) = s;
    *axis_of(&n->scroll, a) = s;   /* the content follows the hand, no glide */
    *axis_of(&n->scroll_v, a) = 0;
    poke(ctx, n);
}

/* ---- the tree ------------------------------------------------------------- */

/* Pushes an element under `parent` (the innermost open container) without
 * linking it into the sibling chain, so the flex passes never see it. */
static int push_overlay(BareContext *ctx, int parent, const BareBox *box, int overlay) {
    int prev = ctx->elems[parent].last_child;
    int i = bare__tree_push(ctx, box, "bare.scrollbar", 0);
    if (i < 0) return -1;
    BareElem *p = &ctx->elems[parent];
    if (prev >= 0) ctx->elems[prev].next_sibling = -1;
    else p->first_child = -1;
    p->last_child = prev;
    p->child_count--;
    ctx->elems[i].overlay = (uint8_t)overlay;
    return i;
}

/* A closing scroll box gets a scrollbar for each axis that overflowed in the
 * last layout. The rail takes the mouse; the bar inside it is what shows. */
void bare__scroll_close(BareContext *ctx, int index) {
    BareElem *e = &ctx->elems[index];
    if (!e->box.scroll || e->no_bars || e->node < 0 || ctx->depth >= BARE__MAX_DEPTH) return;
    BareNode *n = &ctx->store.nodes[e->node];
    bool hovered = bare__chain_has(&ctx->hover, e->id);
    if (hovered && ctx->mouse_moved) n->scroll_seen = ctx->time;

    for (int a = 0; a < 2; a++) {
        if (!bare__scrolls(e->box.scroll, a) || max_scroll(n, a) <= 0.5f) continue;
        BareId rail_id = bare__hash_u64(bare__hash_str(e->id, "bare.scrollbar"), (uint64_t)a);
        bool held = ctx->drag_id == e->id && ctx->drag_axis == a;
        bool over = bare__chain_has(&ctx->hover, rail_id);
        bool recent = ctx->time - n->scroll_seen < SHOW_TIME;
        bool shown = held || over || recent;
        if (recent && !held && !over) bare__want_frame_at(ctx, n->scroll_seen + SHOW_TIME);

        BareBox rail = { .id = rail_id, .passthrough = !shown, .dir = a ? BARE_ROW : BARE_COLUMN,
                         .justify = BARE_END };
        if (a) rail.pad.right = BARE__BAR_INSET;
        else rail.pad.bottom = BARE__BAR_INSET;
        int r = push_overlay(ctx, index, &rail, a + 1);
        if (r < 0) return;
        ctx->elems[index].bar[a] = r;
        ctx->stack[ctx->depth++] = r;
        float thick = (held || over) ? BAR_WIDE : BAR_THIN;
        BareBox bar = {
            .id = bare__hash_u64(rail_id, 1),
            .width = a ? thick : BARE_GROW,
            .height = a ? BARE_GROW : thick,
            .radius = BAR_WIDE / 2,
            .color = (held || over) ? BARE_RGBA(170, 170, 180, 220) : BARE_RGBA(150, 150, 160, 160),
            .opacity = shown ? 1.0f : BARE_ZERO,
            .passthrough = true,
            .animate = BARE_EASE(0.2f, BARE_EASE_OUT),
        };
        bare__tree_push(ctx, &bar, "bare.scrollbar", 0);
        ctx->depth--;
        e = &ctx->elems[index];   /* pushing may have moved the array */
    }
}

/* Layout, when a scroll box's content size is known and before its children
 * are placed: keep the offset within the content (it shrank, or the box
 * grew) and remember the sizes for the input that arrives before the next
 * frame. */
void bare__scroll_fit(BareContext *ctx, int index) {
    BareElem *e = &ctx->elems[index];
    if (e->node < 0) return;
    BareNode *n = &ctx->store.nodes[e->node];
    n->content = (BareVec2){ e->content[0], e->content[1] };
    n->view = (BareVec2){ e->rect.w, e->rect.h };
    n->bars = (uint8_t)((e->bar[0] ? 1 : 0) | (e->bar[1] ? 2 : 0));
    for (int a = 0; a < 2; a++) {
        float max = bare__scrolls(e->box.scroll, a) ? max_scroll(n, a) : 0.0f;
        float *v = axis_of(&n->scroll, a), *to = axis_of(&n->scroll_to, a);
        float cv = fminf(fmaxf(*v, 0.0f), max), ct = fminf(fmaxf(*to, 0.0f), max);
        if (cv != *v || ct != *to) {
            *v = cv;
            *to = ct;
            *axis_of(&n->scroll_v, a) = 0;
            ctx->scroll_active = true;   /* one more frame: what the app saw this frame was stale */
        }
    }
}

/* The offset children are drawn at, in whole physical pixels. */
float bare__scroll_drawn(const BareContext *ctx, const BareElem *e, int axis) {
    if (e->node < 0 || !bare__scrolls(e->box.scroll, axis)) return 0;
    float v = get(ctx->store.nodes[e->node].scroll, axis);
    return roundf(v * ctx->scale) / ctx->scale;
}

/* ---- bare_visible ----------------------------------------------------------- */

static BareElem *open_container(BareContext *ctx) {
    if (!ctx || !ctx->in_frame || ctx->depth < 1) return NULL;
    return &ctx->elems[ctx->stack[ctx->depth - 1]];
}

int bare_visible_(int count, float size, const char *file, int line) {
    BareContext *ctx = bare_ctx_current();
    BareElem *e = open_container(ctx);
    if (!e) {
        bare__log(ctx, "bare_visible at %s:%d called outside a frame", file, line);
        return 0;
    }
    if (count < 0) count = 0;
    e->virt = false;
    e->virt_last = count;
    int m = e->box.dir == BARE_ROW ? 0 : e->box.dir == BARE_COLUMN ? 1 : -1;
    float stride = size + e->box.gap;
    if (m < 0 || !bare__scrolls(e->box.scroll, m) || e->node < 0 || !(size > 0) || count == 0) return 0;

    const BareNode *n = &ctx->store.nodes[e->node];
    float view = get(n->view, m) > 0 ? get(n->view, m) : (m ? ctx->height : ctx->width);   /* first frame */
    float from = get(n->scroll, m) - n->virt_start;
    int first = (int)floorf(from / stride) - 1;          /* one spare item at each end */
    int last = (int)ceilf((from + view) / stride) + 1;
    if (first > count - 1) first = count - 1;
    if (first < 0) first = 0;
    if (last > count) last = count;
    if (last < first + 1) last = first + 1;

    e->virt = true;
    e->virt_count = count;
    e->virt_stride = stride;
    e->virt_last = last;
    e->lead_at = e->child_count;
    e->lead = (float)first * stride;
    e->tail_at = -1;
    e->tail = 0;
    return first;
}

int bare_visible_more_(int i) {
    BareContext *ctx = bare_ctx_current();
    BareElem *e = open_container(ctx);
    if (!e) return 0;
    if (i < e->virt_last) return 1;
    if (e->virt && e->tail_at < 0) {
        e->tail_at = e->child_count;
        e->tail = (float)(e->virt_count - e->virt_last) * e->virt_stride;
    }
    return 0;
}
