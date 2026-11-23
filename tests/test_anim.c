/* Animation, headless: easing curves, springs and the channels that move
 * elements, stepped with a fixed clock. */
#include "bare_test.h"
#include "bare.h"
#include "internal.h"
#include <string.h>

#define FRAME (1.0 / 60.0)

static BareContext *ctx;
static double now;
static bool animating;

static void begin(void) {
    if (!ctx) {
        ctx = bare_ctx_create(NULL);
        bare_ctx_make_current(ctx);
        now = 0;
    }
    bare_ctx_begin(ctx, 800, 600, 1.0f, now);
}
static const BareDrawList *end(void) {
    const BareDrawList *l = bare_ctx_end(ctx, &animating);
    now += FRAME;
    return l;
}
static void reset(void) { bare_ctx_destroy(ctx); ctx = NULL; }

static void move(float x, float y) {
    BareEvent e = { .kind = BARE_EVENT_MOUSE_MOVE };
    e.as.mouse.x = x; e.as.mouse.y = y;
    bare_ctx_event(ctx, &e);
}

/* The element with this id as drawn in the last frame. */
static const BareElem *elem(BareId id) {
    for (int i = 0; i < ctx->elem_count; i++)
        if (ctx->elems[i].id == id) return &ctx->elems[i];
    return NULL;
}

/* ---- easing --------------------------------------------------------------- */

TEST(bezier_matches_css) {
    /* Reference values: the curve solved by high-precision bisection. */
    const float ease[4] = { 0.25f, 0.1f, 0.25f, 1.0f };
    CHECK_NEAR(bare__bezier(ease, 0.25f), 0.40851, 1e-4);
    CHECK_NEAR(bare__bezier(ease, 0.5f), 0.80240, 1e-4);
    CHECK_NEAR(bare__bezier(ease, 0.9f), 0.99432, 1e-4);
    const float in_out[4] = { BARE_EASE_IN_OUT };
    CHECK_NEAR(bare__bezier(in_out, 0.1f), 0.01972, 1e-4);
    CHECK_NEAR(bare__bezier(in_out, 0.5f), 0.5, 1e-4);
    CHECK_NEAR(bare__bezier(in_out, 0.75f), 0.87084, 1e-4);
    const float out[4] = { BARE_EASE_OUT };
    CHECK_NEAR(bare__bezier(out, 0.25f), 0.37814, 1e-4);
    const float back[4] = { BARE_BEZIER(0.68f, -0.55f, 0.27f, 1.55f) };   /* overshoots both ends */
    CHECK_NEAR(bare__bezier(back, 0.25f), -0.08294, 1e-4);
    CHECK_NEAR(bare__bezier(back, 0.75f), 1.08882, 1e-4);
    const float linear[4] = { BARE_LINEAR };
    CHECK_NEAR(bare__bezier(linear, 0.3f), 0.3, 1e-6);
    CHECK(bare__bezier(ease, 0.0f) == 0.0f && bare__bezier(ease, 1.0f) == 1.0f);
    CHECK(bare__bezier(ease, -1.0f) == 0.0f && bare__bezier(ease, 2.0f) == 1.0f);
}

/* ---- springs ---------------------------------------------------------------- */

static void run_spring(float *x, float *v, float response, float damping, float secs, float dt, float *lo, float *hi) {
    for (float t = 0; t < secs; t += dt) {
        bare__spring_step(x, v, response, damping, dt);
        if (lo && *x < *lo) *lo = *x;
        if (hi && *x > *hi) *hi = *x;
    }
}

TEST(spring_reaches_target) {
    float damp[] = { 0.3f, 0.7f, 1.0f, 2.0f };
    for (int i = 0; i < 4; i++) {
        float x = 100, v = 0;
        run_spring(&x, &v, 0.4f, damp[i], 4.0f, 1.0f / 60, NULL, NULL);
        CHECK_NEAR(x, 0, 0.01);
        CHECK_NEAR(v, 0, 0.1);
    }
}

TEST(critical_damping_never_overshoots) {
    float x = 100, v = 0, lo = 100;
    run_spring(&x, &v, 0.35f, 1.0f, 3.0f, 1.0f / 60, &lo, NULL);
    CHECK(lo >= 0);
    x = 100, v = 0, lo = 100;   /* overdamped too */
    run_spring(&x, &v, 0.35f, 1.8f, 3.0f, 1.0f / 60, &lo, NULL);
    CHECK(lo >= 0);
}

TEST(underdamped_overshoots_then_settles) {
    float x = 100, v = 0, lo = 100;
    run_spring(&x, &v, 0.45f, 0.55f, 0.5f, 1.0f / 120, &lo, NULL);
    CHECK(lo < -5);                 /* went past the target */
    run_spring(&x, &v, 0.45f, 0.55f, 3.0f, 1.0f / 120, NULL, NULL);
    CHECK_NEAR(x, 0, 0.01);
}

