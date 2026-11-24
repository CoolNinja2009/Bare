/* Documents and the editor (M7.5), headless.
 *
 * The document tests drive BareDoc directly (no context); the editor tests
 * use a fake monospace font: at size 10 every character is 5 px wide and a
 * line (line height 1.5) is 15 px tall. */
#define _CRT_SECURE_NO_WARNINGS   /* MSVC: plain fopen / sprintf are fine in a test */
#include "bare_test.h"
#include "bare.h"
#include "bare/bare_core.h"
#include "doc.h"
#include "edit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool text_is(BareDoc *d, const char *want) {
    int n;
    const char *s = bare_doc_text(d, &n);
    if ((int)strlen(want) == n && !memcmp(s, want, (size_t)n)) return true;
    printf("       got: \"%.*s\"\n", n, s);
    return false;
}

static BareDoc *doc_of(const char *s) {
    BareDoc *d = bare_doc_create();
    bare_doc_set_text(d, s, -1);
    return d;
}

static BareDocCmd cmd(BareDoc *d) {
    BareDocCmd c;
    memset(&c, 0, sizeof c);
    c.ctx = bare_ctx_current();
    c.doc = d;
    c.tab_size = 4;
    c.auto_close = "()[]{}\"\"";
    return c;
}

static void typ(BareDoc *d, const char *s) {
    BareDocCmd c = cmd(d);
    bare__doc_type(&c, s, (int)strlen(s));
}

static void run(BareDoc *d, BareEditAction a) {
    BareDocCmd c = cmd(d);
    c.comment = "// ";
    bare__doc_run(&c, a, false);
}

static BareTextPos P(int line, int col) { return (BareTextPos){ line, col }; }

/* ---- documents ---------------------------------------------------------------------- */

TEST(lines_load_and_save_with_their_endings) {
    const char *path = "test_doc_crlf.txt";
    FILE *f = fopen(path, "wb");
    CHECK(f);
    fwrite("\xEF\xBB\xBF" "one\r\ntwo\r\n", 1, 13, f);
    fclose(f);
    BareDoc *d = bare_doc_load(path);
    CHECK(d);
    CHECK(bare_doc_lines(d) == 3);
    CHECK(bare_doc_crlf(d));
    CHECK(text_is(d, "one\ntwo\n"));
    CHECK(!bare_doc_modified(d));
    bare_doc_select(d, P(1, 3), P(1, 3));
    bare_doc_insert(d, "!");
    CHECK(bare_doc_modified(d));
    CHECK(bare_doc_save(d, path));
    CHECK(!bare_doc_modified(d));
    f = fopen(path, "rb");
    char buf[64];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    CHECK(n == 14 && !memcmp(buf, "\xEF\xBB\xBF" "one\r\ntwo!\r\n", 14));
    bare_doc_free(d);
    remove(path);
    CHECK(bare_doc_load("no such file.txt") == NULL);
}

TEST(changes_move_every_selection_and_undo_restores_them) {
    BareDoc *d = doc_of("alpha\nbeta\ngamma");
    bare_doc_select(d, P(2, 2), P(2, 2));
    bare__doc_insert(d, P(0, 5), "X\nY", 3);
    CHECK(text_is(d, "alphaX\nY\nbeta\ngamma"));
    CHECK(bare_doc_caret(d).line == 3 && bare_doc_caret(d).col == 2);
    bare__doc_delete(d, P(0, 3), P(2, 1));
    CHECK(text_is(d, "alpeta\ngamma"));
    CHECK(bare_doc_caret(d).line == 1 && bare_doc_caret(d).col == 2);
    CHECK(bare_doc_undo(d));
    CHECK(text_is(d, "alphaX\nY\nbeta\ngamma"));
    CHECK(bare_doc_undo(d));
    CHECK(text_is(d, "alpha\nbeta\ngamma"));
    CHECK(!bare_doc_modified(d));
    CHECK(!bare_doc_undo(d));
    CHECK(bare_doc_redo(d) && bare_doc_redo(d));
    CHECK(text_is(d, "alpeta\ngamma"));
    CHECK(!bare_doc_redo(d));
    bare_doc_free(d);
}

TEST(typing_runs_undo_a_word_at_a_time) {
    BareDoc *d = doc_of("");
    typ(d, "h"); typ(d, "i"); typ(d, " "); typ(d, "y"); typ(d, "o");
    CHECK(text_is(d, "hi yo"));
    bare_doc_undo(d);
    CHECK(text_is(d, "hi "));
    bare_doc_undo(d);
    CHECK(text_is(d, ""));
    bare_doc_free(d);
}

