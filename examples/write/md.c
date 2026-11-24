/* md.c: Markdown for the Write example (CommonMark's everyday subset):
 * headings, emphasis, strong, strike, inline code, links, images (as their
 * text), lists, task lists, quotes, fenced code, rules and pipe tables. */
#include "md.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- inline syntax ------------------------------------------------------------- */

enum { K_CODE, K_BOLD, K_ITALIC, K_BOLD_ITALIC, K_STRIKE, K_LINK, K_IMAGE };
typedef struct Inl { int a, b, ca, cb, kind; } Inl;   /* the whole span, its content */

static bool alnum(char c) { return (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z') || (c & 0x80); }

static int scan_inline(const char *s, int n, Inl *out, int cap) {
    int k = 0;
    for (int i = 0; i < n && k < cap;) {
        char c = s[i];
        if (c == '\\' && i + 1 < n) { i += 2; continue; }
        if (c == '`') {
            int j = i + 1;
            while (j < n && s[j] != '`') j++;
            if (j < n && j > i + 1) { out[k++] = (Inl){ i, j + 1, i + 1, j, K_CODE }; i = j + 1; continue; }
        } else if ((c == '*' || c == '_') && !(c == '_' && i > 0 && alnum(s[i - 1]))) {
            int r = 0;
            while (i + r < n && s[i + r] == c && r < 3) r++;
            int from = i + r;
            bool found = false;
            if (from < n && s[from] != ' ') {
                for (int j = from + 1; j + r <= n && !found; j++) {
                    bool run = true;
                    for (int q = 0; q < r; q++) if (s[j + q] != c) run = false;
                    if (!run || s[j - 1] == ' ' || (j + r < n && s[j + r] == c)) continue;
                    if (c == '_' && j + r < n && alnum(s[j + r])) continue;
                    out[k++] = (Inl){ i, j + r, from, j, r == 1 ? K_ITALIC : r == 2 ? K_BOLD : K_BOLD_ITALIC };
                    i = j + r;
                    found = true;
                }
            }
            if (!found) i += r;
            continue;
        } else if (c == '~' && i + 1 < n && s[i + 1] == '~') {
            int j = i + 2;
            while (j + 1 < n && !(s[j] == '~' && s[j + 1] == '~')) j++;
            if (j + 1 < n && j > i + 2) { out[k++] = (Inl){ i, j + 2, i + 2, j, K_STRIKE }; i = j + 2; continue; }
        } else if (c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '[')) {
            int o = c == '!' ? i + 1 : i, j = o + 1;
            while (j < n && s[j] != ']') j++;
            if (j + 1 < n && s[j + 1] == '(') {
                int u = j + 2;
                while (u < n && s[u] != ')') u++;
                if (u < n) {
                    out[k++] = (Inl){ i, u + 1, o + 1, j, c == '!' ? K_IMAGE : K_LINK };
                    i = u + 1;
                    continue;
                }
            }
        }
        i++;
    }
    return k;
}

static int leading(const char *s, int n) {
    int i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

/* "- ", "* ", "+ ", "12. ", "3) ": the marker's length with its space, else 0. */
static int list_marker(const char *t, int n) {
    if (n >= 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') return 2;
    int d = 0;
    while (d < n && d < 9 && t[d] >= '0' && t[d] <= '9') d++;
    if (d > 0 && d + 1 < n && (t[d] == '.' || t[d] == ')') && t[d + 1] == ' ') return d + 2;
    return 0;
}

static int task_box(const char *t, int n) {   /* "[ ] " / "[x] " after a marker: 1 open, 2 done */
    if (n >= 3 && t[0] == '[' && t[2] == ']' && (n == 3 || t[3] == ' ')) {
        if (t[1] == ' ') return 1;
        if (t[1] == 'x' || t[1] == 'X') return 2;
    }
    return 0;
}

static bool is_rule(const char *t, int n) {
    char c = n > 0 ? t[0] : 0;
    if (c != '-' && c != '*' && c != '_') return false;
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (t[i] == c) count++;
        else if (t[i] != ' ') return false;
    }
    return count >= 3;
}

static bool is_fence(const char *t, int n) { return n >= 3 && (!strncmp(t, "```", 3) || !strncmp(t, "~~~", 3)); }

static int heading(const char *t, int n) {
    int h = 0;
    while (h < n && t[h] == '#') h++;
    return h >= 1 && h <= 6 && (h == n || t[h] == ' ') ? h : 0;
}

/* ---- the highlighter --------------------------------------------------------------- */

typedef struct Out { BareHighlight *o; int k, cap; } Out;
typedef struct Sty { BareColor color, bg; int weight; bool italic, underline, strike; } Sty;

static void put(Out *o, int a, int b, Sty s) {
    if (b <= a || o->k >= o->cap) return;
    o->o[o->k++] = (BareHighlight){ .start = a, .end = b, .color = s.color, .background = s.bg, .weight = s.weight,
                                    .italic = s.italic, .underline = s.underline, .strike = s.strike };
}

static bool plain(Sty s) { return !s.color.a && !s.bg.a && !s.weight && !s.italic && !s.underline && !s.strike; }

static void inline_hl(Out *o, const MdLook *L, const char *s, int from, int n, Sty base) {
    Inl t[64];
    int m = scan_inline(s + from, n - from, t, 64), at = from;
    Sty mark = { .color = L->muted };
    for (int i = 0; i < m; i++) {
        int a = t[i].a + from, b = t[i].b + from, ca = t[i].ca + from, cb = t[i].cb + from;
        if (!plain(base)) put(o, at, a, base);
        Sty c = base;
        switch (t[i].kind) {
        case K_CODE: c.color = L->code; c.bg = L->code_bg; c.italic = false; break;
        case K_BOLD: c.weight = BARE_BOLD; break;
        case K_ITALIC: c.italic = true; break;
        case K_BOLD_ITALIC: c.weight = BARE_BOLD; c.italic = true; break;
        case K_STRIKE: c.strike = true; c.color = L->muted; break;
        default: c.color = L->accent; c.underline = true; break;
        }
        put(o, a, ca, mark);
        put(o, ca, cb, c);
        put(o, cb, b, mark);
        at = b;
    }
    if (!plain(base)) put(o, at, n, base);
}

int md_highlight(void *user, const char *s, int n, int *state, BareHighlight *out, int cap) {
    const MdLook *L = user;
    Out o = { out, 0, cap };
    int ind = leading(s, n);
    const char *t = s + ind;
    int tn = n - ind;
    Sty muted = { .color = L->muted }, accent = { .color = L->accent, .weight = BARE_BOLD };
    if (*state == 1) {   /* inside a fenced code block */
        if (is_fence(t, tn)) { *state = 0; put(&o, 0, n, muted); }
        else put(&o, 0, n, (Sty){ .color = L->code });
        return o.k;
    }
    if (is_fence(t, tn)) { *state = 1; put(&o, 0, n, muted); return o.k; }
    int h = heading(t, tn);
    if (h) {
        put(&o, ind, ind + h, (Sty){ .color = L->accent });
        inline_hl(&o, L, s, ind + h, n, (Sty){ .color = L->text, .weight = BARE_BOLD });
        return o.k;
    }
    if (is_rule(t, tn)) { put(&o, 0, n, muted); return o.k; }
    if (tn && t[0] == '>') {
        put(&o, ind, ind + 1, accent);
        inline_hl(&o, L, s, ind + 1, n, (Sty){ .color = L->muted, .italic = true });
        return o.k;
    }
    int m = list_marker(t, tn);
    if (m) {
        put(&o, ind, ind + m, accent);
        int at = ind + m, box = task_box(s + at, n - at);
        if (box) {
            put(&o, at, at + 3, (Sty){ .color = L->accent });
            at += at + 3 < n ? 4 : 3;
        }
        inline_hl(&o, L, s, at, n, box == 2 ? (Sty){ .color = L->muted } : (Sty){ 0 });
        return o.k;
    }
    if (tn && t[0] == '|') {   /* a table row: the pipes dimmed */
        int at = 0;
        for (int i = 0; i <= n; i++) {
            if (i < n && s[i] != '|') continue;
            inline_hl(&o, L, s, at, i, (Sty){ 0 });
            if (i < n) put(&o, i, i + 1, muted);
            at = i + 1;
        }
        return o.k;
    }
    inline_hl(&o, L, s, 0, n, (Sty){ 0 });
    return o.k;
}

/* ---- the indenter --------------------------------------------------------------- */

/* Enter continues a list or a quote; on an empty item it ends it instead. */
int md_indent(void *user, const char *s, int n, int col, char *out, int cap) {
    (void)user;
    int ind = leading(s, n), k = 0;
    if (ind > col) ind = col;
    for (int i = 0; i < ind && k < cap; i++) out[k++] = s[i];
    const char *t = s + ind;
    int tn = n - ind;
    if (col <= ind || cap - k < 16) return k;
    if (tn && t[0] == '>') {
        int rest = 1 + leading(t + 1, tn - 1);
        if (rest >= tn) return -1;
        memcpy(out + k, "> ", 2);
        return k + 2;
    }
    int m = list_marker(t, tn);
    if (!m) return k;
    int box = task_box(t + m, tn - m);
    int after = m + (box ? 4 : 0);
    if (after >= tn || leading(t + after, tn - after) == tn - after) return -1;
    if (t[0] >= '0' && t[0] <= '9') k += snprintf(out + k, (size_t)(cap - k), "%d%c ", atoi(t) + 1, t[m - 2]);
    else { out[k++] = t[0]; out[k++] = ' '; }
    if (box && cap - k >= 4) { memcpy(out + k, "[ ] ", 4); k += 4; }
    return k;
}

/* ---- the preview ------------------------------------------------------------------- */

static char *scratch;
static size_t scratch_cap;

/* A NUL-terminated copy (Bare copies text when it's declared). Backslash
 * escapes lose their backslash. */
static const char *cstr(const char *s, int n) {
    if ((size_t)n + 1 > scratch_cap) {
        scratch_cap = (size_t)n + 256;
        char *p = realloc(scratch, scratch_cap);
        if (!p) return "";
        scratch = p;
    }
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n && !alnum(s[i + 1]) && s[i + 1] != ' ') i++;
        scratch[k++] = s[i];
    }
    scratch[k] = 0;
    return scratch;
}