TEST(spring_is_independent_of_the_step) {
    float damp[] = { 0.5f, 1.0f, 1.5f };
    for (int i = 0; i < 3; i++) {
        float x1 = 80, v1 = -300, x2 = 80, v2 = -300;
        bare__spring_step(&x1, &v1, 0.3f, damp[i], 0.1f);
        for (int k = 0; k < 10; k++) bare__spring_step(&x2, &v2, 0.3f, damp[i], 0.01f);
        CHECK_NEAR(x1, x2, 1e-3);
        CHECK_NEAR(v1, v2, 1e-2);
    }
}

/* ---- elements ------------------------------------------------------------- */

/* A box whose width (and colour) switch with `wide`. */
static const BareElem *bar(bool wide, BareAnim a) {
    begin();
    bare_box(.id = BARE_ID("bar"), .width = wide ? 300.0f : 100.0f, .height = 20, .animate = a,
             .color = wide ? BARE_RGB(200, 0, 0) : BARE_RGB(0, 0, 200));
    end();
    return elem(BARE_ID("bar"));
}

TEST(first_appearance_does_not_animate) {
    const BareElem *e = bar(true, BARE_SPRING_SMOOTH);
    CHECK_NEAR(e->rect.w, 300, 0);
    CHECK(e->box.color.r == 200);
    CHECK(!animating);
    reset();
}

TEST(no_animation_is_instant) {
    bar(false, (BareAnim){ 0 });
    const BareElem *e = bar(true, (BareAnim){ 0 });
    CHECK_NEAR(e->rect.w, 300, 0);
    CHECK(!animating);
    reset();
}

TEST(ease_follows_the_curve_and_lands_on_time) {
    BareAnim a = BARE_EASE(0.5f, BARE_LINEAR);
    bar(false, a);
    const BareElem *e = bar(true, a);
    /* The first frame after idle moves one frame's worth. */
    CHECK_NEAR(e->rect.w, 100 + 200 * (FRAME / 0.5), 0.01);
    CHECK(animating);
    for (int i = 0; i < 14; i++) e = bar(true, a);   /* 15 frames = 0.25 s */
    CHECK_NEAR(e->rect.w, 200, 0.05);
    CHECK_NEAR(e->box.color.r, 100, 1);           /* colours move with it */
    CHECK_NEAR(e->box.color.b, 100, 1);
    for (int i = 0; i < 15; i++) e = bar(true, a);
    CHECK_NEAR(e->rect.w, 300, 0);
    CHECK(!animating);                             /* landed: idle again */
    reset();
}

TEST(spring_settles_and_clears_active) {
    bar(false, BARE_SPRING_BOUNCY);
    const BareElem *e = bar(true, BARE_SPRING_BOUNCY);
    float most = 0;
    int frames = 1;
    while (animating && frames < 600) {
        e = bar(true, BARE_SPRING_BOUNCY);
        most = fmaxf(most, e->rect.w);
        frames++;
    }
    CHECK(most > 310);                   /* bouncy: overshot */
    CHECK(frames < 600);
    CHECK(!animating);
    CHECK(e->rect.w == 300);             /* snapped exactly */
    CHECK(ctx->anims[ctx->store.nodes[e->node].anim - 1].active == 0);
    reset();
}

TEST(retarget_keeps_value_and_velocity) {
    BareAnim spring = BARE_SPRING(0.4f, 0.8f);
    bar(false, spring);
    const BareElem *e = NULL;
    for (int i = 0; i < 8; i++) e = bar(true, spring);
    float w = e->rect.w;
    const BareAnimSet *s = &ctx->anims[ctx->store.nodes[e->node].anim - 1];
    float v = s->ch[BARE__CH_W].velocity;
    CHECK(v > 100);                      /* heading out fast */
    e = bar(false, spring);              /* back again, mid-flight */
    float step = e->rect.w - w;
    CHECK(step > 0);                     /* still moving outwards: velocity carried over */
    CHECK(step < v * FRAME * 1.01f);     /* and no jump */

    /* Easing restarts from where it is: no jump either. */
    BareAnim ease = BARE_EASE(0.3f, BARE_EASE_IN_OUT);
    for (int i = 0; i < 60; i++) bar(false, ease);
    for (int i = 0; i < 6; i++) e = bar(true, ease);
    w = e->rect.w;
    e = bar(false, ease);
    CHECK(fabsf(e->rect.w - w) < 5);
    reset();
}

