/* glass_motion.c: liquid glass in motion (M4 springs and eases on M1.5 glass).
 *
 * One portrait screen looping through five moments, each a glass control
 * animated by Bare alone (no hand-written tweening):
 *
 *   1. a round lens condenses out of the dunes, shows a play icon, then
 *      dissolves back into the sand
 *   2. a glass capsule over petals, its rim light turning
 *   3. a loupe rests on a colour wheel as a ring, lifts into a magnifying
 *      glass lens and glides around the wheel
 *   4. the same element turns red and morphs into a capsule button; a
 *      checkerboard rolls in behind it to show the tinted glass bending it
 *   5. a switch whose knob turns to glass while held and slides back and forth
 *
 * Backdrops are the example's own shaders (shaders/). Text arrives in M5:
 * bars stand in for labels. `glass_motion 4.6` starts 4.6 s into the loop. */
#include "bare.h"
#include "glass_motion_shaders.h"
#include <math.h>
#include <stdlib.h>

#define LOOP 11.0f

typedef struct { float time, aspect; } SceneParams;   /* std140: two floats */
typedef struct { float kind, size; } IconParams;

static BareImage play_icon, plus_icon;
static float W, H;   /* window size */

static const BareColor white = { 255, 255, 255, 255 };

/* ---- 1: the lens over the dunes ---------------------------------------- */

static void lens_scene(BareImage dunes, float t) {
    bool lens = t >= 0.10f && t < 1.20f;
    bool icon = t >= 0.22f && t < 1.10f;
    float d = 0.64f * W;
    bare_stack(.width = BARE_GROW, .height = BARE_GROW, .align = BARE_CENTER, .image = dunes) {
        bare_stack(.id = BARE_ID("lens"), .width = d, .height = d, .radius = d / 2, .align = BARE_CENTER,
                   .glass = lens ? (BareGlass){ .refraction = 1.1f, .depth = 0.09f * W, .blur = BARE_ZERO,
                                                .shine = 1.2f }
                                 : (BareGlass){ 0 },
                   .animate = BARE_EASE(0.14f, BARE_EASE_OUT)) {
            bare_box(.id = BARE_ID("lens.icon"), .width = 0.2f * W, .height = 0.2f * W, .image = play_icon,
                     .opacity = icon ? 1.0f : BARE_ZERO, .scale = icon ? 1.0f : 0.85f,
                     .animate = BARE_EASE(0.12f, BARE_EASE_OUT));
        }
    }
}

/* ---- 2: the capsule over petals ----------------------------------------- */

static void capsule_scene(BareImage petals, float t) {
    float h = 0.27f * W;
    bare_stack(.width = BARE_GROW, .height = BARE_GROW, .align = BARE_CENTER, .image = petals) {
        bare_row(.id = BARE_ID("capsule"), .width = 0.78f * W, .height = h, .radius = h / 2, .gap = 0.035f * W,
                 .justify = BARE_CENTER, .align = BARE_CENTER, .offset = { 0, -0.04f * H },
                 .glass = { .refraction = 1.0f, .depth = 0.07f * W, .blur = 1.5f, .shine = 1.4f,
                            .light = (t - 1.5f) * 70.0f - 40.0f, .tint = { 255, 255, 255, 18 } }) {
            bare_box(.width = 0.09f * W, .height = 0.09f * W, .image = play_icon);
            bare_box(.width = 0.24f * W, .height = 0.075f * W, .radius = 0.02f * W, .color = BARE_RGBA(255, 255, 255, 235));
        }
    }
}

/* ---- 3 and 4: the loupe that becomes a button --------------------------- */

typedef struct { float t, x, y; } Key;   /* where the loupe heads from time t (units of the wheel's radius) */

static const Key path[] = {
    { 2.70f, -0.45f, -0.45f }, { 3.40f, -0.55f, -0.30f }, { 3.80f, -0.50f, -0.40f },
    { 3.95f, 0.00f, -0.85f },  { 4.20f, 0.85f, 0.20f },   { 4.40f, 0.60f, 0.70f },
};

/* Rows open from nothing; the column is centred, so the board opens outward
 * from the button as its rows grow and glide apart. */
