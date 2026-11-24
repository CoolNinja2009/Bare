/* code.c: bare_editor as a code editor (M7.5): a C highlighter, line
 * numbers, find (Ctrl+F), multiple carets (Ctrl+D, Alt+click), and a
 * million-line log that scrolls like a short file.
 *     code [file.c]     (default: this file) */
#include "bare.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *keywords[] = { "if", "else", "for", "while", "do", "switch", "case", "default", "break",
    "continue", "return", "goto", "sizeof", "typedef", "struct", "union", "enum", "static", "const", "extern",
    "volatile", "inline", "restrict", NULL };
static const char *types[] = { "void", "char", "short", "int", "long", "float", "double", "signed", "unsigned",
    "bool", "size_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int64_t", NULL };

static bool word_in(const char *s, int n, const char **list) {
    for (int i = 0; list[i]; i++)
        if ((int)strlen(list[i]) == n && !memcmp(list[i], s, (size_t)n)) return true;
    return false;
}

static bool ident(char c) { return c == '_' || (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z'); }

/* One line of C. *state is 1 inside a block comment. */
static int highlight_c(void *user, const char *s, int n, int *state, BareHighlight *out, int cap) {
    (void)user;
    const BareColor comment = BARE_RGB(110, 140, 120), keyword = BARE_RGB(200, 140, 255),
                    type = BARE_RGB(100, 190, 255), string = BARE_RGB(240, 190, 120),
                    number = BARE_RGB(250, 150, 140), macro = BARE_RGB(255, 120, 190);
    int k = 0, i = 0;
#define SPAN(from, to, col, bold, slant) do { \
        if (k < cap) out[k++] = (BareHighlight){ .start = (from), .end = (to), .color = (col), \
                                                 .weight = (bold) ? BARE_BOLD : 0, .italic = (slant) }; \
    } while (0)
    if (*state == 1) {   /* the rest of a block comment */
        while (i < n && !(s[i] == '*' && i + 1 < n && s[i + 1] == '/')) i++;
        if (i < n) { i += 2; *state = 0; }
        SPAN(0, i, comment, false, true);
    }
    int first = i;
    while (first < n && (s[first] == ' ' || s[first] == '\t')) first++;
    if (*state == 0 && first < n && s[first] == '#') {
        SPAN(first, n, macro, false, false);
        return k;
    }
    while (i < n && *state == 0) {
        char c = s[i];
        int a = i;
        if (c == '/' && i + 1 < n && s[i + 1] == '/') { SPAN(i, n, comment, false, true); break; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i < n && !(s[i] == '*' && i + 1 < n && s[i + 1] == '/')) i++;
            if (i < n) i += 2; else *state = 1;
            SPAN(a, i, comment, false, true);
        } else if (c == '"' || c == '\'') {
            for (i++; i < n && s[i] != c; i++) if (s[i] == '\\') i++;
            if (i < n) i++;
            SPAN(a, i, string, false, false);
        } else if (c >= '0' && c <= '9') {
            while (i < n && (ident(s[i]) || s[i] == '.')) i++;
            SPAN(a, i, number, false, false);
        } else if (ident(c)) {
            while (i < n && ident(s[i])) i++;
            if (word_in(s + a, i - a, keywords)) SPAN(a, i, keyword, true, false);
            else if (word_in(s + a, i - a, types)) SPAN(a, i, type, false, false);
            else if (i - a > 5 && !memcmp(s + a, "bare_", 5)) SPAN(a, i, BARE_RGB(120, 220, 200), false, false);
        } else {
            i++;
        }
    }
    return k;
#undef SPAN
}

int main(int argc, char **argv) {
    bare_window(1100, 760, "Bare: code");
    const char *path = argc > 1 ? argv[1] : __FILE__;
    BareDoc *source = bare_doc_load(path), *log = NULL, *doc = source;
    if (!source) { source = doc = bare_doc_create(); bare_doc_set_text(doc, "// couldn't open the file", -1); }
    bool wrap = false;
    while (bare_frame()) {
        const BareTheme *t = bare_theme();
        bare_column(.width = BARE_GROW, .height = BARE_GROW, .padding = 16, .gap = 12) {
            bare_row(.gap = 8, .align = BARE_CENTER) {
                if (bare_button(path, .style = doc == source ? &t->primary : &t->button).clicked) doc = source;
                if (bare_button("1,000,000-line log", .style = doc == log ? &t->primary : &t->button).clicked) {
                    if (!log) {   /* generated once: about 40 MB */
                        size_t cap = 1000000u * 48, k = 0;
                        char *s = malloc(cap);
                        for (int i = 0; s && i < 1000000; i++)
                            k += (size_t)snprintf(s + k, cap - k, "%07d  12:%02d:%02d  INFO  request %d done\n", i + 1,
                                                  i / 60 % 60, i % 60, i * 7 % 9973);
                        log = bare_doc_create();
                        bare_doc_set_text(log, s ? s : "", (int)k);
                        free(s);
                    }
                    doc = log;
                }
                bare_checkbox("Wrap", &wrap);
                bare_box(.width = BARE_GROW);
                BareTextPos c = bare_doc_caret(doc);
                char status[96];
                snprintf(status, sizeof status, "Ln %d, Col %d%s%s", c.line + 1, c.col + 1,
                         bare_doc_selections(doc) > 1 ? "  (multiple carets)" : "", bare_doc_modified(doc) ? "  *" : "");
                bare_text(status, .style = &t->caption);
            }
            bare_editor(doc, .line_numbers = true, .highlight_line = true, .wrap = wrap, .ruler = 120,
                        .comment = "// ", .auto_close = "()[]{}\"\"''", .spaces = true,
                        .highlight = doc == source ? highlight_c : NULL);
        }
    }
    bare_doc_free(source);
    bare_doc_free(log);
    bare_quit();
}
