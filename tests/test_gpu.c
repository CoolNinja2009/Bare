/* Images, canvases, shaders and passes, headless: what reaches the draw list. */
#include "bare_test.h"
#include "bare.h"
#include "internal.h"
#include <string.h>

static BareContext *ctx;
static int log_count;
static char last_log[512];

static void capture_log(void *user, const char *msg) {
    (void)user;
    snprintf(last_log, sizeof last_log, "%s", msg);
    log_count++;
}

static void setup(void) {
    ctx = bare_ctx_create(NULL);
    bare_ctx_set_log(ctx, capture_log, NULL);
    bare_ctx_make_current(ctx);
    log_count = 0;
}
static void teardown(void) { bare_ctx_destroy(ctx); ctx = NULL; }

static const BareDrawList *frame(void) {
    bare_ctx_begin(ctx, 400, 300, 1.0f, 0.0);
    return bare_ctx_end(ctx, NULL);
}

static const unsigned char fake_spirv[4] = { 3, 2, 35, 7 };
static const BareShaderCode two_inputs = { .spirv = fake_spirv, .spirv_size = 4, .samplers = 2, .uniform_buffers = 1 };
static const BareShaderCode no_params = { .spirv = fake_spirv, .spirv_size = 4, .samplers = 1 };

typedef struct Params { float texel[2], dt, gain; } Params;

TEST(ops_reach_the_next_draw_list_once_and_in_order) {
    setup();
    BareImage a = bare_canvas(.width = 64, .height = 32, .format = BARE_RG16F, .nearest = true);
    BareImage b = bare_canvas(.width = 64, .height = 32);
    BareShader sh = bare_shader(&two_inputs);
    Params p = { { 1.0f / 64, 1.0f / 32 }, 0.016f, 2.0f };
    bare_pass(.target = b, .shader = sh, .inputs = { a }, BARE_PARAMS(p));
    p.gain = 99;   /* params were copied at the call */
    const BareDrawList *l = frame();
    CHECK(l->op_count == 4);
    CHECK(l->ops[0].kind == BARE_GPU_IMAGE_CREATE && l->ops[0].as.image.id == a);
    CHECK(l->ops[0].as.image.format == BARE_RG16F && l->ops[0].as.image.nearest && l->ops[0].as.image.renderable);
    CHECK(l->ops[1].kind == BARE_GPU_IMAGE_CREATE && l->ops[1].as.image.width == 64);
    CHECK(l->ops[2].kind == BARE_GPU_SHADER_CREATE && l->ops[2].as.shader.code == &two_inputs);
    const BareGpuPass *g = &l->ops[3].as.pass;
    CHECK(l->ops[3].kind == BARE_GPU_PASS && g->target == b && g->shader == sh);
    CHECK(g->inputs[0] == a && g->input_count == 2);
    CHECK(g->params_size == (int)sizeof(Params));
    CHECK_NEAR(((const Params *)g->params)->gain, 2.0, 0);
    CHECK(log_count == 0);
    l = frame();
    CHECK(l->op_count == 0);
    teardown();
}

TEST(passes_every_frame_reuse_memory) {
    setup();
    BareImage a = bare_canvas(.width = 8, .height = 8), b = bare_canvas(.width = 8, .height = 8);
    BareShader sh = bare_shader(&two_inputs);
    Params p = { { 0, 0 }, 0, 0 };
    int caps[4] = { 0 };
    for (int f = 0; f < 40; f++) {
        for (int i = 0; i < 30; i++) {
            p.dt = (float)i;
            bare_pass(.target = (i & 1) ? a : b, .shader = sh, .inputs = { (i & 1) ? b : a }, BARE_PARAMS(p));
        }
        const BareDrawList *l = frame();
        int n = f == 0 ? 33 : 30;
        CHECK(l->op_count == n);
        CHECK_NEAR(((const Params *)l->ops[n - 1].as.pass.params)->dt, 29, 0);
        if (f == 5) {
            caps[0] = ctx->gpu_pending.cap; caps[1] = ctx->gpu_pending.data_cap;
            caps[2] = ctx->gpu_published.cap; caps[3] = ctx->gpu_published.data_cap;
        }
    }
    CHECK(ctx->gpu_pending.cap == caps[0] && ctx->gpu_pending.data_cap == caps[1]);
    CHECK(ctx->gpu_published.cap == caps[2] && ctx->gpu_published.data_cap == caps[3]);
    teardown();
}

