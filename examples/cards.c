/* cards.c: animation (M4), written with styles (M7).
 *
 * - the springy button from the top of bare.h: hover and press bounce it;
 *   clicking it shuffles the cards, which glide to their new places (each card
 *   keeps its BARE_IDI id, so Bare knows it moved)
 * - a switch that widens the cards: sizes animate too, and the content moves
 *   with them
 * - cards lift on hover with a spring; hover in and out quickly: never a jump */
#include "bare.h"

#define CARDS 6

static const BareColor accents[CARDS] = {
    { 70, 110, 245, 255 }, { 40, 170, 120, 255 }, { 225, 70, 90, 255 },
    { 240, 170, 40, 255 }, { 150, 90, 230, 255 }, { 30, 170, 200, 255 },
};
static const char *names[CARDS] = { "Ocean", "Forest", "Ember", "Amber", "Violet", "Lagoon" };

static unsigned rng = 12345;
static int roll(int n) { rng = rng * 1103515245u + 12345u; return (int)((rng >> 16) % (unsigned)n); }

int main(void) {
    bare_window(900, 760, "Bare: cards");
    bare_background(BARE_RGB(16, 18, 24));

    /* The looks, once: cards lift on hover; everything inside glides with the card. */
    const BareBox card = { .padding = 20, .gap = 10, .radius = 16, .color = BARE_RGB(36, 38, 46),
                           .shadow = BARE_SHADOW_SOFT,
                           .hover = { .offset = { 0, -6 }, .shadow = BARE_SHADOW_LARGE },
                           .animate = BARE_SPRING_SNAPPY, .animate_layout = BARE_SPRING_SMOOTH };
    const BareBox glide = { .animate_layout = BARE_SPRING_SMOOTH };
    const BareBox springy = { .pad = { 16, 28, 16, 28 }, .radius = 14, .color = BARE_RGB(40, 120, 255),
                              .text_color = BARE_WHITE, .cursor = BARE_CURSOR_HAND, .shadow = BARE_SHADOW_SMALL,
                              .hover = { .scale = 1.06f, .color = BARE_RGB(70, 145, 255) },
                              .press = { .scale = 0.94f }, .animate = BARE_SPRING_BOUNCY };

    int order[CARDS] = { 0, 1, 2, 3, 4, 5 };
    bool wide = false;

    while (bare_frame()) {
        bare_column(.width = BARE_GROW, .height = BARE_GROW, .padding = 40, .gap = 28, .align = BARE_CENTER) {
            bare_row(.gap = 20, .align = BARE_CENTER) {
                bare_row(.id = BARE_ID("shuffle"), .style = &springy) {
                    if (bare_this().clicked)
                        for (int i = CARDS - 1; i > 0; i--) {   /* Fisher-Yates */
                            int j = roll(i + 1), t = order[i];
                            order[i] = order[j];
                            order[j] = t;
                        }
                    bare_text("Shuffle", .size = 18);
                }
                /* The switch: its knob slides and its colour fades. */
                bare_row(.id = BARE_ID("wide"), .width = 72, .height = 40, .radius = 20, .padding = 4,
                         .justify = wide ? BARE_END : BARE_START, .cursor = BARE_CURSOR_HAND,
                         .color = wide ? BARE_RGB(80, 200, 140) : BARE_RGB(60, 64, 76),
                         .animate = BARE_EASE(0.2f, BARE_EASE_OUT)) {
                    if (bare_this().clicked) wide = !wide;
                    bare_box(.width = 32, .height = 32, .radius = 16, .color = BARE_WHITE,
                             .shadow = BARE_SHADOW_SMALL, .animate = BARE_SPRING_SNAPPY);
                }
            }

            /* Cards in two columns; keyed by card, not by place, so they glide when shuffled. */
            bare_row(.gap = 20) {
                for (int c = 0; c < 2; c++) {
                    bare_column(.id = BARE_IDI("col", c), .gap = 20) {
                        for (int k = c; k < CARDS; k += 2) {
                            int i = order[k];
                            bare_column(.id = BARE_IDI("card", i), .style = &card, .width = wide ? 380.0f : 260.0f) {
                                bare_row(.style = &glide, .width = BARE_GROW, .gap = 12, .align = BARE_CENTER) {
                                    bare_box(.style = &glide, .width = 40, .height = 40, .radius = 12,
                                             .color = accents[i]);
                                    bare_text(names[i], .size = 20);
                                }
                                bare_text("Glides when shuffled.", .size = 15, .color = BARE_RGB(150, 156, 172));
                            }
                        }
                    }
                }
            }
        }
    }
    bare_quit();
}
