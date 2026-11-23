/* Text (M5): UTF-8, line breaking, measuring, the glyph atlas and its uploads.
 *
 * Most tests use a fake monospace font backend so the numbers are exact; a few
 * use the embedded default font (Inter) through stb_truetype. */
#include "bare_test.h"
#include "bare.h"
#include "bare/bare_core.h"
#include "internal.h"
#include <string.h>

/* ---- a fake font: every character is 0.5 em wide; glyphs are solid boxes ---- */

static int fake_rasters;
static unsigned char fake_pixels[256 * 256];
static float fake_glyph_scale = 1.0f;   /* bigger glyph boxes to fill the atlas */

static void *fake_load(const BareAllocator *a, const void *data, int size) {
    (void)a;
    return size >= 4 && memcmp(data, "FAKE", 4) == 0 ? (void *)fake_pixels : NULL;
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
        out[n] = (BareShapedGlyph){ cp, cp == '\n' ? 0 : 0.5f * px, 0, 0, (uint32_t)at };
        n++;
    }
    return n;
}
static bool fake_rasterize(void *f, uint32_t glyph, float px, float shift, BareGlyphBitmap *out) {
    (void)f; (void)glyph; (void)shift;
    fake_rasters++;
    int w = (int)(0.5f * px * fake_glyph_scale + 0.999f), h = (int)(0.7f * px * fake_glyph_scale + 0.999f);
    if (w > 256) w = 256;
    if (h > 256) h = 256;
    memset(fake_pixels, 255, sizeof fake_pixels);
    *out = (BareGlyphBitmap){ w, h, 0, -h, fake_pixels, 256 };
    return true;
}
static void fake_unload(void *f) { (void)f; }

static const BareFontBackend fake_backend = { fake_load, fake_metrics, fake_shape, fake_rasterize, fake_unload };

static BareContext *ctx;
static BareFont fake;
static int log_count;
static char last_log[512];

static void capture_log(void *user, const char *msg) {
    (void)user;
    snprintf(last_log, sizeof last_log, "%s", msg);
    log_count++;
}

/* A fresh context each time (the atlas starts empty). */
static void fresh(bool real_font) {
    if (ctx) bare_ctx_destroy(ctx);
    BareConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    if (!real_font) cfg.fonts = &fake_backend;
    ctx = bare_ctx_create(&cfg);
    bare_ctx_set_log(ctx, capture_log, NULL);
    bare_ctx_make_current(ctx);
    fake = real_font ? 0 : bare_font_from_memory("FAKE", 4);
    fake_rasters = 0;
    fake_glyph_scale = 1.0f;
    log_count = 0;
}

static void begin(float scale) { bare_ctx_begin(ctx, 800, 600, scale, 0.0); }

/* Shapes and wraps `s` at `width` inside a frame (size 10: 5 px per character). */
static BareText *wrap(const char *s, float width, bool nowrap) {
    BareTextStyle st = { .font = fake, .size = 10, .nowrap = nowrap };
    BareText *t = bare__text_new(ctx, s, (int)strlen(s), &st);
    bare__text_wrap(ctx, t, width);
    return t;
}

static bool line_is(const BareText *t, int l, const char *expect) {
    if (l >= t->line_count) return false;
    const BareTextLine *line = &t->lines[l];
    int n = (int)strlen(expect);
    if (line->count == 0) return n == 0;
    int from = (int)t->glyphs[line->first].cluster;
    int last = line->first + line->count - 1;
    int to = (int)t->glyphs[last].cluster + 1;
    /* trailing spaces hang off the line; compare without them */
    while (to > from && t->str[to - 1] == ' ') to--;
    return to - from == n && memcmp(t->str + from, expect, (size_t)n) == 0;
}

/* ---- UTF-8 ------------------------------------------------------------------ */

static uint32_t decode_all(const char *s, uint32_t *out, int cap) {
    int len = (int)strlen(s), i = 0, n = 0;
    while (i < len && n < cap) out[n++] = bare__utf8_decode(s, len, &i);
    return (uint32_t)n;
}

TEST(utf8_valid_sequences) {
    uint32_t cp[8];
    CHECK(decode_all("A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80", cp, 8) == 4);   /* A é € 😀 */
    CHECK(cp[0] == 'A' && cp[1] == 0xE9 && cp[2] == 0x20AC && cp[3] == 0x1F600);
}