TEST(mistakes_are_reported_and_skipped) {
    setup();
    BareImage c = bare_canvas(.width = 8, .height = 8);
    BareImage img;
    unsigned char px[4 * 4 * 4] = { 0 };
    img = bare_image_from_pixels(4, 4, px);
    BareShader sh = bare_shader(&two_inputs), plain = bare_shader(&no_params);
    Params p = { { 0, 0 }, 0, 0 };
    frame();

    bare_pass(.target = img, .shader = sh, BARE_PARAMS(p));             /* not a canvas */
    CHECK(strstr(last_log, "live canvas") != NULL);
    bare_pass(.target = c, .shader = sh, .inputs = { c }, BARE_PARAMS(p));   /* reads its own target */
    CHECK(strstr(last_log, "also a target") != NULL);
    bare_pass(.target = c, .shader = sh);                                /* params missing */
    CHECK(strstr(last_log, "takes params") != NULL);
    bare_pass(.target = c, .shader = plain, .inputs = { img, img });     /* more inputs than samplers */
    CHECK(strstr(last_log, "reads 1") != NULL);
    bare_pass(.target = c, .shader = 77);
    CHECK(strstr(last_log, "live shader") != NULL);
    CHECK(strstr(last_log, "test_gpu.c") != NULL);                       /* the call site is named */
    CHECK(bare_canvas(.width = 0, .height = 8) == 0);
    CHECK(bare_shader(NULL) == 0);
    CHECK(log_count == 7);
    CHECK(frame()->op_count == 0);

    bare_pass(.target = c, .shader = plain, .inputs = { img });          /* fine */
    CHECK(frame()->op_count == 1);
    teardown();
}

TEST(freeing_queues_destroy_and_recycles_ids) {
    setup();
    unsigned char px[2 * 2 * 4] = { 1, 2, 3, 4 };
    BareImage img = bare_image_from_pixels(2, 2, px);
    px[0] = 200;   /* pixels were copied */
    const BareDrawList *l = frame();
    CHECK(l->op_count == 2);
    CHECK(l->ops[1].kind == BARE_GPU_IMAGE_UPLOAD && ((const unsigned char *)l->ops[1].as.image.pixels)[0] == 1);
    CHECK(!l->ops[0].as.image.renderable);
    CHECK_NEAR(bare_image_size(img).x, 2, 0);

    bare_image_free(img);
    CHECK_NEAR(bare_image_size(img).x, 0, 0);
    bare_image_free(img);
    CHECK(strstr(last_log, "freed twice") != NULL);
    BareImage again = bare_canvas(.width = 4, .height = 4);
    CHECK(again == img);   /* the id is reused; the renderer sees destroy, then create */
    l = frame();
    CHECK(l->op_count == 2);
    CHECK(l->ops[0].kind == BARE_GPU_IMAGE_DESTROY && l->ops[1].kind == BARE_GPU_IMAGE_CREATE);

    BareShader sh = bare_shader(&no_params);
    bare_shader_free(sh);
    l = frame();
    CHECK(l->op_count == 2 && l->ops[1].kind == BARE_GPU_SHADER_DESTROY);
    teardown();
}

TEST(image_boxes_draw_between_background_and_border) {
    setup();
    unsigned char px[4] = { 255, 255, 255, 255 };
    BareImage img = bare_image_from_pixels(1, 1, px);
    bare_ctx_begin(ctx, 400, 300, 2.0f, 0.0);
    bare_box(.width = 100, .height = 50, .radius = 10, .color = BARE_RGB(9, 9, 9), .border = 2,
             .border_color = BARE_WHITE, .image = img, .opacity = 0.5f);
    bare_box(.width = 10, .height = 10, .image = 12345);   /* not an image: nothing drawn */
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(l->cmd_count == 3);
    CHECK(l->cmds[0].kind == BARE_CMD_RECT && l->cmds[0].as.rect.border == 0 && l->cmds[0].as.rect.fill.a > 0);
    CHECK(l->cmds[1].kind == BARE_CMD_IMAGE);
    const BareImageCmd *im = &l->cmds[1].as.image;
    CHECK(im->texture == img);
    CHECK_NEAR(im->rect.w, 200, 0.01);
    CHECK_NEAR(im->radius[0], 20, 0.01);
    CHECK_NEAR(im->tint.a, 0.5, 0.001);
    CHECK_NEAR(im->uv.w, 1, 0);
    CHECK(l->cmds[2].kind == BARE_CMD_RECT && l->cmds[2].as.rect.border == 4 && l->cmds[2].as.rect.fill.a == 0);
    teardown();
}

