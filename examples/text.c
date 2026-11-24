/* text.c: text and images (M5).
 *
 * - a paragraph that rewraps as the window resizes
 * - sizes from 10 to 48, colours inherited from .text_color
 * - start / centre / end alignment of wrapped lines
 * - a profile card with a round avatar image (.radius = half its size)
 * - a counter with bare_textf, and a button whose label changes colour on hover
 * - formatting: underline, strike, letter spacing, upper case, opacity, and
 *   text cut to two lines or one with an ellipsis
 * - a "NEW" badge pinned to the card's corner with .position = BARE_ABSOLUTE
 * - weights, italics, the built-in monospace font, a soft text shadow, and
 *   rich text: spans of their own style wrapping together as one paragraph
 * - hover the paragraph: a liquid glass loupe magnifies the text under it */
#include "bare.h"

#ifndef ASSETS
#define ASSETS "assets/"
#endif

static const char *about =
    "Bare is a UI framework in plain C. Text is shaped once when you declare it, wrapped by the "
    "layout at whatever width it gets, and drawn from a glyph atlas rasterized at the screen's real "
    "pixel size, so it stays sharp at 100%, 150% and 200%. Drag the window narrower: this paragraph "
    "rewraps, and the columns beside it shrink down to their longest word.";

static void card(const char *title, BareAlign align, const char *body) {
    bare_column(.width = BARE_GROW, .padding = 18, .gap = 8, .radius = 14, .color = BARE_RGB(32, 35, 46),
                .text_color = BARE_RGB(200, 205, 220)) {
        bare_text(title, .size = 13, .color = BARE_RGB(120, 170, 255));
        bare_text(body, .size = 15, .align = align);
    }
}

