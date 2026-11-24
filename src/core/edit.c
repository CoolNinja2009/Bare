/* edit.c: the editing engine behind text fields (ARCHITECTURE §8).
 *
 * Works on a byte buffer with a caret and an anchor (the selection lies
 * between them), always on code point boundaries. Every editing command is
 * an action; keys reach actions through a table of bindings, the platform's
 * defaults unless the app rebinds them. Changes go through one function that
 * records them for undo; a typing run (or a run of Backspaces) grows one
 * record, broken at word starts, so undo goes back a word at a time. */
#include "edit.h"
#include <string.h>

#define UNDO_MAX 1000

/* ---- bindings ------------------------------------------------------------- */

enum { ALL, PC, MAC };   /* which platforms a default binding is for */
#define SC BARE_MOD_SHORTCUT
#define SH BARE_MOD_SHIFT
#define AL BARE_MOD_ALT

static const struct { uint8_t plat; int key; uint8_t mods; uint8_t action; } defaults[] = {
    { ALL, BARE_KEY_LEFT, 0, BARE_EDIT_LEFT },             { ALL, BARE_KEY_RIGHT, 0, BARE_EDIT_RIGHT },
    { ALL, BARE_KEY_UP, 0, BARE_EDIT_UP },                 { ALL, BARE_KEY_DOWN, 0, BARE_EDIT_DOWN },
    { ALL, BARE_KEY_PAGE_UP, 0, BARE_EDIT_PAGE_UP },       { ALL, BARE_KEY_PAGE_DOWN, 0, BARE_EDIT_PAGE_DOWN },
    { ALL, BARE_KEY_HOME, 0, BARE_EDIT_LINE_START },       { ALL, BARE_KEY_END, 0, BARE_EDIT_LINE_END },
    { ALL, BARE_KEY_A, SC, BARE_EDIT_SELECT_ALL },
    { ALL, BARE_KEY_BACKSPACE, 0, BARE_EDIT_BACKSPACE },   { ALL, BARE_KEY_BACKSPACE, SH, BARE_EDIT_BACKSPACE },
    { ALL, BARE_KEY_DELETE, 0, BARE_EDIT_DELETE },
    { ALL, BARE_KEY_X, SC, BARE_EDIT_CUT },  { ALL, BARE_KEY_C, SC, BARE_EDIT_COPY },
    { ALL, BARE_KEY_V, SC, BARE_EDIT_PASTE },
    { ALL, BARE_KEY_Z, SC, BARE_EDIT_UNDO }, { ALL, BARE_KEY_Z, SC | SH, BARE_EDIT_REDO },
    { ALL, BARE_KEY_ENTER, 0, BARE_EDIT_ENTER },           { ALL, BARE_KEY_ENTER, SH, BARE_EDIT_NEWLINE },
    { ALL, BARE_KEY_TAB, 0, BARE_EDIT_TAB },               { ALL, BARE_KEY_ESCAPE, 0, BARE_EDIT_BLUR },
    /* Windows and Linux */
    { PC, BARE_KEY_LEFT, SC, BARE_EDIT_WORD_LEFT },        { PC, BARE_KEY_RIGHT, SC, BARE_EDIT_WORD_RIGHT },
    { PC, BARE_KEY_HOME, SC, BARE_EDIT_TEXT_START },       { PC, BARE_KEY_END, SC, BARE_EDIT_TEXT_END },
    { PC, BARE_KEY_BACKSPACE, SC, BARE_EDIT_DELETE_WORD_LEFT },
    { PC, BARE_KEY_DELETE, SC, BARE_EDIT_DELETE_WORD_RIGHT },
    { PC, BARE_KEY_DELETE, SH, BARE_EDIT_CUT },            { PC, BARE_KEY_INSERT, SC, BARE_EDIT_COPY },
    { PC, BARE_KEY_INSERT, SH, BARE_EDIT_PASTE },          { PC, BARE_KEY_Y, SC, BARE_EDIT_REDO },
    { PC, BARE_KEY_INSERT, 0, BARE_EDIT_OVERWRITE },
    /* macOS */
    { MAC, BARE_KEY_LEFT, AL, BARE_EDIT_WORD_LEFT },       { MAC, BARE_KEY_RIGHT, AL, BARE_EDIT_WORD_RIGHT },
    { MAC, BARE_KEY_LEFT, SC, BARE_EDIT_LINE_START },      { MAC, BARE_KEY_RIGHT, SC, BARE_EDIT_LINE_END },
    { MAC, BARE_KEY_UP, SC, BARE_EDIT_TEXT_START },        { MAC, BARE_KEY_DOWN, SC, BARE_EDIT_TEXT_END },
    { MAC, BARE_KEY_UP, AL, BARE_EDIT_LINE_START },        { MAC, BARE_KEY_DOWN, AL, BARE_EDIT_LINE_END },
    { MAC, BARE_KEY_BACKSPACE, AL, BARE_EDIT_DELETE_WORD_LEFT },
    { MAC, BARE_KEY_DELETE, AL, BARE_EDIT_DELETE_WORD_RIGHT },
    { MAC, BARE_KEY_BACKSPACE, SC, BARE_EDIT_DELETE_LINE_LEFT },
};

