/* Text fields and focus (M7.5), headless: synthetic keys, text, IME and clicks.
 *
 * A fake monospace font makes positions exact: at size 10 every character is
 * 5 px wide and a line is 13 px tall. Fields use a style without padding, so
 * text starts at the field's top left. */
#include "bare_test.h"
#include "bare.h"
#include "bare/bare_core.h"
#include "edit.h"
#include <string.h>

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
static const BareBox plain = { .width = 200 };   /* no padding, no border */

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

static void begin(void) { bare_ctx_begin(ctx, 800, 600, 1.0f, now); }
static void end(void) { bare_ctx_end(ctx, &again); }

/* ---- events ---- */

static void key(BareKey k, int mods) {
    BareEvent e = { .kind = BARE_EVENT_KEY };
    e.as.key.key = k; e.as.key.down = true; e.as.key.mods = mods;
    bare_ctx_event(ctx, &e);
    e.as.key.down = false;
    bare_ctx_event(ctx, &e);
}
static void hold(BareKey k, bool down) {
    BareEvent e = { .kind = BARE_EVENT_KEY };
    e.as.key.key = k; e.as.key.down = down;
    bare_ctx_event(ctx, &e);
}
static void type(const char *s) {
    BareEvent e = { .kind = BARE_EVENT_TEXT };
    e.as.text.utf8 = s; e.as.text.len = (int)strlen(s); e.as.text.cursor = e.as.text.len;
    bare_ctx_event(ctx, &e);
}
static void compose(const char *s, int cursor) {
    BareEvent e = { .kind = BARE_EVENT_COMPOSITION };
    e.as.text.utf8 = s; e.as.text.len = (int)strlen(s); e.as.text.cursor = cursor;
    bare_ctx_event(ctx, &e);
}
static void mouse_to(float x, float y) {
    BareEvent e = { .kind = BARE_EVENT_MOUSE_MOVE };
    e.as.mouse.x = x; e.as.mouse.y = y;
    bare_ctx_event(ctx, &e);
}
static void button(bool down, float x, float y) {
    BareEvent e = { .kind = BARE_EVENT_MOUSE_BUTTON, .time = now };
    e.as.button.button = BARE_MOUSE_LEFT; e.as.button.down = down; e.as.button.x = x; e.as.button.y = y;
    bare_ctx_event(ctx, &e);
}
static void click(float x, float y) { button(true, x, y); button(false, x, y); }

/* ---- one field ---- */

static char buf[64];
static BareState st;
static int cap = sizeof buf;
static bool multi, pass, tabs, ro;
static float width = 200;

static void frame(void) {
    begin();
    st = bare_input(buf, cap, .id = BARE_ID("f"), .style = &plain, .width = width, .size = 10, .font = font,
                    .multiline = multi, .password = pass, .tabs = tabs, .read_only = ro, .lines = 3);
    end();
}

static void field(const char *text) {
    fresh();
    multi = pass = tabs = ro = false;
    cap = sizeof buf;
    width = 200;
    snprintf(buf, sizeof buf, "%s", text);
    frame();
    bare_focus(BARE_ID("f"));
    frame();
}

static BareEditState *ed(void) {
    int n = bare__store_find(ctx, BARE_ID("f"));
    return n >= 0 ? ctx->store.nodes[n].edit : NULL;
}
static int caret(void) { return ed() ? ed()->caret : -1; }
static int anchor(void) { return ed() ? ed()->anchor : -1; }
static void set_caret(int at) { ed()->caret = ed()->anchor = at; }

/* ---- tests ---- */

TEST(typed_text_lands_at_the_caret) {
    field("hello");
    CHECK(st.focused && caret() == 5);
    set_caret(2);
    type("é");
    type("x");
    frame();
    CHECK(strcmp(buf, "he\xC3\xA9xllo") == 0);
    CHECK(caret() == 5 && st.changed);
    frame();
    CHECK(!st.changed);
}

