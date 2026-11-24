/* widgets.c: every core widget in one settings panel (M8), no custom styling.
 * Tab walks them; Space toggles and presses; arrows move a focused slider. */
#include "bare.h"

static const char *rooms[] = { "Living room", "Kitchen", "Bedroom", "Office", "Garden" };

int main(void) {
    bare_window(760, 560, "Bare: widgets");
    bool power = true, night = false, dark = true;
    float volume = 40, warmth = 3200;
    int room = 0, saved = 0;
    while (bare_frame()) {
        const BareTheme *t = bare_theme();
        bare_row(.width = BARE_GROW, .height = BARE_GROW, .padding = 32, .gap = 24) {
            bare_column(.style = &t->card, .width = 220, .height = BARE_GROW, .gap = 2) {
                bare_text("Rooms", .style = &t->heading);
                for (int i = 0; i < 5; i++)
                    if (bare_list_item(rooms[i], i == room, .id = BARE_IDI("room", i)).clicked) room = i;
            }
            bare_column(.style = &t->card, .width = BARE_GROW, .height = BARE_GROW, .gap = 16) {
                bare_text(rooms[room], .style = &t->title);
                bare_checkbox("Power", &power);
                bare_checkbox("Night mode", &night);
                if (bare_checkbox("Dark theme", &dark)) bare_set_theme(dark ? bare_theme_dark() : bare_theme_light());
                bare_row(.gap = 16, .align = BARE_CENTER) {
                    bare_row(.width = 70) { bare_text("Volume", .style = &t->body); }
                    bare_slider(&volume, 0, 100, .width = 240);
                    bare_textf("%.0f%%", volume);
                }
                bare_row(.gap = 16, .align = BARE_CENTER) {
                    bare_row(.width = 70) { bare_text("Warmth", .style = &t->body); }
                    bare_slider(&warmth, 2000, 6500, .width = 240);
                    bare_textf("%.0f K", warmth);
                }
                bare_box(.height = BARE_GROW);
                bare_row(.gap = 10) {
                    if (bare_button("Reset").clicked) { volume = 40; warmth = 3200; power = true; night = false; }
                    if (bare_button("Save", .style = &t->primary).clicked) saved++;
                }
                if (saved) bare_textf("Saved %d time%s.", saved, saved == 1 ? "" : "s");
            }
        }
    }
    bare_quit();
}
