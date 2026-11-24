/* gpu.c: images, canvases, shaders and passes (ARCHITECTURE §9).
 *
 * The core never touches the GPU. It hands out IDs, checks what the app asks
 * for (with clear messages), and queues the work in call order; the queue goes
 * to the renderer with the next draw list. Two queues swap each frame: one
 * collects new calls while the renderer reads the other, so their bytes stay
 * valid and, once grown, no frame allocates. */
#include "internal.h"
#include <string.h>

#define MAX_PARAMS 4096   /* WebGL guarantees 224 vec4s (3584 bytes); SDL's uniform buffers hold 32 KB */

static BareContext *need_ctx(const char *what) {
    BareContext *ctx = bare_ctx_current();
    if (!ctx) bare__log(NULL, "%s called before bare_window", what);
    return ctx;
}

/* ---- the queue ----------------------------------------------------------- */

static BareGpuOp *push_op(BareContext *ctx, BareGpuOpKind kind) {
    BareGpuQueue *q = &ctx->gpu_pending;
    if (!bare__grow(&ctx->alloc, (void **)&q->ops, &q->cap, q->count + 1, sizeof *q->ops)) {
        bare__log(ctx, "out of memory queueing GPU work");
        return NULL;
    }
    BareGpuOp *op = &q->ops[q->count++];
    memset(op, 0, sizeof *op);
    op->kind = kind;
    return op;
}

/* Copies bytes into the queue; returns their offset + 1 (0 = nothing, -1 = out of memory). */
static intptr_t push_data(BareContext *ctx, const void *bytes, int size) {
    if (!bytes || size <= 0) return 0;
    BareGpuQueue *q = &ctx->gpu_pending;
    int at = (q->data_size + 15) & ~15;   /* keep params aligned for the renderer */
    if (!bare__grow(&ctx->alloc, (void **)&q->data, &q->data_cap, at + size, 1)) {
        bare__log(ctx, "out of memory queueing GPU data");
        return -1;
    }
    memcpy(q->data + at, bytes, (size_t)size);
    q->data_size = at + size;
    return (intptr_t)at + 1;
}

static const void *as_offset(intptr_t o) { return (const void *)o; }

static const void *resolve(const BareGpuQueue *q, const void *p) {
    intptr_t o = (intptr_t)p;
    return o > 0 ? q->data + (o - 1) : NULL;
}

void bare__gpu_publish(BareContext *ctx) {
    BareGpuQueue done = ctx->gpu_published;
    ctx->gpu_published = ctx->gpu_pending;
    ctx->gpu_pending = done;
    ctx->gpu_pending.count = 0;
    ctx->gpu_pending.data_size = 0;

    BareGpuQueue *q = &ctx->gpu_published;
    for (int i = 0; i < q->count; i++) {
        BareGpuOp *op = &q->ops[i];
        if (op->kind == BARE_GPU_IMAGE_UPLOAD) op->as.image.pixels = resolve(q, op->as.image.pixels);
        if (op->kind == BARE_GPU_PASS) op->as.pass.params = resolve(q, op->as.pass.params);
    }
    ctx->list.ops = q->ops;
    ctx->list.op_count = q->count;
}

void bare__gpu_free(BareContext *ctx) {
    BareGpuQueue *qs[2] = { &ctx->gpu_pending, &ctx->gpu_published };
    for (int i = 0; i < 2; i++) {
        bare__free(&ctx->alloc, qs[i]->ops, (size_t)qs[i]->cap * sizeof *qs[i]->ops);
        bare__free(&ctx->alloc, qs[i]->data, (size_t)qs[i]->data_cap);
    }
    bare__free(&ctx->alloc, ctx->images, (size_t)ctx->image_cap * sizeof *ctx->images);
    bare__free(&ctx->alloc, ctx->shaders, (size_t)ctx->shader_cap * sizeof *ctx->shaders);
}

