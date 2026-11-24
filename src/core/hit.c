/* hit.c: the hit list and hit testing (ARCHITECTURE §8).
 *
 * At the end of each frame every element is recorded as it was drawn (after
 * offset and scale), in paint order. Input arriving before the next frame is
 * tested against that list, so a click lands on what the user actually saw. */
#include "internal.h"
#include <math.h>

BareRect bare__visual_rect(const BareElem *e) {
    BareRect r = { e->rect.x * e->vscale + e->vt.x, e->rect.y * e->vscale + e->vt.y,
                   e->rect.w * e->vscale, e->rect.h * e->vscale };
    return r;
}

/* What a clipping box clips its children to: inside its border, as CSS
 * clips overflow to the padding box. Visual, logical pixels. */
BareRect bare__clip_rect(const BareElem *e) {
    BareRect r = bare__visual_rect(e);
    float b = BARE__MIN(e->box.border * e->vscale, BARE__MIN(r.w, r.h) * 0.5f);
    r.x += b; r.y += b; r.w -= 2 * b; r.h -= 2 * b;
    return r;
}

static BareRect intersect(BareRect a, BareRect b) {
    float x0 = fmaxf(a.x, b.x), y0 = fmaxf(a.y, b.y);
    float x1 = fminf(a.x + a.w, b.x + b.w), y1 = fminf(a.y + a.h, b.y + b.h);
    BareRect r = { x0, y0, fmaxf(x1 - x0, 0.0f), fmaxf(y1 - y0, 0.0f) };
    return r;
}

void bare__hits_build(BareContext *ctx) {
    ctx->hit_count = 0;
    if (!bare__grow(&ctx->alloc, (void **)&ctx->hits, &ctx->hit_cap, ctx->elem_count, sizeof *ctx->hits)) {
        bare__log(ctx, "out of memory building the hit list");
        return;
    }
    const BareRect everything = { -1e30f, -1e30f, 2e30f, 2e30f };
    for (int i = 0; i < ctx->elem_count; i++) {
        const BareElem *e = &ctx->elems[i];
        BareHit *h = &ctx->hits[i];
        h->id = e->id;
        h->parent = e->parent;
        h->node = e->node;
        h->rect = bare__visual_rect(e);
        h->radius = BARE__MIN(e->box.radius * e->vscale, BARE__MIN(h->rect.w, h->rect.h) * 0.5f);
        h->passthrough = e->box.passthrough;
        h->scroll = (uint8_t)e->box.scroll;
        h->bar = e->overlay;
        h->focus = e->focus;
        /* Children of a clipping or scrolling box can't be hit outside it (inside its border). */
        if (e->parent < 0) {
            h->clip = everything;
        } else {
            const BareHit *p = &ctx->hits[e->parent];
            const BareElem *pe = &ctx->elems[e->parent];
            h->clip = (pe->box.clip || pe->box.scroll) ? intersect(p->clip, bare__clip_rect(pe)) : p->clip;
        }
    }
    ctx->hit_count = ctx->elem_count;
}

static bool inside(BareRect r, BareVec2 p) {
    return p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h;
}

/* Inside the rounded rect: a point in a corner square must be within the radius. */
static bool inside_rounded(BareRect r, float radius, BareVec2 p) {
    if (!inside(r, p)) return false;
    if (radius <= 0) return true;
    float hw = r.w * 0.5f, hh = r.h * 0.5f;
    float qx = fabsf(p.x - (r.x + hw)) - (hw - radius);
    float qy = fabsf(p.y - (r.y + hh)) - (hh - radius);
    if (qx <= 0 || qy <= 0) return true;
    return qx * qx + qy * qy <= radius * radius;
}

int bare__hit_find(const BareContext *ctx, BareVec2 p) {
    for (int i = ctx->hit_count - 1; i >= 0; i--) {
        const BareHit *h = &ctx->hits[i];
        if (h->passthrough || !inside(h->clip, p)) continue;
        if (inside_rounded(h->rect, h->radius, p)) return i;
    }
    return -1;
}

void bare__hit_chain(const BareContext *ctx, int index, BareChain *out) {
    out->count = 0;
    for (int i = index; i >= 0 && out->count < BARE__MAX_DEPTH + 1; i = ctx->hits[i].parent)
        out->ids[out->count++] = ctx->hits[i].id;
}

bool bare__chain_has(const BareChain *c, BareId id) {
    for (int i = 0; i < c->count; i++)
        if (c->ids[i] == id) return true;
    return false;
}