TEST(backspace_and_delete_remove_whole_code_points) {
    field("a\xE2\x82\xAC" "b");   /* a€b */
    set_caret(4);
    key(BARE_KEY_BACKSPACE, 0);
    frame();
    CHECK(strcmp(buf, "ab") == 0 && caret() == 1);
    snprintf(buf, sizeof buf, "x\xF0\x9F\x98\x80y");   /* an emoji: four bytes */
    frame();
    set_caret(1);
    key(BARE_KEY_DELETE, 0);
    frame();
    CHECK(strcmp(buf, "xy") == 0 && caret() == 1);
}

TEST(arrows_move_by_code_point_and_collapse_selections) {
    field("a\xC3\xA9z");
    set_caret(0);
    key(BARE_KEY_RIGHT, 0);
    key(BARE_KEY_RIGHT, 0);
    frame();
    CHECK(caret() == 3);
    key(BARE_KEY_LEFT, BARE_MOD_SHIFT);
    frame();
    CHECK(caret() == 1 && anchor() == 3);
    key(BARE_KEY_RIGHT, 0);   /* collapses to the selection's end */
    frame();
    CHECK(caret() == 3 && anchor() == 3);
}

TEST(word_jumps_follow_the_platform) {
    field("one two, three");
    set_caret(0);
    key(BARE_KEY_RIGHT, BARE_MOD_CTRL);
    frame();
    CHECK(caret() == 4);                     /* Windows: the next word's start */
    key(BARE_KEY_RIGHT, BARE_MOD_CTRL);
    frame();
    CHECK(caret() == 7);                     /* "two" then the comma is its own run */
    key(BARE_KEY_LEFT, BARE_MOD_CTRL);
    frame();
    CHECK(caret() == 4);
    key(BARE_KEY_RIGHT, BARE_MOD_CTRL | BARE_MOD_SHIFT);
    frame();
    CHECK(anchor() == 4 && caret() == 7);    /* Ctrl+Shift+→ selects to it */

    bare_ctx_set_mac_keys(ctx, true);        /* the bindings reload for macOS */
    set_caret(0);
    key(BARE_KEY_RIGHT, BARE_MOD_ALT);
    frame();
    CHECK(caret() == 3);                     /* macOS: the word's end */
    key(BARE_KEY_RIGHT, BARE_MOD_SUPER);
    frame();
    CHECK(caret() == 14);                    /* Cmd+→: the line's end */
    key(BARE_KEY_BACKSPACE, BARE_MOD_ALT);
    frame();
    CHECK(strcmp(buf, "one two, ") == 0);    /* Option+Backspace: a word */
}

TEST(home_is_smart_and_end_goes_to_the_end) {
    field("   indented");
    set_caret(8);
    key(BARE_KEY_HOME, 0);
    frame();
    CHECK(caret() == 3);                     /* first non-space */
    key(BARE_KEY_HOME, 0);
    frame();
    CHECK(caret() == 0);                     /* then column 0 */
    key(BARE_KEY_END, BARE_MOD_SHIFT);
    frame();
    CHECK(anchor() == 0 && caret() == 11);
}

TEST(selecting_then_typing_replaces_the_selection) {
    field("hello world");
    set_caret(6);
    for (int i = 0; i < 5; i++) key(BARE_KEY_RIGHT, BARE_MOD_SHIFT);
    type("there");
    frame();
    CHECK(strcmp(buf, "hello there") == 0 && caret() == 11);
    key(BARE_KEY_A, BARE_MOD_CTRL);
    key(BARE_KEY_DELETE, 0);
    frame();
    CHECK(buf[0] == 0 && caret() == 0);
}

TEST(the_cap_is_respected_without_splitting_a_code_point) {
    field("");
    cap = 6;   /* 5 bytes of text */
    type("abc");
    type("\xC3\xA9\xC3\xA9");   /* éé: only one fits... no: 2 bytes left, one é */
    frame();
    CHECK(strcmp(buf, "abc\xC3\xA9") == 0 && caret() == 5);
    type("z");
    frame();
    CHECK(strcmp(buf, "abc\xC3\xA9") == 0);
    cap = 5;
    snprintf(buf, sizeof buf, "abcd");
    frame();
    type("\xE2\x82\xAC");   /* € is three bytes; none fit */
    frame();
    CHECK(strcmp(buf, "abcd") == 0);
}