TEST(resuming_from_idle_takes_one_frame_not_the_whole_sleep) {
    BareAnim a = BARE_EASE(0.5f, BARE_LINEAR);
    bar(false, a);
    now += 3.0;                          /* the app slept for 3 s */
    const BareElem *e = bar(true, a);
    CHECK_NEAR(e->rect.w, 100 + 200 * (FRAME / 0.5), 0.01);
    reset();
}

TEST(zero_means_default_and_bare_zero_means_instant) {
    BareAnim def = { .kind = BARE_ANIM_EASE };   /* 0.25 s, CSS `ease` */
    bar(false, def);
    const BareElem *e = bar(true, def);
    CHECK(e->rect.w > 100 && e->rect.w < 300);
    for (int i = 0; i < 15; i++) e = bar(true, def);
    CHECK_NEAR(e->rect.w, 300, 0);
    CHECK(!animating);

    BareAnim instant = { .kind = BARE_ANIM_EASE, .duration = BARE_ZERO };
    e = bar(false, instant);
    CHECK_NEAR(e->rect.w, 100, 0);
    CHECK(!animating);
    reset();
}

/* A parent that moves with a spring, with a child that has no animation. */
static void family(float x) {
    begin();
    bare_row(.id = BARE_ID("parent"), .pad = { 0, 0, 0, x }, .animate = BARE_SPRING_SMOOTH) {
        bare_column(.id = BARE_ID("inner"), .width = 200, .height = 100, .padding = 10,
                    .animate_layout = BARE_SPRING_SMOOTH) {
            bare_box(.id = BARE_ID("child"), .width = 50, .height = 50);
        }
    }
    end();
}

TEST(a_moving_parent_carries_its_children) {
    family(0);
    family(300);
    const BareElem *inner = elem(BARE_ID("inner")), *child = elem(BARE_ID("child"));
    CHECK(inner->rect.x > 0 && inner->rect.x < 300);
    CHECK(inner->moving && child->moving);
    CHECK_NEAR(child->rect.x - inner->rect.x, 10, 1e-4);   /* rigidly, not lagging */
    for (int i = 0; i < 120; i++) family(300);
    CHECK_NEAR(inner->rect.x, 300, 0);
    CHECK_NEAR(child->rect.x, 310, 0);
    CHECK(!child->moving);
    reset();
}

/* "dot" under or over "dot2", gliding with a slow linear ease. */
static BareState dots(bool dot_first) {
    BareState s;
    begin();
    if (!dot_first) bare_box(.id = BARE_ID("dot2"), .width = 40, .height = 40, .color = BARE_WHITE);
    s = bare_box(.id = BARE_ID("dot"), .width = 40, .height = 40, .color = BARE_WHITE,
                 .animate = BARE_EASE(1.0f, BARE_LINEAR));
    if (dot_first) bare_box(.id = BARE_ID("dot2"), .width = 40, .height = 40, .color = BARE_WHITE);
    return s;
}

TEST(moving_elements_are_drawn_unsnapped_and_hit_where_drawn) {
    dots(true); end();
    dots(false);                         /* swapped: "dot" glides from y = 0 to y = 40 */
    const BareDrawList *l = end();
    float y = l->cmds[1].as.rect.rect.y;
    CHECK(y > 0 && y < 1);               /* a fraction of a pixel: not rounded */
    BareState s;
    for (int i = 0; i < 29; i++) { dots(false); end(); }   /* half way: drawn at y = 20 */
    s = dots(false); end();
    CHECK_NEAR(s.rect.y, 20, 0.01);
    move(20, 25);                        /* where it's drawn, not where layout put it (40) */
    s = dots(false); end();
    CHECK(s.hovered);
    reset();
}

TEST(hover_looks_animate) {
    for (int f = 0; f < 2; f++) {
        if (f) move(50, 50);
        begin();
        bare_box(.id = BARE_ID("b"), .width = 100, .height = 100, .color = BARE_RGB(0, 0, 0),
                 .hover = { .scale = 1.2f, .color = BARE_RGB(250, 250, 250) }, .animate = BARE_SPRING_SNAPPY);
        end();
    }
    const BareElem *e = elem(BARE_ID("b"));
    CHECK(e->box.scale > 1.0f && e->box.scale < 1.2f);
    CHECK(e->box.color.r > 0 && e->box.color.r < 250);
    CHECK(animating);
    reset();
}

