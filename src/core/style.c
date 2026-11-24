/* style.c: styles, defaults, looks and themes (ARCHITECTURE §5).
 *
 * For every element: start from zero, lay its style chain on top (the base
 * of the chain first), then the element itself; then the built-in defaults
 * fill what's still 0; hover and press looks go on top; BARE_ZERO becomes 0.
 *
 * "Set" means "any bit non-zero", so the deliberate nothings count as set:
 * BARE_ZERO (-0.0f), BARE_CLEAR (transparent white), BARE_START, BARE_INSTANT.
 * Fields are merged one at a time, except groups that only make sense
 * together (padding and pad, a shadow, glass, an animation), which are taken
 * whole from the most specific place that sets any of them. */
#include "internal.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

#define MAX_CHAIN 8

typedef struct Field { uint16_t off, size; } Field;

#define F(T, f)         { (uint16_t)offsetof(T, f), (uint16_t)sizeof(((T *)0)->f) }
#define SPAN(T, a, b)   { (uint16_t)offsetof(T, a), (uint16_t)(offsetof(T, b) + sizeof(((T *)0)->b) - offsetof(T, a)) }
#define LOOK(T, l)      F(T, l.color), F(T, l.border_color), F(T, l.text_color), F(T, l.shadow), \
                        F(T, l.opacity), F(T, l.scale), F(T, l.radius), F(T, l.offset)

/* Not here: id, style and dir (they always come from the element). */
static const Field box_fields[] = {
    F(BareBox, width), F(BareBox, height), F(BareBox, min_width), F(BareBox, max_width),
    F(BareBox, min_height), F(BareBox, max_height), F(BareBox, grow), F(BareBox, shrink),
    F(BareBox, position), F(BareBox, anchor), F(BareBox, x), F(BareBox, y),
    F(BareBox, align), F(BareBox, justify), F(BareBox, gap),
    SPAN(BareBox, padding, pad),   /* `.padding = 4` replaces a style's .pad, as CSS's shorthand does */
    F(BareBox, color), F(BareBox, border_color), F(BareBox, border), F(BareBox, radius),
    F(BareBox, shadow), F(BareBox, opacity), F(BareBox, scale), F(BareBox, offset), F(BareBox, glass),
    F(BareBox, text_color), F(BareBox, image),
    LOOK(BareBox, hover), LOOK(BareBox, press), LOOK(BareBox, focus),
    F(BareBox, cursor), F(BareBox, focusable), F(BareBox, passthrough), F(BareBox, clip), F(BareBox, scroll),
    F(BareBox, animate), F(BareBox, animate_layout),
};

static const Field text_fields[] = {
    F(BareTextStyle, font), F(BareTextStyle, size), F(BareTextStyle, color), F(BareTextStyle, line_height),
    F(BareTextStyle, align), F(BareTextStyle, nowrap), F(BareTextStyle, animate),
    F(BareTextStyle, max_lines), F(BareTextStyle, ellipsis), F(BareTextStyle, underline), F(BareTextStyle, strike),
    F(BareTextStyle, letter_spacing), F(BareTextStyle, transform), F(BareTextStyle, opacity),
    F(BareTextStyle, position), F(BareTextStyle, anchor), F(BareTextStyle, x), F(BareTextStyle, y),
};

static bool is_set(const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++)
        if (b[i]) return true;
    return false;
}

/* Copies every field `top` sets over `dst`. */
static void lay(void *dst, const void *top, const Field *fields, int n) {
    unsigned char *d = dst;
    const unsigned char *t = top;
    for (int i = 0; i < n; i++)
        if (is_set(t + fields[i].off, fields[i].size)) memcpy(d + fields[i].off, t + fields[i].off, fields[i].size);
}