TEST(undo_and_redo_restore_text_and_caret) {
    field("");
    type("hello");
    type(" ");
    type("w");
    type("o");
    frame();
    CHECK(strcmp(buf, "hello wo") == 0);
    key(BARE_KEY_Z, BARE_MOD_CTRL);
    frame();
    CHECK(strcmp(buf, "hello ") == 0 && caret() == 6);   /* the last word's typing run */
    key(BARE_KEY_Z, BARE_MOD_CTRL);
    frame();
    CHECK(buf[0] == 0 && caret() == 0);
    key(BARE_KEY_Y, BARE_MOD_CTRL);
    frame();
    CHECK(strcmp(buf, "hello ") == 0 && caret() == 6);
    key(BARE_KEY_Z, BARE_MOD_CTRL | BARE_MOD_SHIFT);
    frame();
    CHECK(strcmp(buf, "hello wo") == 0 && caret() == 8);

    set_caret(3);
    key(BARE_KEY_RIGHT, BARE_MOD_SHIFT);
    key(BARE_KEY_RIGHT, BARE_MOD_SHIFT);
    key(BARE_KEY_BACKSPACE, 0);
    frame();
    CHECK(strcmp(buf, "hel wo") == 0);
    key(BARE_KEY_Z, BARE_MOD_CTRL);
    frame();
    CHECK(strcmp(buf, "hello wo") == 0 && anchor() == 3 && caret() == 5);   /* the selection comes back */
}

TEST(backspace_runs_undo_as_one_step) {
    field("abcdef");
    key(BARE_KEY_BACKSPACE, 0);
    key(BARE_KEY_BACKSPACE, 0);
    frame();
    key(BARE_KEY_BACKSPACE, 0);
    frame();
    CHECK(strcmp(buf, "abc") == 0);
    key(BARE_KEY_Z, BARE_MOD_CTRL);
    frame();
    CHECK(strcmp(buf, "abcdef") == 0 && caret() == 6);
}

TEST(cut_copy_paste) {
    field("copy me");
    key(BARE_KEY_A, BARE_MOD_CTRL);
    key(BARE_KEY_C, BARE_MOD_CTRL);
    key(BARE_KEY_END, 0);
    key(BARE_KEY_V, BARE_MOD_CTRL);
    frame();
    CHECK(strcmp(buf, "copy mecopy me") == 0);
    set_caret(0);
    key(BARE_KEY_RIGHT, BARE_MOD_CTRL | BARE_MOD_SHIFT);
    key(BARE_KEY_X, BARE_MOD_CTRL);
    frame();
    CHECK(strcmp(buf, "me" "copy me") == 0);
    CHECK(strcmp(bare__clipboard_get(ctx), "copy ") == 0);
    bare__clipboard_set(ctx, "two\r\nlines");
    key(BARE_KEY_INSERT, BARE_MOD_SHIFT);    /* Shift+Insert pastes too */
    frame();
    CHECK(strcmp(buf, "two linesmecopy me") == 0);   /* one line: the break becomes a space */
}

TEST(passwords_show_dots_and_never_copy) {
    field("");
    pass = true;
    type("p\xC3\xA9");
    frame();
    CHECK(strcmp(buf, "p\xC3\xA9") == 0);
    bare__clipboard_set(ctx, "old");
    key(BARE_KEY_A, BARE_MOD_CTRL);
    key(BARE_KEY_C, BARE_MOD_CTRL);
    key(BARE_KEY_X, BARE_MOD_CTRL);
    frame();
    CHECK(strcmp(bare__clipboard_get(ctx), "old") == 0 && strcmp(buf, "p\xC3\xA9") == 0);
    int t = -1;
    for (int i = 0; i < ctx->elem_count; i++)
        if (ctx->elems[i].text) t = i;
    CHECK(t >= 0 && ctx->elems[t].text->len == 6);   /* two dots of three bytes */
    CHECK(memcmp(ctx->elems[t].text->str, "\xE2\x80\xA2\xE2\x80\xA2", 6) == 0);
}

