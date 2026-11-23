#include "bare_test.h"
#include "bare.h"
#include "bare/bare_core.h"
#include <string.h>

#ifdef _MSC_VER
#pragma warning(disable : 4702)   /* unreachable: the deliberate `break` inside a container */
#endif

static BareContext *ctx;
static char last_log[512];
static int log_count;

static void capture_log(void *user, const char *msg) {
    (void)user;
    snprintf(last_log, sizeof last_log, "%s", msg);
    log_count++;
}

static void begin(float scale) {
    if (!ctx) {
        ctx = bare_ctx_create(NULL);
        bare_ctx_set_log(ctx, capture_log, NULL);
        bare_ctx_make_current(ctx);
    }
    log_count = 0;
    last_log[0] = 0;
    bare_ctx_begin(ctx, 800, 600, scale, 0.0);
}

TEST(emit_hello_box) {
    begin(1.0f);
    bare_box(.width = 300, .height = 200, .radius = 16, .color = BARE_RGB(40, 120, 255));
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(l->cmd_count == 1);
    CHECK(l->width == 800 && l->height == 600);
    const BareRectCmd *r = &l->cmds[0].as.rect;
    CHECK(l->cmds[0].kind == BARE_CMD_RECT);
    CHECK_NEAR(r->rect.x, 0, 0);
    CHECK_NEAR(r->rect.w, 300, 0);
    CHECK_NEAR(r->rect.h, 200, 0);
    CHECK_NEAR(r->radius[0], 16, 0);
    CHECK_NEAR(r->radius[3], 16, 0);
    CHECK_NEAR(r->fill.b, 1.0, 1e-6);
    CHECK_NEAR(r->fill.a, 1.0, 1e-6);
    CHECK_NEAR(r->shadow_color.a, 0, 0);
}

TEST(emit_scales_to_physical_pixels) {
    begin(2.0f);
    bare_box(.width = 300, .height = 200, .radius = 16, .border = 2,
             .border_color = BARE_WHITE, .shadow = BARE_SHADOW_SOFT);
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(l->width == 1600 && l->height == 1200);
    CHECK(l->cmd_count == 1);
    const BareRectCmd *r = &l->cmds[0].as.rect;
    CHECK_NEAR(r->rect.w, 600, 0);
    CHECK_NEAR(r->rect.h, 400, 0);
    CHECK_NEAR(r->radius[0], 32, 0);
    CHECK_NEAR(r->border, 4, 0);
    CHECK_NEAR(r->shadow[1], 16, 0);   /* y offset 8 */
    CHECK_NEAR(r->shadow[2], 48, 0);   /* blur 24 */
    CHECK_NEAR(r->fill.a, 0, 0);       /* no background colour */
}

TEST(emit_snaps_edges_at_fractional_scale) {
    begin(1.5f);
    bare_column() {
        bare_box(.width = 11, .height = 11, .color = BARE_WHITE);
        bare_box(.width = 11, .height = 11, .color = BARE_WHITE);
    }
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(l->cmd_count == 2);
    const BareRect a = l->cmds[0].as.rect.rect, b = l->cmds[1].as.rect.rect;
    CHECK(a.x == (float)(int)a.x && a.h == (float)(int)a.h);
    CHECK_NEAR(a.y + a.h, b.y, 0);     /* no gap, no overlap */
}

TEST(emit_skips_invisible_boxes) {
    begin(1.0f);
    bare_box(.width = 100, .height = 100);                                       /* nothing to draw */
    bare_box(.width = 100, .height = 100, .color = BARE_WHITE, .opacity = BARE_ZERO);
    bare_box(.width = 0, .height = 0, .color = BARE_WHITE);                     /* empty */
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(l->cmd_count == 0);
}

TEST(emit_opacity_multiplies_into_children) {
    begin(1.0f);
    bare_column(.opacity = 0.5f) {
        bare_box(.width = 10, .height = 10, .color = BARE_WHITE, .opacity = 0.5f);
    }
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(l->cmd_count == 1);
    CHECK_NEAR(l->cmds[0].as.rect.fill.a, 0.25, 1e-6);
}

TEST(emit_clamps_radius_and_border) {
    begin(1.0f);
    bare_box(.width = 40, .height = 20, .radius = 100, .border = 50, .border_color = BARE_WHITE);
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(l->cmd_count == 1);
    CHECK_NEAR(l->cmds[0].as.rect.radius[0], 10, 0);
    CHECK_NEAR(l->cmds[0].as.rect.border, 10, 0);
}

TEST(stub_layout_stacks_with_padding_and_gap) {
    begin(1.0f);
    bare_row(.padding = 10, .gap = 5, .color = BARE_BLACK) {
        bare_box(.width = 20, .height = 30, .color = BARE_WHITE);
        bare_box(.width = 40, .height = 10, .color = BARE_WHITE);
    }
    bare_box(.width = 7, .height = 7, .color = BARE_WHITE);
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(l->cmd_count == 4);
    BareRect row = l->cmds[0].as.rect.rect;
    CHECK_NEAR(row.w, 10 + 20 + 5 + 40 + 10, 0);
    CHECK_NEAR(row.h, 10 + 30 + 10, 0);
    CHECK_NEAR(l->cmds[1].as.rect.rect.x, 10, 0);
    CHECK_NEAR(l->cmds[2].as.rect.rect.x, 35, 0);
    CHECK_NEAR(l->cmds[3].as.rect.rect.y, 50, 0);   /* below the row */
}

TEST(unclosed_container_is_reported) {
    begin(1.0f);
    for (int i = 0; i < 1; i++) {
        bare_column() { break; }   /* the mistake bare.h warns about */
    }
    bare_ctx_end(ctx, NULL);
    CHECK(log_count == 1);
    CHECK(strstr(last_log, "never closed") != NULL);
    CHECK(strstr(last_log, "test_emit.c") != NULL);
}

