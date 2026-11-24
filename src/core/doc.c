/* doc.c: documents (ARCHITECTURE §8): lines, changes, undo, find.
 *
 * A document is an array of lines. A loaded file stays one block of bytes
 * the lines point into; a line gets its own copy the first time it changes,
 * so opening a large file is one read and one scan for line breaks. Every
 * change is an insert or a delete at a position; each records itself for
 * undo (the bytes go into a pool) and moves every selection the way the text
 * moved. Commands group their changes into one undo step together with the
 * selections before and after; a run of typing grows one step. */
#include "doc.h"
#include "edit.h"
#include <stdio.h>
#include <string.h>

#define UNDO_MAX 4000   /* steps kept; the older half goes when there are more */

static BareAllocator doc_alloc(void) {
    BareContext *ctx = bare_ctx_current();
    return ctx ? ctx->alloc : bare__default_allocator();
}

static bool grow(BareDoc *d, void **items, int *cap, int need, size_t size) {
    return bare__grow(&d->alloc, items, cap, need, size);
}

int bare__pos_cmp(BareTextPos a, BareTextPos b) {
    return a.line != b.line ? (a.line < b.line ? -1 : 1) : (a.col < b.col ? -1 : a.col > b.col ? 1 : 0);
}

BareTextPos bare__doc_clamp(const BareDoc *d, BareTextPos p) {
    if (p.line < 0) return (BareTextPos){ 0, 0 };
    if (p.line >= d->count) return (BareTextPos){ d->count - 1, d->lines[d->count - 1].len };
    if (p.col < 0) p.col = 0;
    if (p.col > d->lines[p.line].len) p.col = d->lines[p.line].len;
    return p;
}

/* ---- lines ------------------------------------------------------------------ */

static void line_free(BareDoc *d, BareLine *l) {
    if (l->cap > 0) bare__free(&d->alloc, l->s, (size_t)l->cap);
    l->s = NULL;
    l->len = l->cap = 0;
}

/* Makes the line its own, with room for `need` bytes. */
static bool line_reserve(BareDoc *d, BareLine *l, int need) {
    if (l->cap > 0 && need <= l->cap) return true;
    int cap = l->cap > 0 ? l->cap : 16;
    while (cap < need) cap *= 2;
    char *s = bare__alloc(&d->alloc, (size_t)cap);
    if (!s) return false;
    if (l->len) memcpy(s, l->s, (size_t)l->len);
    if (l->cap > 0) bare__free(&d->alloc, l->s, (size_t)l->cap);
    l->s = s;
    l->cap = cap;
    return true;
}

static bool lines_insert(BareDoc *d, int at, int n) {
    if (!grow(d, (void **)&d->lines, &d->cap, d->count + n, sizeof *d->lines)) return false;
    memmove(d->lines + at + n, d->lines + at, sizeof *d->lines * (size_t)(d->count - at));
    memset(d->lines + at, 0, sizeof *d->lines * (size_t)n);
    d->count += n;
    return true;
}

static void lines_remove(BareDoc *d, int at, int n) {
    for (int i = at; i < at + n; i++) line_free(d, &d->lines[i]);
    memmove(d->lines + at, d->lines + at + n, sizeof *d->lines * (size_t)(d->count - at - n));
    d->count -= n;
}

static void touched(BareDoc *d, int line, bool lines_changed) {
    d->version++;
    if (line < d->hl_valid) d->hl_valid = line;
    if (line < d->count) {
        d->lines[line].rows_key = 0;
        if (d->lines[line].len > d->longest) d->longest = d->lines[line].len;
    }
    if (lines_changed) d->rows_dirty = d->longest_dirty = true;   /* a changed line is re-measured when shown */
}

/* ---- loading -------------------------------------------------------------------- */

