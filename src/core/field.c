/* field.c: bare_input, a text field built from Bare's own elements
 * (ARCHITECTURE §8).
 *
 * A field is a focusable scroll box (sideways for one line, down for many)
 * holding one text element: the app's text, dots for a password, or the
 * placeholder, with the IME's composition spliced in at the caret. Mouse and
 * keyboard input change the app's buffer through the editing engine
 * (edit.c) while the field is declared; after layout the box scrolls to keep
 * the caret in view; emit draws the selection under the glyphs and the caret
 * over them. Mouse positions are matched against last frame's layout, as all
 * input in Bare is. */
#include "edit.h"
#include <math.h>
#include <string.h>

#define BLINK      0.53     /* seconds on, then off (Windows' default) */
#define BLINK_STOP 10.0     /* after this long without moving the caret stays solid: idle costs nothing */
#define DOT        "\xE2\x80\xA2"   /* U+2022 BULLET */

typedef struct Field {
    const BareTextStyle *style;
    BareRect text_rect;     /* the text element last frame */
    BareText *layout;       /* the shown text wrapped at box_w, rebuilt after each change */
    int gen;                /* the editor's gen it was built at */
} Field;

/* Password dots: shown offsets are three bytes per code point. */
static int to_shown(const BareEditCtx *e, int at) {
    if (!e->password) return at;
    int n = 0;
    for (int i = 0; i < at && i < e->len;) { bare__utf8_decode(e->buf, e->len, &i); n++; }
    return n * 3;
}

static int from_shown(const BareEditCtx *e, int at) {
    if (!e->password) return at;
    int want = at / 3, i = 0;
    while (want-- > 0 && i < e->len) bare__utf8_decode(e->buf, e->len, &i);
    return i;
}

/* The buffer as shown (no composition), shaped and wrapped like last frame. */
static BareText *layout(BareEditCtx *e) {
    Field *f = e->user;
    if (f->layout && f->gen == e->gen) return f->layout;
    const char *s = e->buf;
    int n = e->len;
    if (e->password) {
        int cps = to_shown(e, e->len) / 3;
        char *d = bare__arena_alloc(&e->ctx->frame, (size_t)cps * 3 + 1, 1);
        if (!d) return NULL;
        for (int i = 0; i < cps; i++) memcpy(d + i * 3, DOT, 3);
        s = d;
        n = cps * 3;
    }
    BareText *t = bare__text_new(e->ctx, s, n, f->style);
    if (!t) return NULL;
    bare__text_wrap(e->ctx, t, e->box_w);
    f->layout = t;
    f->gen = e->gen;
    return t;
}

/* The buffer offset under a window point (last frame's layout). */
static int offset_at(BareEditCtx *e, BareVec2 p, bool *upstream) {
    Field *f = e->user;
    *upstream = false;
    BareText *t = layout(e);
    if (!t || f->text_rect.w <= 0) return e->len;
    return from_shown(e, bare__text_hit(t, p.x - f->text_rect.x, p.y - f->text_rect.y, e->box_w, upstream));
}

static void line_at(const BareEditCtx *e, int at, int *a, int *b) {
    if (!e->multiline) { *a = 0; *b = e->len; return; }
    int lo = at, hi = at;
    while (lo > 0 && e->buf[lo - 1] != '\n') lo--;
    while (hi < e->len && e->buf[hi] != '\n') hi++;
    if (hi < e->len) hi++;   /* the paragraph's line break with it, as browsers select it */
    *a = lo;
    *b = hi;
}

static void unit_at(const BareEditCtx *e, int at, int unit, int *a, int *b) {
    if (unit == 2 && !e->password) bare__edit_word_at(e->buf, e->len, at, a, b);
    else if (unit >= 2) line_at(e, at, a, b);
    else *a = *b = at;
}