TEST(element_outside_frame_is_reported) {
    begin(1.0f);
    bare_ctx_end(ctx, NULL);
    bare_box(.width = 10, .height = 10);
    CHECK(log_count == 1);
    CHECK(strstr(last_log, "outside a frame") != NULL);
}

TEST(stack_layers_children) {
    begin(1.0f);
    bare_stack(.align = BARE_CENTER, .color = BARE_BLACK) {
        bare_box(.width = 100, .height = 50, .color = BARE_WHITE);
        bare_box(.width = 20, .height = 10, .color = BARE_WHITE);
    }
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(l->cmd_count == 3);
    CHECK_NEAR(l->cmds[0].as.rect.rect.w, 100, 0);          /* stack fits its biggest layer */
    CHECK_NEAR(l->cmds[0].as.rect.rect.h, 50, 0);
    CHECK_NEAR(l->cmds[2].as.rect.rect.x, 40, 0);           /* centred on both axes */
    CHECK_NEAR(l->cmds[2].as.rect.rect.y, 20, 0);
}

TEST(offset_moves_children_without_snapping) {
    begin(2.0f);
    bare_column(.offset = { 10.25f, -3.0f }) {
        bare_box(.width = 10, .height = 10, .color = BARE_WHITE);
    }
    bare_box(.width = 10, .height = 10, .color = BARE_WHITE);   /* layout ignores the offset */
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(l->cmd_count == 2);
    CHECK_NEAR(l->cmds[0].as.rect.rect.x, 20.5, 1e-6);
    CHECK_NEAR(l->cmds[0].as.rect.rect.y, -6, 1e-6);
    CHECK_NEAR(l->cmds[1].as.rect.rect.y, 20, 0);
}

TEST(glass_defaults) {
    begin(2.0f);
    bare_box(.width = 100, .height = 50, .radius = 25, .glass = BARE_GLASS);
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(l->cmd_count == 1);
    CHECK(l->cmds[0].kind == BARE_CMD_GLASS);
    const BareGlassCmd *g = &l->cmds[0].as.glass;
    CHECK(g->count == 1);
    CHECK_NEAR(g->shapes[0].rect.w, 200, 0);
    CHECK_NEAR(g->shapes[0].radius, 50, 0);
    CHECK_NEAR(g->refraction, 1, 0);
    CHECK_NEAR(g->depth, 40, 0);       /* 20 px at 2x */
    CHECK_NEAR(g->blur, 4, 0);
    CHECK_NEAR(g->dispersion, 1, 0);
    CHECK_NEAR(g->merge, 0, 0);
}

TEST(glass_zero_and_overrides) {
    begin(1.0f);
    bare_box(.width = 10, .height = 10, .glass = { .blur = BARE_ZERO, .dispersion = BARE_ZERO, .depth = 8 });
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(l->cmd_count == 1);
    const BareGlassCmd *g = &l->cmds[0].as.glass;
    CHECK_NEAR(g->blur, 0, 0);
    CHECK_NEAR(g->dispersion, 0, 0);
    CHECK_NEAR(g->depth, 8, 0);
    CHECK_NEAR(g->refraction, 1, 0);   /* still the default: blur/depth turned glass on */
}

TEST(glass_siblings_merge) {
    begin(1.0f);
    bare_stack() {
        bare_box(.width = 10, .height = 10, .glass = { .merge = 20 });
        bare_box(.width = 10, .height = 10, .glass = { .merge = 20 }, .offset = { 15, 0 });
        bare_box(.width = 10, .height = 10, .glass = { .merge = 20 }, .offset = { 30, 0 });
        bare_box(.width = 10, .height = 10, .glass = BARE_GLASS);   /* no merge: its own command */
    }
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(l->cmd_count == 2);
    CHECK(l->cmds[0].as.glass.count == 3);
    CHECK_NEAR(l->cmds[0].as.glass.shapes[2].rect.x, 30, 0);
    CHECK(l->cmds[1].as.glass.count == 1);
}

TEST(glass_draws_frost_on_top_and_shadow_inside) {
    begin(1.0f);
    bare_box(.width = 50, .height = 50, .glass = BARE_GLASS, .color = BARE_RGBA(255, 255, 255, 40),
             .shadow = BARE_SHADOW_SOFT);
    const BareDrawList *l = bare_ctx_end(ctx, NULL);
    CHECK(l->cmd_count == 2);
    CHECK(l->cmds[0].kind == BARE_CMD_GLASS);
    CHECK(l->cmds[0].as.glass.shadow_color.a > 0);       /* the glass draws the shadow */
    CHECK(l->cmds[1].kind == BARE_CMD_RECT);
    CHECK_NEAR(l->cmds[1].as.rect.shadow_color.a, 0, 0); /* ...so the frost doesn't */
}

int main(void) {
    RUN(stack_layers_children);
    RUN(offset_moves_children_without_snapping);
    RUN(glass_defaults);
    RUN(glass_zero_and_overrides);
    RUN(glass_siblings_merge);
    RUN(glass_draws_frost_on_top_and_shadow_inside);
    RUN(emit_hello_box);
    RUN(emit_scales_to_physical_pixels);
    RUN(emit_snaps_edges_at_fractional_scale);
    RUN(emit_skips_invisible_boxes);
    RUN(emit_opacity_multiplies_into_children);
    RUN(emit_clamps_radius_and_border);
    RUN(stub_layout_stacks_with_padding_and_gap);
    RUN(unclosed_container_is_reported);
    RUN(element_outside_frame_is_reported);
    bare_ctx_destroy(ctx);
    return test_report();
}