TEST(pairs_close_wrap_and_are_typed_over) {
    BareDoc *d = doc_of("x");
    bare_doc_select(d, P(0, 1), P(0, 1));
    typ(d, "(");
    CHECK(text_is(d, "x()"));
    typ(d, "a"); typ(d, ")");
    CHECK(text_is(d, "x(a)"));
    CHECK(bare_doc_caret(d).col == 4);
    bare_doc_select(d, P(0, 0), P(0, 1));
    typ(d, "[");
    CHECK(text_is(d, "[x](a)"));
    int n;
    CHECK(!strcmp(bare_doc_selected(d, &n), "x"));
    bare_doc_select(d, P(0, 3), P(0, 3));
    typ(d, "{");
    run(d, BARE_EDIT_BACKSPACE);   /* {|} goes as a pair */
    CHECK(text_is(d, "[x](a)"));
    bare_doc_free(d);
}

TEST(enter_keeps_the_indentation_and_opens_braces) {
    BareDoc *d = doc_of("    if (x) {}");
    bare_doc_select(d, P(0, 12), P(0, 12));
    run(d, BARE_EDIT_ENTER);
    CHECK(text_is(d, "    if (x) {\n    \t\n    }"));
    CHECK(bare_doc_caret(d).line == 1 && bare_doc_caret(d).col == 5);
    bare_doc_free(d);
}

TEST(carets_type_together_and_select_next_matches) {
    BareDoc *d = doc_of("foo bar foo\nfoo");
    bare_doc_select(d, P(0, 1), P(0, 1));
    run(d, BARE_EDIT_SELECT_NEXT_MATCH);   /* the word under the caret */
    CHECK(bare_doc_selections(d) == 1);
    run(d, BARE_EDIT_SELECT_NEXT_MATCH);
    run(d, BARE_EDIT_SELECT_NEXT_MATCH);
    CHECK(bare_doc_selections(d) == 3);
    typ(d, "q");
    CHECK(text_is(d, "q bar q\nq"));
    run(d, BARE_EDIT_BLUR);   /* back to one caret */
    CHECK(bare_doc_selections(d) == 1);
    bare_doc_undo(d);
    CHECK(text_is(d, "foo bar foo\nfoo"));
    CHECK(bare_doc_selections(d) == 3);
    run(d, BARE_EDIT_SELECT_ALL_MATCHES);
    CHECK(bare_doc_selections(d) == 3);
    bare_doc_free(d);
}

TEST(line_commands) {
    BareDoc *d = doc_of("a\nb\nc");
    bare_doc_select(d, P(1, 0), P(1, 0));
    run(d, BARE_EDIT_MOVE_LINE_UP);
    CHECK(text_is(d, "b\na\nc"));
    CHECK(bare_doc_caret(d).line == 0);
    run(d, BARE_EDIT_MOVE_LINE_DOWN);
    run(d, BARE_EDIT_MOVE_LINE_DOWN);
    CHECK(text_is(d, "a\nc\nb"));
    CHECK(bare_doc_caret(d).line == 2);
    run(d, BARE_EDIT_COPY_LINE_DOWN);
    CHECK(text_is(d, "a\nc\nb\nb"));
    CHECK(bare_doc_caret(d).line == 3);
    run(d, BARE_EDIT_DELETE_LINE);
    CHECK(text_is(d, "a\nc\nb"));
    bare_doc_select(d, P(0, 0), P(1, 1));
    run(d, BARE_EDIT_COMMENT);
    CHECK(text_is(d, "// a\n// c\nb"));
    run(d, BARE_EDIT_COMMENT);
    CHECK(text_is(d, "a\nc\nb"));
    run(d, BARE_EDIT_TAB);   /* a selection over lines indents them */
    CHECK(text_is(d, "\ta\n\tc\nb"));
    run(d, BARE_EDIT_OUTDENT);
    CHECK(text_is(d, "a\nc\nb"));
    bare_doc_select(d, P(2, 0), P(2, 0));
    run(d, BARE_EDIT_SELECT_LINE);
    int n;
    CHECK(!strcmp(bare_doc_selected(d, &n), "b"));
    bare_doc_free(d);
}

