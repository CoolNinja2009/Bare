/* renderer_sdl3gpu.c: BareRenderer on the SDL3 GPU API (ARCHITECTURE §10).
 *
 * Every rect, glyph and image becomes one instance of a quad drawn by
 * shaders/bare.glsl; consecutive instances that share a texture and scissor are
 * one draw call.
 *
 * Frames are drawn into an offscreen "scene" texture and then copied to the
 * window. Liquid glass needs that: before each glass command the scene so far
 * is copied into a mipmapped "backdrop" texture, which shaders/glass.glsl
 * samples and bends.
 *
 * Capture: if the environment variable BARE_CAPTURE names a .bmp file, every
 * frame is also saved there. Used to check rendering without screenshots (and,
 * in M9, to compare backends). */
#include "bare/bare_backend.h"
#include "bare_shaders.h"
#include <SDL3/SDL.h>
#include <string.h>

typedef struct Instance {
    float rect[4], radii[4], fill[4], border_color[4], shadow_color[4], shadow[4], uv[4], params[4];
} Instance;

typedef struct Batch {
    Uint32   first, count;
    Uint32   texture;          /* 0 = the white texture */
    int      glass;            /* >= 0: a glass draw (index into gpu.glass) instead of instances */
    SDL_Rect scissor;
} Batch;

/* Uniforms for one glass draw; layouts match shaders/glass.glsl (std140). */
typedef struct GlassVert { float viewport[4], bounds[4]; } GlassVert;
typedef struct GlassFrag {
    float shapes[8][4], radii[8], p0[4], p1[4], tint[4], shadow[4], shadow_color[4], target[4];
} GlassFrag;
typedef struct GlassDraw { GlassVert vert; GlassFrag frag; } GlassDraw;

typedef struct Texture {
    SDL_GPUTexture *tex;
    int w, h;
    BareTexFormat format;
} Texture;

#define MAX_CLIP_DEPTH 64

typedef struct GpuRenderer {
    SDL_GPUDevice           *device;
    SDL_Window              *window;
    SDL_GPUGraphicsPipeline *pipeline, *glass_pipeline;
    SDL_GPUTextureFormat     format;
    SDL_GPUSampler          *sampler, *mip_sampler;
    SDL_GPUTexture          *white;
    SDL_GPUTexture          *scene, *backdrop;
    Uint32                   scene_w, scene_h;

    SDL_GPUBuffer           *instance_buf;
    SDL_GPUTransferBuffer   *instance_xfer;
    Uint32                   instance_cap;       /* in instances */

    SDL_GPUTransferBuffer   *upload_xfer;
    Uint32                   upload_cap;         /* bytes */

    Instance  *instances; Uint32 instance_count, instances_alloc;
    Batch     *batches;   Uint32 batch_count, batches_alloc;
    GlassDraw *glass;     Uint32 glass_count, glass_alloc;
    Texture   *textures;  Uint32 texture_cap;

    const char              *capture_path;
    SDL_GPUTransferBuffer   *capture_xfer;
    Uint32                   capture_cap;
} GpuRenderer;

static GpuRenderer gpu;

/* ---- setup -------------------------------------------------------------- */

typedef struct ShaderCode {
    const unsigned char *spirv, *dxil, *msl;
    size_t spirv_size, dxil_size, msl_size;
    Uint32 uniforms, samplers;
} ShaderCode;

#define CODE(name, u, smp) { name##_spirv, name##_dxil, name##_msl, \
                             sizeof name##_spirv, sizeof name##_dxil, sizeof name##_msl, u, smp }