static void clear(BareDoc *d) {
    for (int i = 0; i < d->count; i++) line_free(d, &d->lines[i]);
    d->count = 0;
    bare__free(&d->alloc, d->orig, d->orig_size);
    d->orig = NULL;
    d->orig_size = 0;
    d->edit_count = d->group_count = d->undo_pos = 0;
    d->pool_len = 0;
    d->sel_pool_len = 0;
    d->merge = false;
    d->hl_valid = 0;
    d->rows_dirty = true;
    d->version++;
    d->state = d->saved_state = d->base_state = d->next_state++;
}

/* Takes `bytes` (owned) as the whole text: lines point into it. */
static bool adopt(BareDoc *d, char *bytes, size_t size) {
    clear(d);
    const char *s = bytes;
    size_t n = size;
    d->bom = n >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF;
    if (d->bom) { s += 3; n -= 3; }
    int lines = 1;
    for (size_t i = 0; i < n; i++)
        if (s[i] == '\n') lines++;
    if (!grow(d, (void **)&d->lines, &d->cap, lines, sizeof *d->lines)) {
        bare__free(&d->alloc, bytes, size);
        return false;
    }
    d->orig = bytes;
    d->orig_size = size;
    d->crlf = false;
    d->longest = 0;
    size_t start = 0;
    bool first = true;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && s[i] != '\n') continue;
        size_t end = i;
        if (i < n && end > start && s[end - 1] == '\r') {
            end--;
            if (first) d->crlf = true;
        }
        if (i < n) first = false;
        BareLine *l = &d->lines[d->count++];
        memset(l, 0, sizeof *l);
        l->s = (char *)s + start;
        l->len = (int)(end - start);
        if (l->len > d->longest) d->longest = l->len;
        start = i + 1;
    }
    d->sel_count = 1;
    d->main = 0;
    memset(d->sels, 0, sizeof *d->sels);
    return true;
}

BareDoc *bare_doc_create(void) {
    BareAllocator a = doc_alloc();
    BareDoc *d = bare__alloc(&a, sizeof *d);
    if (!d) return NULL;
    memset(d, 0, sizeof *d);
    d->alloc = a;
    d->next_state = 1;
    if (!grow(d, (void **)&d->sels, &d->sel_cap, 1, sizeof *d->sels)) { bare__free(&a, d, sizeof *d); return NULL; }
    char *empty = bare__alloc(&a, 1);
    if (!empty || !adopt(d, empty, 0)) { bare_doc_free(d); return NULL; }
    return d;
}

void bare_doc_set_text(BareDoc *d, const char *text, int len) {
    if (!d) return;
    if (!text) { text = ""; len = 0; }
    size_t n = len < 0 ? strlen(text) : (size_t)len;
    char *copy = bare__alloc(&d->alloc, n ? n : 1);
    if (!copy) return;
    memcpy(copy, text, n);
    adopt(d, copy, n);
}

