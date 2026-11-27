/* pmrem.h: scene.environment from a Radiance .hdr, as three.js r147 makes it:
 * RGBELoader (RLE scanlines, RGBE to half floats, flipY, bilinear), then
 * PMREMGenerator.fromEquirectangular: the panorama drawn into the six faces of
 * a "cube uv" atlas, then each smaller lod blurred from the one before
 * (spherical gaussians, latitudinal then longitudinal) into an RGBA16F canvas.
 *
 *     float env[3];   // CUBEUV_TEXEL_WIDTH, CUBEUV_TEXEL_HEIGHT, CUBEUV_MAX_MIP
 *     r.env_map = tl_pmrem_from_hdr(bare_shader(&pmrem_shader), "studio.hdr", r.env); */
#ifndef SCREENPLUS_PMREM_H
#define SCREENPLUS_PMREM_H
#include "bare.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RGBELoader's parse: the header, then new-style RLE scanlines (or flat RGBE) */
static inline unsigned char *tl_hdr_load(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f), free(buf); return NULL; }
    fclose(f);
    buf[n] = 0;
    long pos = 0;
    *w = *h = 0;
    while (pos < n) {   /* header lines up to the resolution line */
        long e = pos;
        while (e < n && buf[e] != '\n') e++;
        char line[256];
        long len = e - pos < 255 ? e - pos : 255;
        memcpy(line, buf + pos, (size_t)len), line[len] = 0;
        pos = e + 1;
        if (sscanf(line, "-Y %d +X %d", h, w) == 2) break;
    }
    int W = *w, H = *h;
    if (W <= 0 || H <= 0) { free(buf); return NULL; }
    unsigned char *out = malloc((size_t)W * H * 4);
    const unsigned char *p = buf + pos, *end = buf + n;
    if (W < 8 || W > 0x7fff || p + 4 > end || p[0] != 2 || p[1] != 2 || (p[2] & 0x80)) {   /* flat */
        size_t need = (size_t)W * H * 4;
        if ((size_t)(end - p) < need) { free(buf), free(out); return NULL; }
        memcpy(out, p, need);
        free(buf);
        return out;
    }
    unsigned char *line = malloc((size_t)W * 4);
    for (int y = 0; y < H; y++) {
        if (p + 4 > end || p[0] != 2 || p[1] != 2 || ((p[2] << 8) | p[3]) != W) { free(buf), free(out), free(line); return NULL; }
        p += 4;
        for (int c = 0; c < 4; c++) {
            unsigned char *dst = line + c * W;
            int x = 0;
            while (x < W && p < end) {
                int count = *p++;
                if (count > 128) {
                    count -= 128;
                    if (x + count > W || p >= end) { free(buf), free(out), free(line); return NULL; }
                    memset(dst + x, *p++, (size_t)count);
                } else {
                    if (!count || x + count > W || p + count > end) { free(buf), free(out), free(line); return NULL; }
                    memcpy(dst + x, p, (size_t)count), p += count;
                }
                x += count;
            }
        }
        for (int x = 0; x < W; x++)
            for (int c = 0; c < 4; c++) out[((size_t)y * W + x) * 4 + c] = line[c * W + x];
    }
    free(line), free(buf);
    return out;
}

typedef struct TlPmremParams { float rect[4], size[4], blur[4], pole[4], cube[4], weights[20]; } TlPmremParams;

#define TL_PMREM_LOD_MIN 4
#define TL_PMREM_MAX_SAMPLES 20