static SDL_GPUShader *make_shader(SDL_GPUShaderStage stage, ShaderCode c) {
    SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(gpu.device);
    SDL_GPUShaderCreateInfo info;
    SDL_zero(info);
    info.stage = stage;
    info.num_uniform_buffers = c.uniforms;
    info.num_samplers = c.samplers;
    if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        info.code = c.spirv;
        info.code_size = c.spirv_size;
        info.entrypoint = "main";
    } else if (formats & SDL_GPU_SHADERFORMAT_DXIL) {
        info.format = SDL_GPU_SHADERFORMAT_DXIL;
        info.code = c.dxil;
        info.code_size = c.dxil_size;
        info.entrypoint = "main";
    } else if (formats & SDL_GPU_SHADERFORMAT_MSL) {
        info.format = SDL_GPU_SHADERFORMAT_MSL;
        info.code = c.msl;
        info.code_size = c.msl_size;
        info.entrypoint = "main0";
    } else {
        SDL_Log("bare: no supported shader format");
        return NULL;
    }
    SDL_GPUShader *sh = SDL_CreateGPUShader(gpu.device, &info);
    if (!sh) SDL_Log("bare: SDL_CreateGPUShader failed: %s", SDL_GetError());
    return sh;
}

static SDL_GPUGraphicsPipeline *make_pipeline(ShaderCode vcode, ShaderCode fcode, bool instanced) {
    SDL_GPUShader *vs = make_shader(SDL_GPU_SHADERSTAGE_VERTEX, vcode);
    SDL_GPUShader *fs = make_shader(SDL_GPU_SHADERSTAGE_FRAGMENT, fcode);
    if (!vs || !fs) return NULL;

    SDL_GPUVertexBufferDescription vb;
    SDL_zero(vb);
    vb.slot = 0;
    vb.pitch = sizeof(Instance);
    vb.input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;

    SDL_GPUVertexAttribute attrs[8];
    for (Uint32 i = 0; i < 8; i++) {
        attrs[i].location = i;
        attrs[i].buffer_slot = 0;
        attrs[i].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attrs[i].offset = i * 16;
    }

    SDL_GPUColorTargetDescription target;
    SDL_zero(target);
    target.format = gpu.format;
    target.blend_state.enable_blend = true;     /* premultiplied alpha */
    target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(info);
    info.vertex_shader = vs;
    info.fragment_shader = fs;
    if (instanced) {
        info.vertex_input_state.vertex_buffer_descriptions = &vb;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attrs;
        info.vertex_input_state.num_vertex_attributes = 8;
    }
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
    SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(gpu.device, &info);
    SDL_ReleaseGPUShader(gpu.device, vs);
    SDL_ReleaseGPUShader(gpu.device, fs);
    if (!pipeline) SDL_Log("bare: pipeline creation failed: %s", SDL_GetError());
    return pipeline;
}

static SDL_GPUTexture *make_texture(int w, int h, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage,
                                    Uint32 levels) {
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = usage;
    info.width = (Uint32)w;
    info.height = (Uint32)h;
    info.layer_count_or_depth = 1;
    info.num_levels = levels;
    return SDL_CreateGPUTexture(gpu.device, &info);
}

/* Grows a transfer buffer to hold `need` bytes. */
static bool ensure_xfer(SDL_GPUTransferBuffer **buf, Uint32 *cap, Uint32 need, SDL_GPUTransferBufferUsage usage) {
    if (*buf && *cap >= need) return true;
    Uint32 size = *cap ? *cap : 64 * 1024;
    while (size < need) size *= 2;
    if (*buf) SDL_ReleaseGPUTransferBuffer(gpu.device, *buf);
    SDL_GPUTransferBufferCreateInfo info = { .usage = usage, .size = size };
    *buf = SDL_CreateGPUTransferBuffer(gpu.device, &info);
    *cap = *buf ? size : 0;
    return *buf != NULL;
}

