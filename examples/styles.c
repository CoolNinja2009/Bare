/* styles.c: styles and themes (M7).
 *
 * One settings page in the dark and the light theme, switched by the button
 * at the top right: everything styled from the theme fades to the new colours.
 * - the theme's styles (&t->card, &t->row, &t->button, &t->primary, &t->heading ...)
 * - the app's own styles built on them (.style chains), and per-element overrides */
#include "bare.h"

static const char *names[] = { "Notifications", "Sounds", "Sync over cellular", "Show previews" };
static const char *notes[] = { "Banners and badges", "Clicks and alerts",
                               "Uses mobile data when Wi-Fi is off", "Message text on the lock screen" };

/* A switch: the knob slides, the track takes the theme's accent. */
static void toggle(bool *on, const BareTheme *t) {
    bare_row(.width = 48, .height = 28, .radius = 14, .padding = 3, .justify = *on ? BARE_END : BARE_START,
             .color = *on ? t->accent : t->border, .animate = BARE_EASE(0.2f, BARE_EASE_OUT)) {
        bare_box(.width = 22, .height = 22, .radius = 11, .color = BARE_WHITE, .shadow = BARE_SHADOW_SMALL,
                 .animate = BARE_SPRING_SNAPPY);
    }
}

int main(void) {
    bare_window(860, 720, "Bare: styles");
    bool on[4] = { true, false, true, true }, dark = true;
    while (bare_frame()) {
        const BareTheme *t = bare_theme();
        BareBox setting = { .style = &t->row, .width = BARE_GROW };          /* the app's own style, on the theme's */
        BareTextStyle note = { .style = &t->caption, .line_height = 1.4f };

        bare_column(.width = BARE_GROW, .height = BARE_GROW, .padding = 40, .gap = 24, .color = t->background,
                    .text_color = t->text, .animate = BARE_EASE(0.2f, BARE_EASE_OUT)) {
            bare_row(.width = BARE_GROW, .align = BARE_CENTER) {
                bare_text("Settings", .style = &t->title);
                bare_box(.width = BARE_GROW);
                bare_row(.style = &t->button) {
                    if (bare_this().clicked) { dark = !dark; bare_set_theme(dark ? bare_theme_dark() : bare_theme_light()); }
                    bare_text(dark ? "Light theme" : "Dark theme", .style = &t->body);
                }
            }
            bare_column(.style = &t->panel, .width = BARE_GROW, .gap = 2, .padding = 8) {
                for (int i = 0; i < 4; i++) {
                    bare_row(.id = BARE_IDI("setting", i), .style = &setting) {
                        if (bare_this().clicked) on[i] = !on[i];
                        bare_column(.width = BARE_GROW, .gap = 2) {
                            bare_text(names[i], .style = &t->body);
                            bare_text(notes[i], .style = &note);
                        }
                        toggle(&on[i], t);
                    }
                }
            }
            bare_row(.width = BARE_GROW, .gap = 20, .align = BARE_STRETCH) {
                bare_column(.style = &t->card, .width = BARE_GROW) {
                    bare_text("Storage", .style = &t->heading);
                    bare_text("38.2 GB of 64 GB used", .style = &note);
                    bare_row(.width = BARE_GROW, .height = 8, .radius = 4, .color = t->border,
                             .animate = BARE_EASE(0.2f, BARE_EASE_OUT)) {
                        bare_box(.width = BARE_PERCENT(60), .height = BARE_GROW, .radius = 4, .color = t->accent);
                    }
                }
                bare_column(.style = &t->card, .width = BARE_GROW, .gap = 14) {
                    bare_text("Plan", .style = &t->heading);
                    bare_text("Free: 3 devices, 64 GB", .style = &note);
                    bare_row(.gap = 10) {
                        bare_row(.style = &t->primary) { bare_text("Upgrade", .style = &t->body); }
                        bare_row(.style = &t->button) { bare_text("Compare", .style = &t->body); }
                    }
                }
            }
        }
    }
    bare_quit();
}