static inline BareImage tl_pmrem_from_hdr(BareShader sh, const char *path, float env[3]) {
    int w, h;
    unsigned char *rgbe = tl_hdr_load(path, &w, &h);
    if (!rgbe) return 0;
    BareImage src = bare_texture(.width = w, .height = h, .rgba8 = rgbe, .nearest = true);
    free(rgbe);
    BareImage eq = bare_canvas(.width = w, .height = h, .format = BARE_RGBA16F, .bottom_up = true);
    TlPmremParams p;
    memset(&p, 0, sizeof p);
    p.rect[2] = (float)w, p.rect[3] = (float)h, p.size[0] = (float)w, p.size[1] = (float)h, p.size[3] = 2;
    bare_draw(.target = eq, .shader = sh, .inputs = { src }, BARE_PARAMS(p), .vertex_count = 6);

    /* _setSize(width / 4), _allocateTargets, _createPlanes */
    int lod_max = (int)floor(log2(w / 4.0)), cube = 1 << lod_max;
    int W = 3 * (cube > 16 * 7 ? cube : 16 * 7), H = 4 * cube;
    static const double extra[6] = { 0.125, 0.215, 0.35, 0.446, 0.526, 0.582 };
    int lods = lod_max - TL_PMREM_LOD_MIN + 1 + 6, size_lods[32];
    double sigmas[32];
    for (int i = 0, lod = lod_max; i < lods; i++) {
        size_lods[i] = 1 << lod;
        double sigma = 1.0 / size_lods[i];
        if (i > lod_max - TL_PMREM_LOD_MIN) sigma = extra[i - lod_max + TL_PMREM_LOD_MIN - 1];
        else if (i == 0) sigma = 0;
        sigmas[i] = sigma;
        if (lod > TL_PMREM_LOD_MIN) lod--;
    }
    BareImage cubeuv = bare_canvas(.width = W, .height = H, .format = BARE_RGBA16F, .bottom_up = true);
    BareImage ping = bare_canvas(.width = W, .height = H, .format = BARE_RGBA16F, .bottom_up = true);
    env[0] = 1.f / W, env[1] = 1.f / H, env[2] = (float)lod_max;

    /* _textureToCubeUV */
    memset(&p, 0, sizeof p);
    p.rect[2] = 3.f * cube, p.rect[3] = 2.f * cube;
    p.size[0] = (float)W, p.size[1] = (float)H, p.size[2] = (float)cube, p.size[3] = 0;
    bare_draw(.target = cubeuv, .shader = sh, .inputs = { eq }, BARE_PARAMS(p), .vertex_count = 6, .clear = true);
    bare_draw(.target = ping, .shader = sh, .inputs = { eq }, BARE_PARAMS(p), .vertex_count = 6, .clear = true);

    /* _applyPMREM: _blur(i - 1 -> i) as two _halfBlur */
    const double PHI = (1 + sqrt(5.0)) / 2, IP = 1 / PHI;
    const double axes[10][3] = { { 1, 1, 1 }, { -1, 1, 1 }, { 1, 1, -1 }, { -1, 1, -1 }, { 0, PHI, IP },
                                 { 0, PHI, -IP }, { IP, 0, PHI }, { -IP, 0, PHI }, { PHI, IP, 0 }, { -PHI, IP, 0 } };
    for (int i = 1; i < lods; i++) {
        double sigma = sqrt(sigmas[i] * sigmas[i] - sigmas[i - 1] * sigmas[i - 1]);
        const double *pole = axes[(i - 1) % 10];
        for (int half = 0; half < 2; half++) {
            int lod_in = half ? i : i - 1, lod_out = i;
            int pixels = size_lods[lod_in] - 1;
            double rpp = 3.14159265358979323846 / (2.0 * pixels), sigma_px = sigma / rpp;
            int samples = 1 + (int)floor(3 * sigma_px);
            double wts[TL_PMREM_MAX_SAMPLES], sum = 0;
            for (int k = 0; k < TL_PMREM_MAX_SAMPLES; k++) {
                double x = k / sigma_px, wt = exp(-x * x / 2);
                wts[k] = wt;
                if (k == 0) sum += wt;
                else if (k < samples) sum += 2 * wt;
            }
            memset(&p, 0, sizeof p);
            for (int k = 0; k < TL_PMREM_MAX_SAMPLES; k++) p.weights[k] = (float)(wts[k] / sum);
            p.blur[0] = (float)samples, p.blur[1] = half ? 0.f : 1.f, p.blur[2] = (float)rpp, p.blur[3] = (float)(lod_max - lod_in);
            p.pole[0] = (float)pole[0], p.pole[1] = (float)pole[1], p.pole[2] = (float)pole[2];
            p.cube[0] = env[0], p.cube[1] = env[1], p.cube[2] = env[2];
            int out = size_lods[lod_out];
            p.rect[0] = 3.f * out * (lod_out > lod_max - TL_PMREM_LOD_MIN ? lod_out - lod_max + TL_PMREM_LOD_MIN : 0);
            p.rect[1] = 4.f * (cube - out);
            p.rect[2] = 3.f * out, p.rect[3] = 2.f * out;
            p.size[0] = (float)W, p.size[1] = (float)H, p.size[2] = (float)out, p.size[3] = 1;
            bare_draw(.target = half ? cubeuv : ping, .shader = sh, .inputs = { half ? ping : cubeuv }, BARE_PARAMS(p),
                      .vertex_count = 6);
        }
    }
    bare_image_free(eq), bare_image_free(ping), bare_image_free(src);
    return cubeuv;
}
#endif
