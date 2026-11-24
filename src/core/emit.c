/* emit.c: element tree -> draw list in physical pixels (ARCHITECTURE §9). */
#include "internal.h"
#include <math.h>
#include <string.h>

static const BareColor NONE = { 0, 0, 0, 0 };

static BareColorF color_f(BareColor c, float opacity) {
    BareColorF f = { c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (c.a / 255.0f) * opacity };
    return f;
}

static BareDrawCmd *push_cmd(BareContext *ctx) {
    if (!bare__grow(&ctx->alloc, (void **)&ctx->cmds, &ctx->cmd_cap, ctx->cmd_count + 1, sizeof *ctx->cmds)) {
        bare__log(ctx, "out of memory emitting draw commands");
        return NULL;
    }
    return &ctx->cmds[ctx->cmd_count++];
}

/* A plain filled rectangle (selections, carets). */
void bare__emit_fill(BareContext *ctx, BareRect r, BareColorF color) {
    if (r.w <= 0 || r.h <= 0 || color.a <= 0) return;
    BareDrawCmd *cmd = push_cmd(ctx);
    if (!cmd) return;
    cmd->kind = BARE_CMD_RECT;
    BareRectCmd *rc = &cmd->as.rect;
    memset(rc, 0, sizeof *rc);
    rc->rect = r;
    rc->fill = color;
}

/* Snaps the layout rect to whole physical pixels (edges, not sizes, so
 * neighbouring boxes never gap or overlap), then adds the visual offset
 * unsnapped so moving elements glide instead of stepping a pixel at a time.
 * Scaled and animating elements are in motion or deliberately off-grid: no
 * snapping (they land on the grid when they settle). */
static BareRect place(const BareElem *e, float s) {
    if (e->vscale != 1.0f || e->moving) {
        BareRect v = bare__visual_rect(e);
        BareRect o = { v.x * s, v.y * s, v.w * s, v.h * s };
        return o;
    }
    BareRect r = e->rect;
    float x0 = roundf(r.x * s), y0 = roundf(r.y * s);
    float x1 = roundf((r.x + r.w) * s), y1 = roundf((r.y + r.h) * s);
    BareRect o = { x0 + e->vt.x * s, y0 + e->vt.y * s, x1 - x0, y1 - y0 };
    return o;
}

static void shadow_of(const BareBox *b, float s, float op, float out[4], BareColorF *color) {
    if (b->shadow.color.a > 0) {
        out[0] = b->shadow.x * s;
        out[1] = b->shadow.y * s;
        out[2] = BARE__MAX(b->shadow.blur, 0.0f) * s;
        out[3] = b->shadow.spread * s;
        *color = color_f(b->shadow.color, op);
    } else {
        out[0] = out[1] = out[2] = out[3] = 0;
        *color = color_f(NONE, op);
    }
}

/* Background, border and (unless the glass draws it) shadow in one command. */
static void emit_rect(BareContext *ctx, const BareElem *e, BareRect r, bool with_shadow, bool with_fill,
                      bool with_border) {
    const BareBox *b = &e->box;
    float s = ctx->scale * e->vscale, op = e->opacity;
    bool fill = with_fill && b->color.a > 0;
    bool border = with_border && b->border > 0 && b->border_color.a > 0;
    bool shadow = with_shadow && b->shadow.color.a > 0;
    if (!(fill || border || shadow)) return;

    BareDrawCmd *cmd = push_cmd(ctx);
    if (!cmd) return;
    cmd->kind = BARE_CMD_RECT;
    BareRectCmd *rc = &cmd->as.rect;
    float half = BARE__MIN(r.w, r.h) * 0.5f;
    float radius = BARE__MIN(b->radius * s, half);
    rc->rect = r;
    rc->fill = color_f(fill ? b->color : NONE, op);
    rc->border_color = color_f(border ? b->border_color : NONE, op);
    rc->border = border ? BARE__MIN(b->border * s, half) : 0;
    for (int k = 0; k < 4; k++) rc->radius[k] = radius;
    if (shadow) shadow_of(b, s, op, rc->shadow, &rc->shadow_color);
    else rc->shadow_color = color_f(NONE, op);
}

/* An image stretched over the box, inside its rounded corners. */
static void emit_image(BareContext *ctx, const BareElem *e, BareRect r) {
    BareDrawCmd *cmd = push_cmd(ctx);
    if (!cmd) return;
    cmd->kind = BARE_CMD_IMAGE;
    BareImageCmd *im = &cmd->as.image;
    float radius = BARE__MIN(e->box.radius * ctx->scale * e->vscale, BARE__MIN(r.w, r.h) * 0.5f);
    im->texture = e->box.image;
    im->rect = r;
    im->uv = (BareRect){ 0, 0, 1, 1 };
    for (int k = 0; k < 4; k++) im->radius[k] = radius;
    im->tint = (BareColorF){ 1, 1, 1, e->opacity };
}

