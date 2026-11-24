/* editor.c: text fields and keyboard focus (M7.5).
 *
 * - a sign-up form: name, email, password and a multi-line bio, usable by
 *   keyboard alone (Tab / Shift+Tab, Enter submits, Escape leaves a field)
 * - every shortcut works with no app code: word jumps, Home / End, select
 *   all, cut / copy / paste, undo / redo, double / triple click, IME
 * - a search field filtering 5,000 rows drawn with bare_visible
 * - a theme button, focusable like the fields */
#include "bare.h"
#include <stdio.h>
#include <string.h>

static bool contains(const char *hay, const char *needle) {   /* ASCII case-insensitive */
    if (!*needle) return true;
    for (; *hay; hay++) {
        int i = 0;
        while (needle[i] && hay[i] && ((hay[i] | 32) == (needle[i] | 32))) i++;
        if (!needle[i]) return true;
    }
    return false;
}

static const char *fruit[] = { "apple", "banana", "cherry", "dragonfruit", "elderberry", "fig", "grape",
                               "honeydew", "kiwi", "lemon", "mango", "nectarine", "orange", "papaya",
                               "quince", "raspberry", "strawberry", "tangerine", "watermelon" };
#define ITEMS 5000

static void label(const char *text) { bare_text(text, .style = &bare_theme()->caption); }

int main(void) {
    bare_window(1000, 700, "Bare: text fields");
    static char name[64], email[96], password[64], bio[1024], search[64];
    static char names[ITEMS][32];
    for (int i = 0; i < ITEMS; i++) snprintf(names[i], sizeof names[i], "%s %d", fruit[i % 19], i + 1);
    static int match[ITEMS];
    char status[160] = "Fill in the form: Tab moves between fields, Enter submits.";
    bool dark = true;

    while (bare_frame()) {
        const BareTheme *t = bare_theme();
        bare_row(.width = BARE_GROW, .height = BARE_GROW, .padding = 32, .gap = 24) {
            /* The form. */
            bare_column(.style = &t->card, .width = 420, .gap = 8) {
                bare_text("Create an account", .style = &t->heading);
                bare_text(status, .style = &t->caption);
                bare_box(.height = 6);
                bool submit = false;
                label("Name");
                submit |= bare_input(name, sizeof name, .placeholder = "Ada Lovelace", .width = BARE_GROW).submitted;
                label("Email");
                submit |= bare_input(email, sizeof email, .placeholder = "ada@example.com", .width = BARE_GROW).submitted;
                label("Password");
                submit |= bare_input(password, sizeof password, .password = true, .placeholder = "8 characters or more",
                                     .width = BARE_GROW).submitted;
                label("Bio");
                bare_input(bio, sizeof bio, .multiline = true, .lines = 5, .width = BARE_GROW,
                           .placeholder = "A few lines about you. Enter starts a new line.");
                bare_box(.height = 6);
                bare_row(.gap = 10, .align = BARE_CENTER) {
                    bare_row(.style = &t->primary, .focusable = true,
                             .focus = { .shadow = { 0, 0, 0, 3, { 40, 120, 255, 90 } } }) {
                        bare_text("Sign up", .style = &t->body);
                        BareState b = bare_this();
                        submit |= b.clicked || (b.focused && (bare_key_pressed(BARE_KEY_ENTER) ||
                                                              bare_key_pressed(BARE_KEY_SPACE)));
                    }
                    bare_row(.style = &t->button, .focusable = true,
                             .focus = { .shadow = { 0, 0, 0, 3, { 40, 120, 255, 90 } } }) {
                        bare_text(dark ? "Light theme" : "Dark theme", .style = &t->body);
                        BareState b = bare_this();
                        if (b.clicked || (b.focused && bare_key_pressed(BARE_KEY_SPACE))) {
                            dark = !dark;
                            bare_set_theme(dark ? bare_theme_dark() : bare_theme_light());
                        }
                    }
                }
                if (submit) {
                    if (!name[0] || !strchr(email, '@') || strlen(password) < 8)
                        snprintf(status, sizeof status, "Almost: a name, an email with @ and 8+ password characters.");
                    else
                        snprintf(status, sizeof status, "Welcome, %s! (%d characters of bio)", name, (int)strlen(bio));
                }
            }

            /* Search. */
            bare_column(.style = &t->card, .width = BARE_GROW, .height = BARE_GROW, .gap = 10) {
                bare_text("Search", .style = &t->heading);
                bare_input(search, sizeof search, .placeholder = "Type to filter 5,000 rows", .width = BARE_GROW);
                int n = 0;
                for (int i = 0; i < ITEMS; i++)
                    if (contains(names[i], search)) match[n++] = i;
                bare_textf("%d match%s", n, n == 1 ? "" : "es");
                bare_column(.width = BARE_GROW, .height = BARE_GROW, .scroll = BARE_SCROLL_Y, .gap = 2) {
                    bare_visible(i, n, 34) {
                        bare_row(.id = BARE_IDI("row", match[i]), .style = &t->row, .width = BARE_GROW,
                                 .height = 34, .pad = { 0, 12, 0, 12 }) {
                            bare_text(names[match[i]], .style = &t->body);
                        }
                    }
                }
            }
        }
    }
    bare_quit();
}