TEST(enter_submits_one_line_and_breaks_many) {
    field("go");
    key(BARE_KEY_ENTER, 0);
    frame();
    CHECK(st.submitted && strcmp(buf, "go") == 0);
    frame();
    CHECK(!st.submitted);
    multi = true;
    frame();
    key(BARE_KEY_ENTER, 0);
    type("x");
    key(BARE_KEY_ENTER, BARE_MOD_SHIFT);
    frame();
    CHECK(strcmp(buf, "go\nx\n") == 0 && !st.submitted);
}

TEST(up_and_down_keep_the_column) {
    field("");
    multi = true;
    snprintf(buf, sizeof buf, "abcdef\nab\nabcdef");
    frame();
    frame();
    set_caret(5);                 /* line 0, column 5 */
    key(BARE_KEY_DOWN, 0);
    frame();
    CHECK(caret() == 9);          /* line 1 is shorter: its end */
    key(BARE_KEY_DOWN, 0);
    frame();
    CHECK(caret() == 15);         /* line 2, column 5 again */
    key(BARE_KEY_UP, BARE_MOD_SHIFT);
    key(BARE_KEY_UP, BARE_MOD_SHIFT);
    frame();
    CHECK(caret() == 5 && anchor() == 15);
    key(BARE_KEY_UP, 0);
    frame();
    CHECK(caret() == 0);          /* past the first line: the start */
    key(BARE_KEY_END, BARE_MOD_CTRL);
    frame();
    CHECK(caret() == 16);
}

TEST(clicks_map_to_offsets_in_wrapped_text) {
    field("");
    multi = true;
    width = 50;                   /* ten characters a line */
    snprintf(buf, sizeof buf, "aaaa bbbb cccc dddd");   /* "aaaa bbbb " / "cccc dddd" */
    frame();
    frame();
    click(12, 3);                 /* line 0, between the 2nd and 3rd character */
    frame();
    CHECK(caret() == 2);
    click(7, 16);                 /* line 1, after its first character */
    frame();
    CHECK(caret() == 11);
    click(48, 3);                 /* past line 0's end: the wrap point, drawn on line 0 */
    frame();
    CHECK(caret() == 10 && ed()->upstream);
    click(49, 30);                /* below the text: the last line */
    frame();
    CHECK(caret() == 19 && !ed()->upstream);
}

TEST(double_click_selects_a_word_triple_the_line) {
    field("alpha beta gamma");
    now = 1.0;
    click(33, 5);                 /* in "beta" (bytes 6..10) */
    now = 1.1;
    click(33, 5);
    frame();
    CHECK(anchor() == 6 && caret() == 10);
    now = 1.2;
    click(33, 5);
    frame();
    CHECK(anchor() == 0 && caret() == 16);
    now = 3.0;                    /* too late for a double click */
    click(33, 5);
    frame();
    CHECK(anchor() == caret() && caret() == 7);
}

TEST(dragging_selects) {
    field("drag across");
    button(true, 2, 5);
    frame();
    mouse_to(31, 5);
    frame();
    CHECK(anchor() == 0 && caret() == 6);
    button(false, 31, 5);
    frame();
    CHECK(ed()->drag == 0 && caret() == 6);
}

/* ---- focus ---- */

static char b1[16], b2[16];
static BareState s1, s2, sbox, sbutton;
static bool app_saw_z, app_held_w;

