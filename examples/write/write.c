/* write.c: Write, a Markdown and plain text editor on liquid glass.
 *
 *     write [files...]
 *
 * Write, Split and Read modes (Ctrl+1/2/3), a live preview with clickable
 * task boxes, documents in a sidebar (Ctrl+\), open / save with the system's
 * dialogs (Ctrl+O, Ctrl+S, Ctrl+Shift+S, Ctrl+N, Ctrl+W), Markdown shortcuts
 * (Ctrl+B, Ctrl+I, Ctrl+K), zoom (Ctrl+= / Ctrl+- / Ctrl+0), and everything
 * bare_editor does (find, multiple carets, ...). Appearance: dark or light,
 * eight accents, sans or mono. Everything moves on springs, and an idle
 * window uses no CPU. */
#include "bare.h"
#include "md.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define MAX_DOCS 16
#define FILTERS  "Markdown:md;markdown|Text:txt|All files:*"

typedef struct Doc {
    BareDoc *doc;
    char     path[1024];
    char     name[128];
    bool     md;
    uint64_t counted;          /* the version the counts are for */
    int      words, chars;
} Doc;

static Doc docs[MAX_DOCS];
static int doc_count, cur, close_armed = -1;
static int mode = 1;           /* 0 write, 1 split, 2 read */
static bool dark = true, sidebar = true, popover, mono, numbers;
static int accent = 0;
static float zoom = 1.0f;
static char toast[128];
static double toast_at = -100;

static const BareColor accents[] = {
    { 10, 132, 255, 255 }, { 175, 82, 222, 255 }, { 255, 55, 95, 255 }, { 255, 149, 0, 255 },
    { 52, 199, 89, 255 },  { 48, 176, 199, 255 }, { 94, 92, 230, 255 }, { 152, 152, 160, 255 },
};
#define ACCENTS ((int)(sizeof accents / sizeof accents[0]))

static const char *welcome =
    "# Welcome to Write\n"
    "\n"
    "A **Markdown** and *plain text* editor made with [Bare](https://github.com), in pure C.\n"
    "Type on the left, read on the right; the light behind the glass follows your accent.\n"
    "\n"
    "## Things to try\n"
    "\n"
    "- [x] Switch between **Write**, **Split** and **Read** (Ctrl+1, 2, 3)\n"
    "- [ ] Tick this box in the preview\n"
    "- [ ] Pick an accent from the colour dot in the corner\n"
    "- Select a word and press `Ctrl+D` a few times: one caret per match\n"
    "- `Ctrl+F` finds, `Ctrl+H` replaces, `Alt+Up` moves a line\n"
    "\n"
    "> Simplicity is the ultimate sophistication.\n"
    "\n"
    "```c\n"
    "while (bare_frame()) {\n"
    "    bare_editor(doc, .wrap = true);\n"
    "}\n"
    "```\n"
    "\n"
    "| Shortcut | Does |\n"
    "|---|---|\n"
    "| Ctrl+S | Save |\n"
    "| Ctrl+B / Ctrl+I | Bold / italic |\n"
    "| Ctrl+= | Zoom in |\n"
    "\n"
    "---\n"
    "\n"
    "Plain `.txt` files open as plain text. ~~Nothing to configure.~~ Enjoy.\n";

/* ---- documents ------------------------------------------------------------------ */

static void toast_show(const char *msg) {
    snprintf(toast, sizeof toast, "%s", msg);
    toast_at = bare_time();
    bare_wake_in(1.9f);   /* a frame when it's time to fade out */
}