static bool init(void *self, const BarePlatform *platform) {
    (void)self;
    gpu.window = platform->native(platform->self, BARE_NATIVE_SDL_WINDOW);
    if (!gpu.window) return false;
    bool debug = SDL_getenv("BARE_GPU_DEBUG") != NULL;
    gpu.device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL,
                                     debug, NULL);
    if (!gpu.device) {
        SDL_Log("bare: SDL_CreateGPUDevice failed: %s", SDL_GetError());
        return false;
    }
    if (!SDL_ClaimWindowForGPUDevice(gpu.device, gpu.window)) {
        SDL_Log("bare: SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        return false;
    }
    gpu.format = SDL_GetGPUSwapchainTextureFormat(gpu.device, gpu.window);
    ShaderCode bare_v = CODE(bare_vert, 1, 0), bare_f = CODE(bare_frag, 0, 1);
    ShaderCode glass_v = CODE(glass_vert, 1, 0), glass_f = CODE(glass_frag, 1, 1);
    gpu.pipeline = make_pipeline(bare_v, bare_f, true);
    gpu.glass_pipeline = make_pipeline(glass_v, glass_f, false);
    if (!gpu.pipeline || !gpu.glass_pipeline) return false;

    SDL_GPUSamplerCreateInfo si;
    SDL_zero(si);
    si.min_filter = SDL_GPU_FILTER_LINEAR;
    si.mag_filter = SDL_GPU_FILTER_LINEAR;
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    gpu.sampler = SDL_CreateGPUSampler(gpu.device, &si);
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;   /* trilinear for the frosted backdrop */
    si.max_lod = 16.0f;
    gpu.mip_sampler = SDL_CreateGPUSampler(gpu.device, &si);

    /* A 1x1 white texture so the sampler binding is always valid. */
    gpu.white = make_texture(1, 1, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, 1);
    if (!gpu.sampler || !gpu.mip_sampler || !gpu.white) return false;
    if (!ensure_xfer(&gpu.upload_xfer, &gpu.upload_cap, 4, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD)) return false;
    Uint8 *p = SDL_MapGPUTransferBuffer(gpu.device, gpu.upload_xfer, false);
    p[0] = p[1] = p[2] = p[3] = 255;
    SDL_UnmapGPUTransferBuffer(gpu.device, gpu.upload_xfer);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu.device);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = { .transfer_buffer = gpu.upload_xfer };
    SDL_GPUTextureRegion dst = { .texture = gpu.white, .w = 1, .h = 1, .d = 1 };
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);

    const char *capture = SDL_getenv("BARE_CAPTURE");
    gpu.capture_path = capture && *capture ? capture : NULL;
    return true;
}

static void shutdown_renderer(void *self) {
    (void)self;
    if (!gpu.device) return;
    SDL_WaitForGPUIdle(gpu.device);
    for (Uint32 i = 0; i < gpu.texture_cap; i++) {
        if (gpu.textures[i].tex) SDL_ReleaseGPUTexture(gpu.device, gpu.textures[i].tex);
    }
    SDL_free(gpu.textures);
    SDL_free(gpu.instances);
    SDL_free(gpu.batches);
    SDL_free(gpu.glass);
    if (gpu.scene) SDL_ReleaseGPUTexture(gpu.device, gpu.scene);
    if (gpu.backdrop) SDL_ReleaseGPUTexture(gpu.device, gpu.backdrop);
    if (gpu.capture_xfer) SDL_ReleaseGPUTransferBuffer(gpu.device, gpu.capture_xfer);
    if (gpu.instance_buf) SDL_ReleaseGPUBuffer(gpu.device, gpu.instance_buf);
    if (gpu.instance_xfer) SDL_ReleaseGPUTransferBuffer(gpu.device, gpu.instance_xfer);
    if (gpu.upload_xfer) SDL_ReleaseGPUTransferBuffer(gpu.device, gpu.upload_xfer);
    SDL_ReleaseGPUTexture(gpu.device, gpu.white);
    SDL_ReleaseGPUSampler(gpu.device, gpu.sampler);
    SDL_ReleaseGPUSampler(gpu.device, gpu.mip_sampler);
    SDL_ReleaseGPUGraphicsPipeline(gpu.device, gpu.pipeline);
    SDL_ReleaseGPUGraphicsPipeline(gpu.device, gpu.glass_pipeline);
    SDL_ReleaseWindowFromGPUDevice(gpu.device, gpu.window);
    SDL_DestroyGPUDevice(gpu.device);
    memset(&gpu, 0, sizeof gpu);
}

