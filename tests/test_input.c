/* Interaction, headless: synthetic events against what the last frame drew. */
#include "bare_test.h"
#include "bare.h"
#include "internal.h"
#include <string.h>

static BareContext *ctx;
static bool again;              /* bare_ctx_end asked for another frame */

static void begin(void) {
    if (!ctx) {
        ctx = bare_ctx_create(NULL);
        bare_ctx_make_current(ctx);
    }
    bare_ctx_begin(ctx, 800, 600, 1.0f, 0.0);
}
static void end(void) { bare_ctx_end(ctx, &again); }
static void reset(void) { bare_ctx_destroy(ctx); ctx = NULL; }

static void move(float x, float y) {
    BareEvent e = { .kind = BARE_EVENT_MOUSE_MOVE };
    e.as.mouse.x = x; e.as.mouse.y = y;
    bare_ctx_event(ctx, &e);
}
static void button(int b, bool down, float x, float y) {
    BareEvent e = { .kind = BARE_EVENT_MOUSE_BUTTON };
    e.as.button.button = b; e.as.button.down = down; e.as.button.x = x; e.as.button.y = y;
    bare_ctx_event(ctx, &e);
}
static void key(BareKey k, bool down, bool repeat) {
    BareEvent e = { .kind = BARE_EVENT_KEY };
    e.as.key.key = k; e.as.key.down = down; e.as.key.repeat = repeat;
    bare_ctx_event(ctx, &e);
}
static void simple(BareEventKind kind) {
    BareEvent e = { .kind = kind };
    bare_ctx_event(ctx, &e);
}

/* A button (100x40 at 20,20 with 10 px padding) holding an icon (20x20 at 30,30),
 * then a 100x100 stack whose second layer covers the first. */
typedef struct Ui { BareState button, icon, under, over; } Ui;

static Ui ui(void) {
    Ui u;
    memset(&u, 0, sizeof u);
    begin();
    bare_column(.padding = 20, .gap = 20) {
        bare_row(.id = BARE_ID("button"), .width = 100, .height = 40, .padding = 10) {
            u.button = bare_this();
            u.icon = bare_box(.id = BARE_ID("icon"), .width = 20, .height = 20);
        }
        bare_stack(.width = 100, .height = 100) {
            u.under = bare_box(.id = BARE_ID("under"), .width = 100, .height = 100);
            u.over = bare_box(.id = BARE_ID("over"), .width = 100, .height = 100);
        }
    }
    end();
    return u;
}

TEST(nothing_is_hovered_before_anything_was_drawn) {
    begin();
    move(50, 40);
    end();
    Ui u = ui();
    CHECK(!u.button.hovered);
    reset();
}

TEST(hover_picks_the_topmost_and_its_ancestors) {
    ui();
    move(35, 35);                    /* over the icon */
    Ui u = ui();
    CHECK(u.icon.hovered && u.button.hovered);
    CHECK(!u.under.hovered && !u.over.hovered);
    move(50, 120);                   /* over the stack: the later layer is on top */
    u = ui();
    CHECK(u.over.hovered && !u.under.hovered);
    CHECK(!u.button.hovered);
    simple(BARE_EVENT_MOUSE_LEAVE);
    u = ui();
    CHECK(!u.over.hovered);
    reset();
}

TEST(click_is_reported_once) {
    ui();
    button(BARE_MOUSE_LEFT, true, 35, 35);
    Ui u = ui();
    CHECK(u.icon.pressed && u.button.pressed && !u.button.clicked);
    button(BARE_MOUSE_LEFT, false, 35, 35);
    u = ui();
    CHECK(u.icon.clicked && u.button.clicked && !u.button.pressed);
    CHECK(again);                    /* one more frame so the app's reaction shows */
    u = ui();
    CHECK(!u.icon.clicked && !u.button.clicked);
    CHECK(!again);
    reset();
}

TEST(press_and_release_in_one_frame_clicks) {
    ui();
    button(BARE_MOUSE_LEFT, true, 35, 35);
    button(BARE_MOUSE_LEFT, false, 35, 35);
    Ui u = ui();
    CHECK(u.button.clicked && !u.button.pressed);
    reset();
}

