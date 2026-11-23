/* layout.c: flexbox (ARCHITECTURE §6).
 *
 * Elements are in pre-order, so a backwards loop visits children before their
 * parents (bottom-up passes) and a forwards loop parents first (top-down).
 * Every pass is written once for an axis (0 = x, 1 = y):
 *
 *   1. fit x       bottom-up   preferred and minimum content widths
 *   2. resolve x   top-down    final widths: fixed, percent, grow, shrink, stretch
 *   3. (text wraps at its final width here: M5)
 *   4. fit y       bottom-up
 *   5. resolve y   top-down
 *   6. position    top-down    padding, gap, justify, align
 *
 * Sizes: > 0 fixed pixels; BARE_FIT (0) content; BARE_GROW share of the free
 * space by .grow (like CSS flex: 1, basis 0, never below its content's
 * minimum); BARE_PERCENT(p) of the parent's content box; BARE_ZERO a real 0. */
#include "internal.h"
#include <math.h>

typedef enum { FIT, FIXED, GROW, PERCENT } SizeKind;

static float spec(const BareBox *b, int a) { return a ? b->height : b->width; }

static SizeKind kind(const BareBox *b, int a) {
    float v = spec(b, a);
    if (v > 0) return FIXED;
    if (v == 0) return signbit(v) ? FIXED : FIT;   /* BARE_ZERO: a fixed 0 */
    if (v == BARE_GROW) return GROW;
    if (v <= -2.0f) return PERCENT;
    return FIT;
}

static float percent(const BareBox *b, int a) {
    float p = -(spec(b, a) + 2.0f);
    return p < 0 ? 0 : p;
}

static float min_size(const BareBox *b, int a) { return a ? b->min_height : b->min_width; }
static float max_size(const BareBox *b, int a) { return a ? b->max_height : b->max_width; }
static float pad_lo(const BareBox *b, int a) { return a ? b->pad.top : b->pad.left; }
static float pad_hi(const BareBox *b, int a) { return a ? b->pad.bottom : b->pad.right; }
static float pads(const BareBox *b, int a) { return pad_lo(b, a) + pad_hi(b, a); }

/* The axis children are laid out along; -1 for stacks (layers). */
static int main_axis(const BareBox *b) {
    return b->dir == BARE_ROW ? 0 : b->dir == BARE_COLUMN ? 1 : -1;
}

static float clamp_size(float v, const BareBox *b, int a) {
    float hi = max_size(b, a), lo = min_size(b, a);
    if (hi > 0 && v > hi) v = hi;
    if (v < lo) v = lo;
    return v < 0 ? 0 : v;
}

static float *size_of(BareElem *e, int a) { return a ? &e->rect.h : &e->rect.w; }
static float *pos_of(BareElem *e, int a) { return a ? &e->rect.y : &e->rect.x; }

/* ---- 1/4: content sizes, bottom-up --------------------------------------- */

static void fit_pass(BareContext *ctx, int a) {
    BareElem *el = ctx->elems;
    for (int i = ctx->elem_count - 1; i >= 0; i--) {
        BareElem *e = &el[i];
        const BareBox *b = &e->box;
        bool along = main_axis(b) == a;
        float pref = 0, minc = 0;
        for (int c = e->first_child; c >= 0; c = el[c].next_sibling) {
            float cp = el[c].fit[a], cm = el[c].minc[a];
            if (along) { pref += cp; minc += cm; }
            else { pref = BARE__MAX(pref, cp); minc = BARE__MAX(minc, cm); }
        }
        if (along && e->child_count > 1) {
            float gaps = b->gap * (float)(e->child_count - 1);
            pref += gaps;
            minc += gaps;
        }
        pref += pads(b, a);
        minc += pads(b, a);
        if (kind(b, a) == FIXED) pref = minc = spec(b, a) > 0 ? spec(b, a) : 0;
        e->fit[a] = clamp_size(pref, b, a);
        e->minc[a] = BARE__MIN(clamp_size(minc, b, a), e->fit[a]);
    }
}