static const char *base_name(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

static bool is_md(const char *path) {
    const char *dot = strrchr(path, '.');
    return !dot || !strcmp(dot, ".md") || !strcmp(dot, ".markdown") || !strcmp(dot, ".MD");
}

static Doc *add(BareDoc *d, const char *path, bool md) {
    /* An untouched empty document gives its place to the one opened. */
    if (doc_count == 1 && !docs[0].path[0] && !bare_doc_modified(docs[0].doc) && bare_doc_lines(docs[0].doc) == 1) {
        int n;
        bare_doc_line(docs[0].doc, 0, &n);
        if (n == 0) { bare_doc_free(docs[0].doc); doc_count = 0; }
    }
    if (doc_count == MAX_DOCS) { bare_doc_free(d); toast_show("Too many documents open"); return NULL; }
    Doc *x = &docs[doc_count];
    memset(x, 0, sizeof *x);
    x->doc = d;
    x->md = md;
    x->counted = (uint64_t)-1;
    snprintf(x->path, sizeof x->path, "%s", path ? path : "");
    snprintf(x->name, sizeof x->name, "%s", path ? base_name(path) : "Untitled");
    cur = doc_count++;
    return x;
}

static void open_path(const char *path) {
    for (int i = 0; i < doc_count; i++)
        if (!strcmp(docs[i].path, path)) { cur = i; return; }
    BareDoc *d = bare_doc_load(path);
    if (!d) { toast_show("Couldn't open that file"); return; }
    add(d, path, is_md(path));
}

static void save_to(Doc *x, const char *path) {
    if (!bare_doc_save(x->doc, path)) { toast_show("Couldn't save"); return; }
    if (path != x->path) snprintf(x->path, sizeof x->path, "%s", path);
    snprintf(x->name, sizeof x->name, "%s", base_name(path));
    x->md = is_md(path);
    toast_show("Saved");
}

static void save(Doc *x, bool as) {
    if (x->path[0] && !as) save_to(x, x->path);
    else bare_file_dialog(BARE_ID("save"), BARE_FILE_SAVE, FILTERS, x->path[0] ? x->path : "Untitled.md");
}

static void close_doc(int i) {
    if (bare_doc_modified(docs[i].doc) && close_armed != i) {
        close_armed = i;
        toast_show("Unsaved changes: close again to discard them");
        return;
    }
    close_armed = -1;
    bare_doc_free(docs[i].doc);
    memmove(docs + i, docs + i + 1, sizeof *docs * (size_t)(doc_count - i - 1));
    doc_count--;
    if (doc_count == 0) add(bare_doc_create(), NULL, true);
    if (cur >= doc_count) cur = doc_count - 1;
}

static void count(Doc *x) {
    if (x->counted == bare_doc_version(x->doc)) return;
    int n, words = 0;
    const char *s = bare_doc_text(x->doc, &n);
    bool seen = false;   /* a word: a run without spaces holding a letter or digit ("-" and "[ ]" aren't) */
    for (int i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == ' ' || ch == '\n' || ch == '\t') { seen = false; continue; }
        if (!seen && (isalnum(ch) || ch >= 0x80)) { words++; seen = true; }
    }
    int chars = 0;
    for (int i = 0; i < n; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) chars++;
    x->words = words;
    x->chars = chars;
    x->counted = bare_doc_version(x->doc);
}

/* ---- the look -------------------------------------------------------------------- */

static BareTheme theme;

static void apply_theme(void) {
    theme = dark ? *bare_theme_dark() : *bare_theme_light();
    BareColor a = accents[accent];
    theme.accent = a;
    theme.selection = (BareColor){ a.r, a.g, a.b, dark ? 90 : 70 };
    theme.primary.color = a;
    theme.background = dark ? BARE_RGB(10, 10, 16) : BARE_RGB(236, 238, 245);
    bare_set_theme(&theme);
}