TEST(releasing_outside_does_not_click) {
    ui();
    button(BARE_MOUSE_LEFT, true, 35, 35);
    move(400, 400);
    Ui u = ui();
    CHECK(u.button.pressed && !u.button.hovered);   /* stays pressed while held, like :active */
    button(BARE_MOUSE_LEFT, false, 400, 400);
    u = ui();
    CHECK(!u.button.clicked && !u.button.pressed);
    reset();
}

TEST(dragging_out_and_back_in_clicks) {
    ui();
    button(BARE_MOUSE_LEFT, true, 35, 35);
    ui();
    move(400, 400);
    ui();
    move(60, 45);
    button(BARE_MOUSE_LEFT, false, 60, 45);
    Ui u = ui();
    CHECK(u.button.clicked);
    reset();
}

TEST(click_goes_to_the_common_ancestor) {
    ui();
    button(BARE_MOUSE_LEFT, true, 35, 35);    /* down on the icon */
    button(BARE_MOUSE_LEFT, false, 100, 50);  /* up on the button's padding */
    Ui u = ui();
    CHECK(u.button.clicked && !u.icon.clicked);
    reset();
}

TEST(press_outside_then_release_inside_does_not_click) {
    ui();
    button(BARE_MOUSE_LEFT, true, 400, 400);
    button(BARE_MOUSE_LEFT, false, 35, 35);
    Ui u = ui();
    CHECK(!u.button.clicked);
    reset();
}

TEST(right_click) {
    ui();
    button(BARE_MOUSE_RIGHT, true, 50, 120);
    button(BARE_MOUSE_RIGHT, false, 50, 120);
    Ui u = ui();
    CHECK(u.over.right_clicked && !u.over.clicked && !u.over.pressed);
    reset();
}

TEST(rounded_corners_miss) {
    for (int f = 0; f < 2; f++) {
        begin();
        bare_box(.id = BARE_ID("pill"), .width = 100, .height = 100, .radius = 50);
        end();
        if (f == 0) move(5, 5);
    }
    begin();
    BareState s = bare_box(.id = BARE_ID("pill"), .width = 100, .height = 100, .radius = 50);
    end();
    CHECK(!s.hovered);
    move(50, 5);
    begin();
    s = bare_box(.id = BARE_ID("pill"), .width = 100, .height = 100, .radius = 50);
    end();
    CHECK(s.hovered);
    reset();
}

static void overlay(BareState *base, BareState *glass, BareState *child) {
    begin();
    bare_stack(.width = 200, .height = 200) {
        *base = bare_box(.id = BARE_ID("base"), .width = 200, .height = 200);
        bare_stack(.id = BARE_ID("glass"), .width = 200, .height = 200, .passthrough = true) {
            *glass = bare_this();
            *child = bare_box(.id = BARE_ID("child"), .width = 50, .height = 50);
        }
    }
    end();
}

TEST(passthrough_lets_the_mouse_through_but_not_its_children) {
    BareState base, glass, child;
    overlay(&base, &glass, &child);
    move(150, 150);
    overlay(&base, &glass, &child);
    CHECK(base.hovered && !glass.hovered);
    move(20, 20);
    overlay(&base, &glass, &child);
    CHECK(child.hovered && glass.hovered);   /* an ancestor of what's under the mouse */
    CHECK(!base.hovered);
    reset();
}

static BareState clipped(bool clip) {
    begin();
    BareState s = { 0 };
    bare_column(.id = BARE_ID("box"), .width = 50, .height = 50, .clip = clip) {
        s = bare_box(.id = BARE_ID("big"), .width = 200, .height = 200);
    }
    end();
    return s;
}

TEST(clipped_children_cant_be_hit_outside_the_clip) {
    clipped(true);
    move(100, 100);
    CHECK(!clipped(true).hovered);
    clipped(false);                  /* input is tested against the last frame drawn */
    CHECK(clipped(false).hovered);
    reset();
}

