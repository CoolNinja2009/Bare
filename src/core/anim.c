/* anim.c: animation channels (ARCHITECTURE §7).
 *
 * Runs after layout, top-down. For every element the layout rect and the
 * resolved looks are the *targets*; each animated element keeps a set of
 * channels that move toward them with its .animate (and .animate_layout for
 * position and size), and the moved values are written back into the element
 * before transforms, hit testing and emit see it.
 *
 * Elements that never asked for animation have no channels: they're drawn at
 * their targets, relative to their parent's animated position so a moving
 * parent still carries them. */
#include "internal.h"
#include <math.h>
#include <string.h>

_Static_assert(BARE__CH_COUNT <= 64, "one active bit per channel");

#define MAX_DT (1.0f / 20.0f)   /* a stall can't throw anything across the screen */

/* Settling thresholds per channel, in its own units: a channel closer than this
 * to its target and slower than SETTLE_SPEED of them per second snaps there. */
#define SETTLE_SPEED 10.0f

static float settle_eps(int c) {
    if (c >= BARE__CH_COLOR && c < BARE__CH_BORDER) return 0.25f;   /* 0..255 */
    if (c >= BARE__CH_GLASS_TINT && c < BARE__CH_GLASS) return 0.25f;
    if (c == BARE__CH_OPACITY || c == BARE__CH_GLASS || c == BARE__CH_REFRACTION || c == BARE__CH_DISPERSION ||
        c == BARE__CH_SHINE)
        return 0.001f;
    if (c == BARE__CH_SCALE || c == BARE__CH_ZOOM) return 0.0001f;
    return 0.01f;                                                  /* pixels */
}

/* 0 = default, BARE_ZERO = instant (ARCHITECTURE §1). */
static BareAnim resolve_spec(BareAnim a) {
    switch (a.kind) {
    case BARE_ANIM_EASE:
        if (a.duration == 0.0f) {
            if (signbit(a.duration)) { a.kind = BARE_ANIM_NONE; break; }
            a.duration = 0.25f;   /* CSS's default transition-duration is 0; 0.25 s is the usual pick */
        }
        if (a.duration < 0) { a.kind = BARE_ANIM_NONE; break; }
        if (a.bezier[0] == 0 && a.bezier[1] == 0 && a.bezier[2] == 0 && a.bezier[3] == 0) {
            a.bezier[0] = 0.25f; a.bezier[1] = 0.1f; a.bezier[2] = 0.25f; a.bezier[3] = 1.0f;   /* CSS `ease` */
        }
        a.bezier[0] = fminf(fmaxf(a.bezier[0], 0.0f), 1.0f);   /* CSS requires x in [0, 1] */
        a.bezier[2] = fminf(fmaxf(a.bezier[2], 0.0f), 1.0f);
        break;
    case BARE_ANIM_SPRING:
        if (a.response == 0.0f) {
            if (signbit(a.response)) { a.kind = BARE_ANIM_NONE; break; }
            a.response = 0.35f;
        }
        if (a.response < 0) { a.kind = BARE_ANIM_NONE; break; }
        if (a.damping == 0.0f) a.damping = 1.0f;
        a.damping = fmaxf(a.damping, 0.02f);   /* undamped would ring forever */
        break;
    default:
        a.kind = BARE_ANIM_NONE;
        break;
    }
    return a;
}

/* ---- the pool of channel sets -------------------------------------------- */

static BareAnimSet *set_of(BareContext *ctx, const BareNode *n) {
    return n->anim ? &ctx->anims[n->anim - 1] : NULL;
}

static uint32_t set_new(BareContext *ctx) {
    int i;
    if (ctx->anim_free >= 0) {
        i = ctx->anim_free;
        ctx->anim_free = ctx->anims[i].next_free;
    } else {
        if (!bare__grow(&ctx->alloc, (void **)&ctx->anims, &ctx->anim_cap, ctx->anim_count + 1, sizeof *ctx->anims))
            return 0;
        i = ctx->anim_count++;
    }
    memset(&ctx->anims[i], 0, sizeof ctx->anims[i]);
    ctx->anims[i].next_free = -1;
    return (uint32_t)i + 1;
}

void bare__anim_release(BareContext *ctx, uint32_t anim) {
    if (!anim) return;
    int i = (int)anim - 1;
    ctx->anims[i].next_free = ctx->anim_free;
    ctx->anim_free = i;
}

void bare__anim_free(BareContext *ctx) {
    bare__free(&ctx->alloc, ctx->anims, (size_t)ctx->anim_cap * sizeof *ctx->anims);
    ctx->anims = NULL;
    ctx->anim_count = ctx->anim_cap = 0;
    ctx->anim_free = -1;
}