static void form(void) {
    begin();
    bare_column(.gap = 10) {
        s1 = bare_input(b1, sizeof b1, .id = BARE_ID("one"), .style = &plain, .size = 10, .font = font);
        sbox = bare_box(.id = BARE_ID("plainbox"), .width = 100, .height = 20);
        s2 = bare_input(b2, sizeof b2, .id = BARE_ID("two"), .style = &plain, .size = 10, .font = font);
        sbutton = bare_box(.id = BARE_ID("btn"), .width = 50, .height = 20, .focusable = true,
                           .focus = { .color = BARE_RGB(255, 0, 0) });
    }
    app_saw_z = bare_key_pressed(BARE_KEY_Z);
    app_held_w = bare_key_down(BARE_KEY_W);
    end();
}

TEST(tab_walks_the_focusable_elements_in_order) {
    fresh();
    b1[0] = b2[0] = 0;
    form();
    key(BARE_KEY_TAB, 0);
    form();
    CHECK(s1.focused && !s2.focused);
    key(BARE_KEY_TAB, 0);
    form();
    CHECK(s2.focused);
    key(BARE_KEY_TAB, 0);
    form();
    CHECK(sbutton.focused && !s2.focused);
    int bi = -1;
    for (int i = 0; i < ctx->elem_count; i++)
        if (ctx->elems[i].id == BARE_ID("btn")) bi = i;
    CHECK(bi >= 0 && ctx->elems[bi].box.color.r == 255);   /* the .focus look */
    key(BARE_KEY_TAB, 0);
    form();
    CHECK(s1.focused);            /* wraps around */
    key(BARE_KEY_TAB, BARE_MOD_SHIFT);
    form();
    CHECK(sbutton.focused);
}

TEST(clicking_elsewhere_or_escape_blurs) {
    fresh();
    b1[0] = b2[0] = 0;
    form();
    click(5, 5);                  /* field one is at 0..200 x 0..13 */
    form();
    CHECK(s1.focused);
    click(5, 30);                 /* the plain box: not focusable */
    form();
    CHECK(!s1.focused && bare_focused() == 0);
    click(5, 5);
    form();
    key(BARE_KEY_ESCAPE, 0);
    form();
    CHECK(!s1.focused && bare_focused() == 0);
}

TEST(keys_go_to_the_focused_field_not_the_app) {
    fresh();
    b1[0] = b2[0] = 0;
    form();
    key(BARE_KEY_Z, BARE_MOD_CTRL);
    form();
    CHECK(app_saw_z);             /* nothing focused: the app's shortcut */
    bare_focus(BARE_ID("one"));
    form();
    key(BARE_KEY_Z, BARE_MOD_CTRL);
    form();
    CHECK(!app_saw_z);            /* the field's undo instead */
    hold(BARE_KEY_W, true);
    form();
    CHECK(!app_held_w);           /* typing keys aren't "held" for the app */
    hold(BARE_KEY_W, false);
    key(BARE_KEY_F5, 0);
    form();
    CHECK(bare_focused() == BARE_ID("one"));
}

TEST(text_after_a_tab_goes_to_the_next_field) {
    fresh();
    b1[0] = b2[0] = 0;
    form();
    bare_focus(BARE_ID("one"));
    form();
    type("ab");
    key(BARE_KEY_TAB, 0);
    type("cd");
    form();
    CHECK(strcmp(b1, "ab") == 0 && b2[0] == 0 && again);   /* another frame is coming */
    form();
    CHECK(strcmp(b2, "cd") == 0 && s2.focused);
    CHECK(ctx->in_count == 0);
}

TEST(tab_into_a_field_selects_its_text) {
    fresh();
    snprintf(b1, sizeof b1, "keep");
    b2[0] = 0;
    form();
    key(BARE_KEY_TAB, 0);
    form();
    int n = bare__store_find(ctx, BARE_ID("one"));
    CHECK(n >= 0 && ctx->store.nodes[n].edit->anchor == 0 && ctx->store.nodes[n].edit->caret == 4);
}

TEST(tabs_can_be_typed_in_a_multiline_field) {
    field("");
    multi = true;
    tabs = true;
    frame();
    key(BARE_KEY_TAB, 0);
    frame();
    CHECK(strcmp(buf, "\t") == 0 && st.focused);
}

