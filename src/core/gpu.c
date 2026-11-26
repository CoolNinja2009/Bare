/* gpu.c: images, canvases, shaders, passes and draws (ARCHITECTURE §9).
 *
 * The core never touches the GPU. It hands out IDs, checks what the app asks
 * for (with clear messages), and queues the work in call order; the queue goes
 * to the renderer with the next draw list. Two queues swap each frame: one
 * collects new calls while the renderer reads the other, so their bytes stay
 * valid and, once grown, no frame allocates. */
#include "internal.h"
#include <math.h>
#include <string.h>

#define MAX_PARAMS 4096   /* WebGL guarantees 224 vec4s (3584 bytes); SDL's uniform buffers hold 32 KB */
#define MAX_DRAW_DATA (64 << 20)   /* vertex + instance bytes in one draw */

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
    if (done.data_cap > (4 << 20)) {   /* room a burst of pictures needed once isn't kept */
        bare__free(&ctx->alloc, done.data, (size_t)done.data_cap);
        ctx->gpu_pending.data = NULL;
        ctx->gpu_pending.data_cap = 0;
    }

    BareGpuQueue *q = &ctx->gpu_published;
    for (int i = 0; i < q->count; i++) {
        BareGpuOp *op = &q->ops[i];
        if (op->kind == BARE_GPU_IMAGE_UPLOAD) op->as.image.pixels = resolve(q, op->as.image.pixels);
        if (op->kind == BARE_GPU_PASS || op->kind == BARE_GPU_DRAW) op->as.pass.params = resolve(q, op->as.pass.params);
        if (op->kind == BARE_GPU_DRAW) {
            op->as.pass.vertices = resolve(q, op->as.pass.vertices);
            op->as.pass.instances = resolve(q, op->as.pass.instances);
        }
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
    if (c->format < BARE_RGBA8 || c->format > BARE_DEPTH) {
        bare__log(ctx, "bare_canvas: unknown format %d", (int)c->format);
        return 0;
    }
    BareGpuOp *op = push_op(ctx, BARE_GPU_IMAGE_CREATE);
    if (!op) return 0;
    BareImage id = new_image(ctx, c->width, c->height, c->format, true);
    if (!id) { ctx->gpu_pending.count--; return 0; }
    ctx->images[id - 1].bottom_up = c->bottom_up;
    bool mipmaps = c->mipmaps;
    if (mipmaps && c->format == BARE_DEPTH) {
        bare__log(ctx, "bare_canvas: a BARE_DEPTH canvas can't have .mipmaps (ignored)");
        mipmaps = false;
    }
    op->as.image = (BareGpuImage){ .id = id, .width = c->width, .height = c->height, .format = c->format,
                                   .nearest = c->nearest, .repeat = c->repeat, .renderable = true,
                                   .mipmaps = mipmaps };
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

BareImage bare_image_frames(int width, int height, int frames, const void *rgba8) {
    BareContext *ctx = need_ctx("bare_image_frames");
    if (frames < 1) frames = 1;
    if (!ctx || !valid_size(ctx, "bare_image_frames", width, height * frames)) return 0;
    BareImage id = bare_image_from_pixels(width, height * frames, rgba8);
    if (id) ctx->images[id - 1].height = height, ctx->images[id - 1].frames = frames;
    return id;
}

void bare_image_frame(BareImage image, int frame) {
    BareContext *ctx = bare_ctx_current();
    const BareImageRec *r = bare__image(ctx, image);
    if (!r || r->frames < 2) return;
    ctx->images[image - 1].frame = frame < 0 ? 0 : frame >= r->frames ? r->frames - 1 : frame;
}

bool bare_image_update(BareImage image, const void *rgba8) {
    BareContext *ctx = need_ctx("bare_image_update");
    const BareImageRec *r = bare__image(ctx, image);
    if (!r || r->canvas || !rgba8) {
        if (ctx) bare__log(ctx, "bare_image_update: %u isn't an image made from pixels", (unsigned)image);
        return false;
    }
    int h = r->height * (r->frames > 1 ? r->frames : 1);
    intptr_t data = push_data(ctx, rgba8, r->width * h * 4);
    BareGpuOp *upload = data > 0 ? push_op(ctx, BARE_GPU_IMAGE_UPLOAD) : NULL;
    if (!upload) return false;
    upload->as.image = (BareGpuImage){ .id = image, .width = r->width, .height = h, .format = BARE_RGBA8,
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
        code->samplers > BARE_MAX_INPUTS || code->uniform_buffers < 0 || code->uniform_buffers > 1 ||
        code->vert_uniform_buffers < 0 || code->vert_uniform_buffers > 1 || code->vert_inputs < 0 ||
        code->vert_inputs > 16 || code->vert_samplers < 0 || code->vert_samplers > BARE_MAX_INPUTS) {
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

/* ---- passes and draws ------------------------------------------------------ */

static bool is_program(const BareShaderCode *c) { return c->vert_spirv[0] || c->vert_dxil[0] || c->vert_msl[0]; }

/* What passes and draws share: targets, shader, inputs, params. Returns the
 * shader, or NULL after logging what's wrong. */
static const BareShaderRec *check_common(BareContext *ctx, const char *what, const char *file, int line,
                                         BareImage target_id, const BareImage *extra_targets, BareShader shader,
                                         const BareImage *inputs, const void *params, int params_size) {
    const BareImageRec *target = bare__image(ctx, target_id);
    if (!target || !target->canvas) {
        bare__log(ctx, "%s at %s:%d: .target must be a live canvas (from bare_canvas)", what, file, line);
        return NULL;
    }
    if (target->format == BARE_DEPTH) {
        bare__log(ctx, "%s at %s:%d: .target is a depth canvas; draw with it as .depth", what, file, line);
        return NULL;
    }
    for (int i = 0; i < BARE_MAX_TARGETS - 1; i++) {
        if (!extra_targets[i]) continue;
        if (i > 0 && !extra_targets[i - 1]) {
            bare__log(ctx, "%s at %s:%d: extra targets must be contiguous", what, file, line);
            return NULL;
        }
        const BareImageRec *t = bare__image(ctx, extra_targets[i]);
        if (!t || !t->canvas || t->format == BARE_DEPTH) {
            bare__log(ctx, "%s at %s:%d: .extra_targets[%d] must be a live colour canvas (not BARE_DEPTH)", what,
                      file, line, i);
            return NULL;
        }
        if (t->width != target->width || t->height != target->height) {
            bare__log(ctx, "%s at %s:%d: .extra_targets[%d] is %dx%d, .target is %dx%d; they must match",
                      what, file, line, i, t->width, t->height, target->width, target->height);
            return NULL;
        }
        for (int j = -1; j < i; j++) {
            if (extra_targets[i] == (j < 0 ? target_id : extra_targets[j])) {
                bare__log(ctx, "%s at %s:%d: a canvas is given twice as a target", what, file, line);
                return NULL;
            }
        }
    }
    const BareShaderRec *sh = shader_rec(ctx, shader);
    if (!sh) {
        bare__log(ctx, "%s at %s:%d: .shader isn't a live shader (from bare_shader)", what, file, line);
        return NULL;
    }
    int count = 0;
    for (int i = 0; i < BARE_MAX_INPUTS; i++) {
        if (!inputs[i]) continue;
        if (!bare__image(ctx, inputs[i])) {
            bare__log(ctx, "%s at %s:%d: input %d isn't a live image", what, file, line, i);
            return NULL;
        }
        bool is_target = inputs[i] == target_id;
        for (int j = 0; j < BARE_MAX_TARGETS - 1; j++) is_target |= inputs[i] == extra_targets[j];
        if (is_target) {
            bare__log(ctx, "%s at %s:%d: input %d is also a target; draw into a second canvas and swap",
                      what, file, line, i);
            return NULL;
        }
        count = i + 1;
    }
    int reads = sh->code->samplers > sh->code->vert_samplers ? sh->code->samplers : sh->code->vert_samplers;
    if (count > reads) {
        bare__log(ctx, "%s at %s:%d: %d inputs given, the shader reads %d", what, file, line, count, reads);
        return NULL;
    }
    if (params_size < 0 || params_size > MAX_PARAMS || (params_size > 0 && !params)) {
        bare__log(ctx, "%s at %s:%d: .params must be 1 to %d bytes (use BARE_PARAMS(var))", what, file, line,
                  MAX_PARAMS);
        return NULL;
    }
    if ((sh->code->uniform_buffers > 0 || sh->code->vert_uniform_buffers > 0) && params_size == 0) {
        bare__log(ctx, "%s at %s:%d: the shader takes params; pass BARE_PARAMS(var)", what, file, line);
        return NULL;
    }
    return sh;
}

static BareGpuPass *queue(BareContext *ctx, BareGpuOpKind kind, BareImage target, const BareImage *extra_targets,
                          BareShader shader, const BareImage *inputs, int samplers, intptr_t params,
                          int params_size, BareBlend blend) {
    BareGpuOp *op = push_op(ctx, kind);
    if (!op) return NULL;
    BareGpuPass *g = &op->as.pass;
    g->target = target;
    for (int i = 0; i < BARE_MAX_TARGETS - 1; i++) g->extra_targets[i] = extra_targets[i];
    g->shader = shader;
    for (int i = 0; i < BARE_MAX_INPUTS; i++) g->inputs[i] = inputs[i];
    g->input_count = samplers;   /* the renderer binds every sampler the shader declares */
    g->params = as_offset(params);
    g->params_size = params_size;
    g->blend = blend;
    return g;
}

static bool valid_blend(BareBlend b) { return b >= BARE_BLEND_NONE && b <= BARE_BLEND_ALPHA; }

void bare_pass_(const BarePass *p, const char *file, int line) {
    BareContext *ctx = need_ctx("bare_pass");
    if (!ctx) return;
    const BareShaderRec *sh = check_common(ctx, "bare_pass", file, line, p->target, p->extra_targets, p->shader,
                                           p->inputs, p->params, p->params_size);
    if (!sh) return;
    if (is_program(sh->code)) {
        bare__log(ctx, "bare_pass at %s:%d: .shader has a vertex half; draw with it (bare_draw)", file, line);
        return;
    }
    if (!valid_blend(p->blend)) {
        bare__log(ctx, "bare_pass at %s:%d: unknown .blend %d", file, line, (int)p->blend);
        return;
    }
    intptr_t params = push_data(ctx, p->params, p->params_size);
    if (params < 0) return;
    queue(ctx, BARE_GPU_PASS, p->target, p->extra_targets, p->shader, p->inputs, sh->code->samplers, params,
          p->params_size, p->blend);
}

void bare_draw_(const BareDraw *d, const char *file, int line) {
    BareContext *ctx = need_ctx("bare_draw");
    if (!ctx) return;
    const BareShaderRec *sh = check_common(ctx, "bare_draw", file, line, d->target, d->extra_targets, d->shader,
                                           d->inputs, d->params, d->params_size);
    if (!sh) return;
    const BareShaderCode *c = sh->code;
    if (!is_program(c)) {
        bare__log(ctx, "bare_draw at %s:%d: .shader has no vertex half (#ifdef BARE_VERTEX in its .glsl)", file, line);
        return;
    }
    if (!valid_blend(d->blend)) {
        bare__log(ctx, "bare_draw at %s:%d: unknown .blend %d", file, line, (int)d->blend);
        return;
    }
    if (d->depth || d->depth_test || d->depth_read_only || d->clear_depth) {
        const BareImageRec *z = bare__image(ctx, d->depth), *t = bare__image(ctx, d->target);
        if (!z || !z->canvas || z->format != BARE_DEPTH) {
            bare__log(ctx, "bare_draw at %s:%d: .depth must be a live canvas made with .format = BARE_DEPTH",
                      file, line);
            return;
        }
        if (z->width != t->width || z->height != t->height) {
            bare__log(ctx, "bare_draw at %s:%d: .depth is %dx%d, .target is %dx%d; they must match", file, line,
                      z->width, z->height, t->width, t->height);
            return;
        }
        for (int i = 0; i < BARE_MAX_INPUTS; i++) {
            if (d->inputs[i] == d->depth) {
                bare__log(ctx, "bare_draw at %s:%d: input %d is also .depth; copy it first", file, line, i);
                return;
            }
        }
        if (d->depth_test < BARE_DEPTH_LESS || d->depth_test > BARE_DEPTH_ALWAYS) {
            bare__log(ctx, "bare_draw at %s:%d: unknown .depth_test %d", file, line, (int)d->depth_test);
            return;
        }
    }
    if (d->vertex_count <= 0 && !d->clear && !d->clear_depth) return;   /* nothing to draw, as WebGL */
    if (d->instances && d->instance_count <= 0) return;
    int vslots = d->vertices ? d->vertex_size / 16 : 0, islots = d->instances ? d->instance_size / 16 : 0;
    if ((d->vertices && (d->vertex_size <= 0 || d->vertex_size % 16)) ||
        (d->instances && (d->instance_size <= 0 || d->instance_size % 16))) {
        bare__log(ctx, "bare_draw at %s:%d: .vertex_size and .instance_size must be multiples of 16 bytes "
                  "(each attribute a vec4 slot)", file, line);
        return;
    }
    if (vslots + islots != c->vert_inputs) {
        bare__log(ctx, "bare_draw at %s:%d: %d vertex and %d instance attributes given, the program reads %d",
                  file, line, vslots, islots, c->vert_inputs);
        return;
    }
    int64_t vbytes = d->vertices ? (int64_t)d->vertex_size * d->vertex_count : 0;
    int64_t ibytes = d->instances ? (int64_t)d->instance_size * d->instance_count : 0;
    if (vbytes + ibytes > MAX_DRAW_DATA) {
        bare__log(ctx, "bare_draw at %s:%d: more than %d MB of vertex and instance data", file, line,
                  MAX_DRAW_DATA >> 20);
        return;
    }
    intptr_t params = push_data(ctx, d->params, d->params_size);
    intptr_t verts = push_data(ctx, d->vertices, (int)vbytes);
    intptr_t insts = push_data(ctx, d->instances, (int)ibytes);
    if (params < 0 || verts < 0 || insts < 0) return;
    BareGpuPass *g = queue(ctx, BARE_GPU_DRAW, d->target, d->extra_targets, d->shader, d->inputs, c->samplers,
                           params, d->params_size, d->blend);
    if (!g) return;
    g->vertices = as_offset(verts);
    g->instances = as_offset(insts);
    g->vertex_size = d->vertices ? d->vertex_size : 0;
    g->vertex_count = d->vertex_count;
    g->instance_size = d->instances ? d->instance_size : 0;
    g->instance_count = d->instances ? d->instance_count : (d->instance_count > 0 ? d->instance_count : 1);
    g->strip = d->strip;
    g->clear = d->clear;
    g->flip = bare__image(ctx, d->target)->bottom_up;
    for (int i = 0; i < 4; i++) g->clear_color[i] = d->clear_color[i];
    if (d->depth) {
        g->depth = d->depth;
        g->depth_test = d->depth_test;
        g->depth_write = !d->depth_read_only;
        g->clear_depth = d->clear_depth;
        g->depth_clear = d->depth_clear == 0 && !signbit(d->depth_clear) ? 1.0f : d->depth_clear;
    }
}
