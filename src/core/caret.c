/* caret.c: positions in laid-out text, for carets, selections and clicks
 * (ARCHITECTURE §13).
 *
 * Offsets are byte offsets into the text, always on code point boundaries.
 * Line l covers the bytes from its first glyph's cluster up to the next
 * line's first; a hard break's '\n' and the spaces hanging off a wrapped line
 * belong to the line before. The offset where a wrapped line ends is also the
 * next line's start: `upstream` says which of the two the caret is drawn on
 * (the end of the upper line after End or a click past it, else the start of
 * the lower one), as browsers do with caret affinity. Coordinates are logical
 * pixels from the text element's top left; `box_w` is its width, for
 * alignment. */
#include "internal.h"

static float align_of(BareAlign al) {
    return al == BARE_CENTER ? 0.5f : al == BARE_END ? 1.0f : 0.0f;
}

int bare__text_line_start(const BareText *t, int l) {
    if (l <= 0 || t->line_count == 0) return 0;
    if (l >= t->line_count) return t->len;
    int g = t->lines[l].first;
    return g < t->glyph_count ? (int)t->glyphs[g].cluster : t->len;
}

/* Whether line l ends at a '\n' (a hard break) rather than by wrapping. */
static bool hard_break(const BareText *t, int l) {
    if (l + 1 >= t->line_count) return true;
    int g = t->lines[l].first + t->lines[l].count;
    return g < t->glyph_count && t->str[t->glyphs[g].cluster] == '\n';
}

/* Where the caret goes for End on line l: before its '\n', or (upstream) at
 * the wrap point. */
int bare__text_line_end(const BareText *t, int l, bool *upstream) {
    if (upstream) *upstream = false;
    if (t->line_count == 0) return t->len;
    if (l >= t->line_count - 1) return t->len;
    int g = t->lines[l].first + t->lines[l].count;
    if (hard_break(t, l)) return (int)t->glyphs[g].cluster;
    if (upstream) *upstream = true;
    return bare__text_line_start(t, l + 1);
}

int bare__text_line_of(const BareText *t, int offset, bool upstream) {
    int n = t->line_count;
    if (n <= 1) return 0;
    int lo = 0, hi = n - 1;   /* the last line starting at or before offset */
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (bare__text_line_start(t, mid) <= offset) lo = mid;
        else hi = mid - 1;
    }
    /* Empty lines share their start with nothing, but a run of them can't
     * share one; a wrap point can, with the line above. */
    if (upstream && lo > 0 && offset == bare__text_line_start(t, lo) && !hard_break(t, lo - 1)) lo--;
    return lo;
}

static float line_pen0(const BareText *t, int l, float box_w) {
    return (box_w - t->lines[l].width) * align_of(t->align);
}

/* x of `offset` drawn on line l (its hanging spaces included). */
float bare__text_x_on_line(const BareText *t, int l, int offset, float box_w) {
    if (t->line_count == 0) return 0;
    if (l < 0) l = 0;
    if (l >= t->line_count) l = t->line_count - 1;
    float pen = line_pen0(t, l, box_w);
    int limit = l + 1 < t->line_count ? t->lines[l + 1].first : t->glyph_count;
    for (int g = t->lines[l].first; g < limit; g++) {
        const BareShapedGlyph *sg = &t->glyphs[g];
        if ((int)sg->cluster >= offset || t->str[sg->cluster] == '\n') break;
        pen += sg->advance;
    }
    return pen;
}

/* The caret is as tall as the paragraph's font, on the line's baseline. */
BareRect bare__text_caret(const BareText *t, int offset, bool upstream, float box_w) {
    if (t->line_count == 0) return (BareRect){ 0, 0, 0, t->line_height };
    int l = bare__text_line_of(t, offset, upstream);
    float x = bare__text_x_on_line(t, l, offset, box_w);
    const BareTextLine *line = &t->lines[l];
    BareRect r = { x, line->y + line->asc - t->ascent, 0, t->ascent + t->descent };
    if (r.h <= 0) { r.y = line->y; r.h = line->h; }
    return r;
}

/* The offset nearest to x on line l. */
int bare__text_offset_at(const BareText *t, int l, float x, float box_w, bool *upstream) {
    if (upstream) *upstream = false;
    if (t->line_count == 0) return 0;
    if (l < 0) l = 0;
    if (l >= t->line_count) l = t->line_count - 1;
    float pen = line_pen0(t, l, box_w);
    const BareTextLine *line = &t->lines[l];
    for (int g = line->first; g < line->first + line->count; g++) {
        const BareShapedGlyph *sg = &t->glyphs[g];
        if (x < pen + sg->advance * 0.5f) return (int)sg->cluster;
        pen += sg->advance;
    }
    return bare__text_line_end(t, l, upstream);
}

int bare__text_hit(const BareText *t, float x, float y, float box_w, bool *upstream) {
    return bare__text_offset_at(t, bare__text_line_at_y(t, y), x, box_w, upstream);
}

/* Rects (logical, text-local) covering bytes [a, b), one per line; a
 * selected '\n' shows as a sliver past the line's end. Returns how many were
 * written (at most `cap`). */
int bare__text_range_rects(const BareText *t, int a, int b, float box_w, BareRect *out, int cap) {
    if (a > b) { int s = a; a = b; b = s; }
    int n = 0;
    if (a == b || t->line_count == 0) return 0;
    int first = bare__text_line_of(t, a, false), last = bare__text_line_of(t, b, true);
    for (int l = first; l <= last && n < cap; l++) {
        int ls = bare__text_line_start(t, l);
        int le = l + 1 < t->line_count ? bare__text_line_start(t, l + 1) : t->len;
        int s = a > ls ? a : ls, e = b < le ? b : le;
        if (s >= e) continue;
        float x0 = bare__text_x_on_line(t, l, s, box_w);
        float x1 = bare__text_x_on_line(t, l, e, box_w);
        if (b > le - 1 && hard_break(t, l) && l + 1 < t->line_count) x1 += t->size * 0.3f;   /* the '\n' */
        if (x1 - x0 <= 0) continue;
        out[n++] = (BareRect){ x0, t->lines[l].y, x1 - x0, t->lines[l].h };
    }
    return n;
}