TEST(utf8_invalid_bytes_become_replacement_characters) {
    uint32_t cp[8];
    CHECK(decode_all("a\xFF" "b", cp, 8) == 3);                 /* invalid byte */
    CHECK(cp[0] == 'a' && cp[1] == 0xFFFD && cp[2] == 'b');
    CHECK(decode_all("\x80z", cp, 8) == 2);                      /* stray continuation */
    CHECK(cp[0] == 0xFFFD && cp[1] == 'z');
    CHECK(decode_all("\xC0\xAF", cp, 8) == 2);                   /* overlong '/' */
    CHECK(cp[0] == 0xFFFD && cp[1] == 0xFFFD);
    CHECK(decode_all("\xED\xA0\x80", cp, 8) == 3);               /* a surrogate */
    CHECK(cp[0] == 0xFFFD);
    CHECK(decode_all("\xE2\x82", cp, 8) == 2);                   /* cut off at the end */
    CHECK(cp[0] == 0xFFFD && cp[1] == 0xFFFD);
    CHECK(decode_all("\xE2(x", cp, 8) == 3);                     /* cut off by an ASCII byte */
    CHECK(cp[0] == 0xFFFD && cp[1] == '(' && cp[2] == 'x');
    CHECK(decode_all("\xF4\x90\x80\x80", cp, 8) >= 1);           /* above U+10FFFF */
    CHECK(cp[0] == 0xFFFD);
}

/* ---- line breaking ------------------------------------------------------------ */

TEST(wrap_at_spaces) {
    fresh(false);
    begin(1);
    BareText *t = wrap("aaa bbb ccc", 40, false);   /* "aaa bbb" is 35 px */
    CHECK(t->line_count == 2);
    CHECK(line_is(t, 0, "aaa bbb"));
    CHECK(line_is(t, 1, "ccc"));
    CHECK_NEAR(t->lines[0].width, 35, 1e-4);
    CHECK_NEAR(t->lines[1].width, 15, 1e-4);
    CHECK_NEAR(t->max_line, 55, 1e-4);
    CHECK_NEAR(t->min_word, 15, 1e-4);
    bare_ctx_end(ctx, NULL);
}

TEST(wrap_exact_width_fits) {
    fresh(false);
    begin(1);
    BareText *t = wrap("aaa bbb", 35, false);
    CHECK(t->line_count == 1);
    bare_ctx_end(ctx, NULL);
}

TEST(wrap_long_word_breaks_inside) {
    fresh(false);
    begin(1);
    BareText *t = wrap("abcdefghij", 22, false);   /* 4 characters a line */
    CHECK(t->line_count == 3);
    CHECK(line_is(t, 0, "abcd"));
    CHECK(line_is(t, 1, "efgh"));
    CHECK(line_is(t, 2, "ij"));
    bare_ctx_end(ctx, NULL);
}

TEST(wrap_after_hyphens) {
    fresh(false);
    begin(1);
    BareText *t = wrap("well-known", 30, false);
    CHECK(t->line_count == 2);
    CHECK(line_is(t, 0, "well-"));
    CHECK(line_is(t, 1, "known"));
    CHECK_NEAR(t->min_word, 25, 1e-4);   /* "well-" and "known" */
    bare_ctx_end(ctx, NULL);
}

TEST(wrap_spaces_hang_and_vanish_at_line_starts) {
    fresh(false);
    begin(1);
    BareText *t = wrap("aaa    bbb", 20, false);
    CHECK(t->line_count == 2);
    CHECK(line_is(t, 0, "aaa"));
    CHECK(line_is(t, 1, "bbb"));
    CHECK_NEAR(t->lines[0].width, 15, 1e-4);
    bare_ctx_end(ctx, NULL);
}

TEST(wrap_newlines_and_empty_lines) {
    fresh(false);
    begin(1);
    BareText *t = wrap("a\n\nbb\n", 1000, false);
    CHECK(t->line_count == 4);
    CHECK(line_is(t, 0, "a"));
    CHECK(t->lines[1].count == 0);
    CHECK(line_is(t, 2, "bb"));
    CHECK(t->lines[3].count == 0);
    CHECK_NEAR(t->max_line, 10, 1e-4);
    bare_ctx_end(ctx, NULL);
}

