/* Layout, IDs and retained state, headless. Elements are checked by their
 * declaration order: R(0) is the root (the window), R(1) the first element... */
#include "bare_test.h"
#include "bare.h"
#include "internal.h"
#include <string.h>

static BareContext *ctx;
static char last_log[512];
static int log_count;

static void capture_log(void *user, const char *msg) {
    (void)user;
    snprintf(last_log, sizeof last_log, "%s", msg);
    log_count++;
}

static void begin(float w, float h) {
    if (!ctx) {
        ctx = bare_ctx_create(NULL);
        bare_ctx_set_log(ctx, capture_log, NULL);
        bare_ctx_make_current(ctx);
    }
    log_count = 0;
    bare_ctx_begin(ctx, w, h, 1.0f, 0.0);
}
static void end(void) { bare_ctx_end(ctx, NULL); }
static BareRect R(int i) { return ctx->elems[i].rect; }

#define CHECK_RECT(i, X, Y, W, H) do { \
    BareRect r_ = R(i); \
    CHECK_NEAR(r_.x, X, 0.01); CHECK_NEAR(r_.y, Y, 0.01); \
    CHECK_NEAR(r_.w, W, 0.01); CHECK_NEAR(r_.h, H, 0.01); \
} while (0)

/* ---- sizes --------------------------------------------------------------- */

TEST(fixed_sizes) {
    begin(800, 600);
    bare_box(.width = 120, .height = 80);
    end();
    CHECK_RECT(0, 0, 0, 800, 600);
    CHECK_RECT(1, 0, 0, 120, 80);
}

TEST(fit_wraps_children_with_padding_and_gap) {
    begin(800, 600);
    bare_row(.padding = 10, .gap = 4) {
        bare_box(.width = 30, .height = 20);
        bare_box(.width = 50, .height = 40);
    }
    end();
    CHECK_RECT(1, 0, 0, 10 + 30 + 4 + 50 + 10, 10 + 40 + 10);
    CHECK_RECT(2, 10, 10, 30, 20);
    CHECK_RECT(3, 44, 10, 50, 40);
}

TEST(grow_splits_free_space_by_weight) {
    begin(800, 600);
    bare_row(.width = 700, .gap = 10) {
        bare_box(.width = 100, .height = 10);
        bare_box(.width = BARE_GROW, .height = 10);
        bare_box(.width = BARE_GROW, .height = 10, .grow = 2);
    }
    end();
    /* free = 700 - 100 - 20 gaps = 580 -> 1:2 */
    CHECK_NEAR(R(3).w, 580.0 / 3, 0.01);
    CHECK_NEAR(R(4).w, 580.0 * 2 / 3, 0.01);
    CHECK_NEAR(R(4).x + R(4).w, 700, 0.01);
}

TEST(grow_respects_max_and_redistributes) {
    begin(800, 600);
    bare_row(.width = 600) {
        bare_box(.width = BARE_GROW, .height = 10, .max_width = 100);
        bare_box(.width = BARE_GROW, .height = 10);
    }
    end();
    CHECK_NEAR(R(2).w, 100, 0.01);
    CHECK_NEAR(R(3).w, 500, 0.01);
}

TEST(grow_never_below_content) {
    begin(800, 600);
    bare_row(.width = 300) {
        bare_row(.width = BARE_GROW) { bare_box(.width = 250, .height = 10); }
        bare_box(.width = BARE_GROW, .height = 10);
    }
    end();
    CHECK_NEAR(R(2).w, 250, 0.01);   /* half would be 150, but its content needs 250 */
    CHECK_NEAR(R(4).w, 50, 0.01);
}

TEST(grow_fills_the_window_from_the_root) {
    begin(800, 600);
    bare_column(.width = BARE_GROW, .height = BARE_GROW) {}
    end();
    CHECK_RECT(1, 0, 0, 800, 600);
}

TEST(shrink_takes_overflow_back_proportionally) {
    begin(800, 600);
    bare_row(.width = 300) {
        bare_row(.min_width = 0) { bare_box(.width = BARE_ZERO, .height = 1); bare_box(.width = 0, .height = 1); }
        bare_row(.padding = 100) {}              /* fit 200, min-content 200: can't shrink */
        bare_box(.width = 150, .height = 10);    /* fixed: never shrinks */
    }
    end();
    CHECK_NEAR(R(5).w, 200, 0.01);
    CHECK_NEAR(R(6).w, 150, 0.01);
}