TEST(cover_fit_crops_to_the_box_shape) {
    setup();
    unsigned char px[4 * 2 * 4] = { 0 };
    BareImage wide = bare_image_from_pixels(4, 2, px), tall = bare_image_from_pixels(2, 4, px);
    bare_ctx_begin(ctx, 400, 300, 1.0f, 0.0);
    bare_box(.width = 100, .height = 100, .image = wide, .image_fit = BARE_FIT_COVER);
    bare_box(.width = 100, .height = 100, .image = tall, .image_fit = BARE_FIT_COVER);
    bare_box(.width = 100, .height = 100, .image = tall);
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(l->cmd_count == 3);
    const BareRect a = l->cmds[0].as.image.uv, b = l->cmds[1].as.image.uv, c = l->cmds[2].as.image.uv;
    CHECK_NEAR(a.x, 0.25, 0.001); CHECK_NEAR(a.w, 0.5, 0.001); CHECK_NEAR(a.y, 0, 0); CHECK_NEAR(a.h, 1, 0);
    CHECK_NEAR(b.y, 0.25, 0.001); CHECK_NEAR(b.h, 0.5, 0.001); CHECK_NEAR(b.x, 0, 0); CHECK_NEAR(b.w, 1, 0);
    CHECK_NEAR(c.w, 1, 0); CHECK_NEAR(c.h, 1, 0);
    teardown();
}

TEST(nothing_without_a_context) {
    bare_ctx_make_current(NULL);
    CHECK(bare_canvas(.width = 4, .height = 4) == 0);
    CHECK(bare_shader(&no_params) == 0);
    bare_pass(.target = 1, .shader = 1);
    bare_image_free(1);
    CHECK_NEAR(bare_image_size(1).x, 0, 0);
}

TEST(invalid_handles_and_target_slots) {
    setup();
    BareImage a = bare_canvas(.width = 8, .height = 8);
    BareImage b = bare_canvas(.width = 8, .height = 8);
    BareShader sh = bare_shader(&no_params);
    frame();
    CHECK(bare_image_size(UINT32_MAX).x == 0);
    bare_image_free(UINT32_MAX);
    bare_shader_free(UINT32_MAX);
    bare_pass(.target = a, .shader = UINT32_MAX);
    bare_pass(.target = a, .shader = sh, .extra_targets = { 0, b });
    CHECK(strstr(last_log, "contiguous") != NULL);
    CHECK(frame()->op_count == 0);
    bare_pass(.target = a, .shader = sh, .extra_targets = { b });
    CHECK(frame()->op_count == 1);
    teardown();
}

static const BareShaderCode program = { .spirv = fake_spirv, .spirv_size = 4, .samplers = 1,
                                        .vert_spirv = { fake_spirv, fake_spirv }, .vert_spirv_size = { 4, 4 },
                                        .vert_inputs = 1, .vert_samplers = 1 };

static const BareShaderCode sixteen = { .spirv = fake_spirv, .spirv_size = 4, .samplers = 16 };

TEST(a_pass_takes_sixteen_inputs) {
    setup();
    BareImage c[17];
    for (int i = 0; i < 17; i++) c[i] = bare_canvas(.width = 8, .height = 8);
    BareShader sh = bare_shader(&sixteen);
    bare_pass(.target = c[16], .shader = sh, .inputs = { c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9],
                                                         c[10], c[11], c[12], c[13], c[14], c[15] });
    const BareDrawList *l = frame();
    const BareGpuPass *g = &l->ops[l->op_count - 1].as.pass;
    CHECK(l->ops[l->op_count - 1].kind == BARE_GPU_PASS && g->input_count == 16 && g->inputs[15] == c[15]);
    CHECK(log_count == 0);
    teardown();
}

TEST(canvases_ask_for_mipmaps) {
    setup();
    BareImage a = bare_canvas(.width = 64, .height = 16, .format = BARE_RGBA16F, .repeat = true, .mipmaps = true);
    BareImage b = bare_canvas(.width = 64, .height = 16);
    BareImage z = bare_canvas(.width = 64, .height = 16, .format = BARE_DEPTH, .mipmaps = true);
    const BareDrawList *l = frame();
    CHECK(a && b && z && l->op_count == 3);
    CHECK(l->ops[0].as.image.mipmaps && l->ops[0].as.image.repeat);
    CHECK(!l->ops[1].as.image.mipmaps);
    CHECK(!l->ops[2].as.image.mipmaps && log_count == 1 && strstr(last_log, "mipmaps"));   /* depth: ignored, said */
    teardown();
}