static void mouse(BareContext *ctx, BareEditCtx *e, BareId id, bool was_focused) {
    BareEditState *ed = e->ed;
    bool mine = bare__chain_has(&ctx->press[BARE__PRESS_LEFT], id);
    if (ctx->press_new && bare__chain_has(&ctx->press_began, id)) {
        bool up;
        int at = offset_at(e, ctx->press_pos, &up);
        int clicks = ctx->click_count;
        ed->has_goal = false;
        ed->comp_len = 0;
        if (clicks <= 1 && (ctx->key_down[BARE_KEY_SHIFT]) && was_focused) {
            bare__edit_select(e, ed->anchor, at, up);
            ed->drag = 1;
        } else {
            ed->drag = (uint8_t)(clicks > 3 ? 3 : clicks < 1 ? 1 : clicks);
            unit_at(e, at, ed->drag, &ed->drag_a, &ed->drag_b);
            bare__edit_select(e, ed->drag_a, ed->drag_b, ed->drag == 1 && up);
        }
        e->moved = true;
    } else if (ed->drag && ctx->held[BARE__PRESS_LEFT] && mine) {
        bool up;
        int at = offset_at(e, ctx->mouse, &up), a, b;
        unit_at(e, at, ed->drag, &a, &b);
        if (ed->drag == 1) {
            bare__edit_select(e, ed->anchor, at, up);
        } else if (at < ed->drag_a) {
            bare__edit_select(e, ed->drag_b, a, false);
        } else {
            bare__edit_select(e, ed->drag_a, b > ed->drag_b ? b : ed->drag_b, false);
        }
        /* Held past the edge, the box keeps scrolling toward the mouse. */
        bare__want_frame_at(ctx, ctx->time + 1.0 / 60.0);
    }
    if (!ctx->held[BARE__PRESS_LEFT]) ed->drag = 0;
}

static void keys(BareContext *ctx, BareEditCtx *e) {
    for (int i = 0; i < ctx->in_take; i++) {
        const BareKeyEv *k = &ctx->in[i];
        if (k->kind == BARE__IN_TEXT) {
            if (!e->read_only) bare__edit_type(e, ctx->in_text + k->text, k->len);
        } else if (k->kind == BARE__IN_COMPOSE) {
            bare__edit_set_comp(e, ctx->in_text + k->text, k->len, k->cursor);
        } else if (k->kind == BARE__IN_ACTION) {
            bare__edit_run(e, (BareEditAction)k->key, false);
        } else {
            bool extend;
            BareEditAction a = bare__edit_lookup(ctx, k->key, k->mods, &extend);
            if (a == BARE_EDIT_TAB && !(e->multiline && e->tabs)) continue;   /* focus moves instead (focus.c) */
            if (a != BARE_EDIT_NONE) bare__edit_run(e, a, extend);
        }
        if (!ctx->focus_id) break;   /* Escape: the rest isn't ours */
    }
}

static BareColor or_default(BareColor c, BareColor d) { return c.a ? c : d; }