/* ---- the rest ---- */

TEST(ime_composition_is_replaced_by_the_committed_text) {
    field("ab");
    set_caret(1);
    compose("\xE3\x81\x8B", 3);   /* か */
    frame();
    CHECK(strcmp(buf, "ab") == 0);   /* composing doesn't touch the buffer */
    int t = -1;
    for (int i = 0; i < ctx->elem_count; i++)
        if (ctx->elems[i].text) t = i;
    CHECK(t >= 0 && ctx->elems[t].text->len == 5);   /* but it's shown at the caret */
    CHECK(ctx->elems[t].text->deco->comp_a == 1 && ctx->elems[t].text->deco->comp_b == 4);
    key(BARE_KEY_LEFT, 0);        /* keys belong to the IME meanwhile */
    frame();
    CHECK(caret() == 1);
    compose("", 0);
    type("\xE6\xBC\xA2");         /* 漢 */
    frame();
    CHECK(strcmp(buf, "a\xE6\xBC\xA2" "b") == 0 && caret() == 4 && ed()->comp_len == 0);
    compose("x", 1);
    key(BARE_KEY_ESCAPE, 0);      /* Escape cancels the composition first */
    frame();
    CHECK(ed()->comp_len == 0 && st.focused);
}

TEST(the_caret_blinks_by_wake_time_and_then_rests) {
    field("abc");
    now = 0.1;
    frame();
    CHECK(!again);                                    /* no continuous frames */
    double wake = bare_ctx_wake_time(ctx);
    CHECK(wake > 0.1 && wake <= 0.54);                /* the next blink */
    now = wake;
    frame();
    int t = -1;
    for (int i = 0; i < ctx->elem_count; i++)
        if (ctx->elems[i].text) t = i;
    CHECK(!ctx->elems[t].text->deco->caret_on);
    now = 30.0;                                       /* idle long enough: solid, no more wakes */
    frame();
    CHECK(bare_ctx_wake_time(ctx) < 0 && ctx->elems[t].text->deco->caret_on);
    key(BARE_KEY_LEFT, 0);
    frame();
    CHECK(bare_ctx_wake_time(ctx) > 30.0);            /* moving wakes it up again */
}

TEST(the_app_changing_the_buffer_resets_the_field) {
    field("draft");
    type("!");
    frame();
    snprintf(buf, sizeof buf, "new");
    frame();
    CHECK(caret() == 3);
    key(BARE_KEY_Z, BARE_MOD_CTRL);
    frame();
    CHECK(strcmp(buf, "new") == 0);   /* nothing to undo across it */
}

TEST(one_line_scrolls_to_the_caret) {
    field("");
    width = 40;                   /* eight characters show */
    frame();
    type("0123456789abcdef");
    frame();
    CHECK(st.scroll.x == 0);      /* this frame's begin value; drawn scrolled */
    frame();
    CHECK(st.scroll.x > 40);
    key(BARE_KEY_HOME, 0);
    frame();
    frame();
    CHECK(st.scroll.x == 0);
}

TEST(read_only_selects_and_copies_but_never_changes) {
    field("fixed");
    ro = true;
    frame();
    type("x");
    key(BARE_KEY_BACKSPACE, 0);
    key(BARE_KEY_A, BARE_MOD_CTRL);
    key(BARE_KEY_C, BARE_MOD_CTRL);
    frame();
    CHECK(strcmp(buf, "fixed") == 0 && strcmp(bare__clipboard_get(ctx), "fixed") == 0);
}

TEST(overwrite_mode_replaces_characters) {
    field("abcd");
    set_caret(1);
    key(BARE_KEY_INSERT, 0);
    type("XY");
    frame();
    CHECK(strcmp(buf, "aXYd") == 0);
    set_caret(4);
    type("Z");
    frame();
    CHECK(strcmp(buf, "aXYdZ") == 0);   /* at the end it appends */
}