TEST(shrink_splits_by_size_down_to_minimums) {
    begin(800, 600);
    bare_row(.width = 300) {
        bare_column(.min_width = 50) { bare_box(.width = BARE_GROW, .height = 1); }
        bare_column() { bare_row(.padding = 100) {} }   /* min-content 200 */
    }
    end();
    /* children want 50 + 200 = 250 <= 300: no shrink */
    CHECK_NEAR(R(2).w, 50, 0.01);
    CHECK_NEAR(R(4).w, 200, 0.01);
}

TEST(percent_of_parent_content_box) {
    begin(800, 600);
    bare_row(.width = 420, .padding = 10) {
        bare_box(.width = BARE_PERCENT(50), .height = BARE_PERCENT(100));
        bare_box(.width = BARE_PERCENT(25), .height = 20);
    }
    end();
    /* content box 400 wide; the row is 40 tall (padding + tallest fit child = 20) */
    CHECK_NEAR(R(2).w, 200, 0.01);
    CHECK_NEAR(R(3).w, 100, 0.01);
    CHECK_NEAR(R(2).h, 20, 0.01);
}

TEST(min_and_max_clamp) {
    begin(800, 600);
    bare_box(.width = 10, .height = 10, .min_width = 40, .max_height = 5);
    bare_column(.max_width = 30) { bare_box(.width = 100, .height = 10); }
    end();
    CHECK_RECT(1, 0, 0, 40, 5);
    CHECK_NEAR(R(2).w, 30, 0.01);
}

TEST(per_side_padding_beats_uniform) {
    begin(800, 600);
    bare_column(.padding = 50, .pad = { 1, 2, 3, 4 }) { bare_box(.width = 10, .height = 10); }
    end();
    CHECK_RECT(1, 0, 0, 4 + 10 + 2, 1 + 10 + 3);
    CHECK_RECT(2, 4, 1, 10, 10);
}

TEST(zero_size_is_real_with_bare_zero) {
    begin(800, 600);
    bare_column(.width = BARE_ZERO, .padding = 20) {}
    end();
    CHECK_NEAR(R(1).w, 0, 0);    /* fixed 0, padding ignored */
    CHECK_NEAR(R(1).h, 40, 0);   /* height still fits */
}

/* ---- alignment ----------------------------------------------------------- */

TEST(align_cross_axis) {
    begin(800, 600);
    bare_column(.width = 100, .align = BARE_START) { bare_box(.width = 20, .height = 5); }
    bare_column(.width = 100, .align = BARE_CENTER) { bare_box(.width = 20, .height = 5); }
    bare_column(.width = 100, .align = BARE_END) { bare_box(.width = 20, .height = 5); }
    bare_column(.width = 100, .align = BARE_STRETCH) { bare_box(.height = 5); bare_box(.width = 20, .height = 5); }
    end();
    CHECK_NEAR(R(2).x, 0, 0.01);
    CHECK_NEAR(R(4).x, 40, 0.01);
    CHECK_NEAR(R(6).x, 80, 0.01);
    CHECK_NEAR(R(8).w, 100, 0.01);   /* fit child stretched */
    CHECK_NEAR(R(9).w, 20, 0.01);    /* fixed child keeps its width */
}

TEST(justify_main_axis) {
    BareAlign modes[6] = { BARE_START, BARE_CENTER, BARE_END, BARE_SPACE_BETWEEN, BARE_SPACE_AROUND, BARE_SPACE_EVENLY };
    /* row of 100 with two 20-wide children: free = 60 */
    float first[6] = { 0, 30, 60, 0, 15, 20 };
    float second[6] = { 20, 50, 80, 80, 65, 60 };
    for (int m = 0; m < 6; m++) {
        begin(800, 600);
        bare_row(.width = 100, .justify = modes[m]) {
            bare_box(.width = 20, .height = 5);
            bare_box(.width = 20, .height = 5);
        }
        end();
        CHECK_NEAR(R(2).x, first[m], 0.01);
        CHECK_NEAR(R(3).x, second[m], 0.01);
    }
}

TEST(nested_rows_in_columns) {
    begin(800, 600);
    bare_column(.width = 400, .height = 300, .padding = 10, .gap = 10) {
        bare_row(.width = BARE_GROW, .height = 50, .gap = 10) {
            bare_box(.width = 60, .height = BARE_GROW);
            bare_box(.width = BARE_GROW, .height = BARE_GROW);
        }
        bare_row(.width = BARE_GROW, .height = BARE_GROW) {}
    }
    end();
    CHECK_RECT(2, 10, 10, 380, 50);
    CHECK_RECT(3, 10, 10, 60, 50);
    CHECK_RECT(4, 80, 10, 310, 50);
    CHECK_RECT(5, 10, 70, 380, 220);   /* the column's leftover height */
}