TEST(looks_apply_while_hovered_and_pressed) {
    BareBox b = { .id = 0, .width = 100, .height = 100, .color = BARE_RGB(10, 10, 10),
                  .hover = { .color = BARE_RGB(20, 20, 20), .offset = { 0, -2 } },
                  .press = { .color = BARE_RGB(30, 30, 30), .opacity = BARE_ZERO } };
    b.id = BARE_ID("looks");
    for (int f = 0; f < 4; f++) {
        if (f == 1) move(50, 50);
        if (f == 2) button(BARE_MOUSE_LEFT, true, 50, 50);
        if (f == 3) button(BARE_MOUSE_LEFT, false, 50, 50);
        begin();
        bare_box_(&b, __FILE__, __LINE__);
        end();
        const BareBox *r = &ctx->elems[1].box;
        if (f == 0) CHECK(r->color.r == 10);
        if (f == 1) { CHECK(r->color.r == 20); CHECK_NEAR(r->offset.y, -2, 0); CHECK_NEAR(r->opacity, 1, 0); }
        if (f == 2) { CHECK(r->color.r == 30); CHECK_NEAR(r->opacity, 0, 0); }   /* press on top of hover */
        if (f == 3) CHECK(r->color.r == 20);
    }
    reset();
}

TEST(cursor_comes_from_the_innermost_hovered_element) {
    for (int f = 0; f < 2; f++) {
        if (f == 1) move(10, 10);
        begin();
        bare_column(.id = BARE_ID("outer"), .cursor = BARE_CURSOR_MOVE) {
            bare_box(.id = BARE_ID("inner"), .width = 20, .height = 20, .cursor = BARE_CURSOR_HAND);
            bare_box(.id = BARE_ID("plain"), .width = 20, .height = 20);
        }
        end();
    }
    CHECK(bare_ctx_cursor(ctx) == BARE_CURSOR_HAND);
    move(10, 30);
    begin();
    bare_column(.id = BARE_ID("outer"), .cursor = BARE_CURSOR_MOVE) {
        bare_box(.id = BARE_ID("inner"), .width = 20, .height = 20, .cursor = BARE_CURSOR_HAND);
        bare_box(.id = BARE_ID("plain"), .width = 20, .height = 20);
    }
    end();
    CHECK(bare_ctx_cursor(ctx) == BARE_CURSOR_MOVE);
    reset();
}

TEST(scale_draws_about_the_centre_and_is_hit_tested) {
    begin();
    bare_column(.id = BARE_ID("card"), .width = 100, .height = 100, .scale = 2, .color = BARE_WHITE) {
        bare_box(.id = BARE_ID("dot"), .width = 10, .height = 10, .color = BARE_WHITE);
    }
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(l->cmd_count == 2);
    BareRect c = l->cmds[0].as.rect.rect, d = l->cmds[1].as.rect.rect;
    CHECK_NEAR(c.x, -50, 0.01); CHECK_NEAR(c.w, 200, 0.01);   /* centre stays at 50 */
    CHECK_NEAR(d.x, -50, 0.01); CHECK_NEAR(d.w, 20, 0.01);    /* children scale with it */
    move(-40, -40);                                           /* outside the layout box, inside the drawn one */
    begin();
    BareState s = { 0 };
    bare_column(.id = BARE_ID("card"), .width = 100, .height = 100, .scale = 2) {
        s = bare_this();
    }
    end();
    CHECK(s.hovered);
    CHECK_NEAR(s.rect.x, -50, 0.01);
    reset();
}

TEST(keys_pressed_for_one_frame) {
    begin(); end();
    key(BARE_KEY_SPACE, true, false);
    begin();
    CHECK(bare_key_pressed(BARE_KEY_SPACE) && bare_key_down(BARE_KEY_SPACE));
    end();
    CHECK(again);
    begin();
    CHECK(!bare_key_pressed(BARE_KEY_SPACE) && bare_key_down(BARE_KEY_SPACE));
    end();
    key(BARE_KEY_SPACE, true, true);   /* auto-repeat */
    begin();
    CHECK(bare_key_pressed(BARE_KEY_SPACE));
    end();
    key(BARE_KEY_SPACE, false, false);
    begin();
    CHECK(!bare_key_pressed(BARE_KEY_SPACE) && !bare_key_down(BARE_KEY_SPACE));
    end();
    key(BARE_KEY_COUNT_, true, false);  /* out of range: ignored */
    CHECK(!bare_key_down(BARE_KEY_COUNT_));
    reset();
}

