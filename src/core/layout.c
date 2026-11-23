/* layout.c: M1 stand-in layout. Fixed sizes and content-fitting containers
 * that stack children along .dir with padding and gap. BARE_GROW and
 * BARE_PERCENT behave like BARE_FIT for now. M2 replaces this file with the
 * flexbox passes in ARCHITECTURE §6. */
#include "internal.h"

static float clampf(float v, float lo, float hi) {
    if (hi > 0 && v > hi) v = hi;
    if (v < lo) v = lo;
    return v;
}

void bare__layout(BareContext *ctx) {
    BareElem *el = ctx->elems;
    int n = ctx->elem_count;

    /* Bottom-up: content sizes. Pre-order means children come after parents. */
    for (int i = n - 1; i >= 0; i--) {
        BareElem *e = &el[i];
        const BareBox *b = &e->box;
        bool row = b->dir == BARE_ROW, stack = b->dir == BARE_STACK;
        float main = 0, cross = 0;
        for (int c = e->first_child; c >= 0; c = el[c].next_sibling) {
            float cw = el[c].fit_w, ch = el[c].fit_h;
            float cm = row ? cw : ch;
            if (stack) main = BARE__MAX(main, cm);
            else main += cm;
            float cr = row ? ch : cw;
            if (cr > cross) cross = cr;
        }
        if (e->child_count > 1 && !stack) main += b->gap * (float)(e->child_count - 1);
        float content_w = (row ? main : cross) + b->pad.left + b->pad.right;
        float content_h = (row ? cross : main) + b->pad.top + b->pad.bottom;
        e->fit_w = clampf(b->width > 0 ? b->width : content_w, b->min_width, b->max_width);
        e->fit_h = clampf(b->height > 0 ? b->height : content_h, b->min_height, b->max_height);
    }

    /* Top-down: positions. */
    if (n == 0) return;
    el[0].rect = (BareRect){ 0, 0, ctx->width, ctx->height };
    for (int i = 0; i < n; i++) {
        BareElem *e = &el[i];
        const BareBox *b = &e->box;
        bool row = b->dir == BARE_ROW;
        float x = e->rect.x + b->pad.left, y = e->rect.y + b->pad.top;
        float inner_w = e->rect.w - b->pad.left - b->pad.right;
        float inner_h = e->rect.h - b->pad.top - b->pad.bottom;
        for (int c = e->first_child; c >= 0; c = el[c].next_sibling) {
            BareElem *k = &el[c];
            k->rect.w = k->fit_w;
            k->rect.h = k->fit_h;
            float free_cross = row ? inner_h - k->rect.h : inner_w - k->rect.w;
            float off = b->align == BARE_CENTER ? free_cross / 2 : b->align == BARE_END ? free_cross : 0;
            if (b->dir == BARE_STACK) {
                /* Layers share the content box; .align places them on both axes. */
                float fx = inner_w - k->rect.w, fy = inner_h - k->rect.h;
                float t = b->align == BARE_CENTER ? 0.5f : b->align == BARE_END ? 1.0f : 0.0f;
                k->rect.x = x + fx * t;
                k->rect.y = y + fy * t;
            } else if (row) {
                k->rect.x = x;
                k->rect.y = y + off;
                x += k->rect.w + b->gap;
            } else {
                k->rect.x = x + off;
                k->rect.y = y;
                y += k->rect.h + b->gap;
            }
        }
    }
}