/* The page; returns the paragraph's state (for the loupe). */
static BareState page(BareImage avatar, int *clicks) {
    BareState para = { 0 };
    bare_row(.width = BARE_GROW, .height = BARE_GROW, .padding = 40, .gap = 32) {
        /* The text column. */
        bare_column(.width = BARE_GROW, .gap = 22) {
            bare_text("Text, at last", .size = 44, .weight = BARE_BOLD,
                      .shadow = { 0, 4, 14, 0, BARE_RGBA(70, 110, 245, 160) });
            para = bare_text(about, .size = 17, .line_height = 1.5f, .color = BARE_RGB(185, 190, 205));

            bare_row(.gap = 14, .align = BARE_END) {
                static const float sizes[] = { 10, 12, 14, 16, 20, 24, 32, 40, 48 };
                for (int i = 0; i < 9; i++) bare_text("Ag", .size = sizes[i]);
            }

            bare_row(.width = BARE_GROW, .gap = 16, .align = BARE_STRETCH) {
                card("START", BARE_START, "Lines start at the left edge, like most text you read.");
                card("CENTER", BARE_CENTER, "Each wrapped line is centred within the box.");
                card("END", BARE_END, "Lines end at the right edge, as in a column of numbers.");
            }

            bare_text("Accents and symbols: café, naïve, Ångström, 25 °C, 3 × 4 − 2, “quotes”, € £ ¥ — ok",
                      .size = 15, .color = BARE_RGB(150, 155, 170));

            bare_row(.gap = 22, .align = BARE_CENTER) {
                bare_text("Underlined", .size = 15, .underline = true);
                bare_text("Struck through", .size = 15, .strike = true, .color = BARE_RGB(150, 155, 170));
                bare_text("Spaced capitals", .size = 13, .letter_spacing = 2.5f, .transform = BARE_UPPERCASE,
                          .color = BARE_RGB(120, 170, 255));
                bare_text("Half opacity", .size = 15, .opacity = 0.5f);
            }
            bare_row(.gap = 22, .align = BARE_CENTER) {
                bare_text("Regular", .size = 15);
                bare_text("Semibold", .size = 15, .weight = BARE_SEMIBOLD);
                bare_text("Bold", .size = 15, .weight = BARE_BOLD);
                bare_text("Italic", .size = 15, .italic = true);
                bare_text("Bold italic", .size = 15, .weight = BARE_BOLD, .italic = true);
                bare_text("int main(void);", .size = 14, .font = BARE_MONO);
            }
            bare_paragraph(.size = 16, .line_height = 1.5f, .color = BARE_RGB(185, 190, 205)) {
                bare_span("Rich text: ", .weight = BARE_SEMIBOLD, .color = BARE_RGB(255, 255, 255));
                bare_span("spans of ");
                bare_span("bold", .weight = BARE_BOLD);
                bare_span(", ");
                bare_span("italic", .italic = true);
                bare_span(", ");
                bare_span("inline code", .font = BARE_MONO, .size = 14, .background = BARE_RGBA(255, 255, 255, 22));
                bare_span(", a ");
                bare_span("link", .color = BARE_RGB(120, 170, 255), .underline = true);
                bare_span(", ");
                bare_span("bigger", .size = 24, .weight = BARE_BOLD);
                bare_span(" and ");
                bare_span("marked", .background = BARE_RGBA(255, 200, 60, 90));
                bare_span(" words wrap together as one paragraph, like inline elements in HTML.");
            }
            bare_text(about, .size = 15, .max_lines = 2, .ellipsis = true, .color = BARE_RGB(150, 155, 170));
            bare_text(about, .size = 15, .nowrap = true, .ellipsis = true, .color = BARE_RGB(150, 155, 170));
        }

        /* The profile card. */
        bare_column(.width = 300, .padding = 26, .gap = 16, .radius = 20, .align = BARE_CENTER,
                    .color = BARE_RGB(30, 33, 44), .shadow = BARE_SHADOW_SOFT) {
            /* Out of the column's flow: pinned to its top right corner. */
            bare_row(.position = BARE_ABSOLUTE, .anchor = BARE_TOP_RIGHT, .x = -14, .y = 14,
                     .pad = { 4, 9, 4, 9 }, .radius = 8, .color = BARE_RGB(255, 90, 110)) {
                bare_text("NEW", .size = 11, .letter_spacing = 1);
            }
            bare_box(.width = 112, .height = 112, .radius = 56, .image = avatar,
                     .border = 3, .border_color = BARE_RGB(255, 255, 255));
            bare_text("Sunset Hills", .size = 22);
            bare_text("Avatar loaded from a PNG with bare_image_load, rounded by .radius.",
                      .size = 14, .align = BARE_CENTER, .color = BARE_RGB(150, 156, 172));
            bare_textf("Clicked %d time%s", *clicks, *clicks == 1 ? "" : "s");
            bare_row(.pad = { 12, 26, 12, 26 }, .radius = 12, .cursor = BARE_CURSOR_HAND,
                     .color = BARE_RGB(70, 110, 245), .text_color = BARE_RGB(235, 240, 255),
                     .hover = { .color = BARE_RGB(90, 130, 255), .text_color = BARE_RGB(255, 255, 255) },
                     .press = { .scale = 0.96f }, .animate = BARE_EASE(0.15f, BARE_EASE_OUT)) {
                if (bare_this().clicked) (*clicks)++;
                bare_text("Click me", .size = 16);
            }
        }
    }
    return para;
}

int main(void) {
    bare_window(1100, 760, "Bare: text");
    BareImage avatar = bare_image_load(ASSETS "avatar.png");
    int clicks = 0;

    while (bare_frame()) {
        bare_background(BARE_RGB(18, 20, 27));
        bare_stack(.width = BARE_GROW, .height = BARE_GROW) {
            BareState para = page(avatar, &clicks);

            /* The loupe: glass refracts whatever was drawn before it, text included. */
            BareVec2 m = bare_mouse();
            bool lens = para.hovered;
            bare_box(.id = BARE_ID("loupe"), .width = 140, .height = 140, .radius = 70, .passthrough = true,
                     .offset = { m.x - 70, m.y - 70 },
                     .glass = lens ? (BareGlass){ .zoom = 1.6f, .refraction = 0.7f, .depth = 16, .blur = BARE_ZERO,
                                                  .shine = 1.4f, .tint = { 255, 255, 255, 10 } }
                                   : (BareGlass){ 0 },
                     .shadow = { 0, 10, 30, 0, BARE_RGBA(0, 0, 0, lens ? 90 : 0) },
                     .animate = BARE_SPRING(0.22f, 0.8f));
        }
    }
    bare_quit();
}