TEST(losing_focus_releases_keys_and_buttons) {
    ui();
    key(BARE_KEY_CTRL, true, false);
    button(BARE_MOUSE_LEFT, true, 35, 35);
    BareEvent f = { .kind = BARE_EVENT_FOCUS };
    f.as.focused = false;
    bare_ctx_event(ctx, &f);
    Ui u = ui();
    CHECK(!bare_key_down(BARE_KEY_CTRL));
    CHECK(!u.button.pressed);
    button(BARE_MOUSE_LEFT, false, 35, 35);   /* the release that follows doesn't click */
    u = ui();
    CHECK(!u.button.clicked);
    reset();
}

TEST(another_frame_when_the_ui_moves_under_a_still_mouse) {
    begin(); end();
    move(10, 10);
    begin(); bare_box(.id = BARE_ID("a"), .width = 20, .height = 20); end();
    begin(); bare_box(.id = BARE_ID("a"), .width = 20, .height = 20); end();
    CHECK(!again);                                   /* settled: idle */
    begin(); bare_box(.id = BARE_ID("a"), .width = 20, .height = 20, .offset = { 100, 0 }); end();
    CHECK(again);                                    /* it slid away from the mouse */
    begin(); bare_box(.id = BARE_ID("a"), .width = 20, .height = 20, .offset = { 100, 0 }); end();
    CHECK(!again);
    reset();
}

/* A title bar marked .window_drag: its empty space drags the window, a button on it doesn't,
 * and neither does anything outside it. */
TEST(window_drag_areas_skip_what_reacts_to_the_mouse) {
    for (int frame = 0; frame < 2; frame++) {
        begin();
        bare_column() {
            bare_row(.id = BARE_ID("bar"), .width = 400, .height = 40, .window_drag = true) {
                bare_box(.id = BARE_ID("label"), .width = 100, .height = 40);
                bare_box(.id = BARE_ID("close"), .width = 40, .height = 40, .hover = { .opacity = 0.5f });
            }
            bare_box(.id = BARE_ID("page"), .width = 400, .height = 200);
        }
        end();
    }
    CHECK(bare_ctx_window_drag(ctx, (BareVec2){ 250, 20 }));    /* the bar itself */
    CHECK(bare_ctx_window_drag(ctx, (BareVec2){ 50, 20 }));     /* a plain child of it */
    CHECK(!bare_ctx_window_drag(ctx, (BareVec2){ 120, 20 }));   /* a child with a hover look */
    CHECK(!bare_ctx_window_drag(ctx, (BareVec2){ 200, 100 }));  /* outside the bar */
    reset();
}

int main(void) {
    RUN(nothing_is_hovered_before_anything_was_drawn);
    RUN(hover_picks_the_topmost_and_its_ancestors);
    RUN(click_is_reported_once);
    RUN(press_and_release_in_one_frame_clicks);
    RUN(releasing_outside_does_not_click);
    RUN(dragging_out_and_back_in_clicks);
    RUN(click_goes_to_the_common_ancestor);
    RUN(press_outside_then_release_inside_does_not_click);
    RUN(right_click);
    RUN(rounded_corners_miss);
    RUN(passthrough_lets_the_mouse_through_but_not_its_children);
    RUN(clipped_children_cant_be_hit_outside_the_clip);
    RUN(looks_apply_while_hovered_and_pressed);
    RUN(cursor_comes_from_the_innermost_hovered_element);
    RUN(scale_draws_about_the_centre_and_is_hit_tested);
    RUN(keys_pressed_for_one_frame);
    RUN(losing_focus_releases_keys_and_buttons);
    RUN(another_frame_when_the_ui_moves_under_a_still_mouse);
    RUN(window_drag_areas_skip_what_reacts_to_the_mouse);
    return test_report();
}