/* ---- building instances and batches ------------------------------------- */

static bool grow(void **items, Uint32 *cap, Uint32 need, size_t size) {
    if (need <= *cap) return true;
    Uint32 n = *cap ? *cap : 256;
    while (n < need) n *= 2;
    void *p = SDL_realloc(*items, n * size);
    if (!p) return false;
    *items = p;
    *cap = n;
    return true;
}

static void set4(float *dst, float a, float b, float c, float d) { dst[0] = a; dst[1] = b; dst[2] = c; dst[3] = d; }
static void color4(float *dst, BareColorF c) { set4(dst, c.r, c.g, c.b, c.a); }

static Instance *add_instance(Uint32 texture, const SDL_Rect *scissor) {
    if (!grow((void **)&gpu.instances, &gpu.instances_alloc, gpu.instance_count + 1, sizeof(Instance))) return NULL;
    Batch *b = gpu.batch_count ? &gpu.batches[gpu.batch_count - 1] : NULL;
    bool joinable = b && b->glass < 0 && memcmp(&b->scissor, scissor, sizeof *scissor) == 0;
    /* Untextured boxes (texture 0) join any batch with the same scissor. */
    if (joinable && texture == 0) {
        b->count++;
    } else if (joinable && (b->texture == texture || b->texture == 0)) {
        b->texture = texture;
        b->count++;
    } else {
        if (!grow((void **)&gpu.batches, &gpu.batches_alloc, gpu.batch_count + 1, sizeof(Batch))) return NULL;
        b = &gpu.batches[gpu.batch_count++];
        b->first = gpu.instance_count;
        b->count = 1;
        b->texture = texture;
        b->glass = -1;
        b->scissor = *scissor;
    }
    Instance *in = &gpu.instances[gpu.instance_count++];
    memset(in, 0, sizeof *in);
    return in;
}

static void add_glass(const BareGlassCmd *g, const SDL_Rect *scissor, const BareDrawList *list) {
    if (g->count <= 0) return;
    if (!grow((void **)&gpu.glass, &gpu.glass_alloc, gpu.glass_count + 1, sizeof(GlassDraw))) return;
    if (!grow((void **)&gpu.batches, &gpu.batches_alloc, gpu.batch_count + 1, sizeof(Batch))) return;
    GlassDraw *d = &gpu.glass[gpu.glass_count];
    memset(d, 0, sizeof *d);

    /* The quad covers every shape, the merge bulge and the shadow. */
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    int n = SDL_min(g->count, 8);
    for (int i = 0; i < n; i++) {
        BareRect r = g->shapes[i].rect;
        x0 = SDL_min(x0, r.x);
        y0 = SDL_min(y0, r.y);
        x1 = SDL_max(x1, r.x + r.w);
        y1 = SDL_max(y1, r.y + r.h);
        set4(d->frag.shapes[i], r.x, r.y, r.w, r.h);
        d->frag.radii[i] = g->shapes[i].radius;
    }
    float margin = 2.0f + g->merge * 0.5f;
    if (g->shadow_color.a > 0) {
        margin += g->shadow[3] + g->shadow[2] * 1.5f + SDL_max(SDL_fabsf(g->shadow[0]), SDL_fabsf(g->shadow[1]));
    }
    set4(d->vert.viewport, (float)list->width, (float)list->height, 0, 0);
    set4(d->vert.bounds, x0 - margin, y0 - margin, x1 - x0 + 2 * margin, y1 - y0 + 2 * margin);
    set4(d->frag.p0, (float)n, g->merge, g->depth, g->refraction);
    set4(d->frag.p1, g->blur, g->dispersion, g->shine, g->opacity);
    color4(d->frag.tint, g->tint);
    set4(d->frag.shadow, g->shadow[0], g->shadow[1], g->shadow[2], g->shadow[3]);
    color4(d->frag.shadow_color, g->shadow_color);
    set4(d->frag.target, (float)list->width, (float)list->height, 0, 0);

    Batch *b = &gpu.batches[gpu.batch_count++];
    b->first = b->count = 0;
    b->texture = 0;
    b->glass = (int)gpu.glass_count++;
    b->scissor = *scissor;
}