/* ---- 2/5: final sizes, top-down ------------------------------------------ */

/* A child's size across its parent's main axis (or in a stack). */
static float cross_size(const BareElem *parent, const BareElem *c, int a, float inner) {
    const BareBox *b = &c->box;
    switch (kind(b, a)) {
    case FIXED: return clamp_size(spec(b, a), b, a);
    case PERCENT: return clamp_size(inner * percent(b, a), b, a);
    case GROW: return clamp_size(inner, b, a);
    default:
        if (parent->box.align == BARE_STRETCH) return clamp_size(inner, b, a);
        return c->fit[a];
    }
}

/* Distributes `inner` along the main axis among the children of `e`. */
static void flex(BareContext *ctx, BareElem *e, int a, float inner) {
    BareElem *el = ctx->elems;
    float avail = inner - (e->child_count > 1 ? e->box.gap * (float)(e->child_count - 1) : 0);

    /* Base sizes; GROW children start at nothing and take a share of what's left. */
    float used = 0, weights = 0;
    for (int c = e->first_child; c >= 0; c = el[c].next_sibling) {
        BareElem *k = &el[c];
        const BareBox *b = &k->box;
        float s;
        switch (kind(b, a)) {
        case FIXED: s = clamp_size(spec(b, a), b, a); break;
        case PERCENT: s = clamp_size(inner * percent(b, a), b, a); break;
        case GROW: s = 0; weights += b->grow; break;
        default: s = k->fit[a]; break;
        }
        *size_of(k, a) = s;
        used += s;
    }

    /* Grow: split the free space by weight. A child that would go below its
     * content minimum or above its max is frozen there, and the rest is
     * re-split among the others (as CSS does). */
    if (weights > 0) {
        float free = avail - used;
        for (int c = e->first_child; c >= 0; c = el[c].next_sibling) el[c].frozen = false;
        for (int round = 0; round <= e->child_count; round++) {
            float w = 0, left = free;
            for (int c = e->first_child; c >= 0; c = el[c].next_sibling) {
                if (kind(&el[c].box, a) != GROW) continue;
                if (el[c].frozen) left -= *size_of(&el[c], a);
                else w += el[c].box.grow;
            }
            if (w <= 0) break;
            float share = BARE__MAX(left, 0.0f) / w;
            bool froze = false;
            for (int c = e->first_child; c >= 0; c = el[c].next_sibling) {
                BareElem *k = &el[c];
                if (kind(&k->box, a) != GROW || k->frozen) continue;
                float want = share * k->box.grow;
                float lo = BARE__MAX(k->minc[a], min_size(&k->box, a));
                float hi = max_size(&k->box, a);
                float got = want;
                if (got < lo) got = lo;
                if (hi > 0 && got > hi) got = hi;
                *size_of(k, a) = got;
                if (got != want) { k->frozen = true; froze = true; }
            }
            if (!froze) break;
        }
        used = 0;
        for (int c = e->first_child; c >= 0; c = el[c].next_sibling) used += *size_of(&el[c], a);
    }

    /* Shrink: when the children overflow, take the excess back from the
     * content-sized ones in proportion to .shrink x size, never below their
     * content minimum. Fixed and percent sizes don't shrink (as in CSS with
     * an explicit size). */
    float over = used - avail;
    for (int round = 0; over > 0.01f && round < 16; round++) {
        float total = 0;
        for (int c = e->first_child; c >= 0; c = el[c].next_sibling) {
            BareElem *k = &el[c];
            SizeKind sk = kind(&k->box, a);
            if (sk != FIT && sk != GROW) continue;
            float floor = BARE__MAX(k->minc[a], min_size(&k->box, a));
            if (*size_of(k, a) > floor) total += k->box.shrink * *size_of(k, a);
        }
        if (total <= 0) break;
        float took = 0;
        for (int c = e->first_child; c >= 0; c = el[c].next_sibling) {
            BareElem *k = &el[c];
            SizeKind sk = kind(&k->box, a);
            if (sk != FIT && sk != GROW) continue;
            float s = *size_of(k, a);
            float floor = BARE__MAX(k->minc[a], min_size(&k->box, a));
            if (s <= floor) continue;
            float cut = over * (k->box.shrink * s) / total;
            if (s - cut < floor) cut = s - floor;
            *size_of(k, a) = s - cut;
            took += cut;
        }
        over -= took;
        if (took <= 0) break;
    }
}