/* Glass siblings with .merge melt together: consecutive ones join the
 * previous glass command instead of starting a new one. */
static void emit_glass(BareContext *ctx, int index, BareRect r, int *last_glass_cmd, int *last_glass_parent) {
    const BareElem *e = &ctx->elems[index];
    const BareGlass *g = &e->box.glass;
    float s = ctx->scale * e->vscale;
    BareGlassShape shape = { r, BARE__MIN(e->box.radius * s, BARE__MIN(r.w, r.h) * 0.5f) };

    if (g->merge > 0 && *last_glass_cmd == ctx->cmd_count - 1 && *last_glass_parent == e->parent) {
        BareGlassCmd *prev = &ctx->cmds[*last_glass_cmd].as.glass;
        if (prev->merge > 0 && prev->count < BARE_GLASS_MAX_SHAPES) {
            ((BareGlassShape *)prev->shapes)[prev->count++] = shape;
            prev->merge = BARE__MAX(prev->merge, g->merge * s);
            return;
        }
    }

    BareGlassShape *shapes = bare__arena_alloc(&ctx->frame, sizeof *shapes * BARE_GLASS_MAX_SHAPES, 16);
    BareDrawCmd *cmd = shapes ? push_cmd(ctx) : NULL;
    if (!cmd) return;
    shapes[0] = shape;
    cmd->kind = BARE_CMD_GLASS;
    BareGlassCmd *gc = &cmd->as.glass;
    gc->shapes = shapes;
    gc->count = 1;
    gc->refraction = g->refraction;
    gc->depth = g->depth * s;
    gc->blur = g->blur * s;
    gc->dispersion = g->dispersion;
    gc->shine = g->shine;
    gc->merge = g->merge * s;
    gc->zoom = g->zoom > 0 ? g->zoom : 1.0f;
    gc->light = g->light * 0.017453293f;
    gc->amount = e->glass_amount;
    gc->tint = color_f(g->tint, 1.0f);
    gc->opacity = e->opacity;
    shadow_of(&e->box, s, 1.0f, gc->shadow, &gc->shadow_color);
    *last_glass_cmd = ctx->cmd_count - 1;
    *last_glass_parent = e->parent;
}

static BareRect intersect(BareRect a, BareRect b) {
    float x0 = fmaxf(a.x, b.x), y0 = fmaxf(a.y, b.y);
    float x1 = fminf(a.x + a.w, b.x + b.w), y1 = fminf(a.y + a.h, b.y + b.h);
    BareRect r = { x0, y0, fmaxf(x1 - x0, 0.0f), fmaxf(y1 - y0, 0.0f) };
    return r;
}