static uint8_t resolve_mods(const BareContext *ctx, int mods) {
    if (mods & BARE_MOD_SHORTCUT) mods = (mods & ~BARE_MOD_SHORTCUT) | (ctx->mac_keys ? BARE_MOD_SUPER : BARE_MOD_CTRL);
    return (uint8_t)(mods & 15);
}

static bool load_defaults(BareContext *ctx) {
    if (ctx->bind_count) return true;
    int n = (int)(sizeof defaults / sizeof defaults[0]);
    if (!bare__grow(&ctx->alloc, (void **)&ctx->binds, &ctx->bind_cap, n, sizeof *ctx->binds)) return false;
    for (int i = 0; i < n; i++) {
        if (defaults[i].plat == (ctx->mac_keys ? PC : MAC)) continue;
        ctx->binds[ctx->bind_count++] = (BareKeyBinding){ defaults[i].key, resolve_mods(ctx, defaults[i].mods),
                                                          defaults[i].action };
    }
    return true;
}

static bool is_move(int a) { return a >= BARE_EDIT_LEFT && a <= BARE_EDIT_TEXT_END; }

BareEditAction bare__edit_lookup(BareContext *ctx, int key, int mods, bool *extend) {
    *extend = false;
    if (!load_defaults(ctx)) return BARE_EDIT_NONE;
    for (int i = 0; i < ctx->bind_count; i++)
        if (ctx->binds[i].key == key && ctx->binds[i].mods == mods) return (BareEditAction)ctx->binds[i].action;
    if (mods & BARE_MOD_SHIFT) {   /* Shift + a movement selects */
        for (int i = 0; i < ctx->bind_count; i++) {
            const BareKeyBinding *b = &ctx->binds[i];
            if (b->key == key && b->mods == (mods & ~BARE_MOD_SHIFT) && is_move(b->action)) {
                *extend = true;
                return (BareEditAction)b->action;
            }
        }
    }
    return BARE_EDIT_NONE;
}

void bare_edit_bind(int key, int mods, BareEditAction action) {
    BareContext *ctx = bare_ctx_current();
    if (!ctx || key <= BARE_KEY_NONE || key >= BARE_KEY_COUNT_ || !load_defaults(ctx)) return;
    uint8_t m = resolve_mods(ctx, mods);
    for (int i = 0; i < ctx->bind_count; i++) {
        if (ctx->binds[i].key != key || ctx->binds[i].mods != m) continue;
        if (action <= BARE_EDIT_NONE || action >= BARE_EDIT_ACTION_COUNT_) ctx->binds[i] = ctx->binds[--ctx->bind_count];
        else ctx->binds[i].action = (uint8_t)action;
        return;
    }
    if (action <= BARE_EDIT_NONE || action >= BARE_EDIT_ACTION_COUNT_) return;
    if (!bare__grow(&ctx->alloc, (void **)&ctx->binds, &ctx->bind_cap, ctx->bind_count + 1, sizeof *ctx->binds)) return;
    ctx->binds[ctx->bind_count++] = (BareKeyBinding){ key, m, (uint8_t)action };
}

/* Keys that type something: the field takes them even though the text
 * itself comes as a TEXT event. AltGr (Ctrl+Alt on Windows) and Option (on
 * macOS) type characters too. */
