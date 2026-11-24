/* Styles and themes, headless: what wins, chains, cycles, looks, text styles
 * and the theme's background and text colour. */
#include "bare_test.h"
#include "bare.h"
#include "internal.h"
#include <string.h>

static BareContext *ctx;
static int log_count;
static char last_log[512];
static const BareDrawList *list;

static void capture_log(void *user, const char *msg) {
    (void)user;
    snprintf(last_log, sizeof last_log, "%s", msg);
    log_count++;
}

static void begin(void) {
    if (!ctx) {
        ctx = bare_ctx_create(NULL);
        bare_ctx_set_log(ctx, capture_log, NULL);
        bare_ctx_make_current(ctx);
        log_count = 0;
    }
    bare_ctx_begin(ctx, 800, 600, 1.0f, 0.0);
}
static void end(void) { list = bare_ctx_end(ctx, NULL); }
static void reset(void) { bare_ctx_destroy(ctx); ctx = NULL; }
static const BareBox *B(int i) { return &ctx->elems[i].box; }

static void move(float x, float y) {
    BareEvent e = { .kind = BARE_EVENT_MOUSE_MOVE };
    e.as.mouse.x = x; e.as.mouse.y = y;
    bare_ctx_event(ctx, &e);
}

static const BareBox card = {
    .padding = 20, .radius = 16, .opacity = 0.5f, .gap = 8, .align = BARE_CENTER,
    .color = { 36, 38, 46, 255 }, .border = 2, .border_color = { 9, 9, 9, 255 },
    .shadow = { 0, 8, 24, 0, { 0, 0, 0, 70 } }, .cursor = BARE_CURSOR_HAND,
    .hover = { .color = { 50, 52, 60, 255 }, .offset = { 0, -4 } },
};

TEST(element_fields_beat_the_style_and_the_style_beats_defaults) {
    begin();
    bare_column(.style = &card, .radius = 4, .gap = 2) { bare_box(.width = 10, .height = 10); }
    end();
    CHECK_NEAR(B(1)->radius, 4, 0);              /* the element's */
    CHECK_NEAR(B(1)->gap, 2, 0);
    CHECK_NEAR(B(1)->pad.left, 20, 0);           /* the style's */
    CHECK_NEAR(B(1)->opacity, 0.5, 1e-6);
    CHECK(B(1)->color.r == 36 && B(1)->align == BARE_CENTER);
    CHECK_NEAR(B(1)->scale, 1, 0);               /* neither: the default */
    CHECK(B(1)->dir == BARE_COLUMN);
    CHECK_NEAR(ctx->elems[1].rect.w, 50, 0.01);  /* 20 + 10 + 20: laid out with the style's padding */
    reset();
}

TEST(deliberate_nothings_override_a_style) {
    begin();
    bare_column(.style = &card, .radius = BARE_ZERO, .opacity = BARE_ZERO, .color = BARE_CLEAR,
                .align = BARE_START, .cursor = BARE_CURSOR_ARROW, .animate = BARE_INSTANT) {}
    end();
    CHECK_NEAR(B(1)->radius, 0, 0);
    CHECK_NEAR(B(1)->opacity, 0, 0);
    CHECK(B(1)->color.a == 0);
    CHECK(B(1)->align == BARE_START);
    CHECK(B(1)->cursor == BARE_CURSOR_ARROW);
    reset();
}

TEST(clear_draws_no_background) {
    begin();
    bare_box(.style = &card, .width = 50, .height = 50, .color = BARE_CLEAR, .border = BARE_ZERO,
             .shadow = { .color = BARE_CLEAR });
    end();
    CHECK(list->cmd_count == 0);
    reset();
}

TEST(padding_replaces_a_style_pad_as_a_whole) {
    static const BareBox padded = { .pad = { 1, 2, 3, 4 } };
    begin();
    bare_column(.style = &padded, .padding = 9) {}
    bare_column(.style = &padded) {}
    end();
    CHECK_NEAR(B(1)->pad.top, 9, 0);
    CHECK_NEAR(B(1)->pad.left, 9, 0);
    CHECK_NEAR(B(2)->pad.left, 4, 0);
    reset();
}