static bool overlaps(BareRect a, BareRect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

/* Where an element can draw: its rect grown by its shadow (physical pixels). */
static BareRect ink_bounds(const BareElem *e, BareRect r, float s) {
    const BareShadow *sh = &e->box.shadow;
    if (sh->color.a == 0) return r;
    float g = (BARE__MAX(sh->blur, 0.0f) * 3 + BARE__MAX(sh->spread, 0.0f) +
               BARE__MAX(fabsf(sh->x), fabsf(sh->y))) * s * e->vscale;
    BareRect o = { r.x - g, r.y - g, r.w + 2 * g, r.h + 2 * g };
    return o;
}

static void push_clip(BareContext *ctx, BareRect r) {
    BareDrawCmd *cmd = push_cmd(ctx);
    if (!cmd) return;
    cmd->kind = BARE_CMD_CLIP_PUSH;
    cmd->as.clip.rect = r;
}

static void pop_clip(BareContext *ctx) {
    BareDrawCmd *cmd = push_cmd(ctx);
    if (cmd) cmd->kind = BARE_CMD_CLIP_POP;
}

/* A box's own commands: glass, background, image, border. */
static void emit_box(BareContext *ctx, int i, BareRect r, int *last_glass_cmd, int *last_glass_parent) {
    const BareElem *e = &ctx->elems[i];
    bool image = e->box.image && bare__image(ctx, e->box.image);
    if (e->glass) {
        emit_glass(ctx, i, r, last_glass_cmd, last_glass_parent);
        emit_rect(ctx, e, r, false, true, !image);   /* frost colour and border on top of the glass */
    } else {
        emit_rect(ctx, e, r, true, true, !image);
    }
    if (image) {   /* over the background, under the border */
        emit_image(ctx, e, r);
        emit_rect(ctx, e, r, false, false, true);
    }
}

void bare__emit(BareContext *ctx) {
    ctx->cmd_count = 0;
    bare__text_begin_emit(ctx);
    int last_glass_cmd = -1, last_glass_parent = -2;
    float s = ctx->scale;
    int n = ctx->elem_count;
    /* Text colour, inherited down the tree (white unless a .text_color says otherwise). */
    BareColor *ink = bare__arena_alloc(&ctx->frame, sizeof *ink * (size_t)(n + 1), 16);
    /* Clips: a clipping box's clip stays pushed until its last descendant
     * (pre-order: the subtree of i is i..end[i]). Each pushed rect is already
     * intersected with the enclosing one, in whole physical pixels. */
    int *end = bare__arena_alloc(&ctx->frame, sizeof *end * (size_t)(n + 1), 16);
    int *open_end = bare__arena_alloc(&ctx->frame, sizeof *open_end * (BARE__MAX_DEPTH + 2), 16);
    BareRect *open_rect = bare__arena_alloc(&ctx->frame, sizeof *open_rect * (BARE__MAX_DEPTH + 2), 16);
    if (!end || !open_end || !open_rect) {
        bare__log(ctx, "out of memory emitting draw commands");
        return;
    }
    for (int i = 0; i < n; i++) end[i] = i;
    for (int i = n - 1; i > 0; i--) {
        int p = ctx->elems[i].parent;
        if (p >= 0 && end[i] > end[p]) end[p] = end[i];
    }
    int depth = 0;
    open_rect[0] = (BareRect){ -1e30f, -1e30f, 2e30f, 2e30f };

    for (int i = 0; i < n; i++) {
        const BareElem *e = &ctx->elems[i];
        while (depth > 0 && open_end[depth] < i) { pop_clip(ctx); depth--; }
        if (ink) {
            BareColor up = e->parent >= 0 ? ink[e->parent] : ctx->theme.text;
            ink[i] = e->box.text_color.a ? e->box.text_color : up;
        }
        if (e->opacity <= 0) continue;   /* its children are transparent too */
        BareRect r = place(e, s);
        bool clips = (e->box.clip || e->box.scroll) && end[i] > i;
        BareRect clip = open_rect[depth];
        if (clips) {
            BareRect c = bare__clip_rect(e);
            float x0 = roundf(c.x * s), y0 = roundf(c.y * s);
            float x1 = roundf((c.x + c.w) * s), y1 = roundf((c.y + c.h) * s);
            clip = intersect(open_rect[depth], (BareRect){ x0, y0, x1 - x0, y1 - y0 });
        }
        bool seen = r.w > 0 && r.h > 0 && overlaps(ink_bounds(e, r, s), open_rect[depth]);
        if (seen && e->text) {
            bool snap = e->vscale == 1.0f && !e->moving;
            BareColorF tc = color_f(ink ? ink[i] : ctx->theme.text, e->opacity);
            float k = s * e->vscale;
            if (e->text->deco) bare__field_emit_under(ctx, e->text, r, k, snap, e->opacity);   /* selection */
            bare__text_emit(ctx, e->text, r, e->vscale, snap, tc, e->opacity);
            if (e->text->deco) bare__field_emit_over(ctx, e->text, r, k, snap, tc);           /* caret */
            continue;
        }
        if (seen) emit_box(ctx, i, r, &last_glass_cmd, &last_glass_parent);
        if (clips) {
            if (clip.w <= 0 || clip.h <= 0) {   /* nothing inside can show */
                i = end[i];
                continue;
            }
            if (depth + 1 < BARE__MAX_DEPTH + 2) {
                push_clip(ctx, clip);
                depth++;
                open_end[depth] = end[i];
                open_rect[depth] = clip;
            }
        }
    }
    while (depth > 0) { pop_clip(ctx); depth--; }

    BareDrawList *l = &ctx->list;
    bare__text_end_frame(ctx);   /* the atlas upload */
    l->cmds = ctx->cmds;
    l->cmd_count = ctx->cmd_count;
    l->width = (int)roundf(ctx->width * ctx->scale);
    l->height = (int)roundf(ctx->height * ctx->scale);
    l->scale = ctx->scale;
    l->clear = color_f(ctx->clear_set ? ctx->clear : ctx->theme.background, 1.0f);
}