TEST(paste_gives_each_caret_a_line) {
    BareDoc *d = doc_of("x\ny");
    bare__doc_command(d, BARE_EDIT_NONE, false);
    BareDocCmd c = cmd(d);
    bare_doc_select(d, P(0, 1), P(0, 1));
    bare__grow(&d->alloc, (void **)&d->sels, &d->sel_cap, 2, sizeof *d->sels);
    memset(&d->sels[1], 0, sizeof d->sels[1]);
    d->sels[1].anchor = d->sels[1].caret = P(1, 1);
    d->sel_count = 2;
    bare__doc_paste(&c, "1\r\n2", false);
    CHECK(text_is(d, "x1\ny2"));
    bare__doc_paste(&c, "-", false);
    CHECK(text_is(d, "x1-\ny2-"));
    bare_doc_free(d);
}

TEST(find_matches_case_and_whole_words) {
    BareDoc *d = doc_of("Cat cat\ncatalog cat");
    BareTextPos a, b;
    CHECK(bare_doc_find(d, "cat", 0, P(0, 0), &a, &b) && a.line == 0 && a.col == 0);
    CHECK(bare_doc_find(d, "cat", BARE_FIND_CASE, P(0, 0), &a, &b) && a.col == 4);
    CHECK(bare_doc_find(d, "cat", BARE_FIND_WORD | BARE_FIND_CASE, P(0, 5), &a, &b) && a.line == 1 && a.col == 8);
    CHECK(!bare_doc_find(d, "dog", 0, P(0, 0), &a, &b));
    bare_doc_free(d);
}

TEST(a_million_lines_load_as_one_block) {
    int lines = 1000000;
    char *s = malloc((size_t)lines * 8);
    CHECK(s);
    int k = 0;
    for (int i = 0; i < lines; i++) k += sprintf(s + k, "%06d\n", i % 1000000);
    BareDoc *d = bare_doc_create();
    bare_doc_set_text(d, s, k);
    free(s);
    CHECK(bare_doc_lines(d) == lines + 1);
    int n;
    const char *l = bare_doc_line(d, 777777, &n);
    CHECK(n == 6 && !memcmp(l, "777777", 6));
    CHECK(d->lines[777777].cap == 0);   /* still in the loaded block */
    bare_doc_select(d, P(777777, 6), P(777777, 6));
    bare_doc_insert(d, "!");
    l = bare_doc_line(d, 777777, &n);
    CHECK(n == 7 && l[6] == '!' && d->lines[777777].cap > 0);
    bare_doc_free(d);
}

/* ---- the editor ------------------------------------------------------------------------ */

static void *fake_load(const BareAllocator *a, const void *data, int size) {
    (void)a;
    static int handle;
    return size >= 4 && memcmp(data, "FAKE", 4) == 0 ? (void *)&handle : NULL;
}
static void fake_metrics(void *f, float px, float *ascent, float *descent, float *gap) {
    (void)f;
    *ascent = 0.8f * px;
    *descent = 0.2f * px;
    *gap = 0;
}
static int fake_shape(void *f, float px, const char *s, int len, BareShapedGlyph *out, int cap) {
    (void)f;
    int n = 0, i = 0;
    while (i < len && n < cap) {
        int at = i;
        uint32_t cp = bare__utf8_decode(s, len, &i);
        out[n++] = (BareShapedGlyph){ cp, cp == '\n' ? 0 : 0.5f * px, 0, 0, (uint32_t)at };
    }
    return n;
}
static unsigned char pixels[64 * 64];
static bool fake_rasterize(void *f, uint32_t glyph, float px, float shift, BareGlyphBitmap *out) {
    (void)f; (void)glyph; (void)shift;
    int w = (int)(0.5f * px), h = (int)(0.7f * px);
    memset(pixels, 255, sizeof pixels);
    *out = (BareGlyphBitmap){ w, h, 0, -h, pixels, 64 };
    return true;
}
static void fake_unload(void *f) { (void)f; }
static const BareFontBackend fake_backend = { fake_load, fake_metrics, fake_shape, fake_rasterize, fake_unload };

static BareContext *ctx;
static BareFont font;
static double now;
static bool again;
static const BareBox flat = { .padding = BARE_ZERO };

static void fresh(void) {
    if (ctx) bare_ctx_destroy(ctx);
    BareConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.fonts = &fake_backend;
    ctx = bare_ctx_create(&cfg);
    bare_ctx_make_current(ctx);
    bare_ctx_set_mac_keys(ctx, false);
    font = bare_font_from_memory("FAKE", 4);
    now = 0;
}