TEST(wrap_nowrap_keeps_one_line) {
    fresh(false);
    begin(1);
    BareText *t = wrap("aaa bbb ccc", 20, true);
    CHECK(t->line_count == 1);
    CHECK_NEAR(t->min_word, t->max_line, 0);
    bare_ctx_end(ctx, NULL);
}

TEST(wrap_empty_text_is_one_line) {
    fresh(false);
    begin(1);
    BareText *t = wrap("", 100, false);
    CHECK(t->line_count == 1);
    CHECK_NEAR(bare__text_height(t), 13, 1e-4);   /* 10 px x the default line height 1.3 */
    bare_ctx_end(ctx, NULL);
}

/* ---- layout ----------------------------------------------------------------- */

static const BareElem *find_text(int nth) {
    for (int i = 0; i < ctx->elem_count; i++)
        if (ctx->elems[i].text && nth-- == 0) return &ctx->elems[i];
    return NULL;
}

TEST(layout_text_size_from_content) {
    fresh(false);
    begin(1);
    bare_text("hello", .font = fake, .size = 20);
    bare_ctx_end(ctx, NULL);
    const BareElem *e = find_text(0);
    CHECK(e != NULL);
    CHECK_NEAR(e->rect.w, 50, 1e-4);    /* 5 x 10 px */
    CHECK_NEAR(e->rect.h, 26, 1e-4);    /* 20 x 1.3 */
}

TEST(layout_text_wraps_in_a_narrow_column) {
    fresh(false);
    begin(1);
    bare_column(.width = 60) {
        bare_text("aaa bbb ccc ddd", .font = fake, .size = 10, .line_height = 2);
    }
    bare_ctx_end(ctx, NULL);
    const BareElem *e = find_text(0);
    CHECK_NEAR(e->rect.w, 60, 1e-4);   /* fit-content: no wider than the column */
    CHECK(e->text->line_count == 2);   /* "aaa bbb ccc" is 55 and fits; "ddd" goes below */
    CHECK_NEAR(e->rect.h, 40, 1e-4);
    const BareElem *col = &ctx->elems[e->parent];
    CHECK_NEAR(col->rect.h, 40, 1e-4); /* the column grows to the wrapped height */
}

TEST(layout_text_shrinks_in_a_row_to_its_longest_word) {
    fresh(false);
    begin(1);
    bare_row(.width = 100) {
        bare_box(.width = 70, .height = 10);
        bare_text("aa bbbb cc", .font = fake, .size = 10);   /* 50 wide; longest word 20 */
    }
    bare_ctx_end(ctx, NULL);
    const BareElem *e = find_text(0);
    CHECK_NEAR(e->rect.w, 30, 1e-4);
    CHECK(e->text->line_count == 3);   /* "aa bbbb" is 35 > 30, "bbbb cc" too */
}

TEST(layout_text_alignment_in_emit) {
    fresh(false);
    begin(1);
    bare_column(.width = 100, .align = BARE_STRETCH) {
        bare_text("ab", .font = fake, .size = 10, .align = BARE_END);
    }
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(l->cmd_count == 1 && l->cmds[0].kind == BARE_CMD_TEXT);
    const BareTextCmd *t = &l->cmds[0].as.text;
    CHECK(t->count == 2);
    CHECK_NEAR(t->glyphs[0].dst.x, 90, 1e-4);   /* 100 - 10 */
    CHECK_NEAR(t->glyphs[1].dst.x, 95, 1e-4);
}

/* ---- emit and the atlas ------------------------------------------------------- */

TEST(emit_glyph_quads_and_baseline) {
    fresh(false);
    begin(1);
    bare_text("a b", .font = fake, .size = 10, .line_height = 2);
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(l->cmd_count == 1);
    const BareTextCmd *t = &l->cmds[0].as.text;
    CHECK(t->count == 2);   /* the space draws nothing */
    CHECK(t->atlas != 0);
    /* line 20 tall, font box 8 + 2: half-leading 5, baseline at 5 + 8 = 13; glyph 5x7 sits on it */
    CHECK_NEAR(t->glyphs[0].dst.x, 0, 1e-4);
    CHECK_NEAR(t->glyphs[0].dst.y, 6, 1e-4);
    CHECK_NEAR(t->glyphs[0].dst.w, 5, 1e-4);
    CHECK_NEAR(t->glyphs[0].dst.h, 7, 1e-4);
    CHECK_NEAR(t->glyphs[1].dst.x, 10, 1e-4);
    CHECK(t->glyphs[0].uv.w > 0 && t->glyphs[0].uv.w <= 1 && t->glyphs[0].uv.x + t->glyphs[0].uv.w <= 1);
}