static SDL_Rect clip_rect(BareRect r, int w, int h) {
    int x0 = SDL_max((int)r.x, 0), y0 = SDL_max((int)r.y, 0);
    int x1 = SDL_min((int)(r.x + r.w), w), y1 = SDL_min((int)(r.y + r.h), h);
    SDL_Rect s = { x0, y0, SDL_max(x1 - x0, 0), SDL_max(y1 - y0, 0) };
    return s;
}

static void build(const BareDrawList *list) {
    gpu.instance_count = 0;
    gpu.batch_count = 0;
    gpu.glass_count = 0;
    SDL_Rect clips[MAX_CLIP_DEPTH];
    int depth = 0;
    clips[0] = (SDL_Rect){ 0, 0, list->width, list->height };

    for (int i = 0; i < list->cmd_count; i++) {
        const BareDrawCmd *c = &list->cmds[i];
        switch (c->kind) {
        case BARE_CMD_RECT: {
            const BareRectCmd *r = &c->as.rect;
            Instance *in = add_instance(0, &clips[depth]);
            if (!in) return;
            set4(in->rect, r->rect.x, r->rect.y, r->rect.w, r->rect.h);
            set4(in->radii, r->radius[0], r->radius[1], r->radius[2], r->radius[3]);
            color4(in->fill, r->fill);
            color4(in->border_color, r->border_color);
            color4(in->shadow_color, r->shadow_color);
            set4(in->shadow, r->shadow[0], r->shadow[1], r->shadow[2], r->shadow[3]);
            set4(in->params, r->border, 0, 0, 0);
            break;
        }
        case BARE_CMD_TEXT: {
            const BareTextCmd *t = &c->as.text;
            for (int g = 0; g < t->count; g++) {
                const BareGlyph *gl = &t->glyphs[g];
                Instance *in = add_instance(t->atlas, &clips[depth]);
                if (!in) return;
                set4(in->rect, gl->dst.x, gl->dst.y, gl->dst.w, gl->dst.h);
                set4(in->uv, gl->uv.x, gl->uv.y, gl->uv.x + gl->uv.w, gl->uv.y + gl->uv.h);
                color4(in->fill, t->color);
                set4(in->params, 0, 1, 0, 0);
            }
            break;
        }
        case BARE_CMD_IMAGE: {
            const BareImageCmd *im = &c->as.image;
            Instance *in = add_instance(im->texture, &clips[depth]);
            if (!in) return;
            set4(in->rect, im->rect.x, im->rect.y, im->rect.w, im->rect.h);
            set4(in->radii, im->radius[0], im->radius[1], im->radius[2], im->radius[3]);
            set4(in->uv, im->uv.x, im->uv.y, im->uv.x + im->uv.w, im->uv.y + im->uv.h);
            color4(in->fill, im->tint);
            set4(in->params, 0, 2, 0, 0);
            break;
        }
        case BARE_CMD_GLASS:
            add_glass(&c->as.glass, &clips[depth], list);
            break;
        case BARE_CMD_CLIP_PUSH:
            if (depth + 1 < MAX_CLIP_DEPTH) {
                depth++;
                clips[depth] = clip_rect(c->as.clip.rect, list->width, list->height);
            }
            break;
        case BARE_CMD_CLIP_POP:
            if (depth > 0) depth--;
            break;
        }
    }
}