/* The text element: what's shown, and what's drawn with it. */
static int show(BareContext *ctx, BareEditCtx *e, const BareInput *in, const BareTextStyle *style, BareId text_id,
                bool focused, const char *file, int line) {
    BareEditState *ed = e->ed;
    int lo = 0, hi = 0, comp = ed && focused ? ed->comp_len : 0;
    if (ed) {
        lo = ed->caret < ed->anchor ? ed->caret : ed->anchor;
        hi = ed->caret < ed->anchor ? ed->anchor : ed->caret;
    }
    BareTextStyle st = *style;
    st.id = text_id;
    bool placeholder = e->len == 0 && comp == 0 && in->placeholder && *in->placeholder;
    if (placeholder) {   /* an element of its own: swapping it for the text never animates between them */
        st.color = ctx->theme.muted;
        st.id = bare__hash_str(text_id, "placeholder");
    }

    const char *s;
    int n;
    if (placeholder) {
        s = in->placeholder;
        n = (int)strlen(s);
    } else {
        BareText *t = e->password ? layout(e) : NULL;   /* the dots */
        const char *base = t ? t->str : e->buf;
        int blen = t ? t->len : e->len;
        if (comp) {   /* the composition replaces the selection while it's typed */
            int a = to_shown(e, lo), b = to_shown(e, hi);
            char *d = bare__arena_alloc(&ctx->frame, (size_t)(blen - (b - a) + comp) + 1, 1);
            if (!d) return -1;
            memcpy(d, base, (size_t)a);
            memcpy(d + a, ed->comp, (size_t)comp);
            memcpy(d + a + comp, base + b, (size_t)(blen - b));
            s = d;
            n = blen - (b - a) + comp;
        } else {
            s = base;
            n = blen;
        }
    }
    int ti = bare__text_push(ctx, s, n, &st, file, line);
    if (ti < 0 || !ctx->elems[ti].text) return ti;
    BareElem *te = &ctx->elems[ti];
    if (e->multiline) te->box.width = BARE_GROW;   /* wraps at the field's width; clicks past a line land in it */
    if (!focused || !ed) return ti;

    BareTextDeco *d = bare__arena_alloc(&ctx->frame, sizeof *d, 8);
    if (!d) return ti;
    te->text->deco = d;
    double since = ctx->time - ed->blink;
    bool on = ctx->window_focused;
    if (on && since < BLINK_STOP) {
        double phase = floor(since / BLINK);
        on = ((long)phase % 2) == 0;
        bare__want_frame_at(ctx, ed->blink + (phase + 1) * BLINK);
    }
    d->caret_on = on && !e->read_only;
    d->caret_color = in->caret;
    d->sel_color = or_default(in->selection, ctx->theme.selection);
    if (placeholder) {
        d->caret = 0;
        return ti;
    }
    if (comp) {
        int a = to_shown(e, lo);
        d->comp_a = a;
        d->comp_b = a + comp;
        d->caret = a + ed->comp_cursor;
    } else {
        d->caret = to_shown(e, ed->caret);
        d->upstream = ed->upstream;
        d->sel_a = to_shown(e, lo);
        d->sel_b = to_shown(e, hi);
        d->block = ed->overwrite && lo == hi && ed->caret < e->len && e->buf[ed->caret] != '\n';
    }
    return ti;
}