TEST(emit_text_colour_is_inherited) {
    fresh(false);
    begin(1);
    bare_column(.text_color = BARE_RGB(255, 0, 0)) {
        bare_row() { bare_text("x", .font = fake); }
        bare_text("y", .font = fake, .color = BARE_RGB(0, 0, 255));
    }
    bare_text("z", .font = fake);
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(l->cmd_count == 3);
    CHECK_NEAR(l->cmds[0].as.text.color.r, 1, 1e-6);
    CHECK_NEAR(l->cmds[0].as.text.color.b, 0, 1e-6);
    CHECK_NEAR(l->cmds[1].as.text.color.b, 1, 1e-6);
    CHECK_NEAR(l->cmds[1].as.text.color.r, 0, 1e-6);
    CHECK_NEAR(l->cmds[2].as.text.color.g, 1, 1e-6);   /* default: white */
}

TEST(emit_physical_size_at_2x) {
    fresh(false);
    begin(2);
    bare_text("a", .font = fake, .size = 10);
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    const BareTextCmd *t = &l->cmds[0].as.text;
    CHECK_NEAR(t->glyphs[0].dst.w, 10, 1e-4);   /* rasterized at 20 px, not stretched */
    CHECK_NEAR(t->glyphs[0].dst.h, 14, 1e-4);
}

TEST(atlas_rasterizes_each_glyph_once) {
    fresh(false);
    begin(1);
    bare_text("abcabc", .font = fake);
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(fake_rasters == 3);
    CHECK(l->upload_count == 1);
    const BareTextureUpload *u = &l->uploads[0];
    CHECK(u->format == BARE_TEX_R8 && u->width == 1024 && u->height == 1024);
    CHECK(u->texture == l->cmds[0].as.text.atlas);
    CHECK(u->w == 1024 && u->h == 1024);   /* a new texture: all of it */

    begin(1);
    bare_text("cab", .font = fake);
    l = bare_ctx_end(ctx, NULL);
    CHECK(fake_rasters == 3);             /* cached */
    CHECK(l->upload_count == 0);          /* nothing new */

    begin(1);
    bare_text("abd", .font = fake);
    l = bare_ctx_end(ctx, NULL);
    CHECK(fake_rasters == 4);
    CHECK(l->upload_count == 1);
    CHECK(l->uploads[0].w == 8 && l->uploads[0].h == 12);   /* just the new glyph (8x12 at 16 px) */
    CHECK(l->uploads[0].stride == 1024);
}

TEST(atlas_sizes_are_cached_separately) {
    fresh(false);
    begin(1);
    bare_text("a", .font = fake, .size = 10);
    bare_text("a", .font = fake, .size = 12);
    bare_ctx_end(ctx, NULL);
    CHECK(fake_rasters == 2);
    begin(2);   /* the same text on a 2x display is new physical sizes */
    bare_text("a", .font = fake, .size = 10);
    bare_ctx_end(ctx, NULL);
    CHECK(fake_rasters == 3);
}