static void spans(const MdLook *L, const char *s, int n, const BareTextStyle *base) {
    Inl t[128];
    int m = scan_inline(s, n, t, 128), at = 0;
    for (int i = 0; i < m; i++) {
        if (t[i].a > at) bare_span(cstr(s + at, t[i].a - at), .style = base);
        const char *c = cstr(s + t[i].ca, t[i].cb - t[i].ca);
        switch (t[i].kind) {
        case K_CODE:
            bare_span(c, .style = base, .font = BARE_MONO, .size = base->size * 0.88f, .color = L->code,
                      .background = L->code_bg);
            break;
        case K_BOLD: bare_span(c, .style = base, .weight = BARE_BOLD); break;
        case K_ITALIC: bare_span(c, .style = base, .italic = true); break;
        case K_BOLD_ITALIC: bare_span(c, .style = base, .weight = BARE_BOLD, .italic = true); break;
        case K_STRIKE: bare_span(c, .style = base, .strike = true, .color = L->muted); break;
        case K_IMAGE: bare_span(c, .style = base, .italic = true, .color = L->muted); break;
        default: bare_span(c, .style = base, .color = L->accent, .underline = true); break;
        }
        at = t[i].b;
    }
    if (at < n) bare_span(cstr(s + at, n - at), .style = base);
}