static void checker_row(int row, bool shown) {
    float s = W / 6.0f, h = shown ? s : BARE_ZERO;
    BareAnim open = BARE_EASE(0.18f, BARE_EASE_OUT);
    bare_row(.id = BARE_IDI("checks", row), .animate_layout = open) {
        for (int c = 0; c < 6; c++)
            bare_box(.id = BARE_IDI("check", row * 6 + c), .width = s, .height = h, .animate_layout = open,
                     .color = (row + c) % 2 ? BARE_BLACK : BARE_RGBA(0, 0, 0, 0));
    }
}

static void wheel_scene(BareImage wheel, float t) {
    float r = 0.235f * W;   /* the wheel's radius */
    /* Lifted at 3.0; at 4.42 it turns red and drops below the centre as a
     * drop of red glass; at 4.6 it stretches into the button. */
    bool lifted = t >= 3.0f, red = t >= 4.42f, button = t >= 4.6f, label = t >= 4.85f;

    float x = 0, y = 0;
    for (int i = 0; i < (int)(sizeof path / sizeof path[0]); i++)
        if (t >= path[i].t) { x = path[i].x * r; y = path[i].y * r; }
    if (red) { x = 0.05f * r; y = 0.55f * r; }
    if (button) x = y = 0;

    float w = 0.12f * W, h = w;                       /* resting: a ring */
    if (lifted) w = h = 0.24f * W;                    /* lifted: a lens */
    if (red) w = h = 0.2f * W;                        /* a drop */
    if (button) { w = 0.62f * W; h = 0.24f * W; }     /* a capsule button */

    BareGlass glass = { 0 };
    if (lifted) glass = (BareGlass){ .refraction = 0.8f, .zoom = 1.5f, .depth = 0.07f * W, .blur = BARE_ZERO,
                                     .shine = 1.6f, .tint = { 255, 255, 255, 40 } };
    if (red) glass = (BareGlass){ .refraction = 1.1f, .depth = 0.06f * W, .blur = 3.0f, .shine = 1.2f,
                                  .tint = { 240, 0, 22, 236 } };

    bare_stack(.width = BARE_GROW, .height = BARE_GROW, .align = BARE_CENTER, .color = BARE_RGB(236, 238, 237)) {
        if (button) {
            bare_column() {
                for (int i = 0; i < 6; i++) checker_row(i, t >= 6.5f + fabsf((float)i - 2.5f) * 0.08f);
            }
        }
        bare_box(.id = BARE_ID("wheel"), .width = 2 * r, .height = 2 * r, .radius = r, .image = wheel,
                 .opacity = t < 4.45f ? 1.0f : BARE_ZERO, .animate = BARE_EASE(0.2f, BARE_EASE_OUT));
        bare_row(.id = BARE_ID("loupe"), .width = w, .height = h, .radius = h / 2, .gap = 0.03f * W,
                 .justify = BARE_CENTER, .align = BARE_CENTER, .offset = { x, y },
                 .border = lifted ? BARE_ZERO : 0.007f * W, .border_color = white, .glass = glass,
                 .shadow = { 0, lifted ? 0.02f * W : 0.004f * W, lifted ? 0.06f * W : 0.01f * W, 0,
                             BARE_RGBA(0, 0, 0, lifted ? 45 : 60) },
                 .animate = button ? BARE_SPRING(0.5f, 0.62f) : BARE_SPRING(0.42f, 0.7f)) {
            if (button) {
                bare_box(.id = BARE_ID("add.icon"), .width = 0.07f * W, .height = 0.07f * W, .image = plus_icon,
                         .opacity = label ? 1.0f : BARE_ZERO, .animate = BARE_EASE(0.2f, BARE_EASE_OUT));
                bare_box(.id = BARE_ID("add.text"), .width = 0.18f * W, .height = 0.065f * W, .radius = 0.018f * W,
                         .color = BARE_RGBA(255, 255, 255, 240), .opacity = label ? 1.0f : BARE_ZERO,
                         .animate = BARE_EASE(0.2f, BARE_EASE_OUT));
            }
        }
    }
}

