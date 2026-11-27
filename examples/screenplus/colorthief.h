/* colorthief.h: ColorThief's getColor (color-thief 2.3, MMCQ), as the Screen+
 * samples call it on album art: every 10th pixel that is opaque enough and not
 * near white, a 5-bit histogram, median cuts to 5 boxes, and the average colour
 * of the box with the most count x volume. Ported quirks and all (the min/max
 * scan that skips the first pixel's max, the stable sorts, the cut's lookups).
 *
 *     int rgb[3];
 *     if (colorthief_get(pixels, w, h, rgb)) ...   // RGBA8, straight alpha */
#ifndef SCREENPLUS_COLORTHIEF_H
#define SCREENPLUS_COLORTHIEF_H
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define CT_SIGBITS 5
#define CT_RSHIFT (8 - CT_SIGBITS)
#define CT_MAXIT 1000

typedef struct CtBox {
    int r1, r2, g1, g2, b1, b2;
    long count;
    double volume;
} CtBox;

static inline int ct_index(int r, int g, int b) { return (r << (2 * CT_SIGBITS)) + (g << CT_SIGBITS) + b; }

static inline long ct_count(const int *histo, const CtBox *v) {
    long n = 0;
    for (int r = v->r1; r <= v->r2; r++)
        for (int g = v->g1; g <= v->g2; g++)
            for (int b = v->b1; b <= v->b2; b++) n += histo[ct_index(r, g, b)];
    return n;
}
static inline void ct_fill(const int *histo, CtBox *v) {
    v->count = ct_count(histo, v);
    v->volume = (double)(v->r2 - v->r1 + 1) * (v->g2 - v->g1 + 1) * (v->b2 - v->b1 + 1);
}

/* a priority queue as the library keeps one: an array, stably sorted ascending before a peek or pop */
typedef struct CtQueue { CtBox *box; int n, cap; bool by_volume; } CtQueue;
static inline void ct_push(CtQueue *q, CtBox b) {
    if (q->n == q->cap) q->cap = q->cap ? q->cap * 2 : 16, q->box = realloc(q->box, (size_t)q->cap * sizeof *q->box);
    q->box[q->n++] = b;
}
static inline double ct_key(const CtQueue *q, const CtBox *b) { return q->by_volume ? (double)b->count * b->volume : (double)b->count; }
static inline CtBox ct_pop(CtQueue *q) {
    for (int i = 1; i < q->n; i++)   /* stable insertion sort */
        for (int j = i; j > 0 && ct_key(q, &q->box[j - 1]) > ct_key(q, &q->box[j]); j--) {
            CtBox t = q->box[j]; q->box[j] = q->box[j - 1]; q->box[j - 1] = t;
        }
    return q->box[--q->n];
}

/* medianCutApply: 0, 1 or 2 boxes into out */
static inline int ct_cut(const int *histo, const CtBox *v, CtBox out[2]) {
    if (!v->count) return 0;
    int rw = v->r2 - v->r1 + 1, gw = v->g2 - v->g1 + 1, bw = v->b2 - v->b1 + 1;
    int maxw = rw > gw ? rw : gw;
    if (bw > maxw) maxw = bw;
    if (v->count == 1) { out[0] = *v; return 1; }
    int axis = maxw == rw ? 0 : maxw == gw ? 1 : 2;
    double partial[32], lookahead[32];
    bool def[32];
    memset(def, 0, sizeof def);
    double total = 0;
    int lo = axis == 0 ? v->r1 : axis == 1 ? v->g1 : v->b1, hi = axis == 0 ? v->r2 : axis == 1 ? v->g2 : v->b2;
    for (int a = lo; a <= hi; a++) {
        double sum = 0;
        if (axis == 0) { for (int g = v->g1; g <= v->g2; g++) for (int b = v->b1; b <= v->b2; b++) sum += histo[ct_index(a, g, b)]; }
        else if (axis == 1) { for (int r = v->r1; r <= v->r2; r++) for (int b = v->b1; b <= v->b2; b++) sum += histo[ct_index(r, a, b)]; }
        else { for (int r = v->r1; r <= v->r2; r++) for (int g = v->g1; g <= v->g2; g++) sum += histo[ct_index(r, g, a)]; }
        total += sum;
        partial[a] = total, def[a] = true;
    }
    for (int a = 0; a < 32; a++) if (def[a]) lookahead[a] = total - partial[a];
#define CT_TRUTHY(x) ((x) >= 0 && (x) < 32 && def[x] && partial[x] != 0)
    for (int a = lo; a <= hi; a++) {
        if (!(partial[a] > total / 2)) continue;
        CtBox b1 = *v, b2 = *v;
        int left = a - lo, right = hi - a, d;
        if (left <= right) d = (int)fmin(hi - 1, (double)(long)(a + right / 2.0));
        else d = (int)fmax(lo, (double)(long)(a - 1 - left / 2.0));
        while (!CT_TRUTHY(d) && d < 64) d++;
        double c = d >= 0 && d < 32 && def[d] ? lookahead[d] : 0;
        while (c == 0 && CT_TRUTHY(d - 1)) d--, c = lookahead[d];
        if (axis == 0) b1.r2 = d, b2.r1 = d + 1;
        else if (axis == 1) b1.g2 = d, b2.g1 = d + 1;
        else b1.b2 = d, b2.b1 = d + 1;
        ct_fill(histo, &b1);
        ct_fill(histo, &b2);
        out[0] = b1, out[1] = b2;
        return 2;
    }
#undef CT_TRUTHY
    return 0;
}