static void paragraph_of(const MdLook *L, const char *s, int n, const BareTextStyle *base) {
    bare_paragraph(.style = base) { spans(L, s, n, base); }
}

/* Ticks a task box in the document, keeping the caret where it was. */
static void toggle_task(BareDoc *doc, int line, int col, bool done) {
    BareTextPos keep = bare_doc_caret(doc);
    bare_doc_select(doc, (BareTextPos){ line, col }, (BareTextPos){ line, col + 1 });
    bare_doc_insert(doc, done ? " " : "x");
    bare_doc_select(doc, keep, keep);
}

static void task(BareDoc *doc, const MdLook *L, int line, int col, bool done) {
    bare_row(.width = 18, .height = 18, .radius = 5, .align = BARE_CENTER, .justify = BARE_CENTER,
             .border = done ? BARE_ZERO : 1.5f, .border_color = L->muted, .color = done ? L->accent : BARE_CLEAR,
             .cursor = BARE_CURSOR_HAND, .hover = { .scale = 1.1f }, .press = { .scale = 0.9f },
             .animate = BARE_SPRING_SNAPPY, .id = BARE_IDI("md.task", line)) {
        if (bare_this().clicked) toggle_task(doc, line, col, done);
        bare_row(.opacity = done ? 1.0f : BARE_ZERO, .scale = done ? 1.0f : 0.4f, .animate = BARE_SPRING_SNAPPY) {
            bare_text("\xE2\x9C\x93", .size = 12, .weight = BARE_BOLD, .color = BARE_WHITE, .line_height = 1.0f);
        }
    }
}

