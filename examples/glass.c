/* glass.c: liquid glass over a living background.
 *
 * - a glass dock that glides after the mouse (or wanders on its own), with two
 *   glass drops orbiting it that melt into it as they pass its ends
 * - a frosted card drifting over glowing colour
 * - a clear lens sweeping across the stripes
 *
 * Positions come from time and the mouse; .offset places each layer of the
 * bare_stack. (From M4, springs will do the smoothing below.) */
#include "bare.h"
#include <math.h>

/* Moves `cur` toward `target`; the same feel at any frame rate. */
static float follow(float cur, float target, float dt, float speed) {
    return cur + (target - cur) * (1.0f - expf(-speed * dt));
}

static BareColor hue(float h, float s, float v, int alpha) {
    float r = fabsf(h * 6 - 3) - 1, g = 2 - fabsf(h * 6 - 2), b = 2 - fabsf(h * 6 - 4);
    r = fminf(fmaxf(r, 0), 1); g = fminf(fmaxf(g, 0), 1); b = fminf(fmaxf(b, 0), 1);
    return BARE_RGBA(255 * v * (1 - s + s * r), 255 * v * (1 - s + s * g), 255 * v * (1 - s + s * b), alpha);
}

/* Big glowing colour orbs, a field of tiles and a band of stripes. */
static void background(BareVec2 win, float t) {
    static const float orbs[4][4] = {   /* x, y (fraction of window), hue, phase */
        { 0.18f, 0.25f, 0.92f, 0.0f }, { 0.78f, 0.22f, 0.55f, 1.7f },
        { 0.62f, 0.80f, 0.08f, 3.1f }, { 0.25f, 0.78f, 0.72f, 4.4f },
    };
    for (int i = 0; i < 4; i++) {
        BareColor c = hue(orbs[i][2], 0.85f, 1.0f, 255);
        float x = orbs[i][0] * win.x - 190 + sinf(t * 0.21f + orbs[i][3]) * 90;
        float y = orbs[i][1] * win.y - 190 + cosf(t * 0.17f + orbs[i][3]) * 70;
        bare_box(.width = 380, .height = 380, .radius = 190, .color = c, .opacity = 0.85f,
                 .offset = { x, y }, .shadow = { 0, 0, 220, 40, c });
    }

    bare_column(.padding = 36, .gap = 22, .opacity = 0.5f) {
        for (int r = 0; r < 9; r++) {
            bare_row(.gap = 22) {
                for (int c = 0; c < 15; c++) {
                    float h = fmodf((float)(r * 15 + c) * 0.031f + 0.5f, 1.0f);
                    bare_box(.width = 62, .height = 62, .radius = 14,
                             .color = hue(h, 0.35f, 1.0f, 70), .border = 1,
                             .border_color = BARE_RGBA(255, 255, 255, 40));
                }
            }
        }
    }

    bare_row(.gap = 13, .offset = { 0, win.y * 0.5f - 100 }) {
        for (int i = 0; i < 100; i++) {
            bare_box(.width = 5, .height = 200, .radius = 2.5f, .color = BARE_RGBA(255, 255, 255, 120));
        }
    }
}

int main(void) {
    bare_window(1280, 800, "Bare: liquid glass");
    bare_background(BARE_RGB(9, 10, 22));
    bare_continuous(true);

    float dock_x = 380, dock_y = 560;
    BareVec2 last_mouse = { -1, -1 };
    float mouse_time = -100;

    while (bare_frame()) {
        float t = (float)bare_time(), dt = bare_dt();
        BareVec2 win = bare_window_size(), m = bare_mouse();

        /* The dock follows the mouse while it moves; left alone, it wanders. */
        if (last_mouse.x < 0) last_mouse = m;   /* the starting position isn't a move */
        if (m.x != last_mouse.x || m.y != last_mouse.y) { last_mouse = m; mouse_time = t; }
        float tx = win.x * 0.5f - 260 + sinf(t * 0.5f) * win.x * 0.26f;
        float ty = win.y * 0.62f + sinf(t * 0.83f) * win.y * 0.14f;
        if (t - mouse_time < 2.5f) { tx = m.x - 260; ty = m.y - 48; }
        dock_x = follow(dock_x, tx, dt, 6);
        dock_y = follow(dock_y, ty, dt, 6);
        float cx = dock_x + 260, cy = dock_y + 48;

        bare_stack(.width = win.x, .height = win.y) {
            background(win, t);

            /* Frosted card with some placeholder content. */
            bare_column(.width = 380, .height = 230, .radius = 36, .padding = 28, .gap = 14,
                        .glass = BARE_GLASS_FROSTED, .border = 1, .border_color = BARE_RGBA(255, 255, 255, 60),
                        .shadow = { 0, 24, 60, 0, { 0, 0, 0, 110 } },
                        .offset = { 90 + sinf(t * 0.37f) * 40, 70 + cosf(t * 0.29f) * 24 }) {
                bare_row(.gap = 14, .align = BARE_CENTER) {
                    bare_box(.width = 52, .height = 52, .radius = 26, .color = BARE_RGBA(255, 255, 255, 200));
                    bare_column(.gap = 8) {
                        bare_box(.width = 180, .height = 12, .radius = 6, .color = BARE_RGBA(255, 255, 255, 210));
                        bare_box(.width = 120, .height = 10, .radius = 5, .color = BARE_RGBA(255, 255, 255, 120));
                    }
                }
                bare_box(.width = 324, .height = 10, .radius = 5, .color = BARE_RGBA(255, 255, 255, 110));
                bare_box(.width = 280, .height = 10, .radius = 5, .color = BARE_RGBA(255, 255, 255, 110));
                bare_box(.width = 300, .height = 10, .radius = 5, .color = BARE_RGBA(255, 255, 255, 110));
            }

            /* A clear lens sweeping across the stripes. */
            float lens = 190;
            bare_box(.width = lens, .height = lens, .radius = lens / 2,
                     .glass = { .refraction = 1.2f, .depth = 42, .blur = BARE_ZERO },
                     .shadow = { 0, 16, 40, 0, { 0, 0, 0, 90 } },
                     .offset = { win.x * 0.5f - lens / 2 + sinf(t * 0.42f + 1.0f) * win.x * 0.36f,
                                 win.y * 0.5f - lens / 2 + sinf(t * 0.61f) * 30 });

            /* The dock and its two drops: consecutive glass with .merge melt together. */
            bare_box(.width = 520, .height = 96, .radius = 48,
                     .glass = { .merge = 56, .depth = 30, .tint = { 255, 255, 255, 14 } },
                     .shadow = { 0, 18, 44, 0, { 0, 0, 0, 120 } },
                     .offset = { dock_x, dock_y });
            for (int i = 0; i < 2; i++) {
                float a = t * 1.25f + (float)i * 3.14159f;
                float d = 92;
                bare_box(.width = d, .height = d, .radius = d / 2,
                         .glass = { .merge = 56, .depth = 30, .tint = { 255, 255, 255, 14 } },
                         .offset = { cx + cosf(a) * 330 - d / 2, cy + sinf(a) * 80 - d / 2 });
            }
        }
    }
    bare_quit();
}