static BareColor mix(BareColor a, BareColor b, float t) {
    return BARE_RGBA(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

static BareGlass panel_glass(void) {
    return (BareGlass){ .refraction = 0.55f, .depth = 26, .blur = 28, .dispersion = 0.5f, .shine = 1.1f,
                        .tint = dark ? BARE_RGBA(30, 30, 44, 60) : BARE_RGBA(255, 255, 255, 90) };
}

static BareColor panel_fill(void) { return dark ? BARE_RGBA(18, 18, 26, 120) : BARE_RGBA(255, 255, 255, 120); }
static BareColor hairline(void) { return dark ? BARE_RGBA(255, 255, 255, 26) : BARE_RGBA(255, 255, 255, 170); }

/* Soft light behind the glass: three orbs in the accent's family that drift
 * to a new place for each mode and change colour with the accent. */
static void backdrop(BareVec2 win) {
    BareColor a = accents[accent];
    BareColor b = mix(a, dark ? BARE_RGB(255, 60, 140) : BARE_RGB(255, 140, 200), 0.55f);
    BareColor c = mix(a, dark ? BARE_RGB(40, 220, 200) : BARE_RGB(120, 220, 255), 0.5f);
    static const float at[3][3][2] = {   /* per mode: three orbs, x and y as fractions of the window */
        { { 0.15f, 0.20f }, { 0.75f, 0.75f }, { 0.55f, 0.10f } },
        { { 0.30f, 0.30f }, { 0.80f, 0.40f }, { 0.20f, 0.85f } },
        { { 0.70f, 0.20f }, { 0.25f, 0.70f }, { 0.85f, 0.85f } },
    };
    const BareColor col[3] = { a, b, c };
    const float size[3] = { 0.55f, 0.45f, 0.38f };
    bare_stack(.position = BARE_ABSOLUTE, .width = BARE_GROW, .height = BARE_GROW, .passthrough = true) {
        for (int i = 0; i < 3; i++) {
            float d = size[i] * (win.x > win.y ? win.x : win.y);
            bare_box(.id = BARE_IDI("orb", i), .position = BARE_ABSOLUTE, .x = at[mode][i][0] * win.x - d / 2,
                     .y = at[mode][i][1] * win.y - d / 2, .width = d, .height = d, .radius = d / 2,
                     .color = col[i], .opacity = dark ? 0.55f : 0.45f, .shadow = { 0, 0, d * 0.45f, d * 0.1f, col[i] },
                     .passthrough = true, .animate = BARE_EASE(1.2f, BARE_EASE_IN_OUT),
                     .animate_layout = BARE_SPRING(1.6f, 0.9f));
        }
    }
}

/* ---- pieces ------------------------------------------------------------------------ */

static bool ghost(const char *label, BareId id) {
    return bare_button(label, .id = id, .pad = { 7, 12, 7, 12 }, .radius = 14, .color = BARE_CLEAR,
                       .hover = { .color = dark ? BARE_RGBA(255, 255, 255, 22) : BARE_RGBA(0, 0, 0, 14) },
                       .press = { .scale = 0.95f }, .animate = BARE_SPRING_SNAPPY).clicked;
}

static void hamburger(void) {
    bare_column(.id = BARE_ID("menu"), .width = 36, .height = 36, .radius = 18, .gap = 4, .align = BARE_CENTER,
                .justify = BARE_CENTER, .cursor = BARE_CURSOR_HAND, .focusable = true,
                .hover = { .color = dark ? BARE_RGBA(255, 255, 255, 22) : BARE_RGBA(0, 0, 0, 14) },
                .press = { .scale = 0.9f }, .animate = BARE_SPRING_SNAPPY) {
        if (bare_this().clicked) sidebar = !sidebar;
        for (int i = 0; i < 3; i++)
            bare_box(.width = i == 1 && !sidebar ? 10.0f : 16.0f, .height = 2, .radius = 1, .color = theme.text,
                     .animate = BARE_SPRING_SNAPPY, .animate_layout = BARE_SPRING_SNAPPY, .passthrough = true);
    }
}

/* Write | Split | Read, with a glass knob that slides between them. */
static void modes(void) {
    static const char *names[] = { "Write", "Split", "Read" };
    const float w = 76, h = 32;
    bare_row(.width = w * 3 + 6, .height = h + 6, .padding = 3, .radius = (h + 6) / 2,
             .color = dark ? BARE_RGBA(0, 0, 0, 60) : BARE_RGBA(0, 0, 0, 16)) {
        BareColor a = accents[accent];
        bare_box(.id = BARE_ID("knob"), .position = BARE_ABSOLUTE, .x = 3 + w * (float)mode, .y = 3, .width = w,
                 .height = h, .radius = h / 2, .color = (BareColor){ a.r, a.g, a.b, dark ? 150 : 190 },
                 .glass = { .refraction = 1.0f, .depth = 12, .blur = 1.5f, .shine = 1.6f, .dispersion = 1.0f },
                 .shadow = { 0, 3, 10, 0, (BareColor){ a.r, a.g, a.b, 90 } }, .passthrough = true,
                 .animate = BARE_EASE(0.5f, BARE_EASE_OUT), .animate_layout = BARE_SPRING(0.42f, 0.72f));
        for (int i = 0; i < 3; i++) {
            bare_row(.id = BARE_IDI("mode", i), .width = w, .height = h, .align = BARE_CENTER, .justify = BARE_CENTER,
                     .cursor = BARE_CURSOR_HAND, .press = { .scale = 0.94f }, .animate = BARE_SPRING_SNAPPY) {
                if (bare_this().clicked) mode = i;
                bare_text(names[i], .size = 13.5f, .weight = BARE_SEMIBOLD, .nowrap = true,
                          .color = i == mode ? BARE_WHITE : theme.muted, .animate = BARE_EASE(0.25f, BARE_EASE_OUT));
            }
        }
    }
}

static void toolbar(Doc *x) {
    bare_row(.width = BARE_GROW, .height = 54, .radius = 27, .pad = { 0, 10, 0, 10 }, .gap = 6, .align = BARE_CENTER,
             .glass = panel_glass(), .color = panel_fill(), .border = 1, .border_color = hairline(),
             .shadow = { 0, 10, 30, 0, BARE_RGBA(0, 0, 0, dark ? 90 : 30) }) {
        hamburger();
        bare_row(.gap = 8, .align = BARE_CENTER, .pad = { 0, 0, 0, 4 }) {
            bare_text(x->name, .size = 15, .weight = BARE_SEMIBOLD, .nowrap = true, .ellipsis = true);
            bool dirty = bare_doc_modified(x->doc);
            bare_box(.width = 7, .height = 7, .radius = 3.5f, .color = accents[accent],
                     .scale = dirty ? 1.0f : 0.2f, .opacity = dirty ? 1.0f : BARE_ZERO, .animate = BARE_SPRING_BOUNCY);
            bare_row(.pad = { 2, 7, 2, 7 }, .radius = 6, .color = dark ? BARE_RGBA(255, 255, 255, 18)
                                                                        : BARE_RGBA(0, 0, 0, 12)) {
                bare_text(x->md ? "MD" : "TXT", .size = 10.5f, .weight = BARE_BOLD, .color = theme.muted,
                          .letter_spacing = 0.6f);
            }
        }
        bare_box(.width = BARE_GROW);
        modes();
        bare_box(.width = BARE_GROW);
        if (ghost("New", BARE_ID("new"))) add(bare_doc_create(), NULL, true);
        if (ghost("Open", BARE_ID("open"))) bare_file_dialog(BARE_ID("open"), BARE_FILE_OPEN, FILTERS, NULL);
        if (ghost("Save", BARE_ID("save_btn"))) save(x, false);
        if (ghost("Find", BARE_ID("find"))) bare_edit(BARE_ID("editor"), BARE_EDIT_FIND);
        /* the accent dot opens the appearance popover */
        bare_row(.id = BARE_ID("accent"), .width = 36, .height = 36, .radius = 18, .align = BARE_CENTER,
                 .justify = BARE_CENTER, .cursor = BARE_CURSOR_HAND, .focusable = true,
                 .hover = { .scale = 1.08f }, .press = { .scale = 0.92f }, .animate = BARE_SPRING_SNAPPY) {
            if (bare_this().clicked) popover = !popover;
            bare_box(.width = 20, .height = 20, .radius = 10, .color = accents[accent], .border = 2,
                     .border_color = BARE_RGBA(255, 255, 255, 220), .passthrough = true,
                     .shadow = { 0, 2, 8, 0, (BareColor){ accents[accent].r, accents[accent].g, accents[accent].b, 120 } },
                     .animate = BARE_EASE(0.4f, BARE_EASE_OUT));
        }
    }
}

static bool pill(const char *label, bool on, BareId id) {
    BareColor a = accents[accent];
    return bare_button(label, .id = id, .pad = { 6, 12, 6, 12 }, .radius = 13,
                       .color = on ? (BareColor){ a.r, a.g, a.b, 200 } : (dark ? BARE_RGBA(255, 255, 255, 16)
                                                                              : BARE_RGBA(0, 0, 0, 10)),
                       .text_color = on ? BARE_WHITE : theme.text, .press = { .scale = 0.94f },
                       .animate = BARE_EASE(0.25f, BARE_EASE_OUT)).clicked;
}

/* Appearance: accents, light or dark, the editor's font, line numbers. */
static void appearance(BareVec2 win) {
    float w = 324;
    bare_column(.id = BARE_ID("popover"), .position = BARE_ABSOLUTE, .x = win.x - w - 28, .y = 76, .width = w,
                .padding = 18, .gap = 14, .radius = 22, .glass = popover ? panel_glass() : (BareGlass){ 0 },
                .color = popover ? panel_fill() : BARE_CLEAR, .border = popover ? 1.0f : BARE_ZERO,
                .border_color = hairline(), .opacity = popover ? 1.0f : BARE_ZERO, .scale = popover ? 1.0f : 0.92f,
                .offset = { 0, popover ? 0.0f : -10.0f }, .passthrough = !popover,
                .shadow = { 0, 16, 40, 0, BARE_RGBA(0, 0, 0, popover ? (dark ? 110 : 40) : 0) },
                .animate = BARE_SPRING(0.32f, 0.82f)) {
        bare_text("Accent", .size = 12, .weight = BARE_SEMIBOLD, .color = theme.muted, .letter_spacing = 0.4f);
        bare_row(.gap = 10) {
            for (int i = 0; i < ACCENTS; i++) {
                bool on = i == accent;
                bare_row(.id = BARE_IDI("swatch", i), .width = 24, .height = 24, .radius = 12, .color = accents[i],
                         .border = on ? 2.5f : BARE_ZERO, .border_color = dark ? BARE_RGBA(255, 255, 255, 235) : BARE_RGBA(20, 20, 30, 150),
                         .scale = on ? 1.15f : 1.0f, .cursor = BARE_CURSOR_HAND, .hover = { .scale = 1.2f },
                         .press = { .scale = 0.9f }, .animate = BARE_SPRING_BOUNCY) {
                    if (bare_this().clicked && popover) { accent = i; apply_theme(); }
                }
            }
        }
        bare_text("Appearance", .size = 12, .weight = BARE_SEMIBOLD, .color = theme.muted, .letter_spacing = 0.4f);
        bare_row(.gap = 8) {
            if (pill("Dark", dark, BARE_ID("dark")) && popover && !dark) { dark = true; apply_theme(); }
            if (pill("Light", !dark, BARE_ID("light")) && popover && dark) { dark = false; apply_theme(); }
        }
        bare_text("Editor", .size = 12, .weight = BARE_SEMIBOLD, .color = theme.muted, .letter_spacing = 0.4f);
        bare_row(.gap = 8) {
            if (pill("Sans", !mono, BARE_ID("sans")) && popover) mono = false;
            if (pill("Mono", mono, BARE_ID("mono")) && popover) mono = true;
            if (pill("Line numbers", numbers, BARE_ID("numbers")) && popover) numbers = !numbers;
        }
    }
}

static void side(Doc *x) {
    (void)x;
    bare_column(.id = BARE_ID("sidebar"), .width = sidebar ? 220.0f : BARE_ZERO, .height = BARE_GROW, .clip = true,
                .radius = 22, .glass = sidebar ? panel_glass() : (BareGlass){ 0 },
                .color = sidebar ? panel_fill() : BARE_CLEAR, .border = sidebar ? 1.0f : BARE_ZERO,
                .border_color = hairline(), .opacity = sidebar ? 1.0f : BARE_ZERO,
                .animate = BARE_EASE(0.3f, BARE_EASE_OUT), .animate_layout = BARE_SPRING(0.38f, 0.86f)) {
        bare_column(.width = 220, .height = BARE_GROW, .padding = 12, .gap = 2) {
            bare_text("Documents", .size = 12, .weight = BARE_SEMIBOLD, .color = theme.muted,
                      .letter_spacing = 0.4f);
            bare_box(.height = 6);
            for (int i = 0; i < doc_count; i++) {
                char label[160];
                snprintf(label, sizeof label, "%s%s", docs[i].name, bare_doc_modified(docs[i].doc) ? "  \xE2\x80\xA2" : "");
                if (bare_list_item(label, i == cur, .id = BARE_IDI("doc", i), .radius = 12).clicked) cur = i;
            }
            bare_box(.height = BARE_GROW);
            bare_row(.gap = 6) {
                if (ghost("New", BARE_ID("side_new"))) add(bare_doc_create(), NULL, true);
                if (ghost("Open", BARE_ID("side_open"))) bare_file_dialog(BARE_ID("open"), BARE_FILE_OPEN, FILTERS, NULL);
                if (ghost("Close", BARE_ID("side_close"))) close_doc(cur);
            }
        }
    }
}

static void status(Doc *x) {
    count(x);
    BareTextPos c = bare_doc_caret(x->doc);
    char s[200];
    snprintf(s, sizeof s, "%s   \xC2\xB7   %d words   \xC2\xB7   %d characters   \xC2\xB7   Ln %d, Col %d   \xC2\xB7   %s   \xC2\xB7   %d%%",
             x->md ? "Markdown" : "Plain text", x->words, x->chars, c.line + 1, c.col + 1,
             bare_doc_crlf(x->doc) ? "CRLF" : "LF", (int)(zoom * 100 + 0.5f));
    bare_row(.width = BARE_GROW, .justify = BARE_CENTER) {
        bare_row(.pad = { 7, 16, 7, 16 }, .radius = 16, .glass = panel_glass(), .color = panel_fill(), .border = 1,
                 .border_color = hairline()) {
            bare_text(s, .size = 12, .color = theme.muted, .nowrap = true);
        }
    }
}

/* ---- the frame ------------------------------------------------------------------------ */

static void shortcuts(Doc *x) {
    if (!bare_key_down(BARE_KEY_SHORTCUT)) return;
    bool shift = bare_key_down(BARE_KEY_SHIFT);
    if (bare_key_pressed(BARE_KEY_S)) save(x, shift);
    if (bare_key_pressed(BARE_KEY_O)) bare_file_dialog(BARE_ID("open"), BARE_FILE_OPEN, FILTERS, NULL);
    if (bare_key_pressed(BARE_KEY_N)) add(bare_doc_create(), NULL, true);
    if (bare_key_pressed(BARE_KEY_W)) close_doc(cur);
    if (bare_key_pressed(BARE_KEY_1)) mode = 0;
    if (bare_key_pressed(BARE_KEY_2)) mode = 1;
    if (bare_key_pressed(BARE_KEY_3)) mode = 2;
    if (bare_key_pressed(BARE_KEY_BACKSLASH)) sidebar = !sidebar;
    if (bare_key_pressed(BARE_KEY_EQUALS) && zoom < 2.0f) zoom += 0.1f;
    if (bare_key_pressed(BARE_KEY_MINUS) && zoom > 0.6f) zoom -= 0.1f;
    if (bare_key_pressed(BARE_KEY_0)) zoom = 1.0f;
    if (x->md && bare_focused() != 0) {
        if (bare_key_pressed(BARE_KEY_B)) bare_doc_wrap(x->doc, "**", "**");
        if (bare_key_pressed(BARE_KEY_I)) bare_doc_wrap(x->doc, "*", "*");
        if (bare_key_pressed(BARE_KEY_K)) bare_doc_wrap(x->doc, "[", "](https://)");
    }
}

int main(int argc, char **argv) {
    bare_window(1280, 820, "Write");
    apply_theme();
    for (int i = 1; i < argc; i++) open_path(argv[i]);
    if (!doc_count) {
        BareDoc *d = bare_doc_create();
        bare_doc_set_text(d, welcome, -1);
        add(d, NULL, true);
        snprintf(docs[0].name, sizeof docs[0].name, "Welcome.md");
    }
    bare_focus(BARE_ID("editor"));
    static const BareBox clear_editor = { .pad = { 22, 28, 22, 28 } };
    char title[200] = "";

    while (bare_frame()) {
        BareVec2 win = bare_window_size();
        const char *chosen = bare_file_chosen(BARE_ID("open"));
        if (chosen && *chosen) open_path(chosen);
        Doc *x = &docs[cur];
        chosen = bare_file_chosen(BARE_ID("save"));
        if (chosen && *chosen) save_to(x, chosen);
        shortcuts(x);
        x = &docs[cur];
        if (bare_key_pressed(BARE_KEY_ESCAPE)) popover = false;

        char t[200];
        snprintf(t, sizeof t, "%s%s - Write", bare_doc_modified(x->doc) ? "\xE2\x80\xA2 " : "", x->name);
        if (strcmp(t, title)) { snprintf(title, sizeof title, "%s", t); bare_title(title); }

        /* panel widths: they glide when the mode or the sidebar changes */
        float body = win.x - 28 - (sidebar ? 220 + 12 : 0);
        float edit_w = mode == 0 ? body : mode == 1 ? (body - 12) / 2 : 0;
        float read_w = mode == 2 ? body : mode == 1 ? (body - 12) / 2 : 0;
        float size = 16 * zoom;
        BareColor a = accents[accent];
        MdLook look = { .text = theme.text, .muted = theme.muted, .accent = a,
                        .code = dark ? BARE_RGB(255, 200, 150) : BARE_RGB(176, 70, 30),
                        .code_bg = dark ? BARE_RGBA(255, 255, 255, 16) : BARE_RGBA(0, 0, 0, 12),
                        .rule = dark ? BARE_RGBA(255, 255, 255, 30) : BARE_RGBA(0, 0, 0, 24), .size = size,
                        .font = 0 };

        bare_stack(.width = BARE_GROW, .height = BARE_GROW) {
            backdrop(win);
            bare_column(.width = BARE_GROW, .height = BARE_GROW, .padding = 14, .gap = 12) {
                toolbar(x);
                bare_row(.width = BARE_GROW, .height = BARE_GROW, .gap = 0) {
                    side(x);
                    bare_box(.width = sidebar ? 12.0f : BARE_ZERO, .animate_layout = BARE_SPRING(0.38f, 0.86f));
                    bare_column(.id = BARE_ID("edit_panel"), .width = edit_w > 1 ? edit_w : BARE_ZERO,
                                .height = BARE_GROW, .radius = 22, .clip = true,
                                .glass = edit_w > 1 ? panel_glass() : (BareGlass){ 0 },
                                .color = edit_w > 1 ? panel_fill() : BARE_CLEAR,
                                .border = edit_w > 1 ? 1.0f : BARE_ZERO, .border_color = hairline(),
                                .animate = BARE_EASE(0.3f, BARE_EASE_OUT),
                                .animate_layout = BARE_SPRING(0.42f, 0.86f)) {
                        bare_editor(x->doc, .id = BARE_ID("editor"), .style = &clear_editor,
                                    .font = mono ? BARE_MONO : BARE_SANS, .size = mono ? size * 0.9f : size,
                                    .line_height = mono ? 1.6f : 1.7f, .wrap = true, .line_numbers = numbers,
                                    .highlight_line = numbers, .spaces = true, .tab_size = 4,
                                    .auto_close = x->md ? "()[]{}``" : "()[]{}", .comment = x->md ? "> " : NULL,
                                    .highlight = x->md ? md_highlight : NULL, .highlight_user = &look,
                                    .indent = x->md ? md_indent : NULL, .caret = a);
                    }
                    bare_box(.width = mode == 1 ? 12.0f : BARE_ZERO, .animate_layout = BARE_SPRING(0.42f, 0.86f));
                    bare_column(.id = BARE_ID("read_panel"), .width = read_w > 1 ? read_w : BARE_ZERO,
                                .height = BARE_GROW, .radius = 22, .clip = true,
                                .glass = read_w > 1 ? panel_glass() : (BareGlass){ 0 },
                                .color = read_w > 1 ? panel_fill() : BARE_CLEAR,
                                .border = read_w > 1 ? 1.0f : BARE_ZERO, .border_color = hairline(),
                                .animate = BARE_EASE(0.3f, BARE_EASE_OUT),
                                .animate_layout = BARE_SPRING(0.42f, 0.86f)) {
                        bare_column(.width = BARE_GROW, .height = BARE_GROW, .scroll = BARE_SCROLL_Y, .align = BARE_CENTER) {
                            bare_column(.width = BARE_GROW, .max_width = 760, .pad = { 30, 40, 48, 40 }, .gap = 14,
                                        .text_color = theme.text) {
                                if (x->md) {
                                    md_preview(x->doc, &look);
                                } else {
                                    /* plain text reads as it is, in the body font */
                                    bare_text(bare_doc_text(x->doc, NULL), .size = size, .line_height = 1.7f,
                                              .color = theme.text);
                                }
                            }
                        }
                    }
                }
                status(x);
            }
            appearance(win);
            /* a message that springs in at the top and fades */
            bool shown = bare_time() - toast_at < 1.8;
            bare_row(.width = BARE_GROW, .justify = BARE_CENTER, .position = BARE_ABSOLUTE, .y = 84,
                     .passthrough = true) {
                bare_row(.id = BARE_ID("toast"), .pad = { 9, 18, 9, 18 }, .radius = 18,
                         .glass = shown ? panel_glass() : (BareGlass){ 0 }, .color = shown ? panel_fill() : BARE_CLEAR,
                         .opacity = shown ? 1.0f : BARE_ZERO, .offset = { 0, shown ? 0.0f : -14.0f },
                         .scale = shown ? 1.0f : 0.9f, .passthrough = true, .animate = BARE_SPRING(0.34f, 0.78f)) {
                    bare_text(toast, .size = 13.5f, .weight = BARE_SEMIBOLD, .nowrap = true);
                }
            }
        }
    }
    for (int i = 0; i < doc_count; i++) bare_doc_free(docs[i].doc);
    bare_quit();
    return 0;
}
