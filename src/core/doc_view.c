/* doc_view.c: bare_editor, the view of a document (ARCHITECTURE §8).
 *
 * The editor is a box holding an optional find bar and a scroll box, the
 * text area. Only the lines on screen become elements: the content box is as
 * tall as every line together and the visible lines sit in it after a
 * padding that stands for the ones above. The gutter is inside the scroll
 * box too, moved right by the horizontal scroll so it stays put, drawn over
 * the text that scrolls under it.
 *
 * With soft wrap a line takes several rows: each line's row count is
 * measured when it is shown and estimated (from its length) until then, and
 * prefix sums of the counts place lines and find the line at a row. Mouse
 * input is matched against this frame's scroll and layout per line (each
 * line is laid out by the same function the commands use), and keys replay
 * from the focus queue as in text fields. Selections, carets, matches and
 * whitespace are drawn by the line texts' decorations (field.c). */
#include "doc.h"
#include "edit.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLINK      0.53
#define BLINK_STOP 10.0
#define PAD_X      10.0f      /* text from the gutter, when the style has no padding */
#define PAD_Y      6.0f       /* above the first line, likewise */
#define HL_CAP     256        /* highlight spans per line */
#define CACHE      128        /* line layouts kept per frame */
#define MATCH_MAX  100000
#define HL_BUDGET  200000     /* lines highlighted per frame while catching up */
#define BRACKET_LINES 3000    /* how far bracket matching looks */

typedef struct Cached { int line; uint64_t version; BareText *t; } Cached;

typedef struct View {
    BareContext      *ctx;
    BareDoc          *d;
    BareDocView      *v;
    const BareEditor *in;
    BareTextStyle     st;           /* a line's style, resolved */
    float             lh, charw;
    float             width;        /* wrap width; huge without wrapping */
    bool              wrap;
    float             gutter, text_x;   /* gutter width; where text starts in the content */
    BareEdges         pad;          /* the style's padding: insets the text, not the scroll box (CSS textarea) */
    uint32_t          key;          /* what the rows were measured for */
    Cached            cache[CACHE]; int cache_n, cache_next;
    BareHighlight    *hl;
    BareSpanIn       *spans;
    bool              behind;       /* highlighting hasn't caught up: draw again */
} View;

static BareTextPos pos(int line, int col) { return (BareTextPos){ line, col }; }
static BareTextPos lo_of(const BareSel *s) { return bare__pos_cmp(s->anchor, s->caret) <= 0 ? s->anchor : s->caret; }
static BareTextPos hi_of(const BareSel *s) { return bare__pos_cmp(s->anchor, s->caret) <= 0 ? s->caret : s->anchor; }
static BareColor or_default(BareColor c, BareColor d) { return c.a ? c : d; }
static BareColor alpha(BareColor c, int a) { c.a = (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a); return c; }

/* ---- highlighting and line layout ---------------------------------------------- */

static bool highlighting(const View *w) { return w->in->highlight != NULL; }

/* The highlighter's state at the start of `line`, running it down from the
 * last line known to be right. */
static bool hl_state(View *w, int line, int *state) {
    BareDoc *d = w->d;
    if (d->hl_fn != w->in->highlight || d->hl_user != w->in->highlight_user) {
        d->hl_fn = w->in->highlight;
        d->hl_user = w->in->highlight_user;
        d->hl_valid = 0;
    }
    int budget = HL_BUDGET;
    while (d->hl_valid < line) {
        if (budget-- <= 0) { w->behind = true; *state = 0; return false; }
        int l = d->hl_valid, s = l > 0 ? d->lines[l - 1].hl_end : 0, len;
        const char *str = bare_doc_line(d, l, &len);
        w->in->highlight(w->in->highlight_user, str, len, &s, w->hl, HL_CAP);
        d->lines[l].hl_end = s;
        d->hl_valid = l + 1;
    }
    *state = line > 0 ? d->lines[line - 1].hl_end : 0;
    return true;
}

static BareTextStyle styled(const BareTextStyle *base, const BareHighlight *h) {
    BareTextStyle s = *base;
    if (h->color.a) s.color = h->color;
    s.background = h->background;
    if (h->weight) s.weight = h->weight;
    s.italic = h->italic;
    s.underline = h->underline;
    s.strike = h->strike;
    return s;
}

/* A line as shown: shaped with its highlights and wrapped. */
static BareText *shape(View *w, int line, const char *s, int n, bool highlight) {
    BareContext *ctx = w->ctx;
    int count = 0;
    if (highlight && highlighting(w)) {
        int state;
        bool known = hl_state(w, line, &state);
        int k = w->in->highlight(w->in->highlight_user, s, n, &state, w->hl, HL_CAP);
        if (known && line == w->d->hl_valid) {
            w->d->lines[line].hl_end = state;
            w->d->hl_valid = line + 1;
        }
        if (k > HL_CAP) k = HL_CAP;
        int at = 0, used = 0;
        w->spans[count++] = (BareSpanIn){ 0, w->st };
        for (int i = 0; i < k; i++) {
            int a = w->hl[i].start, b = w->hl[i].end;
            if (a < at) a = at;
            if (b > n) b = n;
            if (b <= a) continue;
            BareSpanIn sp = { a, styled(&w->st, &w->hl[i]) };
            if (w->spans[count - 1].start == a) w->spans[count - 1] = sp;
            else w->spans[count++] = sp;
            if (b < n) w->spans[count++] = (BareSpanIn){ b, w->st };
            at = b;
            used++;
        }
        if (!used) count = 0;
    }
    BareText *t = count ? bare__text_runs(ctx, s, n, &w->st, w->spans, count) : bare__text_new(ctx, s, n, &w->st);
    if (t) bare__text_wrap(ctx, t, w->width);
    return t;
}