/* ---- images ---------------------------------------------------------------- */

const BareImageRec *bare__image(const BareContext *ctx, BareImage id) {
    if (!ctx || id == 0 || id > (uint32_t)ctx->image_count) return NULL;
    const BareImageRec *r = &ctx->images[id - 1];
    return r->alive && !r->internal ? r : NULL;
}

static BareImage new_image(BareContext *ctx, int w, int h, BareFormat format, bool canvas) {
    int index;
    if (ctx->image_count > 0 && ctx->image_free > 0) {
        index = ctx->image_free - 1;
        ctx->image_free = ctx->images[index].next_free;
    } else {
        if (!bare__grow(&ctx->alloc, (void **)&ctx->images, &ctx->image_cap, ctx->image_count + 1,
                        sizeof *ctx->images))
            return 0;
        index = ctx->image_count++;
    }
    BareImageRec *r = &ctx->images[index];
    memset(r, 0, sizeof *r);
    r->width = w;
    r->height = h;
    r->format = format;
    r->canvas = canvas;
    r->alive = true;
    return (BareImage)(index + 1);
}

/* The renderer creates it from the core's uploads; no ops. */
uint32_t bare__image_reserve(BareContext *ctx, int w, int h) {
    BareImage id = new_image(ctx, w, h, BARE_RGBA8, false);
    if (id) ctx->images[id - 1].internal = true;
    return id;
}

static bool valid_size(BareContext *ctx, const char *what, int w, int h) {
    if (w > 0 && h > 0 && w <= 16384 && h <= 16384) return true;
    bare__log(ctx, "%s: size %dx%d is not between 1x1 and 16384x16384", what, w, h);
    return false;
}

BareImage bare_canvas_(const BareCanvas *c) {
    BareContext *ctx = need_ctx("bare_canvas");
    if (!ctx || !valid_size(ctx, "bare_canvas", c->width, c->height)) return 0;
    if (c->format < BARE_RGBA8 || c->format > BARE_RGBA32F) {
        bare__log(ctx, "bare_canvas: unknown format %d", (int)c->format);
        return 0;
    }
    BareGpuOp *op = push_op(ctx, BARE_GPU_IMAGE_CREATE);
    if (!op) return 0;
    BareImage id = new_image(ctx, c->width, c->height, c->format, true);
    if (!id) { ctx->gpu_pending.count--; return 0; }
    ctx->images[id - 1].bottom_up = c->bottom_up;
    op->as.image = (BareGpuImage){ .id = id, .width = c->width, .height = c->height, .format = c->format,
                                   .nearest = c->nearest, .repeat = c->repeat, .renderable = true };
    return id;
}

BareImage bare_image_from_pixels(int width, int height, const void *rgba8) {
    BareContext *ctx = need_ctx("bare_image_from_pixels");
    if (!ctx || !valid_size(ctx, "bare_image_from_pixels", width, height)) return 0;
    if (!rgba8) {
        bare__log(ctx, "bare_image_from_pixels: no pixels");
        return 0;
    }
    BareGpuOp *create = push_op(ctx, BARE_GPU_IMAGE_CREATE);
    if (!create) return 0;
    BareImage id = new_image(ctx, width, height, BARE_RGBA8, false);
    if (!id) { ctx->gpu_pending.count--; return 0; }
    BareGpuImage desc = { .id = id, .width = width, .height = height, .format = BARE_RGBA8 };
    create->as.image = desc;
    intptr_t data = push_data(ctx, rgba8, width * height * 4);
    BareGpuOp *upload = data > 0 ? push_op(ctx, BARE_GPU_IMAGE_UPLOAD) : NULL;   /* may move `create` */
    if (upload) {
        upload->as.image = desc;
        upload->as.image.pixels = as_offset(data);
    }
    return id;
}