BareDoc *bare_doc_load(const char *path) {
    FILE *fp = path ? fopen(path, "rb") : NULL;
    if (!fp) return NULL;
    long size = -1;
    if (fseek(fp, 0, SEEK_END) == 0) size = ftell(fp);
    if (size < 0 || fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    BareDoc *d = bare_doc_create();
    char *bytes = d ? bare__alloc(&d->alloc, size ? (size_t)size : 1) : NULL;
    if (!bytes || fread(bytes, 1, (size_t)size, fp) != (size_t)size) {
        fclose(fp);
        if (bytes) bare__free(&d->alloc, bytes, size ? (size_t)size : 1);
        bare_doc_free(d);
        return NULL;
    }
    fclose(fp);
    if (!adopt(d, bytes, (size_t)size)) { bare_doc_free(d); return NULL; }
    return d;
}

bool bare_doc_save(BareDoc *d, const char *path) {
    if (!d || !path) return false;
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    bool ok = !d->bom || fwrite("\xEF\xBB\xBF", 1, 3, fp) == 3;
    for (int i = 0; i < d->count && ok; i++) {
        const BareLine *l = &d->lines[i];
        if (l->len && fwrite(l->s, 1, (size_t)l->len, fp) != (size_t)l->len) ok = false;
        if (i + 1 < d->count && fwrite(d->crlf ? "\r\n" : "\n", 1, d->crlf ? 2 : 1, fp) != (d->crlf ? 2u : 1u))
            ok = false;
    }
    if (fclose(fp) != 0) ok = false;
    if (ok) {
        d->saved_state = d->state;
        d->merge = false;   /* typing after a save is a new step, so undoing it returns to "saved" */
    }
    return ok;
}

void bare_doc_free(BareDoc *d) {
    if (!d) return;
    for (int i = 0; i < d->count; i++) line_free(d, &d->lines[i]);
    BareAllocator a = d->alloc;
    bare__free(&a, d->lines, sizeof *d->lines * (size_t)d->cap);
    bare__free(&a, d->orig, d->orig_size);
    bare__free(&a, d->sels, sizeof *d->sels * (size_t)d->sel_cap);
    bare__free(&a, d->edits, sizeof *d->edits * (size_t)d->edit_cap);
    bare__free(&a, d->groups, sizeof *d->groups * (size_t)d->group_cap);
    bare__free(&a, d->sel_pool, sizeof *d->sel_pool * (size_t)d->sel_pool_cap);
    bare__free(&a, d->pool, d->pool_cap);
    bare__free(&a, d->row_sum, sizeof *d->row_sum * (size_t)d->row_sum_cap);
    bare__free(&a, d->text, d->text_cap);
    bare__free(&a, d->scratch, d->scratch_cap);
    bare__free(&a, d->seltext, d->seltext_cap);
    bare__doc_view_free(d);
    bare__free(&a, d, sizeof *d);
}

/* ---- reading -------------------------------------------------------------------- */

int bare_doc_lines(const BareDoc *d) { return d ? d->count : 0; }

const char *bare_doc_line(const BareDoc *d, int line, int *len) {
    if (!d || line < 0 || line >= d->count) { if (len) *len = 0; return ""; }
    if (len) *len = d->lines[line].len;
    return d->lines[line].s ? d->lines[line].s : "";
}

bool bare_doc_modified(const BareDoc *d) { return d && d->state != d->saved_state; }
uint64_t bare_doc_version(const BareDoc *d) { return d ? d->version : 0; }
bool bare_doc_crlf(const BareDoc *d) { return d && d->crlf; }
void bare_doc_set_crlf(BareDoc *d, bool crlf) {
    if (d && d->crlf != crlf) { d->crlf = crlf; d->saved_state = (uint64_t)-1; }   /* saving would change the file */
}

size_t bare__doc_range_size(const BareDoc *d, BareTextPos a, BareTextPos b) {
    if (a.line == b.line) return (size_t)(b.col - a.col);
    size_t n = (size_t)(d->lines[a.line].len - a.col) + 1;
    for (int i = a.line + 1; i < b.line; i++) n += (size_t)d->lines[i].len + 1;
    return n + (size_t)b.col;
}

void bare__doc_range_copy(const BareDoc *d, BareTextPos a, BareTextPos b, char *out) {
    if (a.line == b.line) { memcpy(out, d->lines[a.line].s + a.col, (size_t)(b.col - a.col)); return; }
    size_t k = (size_t)(d->lines[a.line].len - a.col);
    memcpy(out, d->lines[a.line].s + a.col, k);
    out[k++] = '\n';
    for (int i = a.line + 1; i < b.line; i++) {
        memcpy(out + k, d->lines[i].s, (size_t)d->lines[i].len);
        k += (size_t)d->lines[i].len;
        out[k++] = '\n';
    }
    memcpy(out + k, d->lines[b.line].s, (size_t)b.col);
}

char *bare__doc_range(BareDoc *d, BareArena *arena, BareTextPos a, BareTextPos b, int *len) {
    a = bare__doc_clamp(d, a);
    b = bare__doc_clamp(d, b);
    if (bare__pos_cmp(a, b) > 0) { BareTextPos t = a; a = b; b = t; }
    size_t n = bare__doc_range_size(d, a, b);
    char *s = bare__arena_alloc(arena, n + 1, 1);
    if (!s) { if (len) *len = 0; return NULL; }
    bare__doc_range_copy(d, a, b, s);
    s[n] = 0;
    if (len) *len = (int)n;
    return s;
}

char *bare__doc_scratch(BareDoc *d, size_t n) {
    if (n > d->scratch_cap) {
        size_t cap = d->scratch_cap ? d->scratch_cap : 256;
        while (cap < n) cap *= 2;
        char *s = bare__alloc(&d->alloc, cap);
        if (!s) return NULL;
        bare__free(&d->alloc, d->scratch, d->scratch_cap);
        d->scratch = s;
        d->scratch_cap = cap;
    }
    return d->scratch;
}

const char *bare_doc_text(BareDoc *d, int *len) {
    if (!d) { if (len) *len = 0; return ""; }
    if (d->text && d->text_version == d->version) { if (len) *len = d->text_len; return d->text; }
    BareTextPos a = { 0, 0 }, b = { d->count - 1, d->lines[d->count - 1].len };
    size_t n = bare__doc_range_size(d, a, b);
    if (n + 1 > d->text_cap) {
        char *t = bare__alloc(&d->alloc, n + 1);
        if (!t) { if (len) *len = 0; return ""; }
        bare__free(&d->alloc, d->text, d->text_cap);
        d->text = t;
        d->text_cap = n + 1;
    }
    bare__doc_range_copy(d, a, b, d->text);
    d->text[n] = 0;
    d->text_len = (int)n;
    d->text_version = d->version;
    if (len) *len = (int)n;
    return d->text;
}

/* ---- selections ------------------------------------------------------------------ */

static BareTextPos shift_insert(BareTextPos q, BareTextPos at, BareTextPos end) {
    if (q.line == at.line && q.col >= at.col) return (BareTextPos){ end.line, end.col + (q.col - at.col) };
    if (q.line > at.line) q.line += end.line - at.line;
    return q;
}

static BareTextPos shift_delete(BareTextPos q, BareTextPos a, BareTextPos b) {
    if (bare__pos_cmp(q, a) <= 0) return q;
    if (bare__pos_cmp(q, b) <= 0) return a;
    if (q.line == b.line) return (BareTextPos){ a.line, a.col + (q.col - b.col) };
    q.line -= b.line - a.line;
    return q;
}

static BareTextPos sel_lo(const BareSel *s) { return bare__pos_cmp(s->anchor, s->caret) <= 0 ? s->anchor : s->caret; }
static BareTextPos sel_hi(const BareSel *s) { return bare__pos_cmp(s->anchor, s->caret) <= 0 ? s->caret : s->anchor; }

void bare__doc_sels_normalize(BareDoc *d) {
    BareSel main = d->sels[d->main];
    /* insertion sort by start: few selections, mostly sorted already */
    for (int i = 1; i < d->sel_count; i++) {
        BareSel s = d->sels[i];
        int j = i - 1;
        while (j >= 0 && bare__pos_cmp(sel_lo(&d->sels[j]), sel_lo(&s)) > 0) { d->sels[j + 1] = d->sels[j]; j--; }
        d->sels[j + 1] = s;
    }
    int n = 0;
    d->main = 0;
    for (int i = 0; i < d->sel_count; i++) {
        BareSel s = d->sels[i];
        bool is_main = !memcmp(&s.anchor, &main.anchor, sizeof s.anchor) && !memcmp(&s.caret, &main.caret, sizeof s.caret);
        if (n > 0) {
            BareSel *p = &d->sels[n - 1];
            BareTextPos plo = sel_lo(p), phi = sel_hi(p), lo = sel_lo(&s), hi = sel_hi(&s);
            int c = bare__pos_cmp(lo, phi);
            if (c < 0 || (c == 0 && (bare__pos_cmp(lo, hi) == 0 || bare__pos_cmp(plo, phi) == 0))) {
                BareTextPos nhi = bare__pos_cmp(hi, phi) > 0 ? hi : phi;
                bool fwd = bare__pos_cmp(p->anchor, p->caret) <= 0;
                p->anchor = fwd ? plo : nhi;
                p->caret = fwd ? nhi : plo;
                if (is_main) d->main = n - 1;
                continue;
            }
        }
        if (is_main) d->main = n;
        d->sels[n++] = s;
    }
    d->sel_count = n > 0 ? n : 1;
}

/* ---- undo -------------------------------------------------------------------------- */

static bool pool_add(BareDoc *d, const char *s, size_t n, size_t *at) {
    if (d->pool_len + n > d->pool_cap) {
        size_t cap = d->pool_cap ? d->pool_cap : 4096;
        while (cap < d->pool_len + n) cap *= 2;
        char *p = bare__realloc(&d->alloc, d->pool, d->pool_cap, cap);
        if (!p) return false;
        d->pool = p;
        d->pool_cap = cap;
    }
    if (n && s) memcpy(d->pool + d->pool_len, s, n);
    *at = d->pool_len;
    d->pool_len += n;
    return true;
}

static int sels_save(BareDoc *d) {
    if (!grow(d, (void **)&d->sel_pool, &d->sel_pool_cap, d->sel_pool_len + d->sel_count, sizeof *d->sel_pool))
        return -1;
    memcpy(d->sel_pool + d->sel_pool_len, d->sels, sizeof *d->sels * (size_t)d->sel_count);
    int at = d->sel_pool_len;
    d->sel_pool_len += d->sel_count;
    return at;
}

/* Forgets the oldest half of the history. */
static void trim(BareDoc *d) {
    int drop = d->group_count / 2;
    if (drop <= 0) return;
    const BareDocGroup *first = &d->groups[drop];
    int e0 = first->first;
    size_t p0 = e0 < d->edit_count ? d->edits[e0].text : d->pool_len;
    int s0 = first->sels_before;
    memmove(d->edits, d->edits + e0, sizeof *d->edits * (size_t)(d->edit_count - e0));
    d->edit_count -= e0;
    for (int i = 0; i < d->edit_count; i++) d->edits[i].text -= p0;
    memmove(d->pool, d->pool + p0, d->pool_len - p0);
    d->pool_len -= p0;
    memmove(d->sel_pool, d->sel_pool + s0, sizeof *d->sel_pool * (size_t)(d->sel_pool_len - s0));
    d->sel_pool_len -= s0;
    d->base_state = d->groups[drop - 1].id;
    memmove(d->groups, d->groups + drop, sizeof *d->groups * (size_t)(d->group_count - drop));
    d->group_count -= drop;
    d->undo_pos -= drop;
    for (int i = 0; i < d->group_count; i++) {
        d->groups[i].first -= e0;
        d->groups[i].sels_before -= s0;
        d->groups[i].sels_after -= s0;
    }
}

void bare__doc_begin(BareDoc *d, int kind) {
    if (d->open++ > 0 || d->replaying) return;
    if (d->undo_pos < d->group_count) {   /* a new change forgets what was undone */
        d->group_count = d->undo_pos;
        d->merge = false;
        if (d->group_count) {
            const BareDocGroup *g = &d->groups[d->group_count - 1];
            d->edit_count = g->first + g->count;
            d->sel_pool_len = g->sels_after + g->nafter;
            d->pool_len = d->edit_count ? d->edits[d->edit_count - 1].text + d->edits[d->edit_count - 1].len : 0;
        } else {
            d->edit_count = 0;
            d->sel_pool_len = 0;
            d->pool_len = 0;
        }
    }
    if (d->group_count >= UNDO_MAX) trim(d);
    if (!grow(d, (void **)&d->groups, &d->group_cap, d->group_count + 1, sizeof *d->groups)) { d->open_group = -1; return; }
    BareDocGroup *g = &d->groups[d->group_count];
    memset(g, 0, sizeof *g);
    g->first = d->edit_count;
    g->kind = (uint8_t)kind;
    g->sels_before = sels_save(d);
    g->nbefore = d->sel_count;
    d->open_group = g->sels_before < 0 ? -1 : d->group_count;
}

static bool same_sels(const BareSel *a, int na, const BareSel *b, int nb) {
    if (na != nb) return false;
    for (int i = 0; i < na; i++)
        if (bare__pos_cmp(a[i].caret, b[i].caret) || bare__pos_cmp(a[i].anchor, b[i].anchor)) return false;
    return true;
}

void bare__doc_end(BareDoc *d) {
    if (d->open <= 0 || --d->open > 0 || d->replaying) return;
    int gi = d->open_group;
    d->open_group = -1;
    if (gi < 0) return;
    BareDocGroup *g = &d->groups[gi];
    g->count = d->edit_count - g->first;
    if (g->count == 0) {   /* nothing changed: no step */
        d->sel_pool_len = g->sels_before;
        return;
    }
    g->sels_after = sels_save(d);
    g->nafter = d->sel_count;
    BareDocGroup *prev = gi > 0 ? &d->groups[gi - 1] : NULL;
    bool mergeable = g->kind != BARE__UNDO_OTHER && d->merge && prev && prev->kind == g->kind &&
                     d->undo_pos == gi && g->sels_after >= 0 &&
                     same_sels(d->sel_pool + prev->sels_after, prev->nafter, d->sel_pool + g->sels_before, g->nbefore);
    if (mergeable) {
        /* The run grows: the previous step now ends where this one does. */
        prev->count += g->count;
        int after = g->sels_after, n = g->nafter;
        memmove(d->sel_pool + prev->sels_after, d->sel_pool + after, sizeof *d->sel_pool * (size_t)n);
        prev->nafter = n;
        d->sel_pool_len = prev->sels_after + n;
        prev->id = d->next_state++;
        d->state = prev->id;
    } else {
        g->id = d->next_state++;
        d->state = g->id;
        d->group_count = gi + 1;
        d->undo_pos = d->group_count;
    }
    d->merge = g->kind != BARE__UNDO_OTHER;
}

static void record(BareDoc *d, bool insert, BareTextPos at, const char *s, size_t n, BareTextPos b) {
    if (d->replaying || d->open_group < 0 || d->open <= 0) return;
    if (!grow(d, (void **)&d->edits, &d->edit_cap, d->edit_count + 1, sizeof *d->edits)) return;
    BareDocEdit *e = &d->edits[d->edit_count];
    e->at = at;
    e->insert = insert;
    e->len = n;
    if (insert) {
        if (!pool_add(d, s, n, &e->text)) return;
    } else {
        if (!pool_add(d, NULL, n, &e->text)) return;
        bare__doc_range_copy(d, at, b, d->pool + e->text);
    }
    d->edit_count++;
}

/* ---- changes ------------------------------------------------------------------------ */

static BareTextPos insert_raw(BareDoc *d, BareTextPos at, const char *s, int n) {
    BareLine *l = &d->lines[at.line];
    const char *nl = memchr(s, '\n', (size_t)n);
    if (!nl) {
        if (!line_reserve(d, l, l->len + n)) return at;
        memmove(l->s + at.col + n, l->s + at.col, (size_t)(l->len - at.col));
        memcpy(l->s + at.col, s, (size_t)n);
        l->len += n;
        touched(d, at.line, false);
        return (BareTextPos){ at.line, at.col + n };
    }
    int breaks = 0;
    for (int i = 0; i < n; i++)
        if (s[i] == '\n') breaks++;
    if (!lines_insert(d, at.line + 1, breaks)) return at;
    l = &d->lines[at.line];
    /* The tail after the insertion point moves to the last new line. */
    int tail = l->len - at.col;
    int last_start = n;
    while (last_start > 0 && s[last_start - 1] != '\n') last_start--;
    BareLine *last = &d->lines[at.line + breaks];
    if (!line_reserve(d, last, (n - last_start) + tail)) return at;
    memcpy(last->s, s + last_start, (size_t)(n - last_start));
    memcpy(last->s + (n - last_start), l->s + at.col, (size_t)tail);
    last->len = (n - last_start) + tail;
    int first_len = (int)(nl - s);
    if (!line_reserve(d, l, at.col + first_len)) return at;
    memcpy(l->s + at.col, s, (size_t)first_len);
    l->len = at.col + first_len;
    int k = at.line + 1, p = first_len + 1;
    while (k < at.line + breaks) {
        int e = p;
        while (s[e] != '\n') e++;
        BareLine *m = &d->lines[k++];
        if (line_reserve(d, m, e - p)) {
            memcpy(m->s, s + p, (size_t)(e - p));
            m->len = e - p;
        }
        p = e + 1;
    }
    for (int i = at.line; i <= at.line + breaks; i++) touched(d, i, true);
    return (BareTextPos){ at.line + breaks, n - last_start };
}

static void delete_raw(BareDoc *d, BareTextPos a, BareTextPos b) {
    BareLine *l = &d->lines[a.line];
    if (a.line == b.line) {
        if (!line_reserve(d, l, l->len)) return;
        memmove(l->s + a.col, l->s + b.col, (size_t)(l->len - b.col));
        l->len -= b.col - a.col;
        touched(d, a.line, false);
        return;
    }
    BareLine *e = &d->lines[b.line];
    int tail = e->len - b.col;
    if (!line_reserve(d, l, a.col + tail)) return;
    e = &d->lines[b.line];
    memcpy(l->s + a.col, e->s + b.col, (size_t)tail);
    l->len = a.col + tail;
    lines_remove(d, a.line + 1, b.line - a.line);
    touched(d, a.line, true);
}

BareTextPos bare__doc_insert(BareDoc *d, BareTextPos at, const char *s, int n) {
    at = bare__doc_clamp(d, at);
    if (!s || n <= 0) return at;
    bool solo = d->open == 0;
    if (solo) bare__doc_begin(d, BARE__UNDO_OTHER);
    BareTextPos end = insert_raw(d, at, s, n);
    record(d, true, at, s, (size_t)n, end);
    for (int i = 0; i < d->sel_count; i++) {
        d->sels[i].anchor = shift_insert(d->sels[i].anchor, at, end);
        d->sels[i].caret = shift_insert(d->sels[i].caret, at, end);
    }
    if (solo) bare__doc_end(d);
    return end;
}

void bare__doc_delete(BareDoc *d, BareTextPos a, BareTextPos b) {
    a = bare__doc_clamp(d, a);
    b = bare__doc_clamp(d, b);
    if (bare__pos_cmp(a, b) > 0) { BareTextPos t = a; a = b; b = t; }
    if (!bare__pos_cmp(a, b)) return;
    bool solo = d->open == 0;
    if (solo) bare__doc_begin(d, BARE__UNDO_OTHER);
    record(d, false, a, NULL, bare__doc_range_size(d, a, b), b);
    delete_raw(d, a, b);
    for (int i = 0; i < d->sel_count; i++) {
        d->sels[i].anchor = shift_delete(d->sels[i].anchor, a, b);
        d->sels[i].caret = shift_delete(d->sels[i].caret, a, b);
    }
    if (solo) bare__doc_end(d);
}

static BareTextPos end_of(BareTextPos at, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') { at.line++; at.col = 0; }
        else at.col++;
    }
    return at;
}

