/* Clipping and scroll boxes, headless: wheel, hand-off, the glide, layout of
 * scroll boxes, clips in the draw list, bare_visible and the scrollbar. */
#include "bare_test.h"
#include "bare.h"
#include "internal.h"
#include <string.h>

static BareContext *ctx;
static bool again;              /* bare_ctx_end asked for another frame */
static double now;              /* the clock passed to bare_ctx_begin */
static const BareDrawList *list;

static void begin(void) {
    if (!ctx) {
        ctx = bare_ctx_create(NULL);
        bare_ctx_make_current(ctx);
        now = 1.0;
    }
    bare_ctx_begin(ctx, 800, 600, 1.0f, now);
}
static void end(void) { list = bare_ctx_end(ctx, &again); }
static void reset(void) { bare_ctx_destroy(ctx); ctx = NULL; }

static void move(float x, float y) {
    BareEvent e = { .kind = BARE_EVENT_MOUSE_MOVE };
    e.as.mouse.x = x; e.as.mouse.y = y;
    bare_ctx_event(ctx, &e);
}
static void button(bool down, float x, float y) {
    BareEvent e = { .kind = BARE_EVENT_MOUSE_BUTTON };
    e.as.button.button = BARE_MOUSE_LEFT; e.as.button.down = down; e.as.button.x = x; e.as.button.y = y;
    bare_ctx_event(ctx, &e);
}
static void wheel(float dx, float dy, bool precise) {
    BareEvent e = { .kind = BARE_EVENT_WHEEL };
    e.as.wheel.dx = dx; e.as.wheel.dy = dy; e.as.wheel.precise = precise;
    bare_ctx_event(ctx, &e);
}
static void key(BareKey k, bool down) {
    BareEvent e = { .kind = BARE_EVENT_KEY };
    e.as.key.key = k; e.as.key.down = down;
    bare_ctx_event(ctx, &e);
}

static int find(BareId id) {
    for (int i = 0; i < ctx->elem_count; i++)
        if (ctx->elems[i].id == id) return i;
    return -1;
}

/* A 100x100 list at the window's top left: `n` items of 30 (content n * 30). */
typedef struct List { BareState box, items[64]; } List;

static List list_ui(int n) {
    List l;
    memset(&l, 0, sizeof l);
    begin();
    bare_column(.id = BARE_ID("list"), .width = 100, .height = 100, .scroll = BARE_SCROLL_Y) {
        l.box = bare_this();
        for (int i = 0; i < n; i++) l.items[i] = bare_box(.id = BARE_IDI("item", i), .width = 100, .height = 30);
    }
    end();
    return l;
}

/* ---- wheel ------------------------------------------------------------------ */

TEST(touchpad_pixels_scroll_at_once_and_clamp_to_the_content) {
    list_ui(10);
    move(50, 50);
    wheel(0, -50, true);
    CHECK_NEAR(list_ui(10).box.scroll.y, 50, 0.01);
    wheel(0, -1000, true);                          /* content 300, box 100 */
    CHECK_NEAR(list_ui(10).box.scroll.y, 200, 0.01);
    wheel(0, 500, true);
    CHECK_NEAR(list_ui(10).box.scroll.y, 0, 0.01);
    reset();
}

TEST(a_notch_glides_to_its_target_then_rests) {
    list_ui(10);
    move(50, 50);
    wheel(0, -1, false);                            /* one notch down: 100 px */
    now += 1.0 / 60;
    float first = list_ui(10).box.scroll.y;
    CHECK(first > 1 && first < 100);                /* moving, not jumped */
    CHECK(again);
    float prev = first;
    for (int f = 0; f < 60; f++) {
        now += 1.0 / 60;
        float y = list_ui(10).box.scroll.y;
        CHECK(y >= prev - 1e-3f && y <= 100.001f);   /* critically damped: never overshoots */
        prev = y;
    }
    CHECK_NEAR(prev, 100, 0.001);
    now += 1.0 / 60;
    list_ui(10);
    CHECK(!again);                                  /* settled: the app can sleep */
    reset();
}

