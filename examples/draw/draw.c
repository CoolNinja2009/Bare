/* draw.c: your own triangles into a canvas (bare_draw), as WebGL's
 * drawArrays and drawArraysInstanced.
 *
 *   a ribbon: a triangle strip with a colour per vertex, trailing the cursor
 *             (or a path of its own while the cursor rests)
 *   dots:     300 soft dots, one instance each, their quads built from
 *             gl_VertexIndex, blended with straight alpha
 *
 * The canvas is bottom_up, as a WebGL framebuffer: the shaders' clip space is
 * WebGL's and the picture still shows the right way up. */
#include "bare.h"
#include "draw_shaders.h"
#include <math.h>
#include <stdlib.h>

enum { TRAIL = 96, DOTS = 300 };

typedef struct Vert { float pos[4], color[4]; } Vert;   /* two vec4 slots: a_pos, a_color */
typedef struct Dot { float dot[4], color[4]; } Dot;      /* two vec4 slots: i_dot, i_color */

static float rnd(void) { return (float)rand() / (float)RAND_MAX; }

int main(void) {
    if (!bare_window(1280, 720, "Bare: draw")) return 1;
    BareShader ribbon = bare_shader(&ribbon_shader), dots = bare_shader(&dots_shader);
    BareImage canvas = 0;
    int cw = 0, ch = 0;

    static float trail[TRAIL][2];
    static Vert strip[TRAIL * 2];
    static Dot inst[DOTS];
    static float seed[DOTS][4];
    for (int i = 0; i < DOTS; i++) seed[i][0] = rnd(), seed[i][1] = rnd(), seed[i][2] = rnd(), seed[i][3] = rnd();
    BareVec2 last_mouse = { -1, -1 };
    double last_move = -10;
    bool started = false;

    while (bare_frame()) {
        BareVec2 win = bare_window_size();
        float scale = bare_pixel_scale();
        int w = (int)lroundf(win.x * scale), h = (int)lroundf(win.y * scale);
        if (w < 1 || h < 1) continue;
        if (w != cw || h != ch) {
            if (canvas) bare_image_free(canvas);
            canvas = bare_canvas(.width = w, .height = h, .bottom_up = true);
            cw = w, ch = h;
        }
        bare_box(.width = BARE_GROW, .height = BARE_GROW, .image = canvas, .color = BARE_RGBA(12, 14, 24, 255));

        double t = bare_time();
        BareVec2 m = bare_mouse();
        if ((m.x != last_mouse.x || m.y != last_mouse.y) && last_mouse.x >= 0) last_move = t;
        last_mouse = m;
        float hx = t - last_move < 2 ? m.x * scale : (float)(w * (0.5 + 0.35 * sin(t * 0.9)));
        float hy = t - last_move < 2 ? m.y * scale : (float)(h * (0.5 + 0.3 * sin(t * 1.7)));

        /* the trail eases after the head; each point twice, either side of the line */
        trail[0][0] = hx, trail[0][1] = hy;
        if (!started)
            for (int i = 1; i < TRAIL; i++) trail[i][0] = hx, trail[i][1] = hy;
        started = true;
        for (int i = TRAIL - 1; i > 0; i--)
            trail[i][0] += (trail[i - 1][0] - trail[i][0]) * 0.5f, trail[i][1] += (trail[i - 1][1] - trail[i][1]) * 0.5f;
        for (int i = 0; i < TRAIL; i++) {
            int a = i > 0 ? i - 1 : 0, b = i < TRAIL - 1 ? i + 1 : i;
            float dx = trail[b][0] - trail[a][0], dy = trail[b][1] - trail[a][1], len = sqrtf(dx * dx + dy * dy);
            float nx = len > 1e-3f ? -dy / len : 0, ny = len > 1e-3f ? dx / len : 1;
            float k = 1 - (float)i / TRAIL, half = 26 * scale * k;
            float c[4] = { 0.4f + 0.6f * k, 0.5f * k + 0.2f, 1.0f - 0.5f * k, 1 };
            for (int s = 0; s < 2; s++) {
                Vert *v = &strip[i * 2 + s];
                float side = s ? 1.f : -1.f;
                v->pos[0] = trail[i][0] + nx * half * side, v->pos[1] = trail[i][1] + ny * half * side;
                for (int j = 0; j < 4; j++) v->color[j] = c[j];
            }
        }

        for (int i = 0; i < DOTS; i++) {
            float sp = 0.02f + 0.05f * seed[i][2];
            float x = fmodf(seed[i][0] + (float)t * sp * 0.3f, 1), y = fmodf(seed[i][1] + (float)t * sp, 1);
            inst[i].dot[0] = x * w, inst[i].dot[1] = (1 - y) * h;   /* rising */
            inst[i].dot[2] = (4 + 18 * seed[i][3]) * scale;
            inst[i].color[0] = 1, inst[i].color[1] = 0.55f + 0.4f * seed[i][2], inst[i].color[2] = 0.3f;
            inst[i].color[3] = 0.25f + 0.5f * seed[i][3];
        }

        float res[4] = { (float)w, (float)h, 0, 0 };
        bare_draw(.target = canvas, .shader = dots, .clear = true, .clear_color = { 0.05f, 0.055f, 0.09f, 1 },
                  .vertex_count = 6, BARE_INSTANCES(inst, DOTS), .blend = BARE_BLEND_ALPHA, BARE_PARAMS(res));
        bare_draw(.target = canvas, .shader = ribbon, .strip = true, BARE_VERTICES(strip, TRAIL * 2),
                  BARE_PARAMS(res));
        bare_continuous(true);
    }
    bare_quit();
}