bool bare_image_update(BareImage image, const void *rgba8) {
    BareContext *ctx = need_ctx("bare_image_update");
    const BareImageRec *r = bare__image(ctx, image);
    if (!r || r->canvas || !rgba8) {
        if (ctx) bare__log(ctx, "bare_image_update: %u isn't an image made from pixels", (unsigned)image);
        return false;
    }
    intptr_t data = push_data(ctx, rgba8, r->width * r->height * 4);
    BareGpuOp *upload = data > 0 ? push_op(ctx, BARE_GPU_IMAGE_UPLOAD) : NULL;
    if (!upload) return false;
    upload->as.image = (BareGpuImage){ .id = image, .width = r->width, .height = r->height, .format = BARE_RGBA8,
                                       .pixels = as_offset(data) };
    return true;
}

void bare_image_free(BareImage image) {
    BareContext *ctx = bare_ctx_current();
    if (!ctx || image == 0) return;
    if (!bare__image(ctx, image)) {
        bare__log(ctx, "bare_image_free: image %u isn't alive (freed twice?)", (unsigned)image);
        return;
    }
    BareGpuOp *op = push_op(ctx, BARE_GPU_IMAGE_DESTROY);
    if (op) op->as.image.id = image;
    BareImageRec *r = &ctx->images[image - 1];
    r->alive = false;
    r->next_free = ctx->image_free;
    ctx->image_free = (int)image;
}

BareVec2 bare_image_size(BareImage image) {
    const BareImageRec *r = bare__image(bare_ctx_current(), image);
    return r ? (BareVec2){ (float)r->width, (float)r->height } : (BareVec2){ 0, 0 };
}

/* ---- shaders ------------------------------------------------------------- */

static const BareShaderRec *shader_rec(const BareContext *ctx, BareShader id) {
    if (id == 0 || id > (uint32_t)ctx->shader_count) return NULL;
    const BareShaderRec *r = &ctx->shaders[id - 1];
    return r->alive ? r : NULL;
}

BareShader bare_shader(const BareShaderCode *code) {
    BareContext *ctx = need_ctx("bare_shader");
    if (!ctx) return 0;
    if (!code || !(code->spirv || code->dxil || code->msl) || code->samplers < 0 ||
        code->samplers > BARE_MAX_INPUTS || code->uniform_buffers < 0 || code->uniform_buffers > 1) {
        bare__log(ctx, "bare_shader: not valid shader code (use the header bare_add_shaders generates)");
        return 0;
    }
    int index;
    if (ctx->shader_count > 0 && ctx->shader_free > 0) {
        index = ctx->shader_free - 1;
        ctx->shader_free = ctx->shaders[index].next_free;
    } else {
        if (!bare__grow(&ctx->alloc, (void **)&ctx->shaders, &ctx->shader_cap, ctx->shader_count + 1,
                        sizeof *ctx->shaders))
            return 0;
        index = ctx->shader_count++;
    }
    BareGpuOp *op = push_op(ctx, BARE_GPU_SHADER_CREATE);
    if (!op) return 0;
    ctx->shaders[index] = (BareShaderRec){ .code = code, .alive = true };
    op->as.shader = (BareGpuShader){ .id = (uint32_t)(index + 1), .code = code };
    return (BareShader)(index + 1);
}

void bare_shader_free(BareShader shader) {
    BareContext *ctx = bare_ctx_current();
    if (!ctx || shader == 0) return;
    if (!shader_rec(ctx, shader)) {
        bare__log(ctx, "bare_shader_free: shader %u isn't alive (freed twice?)", (unsigned)shader);
        return;
    }
    BareGpuOp *op = push_op(ctx, BARE_GPU_SHADER_DESTROY);
    if (op) op->as.shader.id = shader;
    BareShaderRec *r = &ctx->shaders[shader - 1];
    r->alive = false;
    r->next_free = ctx->shader_free;
    ctx->shader_free = (int)shader;
}

/* ---- passes -------------------------------------------------------------- */

