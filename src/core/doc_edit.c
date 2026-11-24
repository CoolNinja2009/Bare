/* doc_edit.c: editing commands on a document (ARCHITECTURE §8).
 *
 * Every command works on every selection, so multiple carets need no special
 * cases: changes go through bare__doc_insert / bare__doc_delete, which move
 * all selections the way the text moved, and each command is one undo step.
 * Commands that move the carets by screen rows ask the view for a line's
 * layout (cmd->layout); without one (the public bare_doc_* calls) a row is a
 * line and a column a code point. */
#include "doc.h"
#include "edit.h"
#include <string.h>

typedef struct Block { int a, b; } Block;   /* lines a..b, inclusive */

static const char *ln(const BareDoc *d, int i, int *len) {
    *len = d->lines[i].len;
    return d->lines[i].s ? d->lines[i].s : "";
}

static BareTextPos lo_of(const BareSel *s) { return bare__pos_cmp(s->anchor, s->caret) <= 0 ? s->anchor : s->caret; }
static BareTextPos hi_of(const BareSel *s) { return bare__pos_cmp(s->anchor, s->caret) <= 0 ? s->caret : s->anchor; }
static bool empty(const BareSel *s) { return bare__pos_cmp(s->anchor, s->caret) == 0; }
static BareTextPos pos(int line, int col) { return (BareTextPos){ line, col }; }

static void put(BareSel *s, BareTextPos anchor, BareTextPos caret) {
    s->anchor = anchor;
    s->caret = caret;
    s->upstream = false;
    s->has_goal = false;
}