/* ---- one channel ---------------------------------------------------------- */

/* Moves a channel toward its target; returns true while it's still moving. */
static bool step(BareChannel *ch, const BareAnim *a, float dt, float eps) {
    if (a->kind == BARE_ANIM_EASE) {
        ch->t += dt / a->duration;
        if (ch->t >= 1.0f - 1e-4f) {   /* frame times don't add up to exactly 1 */
            ch->value = ch->target;
            ch->velocity = 0;
            return false;
        }
        float v = ch->from + (ch->target - ch->from) * bare__bezier(a->bezier, ch->t);
        if (dt > 0) ch->velocity = (v - ch->value) / dt;   /* kept so a spring can take over smoothly */
        ch->value = v;
        return true;
    }
    float x = ch->value - ch->target;
    bare__spring_step(&x, &ch->velocity, a->response, a->damping, dt);
    if (fabsf(x) < eps && fabsf(ch->velocity) < eps * SETTLE_SPEED) {
        ch->value = ch->target;
        ch->velocity = 0;
        return false;
    }
    ch->value = ch->target + x;
    return true;
}

/* ---- targets and results --------------------------------------------------- */

static void put_color(float *t, BareColor c) { t[0] = c.r; t[1] = c.g; t[2] = c.b; t[3] = c.a; }