static BareText *layout(void *user, int line) {
    View *w = user;
    if (line < 0 || line >= w->d->count) return NULL;
    for (int i = 0; i < w->cache_n; i++)
        if (w->cache[i].line == line && w->cache[i].version == w->d->version) return w->cache[i].t;
    int len;
    const char *s = bare_doc_line(w->d, line, &len);
    BareText *t = shape(w, line, s, len, true);
    Cached *c = &w->cache[w->cache_next];
    w->cache_next = (w->cache_next + 1) % CACHE;
    if (w->cache_n < CACHE) w->cache_n++;
    *c = (Cached){ line, w->d->version, t };
    return t;
}

/* ---- rows -------------------------------------------------------------------------- */

static int est_rows(const View *w, int len) {
    if (!w->wrap || w->width <= 0) return 1;
    int r = (int)ceilf((float)len * w->charw / w->width);
    return r < 1 ? 1 : r > 65535 ? 65535 : r;
}

static void rows_ready(View *w) {
    BareDoc *d = w->d;
    if (!w->wrap) return;
    if (d->rows_dirty || d->rows_key != w->key) {
        for (int i = 0; i < d->count; i++)
            if (d->lines[i].rows_key != w->key) d->lines[i].rows = (uint16_t)est_rows(w, d->lines[i].len);
        d->rows_dirty = true;
    }
    bare__doc_rows(d, w->key);
}

static int row_of(const View *w, int line) { return w->wrap ? w->d->row_sum[line] : line; }
static int total_rows(const View *w) { return w->wrap ? w->d->row_sum[w->d->count] : w->d->count; }
/* A padding side: unset takes the editor's own, BARE_ZERO is a real zero. */
static float side(float v, float fallback) { return v == 0.0f && !signbit(v) ? fallback : fmaxf(v, 0.0f); }

static float line_y(const View *w, int line) { return w->pad.top + (float)row_of(w, line) * w->lh; }