TEST(stack_centres_layers) {
    begin(800, 600);
    bare_stack(.width = 200, .height = 100, .align = BARE_CENTER) {
        bare_box(.width = BARE_GROW, .height = BARE_GROW);
        bare_box(.width = 20, .height = 10);
    }
    end();
    CHECK_RECT(2, 0, 0, 200, 100);
    CHECK_RECT(3, 90, 45, 20, 10);
}

/* ---- ids and retained state ---------------------------------------------- */

static void loop_ui(int n, bool extra) {
    begin(800, 600);
    bare_column() {
        if (extra) bare_box(.width = 1, .height = 1);   /* another call site, before the loop */
        for (int i = 0; i < n; i++) bare_box(.width = 10, .height = 10);
    }
    end();
}

TEST(auto_ids_are_stable_and_distinct) {
    loop_ui(3, false);
    BareId a[3] = { ctx->elems[2].id, ctx->elems[3].id, ctx->elems[4].id };
    CHECK(a[0] != a[1] && a[1] != a[2] && a[0] != a[2]);
    loop_ui(3, false);
    for (int i = 0; i < 3; i++) CHECK(ctx->elems[2 + i].id == a[i]);
    /* An element from another line appearing first doesn't shift the loop's ids. */
    loop_ui(3, true);
    for (int i = 0; i < 3; i++) CHECK(ctx->elems[3 + i].id == a[i]);
}

TEST(explicit_ids_win) {
    begin(800, 600);
    bare_box(.id = BARE_ID("save"), .width = 10, .height = 10);
    bare_box(.id = BARE_IDI("row", 4), .width = 10, .height = 10);
    end();
    CHECK(ctx->elems[1].id == BARE_ID("save"));
    CHECK(ctx->elems[2].id == BARE_IDI("row", 4));
}

TEST(duplicate_ids_are_reported_once_and_made_unique) {
    for (int frame = 0; frame < 3; frame++) {
        begin(800, 600);
        bare_box(.id = BARE_ID("same"), .width = 10, .height = 10);
        bare_box(.id = BARE_ID("same"), .width = 10, .height = 10);
        end();
        CHECK(ctx->elems[1].id != ctx->elems[2].id);
        CHECK(log_count == (frame == 0 ? 1 : 0));
    }
    CHECK(strstr(last_log, "duplicate element id") != NULL);
}

TEST(state_rect_is_last_frames_position) {
    begin(800, 600);
    BareState s = bare_box(.id = BARE_ID("moving"), .width = 10, .height = 10, .offset = { 5, 0 });
    end();
    CHECK_NEAR(s.rect.w, 0, 0);    /* first frame: nothing known yet */
    begin(800, 600);
    s = bare_box(.id = BARE_ID("moving"), .width = 10, .height = 10);
    end();
    CHECK_NEAR(s.rect.x, 5, 0.01);
    CHECK_NEAR(s.rect.w, 10, 0.01);
}

TEST(undeclared_nodes_are_evicted) {
    begin(800, 600);
    for (int i = 0; i < 100; i++) bare_box(.id = BARE_IDI("item", i), .width = 1, .height = 1);
    end();
    int with_items = bare__store_live(ctx);
    begin(800, 600);
    bare_box(.id = BARE_IDI("item", 7), .width = 1, .height = 1);
    end();
    CHECK(with_items >= 101);
    CHECK(bare__store_live(ctx) == 2);   /* the root and item 7 */
    /* State survives for what's still there, and re-adding works. */
    begin(800, 600);
    for (int i = 0; i < 100; i++) bare_box(.id = BARE_IDI("item", i), .width = 1, .height = 1);
    end();
    CHECK(bare__store_live(ctx) == 101);
}

TEST(store_handles_many_nodes) {
    begin(800, 600);
    for (int i = 0; i < 20000; i++) bare_box(.id = BARE_IDI("n", i), .width = 1, .height = 1);
    end();
    CHECK(bare__store_live(ctx) == 20001);
    begin(800, 600);
    for (int i = 0; i < 20000; i += 2) bare_box(.id = BARE_IDI("n", i), .width = 1, .height = 1);
    end();
    CHECK(bare__store_live(ctx) == 10001);
    /* every surviving id must still be found (backward-shift deletion kept probe runs intact) */
    for (int i = 0; i < 20000; i += 2) {
        bool created = true;
        bare__store_get(ctx, BARE_IDI("n", i), &created);
        CHECK(!created);
    }
}