static void key(BareKey k, int mods) {
    BareEvent e = { .kind = BARE_EVENT_KEY };
    e.as.key.key = k; e.as.key.down = true; e.as.key.mods = mods;
    bare_ctx_event(ctx, &e);
    e.as.key.down = false;
    bare_ctx_event(ctx, &e);
}
static void type(const char *s) {
    BareEvent e = { .kind = BARE_EVENT_TEXT };
    e.as.text.utf8 = s;
    e.as.text.len = (int)strlen(s);
    bare_ctx_event(ctx, &e);
}
static void button(bool down, float x, float y) {
    BareEvent e = { .kind = BARE_EVENT_MOUSE_BUTTON, .time = now };
    e.as.button.button = BARE_MOUSE_LEFT; e.as.button.down = down; e.as.button.x = x; e.as.button.y = y;
    bare_ctx_event(ctx, &e);
}
static void click(float x, float y) {
    BareEvent e = { .kind = BARE_EVENT_MOUSE_MOVE };
    e.as.mouse.x = x; e.as.mouse.y = y;
    bare_ctx_event(ctx, &e);
    button(true, x, y);
    button(false, x, y);
}

static const BareDrawList *last;

static BareState frame(BareDoc *d, bool wrap) {
    now += 0.02;
    bare_ctx_begin(ctx, 400, 300, 1.0f, now);
    BareState s = bare_editor(d, .style = &flat, .font = font, .size = 10, .wrap = wrap, .tab_size = 4,
                              .auto_close = "()");
    last = bare_ctx_end(ctx, &again);
    return s;
}

TEST(the_editor_types_where_it_is_clicked) {
    fresh();
    BareDoc *d = doc_of("hello\nworld");
    frame(d, false);
    frame(d, false);
    click(10 + 2.4f * 5, 6 + 15 + 7);   /* line 2, between "wo" and "rld" (text starts at x 10, y 6) */
    frame(d, false);
    CHECK(bare_focused() != 0);
    CHECK(bare_doc_caret(d).line == 1 && bare_doc_caret(d).col == 2);
    type("-");
    key(BARE_KEY_TAB, 0);   /* the editor keeps Tab */
    BareState s = frame(d, false);
    CHECK(s.changed);
    CHECK(text_is(d, "hello\nwo-\trld"));
    key(BARE_KEY_Z, BARE_MOD_CTRL);
    frame(d, false);
    CHECK(text_is(d, "hello\nworld"));
    bare_doc_free(d);
}

TEST(only_the_lines_on_screen_are_declared) {
    fresh();
    char *s = malloc(200000 * 4);
    CHECK(s);
    int k = 0;
    for (int i = 0; i < 200000; i++) k += sprintf(s + k, "%c\n", 'a' + i % 26);
    BareDoc *d = bare_doc_create();
    bare_doc_set_text(d, s, k);
    free(s);
    frame(d, false);
    frame(d, false);
    CHECK(last && last->cmd_count < 400);
    CHECK(ctx->elem_count < 100);
    bare_doc_free(d);
}

TEST(wrapped_lines_take_rows_and_down_walks_them) {
    fresh();
    /* 400 px wide, text from x 10 with 10 px on the right: 76 characters a row, so 140 wrap once */
    char line[200];
    for (int i = 0; i < 140; i++) line[i] = (char)('a' + (i / 10) % 26);
    for (int i = 0; i < 140; i += 10) line[i] = ' ';
    line[140] = 0;
    BareDoc *d = bare_doc_create();
    bare_doc_set_text(d, line, -1);
    bare_doc_insert(d, "");
    frame(d, true);
    frame(d, true);
    frame(d, true);
    CHECK(d->lines[0].rows == 2);
    bare_doc_select(d, P(0, 0), P(0, 0));
    bare_focus(d->view.area);
    frame(d, true);
    key(BARE_KEY_DOWN, 0);
    frame(d, true);
    CHECK(bare_doc_caret(d).line == 0 && bare_doc_caret(d).col > 60);
    bare_doc_free(d);
}

int main(void) {
    RUN(lines_load_and_save_with_their_endings);
    RUN(changes_move_every_selection_and_undo_restores_them);
    RUN(typing_runs_undo_a_word_at_a_time);
    RUN(pairs_close_wrap_and_are_typed_over);
    RUN(enter_keeps_the_indentation_and_opens_braces);
    RUN(carets_type_together_and_select_next_matches);
    RUN(line_commands);
    RUN(paste_gives_each_caret_a_line);
    RUN(find_matches_case_and_whole_words);
    RUN(a_million_lines_load_as_one_block);
    RUN(the_editor_types_where_it_is_clicked);
    RUN(only_the_lines_on_screen_are_declared);
    RUN(wrapped_lines_take_rows_and_down_walks_them);
    if (ctx) bare_ctx_destroy(ctx);
    return test_report();
}