static uint8_t byte(float v) {
    v = roundf(v);
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static BareColor get_color(const float *v) {
    BareColor c = { byte(v[0]), byte(v[1]), byte(v[2]), byte(v[3]) };
    return c;
}

/* The element's targets, relative to its parent's target rect. */
static void targets_of(const BareElem *e, const BareElem *p, float *t) {
    const BareBox *b = &e->box;
    t[BARE__CH_X] = e->target.x - (p ? p->target.x : 0);
    t[BARE__CH_Y] = e->target.y - (p ? p->target.y : 0);
    t[BARE__CH_W] = e->target.w;
    t[BARE__CH_H] = e->target.h;
    put_color(t + BARE__CH_COLOR, b->color);
    put_color(t + BARE__CH_BORDER_COLOR, b->border_color);
    put_color(t + BARE__CH_TEXT_COLOR, b->text_color);
    put_color(t + BARE__CH_SHADOW_COLOR, b->shadow.color);
    t[BARE__CH_BORDER] = b->border;
    t[BARE__CH_RADIUS] = b->radius;
    t[BARE__CH_OPACITY] = b->opacity;
    t[BARE__CH_SCALE] = b->scale;
    t[BARE__CH_OFFSET_X] = b->offset.x;
    t[BARE__CH_OFFSET_Y] = b->offset.y;
    t[BARE__CH_SHADOW_X] = b->shadow.x;
    t[BARE__CH_SHADOW_Y] = b->shadow.y;
    t[BARE__CH_SHADOW_BLUR] = b->shadow.blur;
    t[BARE__CH_SHADOW_SPREAD] = b->shadow.spread;
    const BareGlass *g = &b->glass;   /* all 0 when the element has no glass */
    put_color(t + BARE__CH_GLASS_TINT, g->tint);
    t[BARE__CH_GLASS] = e->glass ? 1.0f : 0.0f;
    t[BARE__CH_REFRACTION] = g->refraction;
    t[BARE__CH_DEPTH] = g->depth;
    t[BARE__CH_GLASS_BLUR] = g->blur;
    t[BARE__CH_DISPERSION] = g->dispersion;
    t[BARE__CH_SHINE] = g->shine;
    t[BARE__CH_ZOOM] = e->glass ? g->zoom : 1.0f;
    t[BARE__CH_LIGHT] = g->light;
}

/* Glass coming and going: it condenses out of the backdrop and dissolves back
 * into it. Going away, its bending, frost, fringes and tint fade out while
 * its shape and light stay put; coming from nothing, it starts with the new
 * shape and light and no bending yet. */
static void glass_ends(BareAnimSet *s, float *t) {
    BareChannel *ch = s->ch;
    static const int keep[] = { BARE__CH_DEPTH, BARE__CH_SHINE, BARE__CH_LIGHT,
                                BARE__CH_GLASS_TINT, BARE__CH_GLASS_TINT + 1, BARE__CH_GLASS_TINT + 2 };
    const int n = (int)(sizeof keep / sizeof keep[0]);
    if (t[BARE__CH_GLASS] == 0) {
        for (int i = 0; i < n; i++) t[keep[i]] = ch[keep[i]].target;
    } else if (ch[BARE__CH_GLASS].value < 0.001f) {
        for (int i = 0; i < n; i++) ch[keep[i]].value = ch[keep[i]].from = ch[keep[i]].target = t[keep[i]];
    }
}

/* Fading to or from transparent changes only the alpha, as CSS's premultiplied
 * interpolation does: red fading out stays red instead of passing through
 * dark red. `last` is the last channel the colour governs (a shadow's colour
 * also governs its geometry). */
static void transparent_ends(BareAnimSet *s, float *t, int first, int last) {
    BareChannel *ch = s->ch;
    if (t[first + 3] == 0) {
        for (int c = first; c < first + 3; c++) t[c] = ch[c].target;
        for (int c = BARE__CH_SHADOW_X; first == BARE__CH_SHADOW_COLOR && c <= last; c++) t[c] = ch[c].target;
    } else if (ch[first + 3].value < 0.5f) {
        /* Invisible now: start from the new colour (and shape) rather than the old one. */
        for (int c = first; c < first + 3; c++) ch[c].value = ch[c].from = ch[c].target = t[c];
        for (int c = BARE__CH_SHADOW_X; first == BARE__CH_SHADOW_COLOR && c <= last; c++)
            ch[c].value = ch[c].from = ch[c].target = t[c];
    }
}

/* Writes animated values into the element (layout rect stays in e->target). */
static void apply(BareElem *e, const BareElem *p, const float *v) {
    BareBox *b = &e->box;
    e->rect.x = (p ? p->rect.x : 0) + v[BARE__CH_X];
    e->rect.y = (p ? p->rect.y : 0) + v[BARE__CH_Y];
    e->rect.w = fmaxf(v[BARE__CH_W], 0.0f);
    e->rect.h = fmaxf(v[BARE__CH_H], 0.0f);
    b->color = get_color(v + BARE__CH_COLOR);
    b->border_color = get_color(v + BARE__CH_BORDER_COLOR);
    b->text_color = get_color(v + BARE__CH_TEXT_COLOR);
    b->shadow.color = get_color(v + BARE__CH_SHADOW_COLOR);
    b->border = fmaxf(v[BARE__CH_BORDER], 0.0f);
    b->radius = fmaxf(v[BARE__CH_RADIUS], 0.0f);
    b->opacity = fminf(fmaxf(v[BARE__CH_OPACITY], 0.0f), 1.0f);   /* springs overshoot */
    b->scale = fmaxf(v[BARE__CH_SCALE], 0.0f);
    b->offset.x = v[BARE__CH_OFFSET_X];
    b->offset.y = v[BARE__CH_OFFSET_Y];
    b->shadow.x = v[BARE__CH_SHADOW_X];
    b->shadow.y = v[BARE__CH_SHADOW_Y];
    b->shadow.blur = fmaxf(v[BARE__CH_SHADOW_BLUR], 0.0f);
    b->shadow.spread = v[BARE__CH_SHADOW_SPREAD];
    /* Glass is drawn while any of it is left; merge isn't animated. */
    BareGlass *g = &b->glass;
    e->glass_amount = fminf(fmaxf(v[BARE__CH_GLASS], 0.0f), 1.0f);
    e->glass = e->glass_amount > 0.001f;
    g->tint = get_color(v + BARE__CH_GLASS_TINT);
    g->refraction = v[BARE__CH_REFRACTION];
    g->depth = fmaxf(v[BARE__CH_DEPTH], 0.0f);
    g->blur = fmaxf(v[BARE__CH_GLASS_BLUR], 0.0f);
    g->dispersion = fmaxf(v[BARE__CH_DISPERSION], 0.0f);
    g->shine = fmaxf(v[BARE__CH_SHINE], 0.0f);
    g->zoom = fmaxf(v[BARE__CH_ZOOM], 0.01f);
    g->light = v[BARE__CH_LIGHT];
}

/* Steps one element's channels; returns its active bits. */
static uint64_t animate_set(BareAnimSet *s, BareElem *e, const BareElem *p, float dt) {
    float t[BARE__CH_COUNT];
    targets_of(e, p, t);
    BareChannel *ch = s->ch;
    BareId parent_id = p ? p->id : 0;

    if (!s->ready) {   /* first appearance: no flying in from 0 */
        for (int c = 0; c < BARE__CH_COUNT; c++) {
            ch[c].value = ch[c].target = ch[c].from = t[c];
            ch[c].velocity = 0;
            ch[c].t = 1;
        }
        s->active = 0;
        s->ready = true;
        s->parent = parent_id;
    }

    /* Moved to another parent: keep its place on screen, now relative to the new one. */
    uint64_t force = 0;
    if (s->parent != parent_id) {
        float nx = s->abs.x - (p ? p->rect.x : 0), ny = s->abs.y - (p ? p->rect.y : 0);
        ch[BARE__CH_X].from += nx - ch[BARE__CH_X].value;
        ch[BARE__CH_Y].from += ny - ch[BARE__CH_Y].value;
        ch[BARE__CH_X].value = nx;
        ch[BARE__CH_Y].value = ny;
        force = (1ull << BARE__CH_X) | (1ull << BARE__CH_Y);
        s->parent = parent_id;
    }

    transparent_ends(s, t, BARE__CH_COLOR, BARE__CH_COLOR + 3);
    transparent_ends(s, t, BARE__CH_BORDER_COLOR, BARE__CH_BORDER_COLOR + 3);
    transparent_ends(s, t, BARE__CH_TEXT_COLOR, BARE__CH_TEXT_COLOR + 3);
    transparent_ends(s, t, BARE__CH_SHADOW_COLOR, BARE__CH_SHADOW_SPREAD);
    glass_ends(s, t);

    BareAnim look = resolve_spec(e->box.animate);
    BareAnim lay = resolve_spec(e->box.animate_layout);
    if (lay.kind == BARE_ANIM_NONE && e->box.animate_layout.kind == BARE_ANIM_NONE) lay = look;

    float v[BARE__CH_COUNT];
    for (int c = 0; c < BARE__CH_COUNT; c++) {
        const BareAnim *a = c <= BARE__CH_H ? &lay : &look;
        BareChannel *k = &ch[c];
        uint64_t bit = 1ull << c;
        if (a->kind == BARE_ANIM_NONE) {
            k->value = k->target = k->from = t[c];
            k->velocity = 0;
            s->active &= ~bit;
        } else {
            if (t[c] != k->target || (force & bit)) {   /* retarget: from wherever it is now */
                k->target = t[c];
                k->from = k->value;
                k->t = 0;
                if (k->value != k->target) s->active |= bit;
            }
            if ((s->active & bit) && !step(k, a, dt, settle_eps(c))) s->active &= ~bit;
        }
        v[c] = k->value;
    }
    apply(e, p, v);
    s->abs = (BareVec2){ e->rect.x, e->rect.y };
    return s->active;
}

/* How far to move this frame. Coming out of idle the clock has run on while
 * nothing moved, so the first step is one typical frame, not the whole sleep. */
static float frame_dt(BareContext *ctx) {
    float raw = ctx->anim_started ? (float)(ctx->time - ctx->anim_time) : 0.0f;
    ctx->anim_time = ctx->time;
    ctx->anim_started = true;
    if (!(raw > 0)) return 0;
    if (ctx->anim_frame <= 0) ctx->anim_frame = 1.0f / 60.0f;
    if (!ctx->anim_active) return fminf(raw, ctx->anim_frame);
    if (raw < MAX_DT) ctx->anim_frame = raw;
    return fminf(raw, MAX_DT);
}

void bare__animate(BareContext *ctx) {
    float dt = frame_dt(ctx);
    bool any = false;
    const uint64_t layout_bits = (1ull << BARE__CH_X) | (1ull << BARE__CH_Y) | (1ull << BARE__CH_W) |
                                 (1ull << BARE__CH_H);
    for (int i = 0; i < ctx->elem_count; i++) {
        BareElem *e = &ctx->elems[i];
        BareElem *p = e->parent >= 0 ? &ctx->elems[e->parent] : NULL;
        e->target = e->rect;
        BareNode *n = e->node >= 0 ? &ctx->store.nodes[e->node] : NULL;
        if (n && !n->anim && (e->box.animate.kind != BARE_ANIM_NONE || e->box.animate_layout.kind != BARE_ANIM_NONE))
            n->anim = set_new(ctx);   /* 0 when out of memory: then it just doesn't animate */

        BareAnimSet *s = n ? set_of(ctx, n) : NULL;
        uint64_t active = 0;
        if (s) {
            active = animate_set(s, e, p, dt);
        } else if (p) {   /* carried along by its parent */
            e->rect.x = p->rect.x + (e->target.x - p->target.x);
            e->rect.y = p->rect.y + (e->target.y - p->target.y);
        }
        e->moving = (active & layout_bits) || (p && p->moving);
        e->opacity = e->box.opacity * (p ? p->opacity : 1.0f);
        if (active) any = true;
    }
    ctx->anim_active = any;
}