static void resolve_pass(BareContext *ctx, int a) {
    BareElem *el = ctx->elems;
    for (int i = 0; i < ctx->elem_count; i++) {
        BareElem *e = &el[i];
        if (e->first_child < 0) continue;
        float inner = BARE__MAX(*size_of(e, a) - pads(&e->box, a), 0.0f);
        if (main_axis(&e->box) == a) {
            flex(ctx, e, a, inner);
        } else {
            for (int c = e->first_child; c >= 0; c = el[c].next_sibling)
                *size_of(&el[c], a) = cross_size(e, &el[c], a, inner);
        }
    }
}

/* ---- 6: positions, top-down ---------------------------------------------- */

static float align_t(BareAlign al) {
    return al == BARE_CENTER ? 0.5f : al == BARE_END ? 1.0f : 0.0f;
}

static void position_pass(BareContext *ctx) {
    BareElem *el = ctx->elems;
    for (int i = 0; i < ctx->elem_count; i++) {
        BareElem *e = &el[i];
        if (e->first_child < 0) continue;
        const BareBox *b = &e->box;
        float origin[2] = { e->rect.x + b->pad.left, e->rect.y + b->pad.top };
        float inner[2] = { BARE__MAX(e->rect.w - pads(b, 0), 0.0f), BARE__MAX(e->rect.h - pads(b, 1), 0.0f) };
        int m = main_axis(b);

        if (m < 0) {   /* stack: every layer placed by .align on both axes */
            for (int c = e->first_child; c >= 0; c = el[c].next_sibling) {
                for (int a = 0; a < 2; a++)
                    *pos_of(&el[c], a) = origin[a] + (inner[a] - *size_of(&el[c], a)) * align_t(b->align);
            }
            continue;
        }

        int x = 1 - m;   /* cross axis */
        float total = 0;
        int n = e->child_count;
        for (int c = e->first_child; c >= 0; c = el[c].next_sibling) total += *size_of(&el[c], m);
        if (n > 1) total += b->gap * (float)(n - 1);
        float free = inner[m] - total;

        float lead = 0, between = 0;
        if (free > 0) {
            switch (b->justify) {
            case BARE_CENTER: lead = free / 2; break;
            case BARE_END: lead = free; break;
            case BARE_SPACE_BETWEEN: if (n > 1) between = free / (float)(n - 1); else lead = 0; break;
            case BARE_SPACE_AROUND: between = free / (float)n; lead = between / 2; break;
            case BARE_SPACE_EVENLY: between = free / (float)(n + 1); lead = between; break;
            default: break;
            }
        }

        float cursor = origin[m] + lead;
        for (int c = e->first_child; c >= 0; c = el[c].next_sibling) {
            BareElem *k = &el[c];
            *pos_of(k, m) = cursor;
            cursor += *size_of(k, m) + b->gap + between;
            *pos_of(k, x) = origin[x] + (inner[x] - *size_of(k, x)) * align_t(b->align);
        }
    }
}

void bare__layout(BareContext *ctx) {
    if (ctx->elem_count == 0) return;
    BareElem *root = &ctx->elems[0];
    fit_pass(ctx, 0);
    root->rect = (BareRect){ 0, 0, ctx->width, ctx->height };
    resolve_pass(ctx, 0);
    /* 3: text wraps here once it exists (M5): its height depends on its width. */
    fit_pass(ctx, 1);
    resolve_pass(ctx, 1);
    position_pass(ctx);
}
