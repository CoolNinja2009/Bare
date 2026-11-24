/* scroll.c: clipping and scroll boxes (M6).
 *
 * - left: 10,000 rows in a scroll box; only the ones on screen are declared (bare_visible)
 * - right: a page that scrolls, holding strips that scroll sideways. The wheel over a strip
 *   scrolls the page (the strip can't go down), Shift + wheel or a touchpad swipe scrolls
 *   the strip; a strip at its end hands the wheel back to the page
 * - liquid glass inside a strip, clipped with it
 * Scrollbars show while scrolling or when the mouse moves over a box, and can be dragged. */
#include "bare.h"
#include <stdio.h>

static const BareColor tones[] = {
    { 255, 120, 90, 255 }, { 255, 190, 70, 255 }, { 90, 200, 140, 255 },
    { 70, 170, 255, 255 }, { 150, 120, 255, 255 }, { 240, 100, 180, 255 },
};

static void row(int i, int *selected) {
    char size[32];
    snprintf(size, sizeof size, "%d KB", (i * 7919) % 900 + 12);
    bool on = i == *selected;
    bare_row(.id = BARE_IDI("row", i), .width = BARE_GROW, .height = 44, .pad = { 0, 14, 0, 12 }, .gap = 12,
             .align = BARE_CENTER, .radius = 10, .cursor = BARE_CURSOR_HAND,
             .color = on ? BARE_RGB(52, 84, 190) : BARE_RGBA(0, 0, 0, 0),
             .hover = { .color = on ? BARE_RGB(62, 96, 205) : BARE_RGB(40, 44, 58) }) {
        if (bare_this().clicked) *selected = i;
        bare_box(.width = 26, .height = 26, .radius = 13, .color = tones[i % 6]);
        bare_textf("Row %d", i + 1);
        bare_box(.width = BARE_GROW);
        bare_text(size, .size = 13, .color = on ? BARE_RGB(200, 210, 255) : BARE_RGB(120, 126, 145));
    }
}

static void card(int s, int c) {
    char label[32];
    snprintf(label, sizeof label, "Card %d", c + 1);
    if (s == 1 && c == 2) {   /* glass over stripes, clipped by the strip like everything in it */
        bare_stack(.id = BARE_IDI("lens", s), .width = 200, .height = 130, .align = BARE_CENTER) {
            bare_row(.width = BARE_GROW, .height = BARE_GROW, .gap = 2) {
                for (int k = 0; k < 10; k++)
                    bare_box(.width = 18, .height = BARE_GROW, .radius = 9, .color = tones[k % 6]);
            }
            bare_row(.width = 150, .height = 64, .radius = 32, .glass = BARE_GLASS, .justify = BARE_CENTER,
                     .align = BARE_CENTER) { bare_text("Glass", .size = 18); }
        }
        return;
    }
    BareColor t = tones[(s + c) % 6];
    bare_column(.id = BARE_IDI("card", s * 100 + c), .width = 200, .height = 130, .padding = 16,
                .justify = BARE_END, .radius = 16, .color = BARE_RGBA(t.r, t.g, t.b, 60),
                .border = 1, .border_color = BARE_RGBA(t.r, t.g, t.b, 140)) {
        bare_text(label, .size = 18);
    }
}

int main(void) {
    bare_window(1200, 760, "Bare: scroll");
    static const char *titles[] = { "Recent", "Glass in a strip", "Favourites", "Shared", "Archive", "Trash" };
    int selected = -1;
    while (bare_frame()) {
        bare_background(BARE_RGB(18, 20, 27));
        bare_row(.width = BARE_GROW, .height = BARE_GROW, .padding = 28, .gap = 28) {
            bare_column(.width = 380, .height = BARE_GROW, .gap = 12) {
                bare_text("10,000 rows", .size = 28);
                bare_text(selected < 0 ? "Only the rows on screen are declared. Click one." : "Selected:",
                          .size = 14, .color = BARE_RGB(140, 146, 165));
                if (selected >= 0) bare_textf("Row %d", selected + 1);
                bare_column(.width = BARE_GROW, .height = BARE_GROW, .padding = 6, .radius = 16,
                            .color = BARE_RGB(26, 29, 38)) {
                    bare_column(.id = BARE_ID("rows"), .width = BARE_GROW, .height = BARE_GROW, .gap = 2,
                                .scroll = BARE_SCROLL_Y) {
                        bare_visible(i, 10000, 44) row(i, &selected);
                    }
                }
            }
            bare_column(.id = BARE_ID("page"), .width = BARE_GROW, .height = BARE_GROW, .gap = 14,
                        .pad = { 0, 18, 24, 0 }, .scroll = BARE_SCROLL_Y) {
                for (int s = 0; s < 6; s++) {
                    bare_text(titles[s], .size = 22);
                    bare_row(.id = BARE_IDI("strip", s), .width = BARE_GROW, .gap = 14, .pad = { 0, 0, 18, 0 },
                             .scroll = BARE_SCROLL_X) {
                        for (int c = 0; c < 12; c++) card(s, c);
                    }
                }
            }
        }
    }
    bare_quit();
}