BareState bare_input_(char *buf, int cap, const BareInput *in, const char *file, int line) {
    BareState none;
    memset(&none, 0, sizeof none);
    BareContext *ctx = bare_ctx_current();
    if (!ctx || !ctx->in_frame) {
        bare__log(ctx, "bare_input at %s:%d called outside a frame", file, line);
        return none;
    }
    if (!buf || cap < 1) {
        bare__log(ctx, "bare_input at %s:%d needs a buffer and its size", file, line);
        return none;
    }
    buf[cap - 1] = 0;
    int len = (int)strlen(buf);

    /* The text style: the theme's body unless given, then the call's overrides. */
    BareTextStyle st;
    bare__style_text(ctx, in->text_style ? in->text_style : &ctx->theme.body, &st, file, line);
    if (in->size > 0) st.size = in->size;
    if (in->font) st.font = in->font;
    if (in->color.a) st.color = in->color;
    st.nowrap = !in->multiline;
    st.style = NULL;

    BareBox box;
    memset(&box, 0, sizeof box);
    box.id = in->id;
    box.style = in->style ? in->style : &ctx->theme.input;
    box.width = in->width;
    box.height = in->height;
    box.focusable = true;
    box.scroll = in->multiline ? BARE_SCROLL_Y : BARE_SCROLL_X;
    box.cursor = BARE_CURSOR_TEXT;
    int fi = bare__tree_push(ctx, &box, file, line);
    if (fi < 0) return none;
    BareElem *fe = &ctx->elems[fi];
    BareId id = fe->id;
    bool focused = ctx->focus_id == id;
    uint8_t kind = BARE__FOCUS_ABLE | BARE__FOCUS_FIELD | (in->multiline ? 0 : BARE__FOCUS_LINE) |
                   (in->multiline && in->tabs ? BARE__FOCUS_TABS : 0);
    fe->focus = kind;
    if (focused) ctx->focus_kind = kind;
    fe->no_bars = !in->multiline;
    fe->scroll_pad = in->multiline ? 0.0f : 2.0f;   /* room for the caret after the last character */
    if (in->multiline && in->height == 0) {
        float size = st.size > 0 ? st.size : 16.0f, lh = size * (st.line_height > 0 ? st.line_height : 1.3f);
        fe->box.height = (float)(in->lines > 0 ? in->lines : 4) * lh + fe->box.pad.top + fe->box.pad.bottom;
    }
    if (ctx->depth < BARE__MAX_DEPTH) ctx->stack[ctx->depth++] = fi;

    /* Retained editing state, made when it first gets focus. */
    BareNode *node = fe->node >= 0 ? &ctx->store.nodes[fe->node] : NULL;
    if (node && focused && !node->edit) {
        node->edit = bare__alloc(&ctx->alloc, sizeof *node->edit);
        if (node->edit) {
            memset(node->edit, 0, sizeof *node->edit);
            node->edit->caret = node->edit->anchor = len;
            node->edit->hash = bare__edit_hash(buf, len);
            node->edit->len = len;
        }
    }
    BareEditState *ed = node ? node->edit : NULL;
    BareId text_id = bare__hash_str(id, "bare.input.text");

    Field f;
    memset(&f, 0, sizeof f);
    f.style = &st;
    int tn = bare__store_find(ctx, text_id);
    if (tn >= 0) f.text_rect = ctx->store.nodes[tn].rect;
    BareEditCtx e;
    memset(&e, 0, sizeof e);
    e.ctx = ctx;
    e.ed = ed;
    e.buf = buf;
    e.len = len;
    e.cap = cap - 1;
    e.multiline = in->multiline;
    e.password = in->password;
    e.tabs = in->tabs;
    e.read_only = in->read_only;
    e.layout = layout;
    e.user = &f;
    e.box_w = f.text_rect.w > 0 ? f.text_rect.w : 1e6f;
    e.page_h = node ? node->view.y : 0;

    bool gained = false;
    if (ed) {
        if (ed->hash != bare__edit_hash(buf, len) || ed->len != len) {   /* the app changed the text */
            bare__edit_forget(ed);
            ed->caret = ed->anchor = len;
            ed->upstream = false;
        }
        bool was = ed->focused;
        if (focused && !was) {
            gained = true;
            ed->blink = ctx->time;
            if (ctx->focus_how == 2 && !in->multiline) bare__edit_select(&e, 0, len, false);   /* Tab selects all */
        }
        if (!focused && was) { ed->comp_len = 0; ed->drag = 0; }
        ed->focused = focused;
        if (focused) {
            mouse(ctx, &e, id, was);
            keys(ctx, &e);
            if (e.moved) ed->blink = ctx->time;
            ed->hash = bare__edit_hash(buf, e.len);
            ed->len = e.len;
        }
    }
    bool now_focused = ctx->focus_id == id;   /* Escape may have blurred it */
    if (now_focused)
        ctx->ti_kind = in->password ? BARE_TEXT_INPUT_PASSWORD
                     : in->multiline ? BARE_TEXT_INPUT_MULTILINE : BARE_TEXT_INPUT_LINE;
    if (now_focused) ctx->focus_declared = true;

    int ti = show(ctx, &e, in, &st, text_id, now_focused, file, line);
    bare_close_();

    if (bare__grow(&ctx->alloc, (void **)&ctx->fields, &ctx->field_cap, ctx->field_count + 1, sizeof *ctx->fields))
        ctx->fields[ctx->field_count++] = (BareFieldFrame){ fi, ti, id, now_focused, in->multiline,
                                                            e.moved || gained };
    BareState s = bare__state_of(ctx, fi);
    s.focused = now_focused;
    s.changed = e.changed;
    s.submitted = e.submitted;
    return s;
}

/* ---- after layout ------------------------------------------------------------ */

/* The focused field scrolls (at once, as native fields do) to keep a caret
 * that just moved in view; the IME window goes under the caret. */