bool bare__doc_undo(BareDoc *d, bool redo) {
    if (d->open > 0 || (redo ? d->undo_pos >= d->group_count : d->undo_pos <= 0)) return false;
    const BareDocGroup *g = &d->groups[redo ? d->undo_pos : d->undo_pos - 1];
    d->replaying = true;
    for (int k = 0; k < g->count; k++) {
        const BareDocEdit *e = &d->edits[redo ? g->first + k : g->first + g->count - 1 - k];
        const char *s = d->pool + e->text;
        if (e->insert == redo) insert_raw(d, e->at, s, (int)e->len);
        else delete_raw(d, e->at, end_of(e->at, s, e->len));
    }
    d->replaying = false;
    int from = redo ? g->sels_after : g->sels_before, n = redo ? g->nafter : g->nbefore;
    if (from >= 0 && grow(d, (void **)&d->sels, &d->sel_cap, n, sizeof *d->sels)) {
        memcpy(d->sels, d->sel_pool + from, sizeof *d->sels * (size_t)n);
        d->sel_count = n;
        d->main = n - 1;
        for (int i = 0; i < n; i++) {
            d->sels[i].anchor = bare__doc_clamp(d, d->sels[i].anchor);
            d->sels[i].caret = bare__doc_clamp(d, d->sels[i].caret);
        }
    }
    d->undo_pos += redo ? 1 : -1;
    d->state = d->undo_pos > 0 ? d->groups[d->undo_pos - 1].id : d->base_state;
    d->merge = false;
    return true;
}

