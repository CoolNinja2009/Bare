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
#include "backend/dxbc.h"
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
    float shapes[8][4], radii[8], p0[4], p1[4], tint[4], shadow[4], shadow_color[4], target[4], p2[4];
} GlassFrag;
typedef struct GlassDraw { GlassVert vert; GlassFrag frag; } GlassDraw;

typedef struct Texture {
    SDL_GPUTexture      *tex;
    int                  w, h;
    BareTexFormat        format;        /* glyph atlas uploads */
    SDL_GPUTextureFormat gpu_format;    /* images and canvases */
    SDL_GPUSampler      *sampler;       /* NULL = the default (bilinear, clamped) */
    Uint32               levels;        /* mip levels: > 1 for canvases with .mipmaps */
} Texture;

/* An app shader for canvas passes and draws, with a pipeline per (target
 * formats, blend, and for draws the vertex layout, strip and flip). */
typedef struct PassPipeline {
    SDL_GPUTextureFormat     formats[BARE_MAX_TARGETS];
    int                      targets;
    BareBlend                blend;
    int                      vertex_size, instance_size;
    bool                     strip, flip;
    SDL_GPUTextureFormat     depth_format;      /* INVALID: no depth buffer */
    BareDepthTest            depth_test;
    bool                     depth_write;
    SDL_GPUGraphicsPipeline *pipeline;
} PassPipeline;

#define MAX_PASS_PIPELINES 12

typedef struct UserShader {
    SDL_GPUShader *fs;
    SDL_GPUShader *vs[2];     /* a program's vertex half: as written, with y flipped (bottom_up targets) */
    int            vert_samplers;
    bool           no_uv;     /* drawn with the pass vertex shader without v_uv */
    bool           dxbc;      /* made from DXBC: its vertex half must be DXBC too */
    PassPipeline   pipes[MAX_PASS_PIPELINES];
    int            pipe_count;
} UserShader;

#define MAX_CLIP_DEPTH 64

typedef struct GpuRenderer {
    SDL_GPUDevice           *device;
    SDL_Window              *window;
    SDL_GPUGraphicsPipeline *pipeline, *glass_pipeline;
    SDL_GPUTextureFormat     format;
    SDL_GPUSampler          *sampler, *mip_sampler;
    SDL_GPUSampler          *samplers[4];        /* [nearest][repeat]; [0][0] = sampler */
    SDL_GPUSampler          *mip_samplers[2];    /* [repeat]: trilinear, anisotropic, for .mipmaps canvases */
    SDL_GPUShader           *pass_vs[2][2];      /* shaders/pass.glsl, [without v_uv][as DXBC] */
    SDL_GPUTexture          *white;
    SDL_GPUTexture          *scene, *backdrop;
    Uint32                   scene_w, scene_h;

    SDL_GPUBuffer           *instance_buf;
    SDL_GPUTransferBuffer   *instance_xfer;
    Uint32                   instance_cap;       /* in instances */

    SDL_GPUTransferBuffer   *upload_xfer;
    Uint32                   upload_cap;         /* bytes */
    SDL_GPUTransferBuffer   *image_xfer;         /* the frame's image uploads */
    Uint32                   image_cap;          /* bytes */
    SDL_GPUBuffer           *draw_buf;           /* the frame's vertex and instance data (bare_draw) */
    SDL_GPUTransferBuffer   *draw_xfer;
    Uint32                   draw_cap;           /* bytes */
    Uint32                  *draw_offsets;       /* per draw: its vertices' and instances' offsets */
    Uint32                   draw_offset_cap;

    Instance  *instances; Uint32 instance_count, instances_alloc;
    Batch     *batches;   Uint32 batch_count, batches_alloc;
    GlassDraw *glass;     Uint32 glass_count, glass_alloc;
    Texture   *textures;  Uint32 texture_cap;
    UserShader *shaders;  Uint32 shader_cap;

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
    const unsigned char *dxbc;   /* optional: preferred on D3D12 */
    size_t dxbc_size;
} ShaderCode;

#define CODE(name, u, smp) { name##_spirv, name##_dxil, name##_msl, \
                             sizeof name##_spirv, sizeof name##_dxil, sizeof name##_msl, u, smp, NULL, 0 }
#define CODE_DXBC(name, u, smp) { name##_spirv, name##_dxil, name##_msl, \
                                  sizeof name##_spirv, sizeof name##_dxil, sizeof name##_msl, u, smp, \
                                  name##_dxbc, sizeof name##_dxbc }

/* On D3D12, code that has DXBC is made from it, made strict (see dxbc.h):
 * FXC's output run as written is how browsers' WebGL comes out on Windows, so
 * ported WebGL shaders give the same bits (DXC reassociates float maths, and
 * so does the driver when allowed). *dxbc says whether it was used. */