static int line_at_row(const View *w, int row) {
    const BareDoc *d = w->d;
    if (row <= 0) return 0;
    if (!w->wrap) return row < d->count ? row : d->count - 1;
    int lo = 0, hi = d->count - 1;
    while (lo < hi) {   /* the last line starting at or above the row */
        int mid = (lo + hi + 1) / 2;
        if (d->row_sum[mid] <= row) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

/* Measures the lines about to be shown; their rows may differ from the guess. */
static void measure(View *w, int first, int last) {
    BareDoc *d = w->d;
    if (!w->wrap) return;
    for (int i = first; i <= last && i < d->count; i++) {
        if (d->lines[i].rows_key == w->key) continue;
        BareText *t = layout(w, i);
        int rows = t && t->line_count > 0 ? t->line_count : 1;
        if (rows > 65535) rows = 65535;
        if (d->lines[i].rows != rows) d->rows_dirty = true;
        d->lines[i].rows = (uint16_t)rows;
        d->lines[i].rows_key = w->key;
    }
    if (d->rows_dirty) bare__doc_rows(d, w->key);
}

/* ---- positions ----------------------------------------------------------------------- */

static BareTextPos pos_at(View *w, float cx, float cy, bool *up) {
    *up = false;
    int row = (int)floorf((cy - w->pad.top) / w->lh);
    if (row >= total_rows(w)) {
        int last = w->d->count - 1;
        return pos(last, w->d->lines[last].len);
    }
    int line = line_at_row(w, row);
    BareText *t = layout(w, line);
    if (!t) return pos(line, 0);
    int col = bare__text_hit(t, cx - w->text_x, cy - line_y(w, line), w->width, up);
    return pos(line, col);
}

static BareRect caret_rect(View *w, BareTextPos p, bool up) {
    BareText *t = layout(w, p.line);
    BareRect c = t ? bare__text_caret(t, p.col, up, w->width) : (BareRect){ 0, 0, 1, w->lh };
    c.x += w->text_x;
    c.y += line_y(w, p.line);
    return c;
}

/* ---- find ----------------------------------------------------------------------------- */

static void find_update(View *w) {
    BareDoc *d = w->d;
    BareDocView *v = w->v;
    uint64_t h = bare__hash_u64(bare__hash_str(BARE__FNV_OFFSET, v->query), (uint64_t)v->find_flags + 1);
    if (v->find_version == d->version && v->find_hash == h) return;
    v->find_version = d->version;
    v->find_hash = h;
    v->match_count = 0;
    v->matches_capped = false;
    int qn = (int)strlen(v->query);
    if (!qn) return;
    for (int l = 0; l < d->count && !v->matches_capped; l++) {
        int len, at = 0, m;
        const char *s = bare_doc_line(d, l, &len);
        while (bare__doc_find_in_line(s, len, at, v->query, qn, v->find_flags, false, &m)) {
            if (v->match_count >= MATCH_MAX ||
                !bare__grow(&d->alloc, (void **)&v->matches, &v->match_cap, v->match_count + 1, sizeof *v->matches)) {
                v->matches_capped = true;
                break;
            }
            v->matches[v->match_count++] = (BareFindMatch){ l, m, qn };
            at = m + qn;
        }
    }
}

static int match_from(const BareDocView *v, BareTextPos p) {   /* the first match at or after p */
    int lo = 0, hi = v->match_count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (bare__pos_cmp(pos(v->matches[mid].line, v->matches[mid].col), p) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static int match_current(const View *w) {
    const BareDocView *v = w->v;
    const BareSel *m = &w->d->sels[w->d->main];
    BareTextPos a = lo_of(m), b = hi_of(m);
    int i = match_from(v, a);
    if (i < v->match_count && v->matches[i].line == a.line && v->matches[i].col == a.col && b.line == a.line &&
        b.col == a.col + v->matches[i].len)
        return i;
    return -1;
}

static void select_match(View *w, int i) {
    const BareFindMatch *m = &w->v->matches[i];
    bare_doc_select(w->d, pos(m->line, m->col), pos(m->line, m->col + m->len));
    w->v->follow = true;
}

static void find_step(View *w, int dir) {
    find_update(w);
    BareDocView *v = w->v;
    if (!v->match_count) return;
    const BareSel *m = &w->d->sels[w->d->main];
    int i;
    if (dir > 0) {
        i = match_from(v, hi_of(m));
        if (match_current(w) >= 0 && i < v->match_count && i == match_current(w)) i++;
        if (i >= v->match_count) i = 0;
    } else {
        i = match_from(v, lo_of(m)) - 1;
        if (i < 0) i = v->match_count - 1;
    }
    select_match(w, i);
}

static void open_bar(View *w, int which) {
    BareDocView *v = w->v;
    BareDoc *d = w->d;
    v->find_open = which != 3;
    v->replace_open = which == 2;
    v->goto_open = which == 3;
    v->focus_bar = (uint8_t)which;
    if (which == 3) { v->goto_line[0] = 0; return; }
    const BareSel *m = &d->sels[d->main];
    BareTextPos a = lo_of(m), b = hi_of(m);
    if (a.line == b.line && b.col > a.col && b.col - a.col < (int)sizeof v->query) {
        memcpy(v->query, d->lines[a.line].s + a.col, (size_t)(b.col - a.col));
        v->query[b.col - a.col] = 0;
    }
    v->find_from = a;
    v->focus_bar = 1;
}

static void replace_one(View *w) {
    find_update(w);
    int i = match_current(w);
    if (i >= 0) {
        bare_doc_insert(w->d, w->v->replace);
        find_update(w);
    }
    find_step(w, 1);
}

static void replace_all(View *w) {
    find_update(w);
    BareDocView *v = w->v;
    BareDoc *d = w->d;
    if (!v->match_count) return;
    int rn = (int)strlen(v->replace);
    bare__doc_begin(d, BARE__UNDO_OTHER);
    for (int i = v->match_count - 1; i >= 0; i--) {
        const BareFindMatch *m = &v->matches[i];
        bare__doc_delete(d, pos(m->line, m->col), pos(m->line, m->col + m->len));
        if (rn) bare__doc_insert(d, pos(m->line, m->col), v->replace, rn);
    }
    bare__doc_end(d);
}

static void close_bars(View *w) {
    w->v->find_open = w->v->replace_open = w->v->goto_open = false;
    w->v->match_count = 0;
    w->v->find_hash = 0;
}

/* ---- input ------------------------------------------------------------------------------ */

static void set_comp(View *w, const char *s, int n, int cursor) {
    BareDocView *v = w->v;
    BareDoc *d = w->d;
    if (n > 0 && v->comp_len == 0) {   /* a composition replaces the selection */
        bool any = false;
        for (int i = 0; i < d->sel_count; i++) if (bare__pos_cmp(d->sels[i].anchor, d->sels[i].caret)) any = true;
        if (any) bare_doc_insert(d, "");
    }
    if (!bare__grow(&d->alloc, (void **)&v->comp, &v->comp_cap, n + 1, 1)) return;
    if (n > 0) memcpy(v->comp, s, (size_t)n);
    v->comp_len = n;
    v->comp_cursor = cursor < 0 ? 0 : cursor > n ? n : cursor;
}

static void act(View *w, BareDocCmd *c, BareEditAction a, bool extend) {
    BareDocView *v = w->v;
    if (v->comp_len > 0) {
        if (a == BARE_EDIT_BLUR) v->comp_len = 0;
        return;   /* the IME has the keys while composing */
    }
    switch (a) {
    case BARE_EDIT_FIND: open_bar(w, 1); break;
    case BARE_EDIT_REPLACE: open_bar(w, 2); break;
    case BARE_EDIT_GOTO_LINE: open_bar(w, 3); break;
    case BARE_EDIT_FIND_NEXT: case BARE_EDIT_FIND_PREV:
        if (!v->query[0]) { open_bar(w, 1); break; }
        find_step(w, a == BARE_EDIT_FIND_NEXT ? 1 : -1);
        break;
    case BARE_EDIT_OVERWRITE: v->overwrite = !v->overwrite; c->overwrite = v->overwrite; break;
    default:
        bare__doc_run(c, a, extend);
        if (c->blur) {
            c->blur = false;
            if (v->find_open || v->goto_open) close_bars(w);
            else bare__focus_set(w->ctx, 0, 0);
        }
        break;
    }
}

static void keys(View *w, BareDocCmd *c) {
    BareContext *ctx = w->ctx;
    for (int i = 0; i < ctx->in_take; i++) {
        const BareKeyEv *k = &ctx->in[i];
        if (k->kind == BARE__IN_TEXT) {
            w->v->comp_len = 0;
            bare__doc_type(c, ctx->in_text + k->text, k->len);
        } else if (k->kind == BARE__IN_COMPOSE) {
            set_comp(w, ctx->in_text + k->text, k->len, k->cursor);
        } else if (k->kind == BARE__IN_ACTION) {
            act(w, c, (BareEditAction)k->key, false);
        } else {
            bool extend;
            BareEditAction a = bare__edit_lookup(ctx, k->key, k->mods, &extend);
            if (a != BARE_EDIT_NONE) act(w, c, a, extend);
        }
        if (ctx->focus_id != w->v->area) break;   /* Escape, or a bar took focus: the rest isn't ours */
    }
}

static void unit_at(View *w, BareTextPos p, int unit, BareTextPos *a, BareTextPos *b) {
    BareDoc *d = w->d;
    *a = *b = p;
    if (unit == 2) {
        int len, x, y;
        const char *s = bare_doc_line(d, p.line, &len);
        bare__edit_word_at(s, len, p.col, &x, &y);
        *a = pos(p.line, x);
        *b = pos(p.line, y);
    } else if (unit >= 3) {
        *a = pos(p.line, 0);
        *b = p.line + 1 < d->count ? pos(p.line + 1, 0) : pos(p.line, d->lines[p.line].len);
    }
}

static void column_select(View *w, float x0, float y0, float x1, float y1) {
    BareDoc *d = w->d;
    bool up;
    int l0 = pos_at(w, x0, y0, &up).line, l1 = pos_at(w, x1, y1, &up).line;
    int a = l0 < l1 ? l0 : l1, b = l0 < l1 ? l1 : l0;
    if (b - a > 10000) b = a + 10000;
    if (!bare__grow(&d->alloc, (void **)&d->sels, &d->sel_cap, b - a + 1, sizeof *d->sels)) return;
    int n = 0;
    for (int l = a; l <= b; l++) {
        BareText *t = layout(w, l);
        bool u;
        float ly = line_y(w, l) + w->lh * 0.5f - line_y(w, l);
        int c0 = t ? bare__text_hit(t, x0 - w->text_x, ly, w->width, &u) : 0;
        int c1 = t ? bare__text_hit(t, x1 - w->text_x, ly, w->width, &u) : 0;
        memset(&d->sels[n], 0, sizeof d->sels[n]);
        d->sels[n].anchor = pos(l, c0);
        d->sels[n].caret = pos(l, c1);
        if (l == l1) d->main = n;
        n++;
    }
    d->sel_count = n;
}

static void mouse(View *w, BareNode *node, bool was_focused, BareDocCmd *c) {
    BareContext *ctx = w->ctx;
    BareDoc *d = w->d;
    BareDocView *v = w->v;
    BareId area = v->area;
    BareRect r = node->rect;
    if (r.w <= 0 || r.h <= 0) return;
    float cx = ctx->mouse.x - r.x + node->scroll.x, cy = ctx->mouse.y - r.y + node->scroll.y;
    bool mine = bare__chain_has(&ctx->press[BARE__PRESS_LEFT], area);
    if (ctx->press_new && bare__chain_has(&ctx->press_began, area)) {
        float px = ctx->press_pos.x - r.x + node->scroll.x, py = ctx->press_pos.y - r.y + node->scroll.y;
        bool up;
        BareTextPos p = pos_at(w, px, py, &up);
        int clicks = ctx->click_count;
        bool alt = ctx->key_down[BARE_KEY_ALT], shift = ctx->key_down[BARE_KEY_SHIFT];
        d->merge = false;
        v->comp_len = 0;
        for (int i = 0; i < d->sel_count; i++) d->sels[i].has_goal = false;
        if (px - node->scroll.x < w->gutter) {   /* the gutter selects whole lines */
            v->drag = 3;
            unit_at(w, p, 3, &v->drag_a, &v->drag_b);
            bare_doc_select(d, v->drag_a, v->drag_b);
        } else if (alt && clicks <= 1) {   /* another caret; dragging makes it a column */
            if (bare__grow(&d->alloc, (void **)&d->sels, &d->sel_cap, d->sel_count + 1, sizeof *d->sels)) {
                BareSel *s = &d->sels[d->sel_count];
                memset(s, 0, sizeof *s);
                s->anchor = s->caret = p;
                s->upstream = up;
                d->main = d->sel_count++;
                bare__doc_sels_normalize(d);
            }
            v->drag = 5;
            v->drag_x = px;
            v->drag_y = py;
        } else if (shift && clicks <= 1 && was_focused) {
            BareSel m = d->sels[d->main];
            d->sels[0] = m;
            d->sel_count = 1;
            d->main = 0;
            d->sels[0].caret = p;
            d->sels[0].upstream = up;
            v->drag = 1;
        } else {
            v->drag = (uint8_t)(clicks > 3 ? 3 : clicks < 1 ? 1 : clicks);
            unit_at(w, p, v->drag, &v->drag_a, &v->drag_b);
            bare_doc_select(d, v->drag_a, v->drag_b);
            d->sels[0].upstream = v->drag == 1 && up;
        }
        c->moved = true;
    } else if (v->drag && ctx->held[BARE__PRESS_LEFT] && mine) {
        bool up;
        BareTextPos p = pos_at(w, cx, cy, &up);
        BareSel *m = &d->sels[d->main];
        if (v->drag == 5 && (fabsf(cx - v->drag_x) > 4 || fabsf(cy - v->drag_y) > w->lh * 0.5f)) v->drag = 4;
        if (v->drag == 4) {
            column_select(w, v->drag_x, v->drag_y, cx, cy);
        } else if (v->drag == 1) {
            m->caret = p;
            m->upstream = up;
        } else if (v->drag == 2 || v->drag == 3) {
            BareTextPos a, b;
            unit_at(w, p, v->drag, &a, &b);
            if (bare__pos_cmp(p, v->drag_a) < 0) { m->anchor = v->drag_b; m->caret = a; }
            else { m->anchor = v->drag_a; m->caret = bare__pos_cmp(b, v->drag_b) > 0 ? b : v->drag_b; }
        }
        if (v->drag != 5) {
            c->moved = true;
            bare__want_frame_at(ctx, ctx->time + 1.0 / 60.0);   /* held past the edge, it keeps scrolling */
        }
    }
    if (!ctx->held[BARE__PRESS_LEFT]) {
        if (v->drag) bare__doc_sels_normalize(d);
        v->drag = 0;
    }
}

/* ---- scrolling to the caret --------------------------------------------------------------- */

static void follow(View *w, BareNode *node) {
    if (!node || node->view.y <= 0) return;
    const BareSel *m = &w->d->sels[w->d->main];
    BareRect c = caret_rect(w, m->caret, m->upstream);
    float vw = node->view.x, vh = node->view.y;
    float sx = node->scroll.x, sy = node->scroll.y;
    if (c.y - w->lh * 0.5f < sy) sy = c.y - w->lh * 0.5f;
    if (c.y + c.h + w->lh * 0.5f > sy + vh) sy = c.y + c.h + w->lh * 0.5f - vh;
    if (!w->wrap) {
        float left = sx + w->text_x, right = sx + vw - w->charw * 2;
        if (c.x < left) sx = c.x - w->text_x - w->charw * 4;
        if (c.x > right) sx = c.x - vw + w->charw * 6;
    }
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    node->scroll = node->scroll_to = (BareVec2){ sx, sy };
    node->scroll_v = (BareVec2){ 0, 0 };
}

/* ---- brackets ------------------------------------------------------------------------------- */

static bool bracket_match(const BareDoc *d, BareTextPos at, BareTextPos *a, BareTextPos *b) {
    static const char open[] = "([{", close[] = ")]}";
    for (int side = 0; side < 2; side++) {   /* the bracket before the caret, then after it */
        int col = at.col - 1 + side, len;
        const char *s = bare_doc_line(d, at.line, &len);
        if (col < 0 || col >= len) continue;
        const char *o = strchr(open, s[col]), *c = strchr(close, s[col]);
        if (!s[col] || (!o && !c)) continue;
        char me = s[col], other = o ? close[o - open] : open[c - close];
        int dir = o ? 1 : -1, depth = 0;
        int l = at.line, k = col;
        for (int lines = 0; lines < BRACKET_LINES;) {
            const char *t = bare_doc_line(d, l, &len);
            for (; k >= 0 && k < len; k += dir) {
                if (t[k] == me) depth++;
                else if (t[k] == other && --depth == 0) {
                    *a = pos(at.line, col);
                    *b = pos(l, k);
                    return true;
                }
            }
            l += dir;
            lines++;
            if (l < 0 || l >= d->count) break;
            k = dir > 0 ? 0 : d->lines[l].len - 1;
        }
    }
    return false;
}

/* ---- drawing ------------------------------------------------------------------------------- */

/* printf into the frame arena (texts keep a pointer to their string until drawn) */
static const char *fmt(BareContext *ctx, const char *f, ...) {
    char tmp[64];
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(tmp, sizeof tmp, f, ap);
    va_end(ap);
    if (n < 0) return "";
    if (n >= (int)sizeof tmp) n = (int)sizeof tmp - 1;
    char *s = bare__arena_alloc(&ctx->frame, (size_t)n + 1, 1);
    if (!s) return "";
    memcpy(s, tmp, (size_t)n + 1);
    return s;
}

static int *ints(BareContext *ctx, int n) { return bare__arena_alloc(&ctx->frame, sizeof(int) * (size_t)(n > 0 ? n : 1), 8); }

/* What line `l` draws with its text: carets, selections, matches. */
static BareTextDeco *deco_for(View *w, int l, int len, bool focused, bool caret_on, const BareTextPos br[2], bool has_br,
                              int comp_at) {
    BareContext *ctx = w->ctx;
    BareDoc *d = w->d;
    BareDocView *v = w->v;
    const BareEditor *in = w->in;
    BareTextDeco *dc = bare__arena_alloc(&ctx->frame, sizeof *dc, 8);
    if (!dc) return NULL;
    memset(dc, 0, sizeof *dc);
    BareColor sel = or_default(in->selection, ctx->theme.selection);
    if (!focused) sel = alpha(sel, sel.a * 2 / 3);
    dc->sel_color = sel;
    dc->caret_color = or_default(in->caret, ctx->theme.accent);
    dc->mark_color = alpha(ctx->theme.accent, 70);
    dc->whitespace = in->whitespace;
    dc->whitespace_color = alpha(ctx->theme.muted, 110);
    int nc = 0, ns = 0;
    int *carets = ints(ctx, d->sel_count), *sels = ints(ctx, d->sel_count * 2);
    if (!carets || !sels) return dc;
    int shift = comp_at >= 0 ? v->comp_len : 0;
    for (int i = 0; i < d->sel_count; i++) {
        const BareSel *s = &d->sels[i];
        BareTextPos a = lo_of(s), b = hi_of(s);
        if (b.line < l) continue;
        if (a.line > l) break;
        if (bare__pos_cmp(a, b)) {
            sels[ns * 2] = a.line < l ? 0 : a.col;
            sels[ns * 2 + 1] = b.line > l ? len + 1 : b.col;   /* past the end: the line break is selected */
            ns++;
        }
        if (s->caret.line == l) {
            if (i == d->main) {
                dc->caret = s->caret.col;
                dc->upstream = s->upstream;
            } else {
                carets[nc++] = s->caret.col + (comp_at >= 0 && s->caret.col > comp_at ? shift : 0);
            }
        }
    }
    bool main_here = d->sels[d->main].caret.line == l;
    dc->caret_on = caret_on && main_here;
    if (!main_here) dc->caret = -1;
    dc->carets = carets;
    dc->caret_count = caret_on ? nc : 0;
    dc->sels = sels;
    dc->sel_count = ns;
    if (comp_at >= 0) {
        dc->comp_a = comp_at;
        dc->comp_b = comp_at + v->comp_len;
        dc->caret = comp_at + v->comp_cursor;
        dc->sel_count = 0;
    }
    /* find matches and the bracket pair, outlined */
    int nm = 0, cap = 8;
    int *marks = ints(ctx, cap * 2);
    if (marks && (v->find_open && v->match_count)) {
        for (int i = match_from(v, pos(l, 0)); i < v->match_count && v->matches[i].line == l; i++) {
            if (nm == cap) {
                int *more = ints(ctx, cap * 4);
                if (!more) break;
                memcpy(more, marks, sizeof(int) * (size_t)nm * 2);
                marks = more;
                cap *= 2;
            }
            marks[nm * 2] = v->matches[i].col;
            marks[nm * 2 + 1] = v->matches[i].col + v->matches[i].len;
            nm++;
        }
    }
    if (marks && has_br && focused) {
        for (int k = 0; k < 2; k++) {
            if (br[k].line != l) continue;
            if (nm == cap) break;
            marks[nm * 2] = br[k].col;
            marks[nm * 2 + 1] = br[k].col + 1;
            nm++;
        }
    }
    dc->marks = marks;
    dc->mark_count = nm;
    return dc;
}

static void draw_lines(View *w, BareNode *node, bool focused, float vis_h, const char *file, int fline) {
    BareContext *ctx = w->ctx;
    BareDoc *d = w->d;
    BareDocView *v = w->v;
    const BareEditor *in = w->in;
    float sy = node ? node->scroll.y : 0, sx = node ? node->scroll.x : 0;
    int first = line_at_row(w, (int)floorf((sy - w->pad.top) / w->lh));
    int last = line_at_row(w, (int)ceilf((sy + vis_h) / w->lh) + 1);
    measure(w, first, last);
    first = line_at_row(w, (int)floorf((sy - w->pad.top) / w->lh));
    last = line_at_row(w, (int)ceilf((sy + vis_h) / w->lh) + 1);

    /* The caret blinks; every caret blinks together, solid while it moves. */
    double since = ctx->time - v->blink;
    bool on = focused && ctx->window_focused;
    if (on && since < BLINK_STOP) {
        double phase = floor(since / BLINK);
        on = ((long)phase % 2) == 0;
        bare__want_frame_at(ctx, v->blink + (phase + 1) * BLINK);
    }
    BareTextPos br[2];
    const BareSel *m = &d->sels[d->main];
    bool has_br = d->sel_count == 1 && !bare__pos_cmp(m->anchor, m->caret) && bracket_match(d, m->caret, &br[0], &br[1]);

    float total = w->pad.top + (float)total_rows(w) * w->lh + fmaxf(vis_h * 0.5f, w->lh * 2) + w->pad.bottom;
    float content_w = BARE_GROW;
    if (!w->wrap) {
        float longest = (float)d->longest * w->charw;
        content_w = w->text_x + fmaxf(longest, v->max_w) + w->charw * 8;
        if (node && content_w < node->view.x) content_w = node->view.x;
    }
    float y0 = line_y(w, first);
    BareBox col;
    memset(&col, 0, sizeof col);
    col.dir = BARE_COLUMN;
    col.width = content_w;
    col.height = total;
    col.pad = (BareEdges){ y0, w->wrap ? w->pad.right : 0, 0, w->text_x };   /* lines wrap at w->width */
    bare_open_(&col, file, fline);

    /* behind the text: the current line and the ruler */
    if (in->highlight_line && !bare__pos_cmp(m->anchor, m->caret) && d->sel_count == 1) {
        BareText *t = layout(w, m->caret.line);
        int rows = t && t->line_count > 0 ? t->line_count : 1;
        bare_box(.position = BARE_ABSOLUTE, .x = sx, .y = line_y(w, m->caret.line), .width = BARE_GROW,
                 .height = (float)rows * w->lh, .color = alpha(ctx->theme.text, focused ? 12 : 6),
                 .passthrough = true);
    }
    if (in->ruler > 0)
        bare_box(.position = BARE_ABSOLUTE, .x = w->text_x + in->ruler * w->charw, .y = sy, .width = 1,
                 .height = vis_h, .color = alpha(ctx->theme.border, 60), .passthrough = true);

    /* the lines */
    int comp_line = v->comp_len > 0 && focused ? m->caret.line : -1;
    float widest = v->max_w;
    for (int l = first; l <= last && l < d->count; l++) {
        int len;
        const char *s = bare_doc_line(d, l, &len);
        BareText *t;
        int comp_at = -1;
        if (l == comp_line) {   /* the IME's text spliced in at the caret */
            comp_at = m->caret.col;
            char *buf = bare__arena_alloc(&ctx->frame, (size_t)(len + v->comp_len) + 1, 1);
            if (!buf) continue;
            memcpy(buf, s, (size_t)comp_at);
            memcpy(buf + comp_at, v->comp, (size_t)v->comp_len);
            memcpy(buf + comp_at + v->comp_len, s + comp_at, (size_t)(len - comp_at));
            t = shape(w, l, buf, len + v->comp_len, false);
        } else {
            t = layout(w, l);
        }
        BareTextStyle st = w->st;
        st.id = 0;
        int ti = bare__text_push(ctx, "", 0, &st, file, fline);
        if (ti < 0 || !t) continue;
        BareElem *te = &ctx->elems[ti];
        te->text = t;
        if (w->wrap) te->box.width = BARE_GROW;
        else if (t->max_line > widest) widest = t->max_line;
        te->text->deco = deco_for(w, l, comp_at >= 0 ? len + v->comp_len : len, focused, on, br, has_br, comp_at);
    }
    v->max_w = widest;

    /* the gutter, held still against sideways scrolling, over the text under it */
    if (in->line_numbers) {
        BareBox g;
        memset(&g, 0, sizeof g);
        g.position = BARE_ABSOLUTE;
        g.x = sx;
        g.y = 0;
        g.width = w->gutter;
        g.height = total;
        g.dir = BARE_COLUMN;
        g.pad = (BareEdges){ y0, w->charw, 0, 0 };
        g.align = BARE_END;
        g.passthrough = true;
        BareColor bg = ctx->theme.input.color.a ? ctx->theme.input.color : ctx->theme.surface;
        if (in->style) bg = in->style->color;   /* a clear editor has a clear gutter (glass shows through) */
        g.color = bg;
        bare_open_(&g, file, fline);
        for (int l = first; l <= last && l < d->count; l++) {
            BareText *t = layout(w, l);
            int rows = t && t->line_count > 0 ? t->line_count : 1;
            bool cur = l == m->caret.line;
            bare_row(.height = (float)rows * w->lh, .passthrough = true) {
                bare_text(fmt(ctx, "%d", l + 1), .font = w->st.font, .size = w->st.size, .line_height = w->st.line_height,
                           .color = cur ? ctx->theme.text : alpha(ctx->theme.muted, 150), .nowrap = true);
            }
        }
        bare_close_();
    }
    bare_close_();
}

/* ---- the find bar ------------------------------------------------------------------------------ */

static bool small_button(const char *label, bool on, BareId id) {
    const BareTheme *t = bare_theme();
    return bare_button(label, .id = id, .pad = { 4, 9, 4, 9 }, .radius = 6,
                       .color = on ? alpha(t->accent, 90) : BARE_CLEAR).clicked;
}

static void find_bar(View *w, BareId id) {
    BareContext *ctx = w->ctx;
    BareDocView *v = w->v;
    const BareTheme *t = bare_theme();
    v->find_field = bare__hash_str(id, "find");
    v->replace_field = bare__hash_str(id, "replace");
    v->goto_field = bare__hash_str(id, "goto");
    if (v->focus_bar) {
        bare_focus(v->focus_bar == 3 ? v->goto_field : v->focus_bar == 2 && v->query[0] ? v->replace_field
                                                                                         : v->find_field);
        v->focus_bar = 0;
    }
    bool shift = ctx->key_down[BARE_KEY_SHIFT];
    bare_column(.id = bare__hash_str(id, "bar"), .width = BARE_GROW, .gap = 6, .pad = { 6, 8, 6, 8 },
                .border_color = t->border, .color = alpha(t->surface, 160)) {
        if (v->goto_open) {
            bare_row(.gap = 8, .align = BARE_CENTER) {
                const char *hint = fmt(ctx, "Go to line (1 - %d)", w->d->count);
                BareState s = bare_input(v->goto_line, sizeof v->goto_line, .id = v->goto_field, .placeholder = hint,
                                         .width = 220, .font = BARE_MONO, .size = 13);
                if (s.submitted) {
                    int n = atoi(v->goto_line);
                    if (n < 1) n = 1;
                    if (n > w->d->count) n = w->d->count;
                    bare_doc_select(w->d, pos(n - 1, 0), pos(n - 1, 0));
                    close_bars(w);
                    bare_focus(v->area);
                }
                if (small_button("\xC3\x97", false, bare__hash_str(id, "x3"))) { close_bars(w); bare_focus(v->area); }
            }
        } else {
            bare_row(.gap = 6, .align = BARE_CENTER) {
                BareState s = bare_input(v->query, sizeof v->query, .id = v->find_field, .placeholder = "Find",
                                         .width = 240, .font = BARE_MONO, .size = 13);
                if (small_button("Aa", v->find_flags & BARE_FIND_CASE, bare__hash_str(id, "case")))
                    v->find_flags ^= BARE_FIND_CASE;
                if (small_button("W", v->find_flags & BARE_FIND_WORD, bare__hash_str(id, "word")))
                    v->find_flags ^= BARE_FIND_WORD;
                find_update(w);
                if (s.changed) {   /* search as you type, from where the search began */
                    int i = match_from(v, v->find_from);
                    if (v->match_count) select_match(w, i < v->match_count ? i : 0);
                }
                if (s.submitted) find_step(w, shift ? -1 : 1);
                int cur = match_current(w);
                if (!v->query[0]) bare_text("", .size = 13);
                else if (!v->match_count) bare_text("No results", .size = 13, .color = t->muted, .nowrap = true);
                else bare_text(fmt(ctx, "%d of %d%s", cur >= 0 ? cur + 1 : 0, v->match_count,
                                   v->matches_capped ? "+" : ""), .size = 13, .color = t->muted, .nowrap = true);
                bare_box(.width = BARE_GROW);
                if (small_button("Prev", false, bare__hash_str(id, "prev"))) find_step(w, -1);
                if (small_button("Next", false, bare__hash_str(id, "next"))) find_step(w, 1);
                if (small_button("\xC3\x97", false, bare__hash_str(id, "x"))) { close_bars(w); bare_focus(v->area); }
            }
            if (v->replace_open) {
                bare_row(.gap = 6, .align = BARE_CENTER) {
                    BareState r = bare_input(v->replace, sizeof v->replace, .id = v->replace_field,
                                             .placeholder = "Replace", .width = 240, .font = BARE_MONO, .size = 13);
                    if (r.submitted || small_button("Replace", false, bare__hash_str(id, "rep"))) replace_one(w);
                    if (small_button("All", false, bare__hash_str(id, "all"))) replace_all(w);
                }
            }
        }
    }
    if (bare_key_pressed(BARE_KEY_F3)) find_step(w, shift ? -1 : 1);
}

/* ---- the editor ------------------------------------------------------------------------------- */

void bare__doc_view_free(BareDoc *d) {
    BareDocView *v = &d->view;
    bare__free(&d->alloc, v->comp, (size_t)v->comp_cap);
    bare__free(&d->alloc, v->matches, sizeof *v->matches * (size_t)v->match_cap);
}

BareState bare_editor_(BareDoc *doc, const BareEditor *in, const char *file, int line) {
    BareState none;
    memset(&none, 0, sizeof none);
    BareContext *ctx = bare_ctx_current();
    if (!ctx || !ctx->in_frame) {
        bare__log(ctx, "bare_editor at %s:%d called outside a frame", file, line);
        return none;
    }
    if (!doc) {
        bare__log(ctx, "bare_editor at %s:%d needs a document (bare_doc_create / bare_doc_load)", file, line);
        return none;
    }
    BareDocView *v = &doc->view;

    /* the outer box */
    BareBox box;
    memset(&box, 0, sizeof box);
    box.id = in->id;
    box.style = in->style ? in->style : &ctx->theme.input;
    box.width = in->width ? in->width : BARE_GROW;
    box.height = in->height ? in->height : BARE_GROW;
    box.dir = BARE_COLUMN;
    box.padding = BARE_ZERO;
    box.clip = true;
    int oi = bare__tree_push(ctx, &box, file, line);
    if (oi < 0) return none;
    BareId id = ctx->elems[oi].id;
    if (ctx->depth < BARE__MAX_DEPTH) ctx->stack[ctx->depth++] = oi;
    v->area = bare__hash_str(id, "bare.editor.area");
    if (ctx->focus_id == id) bare__focus_set(ctx, v->area, ctx->focus_how);   /* bare_focus / bare_edit by the editor's id */
    int an = bare__store_find(ctx, v->area);
    BareNode *node = an >= 0 ? &ctx->store.nodes[an] : NULL;

    /* the frame's view: style and metrics */
    View *w = bare__arena_alloc(&ctx->frame, sizeof *w, 16);
    if (!w) { bare_close_(); return none; }
    memset(w, 0, sizeof *w);
    w->ctx = ctx;
    w->d = doc;
    w->v = v;
    w->in = in;
    w->hl = bare__arena_alloc(&ctx->frame, sizeof *w->hl * HL_CAP, 8);
    w->spans = bare__arena_alloc(&ctx->frame, sizeof *w->spans * (HL_CAP * 2 + 2), 8);
    if (!w->hl || !w->spans) { bare_close_(); return none; }
    BareTextStyle st;
    memset(&st, 0, sizeof st);
    st.font = !in->font ? BARE_MONO : in->font == BARE_SANS ? 0 : in->font;
    st.size = in->size > 0 ? in->size : 14;
    st.line_height = in->line_height > 0 ? in->line_height : 1.5f;
    st.tab_size = in->tab_size > 0 ? in->tab_size : 4;
    st.color = or_default(in->color, ctx->theme.text);
    st.nowrap = !in->wrap;
    bare__style_text(ctx, &st, &w->st, file, line);
    w->st.style = NULL;
    w->lh = w->st.size * w->st.line_height;
    BareText *probe = bare__text_new(ctx, "0000000000", 10, &w->st);
    w->charw = probe && probe->max_line > 0 ? probe->max_line / 10.0f : w->st.size * 0.6f;
    int digits = 1;
    for (int n = doc->count; n >= 10; n /= 10) digits++;
    if (digits < 3) digits = 3;
    w->gutter = in->line_numbers ? (float)(digits + 2) * w->charw : 0;
    {
        BareBox q = box, rs;   /* the style's padding: the outer box's own is zero */
        q.padding = 0;
        bare__style_box(ctx, &q, &rs, file, line);
        BareEdges p = rs.pad;
        if (p.top == 0 && p.right == 0 && p.bottom == 0 && p.left == 0 && !signbit(rs.padding))
            p.top = p.right = p.bottom = p.left = rs.padding;
        w->pad.top = side(p.top, PAD_Y);
        w->pad.bottom = side(p.bottom, PAD_Y);
        w->pad.left = side(p.left, PAD_X);
        w->pad.right = side(p.right, PAD_X);
    }
    w->text_x = w->gutter + w->pad.left;
    w->wrap = in->wrap;
    float area_w = node && node->view.x > 0 ? node->view.x : 800;
    w->width = w->wrap ? fmaxf(area_w - w->text_x - w->pad.right, w->charw * 8) : 1e9f;
    w->key = w->wrap ? (uint32_t)bare__hash_u64(bare__hash_u64((uint64_t)(w->width * 4), (uint64_t)(w->st.size * 64)),
                                                (uint64_t)w->st.font * 131 + (uint64_t)w->st.tab_size) | 1u
                     : 1u;
    if (doc->longest_dirty) {
        doc->longest = 0;
        for (int i = 0; i < doc->count; i++) if (doc->lines[i].len > doc->longest) doc->longest = doc->lines[i].len;
        doc->longest_dirty = false;
        v->max_w = 0;
    }
    rows_ready(w);

    /* input */
    bool focused = ctx->focus_id == v->area;
    uint8_t kind = BARE__FOCUS_ABLE | BARE__FOCUS_FIELD | BARE__FOCUS_TABS | BARE__FOCUS_EDITOR;
    if (focused) ctx->focus_kind = kind;
    BareDocCmd c;
    memset(&c, 0, sizeof c);
    c.ctx = ctx;
    c.doc = doc;
    c.layout = layout;
    c.user = w;
    c.width = w->width;
    c.page = node && node->view.y > 0 ? (int)(node->view.y / w->lh) : 20;
    c.read_only = in->read_only;
    c.spaces = in->spaces;
    c.overwrite = v->overwrite;
    c.tab_size = w->st.tab_size;
    c.comment = in->comment;
    c.auto_close = in->auto_close;
    c.indent = in->indent;
    c.indent_user = in->indent_user;
    bool was = v->focused;
    if (focused && !was) v->blink = ctx->time;
    if (!focused && was) { v->comp_len = 0; v->drag = 0; }
    /* Escape in a bar field closes the bar and comes back to the text. */
    bool in_bar = ctx->focus_id && (ctx->focus_id == v->find_field || ctx->focus_id == v->replace_field ||
                                    ctx->focus_id == v->goto_field);
    if (v->was_in_bar && !ctx->focus_id && ctx->key_down[BARE_KEY_ESCAPE]) {
        close_bars(w);
        bare__focus_set(ctx, v->area, 3);
        focused = true;
        ctx->focus_kind = kind;
    }
    if (node && (focused || ctx->press_new)) mouse(w, node, was, &c);
    if (focused) keys(w, &c);
    focused = ctx->focus_id == v->area;
    v->focused = focused;
    if (c.changed) rows_ready(w);
    if (c.moved || c.changed || v->follow) {
        v->blink = ctx->time;
        follow(w, node);
        v->follow = false;
    }

    /* the find bar, then the text area */
    if (v->find_open || v->goto_open) find_bar(w, id);
    v->was_in_bar = in_bar || (ctx->focus_id && (ctx->focus_id == v->find_field || ctx->focus_id == v->replace_field ||
                                                 ctx->focus_id == v->goto_field));
    focused = ctx->focus_id == v->area;

    BareBox area;
    memset(&area, 0, sizeof area);
    area.id = v->area;
    area.width = BARE_GROW;
    area.height = BARE_GROW;
    area.scroll = w->wrap ? BARE_SCROLL_Y : BARE_SCROLL_BOTH;
    area.cursor = BARE_CURSOR_TEXT;
    area.focusable = true;
    int ai = bare__tree_push(ctx, &area, file, line);
    if (ai >= 0) {
        ctx->elems[ai].focus = kind;
        if (ctx->focus_id == v->area) ctx->focus_kind = kind;   /* the push made it a plain focusable box */
        if (ctx->depth < BARE__MAX_DEPTH) ctx->stack[ctx->depth++] = ai;
        float vis_h = node && node->view.y > 0 ? node->view.y : 1200;
        draw_lines(w, node, focused, vis_h, file, line);
        bare_close_();
    }
    if (focused) {
        ctx->focus_declared = true;
        ctx->ti_kind = BARE_TEXT_INPUT_MULTILINE;
    }
    if (w->behind) bare__want_frame_at(ctx, ctx->time);
    bare_close_();

    BareState s = bare__state_of(ctx, oi);
    s.focused = focused;
    s.changed = c.changed;
    return s;
}