TEST(steady_state_frames_do_not_grow_memory) {
    for (int f = 0; f < 3; f++) loop_ui(500, true);
    size_t blocks = ctx->frame.block_count;
    int elem_cap = ctx->elem_cap, node_cap = ctx->store.node_cap, table = ctx->store.cap;
    for (int f = 0; f < 50; f++) loop_ui(500, true);
    CHECK(ctx->frame.block_count == blocks);
    CHECK(ctx->elem_cap == elem_cap && ctx->store.node_cap == node_cap && ctx->store.cap == table);
}

/* ---- absolute positioning ----------------------------------------------- */

TEST(absolute_children_take_no_room) {
    begin(800, 600);
    bare_row(.padding = 10, .gap = 4) {
        bare_box(.width = 30, .height = 20);
        bare_box(.position = BARE_ABSOLUTE, .width = 500, .height = 500);
        bare_box(.width = 50, .height = 40);
    }
    end();
    CHECK_RECT(1, 0, 0, 10 + 30 + 4 + 50 + 10, 60);   /* as if it weren't there */
    CHECK_RECT(2, 10, 10, 30, 20);
    CHECK_RECT(3, 0, 0, 500, 500);                     /* top left of the parent, padding ignored */
    CHECK_RECT(4, 44, 10, 50, 40);
}

TEST(absolute_anchors_and_offsets) {
    begin(800, 600);
    bare_row(.width = 200, .height = 100, .border = 2) {
        bare_box(.position = BARE_ABSOLUTE, .anchor = BARE_TOP_RIGHT, .x = 4, .y = -4, .width = 20, .height = 10);
        bare_box(.position = BARE_ABSOLUTE, .anchor = BARE_MIDDLE, .width = 20, .height = 10);
        bare_box(.position = BARE_ABSOLUTE, .anchor = BARE_BOTTOM_LEFT, .x = 3, .width = 20, .height = 10);
        bare_box(.position = BARE_ABSOLUTE, .width = BARE_GROW, .height = BARE_PERCENT(50));
    }
    end();
    CHECK_RECT(2, 2 + 196 - 20 + 4, 2 - 4, 20, 10);   /* inside the 2 px border */
    CHECK_RECT(3, 90, 45, 20, 10);
    CHECK_RECT(4, 2 + 3, 2 + 96 - 10, 20, 10);
    CHECK_RECT(5, 2, 2, 196, 48);
}

TEST(absolute_children_fit_their_content_and_ride_with_the_parent) {
    begin(800, 600);
    bare_column(.padding = 20) {
        bare_box(.width = 300, .height = 50);
        bare_row(.width = 100, .height = 40) {
            bare_row(.position = BARE_ABSOLUTE, .anchor = BARE_BOTTOM, .y = 10, .padding = 5) {
                bare_box(.width = 30, .height = 6);
            }
        }
    }
    end();
    CHECK_RECT(3, 20, 70, 100, 40);
    CHECK_RECT(4, 20 + (100 - 40) / 2.0f, 70 + 40 - 16 + 10, 40, 16);
    CHECK_RECT(5, 20 + 30 + 5, 70 + 24 + 10 + 5, 30, 6);
}

int main(void) {
    RUN(fixed_sizes);
    RUN(fit_wraps_children_with_padding_and_gap);
    RUN(grow_splits_free_space_by_weight);
    RUN(grow_respects_max_and_redistributes);
    RUN(grow_never_below_content);
    RUN(grow_fills_the_window_from_the_root);
    RUN(shrink_takes_overflow_back_proportionally);
    RUN(shrink_splits_by_size_down_to_minimums);
    RUN(percent_of_parent_content_box);
    RUN(min_and_max_clamp);
    RUN(per_side_padding_beats_uniform);
    RUN(zero_size_is_real_with_bare_zero);
    RUN(align_cross_axis);
    RUN(justify_main_axis);
    RUN(nested_rows_in_columns);
    RUN(stack_centres_layers);
    RUN(auto_ids_are_stable_and_distinct);
    RUN(explicit_ids_win);
    RUN(duplicate_ids_are_reported_once_and_made_unique);
    RUN(state_rect_is_last_frames_position);
    RUN(undeclared_nodes_are_evicted);
    RUN(store_handles_many_nodes);
    RUN(steady_state_frames_do_not_grow_memory);
    RUN(absolute_children_take_no_room);
    RUN(absolute_anchors_and_offsets);
    RUN(absolute_children_fit_their_content_and_ride_with_the_parent);
    bare_ctx_destroy(ctx);
    return test_report();
}