TEST(a_box_that_fits_its_content_doesnt_scroll) {
    list_ui(3);                                     /* 90 in 100 */
    move(50, 50);
    wheel(0, -5, true);
    CHECK_NEAR(list_ui(3).box.scroll.y, 0, 0.0);
    reset();
}

TEST(scrolled_children_are_drawn_and_hit_where_they_show) {
    list_ui(10);
    move(50, 50);
    wheel(0, -50, true);
    list_ui(10);                                    /* drawn scrolled by 50 */
    List l = list_ui(10);                           /* states report that frame */
    CHECK_NEAR(l.items[0].rect.y, -50, 0.01);
    CHECK_NEAR(l.items[3].rect.y, 40, 0.01);
    CHECK(l.items[3].hovered);                      /* 90 + 30 - 50 = 40..70 */
    CHECK(!l.items[1].hovered);
    move(50, 150);                                  /* below the box: item 6 is there, but clipped */
    l = list_ui(10);
    CHECK(!l.items[6].hovered);
    reset();
}

TEST(offsets_snap_to_physical_pixels_so_text_stays_sharp) {
    list_ui(10);
    move(50, 50);
    wheel(0, -10.4f, true);
    List l = list_ui(10);
    CHECK_NEAR(l.box.scroll.y, 10.4, 0.001);        /* the app sees the exact value */
    l = list_ui(10);
    CHECK_NEAR(l.items[0].rect.y, -10, 0.001);      /* drawn on the pixel grid */
    reset();
}

/* An outer 200-tall scroll box (content 400) with an inner 100-tall one
 * (content 300) at its top, and a horizontal strip below. */
static void nested(BareState *outer, BareState *inner) {
    begin();
    bare_column(.id = BARE_ID("outer"), .width = 300, .height = 200, .scroll = BARE_SCROLL_Y) {
        *outer = bare_this();
        bare_column(.id = BARE_ID("inner"), .width = 300, .height = 100, .scroll = BARE_SCROLL_Y) {
            *inner = bare_this();
            for (int i = 0; i < 10; i++) bare_box(.id = BARE_IDI("in", i), .width = 300, .height = 30);
        }
        bare_box(.width = 300, .height = 300);
    }
    end();
}

TEST(wheel_goes_to_the_inner_box_until_its_end_then_the_outer) {
    BareState outer, inner;
    nested(&outer, &inner);
    move(50, 50);
    wheel(0, -150, true);
    nested(&outer, &inner);
    CHECK_NEAR(inner.scroll.y, 150, 0.01);
    CHECK_NEAR(outer.scroll.y, 0, 0.01);
    wheel(0, -100, true);                           /* the inner one takes what it can */
    nested(&outer, &inner);
    CHECK_NEAR(inner.scroll.y, 200, 0.01);
    CHECK_NEAR(outer.scroll.y, 0, 0.01);
    wheel(0, -100, true);                           /* at its end: on to the outer one */
    nested(&outer, &inner);
    CHECK_NEAR(inner.scroll.y, 200, 0.01);
    CHECK_NEAR(outer.scroll.y, 100, 0.01);
    nested(&outer, &inner);
    move(50, 150);                                  /* the inner box scrolled out of view (-100..0): */
    wheel(0, 50, true);                             /* up goes to the page */
    nested(&outer, &inner);
    CHECK_NEAR(inner.scroll.y, 200, 0.01);
    CHECK_NEAR(outer.scroll.y, 50, 0.01);
    move(50, 75);                                   /* the inner box is at -50..50 now */
    nested(&outer, &inner);
    wheel(0, 30, true);
    nested(&outer, &inner);
    CHECK_NEAR(inner.scroll.y, 200, 0.01);          /* 75 is below it: the page again */
    CHECK_NEAR(outer.scroll.y, 20, 0.01);
    move(50, 50);                                   /* inner box at -20..80 */
    nested(&outer, &inner);
    wheel(0, 30, true);
    nested(&outer, &inner);
    CHECK_NEAR(inner.scroll.y, 170, 0.01);          /* over it: the inner box first */
    CHECK_NEAR(outer.scroll.y, 20, 0.01);
    reset();
}