/* ---- per-frame GPU work ------------------------------------------------- */

static Texture *texture_slot(Uint32 id) {
    if (!grow((void **)&gpu.textures, &gpu.texture_cap, id + 1, sizeof(Texture))) return NULL;
    return &gpu.textures[id];
}

/* Creates, resizes, fills or frees textures as the core asked. */
static void apply_uploads(SDL_GPUCopyPass *copy, const BareDrawList *list) {
    Uint32 total = 0;
    for (int i = 0; i < list->upload_count; i++) {
        const BareTextureUpload *u = &list->uploads[i];
        if (!u->destroy) total += (Uint32)(u->w * u->h * (u->format == BARE_TEX_R8 ? 1 : 4));
    }
    if (total && !ensure_xfer(&gpu.upload_xfer, &gpu.upload_cap, total, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD)) return;
    Uint8 *base = total ? SDL_MapGPUTransferBuffer(gpu.device, gpu.upload_xfer, true) : NULL;
    Uint32 offset = 0;
    for (int i = 0; i < list->upload_count; i++) {
        const BareTextureUpload *u = &list->uploads[i];
        Texture *t = texture_slot(u->texture);
        if (!t) continue;
        if (u->destroy) {
            if (t->tex) SDL_ReleaseGPUTexture(gpu.device, t->tex);
            memset(t, 0, sizeof *t);
            continue;
        }
        if (!t->tex || t->w != u->width || t->h != u->height || t->format != u->format) {
            if (t->tex) SDL_ReleaseGPUTexture(gpu.device, t->tex);
            SDL_GPUTextureFormat f = u->format == BARE_TEX_R8 ? SDL_GPU_TEXTUREFORMAT_R8_UNORM
                                                              : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            t->tex = make_texture(u->width, u->height, f, SDL_GPU_TEXTUREUSAGE_SAMPLER, 1);
            t->w = u->width;
            t->h = u->height;
            t->format = u->format;
        }
        int bpp = u->format == BARE_TEX_R8 ? 1 : 4;
        Uint32 row = (Uint32)(u->w * bpp);
        if (!base || !t->tex || !u->pixels || row == 0) continue;
        for (int y = 0; y < u->h; y++) {
            memcpy(base + offset + (Uint32)y * row, (const Uint8 *)u->pixels + (size_t)y * (size_t)u->stride, row);
        }
        SDL_GPUTextureTransferInfo src = { .transfer_buffer = gpu.upload_xfer, .offset = offset,
                                           .pixels_per_row = (Uint32)u->w, .rows_per_layer = (Uint32)u->h };
        SDL_GPUTextureRegion dst = { .texture = t->tex, .x = (Uint32)u->x, .y = (Uint32)u->y,
                                     .w = (Uint32)u->w, .h = (Uint32)u->h, .d = 1 };
        offset += row * (Uint32)u->h;
        SDL_UploadToGPUTexture(copy, &src, &dst, false);
    }
    if (base) SDL_UnmapGPUTransferBuffer(gpu.device, gpu.upload_xfer);
}