TEST(actions_run_directly_and_keys_can_be_rebound) {
    field("abc");
    bare_edit(0, BARE_EDIT_SELECT_ALL);
    frame();
    CHECK(anchor() == 0 && caret() == 3);
    bare_edit_bind(BARE_KEY_K, BARE_MOD_SHORTCUT, BARE_EDIT_DELETE);
    bare_edit_bind(BARE_KEY_A, BARE_MOD_SHORTCUT, BARE_EDIT_NONE);   /* Ctrl+A unbound */
    key(BARE_KEY_K, BARE_MOD_CTRL);
    frame();
    CHECK(buf[0] == 0);
    snprintf(buf, sizeof buf, "xyz");
    frame();
    key(BARE_KEY_A, BARE_MOD_CTRL);
    frame();
    CHECK(anchor() == caret());
}

TEST(a_vanished_field_loses_focus) {
    field("x");
    begin();
    end();
    CHECK(bare_focused() == 0);
}

TEST(a_click_mid_frame_drops_the_old_fields_keys) {
    fresh();
    snprintf(b1, sizeof b1, "Ada");
    snprintf(b2, sizeof b2, "one two three");
    form();
    bare_focus(BARE_ID("two"));
    form();
    key(BARE_KEY_LEFT, BARE_MOD_CTRL | BARE_MOD_SHIFT);   /* queued for field two */
    key(BARE_KEY_LEFT, BARE_MOD_CTRL | BARE_MOD_SHIFT);
    begin();
    click(150, 5);                /* arrives mid-frame, before the fields are declared */
    key(BARE_KEY_END, 0);
    s1 = bare_input(b1, sizeof b1, .id = BARE_ID("one"), .style = &plain, .size = 10, .font = font);
    end();
    form();
    int n = bare__store_find(ctx, BARE_ID("one"));
    CHECK(n >= 0 && ctx->store.nodes[n].edit->caret == 3 && ctx->store.nodes[n].edit->anchor == 3);
}

int main(void) {
    RUN(typed_text_lands_at_the_caret);
    RUN(backspace_and_delete_remove_whole_code_points);
    RUN(arrows_move_by_code_point_and_collapse_selections);
    RUN(word_jumps_follow_the_platform);
    RUN(home_is_smart_and_end_goes_to_the_end);
    RUN(selecting_then_typing_replaces_the_selection);
    RUN(the_cap_is_respected_without_splitting_a_code_point);
    RUN(undo_and_redo_restore_text_and_caret);
    RUN(backspace_runs_undo_as_one_step);
    RUN(cut_copy_paste);
    RUN(passwords_show_dots_and_never_copy);
    RUN(enter_submits_one_line_and_breaks_many);
    RUN(up_and_down_keep_the_column);
    RUN(clicks_map_to_offsets_in_wrapped_text);
    RUN(double_click_selects_a_word_triple_the_line);
    RUN(dragging_selects);
    RUN(tab_walks_the_focusable_elements_in_order);
    RUN(clicking_elsewhere_or_escape_blurs);
    RUN(keys_go_to_the_focused_field_not_the_app);
    RUN(text_after_a_tab_goes_to_the_next_field);
    RUN(tab_into_a_field_selects_its_text);
    RUN(tabs_can_be_typed_in_a_multiline_field);
    RUN(ime_composition_is_replaced_by_the_committed_text);
    RUN(the_caret_blinks_by_wake_time_and_then_rests);
    RUN(the_app_changing_the_buffer_resets_the_field);
    RUN(one_line_scrolls_to_the_caret);
    RUN(read_only_selects_and_copies_but_never_changes);
    RUN(overwrite_mode_replaces_characters);
    RUN(actions_run_directly_and_keys_can_be_rebound);
    RUN(a_vanished_field_loses_focus);
    RUN(a_click_mid_frame_drops_the_old_fields_keys);
    bare_ctx_destroy(ctx);
    return test_report();
}