static inline void ct_iter(const int *histo, CtQueue *q, double target) {
    int ncolors = q->n, niters = 0;
    while (niters < CT_MAXIT) {
        if (ncolors >= target) return;
        if (niters++ > CT_MAXIT) return;
        CtBox v = ct_pop(q);
        if (v.count) {
            CtBox out[2];
            int k = ct_cut(histo, &v, out);
            if (k == 0) return;
            ct_push(q, out[0]);
            if (k == 2) ct_push(q, out[1]), ncolors++;
        } else {
            ct_push(q, v);
            niters++;
        }
    }
}

static inline bool colorthief_get(const unsigned char *px, int w, int h, int rgb[3]) {
    int *histo = calloc(1 << (3 * CT_SIGBITS), sizeof(int));
    if (!histo) return false;
    int rmin = 1000000, rmax = 0, gmin = 1000000, gmax = 0, bmin = 1000000, bmax = 0, n = 0;
    for (int i = 0; i < w * h; i += 10) {
        const unsigned char *p = px + (size_t)i * 4;
        if (p[3] < 125 || (p[0] > 250 && p[1] > 250 && p[2] > 250)) continue;
        int r = p[0] >> CT_RSHIFT, g = p[1] >> CT_RSHIFT, b = p[2] >> CT_RSHIFT;
        histo[ct_index(r, g, b)]++;
        if (r < rmin) rmin = r; else if (r > rmax) rmax = r;
        if (g < gmin) gmin = g; else if (g > gmax) gmax = g;
        if (b < bmin) bmin = b; else if (b > bmax) bmax = b;
        n++;
    }
    if (!n) { free(histo); return false; }
    CtBox v = { rmin, rmax, gmin, gmax, bmin, bmax, 0, 0 };
    ct_fill(histo, &v);
    CtQueue pq = { 0 }, pq2 = { 0 };
    pq2.by_volume = true;
    ct_push(&pq, v);
    ct_iter(histo, &pq, 0.75 * 5);
    while (pq.n) ct_push(&pq2, ct_pop(&pq));
    ct_iter(histo, &pq2, 5);
    bool ok = pq2.n > 0;
    if (ok) {
        CtBox best = ct_pop(&pq2);   /* the CMap's first entry: the largest count x volume */
        double s = 0, rs = 0, gs = 0, bs = 0, mult = 1 << CT_RSHIFT;
        for (int r = best.r1; r <= best.r2; r++)
            for (int g = best.g1; g <= best.g2; g++)
                for (int b = best.b1; b <= best.b2; b++) {
                    double hv = histo[ct_index(r, g, b)];
                    s += hv, rs += hv * (r + 0.5) * mult, gs += hv * (g + 0.5) * mult, bs += hv * (b + 0.5) * mult;
                }
        if (s) rgb[0] = (int)(rs / s), rgb[1] = (int)(gs / s), rgb[2] = (int)(bs / s);
        else {
            rgb[0] = (int)(mult * (best.r1 + best.r2 + 1) / 2);
            rgb[1] = (int)(mult * (best.g1 + best.g2 + 1) / 2);
            rgb[2] = (int)(mult * (best.b1 + best.b2 + 1) / 2);
        }
    }
    free(pq.box), free(pq2.box), free(histo);
    return ok;
}
#endif