static SDL_GPUShader *make_shader_ex(SDL_GPUShaderStage stage, ShaderCode c, bool *dxbc) {
    SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(gpu.device);
    SDL_GPUShaderCreateInfo info;
    SDL_zero(info);
    info.stage = stage;
    info.num_uniform_buffers = c.uniforms;
    info.num_samplers = c.samplers;
    unsigned char *strict = NULL;
    if (dxbc) *dxbc = false;
    if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        info.code = c.spirv;
        info.code_size = c.spirv_size;
        info.entrypoint = "main";
    } else if (c.dxbc && (formats & SDL_GPU_SHADERFORMAT_DXBC)) {
        strict = SDL_malloc(c.dxbc_size);
        if (!strict) return NULL;
        memcpy(strict, c.dxbc, c.dxbc_size);
        if (!bare_dxbc_make_strict(strict, c.dxbc_size)) SDL_Log("bare: unrecognised DXBC, used as it is");
        info.format = SDL_GPU_SHADERFORMAT_DXBC;
        info.code = strict;
        info.code_size = c.dxbc_size;
        info.entrypoint = "main";
        if (dxbc) *dxbc = true;
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
    SDL_free(strict);
    return sh;
}

static SDL_GPUShader *make_shader(SDL_GPUShaderStage stage, ShaderCode c) { return make_shader_ex(stage, c, NULL); }

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
    gpu.device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_DXBC |
                                     SDL_GPU_SHADERFORMAT_MSL, debug, NULL);
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
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.max_lod = 0;
    for (int k = 0; k < 4; k++) {   /* for images and canvases: [nearest][repeat] */
        bool nearest = k & 2, repeat = k & 1;
        si.min_filter = si.mag_filter = nearest ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
        si.address_mode_u = si.address_mode_v = si.address_mode_w =
            repeat ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        gpu.samplers[k] = k == 0 ? gpu.sampler : SDL_CreateGPUSampler(gpu.device, &si);
        if (!gpu.samplers[k]) return false;
    }
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    si.max_lod = 16.0f;
    si.enable_anisotropy = true;
    si.max_anisotropy = 8.0f;
    for (int k = 0; k < 2; k++) {
        si.address_mode_u = si.address_mode_v = si.address_mode_w =
            k ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        gpu.mip_samplers[k] = SDL_CreateGPUSampler(gpu.device, &si);
        if (!gpu.mip_samplers[k]) return false;
    }
    /* The pass vertex shader with and without v_uv, each also as DXBC (on
     * D3D12) for fragment shaders made from DXBC: a pipeline can't mix them. */
    ShaderCode pass_v[2] = { CODE_DXBC(pass_vert, 0, 0), CODE_DXBC(pass_nouv_vert, 0, 0) };
    for (int k = 0; k < 2; k++) {
        ShaderCode plain = pass_v[k];
        plain.dxbc = NULL;
        gpu.pass_vs[k][0] = make_shader(SDL_GPU_SHADERSTAGE_VERTEX, plain);
        gpu.pass_vs[k][1] = make_shader(SDL_GPU_SHADERSTAGE_VERTEX, pass_v[k]);
        if (!gpu.pass_vs[k][0] || !gpu.pass_vs[k][1]) return false;
    }

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

static void destroy_shader(Uint32 id);