TEST(groups_come_whole_from_one_place) {
    begin();
    bare_box(.style = &card, .shadow = { .blur = 4, .color = { 255, 0, 0, 255 } });
    end();
    CHECK_NEAR(B(1)->shadow.y, 0, 0);            /* not the style's 8: the element gave the whole shadow */
    CHECK_NEAR(B(1)->shadow.blur, 4, 0);
    reset();
}

TEST(a_styles_hover_look_applies_and_combines_with_the_elements) {
    begin();
    bare_box(.id = BARE_ID("c"), .style = &card, .width = 100, .height = 100, .hover = { .scale = 1.1f });
    end();
    move(50, 50);
    begin();
    bare_box(.id = BARE_ID("c"), .style = &card, .width = 100, .height = 100, .hover = { .scale = 1.1f });
    end();
    CHECK(B(1)->color.r == 50);                  /* the style's hover colour */
    CHECK_NEAR(B(1)->scale, 1.1, 1e-6);          /* the element's hover scale */
    CHECK_NEAR(B(1)->offset.y, -4, 1e-6);        /* the style's hover offset */
    CHECK(bare_ctx_cursor(ctx) == BARE_CURSOR_HAND);
    reset();
}

TEST(the_arrow_cursor_overrides_a_parents_hand) {
    begin();
    bare_column(.width = 100, .height = 100, .cursor = BARE_CURSOR_HAND) {
        bare_box(.width = 50, .height = 50, .cursor = BARE_CURSOR_ARROW);
    }
    end();
    move(10, 10);
    begin();
    bare_column(.width = 100, .height = 100, .cursor = BARE_CURSOR_HAND) {
        bare_box(.width = 50, .height = 50, .cursor = BARE_CURSOR_ARROW);
    }
    end();
    CHECK(bare_ctx_cursor(ctx) == BARE_CURSOR_DEFAULT);
    reset();
}

static const BareBox base = { .radius = 3, .gap = 3, .color = { 1, 1, 1, 255 } };
static const BareBox mid = { .style = &base, .gap = 5 };
static const BareBox top = { .style = &mid, .color = { 7, 7, 7, 255 } };

TEST(chained_styles_layer_from_the_base_up) {
    begin();
    bare_column(.style = &top) {}
    end();
    CHECK_NEAR(B(1)->radius, 3, 0);              /* base */
    CHECK_NEAR(B(1)->gap, 5, 0);                 /* mid over base */
    CHECK(B(1)->color.r == 7);                   /* top over both */
    CHECK(log_count == 0);
    reset();
}

static BareBox loop_a, loop_b;

TEST(a_style_cycle_is_reported_once_and_cut) {
    loop_a = (BareBox){ .style = &loop_b, .radius = 2 };
    loop_b = (BareBox){ .style = &loop_a, .gap = 6 };
    for (int f = 0; f < 3; f++) {
        begin();
        bare_column(.style = &loop_a, .padding = 1) {}
        end();
    }
    CHECK(log_count == 1);
    CHECK(strstr(last_log, "test_style.c") != NULL);
    CHECK_NEAR(B(1)->radius, 2, 0);              /* what was reached before the loop still applies */
    CHECK_NEAR(B(1)->gap, 6, 0);
    reset();
}

TEST(text_styles_work_the_same_way) {
    static const BareTextStyle heading = { .size = 24, .color = { 200, 10, 10, 255 }, .line_height = 1.5f };
    static const BareTextStyle loud = { .style = &heading, .size = 30 };
    begin();
    bare_text("Hi", .style = &loud);
    bare_text("Hi", .style = &heading, .size = 12);
    end();
    CHECK_NEAR(ctx->elems[1].text->size, 30, 0);
    CHECK_NEAR(ctx->elems[1].text->line_height, 45, 1e-4);
    CHECK(ctx->elems[1].box.text_color.r == 200);
    CHECK_NEAR(ctx->elems[2].text->size, 12, 0);
    reset();
}

/* ---- themes ----------------------------------------------------------------- */