TEST(fading_to_transparent_keeps_the_colour) {
    BareAnim a = BARE_EASE(0.5f, BARE_LINEAR);
    for (int f = 0; f < 10; f++) {
        begin();
        bare_box(.id = BARE_ID("f"), .width = 10, .height = 10, .animate = a,
                 .color = f ? BARE_RGBA(0, 0, 0, 0) : BARE_RGB(255, 0, 0),
                 .shadow = f ? (BareShadow){ 0 } : BARE_SHADOW_LARGE);
        end();
    }
    const BareElem *e = elem(BARE_ID("f"));
    CHECK(e->box.color.r == 255);        /* red fading out, not darkening */
    CHECK(e->box.color.a > 0 && e->box.color.a < 255);
    CHECK_NEAR(e->box.shadow.blur, 48, 1e-3);   /* the shadow fades in place */
    CHECK(e->box.shadow.color.a > 0);

    /* Fading back in from nothing starts from the new colour. */
    for (int f = 0; f < 60; f++) {
        begin();
        bare_box(.id = BARE_ID("f"), .width = 10, .height = 10, .animate = a, .color = BARE_RGBA(0, 0, 0, 0));
        end();
    }
    begin();
    bare_box(.id = BARE_ID("f"), .width = 10, .height = 10, .animate = a, .color = BARE_RGB(0, 200, 0));
    end();
    e = elem(BARE_ID("f"));
    CHECK(e->box.color.g == 200 && e->box.color.r == 0);
    reset();
}

TEST(opacity_animates_into_children) {
    for (int f = 0; f < 4; f++) {
        begin();
        bare_column(.id = BARE_ID("p"), .opacity = f ? BARE_ZERO : 1.0f, .animate = BARE_EASE(0.5f, BARE_LINEAR)) {
            bare_box(.id = BARE_ID("c"), .width = 10, .height = 10, .color = BARE_WHITE);
        }
        end();
    }
    const BareElem *c = elem(BARE_ID("c"));
    CHECK(c->opacity > 0.8f && c->opacity < 1.0f);
    reset();
}

/* Two columns; the item lives in one of them. */
static void columns(int which) {
    begin();
    bare_row(.gap = 100) {
        for (int k = 0; k < 2; k++) {
            bare_column(.id = BARE_IDI("col", k), .width = 200, .height = 400, .pad = { 50 * (float)k, 0, 0, 0 }) {
                if (k == which)
                    bare_box(.id = BARE_ID("item"), .width = 50, .height = 50,
                             .animate = BARE_SPRING_SMOOTH);
            }
        }
    }
    end();
}

TEST(moving_to_another_parent_glides_from_where_it_was) {
    columns(0);
    columns(1);
    const BareElem *e = elem(BARE_ID("item"));
    CHECK(e->rect.x < 50 && e->rect.y < 10);       /* still near the old place */
    for (int i = 0; i < 120; i++) columns(1);
    e = elem(BARE_ID("item"));
    CHECK_NEAR(e->rect.x, 300, 0);
    CHECK_NEAR(e->rect.y, 50, 0);
    reset();
}

TEST(channel_sets_are_reused) {
    for (int round = 0; round < 3; round++) {
        for (int f = 0; f < 2; f++) {
            begin();
            for (int i = 0; i < 20; i++)
                bare_box(.id = BARE_IDI(round % 2 ? "a" : "b", i), .width = 10, .height = 10,
                         .animate = BARE_SPRING_SMOOTH);
            end();
        }
    }
    CHECK(ctx->anim_count <= 40);   /* evicted sets went back to the pool */
    int cap = ctx->anim_cap;
    for (int f = 0; f < 50; f++) {
        begin();
        for (int i = 0; i < 20; i++)
            bare_box(.id = BARE_IDI("a", i), .width = 10 + (float)(f % 3), .height = 10,
                     .animate = BARE_SPRING_SMOOTH);
        end();
    }
    CHECK(ctx->anim_cap == cap);    /* steady state: no growth */
    reset();
}

int main(void) {
    RUN(bezier_matches_css);
    RUN(spring_reaches_target);
    RUN(critical_damping_never_overshoots);
    RUN(underdamped_overshoots_then_settles);
    RUN(spring_is_independent_of_the_step);
    RUN(first_appearance_does_not_animate);
    RUN(no_animation_is_instant);
    RUN(ease_follows_the_curve_and_lands_on_time);
    RUN(spring_settles_and_clears_active);
    RUN(retarget_keeps_value_and_velocity);
    RUN(resuming_from_idle_takes_one_frame_not_the_whole_sleep);
    RUN(zero_means_default_and_bare_zero_means_instant);
    RUN(a_moving_parent_carries_its_children);
    RUN(moving_elements_are_drawn_unsnapped_and_hit_where_drawn);
    RUN(hover_looks_animate);
    RUN(fading_to_transparent_keeps_the_colour);
    RUN(opacity_animates_into_children);
    RUN(moving_to_another_parent_glides_from_where_it_was);
    RUN(channel_sets_are_reused);
    return test_report();
}
