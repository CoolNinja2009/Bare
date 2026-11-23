/* buttons.c: hover, press, click, right click and keys.
 *
 * - three buttons: each click drops a dot of its colour into the tray
 * - a switch that flips on click
 * - cards that lift on hover; right-click one to select it
 * - keys: Space adds a dot, Backspace removes one, Escape clears
 *
 * Looks change instantly here; M4 makes them animate. Text comes in M5. */
#include "bare.h"

#define MAX_DOTS 48

static BareColor dots[MAX_DOTS];
static int dot_count;

static void add_dot(BareColor c) {
    if (dot_count == MAX_DOTS) {   /* full: slide everything along */
        for (int i = 1; i < MAX_DOTS; i++) dots[i - 1] = dots[i];
        dot_count--;
    }
    dots[dot_count++] = c;
}

static BareColor shade(BareColor c, int d) {
    int r = c.r + d, g = c.g + d, b = c.b + d;
    return BARE_RGB(r < 0 ? 0 : r > 255 ? 255 : r, g < 0 ? 0 : g > 255 ? 255 : g, b < 0 ? 0 : b > 255 ? 255 : b);
}

/* A button is a box with looks for hover and press. Returns true when clicked. */
static bool button(BareId id, BareColor color) {
    return bare_box(.id = id, .width = 150, .height = 52, .radius = 14, .color = color,
                    .cursor = BARE_CURSOR_HAND, .shadow = BARE_SHADOW_SMALL,
                    .hover = { .color = shade(color, 22), .scale = 1.04f,
                               .shadow = { 0, 10, 24, 0, BARE_RGBA(0, 0, 0, 90) } },
                    .press = { .color = shade(color, -18), .scale = 0.96f }).clicked;
}

static bool toggle(BareId id, bool on) {
    bool clicked = false;
    bare_row(.id = id, .width = 72, .height = 40, .radius = 20, .padding = 4,
             .justify = on ? BARE_END : BARE_START, .cursor = BARE_CURSOR_HAND,
             .color = on ? BARE_RGB(80, 200, 140) : BARE_RGB(60, 64, 76),
             .hover = { .color = on ? BARE_RGB(95, 215, 155) : BARE_RGB(74, 78, 92) }) {
        clicked = bare_this().clicked;
        bare_box(.width = 32, .height = 32, .radius = 16, .color = BARE_WHITE, .shadow = BARE_SHADOW_SMALL);
    }
    return clicked;
}

int main(void) {
    bare_window(1100, 720, "Bare: buttons");

    const BareColor blue = BARE_RGB(70, 110, 245), green = BARE_RGB(40, 170, 120), red = BARE_RGB(225, 70, 90);
    bool dark = true;
    int selected = -1;

    while (bare_frame()) {
        if (bare_key_pressed(BARE_KEY_SPACE)) add_dot(BARE_WHITE);
        if (bare_key_pressed(BARE_KEY_BACKSPACE) && dot_count > 0) dot_count--;
        if (bare_key_pressed(BARE_KEY_ESCAPE)) dot_count = 0;
        bare_background(dark ? BARE_RGB(16, 18, 24) : BARE_RGB(240, 242, 247));

        bare_column(.width = BARE_GROW, .height = BARE_GROW, .padding = 48, .gap = 36) {
            /* Buttons and the switch. */
            bare_row(.gap = 20, .align = BARE_CENTER) {
                if (button(BARE_ID("blue"), blue)) add_dot(blue);
                if (button(BARE_ID("green"), green)) add_dot(green);
                if (button(BARE_ID("red"), red)) add_dot(red);
                bare_box(.width = 24);
                if (toggle(BARE_ID("theme"), dark)) dark = !dark;
            }

            /* The tray of dots. */
            bare_row(.width = BARE_GROW, .height = 64, .padding = 18, .gap = 8, .radius = 18,
                     .align = BARE_CENTER, .color = dark ? BARE_RGB(26, 29, 38) : BARE_RGB(225, 228, 236)) {
                for (int i = 0; i < dot_count; i++)
                    bare_box(.width = 14, .height = 14, .radius = 7, .color = dots[i]);
            }

            /* Cards: hover lifts, right click selects. */
            bare_row(.width = BARE_GROW, .height = BARE_GROW, .gap = 24) {
                for (int i = 0; i < 4; i++) {
                    bool sel = i == selected;
                    BareColor ink = dark ? BARE_RGBA(255, 255, 255, 60) : BARE_RGBA(0, 0, 0, 40);
                    bare_column(.id = BARE_IDI("card", i), .width = BARE_GROW, .height = 260,
                                .padding = 22, .gap = 14, .radius = 20,
                                .color = dark ? BARE_RGB(30, 34, 45) : BARE_WHITE,
                                .border = sel ? 3.0f : 1.0f,
                                .border_color = sel ? blue : BARE_RGBA(128, 128, 128, 40),
                                .shadow = BARE_SHADOW_SOFT,
                                .hover = { .offset = { 0, -8 }, .shadow = BARE_SHADOW_LARGE }) {
                        BareState card = bare_this();   /* a container's state */
                        if (card.right_clicked) selected = sel ? -1 : i;
                        bare_box(.width = 44, .height = 44, .radius = 12,
                                 .color = card.hovered ? (i % 2 ? green : blue) : ink);
                        bare_box(.width = BARE_PERCENT(80), .height = 12, .radius = 6, .color = ink);
                        bare_box(.width = BARE_PERCENT(55), .height = 12, .radius = 6, .color = ink);
                    }
                }
            }
        }
    }
    bare_quit();
}