void bare__fields_layout(BareContext *ctx) {
    for (int i = 0; i < ctx->field_count; i++) {
        const BareFieldFrame *f = &ctx->fields[i];
        if (!f->focused || f->text < 0) continue;
        BareElem *box = &ctx->elems[f->box], *te = &ctx->elems[f->text];
        if (!te->text || !te->text->deco || box->node < 0) continue;
        const BareTextDeco *d = te->text->deco;
        BareRect c = bare__text_caret(te->text, d->caret, d->upstream, te->rect.w);
        c.x += te->rect.x;
        c.y += te->rect.y;
        c.w = 2;
        BareNode *n = &ctx->store.nodes[box->node];
        int a = f->multiline ? 1 : 0;
        float lo = a ? box->rect.y + box->box.pad.top : box->rect.x + box->box.pad.left;
        float hi = a ? box->rect.y + box->rect.h - box->box.pad.bottom : box->rect.x + box->rect.w - box->box.pad.right;
        float c0 = a ? c.y : c.x, c1 = a ? c.y + c.h : c.x + c.w;
        float *s = a ? &n->scroll.y : &n->scroll.x;
        float *to = a ? &n->scroll_to.y : &n->scroll_to.x;
        float max = fmaxf((a ? box->content[1] - box->rect.h : box->content[0] - box->rect.w), 0.0f);
        if (f->follow) {
            float v = *s;
            if (c1 - v > hi) v = c1 - hi;
            if (c0 - v < lo) v = c0 - lo;
            v = fminf(fmaxf(v, 0.0f), max);
            *s = *to = v;
            if (a) n->scroll_v.y = 0; else n->scroll_v.x = 0;
        }
        ctx->ti_rect = (BareRect){ c.x - (a ? 0 : *s), c.y - (a ? *s : 0), c.w, c.h };
    }
}

/* ---- drawing ------------------------------------------------------------------ */

static BareColorF color_f(BareColor c, float opacity) {
    return (BareColorF){ c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (c.a / 255.0f) * opacity };
}

static BareRect to_screen(BareRect local, BareRect r, float k, bool snap) {
    BareRect o = { r.x + local.x * k, r.y + local.y * k, local.w * k, local.h * k };
    if (snap) {
        float x0 = roundf(o.x), y0 = roundf(o.y), x1 = roundf(o.x + o.w), y1 = roundf(o.y + o.h);
        o = (BareRect){ x0, y0, x1 - x0, y1 - y0 };
    }
    return o;
}

/* Fills the rows of [a, b); b past the text's end also marks its line break. */
static void fill_range(BareContext *ctx, const BareText *t, int a, int b, BareRect r, float k, bool snap,
                       BareColorF color) {
    bool eol = b > t->len;
    if (eol) b = t->len;
    float box_w = r.w / k;
    if (b > a) {
        int cap = t->line_count + 1;
        BareRect *rects = bare__arena_alloc(&ctx->frame, sizeof *rects * (size_t)cap, 8);
        int n = rects ? bare__text_range_rects(t, a, b, box_w, rects, cap) : 0;
        for (int i = 0; i < n; i++) bare__emit_fill(ctx, to_screen(rects[i], r, k, snap), color);
    }
    if (eol) {
        BareRect c = bare__text_caret(t, t->len, false, box_w);
        c.w = fmaxf(t->size * 0.4f, 3.0f);
        bare__emit_fill(ctx, to_screen(c, r, k, snap), color);
    }
}

void bare__field_emit_under(BareContext *ctx, const BareText *t, BareRect r, float k, bool snap, float opacity) {
    const BareTextDeco *d = t->deco;
    if (!d || k <= 0) return;
    for (int i = 0; i < d->mark_count; i++)   /* find matches, brackets */
        fill_range(ctx, t, d->marks[i * 2], d->marks[i * 2 + 1], r, k, snap, color_f(d->mark_color, opacity));
    if (d->sel_a != d->sel_b) fill_range(ctx, t, d->sel_a, d->sel_b, r, k, snap, color_f(d->sel_color, opacity));
    for (int i = 0; i < d->sel_count; i++)
        fill_range(ctx, t, d->sels[i * 2], d->sels[i * 2 + 1], r, k, snap, color_f(d->sel_color, opacity));
}

