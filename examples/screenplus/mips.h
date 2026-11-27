/* mips.h: WebGL's mipmaps for Bare pictures, which have one level. The levels
 * are made on the CPU (2x2 averages, as glGenerateMipmap) and laid side by side
 * in one image; a shader picks and blends them as the GPU would (see
 * tunnel/shaders/tunnel.glsl, rain/shaders/rain.glsl). */
#ifndef SCREENPLUS_MIPS_H
#define SCREENPLUS_MIPS_H
#include "bare.h"
#include <stdlib.h>
#include <string.h>

#define MAX_LEVELS 12

typedef struct Mips {
    BareImage atlas;
    int aw, ah, levels;
    float lv[MAX_LEVELS][4];   /* x, y, w, h */
} Mips;

/* Level 0 on the left, the rest stacked on its right. Each level halves the
 * last (sizes rounded down, as WebGL's), each texel the mean of its 2x2. */
static inline Mips make_mips(const unsigned char *rgba, int w, int h) {
    Mips m;
    memset(&m, 0, sizeof m);
    int sizes[MAX_LEVELS][2], n = 0;
    for (int lw = w, lh = h; n < MAX_LEVELS; n++) {
        sizes[n][0] = lw, sizes[n][1] = lh;
        if (lw == 1 && lh == 1) { n++; break; }
        lw = lw > 1 ? lw / 2 : 1, lh = lh > 1 ? lh / 2 : 1;
    }
    m.levels = n;
    int right = n > 1 ? sizes[1][0] : 0, stack = 0;
    for (int i = 1; i < n; i++) stack += sizes[i][1];
    m.aw = w + right, m.ah = h > stack ? h : stack;
    unsigned char *atlas = calloc((size_t)m.aw * m.ah, 4);
    unsigned char *prev = malloc((size_t)w * h * 4);
    if (!atlas || !prev) { free(atlas); free(prev); return m; }
    memcpy(prev, rgba, (size_t)w * h * 4);
    int y = 0;
    for (int i = 0; i < n; i++) {
        int lw = sizes[i][0], lh = sizes[i][1];
        unsigned char *cur = prev;
        if (i > 0) {
            int pw = sizes[i - 1][0], ph = sizes[i - 1][1];
            cur = malloc((size_t)lw * lh * 4);
            if (!cur) break;
            for (int yy = 0; yy < lh; yy++)
                for (int xx = 0; xx < lw; xx++)
                    for (int c = 0; c < 4; c++) {
                        int x0 = xx * 2 < pw ? xx * 2 : pw - 1, x1 = xx * 2 + 1 < pw ? xx * 2 + 1 : pw - 1;
                        int y0 = yy * 2 < ph ? yy * 2 : ph - 1, y1 = yy * 2 + 1 < ph ? yy * 2 + 1 : ph - 1;
                        int s = prev[((size_t)y0 * pw + x0) * 4 + c] + prev[((size_t)y0 * pw + x1) * 4 + c] +
                                prev[((size_t)y1 * pw + x0) * 4 + c] + prev[((size_t)y1 * pw + x1) * 4 + c];
                        cur[((size_t)yy * lw + xx) * 4 + c] = (unsigned char)((s + 2) / 4);
                    }
            free(prev);
            prev = cur;
        }
        int ox = i == 0 ? 0 : w, oy = i == 0 ? 0 : y;
        for (int r = 0; r < lh; r++) memcpy(atlas + ((size_t)(oy + r) * m.aw + ox) * 4, cur + (size_t)r * lw * 4, (size_t)lw * 4);
        if (i > 0) y += lh;
        m.lv[i][0] = (float)ox, m.lv[i][1] = (float)oy, m.lv[i][2] = (float)lw, m.lv[i][3] = (float)lh;
    }
    free(prev);
    m.atlas = bare_image_from_pixels(m.aw, m.ah, atlas);
    free(atlas);
    return m;
}

static inline void free_mips(Mips *m) {
    if (m->atlas) bare_image_free(m->atlas);
    memset(m, 0, sizeof *m);
}

#endif