static bool overlaps(const BareGlyphSlot *a, const BareGlyphSlot *b) {
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

static bool atlas_is_tidy(void) {
    int n = 0;
    const BareGlyphSlot *s[4096];
    for (int i = 0; i < ctx->glyph_cap && n < 4096; i++)
        if (ctx->glyphs[i].key && ctx->glyphs[i].w > 0) s[n++] = &ctx->glyphs[i];
    for (int i = 0; i < n; i++) {
        if (s[i]->x < 0 || s[i]->y < 0 || s[i]->x + s[i]->w > ctx->atlas_w || s[i]->y + s[i]->h > ctx->atlas_h)
            return false;
        for (int j = i + 1; j < n; j++)
            if (overlaps(s[i], s[j])) return false;
    }
    return n > 0;
}

TEST(atlas_packing_never_overlaps) {
    fresh(false);
    char line[64];
    for (int frame = 0; frame < 4; frame++) {
        begin(1);
        for (int size = 8; size <= 60; size += 4) {
            int n = 0;
            for (int c = 0; c < 30; c++) line[n++] = (char)('A' + (c + frame * 7) % 58);
            line[n] = 0;
            bare_text(line, .font = fake, .size = (float)size);
        }
        bare_ctx_end(ctx, NULL);
    }
    CHECK(atlas_is_tidy());
    CHECK(ctx->atlas_w == 1024);
}

TEST(atlas_grows_when_full_and_resends_everything) {
    fresh(false);
    fake_glyph_scale = 3;   /* big boxes: 1024x1024 fills quickly */
    char line[2];
    line[1] = 0;
    begin(1);
    for (int c = 0; c < 90; c++) {
        line[0] = (char)(33 + c);
        bare_text(line, .font = fake, .size = 60);   /* 90x126 glyphs */
    }
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(ctx->atlas_w == 2048 && ctx->atlas_h == 2048);
    CHECK(l->upload_count == 1);
    CHECK(l->uploads[0].width == 2048 && l->uploads[0].w == 2048 && l->uploads[0].h == 2048);
    CHECK(atlas_is_tidy());
    /* uvs are normalised by the final size, even for glyphs placed before it grew */
    for (int i = 0; i < l->cmd_count; i++) {
        const BareGlyph *g = &l->cmds[i].as.text.glyphs[0];
        CHECK(g->uv.x + g->uv.w <= 1.0f && g->uv.y + g->uv.h <= 1.0f);
        CHECK_NEAR(g->uv.w * 2048, 90, 1e-3);
    }
    CHECK(log_count == 0);
}

TEST(atlas_full_starts_over_next_frame) {
    fresh(false);
    fake_glyph_scale = 3;
    char line[2];
    line[1] = 0;
    bool more = false;
    begin(1);
    for (int c = 0; c < 400; c++) {   /* more 90x126 boxes than 2048x2048 holds (~ 352) */
        line[0] = (char)(c % 94 + 33);
        bare_text(line, .font = fake, .size = 60.0f + (float)(c / 94) * 0.5f);
    }
    bare_ctx_end(ctx, &more);
    CHECK(log_count == 1);
    CHECK(strstr(last_log, "atlas is full") != NULL);
    CHECK(more);   /* skipped glyphs: another frame is needed */
    begin(1);
    bare_text("A", .font = fake, .size = 60);
    const BareDrawList *l = bare_ctx_end(ctx, &more);
    CHECK(!more);
    CHECK(l->cmd_count == 1 && l->cmds[0].as.text.count == 1);
    CHECK(atlas_is_tidy());
}

/* ---- the real default font ---------------------------------------------------- */

TEST(default_font_widths_match_advances) {
    fresh(true);
    const BareFontBackend *fb = bare_font_backend_stb();
    begin(1);
    bare_text("Hello, world", .size = 16);
    bare_ctx_end(ctx, NULL);
    const BareElem *e = find_text(0);
    CHECK(e && e->text->glyph_count == 12);
    float sum = 0;
    for (int i = 0; i < e->text->glyph_count; i++) sum += e->text->glyphs[i].advance;
    CHECK_NEAR(e->rect.w, sum, 1e-3);
    CHECK(sum > 70 && sum < 110);   /* Inter at 16 px: about 88 */
    CHECK_NEAR(e->rect.h, 16 * 1.3f, 1e-4);
    (void)fb;
    CHECK(log_count == 0);
}

TEST(default_font_kerns) {
    fresh(true);
    begin(1);
    BareTextStyle st = { .size = 100 };
    BareText *av = bare__text_new(ctx, "AV", 2, &st);
    BareText *a = bare__text_new(ctx, "A", 1, &st);
    CHECK(av && a && av->glyph_count == 2);
    CHECK(av->glyphs[0].advance < a->glyphs[0].advance - 1.0f);   /* GPOS pair kerning pulls V in */
    CHECK(av->ascent > 80 && av->ascent < 110 && av->descent > 15 && av->descent < 35);
    bare_ctx_end(ctx, NULL);
}

TEST(default_font_draws_crisp_glyphs) {
    fresh(true);
    begin(1);
    bare_text("Bare", .size = 24);
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(l->cmd_count == 1 && l->cmds[0].as.text.count == 4);
    const BareGlyph *g = l->cmds[0].as.text.glyphs;
    for (int i = 0; i < 4; i++) {
        CHECK(g[i].dst.x == (float)(int)g[i].dst.x);   /* whole pixels when still */
        CHECK(g[i].dst.y == (float)(int)g[i].dst.y);
        CHECK(g[i].dst.w > 5 && g[i].dst.h > 10);
    }
    CHECK(l->upload_count == 1);
    /* the atlas holds real coverage */
    int ink = 0;
    for (int i = 0; i < ctx->atlas_w * 64; i++) ink += ctx->atlas[i] > 128;
    CHECK(ink > 50);
}

TEST(textf_formats) {
    fresh(false);
    begin(1);
    bare_textf("%d-%s", 42, "ok");
    bare_ctx_end(ctx, NULL);
    const BareElem *e = find_text(0);
    CHECK(e && e->text->len == 5 && memcmp(e->text->str, "42-ok", 5) == 0);
}

TEST(bad_font_data_falls_back) {
    fresh(false);
    BareFont f = bare_font_from_memory("nope", 4);
    CHECK(f == 0);
    CHECK(log_count == 1);
    CHECK(bare_font_load("does/not/exist.ttf") == 0);
    CHECK(log_count == 2);
}

TEST(text_state_and_hits) {
    fresh(false);
    begin(1);
    bare_text("click", .id = BARE_ID("t"), .font = fake, .size = 10);
    bare_ctx_end(ctx, NULL);
    begin(1);
    BareState s = bare_text("click", .id = BARE_ID("t"), .font = fake, .size = 10);
    bare_ctx_end(ctx, NULL);
    CHECK_NEAR(s.rect.w, 25, 1e-4);
    CHECK_NEAR(s.rect.h, 13, 1e-4);
}

/* ---- images -------------------------------------------------------------------- */

TEST(image_load_decodes_png) {
    fresh(false);
    BareImage im = bare_image_load(BARE_TEST_ASSETS "avatar.png");
    CHECK(im != 0);
    BareVec2 size = bare_image_size(im);
    CHECK(size.x == 192 && size.y == 192);
    begin(1);
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(l->op_count == 2);
    CHECK(l->ops[0].kind == BARE_GPU_IMAGE_CREATE && l->ops[1].kind == BARE_GPU_IMAGE_UPLOAD);
    const unsigned char *px = l->ops[1].as.image.pixels;
    CHECK(px && px[3] == 255);   /* opaque RGBA8 */
    CHECK(bare_image_load("missing.png") == 0);
    CHECK(log_count == 1 && strstr(last_log, "missing.png"));
}

int main(void) {
    RUN(utf8_valid_sequences);
    RUN(utf8_invalid_bytes_become_replacement_characters);
    RUN(wrap_at_spaces);
    RUN(wrap_exact_width_fits);
    RUN(wrap_long_word_breaks_inside);
    RUN(wrap_after_hyphens);
    RUN(wrap_spaces_hang_and_vanish_at_line_starts);
    RUN(wrap_newlines_and_empty_lines);
    RUN(wrap_nowrap_keeps_one_line);
    RUN(wrap_empty_text_is_one_line);
    RUN(layout_text_size_from_content);
    RUN(layout_text_wraps_in_a_narrow_column);
    RUN(layout_text_shrinks_in_a_row_to_its_longest_word);
    RUN(layout_text_alignment_in_emit);
    RUN(emit_glyph_quads_and_baseline);
    RUN(emit_text_colour_is_inherited);
    RUN(emit_physical_size_at_2x);
    RUN(atlas_rasterizes_each_glyph_once);
    RUN(atlas_sizes_are_cached_separately);
    RUN(atlas_packing_never_overlaps);
    RUN(atlas_grows_when_full_and_resends_everything);
    RUN(atlas_full_starts_over_next_frame);
    RUN(default_font_widths_match_advances);
    RUN(default_font_kerns);
    RUN(default_font_draws_crisp_glyphs);
    RUN(textf_formats);
    RUN(bad_font_data_falls_back);
    RUN(text_state_and_hits);
    RUN(image_load_decodes_png);
    if (ctx) bare_ctx_destroy(ctx);
    return test_report();
}
