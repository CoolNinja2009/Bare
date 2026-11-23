/* layout.c: a dashboard built only from rows, columns and sizes.
 *
 *   sidebar (fixed width) | header (fixed height)
 *                         | cards: as many columns as fit, each growing
 *
 * Resize the window: everything reflows. Text arrives in M5, so grey bars
 * stand in for labels. */
#include "bare.h"

#define BG      BARE_RGB(15, 17, 23)
#define PANEL   BARE_RGB(24, 27, 36)
#define CARD    BARE_RGB(31, 35, 46)
#define LINE    BARE_RGBA(255, 255, 255, 18)
#define ACCENT  BARE_RGB(99, 132, 255)

/* A placeholder for a line of text. */
static void label(float width, float height, BareColor color) {
    bare_box(.width = width, .height = height, .radius = height / 2, .color = color);
}

static void sidebar(void) {
    bare_column(.width = 240, .height = BARE_GROW, .padding = 20, .gap = 6,
                .color = PANEL, .border = 1, .border_color = LINE) {
        bare_row(.gap = 12, .align = BARE_CENTER, .pad = { .bottom = 24 }) {
            bare_box(.width = 36, .height = 36, .radius = 10, .color = ACCENT);
            label(110, 12, BARE_RGBA(255, 255, 255, 200));
        }
        for (int i = 0; i < 7; i++) {
            bool active = i == 1;
            bare_row(.id = BARE_IDI("nav", i), .width = BARE_GROW, .height = 40, .radius = 10,
                     .pad = { 0, 12, 0, 12 }, .gap = 12, .align = BARE_CENTER,
                     .color = active ? BARE_RGBA(99, 132, 255, 40) : BARE_RGBA(0, 0, 0, 0)) {
                bare_box(.width = 18, .height = 18, .radius = 5,
                         .color = active ? ACCENT : BARE_RGBA(255, 255, 255, 70));
                label(60 + (float)(i * 37 % 50), 9, BARE_RGBA(255, 255, 255, active ? 190 : 90));
            }
        }
        bare_box(.height = BARE_GROW);   /* spacer: pushes the profile to the bottom */
        bare_row(.width = BARE_GROW, .gap = 12, .align = BARE_CENTER, .padding = 12,
                 .radius = 12, .color = CARD) {
            bare_box(.width = 32, .height = 32, .radius = 16, .color = BARE_RGB(240, 160, 90));
            bare_column(.gap = 6) {
                label(90, 9, BARE_RGBA(255, 255, 255, 170));
                label(60, 7, BARE_RGBA(255, 255, 255, 70));
            }
        }
    }
}

static void header(void) {
    bare_row(.width = BARE_GROW, .height = 72, .pad = { 0, 28, 0, 28 }, .gap = 16,
             .align = BARE_CENTER, .border = 1, .border_color = LINE) {
        label(160, 14, BARE_RGBA(255, 255, 255, 210));
        bare_box(.width = BARE_GROW);                      /* push the rest to the right */
        bare_box(.width = BARE_PERCENT(30), .min_width = 120, .max_width = 360, .height = 38,
                 .radius = 19, .color = CARD, .border = 1, .border_color = LINE);   /* search */
        bare_box(.width = 38, .height = 38, .radius = 19, .color = CARD);
        bare_box(.width = 110, .height = 38, .radius = 19, .color = ACCENT,
                 .shadow = { 0, 6, 18, 0, BARE_RGBA(99, 132, 255, 90) });
    }
}

static void card(int i) {
    const BareColor tints[4] = {   /* not static: BARE_RGB is a compound literal */
        BARE_RGB(99, 132, 255), BARE_RGB(80, 200, 160), BARE_RGB(240, 160, 90), BARE_RGB(230, 100, 140),
    };
    BareColor tint = tints[i % 4];
    bare_column(.id = BARE_IDI("card", i), .width = BARE_GROW, .height = 190, .padding = 20, .gap = 12,
                .radius = 16, .color = CARD, .border = 1, .border_color = LINE,
                .shadow = BARE_SHADOW_SOFT) {
        bare_row(.width = BARE_GROW, .align = BARE_CENTER, .justify = BARE_SPACE_BETWEEN) {
            label(70 + (float)(i * 23 % 40), 10, BARE_RGBA(255, 255, 255, 120));
            bare_box(.width = 28, .height = 28, .radius = 8, .color = tint, .opacity = 0.9f);
        }
        label(110, 22, BARE_RGBA(255, 255, 255, 230));
        /* a tiny bar chart: bars share the width and sit on the baseline */
        bare_row(.width = BARE_GROW, .height = BARE_GROW, .gap = 6, .align = BARE_END) {
            for (int b = 0; b < 12; b++) {
                float h = 18 + (float)((i * 7 + b * 13) % 37) * 1.3f;
                bare_box(.width = BARE_GROW, .height = h, .radius = 3, .color = tint,
                         .opacity = b == 11 ? 1.0f : 0.45f);
            }
        }
    }
}

int main(void) {
    bare_window(1280, 800, "Bare: layout");
    bare_background(BG);

    while (bare_frame()) {
        BareVec2 win = bare_window_size();
        /* Reflow: one card column per 300 px of content width (flex-wrap comes later). */
        int cols = (int)((win.x - 240 - 56) / 300);
        if (cols < 1) cols = 1;
        int cards = 9;

        bare_row(.width = BARE_GROW, .height = BARE_GROW) {
            sidebar();
            bare_column(.width = BARE_GROW, .height = BARE_GROW) {
                header();
                bare_column(.width = BARE_GROW, .height = BARE_GROW, .padding = 28, .gap = 20) {
                    for (int r = 0; r * cols < cards; r++) {
                        bare_row(.id = BARE_IDI("cards", r), .width = BARE_GROW, .gap = 20) {
                            for (int c = 0; c < cols; c++) {
                                int i = r * cols + c;
                                if (i < cards) card(i);
                                else bare_box(.width = BARE_GROW);   /* keep the last row's columns aligned */
                            }
                        }
                    }
                }
            }
        }
    }
    bare_quit();
}