bool bare_doc_undo(BareDoc *d) { return d && bare__doc_undo(d, false); }
bool bare_doc_redo(BareDoc *d) { return d && bare__doc_undo(d, true); }

/* ---- find ----------------------------------------------------------------------------- */

static bool word_byte(unsigned char c) {
    return c >= 0x80 || c == '_' || (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z');
}

static bool match_at(const char *s, int len, int at, const char *q, int qn, int flags) {
    if (at < 0 || at + qn > len) return false;
    if (flags & BARE_FIND_CASE) {
        if (memcmp(s + at, q, (size_t)qn)) return false;
    } else {
        for (int i = 0; i < qn; i++) {
            unsigned char a = (unsigned char)s[at + i], b = (unsigned char)q[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) return false;
        }
    }
    if (flags & BARE_FIND_WORD) {
        if (at > 0 && word_byte((unsigned char)s[at - 1]) && word_byte((unsigned char)q[0])) return false;
        if (at + qn < len && word_byte((unsigned char)s[at + qn]) && word_byte((unsigned char)q[qn - 1])) return false;
    }
    return true;
}

/* The first match at or after `from` (back: the last one starting before it). */
bool bare__doc_find_in_line(const char *s, int len, int from, const char *q, int qn, int flags, bool back, int *at) {
    if (qn <= 0) return false;
    if (back) {
        for (int i = (from > len ? len : from) - 1; i >= 0; i--)
            if (match_at(s, len, i, q, qn, flags)) { *at = i; return true; }
        return false;
    }
    for (int i = from < 0 ? 0 : from; i + qn <= len; i++)
        if (match_at(s, len, i, q, qn, flags)) { *at = i; return true; }
    return false;
}

bool bare_doc_find(const BareDoc *d, const char *query, int flags, BareTextPos from, BareTextPos *start,
                   BareTextPos *end) {
    if (!d || !query || !*query || strchr(query, '\n')) return false;
    int qn = (int)strlen(query);
    from = bare__doc_clamp(d, from);
    for (int l = from.line; l < d->count; l++) {
        int at;
        if (bare__doc_find_in_line(d->lines[l].s, d->lines[l].len, l == from.line ? from.col : 0, query, qn, flags,
                                   false, &at)) {
            if (start) *start = (BareTextPos){ l, at };
            if (end) *end = (BareTextPos){ l, at + qn };
            return true;
        }
    }
    return false;
}

/* ---- rows (soft wrap) ----------------------------------------------------------------- */

/* Prefix sums of the lines' screen rows: row_sum[i] = rows above line i. */
bool bare__doc_rows(BareDoc *d, uint32_t key) {
    if (!d->rows_dirty && d->rows_key == key) return true;
    if (!grow(d, (void **)&d->row_sum, &d->row_sum_cap, d->count + 1, sizeof *d->row_sum)) return false;
    int sum = 0;
    for (int i = 0; i < d->count; i++) {
        d->row_sum[i] = sum;
        sum += d->lines[i].rows ? d->lines[i].rows : 1;
    }
    d->row_sum[d->count] = sum;
    d->rows_dirty = false;
    d->rows_key = key;
    return true;
}
