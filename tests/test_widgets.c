/* Widgets (M8), headless: synthetic mouse and keys against the last frame.
 *
 * The widgets are given explicit sizes, so they sit in one column from the
 * top left: button (0,0 100x40), checkbox (0,40 200x24), slider (0,64
 * 220x24), then two list items 30 px tall across the window. */
#include "bare_test.h"
#include "bare.h"
#include "internal.h"
#include <string.h>

static BareContext *ctx;
static bool again;

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
static void button(bool down, float x, float y) {
    BareEvent e = { .kind = BARE_EVENT_MOUSE_BUTTON };
    e.as.button.button = BARE_MOUSE_LEFT; e.as.button.down = down; e.as.button.x = x; e.as.button.y = y;
    bare_ctx_event(ctx, &e);
}
static void key(BareKey k) {
    BareEvent e = { .kind = BARE_EVENT_KEY };
    e.as.key.key = k; e.as.key.down = true;
    bare_ctx_event(ctx, &e);
    e.as.key.down = false;
    bare_ctx_event(ctx, &e);
}

typedef struct Ui {
    bool pressed, toggled, moved;
    BareState item[2];
} Ui;

static bool check;
static float value = 50;
static int selected;

static Ui ui(void) {
    Ui u;
    memset(&u, 0, sizeof u);
    begin();
    bare_column(.width = BARE_GROW) {
        u.pressed = bare_button("Save", .id = BARE_ID("button"), .width = 100, .height = 40).clicked;
        u.toggled = bare_checkbox("Wrap", &check, .id = BARE_ID("check"), .width = 200, .height = 24);
        u.moved = bare_slider(&value, 0, 100, .id = BARE_ID("slider"), .width = 220, .height = 24);
        for (int i = 0; i < 2; i++) {
            u.item[i] = bare_list_item(i ? "Two" : "One", i == selected, .id = BARE_IDI("item", i), .height = 30);
            if (u.item[i].clicked) selected = i;
        }
    }
    end();
    return u;
}

static void click(float x, float y) {
    move(x, y);
    button(true, x, y);
    button(false, x, y);
}

TEST(button_click_returns_true_once) {
    ui();
    click(50, 20);
    CHECK(ui().pressed);
    CHECK(!ui().pressed);
    reset();
}

TEST(button_is_pressed_by_enter_and_space_when_focused) {
    ui();
    click(50, 20);                   /* clicking focuses it */
    ui();
    key(BARE_KEY_ENTER);
    CHECK(ui().pressed);
    key(BARE_KEY_SPACE);
    CHECK(ui().pressed);
    CHECK(!ui().pressed);
    reset();
}

TEST(checkbox_toggles_by_click_and_space) {
    check = false;
    ui();
    click(10, 52);
    Ui u = ui();
    CHECK(u.toggled && check);
    CHECK(!ui().toggled && check);   /* once */
    key(BARE_KEY_SPACE);
    u = ui();
    CHECK(u.toggled && !check);
    reset();
}

TEST(slider_maps_the_mouse_and_clamps) {
    value = 50;
    ui();
    /* the knob's centre travels from 9 to 211 (220 minus an 18 px knob) */
    move(9 + 202 * 0.25f, 76);
    button(true, 9 + 202 * 0.25f, 76);
    Ui u = ui();
    CHECK(u.moved);
    CHECK_NEAR(value, 25, 0.01f);
    move(700, 300);                  /* far outside while held: still dragging, clamped */
    ui();
    CHECK_NEAR(value, 100, 0.01f);
    move(-50, 76);
    ui();
    CHECK_NEAR(value, 0, 0.01f);
    button(false, -50, 76);
    ui();
    move(150, 76);                   /* released: moving no longer drags */
    u = ui();
    CHECK(!u.moved);
    CHECK_NEAR(value, 0, 0.01f);
    reset();
}

TEST(slider_takes_arrow_keys_when_focused) {
    value = 50;
    ui();
    button(true, 110, 76);           /* press focuses it and sets the value */
    button(false, 110, 76);
    ui();
    float at = value;
    move(400, 400);                  /* not hovered: the keys still go to the focused slider */
    key(BARE_KEY_RIGHT);
    ui();
    CHECK_NEAR(value, at + 1, 0.01f);
    key(BARE_KEY_END);
    ui();
    CHECK_NEAR(value, 100, 0.01f);
    key(BARE_KEY_HOME);
    ui();
    CHECK_NEAR(value, 0, 0.01f);
    reset();
}

TEST(list_item_selection_follows_clicks) {
    selected = 0;
    ui();
    click(400, 88 + 30 + 15);        /* the second item */
    Ui u = ui();
    CHECK(u.item[1].clicked && selected == 1);
    u = ui();
    CHECK(u.item[1].rect.w == 800);  /* items fill the list's width */
    reset();
}

int main(void) {
    RUN(button_click_returns_true_once);
    RUN(button_is_pressed_by_enter_and_space_when_focused);
    RUN(checkbox_toggles_by_click_and_space);
    RUN(slider_maps_the_mouse_and_clamps);
    RUN(slider_takes_arrow_keys_when_focused);
    RUN(list_item_selection_follows_clicks);
    return test_report();
}
