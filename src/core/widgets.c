/* widgets.c: one-call widgets (M8), built only from the public API in bare.h,
 * to show it is enough: each is a few boxes and texts with the theme's looks.
 *
 * Every widget takes a BareBox of options: .style replaces the theme's look,
 * and any field set on the call overrides that style (as for any element).
 * All are focusable: Tab reaches them, Space (and Enter for buttons) presses
 * them, a slider takes the arrow keys. */
#include "bare.h"
#include <string.h>

static BareBox with_style(const BareBox *box, const BareBox *fallback) {
    BareBox b = *box;
    if (!b.style) b.style = fallback;
    b.focusable = true;
    return b;
}

static bool activated(BareState s, bool enter) {
    return s.focused && (bare_key_pressed(BARE_KEY_SPACE) || (enter && bare_key_pressed(BARE_KEY_ENTER)));
}

/* ---- button ------------------------------------------------------------------- */

BareState bare_button_(const char *label, const BareBox *box, const char *file, int line) {
    const BareTheme *t = bare_theme();
    BareBox b = with_style(box, &t->button);
    b.dir = BARE_ROW;
    BareState s;
    memset(&s, 0, sizeof s);
    bare_open_(&b, file, line);
    bare_text(label ? label : "", .style = &t->body, .nowrap = true);
    s = bare_this();
    bare_close_();
    if (activated(s, true)) s.clicked = true;
    return s;
}

/* ---- checkbox ------------------------------------------------------------------- */

bool bare_checkbox_(const char *label, bool *value, const BareBox *box, const char *file, int line) {
    const BareTheme *t = bare_theme();
    static const BareBox row = { .dir = BARE_ROW, .gap = 10, .align = BARE_CENTER, .cursor = BARE_CURSOR_HAND,
                                 .radius = 6 };
    BareBox b = with_style(box, &row);
    b.dir = BARE_ROW;
    bool on = value && *value, changed = false;
    bare_open_(&b, file, line);
    {
        BareState s = bare_this();
        if (s.clicked || activated(s, false)) {
            on = !on;
            changed = true;
            if (value) *value = on;
        }
        BareColor ring = s.hovered ? t->muted : t->border;
        if (ring.a < 90) ring.a = 90;
        /* The box fills with the accent; the tick springs in. */
        bare_row(.width = 20, .height = 20, .radius = 6, .align = BARE_CENTER, .justify = BARE_CENTER,
                 .border = on ? BARE_ZERO : 1.5f, .border_color = ring,
                 .color = on ? t->accent : BARE_CLEAR, .text_color = BARE_WHITE,
                 .shadow = s.focused ? (BareShadow){ 0, 0, 0, 3, { t->accent.r, t->accent.g, t->accent.b, 90 } }
                                     : (BareShadow){ 0 },
                 .animate = BARE_EASE(0.15f, BARE_EASE_OUT)) {
            bare_row(.scale = on ? 1.0f : 0.3f, .opacity = on ? 1.0f : BARE_ZERO, .animate = BARE_SPRING_SNAPPY) {
                bare_text("\xE2\x9C\x93", .size = 14, .weight = BARE_BOLD, .line_height = 1.0f);   /* U+2713 */
            }
        }
        if (label && *label) bare_text(label, .style = &t->body);
    }
    bare_close_();
    return changed;
}

/* ---- slider ---------------------------------------------------------------------- */

#define KNOB 18.0f

bool bare_slider_(float *value, float lo, float hi, const BareBox *box, const char *file, int line) {
    const BareTheme *t = bare_theme();
    static const BareBox area = { .dir = BARE_ROW, .width = 220, .height = 24, .align = BARE_CENTER,
                                  .cursor = BARE_CURSOR_HAND };
    BareBox b = with_style(box, &area);
    b.dir = BARE_ROW;
    float v = value ? *value : lo, old = v;
    float span = hi - lo;
    bare_open_(&b, file, line);
    {
        BareState s = bare_this();
        /* Pressed: the knob follows the mouse, even outside the slider (the
         * press stays with it until the button comes up). */
        float w = s.rect.w - KNOB;
        if (s.pressed && w > 0 && span != 0) {
            float f = (bare_mouse().x - s.rect.x - KNOB * 0.5f) / w;
            v = lo + (f < 0 ? 0 : f > 1 ? 1 : f) * span;
        }
        if ((s.focused || s.hovered) && span != 0) {
            float step = span / (bare_key_down(BARE_KEY_SHIFT) ? 10.0f : 100.0f);
            if (bare_key_pressed(BARE_KEY_LEFT) || bare_key_pressed(BARE_KEY_DOWN)) v -= step;
            if (bare_key_pressed(BARE_KEY_RIGHT) || bare_key_pressed(BARE_KEY_UP)) v += step;
            if (bare_key_pressed(BARE_KEY_PAGE_DOWN)) v -= span / 10.0f;
            if (bare_key_pressed(BARE_KEY_PAGE_UP)) v += span / 10.0f;
            if (bare_key_pressed(BARE_KEY_HOME)) v = lo;
            if (bare_key_pressed(BARE_KEY_END)) v = hi;
        }
        float a = lo < hi ? lo : hi, z = lo < hi ? hi : lo;
        if (v < a) v = a;
        if (v > z) v = z;
        float f = span != 0 ? (v - lo) / span : 0;

        /* The track, its filled part, then the knob between two growing spacers. */
        bare_box(.position = BARE_ABSOLUTE, .anchor = BARE_LEFT, .width = BARE_GROW, .height = 4, .radius = 2,
                 .color = t->border.a < 60 ? (BareColor){ t->border.r, t->border.g, t->border.b, 60 } : t->border,
                 .passthrough = true);
        bare_box(.position = BARE_ABSOLUTE, .anchor = BARE_LEFT, .width = BARE_PERCENT(f * 100.0f), .height = 4,
                 .radius = 2, .color = t->accent, .passthrough = true);
        bare_box(.width = BARE_GROW, .grow = f > 0 ? f : BARE_ZERO, .passthrough = true);
        bare_box(.width = KNOB, .height = KNOB, .radius = KNOB / 2, .color = BARE_WHITE, .passthrough = true,
                 .shadow = s.focused ? (BareShadow){ 0, 0, 0, 4, { t->accent.r, t->accent.g, t->accent.b, 90 } }
                                     : (BareShadow){ 0, 1, 4, 0, { 0, 0, 0, 90 } },
                 .scale = s.pressed ? 1.15f : s.hovered ? 1.08f : 1.0f, .animate = BARE_SPRING_SNAPPY);
        bare_box(.width = BARE_GROW, .grow = f < 1 ? 1 - f : BARE_ZERO, .passthrough = true);
    }
    bare_close_();
    if (value) *value = v;
    return v != old;
}

/* ---- list item ------------------------------------------------------------------ */

BareState bare_list_item_(const char *label, bool selected, const BareBox *box, const char *file, int line) {
    const BareTheme *t = bare_theme();
    BareBox b = with_style(box, &t->row);
    b.dir = BARE_ROW;
    if (!b.width) b.width = BARE_GROW;
    if (selected) {
        BareColor c = t->accent;
        c.a = 46;
        if (!b.color.a) b.color = c;
        if (!b.hover.color.a) { c.a = 62; b.hover.color = c; }
    }
    BareState s;
    bare_open_(&b, file, line);
    bare_text(label ? label : "", .style = &t->body, .weight = selected ? BARE_SEMIBOLD : 0, .nowrap = true,
              .ellipsis = true);
    s = bare_this();
    bare_close_();
    if (activated(s, true)) s.clicked = true;
    return s;
}