static bool upload_instances(SDL_GPUCopyPass *copy) {
    if (!gpu.instance_count) return true;
    if (gpu.instance_count > gpu.instance_cap) {
        Uint32 cap = gpu.instance_cap ? gpu.instance_cap : 1024;
        while (cap < gpu.instance_count) cap *= 2;
        if (gpu.instance_buf) SDL_ReleaseGPUBuffer(gpu.device, gpu.instance_buf);
        if (gpu.instance_xfer) SDL_ReleaseGPUTransferBuffer(gpu.device, gpu.instance_xfer);
        SDL_GPUBufferCreateInfo bi = { .usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = cap * (Uint32)sizeof(Instance) };
        SDL_GPUTransferBufferCreateInfo ti = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = bi.size };
        gpu.instance_buf = SDL_CreateGPUBuffer(gpu.device, &bi);
        gpu.instance_xfer = SDL_CreateGPUTransferBuffer(gpu.device, &ti);
        gpu.instance_cap = (gpu.instance_buf && gpu.instance_xfer) ? cap : 0;
        if (!gpu.instance_cap) return false;
    }
    Uint32 bytes = gpu.instance_count * (Uint32)sizeof(Instance);
    void *dst = SDL_MapGPUTransferBuffer(gpu.device, gpu.instance_xfer, true);
    if (!dst) return false;
    memcpy(dst, gpu.instances, bytes);
    SDL_UnmapGPUTransferBuffer(gpu.device, gpu.instance_xfer);
    SDL_GPUTransferBufferLocation src = { .transfer_buffer = gpu.instance_xfer, .offset = 0 };
    SDL_GPUBufferRegion region = { .buffer = gpu.instance_buf, .offset = 0, .size = bytes };
    SDL_UploadToGPUBuffer(copy, &src, &region, true);
    return true;
}

/* (Re)creates the scene and backdrop textures at the window's size. */
static bool ensure_targets(Uint32 w, Uint32 h) {
    if (gpu.scene && gpu.scene_w == w && gpu.scene_h == h) return true;
    if (gpu.scene) SDL_ReleaseGPUTexture(gpu.device, gpu.scene);
    if (gpu.backdrop) SDL_ReleaseGPUTexture(gpu.device, gpu.backdrop);
    Uint32 levels = 1;
    while ((SDL_max(w, h) >> levels) > 0) levels++;
    SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    gpu.scene = make_texture((int)w, (int)h, gpu.format, usage, 1);
    gpu.backdrop = make_texture((int)w, (int)h, gpu.format, usage, levels);
    gpu.scene_w = w;
    gpu.scene_h = h;
    if (!gpu.scene || !gpu.backdrop) SDL_Log("bare: offscreen targets failed: %s", SDL_GetError());
    return gpu.scene && gpu.backdrop;
}

static void blit(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *from, SDL_GPUTexture *to, Uint32 w, Uint32 h) {
    SDL_GPUBlitInfo info;
    SDL_zero(info);
    info.source.texture = from;
    info.source.w = w;
    info.source.h = h;
    info.destination.texture = to;
    info.destination.w = w;
    info.destination.h = h;
    info.load_op = SDL_GPU_LOADOP_DONT_CARE;
    info.filter = SDL_GPU_FILTER_NEAREST;
    SDL_BlitGPUTexture(cmd, &info);
}

static SDL_GPURenderPass *begin_scene_pass(SDL_GPUCommandBuffer *cmd, bool clear, BareColorF c) {
    SDL_GPUColorTargetInfo ct;
    SDL_zero(ct);
    ct.texture = gpu.scene;
    ct.clear_color = (SDL_FColor){ c.r, c.g, c.b, c.a };
    ct.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    return SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
}

static void save_capture(Uint32 w, Uint32 h) {
    void *pixels = SDL_MapGPUTransferBuffer(gpu.device, gpu.capture_xfer, false);
    if (!pixels) return;
    SDL_PixelFormat pf = gpu.format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ? SDL_PIXELFORMAT_BGRA32
                                                                            : SDL_PIXELFORMAT_RGBA32;
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, pf, pixels, (int)w * 4);
    if (s) {
        if (!SDL_SaveBMP(s, gpu.capture_path)) SDL_Log("bare: capture failed: %s", SDL_GetError());
        SDL_DestroySurface(s);
    }
    SDL_UnmapGPUTransferBuffer(gpu.device, gpu.capture_xfer);
}