/* A broken chain is reported once per style, not every frame. */
static void warn_cycle(BareContext *ctx, const void *style, const char *file, int line) {
    for (int i = 0; i < ctx->style_warned_count; i++)
        if (ctx->style_warned[i] == style) return;
    if (ctx->style_warned_count < (int)(sizeof ctx->style_warned / sizeof ctx->style_warned[0]))
        ctx->style_warned[ctx->style_warned_count++] = style;
    bare__log(ctx, "style chain at %s:%d loops back on itself or is deeper than %d; the rest is ignored",
              file, line, MAX_CHAIN);
}

/* Walks a chain of `.style` pointers (the pointer sits at `link` in each
 * value) from `top`; returns how many styles it holds, most specific first. */
static int chain_of(BareContext *ctx, const void *top, size_t link, const void **out, const char *file, int line) {
    int n = 0;
    const void *s;
    memcpy(&s, (const unsigned char *)top + link, sizeof s);
    while (s) {
        bool loop = s == top;
        for (int i = 0; i < n; i++) loop = loop || out[i] == s;
        if (loop || n == MAX_CHAIN) { warn_cycle(ctx, s, file, line); break; }
        out[n++] = s;
        memcpy(&s, (const unsigned char *)s + link, sizeof s);
    }
    return n;
}

void bare__style_box(BareContext *ctx, const BareBox *box, BareBox *out, const char *file, int line) {
    if (!box->style) { *out = *box; return; }
    const void *chain[MAX_CHAIN];
    int n = chain_of(ctx, box, offsetof(BareBox, style), chain, file, line);
    const int nf = (int)(sizeof box_fields / sizeof box_fields[0]);
    memset(out, 0, sizeof *out);
    for (int i = n - 1; i >= 0; i--) lay(out, chain[i], box_fields, nf);
    lay(out, box, box_fields, nf);
    out->id = box->id;
    out->dir = box->dir;
    out->style = NULL;
}

void bare__style_text(BareContext *ctx, const BareTextStyle *st, BareTextStyle *out, const char *file, int line) {
    if (!st->style) { *out = *st; return; }
    const void *chain[MAX_CHAIN];
    int n = chain_of(ctx, st, offsetof(BareTextStyle, style), chain, file, line);
    const int nf = (int)(sizeof text_fields / sizeof text_fields[0]);
    memset(out, 0, sizeof *out);
    for (int i = n - 1; i >= 0; i--) lay(out, chain[i], text_fields, nf);
    lay(out, st, text_fields, nf);
    out->id = st->id;
    out->style = NULL;
}

/* ---- defaults and looks ------------------------------------------------------ */

/* 0 means "default"; BARE_ZERO (-0.0f) means a real zero. */
static float def(float v, float fallback) {
    if (v == 0.0f) return signbit(v) ? 0.0f : fallback;
    return v;
}

/* Glass is on when any field is set; the rest then take their defaults. */
bool bare__resolve_glass(BareGlass *g) {
    bool on = g->refraction != 0 || g->depth != 0 || g->blur != 0 || g->dispersion != 0 ||
              g->shine != 0 || g->merge != 0 || g->zoom != 0 || g->light != 0 || g->tint.a != 0;
    if (!on) return false;
    g->refraction = def(g->refraction, 1.0f);
    g->depth = def(g->depth, 20.0f);
    g->blur = def(g->blur, 2.0f);
    g->dispersion = def(g->dispersion, 1.0f);
    g->shine = def(g->shine, 1.0f);
    g->merge = BARE__MAX(g->merge, 0.0f);
    g->zoom = g->zoom > 0 ? g->zoom : 1.0f;
    return true;
}

void bare__resolve_box(BareBox *b) {
    b->opacity = def(b->opacity, 1.0f);
    b->scale = def(b->scale, 1.0f);
    b->grow = def(b->grow, 1.0f);
    b->shrink = def(b->shrink, 1.0f);
    if (b->pad.top == 0 && b->pad.right == 0 && b->pad.bottom == 0 && b->pad.left == 0) {
        b->pad.top = b->pad.right = b->pad.bottom = b->pad.left = b->padding;
    }
    if (b->opacity < 0) b->opacity = 0;
    if (b->opacity > 1) b->opacity = 1;
    if (b->border < 0) b->border = 0;
    if (b->radius < 0) b->radius = 0;
    if (b->scale < 0) b->scale = 0;
}