TEST(the_theme_gives_the_background_and_text_colour) {
    begin(); end();
    CHECK_NEAR(list->clear.r, 24 / 255.0, 1e-6);   /* dark until told otherwise */
    bare_set_theme(bare_theme_light());
    begin();
    bare_textf("ink");
    end();
    CHECK_NEAR(list->clear.r, 244 / 255.0, 1e-6);
    CHECK(list->cmd_count == 1 && list->cmds[0].kind == BARE_CMD_TEXT);
    CHECK_NEAR(list->cmds[0].as.text.color.r, 24 / 255.0, 1e-6);
    bare_ctx_set_clear(ctx, BARE_RGB(1, 2, 3));   /* bare_background: the app's choice wins over the theme */
    begin(); end();
    CHECK_NEAR(list->clear.b, 3 / 255.0, 1e-6);
    reset();
}

TEST(theme_styles_follow_a_theme_switch_and_animate) {
    begin();
    const BareTheme *t = bare_theme();
    bare_column(.id = BARE_ID("c"), .style = &bare_theme()->card) {}
    end();
    CHECK(B(1)->color.r == bare_theme_dark()->card.color.r);
    bare_set_theme(bare_theme_light());
    CHECK(bare_theme() == t);                     /* the same place, new values */
    bool again = false;
    for (int f = 1; f <= 30; f++) {
        bare_ctx_begin(ctx, 800, 600, 1.0f, f / 60.0);
        bare_column(.id = BARE_ID("c"), .style = &bare_theme()->card) {}
        bare_ctx_end(ctx, &again);
        if (f == 2) CHECK(again && B(1)->color.r > 36 && B(1)->color.r < 255);   /* fading over */
    }
    CHECK(!again && B(1)->color.r == 255);
    reset();
}

TEST(a_tweaked_copy_of_a_theme) {
    BareTheme mine = *bare_theme_dark();
    mine.primary.color = BARE_RGB(200, 60, 120);
    begin();
    bare_set_theme(&mine);
    bare_row(.style = &bare_theme()->primary) { bare_textf("Go"); }
    end();
    CHECK(B(1)->color.r == 200);
    CHECK(bare_theme_dark()->primary.color.r == 40);   /* the built-in is untouched */
    reset();
}

TEST(glass_stack_presets_and_overrides) {
    begin();
    bare_glass_stack(.width = 400, .height = 300) {
        bare_glass_layer(BARE_GLASS_FROST, .width = 160, .height = 80, .radius = 20) {
            bare_glass_surface(BARE_GLASS_LENS, .width = 32, .height = 32,
                               .glass = { .refraction = 1.8f, .blur = BARE_ZERO });
        }
        bare_glass_surface(BARE_GLASS_DOCK, .width = 100, .height = 40);
        bare_glass_surface(BARE_GLASS_DROP, .width = 40, .height = 40);
    }
    end();
    CHECK(B(1)->dir == BARE_STACK);
    CHECK_NEAR(B(2)->glass.blur, 18, 0.01);
    CHECK_NEAR(B(3)->glass.refraction, 1.8, 0.01);
    CHECK_NEAR(B(3)->glass.blur, 0, 0.01);
    CHECK_NEAR(B(4)->glass.merge, 56, 0.01);
    CHECK_NEAR(B(5)->glass.merge, 56, 0.01);
    reset();
}

int main(void) {
    RUN(glass_stack_presets_and_overrides);
    RUN(element_fields_beat_the_style_and_the_style_beats_defaults);
    RUN(deliberate_nothings_override_a_style);
    RUN(clear_draws_no_background);
    RUN(padding_replaces_a_style_pad_as_a_whole);
    RUN(groups_come_whole_from_one_place);
    RUN(a_styles_hover_look_applies_and_combines_with_the_elements);
    RUN(the_arrow_cursor_overrides_a_parents_hand);
    RUN(chained_styles_layer_from_the_base_up);
    RUN(a_style_cycle_is_reported_once_and_cut);
    RUN(text_styles_work_the_same_way);
    RUN(the_theme_gives_the_background_and_text_colour);
    RUN(theme_styles_follow_a_theme_switch_and_animate);
    RUN(a_tweaked_copy_of_a_theme);
    return test_report();
}