/* ---- 5: the switch -------------------------------------------------------- */

static void switch_scene(float t) {
    bool held = t >= 8.0f && t < 10.2f;
    bool on = t < 8.1f || (t >= 8.75f && t < 9.4f);
    float track_w = 0.68f * W, track_h = 0.28f * W;
    float knob_w = held ? 0.52f * W : 0.38f * W, knob_h = held ? 0.36f * W : 0.24f * W;
    float travel = (track_w - 0.38f * W) / 2 - 0.01f * W;

    bare_stack(.width = BARE_GROW, .height = BARE_GROW, .align = BARE_CENTER, .color = BARE_RGB(206, 207, 210)) {
        bare_stack(.width = 1.04f * W, .height = 0.42f * W, .radius = 0.21f * W, .align = BARE_CENTER,
                   .offset = { -0.06f * W, 0 },
                   .color = BARE_RGB(238, 238, 240), .shadow = BARE_SHADOW_SMALL) {
            bare_box(.id = BARE_ID("track"), .width = track_w, .height = track_h, .radius = track_h / 2,
                     .color = on ? BARE_RGB(52, 199, 89) : BARE_RGB(228, 228, 231),
                     .animate = BARE_EASE(0.25f, BARE_EASE_IN_OUT));
            bare_box(.id = BARE_ID("knob"), .width = knob_w, .height = knob_h, .radius = knob_h / 2,
                     .offset = { on ? travel : -travel, 0 },
                     .color = held ? BARE_RGBA(255, 255, 255, 0) : white,
                     .glass = held ? (BareGlass){ .refraction = 1.0f, .depth = 0.05f * W, .zoom = 1.15f,
                                                  .blur = BARE_ZERO, .shine = 1.3f }
                                   : (BareGlass){ 0 },
                     .shadow = { 0, 0.008f * W, 0.03f * W, 0, BARE_RGBA(0, 0, 0, held ? 35 : 55) },
                     .animate = BARE_SPRING(0.38f, 0.72f));
        }
    }
}

int main(int argc, char **argv) {
    float start = argc > 1 ? (float)atof(argv[1]) : 0.0f;
    bare_window(432, 768, "Bare: liquid glass in motion");
    bare_continuous(true);

    /* Backdrops and icons drawn by shaders into canvases. */
    float k = bare_pixel_scale();
    BareVec2 win = bare_window_size();
    int cw = (int)(win.x * k), ch = (int)(win.y * k), iw = (int)(0.2f * win.x * k);
    BareImage dunes = bare_canvas(.width = cw, .height = ch), petals = bare_canvas(.width = cw, .height = ch);
    BareImage wheel = bare_canvas(.width = (int)(0.47f * win.x * k), .height = (int)(0.47f * win.x * k));
    play_icon = bare_canvas(.width = iw, .height = iw);
    plus_icon = bare_canvas(.width = iw, .height = iw);
    BareShader dunes_fx = bare_shader(&dunes_shader), petals_fx = bare_shader(&petals_shader);
    BareShader icon_fx = bare_shader(&icons_shader);
    bare_pass(.target = wheel, .shader = bare_shader(&wheel_shader));
    IconParams play = { 0, (float)iw }, plus = { 1, (float)iw };
    bare_pass(.target = play_icon, .shader = icon_fx, BARE_PARAMS(play));
    bare_pass(.target = plus_icon, .shader = icon_fx, BARE_PARAMS(plus));

    while (bare_frame()) {
        BareVec2 size = bare_window_size();
        W = size.x;
        H = size.y;
        float time = (float)bare_time(), t = fmodf(time + start, LOOP);
        SceneParams p = { time, W / H };

        if (t < 1.5f) {
            bare_pass(.target = dunes, .shader = dunes_fx, BARE_PARAMS(p));
            lens_scene(dunes, t);
        } else if (t < 2.7f) {
            bare_pass(.target = petals, .shader = petals_fx, BARE_PARAMS(p));
            capsule_scene(petals, t);
        } else if (t < 7.7f) {
            wheel_scene(wheel, t);
        } else {
            switch_scene(t);
        }
    }
    bare_quit();
}