static int cells(const char *s, int n, int *a, int *b, int cap) {   /* a table row's cells, trimmed */
    int k = 0, i = 0;
    if (i < n && s[i] == '|') i++;
    while (i < n && k < cap) {
        int j = i;
        while (j < n && s[j] != '|') j++;
        int x = i, y = j;
        while (x < y && s[x] == ' ') x++;
        while (y > x && s[y - 1] == ' ') y--;
        if (j < n || y > x) { a[k] = x; b[k] = y; k++; }
        i = j + 1;
    }
    return k;
}

void md_preview(BareDoc *doc, const MdLook *L) {
    float size = L->size;
    BareTextStyle body = { .font = L->font, .size = size, .line_height = 1.65f, .color = L->text };
    int count = bare_doc_lines(doc);
    for (int l = 0; l < count;) {
        int n;
        const char *s = bare_doc_line(doc, l, &n);
        int ind = leading(s, n);
        const char *t = s + ind;
        int tn = n - ind;
        if (tn == 0) { l++; continue; }

        if (is_fence(t, tn)) {   /* fenced code */
            const char *lang = cstr(t + 3, tn - 3);
            char label[32];
            snprintf(label, sizeof label, "%s", lang);
            bare_column(.width = BARE_GROW, .padding = 16, .radius = 12, .color = L->code_bg, .clip = true,
                        .gap = 0) {
                if (label[0]) bare_text(label, .size = size * 0.72f, .color = L->muted);
                for (l++; l < count; l++) {
                    s = bare_doc_line(doc, l, &n);
                    if (is_fence(s + leading(s, n), n - leading(s, n))) { l++; break; }
                    bare_text(cstr(s, n), .font = BARE_MONO, .size = size * 0.86f, .line_height = 1.55f,
                              .color = L->code, .nowrap = true);
                }
            }
            continue;
        }
        int h = heading(t, tn);
        if (h) {
            static const float scale[] = { 0, 2.0f, 1.55f, 1.28f, 1.12f, 1.0f, 0.92f };
            int at = h;
            while (at < tn && t[at] == ' ') at++;
            int end = tn;
            while (end > at && (t[end - 1] == '#' || t[end - 1] == ' ')) end--;
            BareTextStyle hs = body;
            hs.size = size * scale[h];
            hs.weight = h <= 2 ? BARE_BOLD : BARE_SEMIBOLD;
            hs.line_height = 1.25f;
            bare_box(.height = h <= 2 ? size * 0.7f : size * 0.3f);
            paragraph_of(L, t + at, end - at, &hs);
            if (h <= 2) bare_box(.width = BARE_GROW, .height = 1, .color = L->rule);
            l++;
            continue;
        }
        if (is_rule(t, tn)) {
            bare_box(.width = BARE_GROW, .height = 1, .color = L->rule);
            l++;
            continue;
        }
        if (t[0] == '>') {   /* a quote: its lines joined */
            bare_row(.width = BARE_GROW, .gap = 14) {
                bare_box(.width = 3, .height = BARE_GROW, .radius = 1.5f, .color = L->accent);
                BareTextStyle qs = body;
                qs.italic = true;
                qs.color = L->muted;
                bare_column(.width = BARE_GROW, .gap = 6) {
                    for (; l < count; l++) {
                        s = bare_doc_line(doc, l, &n);
                        ind = leading(s, n);
                        if (ind >= n || s[ind] != '>') break;
                        int at = ind + 1;
                        while (at < n && s[at] == ' ') at++;
                        if (at < n) paragraph_of(L, s + at, n - at, &qs);
                    }
                }
            }
            continue;
        }
        int m = list_marker(t, tn);
        if (m) {   /* a list: one row per item, nested by indentation */
            bare_column(.width = BARE_GROW, .gap = 4) {
                for (; l < count; l++) {
                    s = bare_doc_line(doc, l, &n);
                    ind = leading(s, n);
                    m = list_marker(s + ind, n - ind);
                    if (!m) break;
                    int at = ind + m, box = task_box(s + at, n - at);
                    bare_row(.width = BARE_GROW, .gap = 10, .pad = { 0, 0, 0, (float)ind * size * 0.5f }) {
                        if (box) {
                            bare_row(.height = size * 1.65f, .align = BARE_CENTER) { task(doc, L, l, at + 1, box == 2); }
                            at += at + 3 < n ? 4 : 3;
                        } else if (s[ind] >= '0' && s[ind] <= '9') {
                            bare_text(cstr(s + ind, m - 1), .style = &body, .color = L->accent, .weight = BARE_SEMIBOLD,
                                      .nowrap = true);
                        } else {
                            bare_text("\xE2\x80\xA2", .style = &body, .color = L->accent, .weight = BARE_BOLD);
                        }
                        BareTextStyle is = body;
                        if (box == 2) { is.color = L->muted; is.strike = true; }
                        bare_column(.width = BARE_GROW) { paragraph_of(L, s + at, n - at, &is); }
                    }
                }
            }
            continue;
        }
        if (t[0] == '|') {   /* a table: the first row is the header, the --- row is skipped */
            bare_column(.width = BARE_GROW, .radius = 10, .border = 1, .border_color = L->rule, .clip = true) {
                for (int row = 0; l < count; l++) {
                    s = bare_doc_line(doc, l, &n);
                    ind = leading(s, n);
                    if (ind >= n || s[ind] != '|') break;
                    int a[16], b[16], k = cells(s + ind, n - ind, a, b, 16);
                    bool sep = k > 0;
                    for (int c = 0; c < k; c++)
                        for (int i = a[c]; i < b[c]; i++)
                            if (s[ind + i] != '-' && s[ind + i] != ':') sep = false;
                    if (sep) continue;
                    BareTextStyle cs = body;
                    if (row == 0) cs.weight = BARE_SEMIBOLD;
                    bare_row(.width = BARE_GROW, .color = row == 0 ? L->code_bg : BARE_CLEAR) {
                        for (int c = 0; c < k; c++)
                            bare_column(.width = BARE_GROW, .pad = { 8, 12, 8, 12 }) {
                                paragraph_of(L, s + ind + a[c], b[c] - a[c], &cs);
                            }
                    }
                    row++;
                }
            }
            continue;
        }
        /* a paragraph: lines until a blank line or a block starts; two spaces at the end break the line */
        bare_paragraph(.style = &body) {
            for (bool first = true; l < count; l++, first = false) {
                s = bare_doc_line(doc, l, &n);
                ind = leading(s, n);
                t = s + ind;
                tn = n - ind;
                if (!first && (tn == 0 || is_fence(t, tn) || heading(t, tn) || t[0] == '>' || list_marker(t, tn) ||
                               t[0] == '|' || is_rule(t, tn)))
                    break;
                if (tn == 0) break;
                if (!first) bare_span(" ", .style = &body);
                bool hard = n >= 2 && s[n - 1] == ' ' && s[n - 2] == ' ';
                spans(L, t, tn, &body);
                if (hard) bare_span("\n", .style = &body);
            }
        }
    }
}