/* A look field replaces the box's when set (BARE_ZERO and BARE_CLEAR count as set). */
static float look_f(float base, float v) { return (v != 0.0f || signbit(v)) ? v : base; }

static void look_color(BareColor *dst, BareColor c) {
    if (c.r || c.g || c.b || c.a) *dst = c;
}

void bare__apply_look(BareBox *b, const BareLook *l) {
    look_color(&b->color, l->color);
    look_color(&b->border_color, l->border_color);
    look_color(&b->text_color, l->text_color);
    if (is_set(&l->shadow, sizeof l->shadow)) b->shadow = l->shadow;
    b->opacity = look_f(b->opacity, l->opacity);
    b->scale = look_f(b->scale, l->scale);
    b->radius = look_f(b->radius, l->radius);
    b->offset.x += l->offset.x;   /* a nudge on top of the element's own offset */
    b->offset.y += l->offset.y;
}

/* ---- themes -------------------------------------------------------------------- */

#define EASE        { .kind = BARE_ANIM_EASE, .duration = 0.2f, .bezier = { 0, 0, 0.58f, 1 } }
#define EASE_LOOKS  .animate = EASE, .animate_layout = { .kind = BARE_ANIM_EASE, .duration = BARE_ZERO }

/* Static constants, written with braces (compound literals aren't constant
 * expressions). Looks animate, layout doesn't: a theme switch fades, a
 * window resize doesn't lag. */
static const BareTheme dark = {
    .background = { 24, 24, 28, 255 },
    .surface = { 36, 38, 46, 255 },
    .text = { 255, 255, 255, 255 },
    .muted = { 150, 156, 172, 255 },
    .accent = { 40, 120, 255, 255 },
    .border = { 255, 255, 255, 26 },
    .selection = { 40, 120, 255, 110 },
    .panel = { .padding = 20, .gap = 12, .radius = 16, .color = { 30, 32, 40, 255 },
               .border = 1, .border_color = { 255, 255, 255, 14 }, EASE_LOOKS },
    .card = { .padding = 20, .gap = 10, .radius = 16, .color = { 36, 38, 46, 255 },
              .shadow = { 0, 8, 24, 0, { 0, 0, 0, 70 } }, EASE_LOOKS },
    .button = { .pad = { 10, 18, 10, 18 }, .gap = 8, .radius = 10, .align = BARE_CENTER, .justify = BARE_CENTER,
                .color = { 52, 56, 68, 255 }, .text_color = { 235, 238, 245, 255 }, .cursor = BARE_CURSOR_HAND,
                .hover = { .color = { 66, 70, 84, 255 } }, .press = { .scale = 0.97f }, EASE_LOOKS },
    .primary = { .pad = { 10, 18, 10, 18 }, .gap = 8, .radius = 10, .align = BARE_CENTER, .justify = BARE_CENTER,
                 .color = { 40, 120, 255, 255 }, .text_color = { 255, 255, 255, 255 }, .cursor = BARE_CURSOR_HAND,
                 .hover = { .color = { 70, 145, 255, 255 } }, .press = { .scale = 0.97f }, EASE_LOOKS },
    .row = { .pad = { 10, 14, 10, 14 }, .gap = 12, .radius = 10, .align = BARE_CENTER, .color = { 255, 255, 255, 0 },
             .cursor = BARE_CURSOR_HAND, .hover = { .color = { 255, 255, 255, 14 } }, EASE_LOOKS },
    .input = { .pad = { 9, 12, 9, 12 }, .radius = 8, .color = { 22, 24, 30, 255 }, .border = 1,
               .border_color = { 255, 255, 255, 30 }, .text_color = { 235, 238, 245, 255 },
               .hover = { .border_color = { 255, 255, 255, 52 } },
               .focus = { .border_color = { 40, 120, 255, 255 }, .shadow = { 0, 0, 0, 3, { 40, 120, 255, 70 } } },
               EASE_LOOKS },
    .title = { .size = 32, .line_height = 1.2f, .animate = EASE },
    .heading = { .size = 20, .line_height = 1.25f, .animate = EASE },
    .body = { .size = 16, .animate = EASE },
    .caption = { .size = 13, .color = { 150, 156, 172, 255 }, .animate = EASE },
};

