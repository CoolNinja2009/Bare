/* shapes.c: corner radii, borders, shadows and opacity side by side. */
#include "bare.h"

#define CARD BARE_RGB(255, 255, 255)
#define BLUE BARE_RGB(40, 120, 255)

int main(void) {
    bare_window(1000, 720, "Bare: shapes");
    bare_background(BARE_RGB(232, 236, 242));
    while (bare_frame()) {
        bare_column(.padding = 40, .gap = 48) {
            /* Corner radii: square, soft, round, pill, circle. */
            bare_row(.gap = 32, .align = BARE_CENTER) {
                bare_box(.width = 140, .height = 100, .color = BLUE);
                bare_box(.width = 140, .height = 100, .radius = 8, .color = BLUE);
                bare_box(.width = 140, .height = 100, .radius = 24, .color = BLUE);
                bare_box(.width = 140, .height = 56, .radius = 28, .color = BLUE);
                bare_box(.width = 100, .height = 100, .radius = 50, .color = BLUE);
            }
            /* Borders: thin, thick, border only, translucent fill. */
            bare_row(.gap = 32) {
                bare_box(.width = 140, .height = 100, .radius = 12, .color = CARD,
                         .border = 1, .border_color = BARE_RGB(200, 205, 215));
                bare_box(.width = 140, .height = 100, .radius = 12, .color = CARD,
                         .border = 6, .border_color = BLUE);
                bare_box(.width = 140, .height = 100, .radius = 24,
                         .border = 3, .border_color = BARE_RGB(230, 70, 90));
                bare_box(.width = 140, .height = 100, .radius = 50,
                         .color = BARE_RGBA(40, 120, 255, 60), .border = 2, .border_color = BLUE);
            }
            /* Shadows: small, soft, large, coloured glow, offset hard shadow. */
            bare_row(.gap = 40, .padding = 8) {
                bare_box(.width = 140, .height = 100, .radius = 12, .color = CARD, .shadow = BARE_SHADOW_SMALL);
                bare_box(.width = 140, .height = 100, .radius = 12, .color = CARD, .shadow = BARE_SHADOW_SOFT);
                bare_box(.width = 140, .height = 100, .radius = 12, .color = CARD, .shadow = BARE_SHADOW_LARGE);
                bare_box(.width = 140, .height = 100, .radius = 50, .color = BLUE,
                         .shadow = { 0, 0, 32, 4, { 40, 120, 255, 160 } });
                bare_box(.width = 140, .height = 100, .radius = 12, .color = CARD,
                         .border = 2, .border_color = BARE_BLACK, .shadow = { 8, 8, 0, 0, { 0, 0, 0, 255 } });
            }
            /* Opacity, nested. */
            bare_row(.gap = 32) {
                bare_box(.width = 140, .height = 60, .radius = 12, .color = BLUE, .opacity = 1.0f);
                bare_box(.width = 140, .height = 60, .radius = 12, .color = BLUE, .opacity = 0.6f);
                bare_box(.width = 140, .height = 60, .radius = 12, .color = BLUE, .opacity = 0.3f);
                bare_row(.padding = 10, .radius = 16, .color = CARD, .opacity = 0.5f) {
                    bare_box(.width = 120, .height = 40, .radius = 8, .color = BLUE);
                }
            }
        }
    }
    bare_quit();
}