TEST(draws_test_and_write_depth) {
    setup();
    BareImage scene = bare_canvas(.width = 64, .height = 32, .format = BARE_RGBA16F);
    BareImage zbuf = bare_canvas(.width = 64, .height = 32, .format = BARE_DEPTH);
    BareImage small = bare_canvas(.width = 32, .height = 32, .format = BARE_DEPTH);
    BareShader sh = bare_shader(&program);
    float tri[3][4] = { { 0 } };
    const BareDrawList *l = frame();
    CHECK(l->op_count == 4 && l->ops[1].as.image.format == BARE_DEPTH && l->ops[1].as.image.renderable);

    bare_draw(.target = scene, .shader = sh, .depth = zbuf, .clear_depth = true, BARE_VERTICES(tri, 3));
    bare_draw(.target = scene, .shader = sh, .depth = zbuf, .depth_read_only = true,
              .depth_test = BARE_DEPTH_LESS_EQUAL, BARE_VERTICES(tri, 3));
    bare_draw(.target = scene, .shader = sh, .depth = zbuf, .clear_depth = true, .depth_clear = BARE_ZERO,
              .depth_test = BARE_DEPTH_GREATER, BARE_VERTICES(tri, 3));
    bare_draw(.target = scene, .shader = sh, .depth = zbuf, .clear = true, .clear_depth = true,
              BARE_VERTICES(tri, 0));   /* clears only, as gl.clear */
    bare_draw(.target = scene, .shader = sh, BARE_VERTICES(tri, 3));
    CHECK(log_count == 0);
    l = frame();
    CHECK(l->op_count == 5);
    const BareGpuPass *a = &l->ops[0].as.pass, *b = &l->ops[1].as.pass, *c = &l->ops[2].as.pass;
    CHECK(a->depth == zbuf && a->depth_write && a->clear_depth && a->depth_test == BARE_DEPTH_LESS);
    CHECK_NEAR(a->depth_clear, 1, 0);
    CHECK(b->depth == zbuf && !b->depth_write && !b->clear_depth && b->depth_test == BARE_DEPTH_LESS_EQUAL);
    CHECK(c->depth_test == BARE_DEPTH_GREATER && c->depth_clear == 0);
    CHECK(l->ops[3].as.pass.vertex_count == 0 && l->ops[3].as.pass.clear && l->ops[3].as.pass.clear_depth);
    CHECK(l->ops[4].as.pass.depth == 0 && !l->ops[4].as.pass.depth_write);

    /* mistakes: wrong size, not a depth canvas, depth as target or input */
    bare_draw(.target = scene, .shader = sh, .depth = small, BARE_VERTICES(tri, 3));
    CHECK(strstr(last_log, "must match") != NULL);
    bare_draw(.target = scene, .shader = sh, .depth = scene, BARE_VERTICES(tri, 3));
    CHECK(strstr(last_log, "BARE_DEPTH") != NULL);
    bare_draw(.target = zbuf, .shader = sh, BARE_VERTICES(tri, 3));
    CHECK(strstr(last_log, "as .depth") != NULL);
    bare_draw(.target = scene, .shader = sh, .depth = zbuf, .inputs = { zbuf }, BARE_VERTICES(tri, 3));
    CHECK(strstr(last_log, "also .depth") != NULL);
    bare_draw(.target = scene, .shader = sh, .depth_read_only = true, BARE_VERTICES(tri, 3));
    CHECK(strstr(last_log, "BARE_DEPTH") != NULL);
    CHECK(log_count == 5);
    CHECK(frame()->op_count == 0);

    /* a depth canvas is an input like any other in a later draw or pass */
    bare_draw(.target = scene, .shader = sh, .inputs = { zbuf }, BARE_VERTICES(tri, 3));
    CHECK(frame()->op_count == 1);
    teardown();
}

int main(void) {
    RUN(a_pass_takes_sixteen_inputs);
    RUN(canvases_ask_for_mipmaps);
    RUN(draws_test_and_write_depth);
    RUN(invalid_handles_and_target_slots);
    RUN(ops_reach_the_next_draw_list_once_and_in_order);
    RUN(passes_every_frame_reuse_memory);
    RUN(mistakes_are_reported_and_skipped);
    RUN(freeing_queues_destroy_and_recycles_ids);
    RUN(image_boxes_draw_between_background_and_border);
    RUN(cover_fit_crops_to_the_box_shape);
    RUN(nothing_without_a_context);
    return test_report();
}