static const BareTheme light = {
    .background = { 244, 245, 248, 255 },
    .surface = { 255, 255, 255, 255 },
    .text = { 24, 26, 32, 255 },
    .muted = { 104, 110, 124, 255 },
    .accent = { 30, 100, 240, 255 },
    .border = { 0, 0, 0, 22 },
    .selection = { 30, 100, 240, 70 },
    .panel = { .padding = 20, .gap = 12, .radius = 16, .color = { 255, 255, 255, 255 },
               .border = 1, .border_color = { 0, 0, 0, 18 }, EASE_LOOKS },
    .card = { .padding = 20, .gap = 10, .radius = 16, .color = { 255, 255, 255, 255 },
              .shadow = { 0, 6, 20, 0, { 20, 30, 60, 36 } }, EASE_LOOKS },
    .button = { .pad = { 10, 18, 10, 18 }, .gap = 8, .radius = 10, .align = BARE_CENTER, .justify = BARE_CENTER,
                .color = { 230, 233, 239, 255 }, .text_color = { 24, 26, 32, 255 }, .cursor = BARE_CURSOR_HAND,
                .hover = { .color = { 218, 222, 230, 255 } }, .press = { .scale = 0.97f }, EASE_LOOKS },
    .primary = { .pad = { 10, 18, 10, 18 }, .gap = 8, .radius = 10, .align = BARE_CENTER, .justify = BARE_CENTER,
                 .color = { 30, 100, 240, 255 }, .text_color = { 255, 255, 255, 255 }, .cursor = BARE_CURSOR_HAND,
                 .hover = { .color = { 55, 125, 255, 255 } }, .press = { .scale = 0.97f }, EASE_LOOKS },
    .row = { .pad = { 10, 14, 10, 14 }, .gap = 12, .radius = 10, .align = BARE_CENTER, .color = { 0, 0, 0, 0 },
             .cursor = BARE_CURSOR_HAND, .hover = { .color = { 0, 0, 0, 12 } }, EASE_LOOKS },
    .input = { .pad = { 9, 12, 9, 12 }, .radius = 8, .color = { 255, 255, 255, 255 }, .border = 1,
               .border_color = { 0, 0, 0, 40 }, .text_color = { 24, 26, 32, 255 },
               .hover = { .border_color = { 0, 0, 0, 70 } },
               .focus = { .border_color = { 30, 100, 240, 255 }, .shadow = { 0, 0, 0, 3, { 30, 100, 240, 55 } } },
               EASE_LOOKS },
    .title = { .size = 32, .line_height = 1.2f, .animate = EASE },
    .heading = { .size = 20, .line_height = 1.25f, .animate = EASE },
    .body = { .size = 16, .animate = EASE },
    .caption = { .size = 13, .color = { 104, 110, 124, 255 }, .animate = EASE },
};

const BareTheme *bare_theme_dark(void) { return &dark; }
const BareTheme *bare_theme_light(void) { return &light; }

const BareTheme *bare_theme(void) {
    BareContext *ctx = bare_ctx_current();
    return ctx ? &ctx->theme : &dark;
}

void bare_set_theme(const BareTheme *theme) {
    BareContext *ctx = bare_ctx_current();
    if (ctx) ctx->theme = theme ? *theme : dark;
}