static void shutdown_renderer(void *self) {
    (void)self;
    if (!gpu.device) return;
    SDL_WaitForGPUIdle(gpu.device);
    for (Uint32 i = 0; i < gpu.texture_cap; i++) {
        if (gpu.textures[i].tex) SDL_ReleaseGPUTexture(gpu.device, gpu.textures[i].tex);
    }
    SDL_free(gpu.textures);
    for (Uint32 i = 0; i < gpu.shader_cap; i++) destroy_shader(i);
    SDL_free(gpu.shaders);
    for (int k = 1; k < 4; k++) SDL_ReleaseGPUSampler(gpu.device, gpu.samplers[k]);
    for (int k = 0; k < 2; k++) SDL_ReleaseGPUSampler(gpu.device, gpu.mip_samplers[k]);
    for (int k = 0; k < 4; k++)
        if (gpu.pass_vs[k / 2][k % 2]) SDL_ReleaseGPUShader(gpu.device, gpu.pass_vs[k / 2][k % 2]);
    SDL_free(gpu.instances);
    SDL_free(gpu.batches);
    SDL_free(gpu.glass);
    if (gpu.scene) SDL_ReleaseGPUTexture(gpu.device, gpu.scene);
    if (gpu.backdrop) SDL_ReleaseGPUTexture(gpu.device, gpu.backdrop);
    if (gpu.capture_xfer) SDL_ReleaseGPUTransferBuffer(gpu.device, gpu.capture_xfer);
    if (gpu.instance_buf) SDL_ReleaseGPUBuffer(gpu.device, gpu.instance_buf);
    if (gpu.instance_xfer) SDL_ReleaseGPUTransferBuffer(gpu.device, gpu.instance_xfer);
    if (gpu.upload_xfer) SDL_ReleaseGPUTransferBuffer(gpu.device, gpu.upload_xfer);
    if (gpu.image_xfer) SDL_ReleaseGPUTransferBuffer(gpu.device, gpu.image_xfer);
    if (gpu.draw_buf) SDL_ReleaseGPUBuffer(gpu.device, gpu.draw_buf);
    if (gpu.draw_xfer) SDL_ReleaseGPUTransferBuffer(gpu.device, gpu.draw_xfer);
    SDL_free(gpu.draw_offsets);
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
    while (n < need) {
        if (n > UINT32_MAX / 2) { n = need; break; }
        n *= 2;
    }
    if (size && (size_t)n > SIZE_MAX / size) return false;
    void *p = SDL_realloc(*items, (size_t)n * size);
    if (!p) return false;
    /* New slots must read as empty: textures and shaders are looked up by id. */
    memset((char *)p + (size_t)*cap * size, 0, (size_t)(n - *cap) * size);
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
    set4(d->frag.p2, g->amount, g->zoom > 0 ? g->zoom : 1.0f, g->light, 0);

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
                set4(in->params, 0, 1, t->blur, 0);
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

/* The backdrop is only needed by glass, and is retained until resize. */
static bool ensure_targets(Uint32 w, Uint32 h) {
    if (!gpu.scene || gpu.scene_w != w || gpu.scene_h != h) {
        if (gpu.scene) SDL_ReleaseGPUTexture(gpu.device, gpu.scene);
        if (gpu.backdrop) SDL_ReleaseGPUTexture(gpu.device, gpu.backdrop);
        gpu.backdrop = NULL;
        gpu.scene = make_texture((int)w, (int)h, gpu.format,
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, 1);
        gpu.scene_w = w;
        gpu.scene_h = h;
    }
    if (gpu.glass_count && !gpu.backdrop) {
        Uint32 levels = 1;
        while ((SDL_max(w, h) >> levels) > 0) levels++;
        gpu.backdrop = make_texture((int)w, (int)h, gpu.format,
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, levels);
    }
    bool ok = gpu.scene && (!gpu.glass_count || gpu.backdrop);
    if (!ok) SDL_Log("bare: offscreen targets failed: %s", SDL_GetError());
    return ok;
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

/* Copies only the part of the scene a glass draw can sample: its quad, grown by
 * how far refraction, zoom and frost reach. The rest of the backdrop keeps the
 * previous snapshot, which this glass never reads. */
static void blit_region(SDL_GPUCommandBuffer *cmd, const GlassDraw *g, Uint32 w, Uint32 h) {
    const float *b = g->vert.bounds;
    float reach = g->frag.p0[2] * 2.0f * SDL_max(g->frag.p0[3], 1.0f) + g->frag.p1[0] * 8.0f + 48.0f;
    float zoom = g->frag.p2[1] > 1.0f ? g->frag.p2[1] : 1.0f;
    reach += (zoom - 1.0f) * SDL_max(b[2], b[3]);
    int x0 = SDL_max((int)(b[0] - reach), 0), y0 = SDL_max((int)(b[1] - reach), 0);
    int x1 = SDL_min((int)(b[0] + b[2] + reach) + 1, (int)w), y1 = SDL_min((int)(b[1] + b[3] + reach) + 1, (int)h);
    if (x1 <= x0 || y1 <= y0) return;
    SDL_GPUBlitInfo info;
    SDL_zero(info);
    info.source.texture = gpu.scene;
    info.source.x = info.destination.x = (Uint32)x0;
    info.source.y = info.destination.y = (Uint32)y0;
    info.source.w = info.destination.w = (Uint32)(x1 - x0);
    info.source.h = info.destination.h = (Uint32)(y1 - y0);
    info.destination.texture = gpu.backdrop;
    info.load_op = SDL_GPU_LOADOP_LOAD;
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

/* ---- images, canvases, shaders and passes -------------------------------- */

/* The first format the device can sample (and render to, for canvases);
 * narrower float formats fall back to wider ones, as WebGL apps do. */
static bool is_depth_format(SDL_GPUTextureFormat f) {
    return f == SDL_GPU_TEXTUREFORMAT_D32_FLOAT || f == SDL_GPU_TEXTUREFORMAT_D24_UNORM ||
           f == SDL_GPU_TEXTUREFORMAT_D16_UNORM;
}

/* Depth canvases: the most precise depth format, one that can be sampled if
 * there is one (later passes read it: soft particles, fog by distance). */
static SDL_GPUTextureFormat depth_format(SDL_GPUTextureUsageFlags *usage) {
    static const SDL_GPUTextureFormat list[] = { SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREFORMAT_D24_UNORM,
                                                 SDL_GPU_TEXTUREFORMAT_D16_UNORM };
    for (int sampled = 1; sampled >= 0; sampled--) {
        *usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | (sampled ? SDL_GPU_TEXTUREUSAGE_SAMPLER : 0);
        for (int i = 0; i < 3; i++)
            if (SDL_GPUTextureSupportsFormat(gpu.device, list[i], SDL_GPU_TEXTURETYPE_2D, *usage)) return list[i];
    }
    return SDL_GPU_TEXTUREFORMAT_INVALID;
}

static SDL_GPUTextureFormat image_format(BareFormat f, bool renderable) {
    static const SDL_GPUTextureFormat floats[] = { SDL_GPU_TEXTUREFORMAT_R16_FLOAT, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT,
                                                   SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT };
    static const SDL_GPUTextureFormat rgba8[] = { SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM };
    static const SDL_GPUTextureFormat rgba32f[] = { SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT,
                                                    SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT };
    const SDL_GPUTextureFormat *list = rgba8;
    int n = 1;
    switch (f) {
    case BARE_R16F: list = floats; n = 3; break;
    case BARE_RG16F: list = floats + 1; n = 2; break;
    case BARE_RGBA16F: list = floats + 2; n = 1; break;
    case BARE_RGBA32F: list = rgba32f; n = 2; break;
    default: break;
    }
    SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (renderable ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
    for (int i = 0; i < n; i++)
        if (SDL_GPUTextureSupportsFormat(gpu.device, list[i], SDL_GPU_TEXTURETYPE_2D, usage)) return list[i];
    return SDL_GPU_TEXTUREFORMAT_INVALID;
}

static void create_image(SDL_GPUCommandBuffer *cmd, const BareGpuImage *im) {
    Texture *t = texture_slot(im->id);
    if (!t) return;
    if (t->tex) SDL_ReleaseGPUTexture(gpu.device, t->tex);
    memset(t, 0, sizeof *t);
    bool depth = im->format == BARE_DEPTH;
    SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER |
                                     (im->renderable ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
    SDL_GPUTextureFormat f = depth ? depth_format(&usage) : image_format(im->format, im->renderable);
    if (f == SDL_GPU_TEXTUREFORMAT_INVALID) {
        SDL_Log("bare: no supported texture format for image %u (format %d)", (unsigned)im->id, (int)im->format);
        return;
    }
    Uint32 levels = 1;
    if (im->mipmaps && !depth)
        while ((im->width >> levels) > 0 || (im->height >> levels) > 0) levels++;
    if (!depth && !im->renderable && (im->srgb || levels > 1)) {   /* bare_texture; mips are made by drawing */
        SDL_GPUTextureFormat want = im->srgb ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        SDL_GPUTextureUsageFlags u = SDL_GPU_TEXTUREUSAGE_SAMPLER | (levels > 1 ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
        if (SDL_GPUTextureSupportsFormat(gpu.device, want, SDL_GPU_TEXTURETYPE_2D, u)) f = want, usage = u;
        else levels = 1;
    }
    t->tex = make_texture(im->width, im->height, f, usage, levels);
    if (!t->tex) {
        SDL_Log("bare: creating a %dx%d image failed: %s", im->width, im->height, SDL_GetError());
        return;
    }
    t->w = im->width;
    t->h = im->height;
    t->gpu_format = f;
    /* depth is sampled nearest: not every device filters depth formats */
    t->sampler = gpu.samplers[(im->nearest || depth ? 2 : 0) + (im->repeat ? 1 : 0)];
    t->levels = levels;
    if (levels > 1 && !im->nearest) t->sampler = gpu.mip_samplers[im->repeat ? 1 : 0];
    if (depth) {   /* depth starts far */
        SDL_GPUDepthStencilTargetInfo dt;
        SDL_zero(dt);
        dt.texture = t->tex;
        dt.clear_depth = 1.0f;
        dt.load_op = SDL_GPU_LOADOP_CLEAR;
        dt.store_op = SDL_GPU_STOREOP_STORE;
        dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, NULL, 0, &dt));
    } else if (im->renderable) {   /* canvases start transparent black, not with leftover memory */
        SDL_GPUColorTargetInfo ct;
        SDL_zero(ct);
        ct.texture = t->tex;
        ct.load_op = SDL_GPU_LOADOP_CLEAR;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &ct, 1, NULL));
        if (levels > 1) SDL_GenerateMipmapsForGPUTexture(cmd, t->tex);
    }
}

static Uint32 image_bytes(const BareGpuImage *im) { return (Uint32)im->width * (Uint32)im->height * 4; }

/* Every image upload of the frame goes through one transfer buffer, filled
 * once: mapping a buffer per upload makes SDL keep a new copy for each while
 * the GPU still reads the last, and a burst of pictures would leave dozens of
 * picture-sized buffers behind. One that grew past STAGE_KEEP for a burst is
 * let go once the frame's copies are recorded. */
#define STAGE_KEEP (4u << 20)

static bool stage_images(const BareDrawList *list) {
    Uint32 total = 0;
    for (int i = 0; i < list->op_count; i++)
        if (list->ops[i].kind == BARE_GPU_IMAGE_UPLOAD && list->ops[i].as.image.pixels)
            total += image_bytes(&list->ops[i].as.image);
    if (!total) return false;
    if (!ensure_xfer(&gpu.image_xfer, &gpu.image_cap, total, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD)) return false;
    Uint8 *dst = SDL_MapGPUTransferBuffer(gpu.device, gpu.image_xfer, true);
    if (!dst) return false;
    for (int i = 0; i < list->op_count; i++) {
        const BareGpuImage *im = &list->ops[i].as.image;
        if (list->ops[i].kind != BARE_GPU_IMAGE_UPLOAD || !im->pixels) continue;
        memcpy(dst, im->pixels, image_bytes(im));
        dst += image_bytes(im);
    }
    SDL_UnmapGPUTransferBuffer(gpu.device, gpu.image_xfer);
    return true;
}

static void upload_image(SDL_GPUCommandBuffer *cmd, const BareGpuImage *im, Uint32 offset) {
    if (im->id >= gpu.texture_cap || !gpu.textures[im->id].tex) return;
    Texture *t = &gpu.textures[im->id];
    if (t->w != im->width || t->h != im->height) return;
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = { .transfer_buffer = gpu.image_xfer, .offset = offset };
    SDL_GPUTextureRegion region = { .texture = t->tex, .w = (Uint32)t->w, .h = (Uint32)t->h, .d = 1 };
    SDL_UploadToGPUTexture(copy, &src, &region, false);
    SDL_EndGPUCopyPass(copy);
    if (t->levels > 1) SDL_GenerateMipmapsForGPUTexture(cmd, t->tex);
}

static UserShader *shader_slot(Uint32 id) {
    if (!grow((void **)&gpu.shaders, &gpu.shader_cap, id + 1, sizeof(UserShader))) return NULL;
    return &gpu.shaders[id];
}

static void destroy_shader(Uint32 id) {
    if (id >= gpu.shader_cap) return;
    UserShader *u = &gpu.shaders[id];
    for (int k = 0; k < u->pipe_count; k++) SDL_ReleaseGPUGraphicsPipeline(gpu.device, u->pipes[k].pipeline);
    if (u->fs) SDL_ReleaseGPUShader(gpu.device, u->fs);
    for (int k = 0; k < 2; k++)
        if (u->vs[k]) SDL_ReleaseGPUShader(gpu.device, u->vs[k]);
    memset(u, 0, sizeof *u);
}

static void create_shader(const BareGpuShader *sh) {
    if (!shader_slot(sh->id)) return;
    destroy_shader(sh->id);
    const BareShaderCode *c = sh->code;
    UserShader *u = &gpu.shaders[sh->id];
    bool program = c->vert_spirv[0] || c->vert_dxil[0] || c->vert_msl[0];
    /* Both halves from DXBC or neither: D3D12 can't link one with the other. */
    bool dxbc = c->dxbc && (!program || (c->vert_dxbc[0] && c->vert_dxbc[1]));
    ShaderCode code = { c->spirv, c->dxil, c->msl, c->spirv_size, c->dxil_size, c->msl_size,
                        (Uint32)c->uniform_buffers, (Uint32)c->samplers, dxbc ? c->dxbc : NULL,
                        dxbc ? c->dxbc_size : 0 };
    u->fs = make_shader_ex(SDL_GPU_SHADERSTAGE_FRAGMENT, code, &u->dxbc);
    u->no_uv = c->no_uv != 0;
    u->vert_samplers = program ? c->vert_samplers : 0;
    for (int k = 0; program && u->fs && k < 2; k++) {
        ShaderCode v = { c->vert_spirv[k], c->vert_dxil[k], c->vert_msl[k], c->vert_spirv_size[k],
                         c->vert_dxil_size[k], c->vert_msl_size[k], (Uint32)c->vert_uniform_buffers,
                         (Uint32)c->vert_samplers,
                         u->dxbc ? c->vert_dxbc[k] : NULL, u->dxbc ? c->vert_dxbc_size[k] : 0 };
        u->vs[k] = make_shader(SDL_GPU_SHADERSTAGE_VERTEX, v);
        if (!u->vs[k]) {   /* a program without its vertex half can't draw */
            SDL_ReleaseGPUShader(gpu.device, u->fs);
            u->fs = NULL;
        }
    }
}

static SDL_GPUCompareOp compare_op(BareDepthTest t) {
    switch (t) {
    case BARE_DEPTH_LESS_EQUAL: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    case BARE_DEPTH_GREATER: return SDL_GPU_COMPAREOP_GREATER;
    case BARE_DEPTH_GREATER_EQUAL: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    case BARE_DEPTH_EQUAL: return SDL_GPU_COMPAREOP_EQUAL;
    case BARE_DEPTH_ALWAYS: return SDL_GPU_COMPAREOP_ALWAYS;
    default: return SDL_GPU_COMPAREOP_LESS;
    }
}

static SDL_GPUGraphicsPipeline *pass_pipeline(UserShader *u, const SDL_GPUTextureFormat *formats, int n,
                                              SDL_GPUTextureFormat zformat, const BareGpuPass *op, bool draw) {
    BareBlend blend = op->blend;
    int vsize = draw ? op->vertex_size : 0, isize = draw ? op->instance_size : 0;
    bool strip = draw && op->strip, flip = draw && op->flip;
    bool zon = zformat != SDL_GPU_TEXTUREFORMAT_INVALID;
    bool zwrite = zon && op->depth_write;
    BareDepthTest ztest = zon ? op->depth_test : BARE_DEPTH_LESS;
    for (int k = 0; k < u->pipe_count; k++) {
        const PassPipeline *pp = &u->pipes[k];
        if (pp->targets == n && pp->blend == blend && pp->vertex_size == vsize && pp->instance_size == isize &&
            pp->strip == strip && pp->flip == flip && pp->depth_format == zformat && pp->depth_test == ztest &&
            pp->depth_write == zwrite && !memcmp(pp->formats, formats, (size_t)n * sizeof *formats))
            return pp->pipeline;
    }
    if (u->pipe_count == MAX_PASS_PIPELINES) {
        SDL_Log("bare: canvas shader exceeded %d pipeline variants", MAX_PASS_PIPELINES);
        return NULL;
    }

    SDL_GPUColorTargetDescription targets[BARE_MAX_TARGETS];
    SDL_zero(targets);
    SDL_GPUColorTargetDescription target;
    SDL_zero(target);
    if (blend != BARE_BLEND_NONE) {
        SDL_GPUBlendFactor dst = blend == BARE_BLEND_ADD ? SDL_GPU_BLENDFACTOR_ONE
                                                         : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        SDL_GPUBlendFactor src = blend == BARE_BLEND_ALPHA ? SDL_GPU_BLENDFACTOR_SRC_ALPHA : SDL_GPU_BLENDFACTOR_ONE;
        target.blend_state.enable_blend = true;
        target.blend_state.src_color_blendfactor = src;
        target.blend_state.dst_color_blendfactor = dst;
        target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        /* ALPHA: the alpha channel adds coverage (ONE, 1 - a), so an opaque target stays opaque */
        target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        target.blend_state.dst_alpha_blendfactor = dst;
        target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(info);
    info.vertex_shader = draw ? u->vs[flip] : gpu.pass_vs[u->no_uv][u->dxbc];
    info.fragment_shader = u->fs;
    info.primitive_type = strip ? SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP : SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    /* A draw's attributes: vec4 slots, the vertex's (buffer 0) then the instance's (the next buffer). */
    SDL_GPUVertexBufferDescription bufs[2];
    SDL_GPUVertexAttribute attrs[32];
    Uint32 nb = 0, na = 0;
    SDL_zero(bufs);
    SDL_zero(attrs);
    for (int k = 0; k < 2; k++) {
        int size = k == 0 ? vsize : isize;
        if (size <= 0) continue;
        bufs[nb] = (SDL_GPUVertexBufferDescription){
            .slot = nb, .pitch = (Uint32)size,
            .input_rate = k == 0 ? SDL_GPU_VERTEXINPUTRATE_VERTEX : SDL_GPU_VERTEXINPUTRATE_INSTANCE };
        for (int a = 0; a < size / 16 && na < 32; a++, na++)
            attrs[na] = (SDL_GPUVertexAttribute){ .location = na, .buffer_slot = nb,
                                                  .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, .offset = (Uint32)a * 16 };
        nb++;
    }
    info.vertex_input_state.vertex_buffer_descriptions = bufs;
    info.vertex_input_state.num_vertex_buffers = nb;
    info.vertex_input_state.vertex_attributes = attrs;
    info.vertex_input_state.num_vertex_attributes = na;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    for (int i = 0; i < n; i++) {
        targets[i] = target;
        targets[i].format = formats[i];
    }
    info.target_info.color_target_descriptions = targets;
    info.target_info.num_color_targets = (Uint32)n;
    if (zon) {
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = zformat;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = zwrite;
        info.depth_stencil_state.compare_op = compare_op(ztest);
    }
    SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(gpu.device, &info);
    if (!p) {
        SDL_Log("bare: canvas pass pipeline failed: %s", SDL_GetError());
        return NULL;
    }
    PassPipeline *pp = &u->pipes[u->pipe_count++];
    memset(pp, 0, sizeof *pp);
    memcpy(pp->formats, formats, (size_t)n * sizeof *formats);
    pp->targets = n;
    pp->blend = blend;
    pp->vertex_size = vsize, pp->instance_size = isize;
    pp->strip = strip, pp->flip = flip;
    pp->depth_format = zformat, pp->depth_test = ztest, pp->depth_write = zwrite;
    pp->pipeline = p;
    return p;
}

/* A draw's vertex and instance data: every draw of the frame in one buffer,
 * copied over once before the first pass. Offsets go in draw order. */
static Uint32 draw_bytes(const BareGpuPass *p, int which) {
    Uint32 n = which == 0 ? (p->vertices ? (Uint32)p->vertex_size * (Uint32)p->vertex_count : 0)
                          : (p->instances ? (Uint32)p->instance_size * (Uint32)p->instance_count : 0);
    return (n + 15) & ~15u;
}

static bool stage_draws(SDL_GPUCommandBuffer *cmd, const BareDrawList *list) {
    Uint32 total = 0, draws = 0;
    for (int i = 0; i < list->op_count; i++) {
        if (list->ops[i].kind != BARE_GPU_DRAW) continue;
        total += draw_bytes(&list->ops[i].as.pass, 0) + draw_bytes(&list->ops[i].as.pass, 1);
        draws++;
    }
    if (!total) return true;
    if (!grow((void **)&gpu.draw_offsets, &gpu.draw_offset_cap, draws * 2, sizeof *gpu.draw_offsets)) return false;
    if (total > gpu.draw_cap) {
        Uint32 cap = gpu.draw_cap ? gpu.draw_cap : 64 * 1024;
        while (cap < total) cap *= 2;
        if (gpu.draw_buf) SDL_ReleaseGPUBuffer(gpu.device, gpu.draw_buf);
        if (gpu.draw_xfer) SDL_ReleaseGPUTransferBuffer(gpu.device, gpu.draw_xfer);
        SDL_GPUBufferCreateInfo bi = { .usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = cap };
        SDL_GPUTransferBufferCreateInfo ti = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = cap };
        gpu.draw_buf = SDL_CreateGPUBuffer(gpu.device, &bi);
        gpu.draw_xfer = SDL_CreateGPUTransferBuffer(gpu.device, &ti);
        gpu.draw_cap = gpu.draw_buf && gpu.draw_xfer ? cap : 0;
        if (!gpu.draw_cap) return false;
    }
    Uint8 *dst = SDL_MapGPUTransferBuffer(gpu.device, gpu.draw_xfer, true);
    if (!dst) return false;
    Uint32 at = 0, d = 0;
    for (int i = 0; i < list->op_count; i++) {
        const BareGpuPass *p = &list->ops[i].as.pass;
        if (list->ops[i].kind != BARE_GPU_DRAW) continue;
        for (int k = 0; k < 2; k++) {
            const void *src = k == 0 ? p->vertices : p->instances;
            Uint32 n = draw_bytes(p, k);
            gpu.draw_offsets[d * 2 + (Uint32)k] = at;
            if (src && n) memcpy(dst + at, src, k == 0 ? (size_t)p->vertex_size * (size_t)p->vertex_count
                                                       : (size_t)p->instance_size * (size_t)p->instance_count);
            at += n;
        }
        d++;
    }
    SDL_UnmapGPUTransferBuffer(gpu.device, gpu.draw_xfer);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src = { .transfer_buffer = gpu.draw_xfer, .offset = 0 };
    SDL_GPUBufferRegion region = { .buffer = gpu.draw_buf, .offset = 0, .size = total };
    SDL_UploadToGPUBuffer(copy, &src, &region, true);
    SDL_EndGPUCopyPass(copy);
    return true;
}

/* A pass (one triangle over the target) or a draw (`draw` >= 0: the frame's
 * draw number, for its data's place in draw_buf). */
/* Canvases with .mipmaps get their chain rebuilt after each pass or draw into them. */
static void update_mips(SDL_GPUCommandBuffer *cmd, Texture *const *t, int nt) {
    for (int i = 0; i < nt; i++)
        if (t[i]->levels > 1) SDL_GenerateMipmapsForGPUTexture(cmd, t[i]->tex);
}

static void run_pass(SDL_GPUCommandBuffer *cmd, const BareGpuPass *p, int draw) {
    if (p->shader >= gpu.shader_cap || !gpu.shaders[p->shader].fs) return;
    if (draw >= 0 && !gpu.shaders[p->shader].vs[0]) return;
    /* The target, then the extra targets in order (the core checked them). */
    Texture *t[BARE_MAX_TARGETS];
    SDL_GPUTextureFormat formats[BARE_MAX_TARGETS];
    int nt = 0;
    for (int i = 0; i < BARE_MAX_TARGETS; i++) {
        Uint32 id = i == 0 ? p->target : p->extra_targets[i - 1];
        if (i > 0 && !id) continue;
        if (id >= gpu.texture_cap || !gpu.textures[id].tex) return;
        t[nt] = &gpu.textures[id];
        formats[nt++] = gpu.textures[id].gpu_format;
    }
    /* A depth canvas (the core checked its size and format). */
    Texture *z = NULL;
    if (draw >= 0 && p->depth) {
        if (p->depth >= gpu.texture_cap || !gpu.textures[p->depth].tex ||
            !is_depth_format(gpu.textures[p->depth].gpu_format)) return;
        z = &gpu.textures[p->depth];
    }
    SDL_GPUGraphicsPipeline *pipeline = pass_pipeline(&gpu.shaders[p->shader], formats, nt,
                                                      z ? z->gpu_format : SDL_GPU_TEXTUREFORMAT_INVALID, p, draw >= 0);
    if (!pipeline) return;

    SDL_GPUTextureSamplerBinding inputs[BARE_MAX_INPUTS];
    int n = SDL_min(p->input_count, BARE_MAX_INPUTS);
    int nv = draw >= 0 ? SDL_min(gpu.shaders[p->shader].vert_samplers, BARE_MAX_INPUTS) : 0;
    for (int i = 0; i < SDL_max(n, nv); i++) {
        Uint32 id = p->inputs[i];
        bool ok = id && id < gpu.texture_cap && gpu.textures[id].tex;
        inputs[i].texture = ok ? gpu.textures[id].tex : gpu.white;
        inputs[i].sampler = ok && gpu.textures[id].sampler ? gpu.textures[id].sampler : gpu.sampler;
    }

    SDL_GPUColorTargetInfo ct[BARE_MAX_TARGETS];
    SDL_zero(ct);
    for (int i = 0; i < nt; i++) {
        ct[i].texture = t[i]->tex;
        ct[i].store_op = SDL_GPU_STOREOP_STORE;
        /* Without blending every pixel is overwritten, so the old contents
         * needn't be loaded. No cycling: a canvas written many times a frame
         * (ping-pong simulations) would get a new copy of its memory for
         * every write. */
        ct[i].load_op = p->blend == BARE_BLEND_NONE ? SDL_GPU_LOADOP_DONT_CARE : SDL_GPU_LOADOP_LOAD;
        if (draw >= 0) {   /* a draw covers what its triangles cover: the rest stays, or is cleared */
            ct[i].load_op = p->clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
            ct[i].clear_color = (SDL_FColor){ p->clear_color[0], p->clear_color[1], p->clear_color[2],
                                              p->clear_color[3] };
        }
    }
    SDL_GPUDepthStencilTargetInfo dt;
    SDL_zero(dt);
    if (z) {
        dt.texture = z->tex;
        dt.clear_depth = p->depth_clear;
        dt.load_op = p->clear_depth ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        dt.store_op = SDL_GPU_STOREOP_STORE;
        dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    }
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, ct, (Uint32)nt, z ? &dt : NULL);
    if (!pass) return;
    if (draw >= 0 && p->vertex_count <= 0) {   /* only clearing */
        SDL_EndGPURenderPass(pass);
        update_mips(cmd, t, nt);
        return;
    }
    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    if (n) SDL_BindGPUFragmentSamplers(pass, 0, inputs, (Uint32)n);
    if (nv) SDL_BindGPUVertexSamplers(pass, 0, inputs, (Uint32)nv);
    if (p->params_size > 0) SDL_PushGPUFragmentUniformData(cmd, 0, p->params, (Uint32)p->params_size);
    if (draw < 0) {
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    } else {
        if (p->params_size > 0) SDL_PushGPUVertexUniformData(cmd, 0, p->params, (Uint32)p->params_size);
        SDL_GPUBufferBinding vb[2];
        Uint32 nb = 0;
        if (p->vertex_size > 0) vb[nb++] = (SDL_GPUBufferBinding){ gpu.draw_buf, gpu.draw_offsets[draw * 2] };
        if (p->instance_size > 0) vb[nb++] = (SDL_GPUBufferBinding){ gpu.draw_buf, gpu.draw_offsets[draw * 2 + 1] };
        if (nb) SDL_BindGPUVertexBuffers(pass, 0, vb, nb);
        SDL_DrawGPUPrimitives(pass, (Uint32)p->vertex_count, (Uint32)SDL_max(1, p->instance_count), 0, 0);
    }
    SDL_EndGPURenderPass(pass);
    update_mips(cmd, t, nt);
}

static void run_ops(SDL_GPUCommandBuffer *cmd, const BareDrawList *list) {
    bool staged = stage_images(list);
    bool drawable = stage_draws(cmd, list);
    Uint32 offset = 0;
    int draws = 0;
    for (int i = 0; i < list->op_count; i++) {
        const BareGpuOp *op = &list->ops[i];
        switch (op->kind) {
        case BARE_GPU_IMAGE_CREATE: create_image(cmd, &op->as.image); break;
        case BARE_GPU_IMAGE_UPLOAD:
            if (staged && op->as.image.pixels) {
                upload_image(cmd, &op->as.image, offset);
                offset += image_bytes(&op->as.image);
            }
            break;
        case BARE_GPU_IMAGE_DESTROY: {
            Uint32 id = op->as.image.id;
            if (id < gpu.texture_cap && gpu.textures[id].tex) SDL_ReleaseGPUTexture(gpu.device, gpu.textures[id].tex);
            if (id < gpu.texture_cap) memset(&gpu.textures[id], 0, sizeof gpu.textures[id]);
            break;
        }
        case BARE_GPU_SHADER_CREATE: create_shader(&op->as.shader); break;
        case BARE_GPU_SHADER_DESTROY: destroy_shader(op->as.shader.id); break;
        case BARE_GPU_PASS: run_pass(cmd, &op->as.pass, -1); break;
        case BARE_GPU_DRAW:
            if (drawable) run_pass(cmd, &op->as.pass, draws);
            draws++;
            break;
        }
    }
    if (gpu.image_cap > STAGE_KEEP) {   /* released once the GPU is done with it */
        SDL_ReleaseGPUTransferBuffer(gpu.device, gpu.image_xfer);
        gpu.image_xfer = NULL;
        gpu.image_cap = 0;
    }
}

static void render(void *self, const BareDrawList *list) {
    (void)self;
    build(list);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu.device);
    if (!cmd) return;
    run_ops(cmd, list);   /* images, shaders and canvas passes, in the app's call order */

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
            const GlassDraw *g = &gpu.glass[b->glass];
            blit_region(cmd, g, sw, sh);
            SDL_GenerateMipmapsForGPUTexture(cmd, gpu.backdrop);
            pass = begin_scene_pass(cmd, false, list->clear);
            bound = false;

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
        SDL_GPUSampler *smp = gpu.sampler;
        if (b->texture && b->texture < gpu.texture_cap && gpu.textures[b->texture].tex) {
            tex = gpu.textures[b->texture].tex;
            if (gpu.textures[b->texture].sampler) smp = gpu.textures[b->texture].sampler;
        }
        SDL_GPUTextureSamplerBinding tsb = { .texture = tex, .sampler = smp };
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