TEST(horizontal_scrollers_pass_vertical_wheels_on_and_shift_turns_it) {
    BareState outer = { 0 }, strip = { 0 };
    for (int pass = 0; pass < 5; pass++) {
        begin();
        bare_column(.id = BARE_ID("page"), .width = 300, .height = 200, .scroll = BARE_SCROLL_Y) {
            outer = bare_this();
            bare_row(.id = BARE_ID("strip"), .width = 300, .height = 100, .scroll = BARE_SCROLL_X) {
                strip = bare_this();
                for (int i = 0; i < 10; i++) bare_box(.id = BARE_IDI("card", i), .width = 100, .height = 100);
            }
            bare_box(.width = 300, .height = 300);
        }
        end();
        if (pass == 0) { move(50, 50); wheel(0, -40, true); }
        if (pass == 1) { key(BARE_KEY_SHIFT, true); wheel(0, -30, true); key(BARE_KEY_SHIFT, false); }
        if (pass == 2) wheel(25, 0, true);          /* a touchpad's sideways swipe */
    }
    CHECK_NEAR(outer.scroll.y, 40, 0.01);           /* the strip can't scroll down: the page did */
    CHECK_NEAR(strip.scroll.x, 55, 0.01);
    CHECK_NEAR(outer.scroll.x, 0, 0.0);
    reset();
}

/* ---- layout ------------------------------------------------------------------- */

TEST(a_scroll_box_takes_the_space_there_is) {
    begin();
    bare_column(.width = 200, .height = 300) {
        bare_box(.width = 200, .height = 50);
        bare_column(.id = BARE_ID("s"), .width = 200, .scroll = BARE_SCROLL_Y) {   /* height: content (1000) */
            for (int i = 0; i < 10; i++) bare_box(.width = 200, .height = 100);
        }
    }
    end();
    int s = find(BARE_ID("s"));
    CHECK_NEAR(ctx->elems[s].rect.h, 250, 0.01);   /* squeezed into what's left */
    CHECK_NEAR(ctx->elems[s].content[1], 1000, 0.01);
    reset();
}

TEST(a_scroll_box_doesnt_shrink_its_children) {
    begin();
    bare_column(.id = BARE_ID("s"), .width = 100, .height = 100, .padding = 5, .gap = 2,
                .scroll = BARE_SCROLL_Y) {
        for (int i = 0; i < 5; i++)
            bare_row(.id = BARE_IDI("row", i)) { bare_box(.width = 50, .height = 40); }
    }
    end();
    CHECK_NEAR(ctx->elems[find(BARE_IDI("row", 4))].rect.h, 40, 0.01);
    CHECK_NEAR(ctx->elems[find(BARE_ID("s"))].content[1], 5 + 5 * 40 + 4 * 2 + 5, 0.01);
    reset();
}

TEST(clip_without_scroll_can_be_squeezed_too) {
    begin();
    bare_row(.width = 100, .height = 50) {
        bare_row(.id = BARE_ID("c"), .clip = true) { bare_box(.width = 300, .height = 20); }
    }
    end();
    CHECK_NEAR(ctx->elems[find(BARE_ID("c"))].rect.w, 100, 0.01);
    reset();
}

TEST(content_that_shrinks_pulls_the_offset_back) {
    list_ui(10);
    move(50, 50);
    wheel(0, -1000, true);
    CHECK_NEAR(list_ui(10).box.scroll.y, 200, 0.01);
    list_ui(5);                                     /* 150 of content: at most 50 */
    CHECK(again);                                   /* the app saw 200 this frame: one more */
    CHECK_NEAR(list_ui(5).box.scroll.y, 50, 0.01);
    reset();
}