bool bare__edit_types(const BareContext *ctx, int key, int mods) {
    bool printable = (key >= BARE_KEY_A && key <= BARE_KEY_Z) || (key >= BARE_KEY_0 && key <= BARE_KEY_9) ||
                     key == BARE_KEY_SPACE || (key >= BARE_KEY_MINUS && key < BARE_KEY_COUNT_);
    if (!printable) return false;
    int held = mods & (BARE_MOD_CTRL | BARE_MOD_ALT | BARE_MOD_SUPER);
    if (!held) return true;
    if (ctx->mac_keys) return held == BARE_MOD_ALT;
    return held == (BARE_MOD_CTRL | BARE_MOD_ALT);
}

/* ---- text ------------------------------------------------------------------- */

uint64_t bare__edit_hash(const char *s, int len) {
    return bare__hash_u64(bare__hash_bytes(BARE__FNV_OFFSET, s, (size_t)len), (uint64_t)len);
}

int bare__edit_next(const char *s, int len, int at) {
    if (at >= len) return len;
    int i = at;
    bare__utf8_decode(s, len, &i);
    return i;
}

int bare__edit_prev(const char *s, int at) {
    if (at <= 0) return 0;
    int i = at - 1;   /* back over continuation bytes, at most three */
    while (i > 0 && at - i < 4 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    int j = i;
    bare__utf8_decode(s, at, &j);
    return j == at ? i : at - 1;   /* an invalid sequence steps back one byte */
}

enum { C_SPACE, C_NEWLINE, C_WORD, C_PUNCT };

static int class_at(const char *s, int len, int at) {
    if (at < 0 || at >= len) return C_SPACE;
    unsigned char c = (unsigned char)s[at];
    if (c == '\n') return C_NEWLINE;
    if (c == ' ' || c == '\t' || c == '\r') return C_SPACE;
    if (c >= 0x80 || c == '_' || (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z')) return C_WORD;
    return C_PUNCT;
}

void bare__edit_word_at(const char *s, int len, int at, int *a, int *b) {
    if (at >= len && at > 0) at = bare__edit_prev(s, at);
    int c = class_at(s, len, at);
    int lo = at, hi = at;
    if (c != C_NEWLINE) {
        while (lo > 0 && class_at(s, len, bare__edit_prev(s, lo)) == c) lo = bare__edit_prev(s, lo);
        while (hi < len && class_at(s, len, hi) == c) hi = bare__edit_next(s, len, hi);
    } else if (hi < len) {
        hi++;
    }
    *a = lo;
    *b = hi;
}

/* Ctrl+← (and Option+←): back over spaces, then over one run of a word or
 * of punctuation. A line start stops it at the line break. */
static int word_left(const char *s, int at) {
    int len = at;
    if (at > 0 && s[at - 1] == '\n') return at - 1;
    while (at > 0 && class_at(s, len, bare__edit_prev(s, at)) == C_SPACE) at = bare__edit_prev(s, at);
    if (at == 0 || s[at - 1] == '\n') return at;
    int c = class_at(s, len, bare__edit_prev(s, at));
    while (at > 0 && class_at(s, len, bare__edit_prev(s, at)) == c) at = bare__edit_prev(s, at);
    return at;
}

/* Ctrl+→ on Windows / Linux stops at the next word's start; Option+→ on
 * macOS at the end of this or the next word. */
static int word_right(const char *s, int len, int at, bool mac) {
    if (at < len && s[at] == '\n') return at + 1;
    if (mac) {
        while (at < len && class_at(s, len, at) == C_SPACE) at = bare__edit_next(s, len, at);
        int c = class_at(s, len, at);
        if (c == C_NEWLINE) return at;
        while (at < len && class_at(s, len, at) == c) at = bare__edit_next(s, len, at);
        return at;
    }
    int c = class_at(s, len, at);
    if (c != C_SPACE)
        while (at < len && class_at(s, len, at) == c) at = bare__edit_next(s, len, at);
    while (at < len && class_at(s, len, at) == C_SPACE) at = bare__edit_next(s, len, at);
    return at;
}

/* ---- changes and undo -------------------------------------------------------- */

static bool pool_add(BareContext *ctx, BareEditState *ed, const char *s, int n) {
    if (n <= 0) return true;
    if (!bare__grow(&ctx->alloc, (void **)&ed->bytes, &ed->bytes_cap, ed->bytes_len + n, 1)) return false;
    memcpy(ed->bytes + ed->bytes_len, s, (size_t)n);
    ed->bytes_len += n;
    return true;
}

static void undo_trim(BareEditState *ed) {
    int drop = ed->undo_count / 2;
    int cut = ed->undo[drop].bytes;
    memmove(ed->bytes, ed->bytes + cut, (size_t)(ed->bytes_len - cut));
    ed->bytes_len -= cut;
    memmove(ed->undo, ed->undo + drop, sizeof *ed->undo * (size_t)(ed->undo_count - drop));
    ed->undo_count -= drop;
    ed->undo_pos -= drop;
    for (int i = 0; i < ed->undo_count; i++) ed->undo[i].bytes -= cut;
}

/* Grows the last record instead of adding one, for typing runs. */
static bool merge(BareEditor *e, int at, int del, const char *ins, int n, int kind) {
    BareEditState *ed = e->ed;
    if (!ed->merge || ed->undo_pos != ed->undo_count || ed->undo_count == 0) return false;
    BareUndo *u = &ed->undo[ed->undo_count - 1];
    if (u->kind != kind) return false;
    if (kind == BARE__UNDO_TYPE && del == 0 && u->at + u->ins == at) {
        const char *last = ed->bytes + u->bytes + u->del + u->ins - 1;
        bool word_start = class_at(ins, n, 0) == C_WORD && (*last == ' ' || *last == '\n' || *last == '\t');
        if (word_start || !pool_add(e->ctx, ed, ins, n)) return false;
        u->ins += n;
        return true;
    }
    if (kind == BARE__UNDO_BACK && n == 0 && at + del == u->at && u->ins == 0) {
        if (!pool_add(e->ctx, ed, e->buf + at, del)) return false;   /* room, then prepend */
        char *d = ed->bytes + u->bytes;
        memmove(d + del, d, (size_t)u->del);
        memcpy(d, e->buf + at, (size_t)del);
        u->at = at;
        u->del += del;
        return true;
    }
    if (kind == BARE__UNDO_FORWARD && n == 0 && at == u->at && u->ins == 0) {
        if (!pool_add(e->ctx, ed, e->buf + at, del)) return false;
        u->del += del;
        return true;
    }
    return false;
}

static void record(BareEditor *e, int at, int del, const char *ins, int n, int kind, int caret0, int anchor0) {
    BareEditState *ed = e->ed;
    if (merge(e, at, del, ins, n, kind)) return;
    ed->undo_count = ed->undo_pos;   /* a new change forgets what was undone */
    if (ed->undo_count) ed->bytes_len = ed->undo[ed->undo_count - 1].bytes + ed->undo[ed->undo_count - 1].del +
                                        ed->undo[ed->undo_count - 1].ins;
    else ed->bytes_len = 0;
    if (ed->undo_count >= UNDO_MAX) undo_trim(ed);
    if (!bare__grow(&e->ctx->alloc, (void **)&ed->undo, &ed->undo_cap, ed->undo_count + 1, sizeof *ed->undo)) return;
    BareUndo u = { at, del, n, ed->bytes_len, caret0, anchor0, 0, 0, (uint8_t)kind };
    if (!pool_add(e->ctx, ed, e->buf + at, del) || !pool_add(e->ctx, ed, ins, n)) return;
    ed->undo[ed->undo_count++] = u;
    ed->undo_pos = ed->undo_count;
}

/* The longest prefix of s[0..n) that is whole code points and fits `room`. */
static int fit(const char *s, int n, int room) {
    if (n <= room) return n;
    int i = 0, ok = 0;
    while (i < n) {
        bare__utf8_decode(s, n, &i);
        if (i > room) break;
        ok = i;
    }
    return ok;
}

/* The one way text changes: bytes [at, at + del) become ins[0..n). */
static void replace(BareEditor *e, int at, int del, const char *ins, int n, int kind) {
    if (e->read_only) return;
    n = fit(ins, n, e->cap - (e->len - del));
    if (del == 0 && n == 0) return;
    BareEditState *ed = e->ed;
    int caret0 = ed->caret, anchor0 = ed->anchor;
    record(e, at, del, ins, n, kind, caret0, anchor0);
    memmove(e->buf + at + n, e->buf + at + del, (size_t)(e->len - at - del));
    memcpy(e->buf + at, ins, (size_t)n);
    e->len += n - del;
    e->buf[e->len] = 0;
    ed->caret = ed->anchor = at + n;
    ed->upstream = false;
    ed->merge = kind != BARE__UNDO_OTHER;
    e->gen++;
    if (ed->undo_count && ed->undo_pos == ed->undo_count) {
        ed->undo[ed->undo_count - 1].caret1 = ed->caret;
        ed->undo[ed->undo_count - 1].anchor1 = ed->anchor;
    }
    e->changed = true;
}

static void undo(BareEditor *e, bool redo) {
    BareEditState *ed = e->ed;
    if (e->read_only || (redo ? ed->undo_pos >= ed->undo_count : ed->undo_pos <= 0)) return;
    const BareUndo *u = &ed->undo[redo ? ed->undo_pos : ed->undo_pos - 1];
    const char *del = ed->bytes + u->bytes, *ins = del + u->del;
    int remove = redo ? u->del : u->ins, put = redo ? u->ins : u->del;
    const char *with = redo ? ins : del;
    if (e->len - remove + put > e->cap) return;
    memmove(e->buf + u->at + put, e->buf + u->at + remove, (size_t)(e->len - u->at - remove));
    memcpy(e->buf + u->at, with, (size_t)put);
    e->len += put - remove;
    e->buf[e->len] = 0;
    ed->caret = redo ? u->caret1 : u->caret0;
    ed->anchor = redo ? u->anchor1 : u->anchor0;
    ed->undo_pos += redo ? 1 : -1;
    e->gen++;
    ed->merge = false;
    ed->upstream = false;
    e->changed = true;
}

void bare__edit_forget(BareEditState *ed) {
    ed->undo_count = ed->undo_pos = 0;
    ed->bytes_len = 0;
    ed->comp_len = 0;
    ed->merge = false;
}

void bare__edit_release(BareContext *ctx, BareEditState *ed) {
    if (!ed) return;
    bare__free(&ctx->alloc, ed->undo, sizeof *ed->undo * (size_t)ed->undo_cap);
    bare__free(&ctx->alloc, ed->bytes, (size_t)ed->bytes_cap);
    bare__free(&ctx->alloc, ed->comp, (size_t)ed->comp_cap);
    bare__free(&ctx->alloc, ed, sizeof *ed);
}

/* ---- commands -------------------------------------------------------------- */

static int sel_lo(const BareEditState *ed) { return ed->caret < ed->anchor ? ed->caret : ed->anchor; }
static int sel_hi(const BareEditState *ed) { return ed->caret > ed->anchor ? ed->caret : ed->anchor; }

void bare__edit_select(BareEditor *e, int anchor, int caret, bool upstream) {
    BareEditState *ed = e->ed;
    if (anchor < 0) anchor = 0;
    if (caret < 0) caret = 0;
    if (anchor > e->len) anchor = e->len;
    if (caret > e->len) caret = e->len;
    if (ed->caret != caret || ed->anchor != anchor) e->moved = true;
    ed->caret = caret;
    ed->anchor = anchor;
    ed->upstream = upstream;
    ed->merge = false;
}

static void move_to(BareEditor *e, int to, bool extend, bool upstream) {
    bare__edit_select(e, extend ? e->ed->anchor : to, to, upstream);
}

static void delete_sel(BareEditor *e) {
    int lo = sel_lo(e->ed), hi = sel_hi(e->ed);
    if (hi > lo) replace(e, lo, hi - lo, "", 0, BARE__UNDO_OTHER);
}

/* Visual line start / end: the wrapped line in a multi-line field. */
static void line_bounds(BareEditor *e, int *start, int *end, bool *end_upstream) {
    BareEditState *ed = e->ed;
    *end_upstream = false;
    BareText *t = e->multiline && e->layout ? e->layout(e) : NULL;
    if (!t) { *start = 0; *end = e->len; return; }
    int l = bare__text_line_of(t, ed->caret, ed->upstream);
    *start = bare__text_line_start(t, l);
    *end = bare__text_line_end(t, l, end_upstream);
}

/* ↑ ↓ PageUp PageDown: lines away, keeping the caret's column. */
static void vertical(BareEditor *e, int lines, bool extend) {
    BareEditState *ed = e->ed;
    BareText *t = e->multiline && e->layout ? e->layout(e) : NULL;
    if (!t) { move_to(e, lines < 0 ? 0 : e->len, extend, false); return; }
    int l = bare__text_line_of(t, ed->caret, ed->upstream);
    if (!ed->has_goal) ed->goal_x = bare__text_x_on_line(t, l, ed->caret, e->box_w);
    int to = l + lines;
    bool up = false;
    if (to < 0) { move_to(e, 0, extend, false); }
    else if (to >= t->line_count) { move_to(e, e->len, extend, false); }
    else {
        int at = bare__text_offset_at(t, to, ed->goal_x, e->box_w, &up);
        move_to(e, at, extend, up);
    }
    ed->has_goal = true;
}

static void paste(BareEditor *e) {
    const char *clip = bare__clipboard_get(e->ctx);
    int n = (int)strlen(clip);
    char *s = bare__arena_alloc(&e->ctx->frame, (size_t)n + 1, 1);
    if (!s) return;
    int k = 0;
    for (int i = 0; i < n; i++) {   /* \r\n -> \n; a single line gets spaces for line breaks */
        char c = clip[i];
        if (c == '\r') { if (i + 1 < n && clip[i + 1] == '\n') continue; c = '\n'; }
        if (c == '\n' && !e->multiline) c = ' ';
        if ((unsigned char)c < 0x20 && c != '\n' && c != '\t') continue;
        s[k++] = c;
    }
    int lo = sel_lo(e->ed), hi = sel_hi(e->ed);
    replace(e, lo, hi - lo, s, k, BARE__UNDO_OTHER);
}

static void copy(BareEditor *e) {
    int lo = sel_lo(e->ed), hi = sel_hi(e->ed);
    if (hi <= lo || e->password) return;
    char *s = bare__arena_alloc(&e->ctx->frame, (size_t)(hi - lo) + 1, 1);
    if (!s) return;
    memcpy(s, e->buf + lo, (size_t)(hi - lo));
    bare__clipboard_set(e->ctx, s);
}

void bare__edit_run(BareEditor *e, BareEditAction a, bool extend) {
    BareEditState *ed = e->ed;
    bool mac = e->ctx->mac_keys;
    int lo = sel_lo(ed), hi = sel_hi(ed);
    bool sel = hi > lo;
    if (ed->comp_len > 0 && a != BARE_EDIT_BLUR) return;   /* the IME has the keys while composing */
    if (a != BARE_EDIT_UP && a != BARE_EDIT_DOWN && a != BARE_EDIT_PAGE_UP && a != BARE_EDIT_PAGE_DOWN)
        ed->has_goal = false;
    switch (a) {
    case BARE_EDIT_LEFT:
        if (sel && !extend) move_to(e, lo, false, false);
        else move_to(e, bare__edit_prev(e->buf, ed->caret), extend, false);
        break;
    case BARE_EDIT_RIGHT:
        if (sel && !extend) move_to(e, hi, false, false);
        else move_to(e, bare__edit_next(e->buf, e->len, ed->caret), extend, false);
        break;
    case BARE_EDIT_WORD_LEFT: move_to(e, word_left(e->buf, ed->caret), extend, false); break;
    case BARE_EDIT_WORD_RIGHT: move_to(e, word_right(e->buf, e->len, ed->caret, mac), extend, false); break;
    case BARE_EDIT_LINE_START: {
        int s, end; bool up;
        line_bounds(e, &s, &end, &up);
        int first = s;   /* smart Home: the first non-space, then the line's start */
        while (first < end && (e->buf[first] == ' ' || e->buf[first] == '\t')) first++;
        bool para = s == 0 || e->buf[s - 1] == '\n';
        move_to(e, (para && !mac && ed->caret != first) ? first : s, extend, false);
        break;
    }
    case BARE_EDIT_LINE_END: {
        int s, end; bool up;
        line_bounds(e, &s, &end, &up);
        move_to(e, end, extend, up);
        break;
    }
    case BARE_EDIT_UP: vertical(e, -1, extend); break;
    case BARE_EDIT_DOWN: vertical(e, 1, extend); break;
    case BARE_EDIT_PAGE_UP:
    case BARE_EDIT_PAGE_DOWN: {
        BareText *t = e->multiline && e->layout ? e->layout(e) : NULL;
        int page = t && t->line_height > 0 ? (int)(e->page_h / t->line_height) - 1 : 1;
        vertical(e, (page < 1 ? 1 : page) * (a == BARE_EDIT_PAGE_UP ? -1 : 1), extend);
        break;
    }
    case BARE_EDIT_TEXT_START: move_to(e, 0, extend, false); break;
    case BARE_EDIT_TEXT_END: move_to(e, e->len, extend, false); break;
    case BARE_EDIT_SELECT_ALL: bare__edit_select(e, 0, e->len, false); break;
    case BARE_EDIT_BACKSPACE:
        if (sel) delete_sel(e);
        else if (ed->caret > 0) {
            int p = bare__edit_prev(e->buf, ed->caret);
            replace(e, p, ed->caret - p, "", 0, BARE__UNDO_BACK);
        }
        break;
    case BARE_EDIT_DELETE:
        if (sel) delete_sel(e);
        else if (ed->caret < e->len)
            replace(e, ed->caret, bare__edit_next(e->buf, e->len, ed->caret) - ed->caret, "", 0, BARE__UNDO_FORWARD);
        break;
    case BARE_EDIT_DELETE_WORD_LEFT:
    case BARE_EDIT_DELETE_LINE_LEFT:
        if (sel) { delete_sel(e); break; }
        {
            int to;
            if (a == BARE_EDIT_DELETE_WORD_LEFT) to = word_left(e->buf, ed->caret);
            else { int end; bool up; line_bounds(e, &to, &end, &up); if (to == ed->caret) to = bare__edit_prev(e->buf, to); }
            replace(e, to, ed->caret - to, "", 0, BARE__UNDO_OTHER);
        }
        break;
    case BARE_EDIT_DELETE_WORD_RIGHT:
        if (sel) delete_sel(e);
        else {
            int to = word_right(e->buf, e->len, ed->caret, mac);
            replace(e, ed->caret, to - ed->caret, "", 0, BARE__UNDO_OTHER);
        }
        break;
    case BARE_EDIT_CUT: if (!e->password && !e->read_only) { copy(e); delete_sel(e); } break;
    case BARE_EDIT_COPY: copy(e); break;
    case BARE_EDIT_PASTE: paste(e); break;
    case BARE_EDIT_UNDO: undo(e, false); break;
    case BARE_EDIT_REDO: undo(e, true); break;
    case BARE_EDIT_ENTER:
    case BARE_EDIT_NEWLINE:
        if (e->multiline) replace(e, lo, hi - lo, "\n", 1, BARE__UNDO_TYPE);
        else e->submitted = true;
        break;
    case BARE_EDIT_TAB:
        if (e->multiline && e->tabs) replace(e, lo, hi - lo, "\t", 1, BARE__UNDO_TYPE);
        break;
    case BARE_EDIT_BLUR:
        if (ed->comp_len > 0) ed->comp_len = 0;
        else bare__focus_set(e->ctx, 0, 0);
        break;
    case BARE_EDIT_OVERWRITE: ed->overwrite = !ed->overwrite; break;
    default: break;
    }
    if (e->changed) e->moved = true;
}

/* Typed (or IME-committed) text replaces the selection; in overwrite mode it
 * replaces the characters after the caret too, but not a line break. */
void bare__edit_type(BareEditor *e, const char *text, int n) {
    BareEditState *ed = e->ed;
    ed->comp_len = 0;
    ed->has_goal = false;
    char *s = bare__arena_alloc(&e->ctx->frame, (size_t)n + 1, 1);
    if (!s) return;
    int k = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\r') continue;
        if (c == '\n' && !e->multiline) continue;
        if (c < 0x20 && c != '\n' && !(c == '\t' && e->tabs)) continue;
        if (c == 0x7F) continue;
        s[k++] = (char)c;
    }
    if (!k) return;
    int lo = sel_lo(ed), hi = sel_hi(ed);
    if (ed->overwrite && lo == hi) {
        int cps = 0;
        for (int i = 0; i < k;) { bare__utf8_decode(s, k, &i); cps++; }
        while (cps-- > 0 && hi < e->len && e->buf[hi] != '\n') hi = bare__edit_next(e->buf, e->len, hi);
    }
    replace(e, lo, hi - lo, s, k, lo == hi ? BARE__UNDO_TYPE : BARE__UNDO_OTHER);
    e->moved = true;
}

void bare__edit_set_comp(BareEditor *e, const char *text, int n, int cursor) {
    BareEditState *ed = e->ed;
    if (e->read_only) n = 0;
    if (!bare__grow(&e->ctx->alloc, (void **)&ed->comp, &ed->comp_cap, n + 1, 1)) return;
    memcpy(ed->comp, text, (size_t)n);
    ed->comp[n] = 0;
    ed->comp_len = n;
    ed->comp_cursor = cursor < 0 ? n : cursor > n ? n : cursor;
    e->moved = true;
}
