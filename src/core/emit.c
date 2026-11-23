/* emit.c: element tree -> draw list in physical pixels (ARCHITECTURE §9). */
#include "internal.h"
#include <math.h>

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
    gc->tint = color_f(g->tint, 1.0f);
    gc->opacity = e->opacity;
    shadow_of(&e->box, s, 1.0f, gc->shadow, &gc->shadow_color);
    *last_glass_cmd = ctx->cmd_count - 1;
    *last_glass_parent = e->parent;
}

void bare__emit(BareContext *ctx) {
    ctx->cmd_count = 0;
    int last_glass_cmd = -1, last_glass_parent = -2;
    float s = ctx->scale;
    for (int i = 0; i < ctx->elem_count; i++) {
        const BareElem *e = &ctx->elems[i];
        if (e->opacity <= 0) continue;
        BareRect r = place(e, s);
        if (r.w <= 0 || r.h <= 0) continue;
        bool image = e->box.image && bare__image(ctx, e->box.image);
        if (e->glass) {
            emit_glass(ctx, i, r, &last_glass_cmd, &last_glass_parent);
            emit_rect(ctx, e, r, false, true, !image);   /* frost colour and border on top of the glass */
        } else {
            emit_rect(ctx, e, r, true, true, !image);
        }
        if (image) {   /* over the background, under the border */
            emit_image(ctx, e, r);
            emit_rect(ctx, e, r, false, false, true);
        }
    }

    BareDrawList *l = &ctx->list;
    l->uploads = NULL;
    l->upload_count = 0;
    l->cmds = ctx->cmds;
    l->cmd_count = ctx->cmd_count;
    l->width = (int)roundf(ctx->width * ctx->scale);
    l->height = (int)roundf(ctx->height * ctx->scale);
    l->scale = ctx->scale;
    l->clear = color_f(ctx->clear, 1.0f);
}