TEST(children_with_animate_layout_dont_lag_behind_the_scroll) {
    BareState s = { 0 };
    for (int pass = 0; pass < 4; pass++) {
        begin();
        bare_column(.id = BARE_ID("list"), .width = 100, .height = 100, .scroll = BARE_SCROLL_Y) {
            for (int i = 0; i < 10; i++) {
                BareState t = bare_box(.id = BARE_IDI("item", i), .width = 100, .height = 30,
                                       .animate_layout = BARE_SPRING_SMOOTH);
                if (i == 2) s = t;
            }
        }
        end();
        if (pass == 0) { move(50, 50); wheel(0, -40, true); }
        now += 1.0 / 60;
    }
    CHECK_NEAR(s.rect.y, 60 - 40, 0.01);
    reset();
}

/* ---- clips in the draw list --------------------------------------------------- */

TEST(nested_clips_push_intersections_and_pop_in_pairs) {
    begin();
    bare_column(.padding = 10, .gap = 10) {
        bare_column(.width = 100, .height = 100, .clip = true, .border = 2, .border_color = BARE_WHITE) {
            bare_row(.width = 300, .height = 50, .clip = true) {
                bare_box(.width = 20, .height = 20, .color = BARE_RGB(1, 2, 3));
            }
        }
        bare_box(.width = 30, .height = 30, .color = BARE_RGB(4, 5, 6));
    }
    end();
    int depth = 0, max_depth = 0, pushes = 0;
    BareRect inner = { 0 };
    int inside_cmds = 0, after = -1;
    for (int i = 0; i < list->cmd_count; i++) {
        const BareDrawCmd *c = &list->cmds[i];
        if (c->kind == BARE_CMD_CLIP_PUSH) {
            depth++; pushes++;
            if (depth > max_depth) max_depth = depth;
            if (depth == 2) inner = c->as.clip.rect;
        } else if (c->kind == BARE_CMD_CLIP_POP) {
            depth--;
            CHECK(depth >= 0);
        } else if (c->kind == BARE_CMD_RECT && c->as.rect.fill.r > 0.003f && c->as.rect.fill.r < 0.005f) {
            if (depth == 2) inside_cmds++;
        } else if (c->kind == BARE_CMD_RECT && c->as.rect.fill.b > 0.02f && c->as.rect.fill.b < 0.03f) {
            after = depth;
        }
    }
    CHECK(depth == 0 && pushes == 2 && max_depth == 2);
    CHECK(inside_cmds == 1);
    CHECK(after == 0);                              /* the box after the clipped column isn't clipped */
    CHECK_NEAR(inner.x, 12, 0.0);                   /* the row (300 wide) cut by the outer clip, */
    CHECK_NEAR(inner.y, 12, 0.0);                   /* which is inside the 2 px border */
    CHECK_NEAR(inner.w, 96, 0.0);
    CHECK_NEAR(inner.h, 48, 0.0);                   /* the row is at 10..60, the clip starts at 12 */
    reset();
}

TEST(clipped_out_elements_draw_nothing) {
    begin();
    bare_column(.width = 100, .height = 100, .clip = true) {
        for (int i = 0; i < 100; i++) bare_box(.width = 100, .height = 30, .color = BARE_RGB(9, 9, 9));
    }
    end();
    int rects = 0;
    for (int i = 0; i < list->cmd_count; i++) rects += list->cmds[i].kind == BARE_CMD_RECT;
    CHECK(rects == 4);                              /* 0..90 show; 120 and on are culled */
    reset();
}

/* ---- bare_visible ----------------------------------------------------------------- */

#define ROWS 10000

/* A 200-tall list with padding, gap, a header and a footer, either declared
 * in full or with bare_visible; returns the states of `probe` items and the footer. */