void bare_pass_(const BarePass *p, const char *file, int line) {
    BareContext *ctx = need_ctx("bare_pass");
    if (!ctx) return;
    const BareImageRec *target = bare__image(ctx, p->target);
    if (!target || !target->canvas) {
        bare__log(ctx, "bare_pass at %s:%d: .target must be a live canvas (from bare_canvas)", file, line);
        return;
    }
    for (int i = 0; i < BARE_MAX_TARGETS - 1; i++) {
        if (!p->extra_targets[i]) continue;
        if (i > 0 && !p->extra_targets[i - 1]) {
            bare__log(ctx, "bare_pass at %s:%d: extra targets must be contiguous", file, line);
            return;
        }
        const BareImageRec *t = bare__image(ctx, p->extra_targets[i]);
        if (!t || !t->canvas) {
            bare__log(ctx, "bare_pass at %s:%d: .extra_targets[%d] must be a live canvas", file, line, i);
            return;
        }
        if (t->width != target->width || t->height != target->height) {
            bare__log(ctx, "bare_pass at %s:%d: .extra_targets[%d] is %dx%d, .target is %dx%d; they must match",
                      file, line, i, t->width, t->height, target->width, target->height);
            return;
        }
        for (int j = -1; j < i; j++) {
            if (p->extra_targets[i] == (j < 0 ? p->target : p->extra_targets[j])) {
                bare__log(ctx, "bare_pass at %s:%d: a canvas is given twice as a target", file, line);
                return;
            }
        }
    }
    const BareShaderRec *sh = shader_rec(ctx, p->shader);
    if (!sh) {
        bare__log(ctx, "bare_pass at %s:%d: .shader isn't a live shader (from bare_shader)", file, line);
        return;
    }
    int count = 0;
    for (int i = 0; i < BARE_MAX_INPUTS; i++) {
        if (!p->inputs[i]) continue;
        if (!bare__image(ctx, p->inputs[i])) {
            bare__log(ctx, "bare_pass at %s:%d: input %d isn't a live image", file, line, i);
            return;
        }
        bool is_target = p->inputs[i] == p->target;
        for (int j = 0; j < BARE_MAX_TARGETS - 1; j++) is_target |= p->inputs[i] == p->extra_targets[j];
        if (is_target) {
            bare__log(ctx, "bare_pass at %s:%d: input %d is also a target; draw into a second canvas and swap",
                      file, line, i);
            return;
        }
        count = i + 1;
    }
    if (count > sh->code->samplers) {
        bare__log(ctx, "bare_pass at %s:%d: %d inputs given, the shader reads %d", file, line, count,
                  sh->code->samplers);
        return;
    }
    if (p->params_size < 0 || p->params_size > MAX_PARAMS || (p->params_size > 0 && !p->params)) {
        bare__log(ctx, "bare_pass at %s:%d: .params must be 1 to %d bytes (use BARE_PARAMS(var))", file, line,
                  MAX_PARAMS);
        return;
    }
    if (sh->code->uniform_buffers > 0 && p->params_size == 0) {
        bare__log(ctx, "bare_pass at %s:%d: the shader takes params; pass BARE_PARAMS(var)", file, line);
        return;
    }
    intptr_t params = push_data(ctx, p->params, p->params_size);
    if (params < 0) return;
    BareGpuOp *op = push_op(ctx, BARE_GPU_PASS);
    if (!op) return;
    BareGpuPass *g = &op->as.pass;
    g->target = p->target;
    for (int i = 0; i < BARE_MAX_TARGETS - 1; i++) g->extra_targets[i] = p->extra_targets[i];
    g->shader = p->shader;
    for (int i = 0; i < BARE_MAX_INPUTS; i++) g->inputs[i] = p->inputs[i];
    g->input_count = sh->code->samplers;   /* the renderer binds every sampler the shader declares */
    g->params = as_offset(params);
    g->params_size = p->params_size;
    g->blend = p->blend;
}