static void render(void *self, const BareDrawList *list) {
    (void)self;
    build(list);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu.device);
    if (!cmd) return;

    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    apply_uploads(copy, list);
    bool have_instances = upload_instances(copy);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUTexture *swap = NULL;
    Uint32 sw = 0, sh = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, gpu.window, &swap, &sw, &sh) || !swap ||
        !ensure_targets(sw, sh)) {
        SDL_SubmitGPUCommandBuffer(cmd);   /* minimised: nothing to draw into */
        return;
    }

    float viewport[4] = { (float)sw, (float)sh, 0, 0 };
    SDL_GPURenderPass *pass = begin_scene_pass(cmd, true, list->clear);
    bool bound = false;   /* main pipeline bound in this pass */
    for (Uint32 i = 0; i < gpu.batch_count; i++) {
        const Batch *b = &gpu.batches[i];
        if (b->scissor.w <= 0 || b->scissor.h <= 0) continue;

        if (b->glass >= 0) {
            /* Snapshot what's behind the glass, with blurred levels for frost. */
            SDL_EndGPURenderPass(pass);
            blit(cmd, gpu.scene, gpu.backdrop, sw, sh);
            SDL_GenerateMipmapsForGPUTexture(cmd, gpu.backdrop);
            pass = begin_scene_pass(cmd, false, list->clear);
            bound = false;

            const GlassDraw *g = &gpu.glass[b->glass];
            SDL_BindGPUGraphicsPipeline(pass, gpu.glass_pipeline);
            SDL_PushGPUVertexUniformData(cmd, 0, &g->vert, sizeof g->vert);
            SDL_PushGPUFragmentUniformData(cmd, 0, &g->frag, sizeof g->frag);
            SDL_GPUTextureSamplerBinding tsb = { .texture = gpu.backdrop, .sampler = gpu.mip_sampler };
            SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
            SDL_SetGPUScissor(pass, &b->scissor);
            SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
            continue;
        }

        if (!have_instances) continue;
        if (!bound) {
            SDL_BindGPUGraphicsPipeline(pass, gpu.pipeline);
            SDL_PushGPUVertexUniformData(cmd, 0, viewport, sizeof viewport);
            bound = true;
        }
        SDL_GPUTexture *tex = gpu.white;
        if (b->texture && b->texture < gpu.texture_cap && gpu.textures[b->texture].tex)
            tex = gpu.textures[b->texture].tex;
        SDL_GPUTextureSamplerBinding tsb = { .texture = tex, .sampler = gpu.sampler };
        SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
        /* Offset the binding instead of using first_instance (portable). */
        SDL_GPUBufferBinding vb = { .buffer = gpu.instance_buf, .offset = b->first * (Uint32)sizeof(Instance) };
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
        SDL_SetGPUScissor(pass, &b->scissor);
        SDL_DrawGPUPrimitives(pass, 4, b->count, 0, 0);
    }
    SDL_EndGPURenderPass(pass);
    blit(cmd, gpu.scene, swap, sw, sh);

    if (gpu.capture_path && ensure_xfer(&gpu.capture_xfer, &gpu.capture_cap, sw * sh * 4,
                                        SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD)) {
        SDL_GPUCopyPass *dl = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion src = { .texture = gpu.scene, .w = sw, .h = sh, .d = 1 };
        SDL_GPUTextureTransferInfo dst = { .transfer_buffer = gpu.capture_xfer };
        SDL_DownloadFromGPUTexture(dl, &src, &dst);
        SDL_EndGPUCopyPass(dl);
        SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        if (fence) {
            SDL_WaitForGPUFences(gpu.device, true, &fence, 1);
            SDL_ReleaseGPUFence(gpu.device, fence);
            save_capture(sw, sh);
        }
        return;
    }
    SDL_SubmitGPUCommandBuffer(cmd);
}

BareRenderer bare__sdl3gpu_renderer(void) {
    BareRenderer r = { .self = &gpu, .init = init, .render = render, .shutdown = shutdown_renderer };
    return r;
}