static int declared;
static void long_list(bool virtual_, int n, const int *probe, int probes, BareState *out, BareState *footer,
                      BareState *box) {
    declared = 0;
    begin();
    bare_column(.id = BARE_ID("long"), .width = 300, .height = 200, .padding = 8, .gap = 4,
                .scroll = BARE_SCROLL_Y) {
        *box = bare_this();
        bare_box(.id = BARE_ID("header"), .width = 280, .height = 50);
        if (virtual_) {
            bare_visible(i, n, 20) {
                BareState st = bare_box(.id = BARE_IDI("row", i), .width = 280, .height = 20);
                for (int p = 0; p < probes; p++) if (probe[p] == i) out[p] = st;
                declared++;
            }
        } else {
            for (int i = 0; i < n; i++) {
                BareState st = bare_box(.id = BARE_IDI("row", i), .width = 280, .height = 20);
                for (int p = 0; p < probes; p++) if (probe[p] == i) out[p] = st;
                declared++;
            }
        }
        *footer = bare_box(.id = BARE_ID("footer"), .width = 280, .height = 30);
    }
    end();
}

TEST(visible_declares_only_what_shows_and_keeps_room_for_the_rest) {
    BareState st[1], footer, box;
    int probe[1] = { 0 };
    long_list(true, ROWS, probe, 1, st, &footer, &box);
    long_list(true, ROWS, probe, 1, st, &footer, &box);
    CHECK(declared <= 12);
    float content = 8 + 50 + 4 + ROWS * 24.0f - 4 + 4 + 30 + 8;
    CHECK_NEAR(ctx->elems[find(BARE_ID("long"))].content[1], content, 0.5);
    move(100, 100);
    wheel(0, -120000, true);
    long_list(true, ROWS, probe, 1, st, &footer, &box);
    CHECK_NEAR(box.scroll.y, 120000, 0.01);
    CHECK(declared <= 12);
    wheel(0, -1e9f, true);                          /* to the end: the footer shows at the bottom */
    long_list(true, ROWS, probe, 1, st, &footer, &box);
    long_list(true, ROWS, probe, 1, st, &footer, &box);
    CHECK_NEAR(footer.rect.y + footer.rect.h, 200 - 8, 0.01);
    reset();
}

TEST(visible_items_sit_where_a_full_list_puts_them) {
    int probe[3] = { 36, 40, 43 };                  /* on screen after scrolling 900 */
    BareState full[3], virt[3], ffoot, vfoot, box;
    for (int f = 0; f < 3; f++) {
        long_list(false, 100, probe, 3, full, &ffoot, &box);
        if (f == 0) { move(100, 100); wheel(0, -900, true); }
    }
    reset();
    for (int f = 0; f < 3; f++) {
        long_list(true, 100, probe, 3, virt, &vfoot, &box);
        if (f == 0) { move(100, 100); wheel(0, -900, true); }
    }
    for (int p = 0; p < 3; p++) CHECK_NEAR(virt[p].rect.y, full[p].rect.y, 0.01);
    CHECK_NEAR(full[0].rect.y, 8 + 50 + 4 + 36 * 24 - 900, 0.01);
    CHECK(declared < 15);
    reset();
}

TEST(visible_declares_everything_where_nothing_scrolls) {
    begin();
    int n = 0;
    bare_column(.width = 100) {
        bare_visible(i, 50, 10) { bare_box(.width = 10, .height = 10); n++; }
    }
    end();
    CHECK(n == 50);
    reset();
}

/* ---- the scrollbar ---------------------------------------------------------------- */

static int rail(void) {
    for (int i = 0; i < ctx->elem_count; i++)
        if (ctx->elems[i].overlay == 2) return i;
    return -1;
}