/* Dots for spaces and a dash for tabs, between the glyphs. */
static void whitespace(BareContext *ctx, const BareText *t, BareRect r, float k, bool snap, BareColorF color) {
    float box_w = r.w / k, dot = fmaxf(1.0f, roundf(1.6f * k)) / k;
    for (int i = 0; i < t->len; i++) {
        char c = t->str[i];
        if (c != ' ' && c != '\t') continue;
        BareRect a = bare__text_caret(t, i, false, box_w), b = bare__text_caret(t, i + 1, true, box_w);
        if (b.y != a.y || b.x <= a.x) continue;
        BareRect m;
        if (c == ' ') m = (BareRect){ (a.x + b.x) * 0.5f - dot * 0.5f, a.y + a.h * 0.5f - dot * 0.5f, dot, dot };
        else m = (BareRect){ a.x + 2.0f, a.y + a.h * 0.5f - dot * 0.5f, fmaxf(b.x - a.x - 4.0f, dot), dot };
        bare__emit_fill(ctx, to_screen(m, r, k, snap), color);
    }
}

void bare__field_emit_over(BareContext *ctx, const BareText *t, BareRect r, float k, bool snap, BareColorF ink) {
    const BareTextDeco *d = t->deco;
    if (!d || k <= 0) return;
    float box_w = r.w / k;
    float px = fmaxf(1.0f, roundf(k));   /* a hairline of one logical pixel */
    if (d->comp_b > d->comp_a) {         /* the IME's text, underlined */
        int cap = t->line_count + 1;
        BareRect *rects = bare__arena_alloc(&ctx->frame, sizeof *rects * (size_t)cap, 8);
        int n = rects ? bare__text_range_rects(t, d->comp_a, d->comp_b, box_w, rects, cap) : 0;
        for (int i = 0; i < n; i++) {
            BareRect u = to_screen(rects[i], r, k, snap);
            const BareTextLine *line = &t->lines[bare__text_line_at_y(t, rects[i].y + 0.01f)];
            u.y = roundf(r.y + (line->y + line->asc + t->descent * 0.5f) * k);
            u.h = px;
            bare__emit_fill(ctx, u, ink);
        }
    }
    if (d->whitespace) {
        BareColorF wc = color_f(d->whitespace_color, ink.a);
        whitespace(ctx, t, r, k, snap, wc);
    }
    for (int i = 0; i < d->caret_count; i++) {   /* more carets (editors): thin, no upstream */
        BareRect c = bare__text_caret(t, d->carets[i], false, box_w);
        BareRect s = to_screen(c, r, k, snap);
        s.w = px;
        bare__emit_fill(ctx, s, d->caret_color.a ? color_f(d->caret_color, ink.a) : ink);
    }
    if (!d->caret_on || d->caret < 0) return;
    BareRect c = bare__text_caret(t, d->caret, d->upstream, box_w);
    if (d->block) {
        int next = bare__edit_next(t->str, t->len, d->caret);
        int l = bare__text_line_of(t, d->caret, d->upstream);
        c.w = bare__text_x_on_line(t, l, next, box_w) - c.x;
    }
    if (c.x > box_w - 1.0f && !d->block) c.x = fmaxf(box_w - 1.0f, 0.0f);   /* spaces hanging past the edge */
    BareRect s = to_screen(c, r, k, snap);
    if (!d->block) s.w = px;
    BareColorF col = d->caret_color.a ? color_f(d->caret_color, ink.a) : ink;
    if (d->block) col.a *= 0.45f;
    bare__emit_fill(ctx, s, col);
    ctx->ti_rect = (BareRect){ s.x / ctx->scale, s.y / ctx->scale, s.w / ctx->scale, s.h / ctx->scale };
}