static bool word_byte(unsigned char c) {
    return c >= 0x80 || c == '_' || (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z');
}

static BareTextPos prev_pos(const BareDoc *d, BareTextPos p) {
    int n;
    const char *s = ln(d, p.line, &n);
    if (p.col > 0) { p.col = bare__edit_prev(s, p.col); return p; }
    if (p.line > 0) { p.line--; p.col = d->lines[p.line].len; }
    return p;
}

static BareTextPos next_pos(const BareDoc *d, BareTextPos p) {
    int n;
    const char *s = ln(d, p.line, &n);
    if (p.col < n) { p.col = bare__edit_next(s, n, p.col); return p; }
    if (p.line + 1 < d->count) { p.line++; p.col = 0; }
    return p;
}

static int leading_ws(const char *s, int n) {
    int i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

/* The column on screen, tabs expanded. */
static int vis_col(const char *s, int n, int col, int tab) {
    int v = 0;
    for (int i = 0; i < col && i < n;) {
        if (s[i] == '\t') { v += tab - v % tab; i++; }
        else { bare__utf8_decode(s, n, &i); v++; }
    }
    return v;
}

static int cps_before(const char *s, int n, int col) {
    int k = 0;
    for (int i = 0; i < col && i < n; k++) bare__utf8_decode(s, n, &i);
    return k;
}

static int col_after_cps(const char *s, int n, int k) {
    int i = 0;
    while (k-- > 0 && i < n) bare__utf8_decode(s, n, &i);
    return i;
}

static bool grow_sels(BareDoc *d, int need) {
    return bare__grow(&d->alloc, (void **)&d->sels, &d->sel_cap, need, sizeof *d->sels);
}

static BareSel *copy_sels(BareDoc *d) {
    BareSel *s = bare__alloc(&d->alloc, sizeof *s * (size_t)d->sel_count);
    if (s) memcpy(s, d->sels, sizeof *s * (size_t)d->sel_count);
    return s;
}

/* Lines the selections cover, merged where they overlap. A selection that
 * ends at a line's start doesn't take that line. */
static Block *line_blocks(BareDoc *d, int *count) {
    Block *bl = bare__alloc(&d->alloc, sizeof *bl * (size_t)d->sel_count);
    int n = 0;
    if (bl) {
        for (int i = 0; i < d->sel_count; i++) {
            BareTextPos lo = lo_of(&d->sels[i]), hi = hi_of(&d->sels[i]);
            int a = lo.line, b = hi.line;
            if (b > a && hi.col == 0) b--;
            if (n > 0 && a <= bl[n - 1].b) { if (b > bl[n - 1].b) bl[n - 1].b = b; }
            else bl[n++] = (Block){ a, b };
        }
    }
    *count = n;
    return bl;
}

static const char *pair_close(const char *pairs, char c) {
    if (!pairs || !c) return NULL;
    for (int i = 0; pairs[i] && pairs[i + 1]; i += 2)
        if (pairs[i] == c) return &pairs[i + 1];
    return NULL;
}

static bool is_closer(const char *pairs, char c) {
    if (!pairs || !c) return false;
    for (int i = 0; pairs[i] && pairs[i + 1]; i += 2)
        if (pairs[i + 1] == c) return true;
    return false;
}

/* ---- moving ------------------------------------------------------------------ */

static BareText *lay(BareDocCmd *c, int line) { return c->layout ? c->layout(c->user, line) : NULL; }

/* `rows` screen rows down (or up), keeping the x the move started from. */
static void vertical(BareDocCmd *c, BareSel *s, int rows) {
    BareDoc *d = c->doc;
    int line = s->caret.line, len;
    const char *str = ln(d, line, &len);
    BareText *t = lay(c, line);
    int row = t ? bare__text_line_of(t, s->caret.col, s->upstream) : 0;
    if (!s->has_goal) {
        s->goal_x = t ? bare__text_x_on_line(t, row, s->caret.col, c->width) : (float)cps_before(str, len, s->caret.col);
        s->has_goal = true;
    }
    for (; rows > 0; rows--) {
        if (t && row + 1 < t->line_count) row++;
        else if (line + 1 < d->count) { line++; t = lay(c, line); row = 0; }
        else { s->caret = pos(line, d->lines[line].len); s->upstream = false; return; }
    }
    for (; rows < 0; rows++) {
        if (row > 0) row--;
        else if (line > 0) { line--; t = lay(c, line); row = t && t->line_count > 0 ? t->line_count - 1 : 0; }
        else { s->caret = pos(0, 0); s->upstream = false; return; }
    }
    bool up = false;
    str = ln(d, line, &len);
    int col = t ? bare__text_offset_at(t, row, s->goal_x, c->width, &up) : col_after_cps(str, len, (int)s->goal_x);
    s->caret = pos(line, col);
    s->upstream = up;
}

/* The screen row's start and end around a position. */
static void row_bounds(BareDocCmd *c, BareTextPos p, bool upstream, int *a, int *b, bool *b_up) {
    BareText *t = lay(c, p.line);
    *b_up = false;
    if (!t || t->line_count <= 1) { *a = 0; *b = c->doc->lines[p.line].len; return; }
    int r = bare__text_line_of(t, p.col, upstream);
    *a = bare__text_line_start(t, r);
    *b = bare__text_line_end(t, r, b_up);
}

static void move(BareDocCmd *c, BareEditAction a, bool extend) {
    BareDoc *d = c->doc;
    bool mac = c->ctx && c->ctx->mac_keys;
    int page = c->page > 1 ? c->page - 1 : 1;
    for (int i = 0; i < d->sel_count; i++) {
        BareSel *s = &d->sels[i];
        BareTextPos lo = lo_of(s), hi = hi_of(s), to = s->caret;
        bool up = false, sel = !empty(s);
        int len;
        const char *str = ln(d, s->caret.line, &len);
        switch (a) {
        case BARE_EDIT_LEFT: to = sel && !extend ? lo : prev_pos(d, s->caret); break;
        case BARE_EDIT_RIGHT: to = sel && !extend ? hi : next_pos(d, s->caret); break;
        case BARE_EDIT_WORD_LEFT:
            to = to.col == 0 ? prev_pos(d, to) : pos(to.line, bare__edit_word_left(str, to.col));
            break;
        case BARE_EDIT_WORD_RIGHT:
            to = to.col >= len ? next_pos(d, to) : pos(to.line, bare__edit_word_right(str, len, to.col, mac));
            break;
        case BARE_EDIT_LINE_START: {
            int ra, rb;
            bool rup;
            row_bounds(c, s->caret, s->upstream, &ra, &rb, &rup);
            int first = ra == 0 ? leading_ws(str, len) : ra;   /* smart Home: the text's start, then column 0 */
            to.col = (ra == 0 && !mac && s->caret.col != first) ? first : ra;
            break;
        }
        case BARE_EDIT_LINE_END: {
            int ra, rb;
            row_bounds(c, s->caret, s->upstream, &ra, &rb, &up);
            to.col = rb;
            break;
        }
        case BARE_EDIT_UP: case BARE_EDIT_DOWN: case BARE_EDIT_PAGE_UP: case BARE_EDIT_PAGE_DOWN: {
            BareSel m = *s;
            int rows = a == BARE_EDIT_UP ? -1 : a == BARE_EDIT_DOWN ? 1 : a == BARE_EDIT_PAGE_UP ? -page : page;
            vertical(c, &m, rows);
            to = m.caret;
            up = m.upstream;
            s->goal_x = m.goal_x;
            s->has_goal = true;
            break;
        }
        case BARE_EDIT_TEXT_START: to = pos(0, 0); break;
        case BARE_EDIT_TEXT_END: to = pos(d->count - 1, d->lines[d->count - 1].len); break;
        default: break;
        }
        s->caret = to;
        s->upstream = up;
        if (!extend) s->anchor = to;
    }
    d->merge = false;
}

/* ---- typing --------------------------------------------------------------------- */

static void replace_each(BareDoc *d, const char *text, int n, int kind) {
    bare__doc_begin(d, kind);
    for (int i = 0; i < d->sel_count; i++) {
        BareSel *s = &d->sels[i];
        if (!empty(s)) bare__doc_delete(d, lo_of(s), hi_of(s));
        BareTextPos e = bare__doc_insert(d, s->caret, text, n);
        put(s, e, e);
    }
    bare__doc_end(d);
    bare__doc_sels_normalize(d);
}

void bare__doc_type(BareDocCmd *c, const char *text, int n) {
    BareDoc *d = c->doc;
    if (c->read_only || !text || n <= 0) return;
    const char *pairs = c->auto_close;
    char ch = n == 1 ? text[0] : 0;
    const char *close = pair_close(pairs, ch);
    bool closer = is_closer(pairs, ch);
    /* A word started after a space begins a new undo step: undo goes back a word at a time. */
    BareSel *m = &d->sels[d->main];
    if (empty(m) && m->caret.col > 0 && word_byte((unsigned char)text[0])) {
        char p = d->lines[m->caret.line].s[m->caret.col - 1];
        if (p == ' ' || p == '\t') d->merge = false;
    }
    int typed = cps_before(text, n, n);
    bare__doc_begin(d, BARE__UNDO_TYPE);
    for (int i = 0; i < d->sel_count; i++) {
        BareSel *s = &d->sels[i];
        BareTextPos lo = lo_of(s), hi = hi_of(s);
        bool has = !empty(s);
        int len;
        const char *str = ln(d, hi.line, &len);
        char next = hi.col < len ? str[hi.col] : 0;
        char prev = lo.col > 0 ? d->lines[lo.line].s[lo.col - 1] : 0;
        if (has && close) {   /* wrap the selection */
            bool fwd = bare__pos_cmp(s->anchor, s->caret) <= 0;
            bare__doc_insert(d, hi, close, 1);
            bare__doc_insert(d, lo, &ch, 1);
            BareTextPos a = pos(lo.line, lo.col + 1), b = pos(hi.line, hi.col + (hi.line == lo.line ? 1 : 0));
            put(s, fwd ? a : b, fwd ? b : a);
            continue;
        }
        if (!has && closer && next == ch) {   /* typed over the closer we put there */
            put(s, pos(hi.line, hi.col + 1), pos(hi.line, hi.col + 1));
            continue;
        }
        if (has) bare__doc_delete(d, lo, hi);
        BareTextPos at = s->caret;
        if (c->overwrite && !has) {
            int end = at.col;
            str = ln(d, at.line, &len);
            for (int k = 0; k < typed && end < len; k++) end = bare__edit_next(str, len, end);
            bare__doc_delete(d, at, pos(at.line, end));
        }
        bool quote = close && *close == ch;
        bool pair = !has && close && (next == 0 || next == ' ' || next == '\t' || is_closer(pairs, next)) &&
                    !(quote && word_byte((unsigned char)prev));
        if (pair) {
            char two[2] = { ch, *close };
            bare__doc_insert(d, at, two, 2);
            put(s, pos(at.line, at.col + 1), pos(at.line, at.col + 1));
        } else {
            BareTextPos e = bare__doc_insert(d, at, text, n);
            put(s, e, e);
        }
    }
    bare__doc_end(d);
    bare__doc_sels_normalize(d);
    c->changed = c->moved = true;
}

/* What Enter puts on the new line: the indenter's answer, or the current
 * line's indentation. -1: the indenter ends the block (e.g. an empty list item). */
static int indent_for(BareDocCmd *c, int line, int col, char *out, int cap) {
    int len;
    const char *s = ln(c->doc, line, &len);
    if (c->indent) {
        int k = c->indent(c->indent_user, s, len, col, out, cap);
        return k > cap ? cap : k;
    }
    int k = leading_ws(s, len);
    if (k > col) k = col;
    if (k > cap) k = cap;
    memcpy(out, s, (size_t)k);
    return k;
}

static int unit(const BareDocCmd *c, char *out) {
    int tab = c->tab_size > 0 ? c->tab_size : 4;
    if (!c->spaces) { out[0] = '\t'; return 1; }
    memset(out, ' ', (size_t)tab);
    return tab;
}

static void newline(BareDocCmd *c) {
    BareDoc *d = c->doc;
    bare__doc_begin(d, BARE__UNDO_TYPE);
    for (int i = 0; i < d->sel_count; i++) {
        BareSel *s = &d->sels[i];
        if (!empty(s)) bare__doc_delete(d, lo_of(s), hi_of(s));
        BareTextPos at = s->caret;
        char buf[600];
        int k = indent_for(c, at.line, at.col, buf + 1, 256);
        int len;
        const char *str = ln(d, at.line, &len);
        if (k < 0) {   /* ends the block: the line before the caret is cleared instead */
            bare__doc_delete(d, pos(at.line, 0), at);
            put(s, pos(at.line, 0), pos(at.line, 0));
            continue;
        }
        if (at.col > 0 && leading_ws(str, len) == len) {   /* leave no indentation behind on an empty line */
            bare__doc_delete(d, pos(at.line, 0), pos(at.line, len));
            at = s->caret;
            str = ln(d, at.line, &len);
        }
        char before = at.col > 0 ? str[at.col - 1] : 0, after = at.col < len ? str[at.col] : 0;
        const char *cl = pair_close(c->auto_close, before);
        bool split = cl && *cl == after && before != after;   /* {|}: the closer goes a line further down */
        buf[0] = '\n';
        int n = 1 + k, caret_col = k;
        if (split) {
            int u = unit(c, buf + n);
            n += u;
            caret_col += u;
            buf[n++] = '\n';
            memcpy(buf + n, buf + 1, (size_t)k);
            n += k;
        }
        bare__doc_insert(d, at, buf, n);
        put(s, pos(at.line + 1, caret_col), pos(at.line + 1, caret_col));
    }
    bare__doc_end(d);
    bare__doc_sels_normalize(d);
}

static bool spans_lines(const BareDoc *d) {
    for (int i = 0; i < d->sel_count; i++)
        if (d->sels[i].anchor.line != d->sels[i].caret.line) return true;
    return false;
}

/* Tab and Shift+Tab on whole lines. Selections that started at a line's
 * start stay there. */
static void indent_lines(BareDocCmd *c, int dir) {
    BareDoc *d = c->doc;
    int nb;
    Block *bl = line_blocks(d, &nb);
    BareSel *old = copy_sels(d);
    if (!bl || !old) goto done;
    int tab = c->tab_size > 0 ? c->tab_size : 4;
    char u[64];
    int un = unit(c, u);
    bare__doc_begin(d, BARE__UNDO_OTHER);
    for (int k = 0; k < nb; k++) {
        for (int l = bl[k].a; l <= bl[k].b; l++) {
            int len;
            const char *s = ln(d, l, &len);
            if (dir > 0) {
                if (len > 0) bare__doc_insert(d, pos(l, 0), u, un);
            } else {
                int r = 0;
                if (len > 0 && s[0] == '\t') r = 1;
                else while (r < len && r < tab && s[r] == ' ') r++;
                if (r) bare__doc_delete(d, pos(l, 0), pos(l, r));
            }
        }
    }
    for (int i = 0; i < d->sel_count; i++) {
        if (old[i].anchor.col == 0 && d->sels[i].anchor.line == old[i].anchor.line) d->sels[i].anchor.col = 0;
        if (old[i].caret.col == 0 && d->sels[i].caret.line == old[i].caret.line) d->sels[i].caret.col = 0;
    }
    bare__doc_end(d);
done:
    if (bl) bare__free(&d->alloc, bl, sizeof *bl * (size_t)d->sel_count);
    if (old) bare__free(&d->alloc, old, sizeof *old * (size_t)d->sel_count);
}

static void tab(BareDocCmd *c) {
    BareDoc *d = c->doc;
    if (spans_lines(d)) { indent_lines(c, 1); return; }
    int tabw = c->tab_size > 0 ? c->tab_size : 4;
    bare__doc_begin(d, BARE__UNDO_TYPE);
    for (int i = 0; i < d->sel_count; i++) {
        BareSel *s = &d->sels[i];
        if (!empty(s)) bare__doc_delete(d, lo_of(s), hi_of(s));
        BareTextPos at = s->caret;
        char sp[64];
        int n = 1;
        sp[0] = '\t';
        if (c->spaces) {
            int len;
            const char *str = ln(d, at.line, &len);
            n = tabw - vis_col(str, len, at.col, tabw) % tabw;
            memset(sp, ' ', (size_t)n);
        }
        BareTextPos e = bare__doc_insert(d, at, sp, n);
        put(s, e, e);
    }
    bare__doc_end(d);
    bare__doc_sels_normalize(d);
}

void bare__doc_toggle(BareDoc *d, const char *prefix) {
    int plen = prefix ? (int)strlen(prefix) : 0, tlen = plen;
    while (tlen > 0 && prefix[tlen - 1] == ' ') tlen--;
    if (tlen <= 0) return;
    int nb;
    Block *bl = line_blocks(d, &nb);
    if (!bl) return;
    bool all = true, any = false;
    int min_ind = 1 << 30;
    for (int k = 0; k < nb; k++)
        for (int l = bl[k].a; l <= bl[k].b; l++) {
            int len;
            const char *s = ln(d, l, &len);
            int ind = leading_ws(s, len);
            if (ind == len) continue;   /* blank lines don't count */
            any = true;
            if (len - ind < tlen || memcmp(s + ind, prefix, (size_t)tlen)) all = false;
            if (ind < min_ind) min_ind = ind;
        }
    if (any) {
        bare__doc_begin(d, BARE__UNDO_OTHER);
        for (int k = 0; k < nb; k++)
            for (int l = bl[k].a; l <= bl[k].b; l++) {
                int len;
                const char *s = ln(d, l, &len);
                int ind = leading_ws(s, len);
                if (ind == len) continue;
                if (all) {
                    int r = len - ind >= plen && !memcmp(s + ind, prefix, (size_t)plen) ? plen : tlen;
                    bare__doc_delete(d, pos(l, ind), pos(l, ind + r));
                } else {
                    bare__doc_insert(d, pos(l, min_ind), prefix, plen);
                }
            }
        bare__doc_end(d);
    }
    bare__free(&d->alloc, bl, sizeof *bl * (size_t)d->sel_count);
}

/* ---- deleting ---------------------------------------------------------------------- */

static void erase(BareDocCmd *c, BareEditAction a) {
    BareDoc *d = c->doc;
    bool mac = c->ctx && c->ctx->mac_keys;
    int tabw = c->tab_size > 0 ? c->tab_size : 4;
    int kind = a == BARE_EDIT_BACKSPACE ? BARE__UNDO_BACK : a == BARE_EDIT_DELETE ? BARE__UNDO_FORWARD : BARE__UNDO_OTHER;
    bare__doc_begin(d, kind);
    for (int i = 0; i < d->sel_count; i++) {
        BareSel *s = &d->sels[i];
        if (!empty(s)) { bare__doc_delete(d, lo_of(s), hi_of(s)); continue; }
        BareTextPos p = s->caret, from = p, to = p;
        int len;
        const char *str = ln(d, p.line, &len);
        switch (a) {
        case BARE_EDIT_BACKSPACE: {
            from = prev_pos(d, p);
            const char *cl = p.col > 0 ? pair_close(c->auto_close, str[p.col - 1]) : NULL;
            if (cl && p.col < len && str[p.col] == *cl) to = pos(p.line, p.col + 1);   /* (|) goes as a pair */
            else if (c->spaces && p.col > 0 && leading_ws(str, len) >= p.col) {
                bool spaces = true;   /* in the indentation: back to the previous tab stop */
                for (int k = 0; k < p.col; k++) if (str[k] != ' ') spaces = false;
                if (spaces) from = pos(p.line, ((p.col - 1) / tabw) * tabw);
            }
            break;
        }
        case BARE_EDIT_DELETE: to = next_pos(d, p); break;
        case BARE_EDIT_DELETE_WORD_LEFT:
            from = p.col == 0 ? prev_pos(d, p) : pos(p.line, bare__edit_word_left(str, p.col));
            break;
        case BARE_EDIT_DELETE_WORD_RIGHT:
            to = p.col >= len ? next_pos(d, p) : pos(p.line, bare__edit_word_right(str, len, p.col, mac));
            break;
        case BARE_EDIT_DELETE_LINE_LEFT: {
            int ra, rb;
            bool up;
            row_bounds(c, p, s->upstream, &ra, &rb, &up);
            from = ra == p.col ? prev_pos(d, p) : pos(p.line, ra);
            break;
        }
        default: break;
        }
        bare__doc_delete(d, from, to);
    }
    bare__doc_end(d);
    bare__doc_sels_normalize(d);
}

/* ---- lines ------------------------------------------------------------------------- */

static void move_lines(BareDocCmd *c, int dir) {
    BareDoc *d = c->doc;
    int nb;
    Block *bl = line_blocks(d, &nb);
    BareSel *old = copy_sels(d);
    int n = d->sel_count;
    if (!bl || !old || nb == 0 || (dir < 0 ? bl[0].a == 0 : bl[nb - 1].b == d->count - 1)) goto done;
    bare__doc_begin(d, BARE__UNDO_OTHER);
    if (dir < 0) {
        for (int k = 0; k < nb; k++) {   /* the line above each block goes below it */
            int a = bl[k].a, b = bl[k].b, len;
            const char *s = ln(d, a - 1, &len);
            char *t = bare__doc_scratch(d, (size_t)len + 1);
            if (!t) break;
            if (b + 1 < d->count) {
                memcpy(t, s, (size_t)len);
                t[len] = '\n';
                bare__doc_insert(d, pos(b + 1, 0), t, len + 1);
            } else {
                t[0] = '\n';
                memcpy(t + 1, s, (size_t)len);
                bare__doc_insert(d, pos(b, d->lines[b].len), t, len + 1);
            }
            bare__doc_delete(d, pos(a - 1, 0), pos(a, 0));
        }
    } else {
        for (int k = nb - 1; k >= 0; k--) {   /* the line below each block goes above it */
            int a = bl[k].a, b = bl[k].b, len;
            const char *s = ln(d, b + 1, &len);
            char *t = bare__doc_scratch(d, (size_t)len + 1);
            if (!t) break;
            memcpy(t, s, (size_t)len);
            t[len] = '\n';
            bare__doc_delete(d, pos(b, d->lines[b].len), pos(b + 1, len));
            bare__doc_insert(d, pos(a, 0), t, len + 1);
        }
    }
    for (int i = 0; i < n && i < d->sel_count; i++) {
        d->sels[i] = old[i];
        d->sels[i].anchor.line += dir;
        d->sels[i].caret.line += dir;
    }
    bare__doc_end(d);
done:
    if (bl) bare__free(&d->alloc, bl, sizeof *bl * (size_t)n);
    if (old) bare__free(&d->alloc, old, sizeof *old * (size_t)n);
}

static void copy_lines(BareDocCmd *c, int dir) {
    BareDoc *d = c->doc;
    int nb;
    Block *bl = line_blocks(d, &nb);
    BareSel *old = copy_sels(d);
    int n = d->sel_count;
    if (!bl || !old) goto done;
    bare__doc_begin(d, BARE__UNDO_OTHER);
    for (int k = nb - 1; k >= 0; k--) {
        BareTextPos a = pos(bl[k].a, 0), b = pos(bl[k].b, d->lines[bl[k].b].len);
        size_t size = bare__doc_range_size(d, a, b);
        char *t = bare__doc_scratch(d, size + 1);
        if (!t) break;
        bare__doc_range_copy(d, a, b, t);
        t[size] = '\n';
        bare__doc_insert(d, a, t, (int)size + 1);
    }
    for (int i = 0; i < n && i < d->sel_count; i++) {
        /* below every copy made above it, and below its own copy going down */
        int shift = 0, line = lo_of(&old[i]).line;
        for (int k = 0; k < nb; k++) {
            int rows = bl[k].b - bl[k].a + 1;
            if (bl[k].b < line) shift += rows;
            else { if (dir > 0) shift += rows; break; }
        }
        d->sels[i] = old[i];
        d->sels[i].anchor.line += shift;
        d->sels[i].caret.line += shift;
    }
    bare__doc_end(d);
done:
    if (bl) bare__free(&d->alloc, bl, sizeof *bl * (size_t)n);
    if (old) bare__free(&d->alloc, old, sizeof *old * (size_t)n);
}

static void delete_lines(BareDoc *d) {
    int nb;
    Block *bl = line_blocks(d, &nb);
    int n = d->sel_count;
    if (!bl) return;
    bare__doc_begin(d, BARE__UNDO_OTHER);
    for (int k = nb - 1; k >= 0; k--) {
        int a = bl[k].a, b = bl[k].b;
        if (b + 1 < d->count) bare__doc_delete(d, pos(a, 0), pos(b + 1, 0));
        else if (a > 0) bare__doc_delete(d, pos(a - 1, d->lines[a - 1].len), pos(b, d->lines[b].len));
        else bare__doc_delete(d, pos(a, 0), pos(b, d->lines[b].len));
    }
    bare__doc_end(d);
    bare__free(&d->alloc, bl, sizeof *bl * (size_t)n);
    bare__doc_sels_normalize(d);
}

static void open_line(BareDocCmd *c, bool below) {
    BareDoc *d = c->doc;
    bare__doc_begin(d, BARE__UNDO_OTHER);
    for (int i = 0; i < d->sel_count; i++) {
        BareSel *s = &d->sels[i];
        int line = s->caret.line, len;
        ln(d, line, &len);
        char buf[300];
        int k = indent_for(c, line, below ? len : 0, buf + 1, 256);
        if (k < 0) k = 0;
        if (below) {
            buf[0] = '\n';
            BareTextPos e = bare__doc_insert(d, pos(line, len), buf, k + 1);
            put(s, e, e);
        } else {
            buf[k + 1] = '\n';
            bare__doc_insert(d, pos(line, 0), buf + 1, k + 1);
            put(s, pos(line, k), pos(line, k));
        }
    }
    bare__doc_end(d);
    bare__doc_sels_normalize(d);
}

static void select_lines(BareDoc *d) {
    for (int i = 0; i < d->sel_count; i++) {
        BareSel *s = &d->sels[i];
        BareTextPos lo = lo_of(s), hi = hi_of(s);
        bool whole = lo.col == 0 && hi.col == 0 && hi.line > lo.line;   /* again: one more line */
        int last = whole ? hi.line : hi.line + 1;
        BareTextPos end = last < d->count ? pos(last, 0) : pos(d->count - 1, d->lines[d->count - 1].len);
        put(s, pos(lo.line, 0), end);
    }
    bare__doc_sels_normalize(d);
}

/* ---- clipboard ---------------------------------------------------------------------- */

static void copy(BareDocCmd *c, bool cut) {
    BareDoc *d = c->doc;
    bool none = true;
    for (int i = 0; i < d->sel_count; i++) if (!empty(&d->sels[i])) none = false;
    size_t total = 1;
    int last = -1;
    for (int i = 0; i < d->sel_count; i++) {   /* nothing selected: the carets' whole lines */
        const BareSel *s = &d->sels[i];
        if (none) {
            if (s->caret.line != last) total += (size_t)d->lines[s->caret.line].len + 1;
            last = s->caret.line;
        } else if (!empty(s)) {
            total += bare__doc_range_size(d, lo_of(s), hi_of(s)) + 1;
        }
    }
    char *t = bare__doc_scratch(d, total);
    if (!t) return;
    size_t k = 0;
    last = -1;
    for (int i = 0; i < d->sel_count; i++) {
        const BareSel *s = &d->sels[i];
        if (none) {
            if (s->caret.line == last) continue;
            last = s->caret.line;
            memcpy(t + k, d->lines[last].s, (size_t)d->lines[last].len);
            k += (size_t)d->lines[last].len;
            t[k++] = '\n';
        } else if (!empty(s)) {
            if (k) t[k++] = '\n';
            bare__doc_range_copy(d, lo_of(s), hi_of(s), t + k);
            k += bare__doc_range_size(d, lo_of(s), hi_of(s));
        }
    }
    t[k] = 0;
    d->line_clip = none ? bare__edit_hash(t, (int)k) : 0;
    if (c->ctx) bare__clipboard_set(c->ctx, t);
    if (!cut || c->read_only) return;
    if (none) delete_lines(d);
    else replace_each(d, "", 0, BARE__UNDO_OTHER);
}

void bare__doc_paste(BareDocCmd *c, const char *clip, bool as_typed) {
    BareDoc *d = c->doc;
    if (c->read_only || !clip) return;
    int n = (int)strlen(clip), k = 0, lines = 1;
    char *s = bare__doc_scratch(d, (size_t)n + 1);
    if (!s) return;
    for (int i = 0; i < n; i++) {   /* \r\n and \r -> \n; other control characters go */
        char ch = clip[i];
        if (ch == '\r') { if (i + 1 < n && clip[i + 1] == '\n') continue; ch = '\n'; }
        if ((unsigned char)ch < 0x20 && ch != '\n' && ch != '\t') continue;
        if (ch == '\n') lines++;
        s[k++] = ch;
    }
    s[k] = 0;
    if (as_typed) { bare__doc_type(c, s, k); return; }
    bool none = true;
    for (int i = 0; i < d->sel_count; i++) if (!empty(&d->sels[i])) none = false;
    if (none && d->line_clip && d->line_clip == bare__edit_hash(s, k)) {   /* whole lines go above the caret's */
        bare__doc_begin(d, BARE__UNDO_OTHER);
        for (int i = 0; i < d->sel_count; i++) bare__doc_insert(d, pos(d->sels[i].caret.line, 0), s, k);
        bare__doc_end(d);
    } else if (d->sel_count > 1 && lines == d->sel_count) {   /* one line per caret */
        bare__doc_begin(d, BARE__UNDO_OTHER);
        int p = 0;
        for (int i = 0; i < d->sel_count; i++) {
            int e = p;
            while (e < k && s[e] != '\n') e++;
            BareSel *sel = &d->sels[i];
            if (!empty(sel)) bare__doc_delete(d, lo_of(sel), hi_of(sel));
            BareTextPos end = bare__doc_insert(d, sel->caret, s + p, e - p);
            put(sel, end, end);
            p = e + 1;
        }
        bare__doc_end(d);
    } else {
        replace_each(d, s, k, BARE__UNDO_OTHER);
    }
    bare__doc_sels_normalize(d);
    c->changed = c->moved = true;
}

/* ---- matches ------------------------------------------------------------------------- */

/* The main selection's text when it's on one line; an empty one first becomes
 * the word under the caret (returns false then: that's the whole step). */
static bool main_query(BareDoc *d, char **q, int *qn, bool *took_word) {
    BareSel *m = &d->sels[d->main];
    *took_word = false;
    if (empty(m)) {
        int len, a, b;
        const char *s = ln(d, m->caret.line, &len);
        bare__edit_word_at(s, len, m->caret.col, &a, &b);
        if (b <= a || !word_byte((unsigned char)s[a])) return false;
        put(m, pos(m->caret.line, a), pos(m->caret.line, b));
        *took_word = true;
    }
    BareTextPos lo = lo_of(m), hi = hi_of(m);
    if (lo.line != hi.line) return false;
    *qn = hi.col - lo.col;
    *q = bare__doc_scratch(d, (size_t)*qn + 1);
    if (!*q) return false;
    memcpy(*q, d->lines[lo.line].s + lo.col, (size_t)*qn);
    (*q)[*qn] = 0;
    return true;
}

static bool selected_at(const BareDoc *d, BareTextPos a, BareTextPos b) {
    for (int i = 0; i < d->sel_count; i++)
        if (!bare__pos_cmp(lo_of(&d->sels[i]), a) && !bare__pos_cmp(hi_of(&d->sels[i]), b)) return true;
    return false;
}

static void select_next(BareDoc *d) {
    char *q;
    int qn;
    bool word;
    if (!main_query(d, &q, &qn, &word) || word) return;
    BareTextPos from = hi_of(&d->sels[d->main]);
    for (int pass = 0; pass < 2; pass++) {   /* to the end, then from the top */
        BareTextPos a, b;
        BareTextPos at = pass ? pos(0, 0) : from;
        while (bare_doc_find(d, q, BARE_FIND_CASE, at, &a, &b)) {
            if (pass && bare__pos_cmp(a, from) >= 0) return;
            if (!selected_at(d, a, b)) {
                if (!grow_sels(d, d->sel_count + 1)) return;
                put(&d->sels[d->sel_count], a, b);
                d->main = d->sel_count++;
                bare__doc_sels_normalize(d);
                return;
            }
            at = b;
        }
    }
}

static void select_all_matches(BareDoc *d) {
    char *q;
    int qn;
    bool word;
    if (!main_query(d, &q, &qn, &word)) return;
    BareTextPos keep = lo_of(&d->sels[d->main]), at = pos(0, 0), a, b;
    int n = 0, main = 0;
    while (n < 100000 && bare_doc_find(d, q, BARE_FIND_CASE, at, &a, &b)) {
        if (!grow_sels(d, n + 1)) break;
        put(&d->sels[n], a, b);
        if (!bare__pos_cmp(a, keep)) main = n;
        n++;
        at = b;
    }
    if (n) { d->sel_count = n; d->main = main; }
}

static void add_caret(BareDocCmd *c, int dir) {
    BareDoc *d = c->doc;
    BareSel s = d->sels[d->main];
    s.anchor = s.caret;
    vertical(c, &s, dir);
    if (!bare__pos_cmp(s.caret, d->sels[d->main].caret) || !grow_sels(d, d->sel_count + 1)) return;
    s.anchor = s.caret;
    d->sels[d->sel_count] = s;
    d->main = d->sel_count++;
    bare__doc_sels_normalize(d);
}

/* ---- commands ------------------------------------------------------------------------- */

static bool edits(BareEditAction a) {
    switch (a) {
    case BARE_EDIT_BACKSPACE: case BARE_EDIT_DELETE: case BARE_EDIT_DELETE_WORD_LEFT:
    case BARE_EDIT_DELETE_WORD_RIGHT: case BARE_EDIT_DELETE_LINE_LEFT: case BARE_EDIT_CUT: case BARE_EDIT_PASTE:
    case BARE_EDIT_UNDO: case BARE_EDIT_REDO: case BARE_EDIT_ENTER: case BARE_EDIT_NEWLINE: case BARE_EDIT_TAB:
    case BARE_EDIT_MOVE_LINE_UP: case BARE_EDIT_MOVE_LINE_DOWN: case BARE_EDIT_COPY_LINE_UP:
    case BARE_EDIT_COPY_LINE_DOWN: case BARE_EDIT_DELETE_LINE: case BARE_EDIT_LINE_BELOW: case BARE_EDIT_LINE_ABOVE:
    case BARE_EDIT_INDENT: case BARE_EDIT_OUTDENT: case BARE_EDIT_COMMENT:
        return true;
    default:
        return false;
    }
}

void bare__doc_run(BareDocCmd *c, BareEditAction a, bool extend) {
    BareDoc *d = c->doc;
    if (c->read_only && edits(a)) return;
    uint64_t version = d->version;
    bool vert = a == BARE_EDIT_UP || a == BARE_EDIT_DOWN || a == BARE_EDIT_PAGE_UP || a == BARE_EDIT_PAGE_DOWN;
    if (!vert && a != BARE_EDIT_ADD_CARET_UP && a != BARE_EDIT_ADD_CARET_DOWN)
        for (int i = 0; i < d->sel_count; i++) d->sels[i].has_goal = false;
    switch (a) {
    case BARE_EDIT_LEFT: case BARE_EDIT_RIGHT: case BARE_EDIT_WORD_LEFT: case BARE_EDIT_WORD_RIGHT:
    case BARE_EDIT_LINE_START: case BARE_EDIT_LINE_END: case BARE_EDIT_UP: case BARE_EDIT_DOWN:
    case BARE_EDIT_PAGE_UP: case BARE_EDIT_PAGE_DOWN: case BARE_EDIT_TEXT_START: case BARE_EDIT_TEXT_END:
        move(c, a, extend);
        break;
    case BARE_EDIT_SELECT_ALL:
        d->sel_count = 1;
        d->main = 0;
        put(&d->sels[0], pos(0, 0), pos(d->count - 1, d->lines[d->count - 1].len));
        break;
    case BARE_EDIT_BACKSPACE: case BARE_EDIT_DELETE: case BARE_EDIT_DELETE_WORD_LEFT:
    case BARE_EDIT_DELETE_WORD_RIGHT: case BARE_EDIT_DELETE_LINE_LEFT:
        erase(c, a);
        break;
    case BARE_EDIT_CUT: copy(c, true); break;
    case BARE_EDIT_COPY: copy(c, false); return;
    case BARE_EDIT_PASTE: bare__doc_paste(c, c->ctx ? bare__clipboard_get(c->ctx) : "", false); break;
    case BARE_EDIT_UNDO: bare__doc_undo(d, false); break;
    case BARE_EDIT_REDO: bare__doc_undo(d, true); break;
    case BARE_EDIT_ENTER: case BARE_EDIT_NEWLINE: newline(c); break;
    case BARE_EDIT_TAB: tab(c); break;
    case BARE_EDIT_INDENT: indent_lines(c, 1); break;
    case BARE_EDIT_OUTDENT: indent_lines(c, -1); break;
    case BARE_EDIT_COMMENT: if (c->comment && *c->comment) bare__doc_toggle(d, c->comment); break;
    case BARE_EDIT_MOVE_LINE_UP: move_lines(c, -1); break;
    case BARE_EDIT_MOVE_LINE_DOWN: move_lines(c, 1); break;
    case BARE_EDIT_COPY_LINE_UP: copy_lines(c, -1); break;
    case BARE_EDIT_COPY_LINE_DOWN: copy_lines(c, 1); break;
    case BARE_EDIT_DELETE_LINE: delete_lines(d); break;
    case BARE_EDIT_LINE_BELOW: open_line(c, true); break;
    case BARE_EDIT_LINE_ABOVE: open_line(c, false); break;
    case BARE_EDIT_SELECT_LINE: select_lines(d); break;
    case BARE_EDIT_SELECT_NEXT_MATCH: select_next(d); break;
    case BARE_EDIT_SELECT_ALL_MATCHES: select_all_matches(d); break;
    case BARE_EDIT_ADD_CARET_UP: add_caret(c, -1); break;
    case BARE_EDIT_ADD_CARET_DOWN: add_caret(c, 1); break;
    case BARE_EDIT_BLUR:   /* Escape: back to one caret first */
        if (d->sel_count > 1) {
            BareSel m = d->sels[d->main];
            d->sels[0] = m;
            d->sel_count = 1;
            d->main = 0;
        } else {
            c->blur = true;
        }
        break;
    default:
        return;
    }
    bare__doc_sels_normalize(d);
    if (d->version != version) c->changed = true;
    c->moved = true;
}

/* ---- public ------------------------------------------------------------------------------ */

static BareDocCmd plain(BareDoc *d) {
    BareDocCmd c;
    memset(&c, 0, sizeof c);
    c.ctx = bare_ctx_current();
    c.doc = d;
    c.tab_size = 4;
    return c;
}

void bare_doc_insert(BareDoc *d, const char *text) {
    if (!d || !text) return;
    replace_each(d, text, (int)strlen(text), BARE__UNDO_OTHER);
}

void bare_doc_wrap(BareDoc *d, const char *before, const char *after) {
    if (!d) return;
    int bn = before ? (int)strlen(before) : 0, an = after ? (int)strlen(after) : 0;
    bare__doc_begin(d, BARE__UNDO_OTHER);
    for (int i = 0; i < d->sel_count; i++) {
        BareSel *s = &d->sels[i];
        BareTextPos lo = lo_of(s), hi = hi_of(s);
        bool fwd = bare__pos_cmp(s->anchor, s->caret) <= 0;
        if (an) bare__doc_insert(d, hi, after, an);
        BareTextPos a = bn ? bare__doc_insert(d, lo, before, bn) : lo;
        /* the selection keeps the text between them */
        BareTextPos b = hi;
        if (bn && memchr(before, '\n', (size_t)bn)) {
            int breaks = 0;
            for (int k = 0; k < bn; k++) if (before[k] == '\n') breaks++;
            b.line += breaks;
            if (hi.line == lo.line) b.col = a.col + (hi.col - lo.col);
        } else if (hi.line == lo.line) {
            b.col += bn;
        }
        put(s, fwd ? a : b, fwd ? b : a);
    }
    bare__doc_end(d);
    bare__doc_sels_normalize(d);
}

void bare_doc_toggle_prefix(BareDoc *d, const char *prefix) {
    if (d) bare__doc_toggle(d, prefix);
}

void bare_doc_select(BareDoc *d, BareTextPos anchor, BareTextPos caret) {
    if (!d) return;
    d->sel_count = 1;
    d->main = 0;
    put(&d->sels[0], bare__doc_clamp(d, anchor), bare__doc_clamp(d, caret));
    d->merge = false;
    d->view.follow = true;
}

BareTextPos bare_doc_caret(const BareDoc *d) { return d ? d->sels[d->main].caret : pos(0, 0); }
int bare_doc_selections(const BareDoc *d) { return d ? d->sel_count : 0; }

const char *bare_doc_selected(BareDoc *d, int *len) {
    if (len) *len = 0;
    if (!d) return "";
    BareTextPos a = lo_of(&d->sels[d->main]), b = hi_of(&d->sels[d->main]);
    size_t n = bare__doc_range_size(d, a, b);
    if (n + 1 > d->seltext_cap) {
        char *t = bare__alloc(&d->alloc, n + 1);
        if (!t) return "";
        bare__free(&d->alloc, d->seltext, d->seltext_cap);
        d->seltext = t;
        d->seltext_cap = n + 1;
    }
    bare__doc_range_copy(d, a, b, d->seltext);
    d->seltext[n] = 0;
    if (len) *len = (int)n;
    return d->seltext;
}

/* For tests and apps driving a document without an editor on screen. */
void bare__doc_command(BareDoc *d, BareEditAction a, bool extend) {
    BareDocCmd c = plain(d);
    bare__doc_run(&c, a, extend);
}