TEST(the_scrollbar_shows_while_scrolling_then_fades) {
    list_ui(10);                                    /* the first layout finds the overflow */
    list_ui(10);
    int r = rail();
    CHECK(r > 0);
    CHECK(ctx->elems[r + 1].opacity == 0);          /* hidden until something happens */
    CHECK(bare_ctx_wake_time(ctx) < 0);
    move(50, 50);
    wheel(0, -20, true);
    list_ui(10);
    double t = now;
    CHECK_NEAR(bare_ctx_wake_time(ctx), t + 1.0, 1e-6);   /* wake to hide it */
    CHECK(again);                                   /* fading in */
    for (int f = 0; f < 20; f++) { now += 1.0 / 60; list_ui(10); }
    CHECK(ctx->elems[rail() + 1].opacity > 0.99f);
    CHECK(!again);                                  /* nothing moving: sleep until the wake time */
    CHECK_NEAR(bare_ctx_wake_time(ctx), t + 1.0, 1e-6);
    now = t + 1.01;
    list_ui(10);
    CHECK(again);                                   /* fading out */
    for (int f = 0; f < 30; f++) { now += 1.0 / 60; list_ui(10); }
    CHECK(ctx->elems[rail() + 1].opacity == 0);
    CHECK(!again && bare_ctx_wake_time(ctx) < 0);
    reset();
}

TEST(dragging_the_thumb_scrolls_and_blocks_whats_under_it) {
    list_ui(10);
    list_ui(10);
    move(95, 10);                                   /* over the rail: shows it */
    List l = list_ui(10);
    l = list_ui(10);
    int r = rail();
    BareRect rr = ctx->hits[r].rect;
    /* box 100, content 300: track 96, thumb 32 at 2 */
    CHECK_NEAR(rr.h, 32, 0.01);
    CHECK_NEAR(rr.y, 2, 0.01);
    CHECK_NEAR(rr.x, 100 - BARE__BAR_HIT, 0.01);
    CHECK(!l.items[0].hovered);                     /* the rail is on top of the item */
    button(true, 95, 10);
    move(95, 42);                                   /* 32 of the 64 the thumb can travel: half way */
    l = list_ui(10);
    CHECK_NEAR(l.box.scroll.y, 100, 0.01);
    move(95, 500);                                  /* past the end, outside the box: clamps */
    CHECK_NEAR(list_ui(10).box.scroll.y, 200, 0.01);
    button(false, 95, 500);
    l = list_ui(10);
    CHECK(!l.items[0].clicked);
    reset();
}

TEST(scrollbars_stay_out_of_the_layout) {
    list_ui(10);
    list_ui(10);
    CHECK(rail() > 0);
    CHECK(ctx->elems[find(BARE_ID("list"))].child_count == 10);
    CHECK_NEAR(ctx->elems[find(BARE_IDI("item", 9))].rect.y, 270, 0.01);
    reset();
}

int main(void) {
    RUN(touchpad_pixels_scroll_at_once_and_clamp_to_the_content);
    RUN(a_notch_glides_to_its_target_then_rests);
    RUN(a_box_that_fits_its_content_doesnt_scroll);
    RUN(scrolled_children_are_drawn_and_hit_where_they_show);
    RUN(offsets_snap_to_physical_pixels_so_text_stays_sharp);
    RUN(wheel_goes_to_the_inner_box_until_its_end_then_the_outer);
    RUN(horizontal_scrollers_pass_vertical_wheels_on_and_shift_turns_it);
    RUN(a_scroll_box_takes_the_space_there_is);
    RUN(a_scroll_box_doesnt_shrink_its_children);
    RUN(clip_without_scroll_can_be_squeezed_too);
    RUN(content_that_shrinks_pulls_the_offset_back);
    RUN(children_with_animate_layout_dont_lag_behind_the_scroll);
    RUN(nested_clips_push_intersections_and_pop_in_pairs);
    RUN(clipped_out_elements_draw_nothing);
    RUN(visible_declares_only_what_shows_and_keeps_room_for_the_rest);
    RUN(visible_items_sit_where_a_full_list_puts_them);
    RUN(visible_declares_everything_where_nothing_scrolls);
    RUN(the_scrollbar_shows_while_scrolling_then_fades);
    RUN(dragging_the_thumb_scrolls_and_blocks_whats_under_it);
    RUN(scrollbars_stay_out_of_the_layout);
    return test_report();
}
