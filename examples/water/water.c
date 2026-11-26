/* water.c (a copy of examples/ocean/ to build on): the open sea in real time, simulated on the GPU.
 *
 *   W A S D        fly (along where you look)
 *   Space / Shift  up / down; fly under the surface too
 *   drag           look around (arrow keys too)
 *   wheel          flying speed; Ctrl: five times faster
 *   H              hide or show the panel
 *
 * The sea is Tessendorf's: a JONSWAP spectrum of waves for the wind, spread
 * around its direction, moved on in time and turned into a surface by inverse
 * FFTs on the GPU, every frame. Three cascades (450 m, 53 m and 7.3 m across)
 * share the work, each with its own band of wavelengths, so the surface has
 * swells and ripples and no visible tiling. Where the choppy displacement
 * folds the surface over, foam builds and slowly fades; spray is thrown off
 * where it's breaking right now. Finer than the cascades, a detail texture
 * baked at startup (mipmapped) adds ripples that come and go with gusts and
 * slicks, and gives foam its bubbles.
 *
 * Per frame: one pass moves the spectra on (eight real spectra packed into
 * four complex ones, the three cascades side by side), 16 FFT stages (8 along
 * rows, 8 down columns, all cascades at once), three passes assemble the
 * displacement, normals and foam of each cascade, one finds the water's height
 * under the camera (it can ride the swell, on a spring), then the sky, the sea
 * as a mesh of rings drawn with a depth buffer, spray as instanced quads tested
 * against that depth, the underwater pass (fog from the depth canvas, light
 * shafts, the waterline across the lens), depth of field, motes and bubbles
 * drawn as blur circles against the same depth, bloom, the tone curve and a
 * cool, cinematic grade (letterboxed to 2.39 with Cinema on).
 *
 * Settings can be given as arguments too (for captures with BARE_FIXED_DT):
 *   water wind=14 dir=40 chop=1.2 foam=1 clouds=0.8 hour=16 swell=0.4 time=30
 *         coast=0.7 cinema=1 cam=x,y,z,yaw,pitch ride=1 panel=0 */
#include "bare.h"
#include "water_shaders.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI_F 3.14159265f
#define DETAIL_TILE 4.0f   /* metres across the baked ripples and foam texture */
#define LOOP 2048.0f   /* the sea repeats after this many seconds (frequencies are rounded to it) */

enum { N = 256, LOG_N = 8, CASCADES = 3, BLOOM_LEVELS = 6, SPRAY = 48000, SKY_W = 1024, SKY_H = 256,
       RINGS = 360, SEGMENTS = 640 };
static const float LENGTHS[CASCADES] = { 450.0f, 53.0f, 7.3f };

/* ---- settings ---------------------------------------------------------------- */

static struct {
    float wind, direction, chop, foam, clouds, hour, swell, coast, time;
    bool ride, cinema, panel;
} cfg = { .wind = 17, .direction = 25, .chop = 1.45f, .foam = 0.8f, .clouds = 0.85f, .hour = 16.2f, .swell = 0.15f,
          .coast = 0.7f, .ride = true, .cinema = true, .panel = true };

static struct {
    double x, y, z;      /* y: above the swell while riding it, else above the mean sea level */
    float yaw, pitch;    /* radians: yaw 0 looks along +x, positive pitch looks up */
    float speed;         /* m/s */
    double vx, vy, vz;
} cam = { 0, 0.6f, 0, 1.75f, -0.05f, 4, 0, 0, 0 };

static void parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        char key[32];
        const char *eq = strchr(argv[i], '=');
        if (!eq || eq - argv[i] >= (int)sizeof key) continue;
        memcpy(key, argv[i], (size_t)(eq - argv[i]));
        key[eq - argv[i]] = 0;
        const char *v = eq + 1;
        float f = (float)atof(v);
        if (!strcmp(key, "wind")) cfg.wind = f;
        else if (!strcmp(key, "dir")) cfg.direction = f;
        else if (!strcmp(key, "chop")) cfg.chop = f;
        else if (!strcmp(key, "foam")) cfg.foam = f;
        else if (!strcmp(key, "clouds")) cfg.clouds = f;
        else if (!strcmp(key, "hour")) cfg.hour = f;
        else if (!strcmp(key, "swell")) cfg.swell = f;
        else if (!strcmp(key, "time")) cfg.time = f;
        else if (!strcmp(key, "coast")) cfg.coast = f;
        else if (!strcmp(key, "cinema")) cfg.cinema = f != 0;
        else if (!strcmp(key, "ride")) cfg.ride = f != 0;
        else if (!strcmp(key, "panel")) cfg.panel = f != 0;
        else if (!strcmp(key, "cam")) {
            float c[5] = { 0, 0.02f, 0, 20, 1 };
            sscanf(v, "%f,%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3], &c[4]);
            cam.x = c[0], cam.y = c[1], cam.z = c[2];
            cam.yaw = c[3] * PI_F / 180, cam.pitch = c[4] * PI_F / 180;
        }
    }
}

/* ---- GPU state ----------------------------------------------------------------- */

static struct {
    BareShader detail, spectrum, evolve, fft, assemble, probe, skymap, sky, ocean, spray, water, dof, motes, down, up, final;
} fx;

static BareImage h0, fft_a[2], fft_b[2];             /* atlases 3N x N: spectra, transforms in flight */
static BareImage disp[CASCADES][2], deriv[CASCADES]; /* per cascade; displacement ping-pongs for its foam */
static BareImage probe[2], skymap, detail;
static BareImage scene, depth, hdr, lens, display, bloom[BLOOM_LEVELS];
static int bloom_w[BLOOM_LEVELS], bloom_h[BLOOM_LEVELS], bloom_count;
static int width, height;

/* The shaders' std140 blocks: vec4s only. */
typedef struct Frame {
    float cam[4], right[4], up[4], fwd[4], sun[4], sun_color[4], zenith[4], horizon[4], cloud[4], water[4],
          scatter[4], absorb[4], lengths[4], wind[4], res[4], grid[4], misc[4], extra[4];
} Frame;
typedef struct SpectrumParams { float len[4], wind[4], band[4], seed[4]; } SpectrumParams;
typedef struct EvolveParams { float len[4], time[4]; } EvolveParams;
typedef struct FftParams { float stage[4]; } FftParams;
typedef struct AssembleParams { float c[4], foam[4]; } AssembleParams;
typedef struct PostParams { float post[4], res[4], grade[4], tone[4]; } PostParams;
typedef struct DofParams { float res[4], lens[4], view[4]; } DofParams;

static void make_sim(void) {
    h0 = bare_canvas(.width = N * CASCADES, .height = N, .format = BARE_RGBA32F, .nearest = true);
    for (int i = 0; i < 2; i++) {
        fft_a[i] = bare_canvas(.width = N * CASCADES, .height = N, .format = BARE_RGBA32F, .nearest = true);
        fft_b[i] = bare_canvas(.width = N * CASCADES, .height = N, .format = BARE_RGBA32F, .nearest = true);
        probe[i] = bare_canvas(.width = 1, .height = 1, .format = BARE_RGBA32F, .nearest = true);
    }
    for (int c = 0; c < CASCADES; c++) {
        /* mipmapped: their foam is read far away too */
        disp[c][0] = bare_canvas(.width = N, .height = N, .format = BARE_RGBA16F, .repeat = true, .mipmaps = true);
        disp[c][1] = bare_canvas(.width = N, .height = N, .format = BARE_RGBA16F, .repeat = true, .mipmaps = true);
        deriv[c] = bare_canvas(.width = N, .height = N, .format = BARE_RGBA16F, .repeat = true, .mipmaps = true);
    }
    skymap = bare_canvas(.width = SKY_W, .height = SKY_H, .format = BARE_RGBA16F, .repeat = true);
    /* the fine print, baked once: ripples shorter than the cascades, bubbly foam, foam lace */
    detail = bare_canvas(.width = 1024, .height = 1024, .format = BARE_RGBA16F, .repeat = true, .mipmaps = true);
    float tile[4] = { DETAIL_TILE, 0.55f, 0, 0 };
    bare_pass(.target = detail, .shader = fx.detail, BARE_PARAMS(tile));
}

static void make_screen(int w, int h) {
    BareImage old[] = { scene, depth, hdr, lens, display };
    for (int i = 0; i < 5; i++) if (old[i]) bare_image_free(old[i]);
    for (int i = 0; i < bloom_count; i++) bare_image_free(bloom[i]);
    scene = bare_canvas(.width = w, .height = h, .format = BARE_RGBA16F);
    depth = bare_canvas(.width = w, .height = h, .format = BARE_DEPTH);
    hdr = bare_canvas(.width = w, .height = h, .format = BARE_RGBA16F);
    lens = bare_canvas(.width = w, .height = h, .format = BARE_RGBA16F);
    display = bare_canvas(.width = w, .height = h);
    bloom_count = 0;
    for (int i = 0; i < BLOOM_LEVELS; i++) {
        int lw = w >> (i + 1), lh = h >> (i + 1);
        if (lw < 2 || lh < 2) break;
        bloom[i] = bare_canvas(.width = lw, .height = lh, .format = BARE_RGBA16F);
        bloom_w[i] = lw, bloom_h[i] = lh;
        bloom_count++;
    }
    width = w, height = h;
}

/* ---- the simulation -------------------------------------------------------- */

static void spectrum(void) {
    float a = cfg.direction * PI_F / 180;
    /* band edges: each cascade starts at 4 of its own longest waves */
    /* 10 km of fetch: a young, steep wind sea, as close to a coast */
    SpectrumParams p = { { LENGTHS[0], LENGTHS[1], LENGTHS[2], N }, { cosf(a), sinf(a), cfg.wind, 10000 },
                         { 4 * 2 * PI_F / LENGTHS[1], 4 * 2 * PI_F / LENGTHS[2], 0.012f, cfg.swell }, { 7, 0, 0, 0 } };
    bare_pass(.target = h0, .shader = fx.spectrum, BARE_PARAMS(p));
}

static int cur;   /* which of each ping-pong pair is this frame's */

static void simulate(float t, float dt) {
    EvolveParams ep = { { LENGTHS[0], LENGTHS[1], LENGTHS[2], N }, { fmodf(t, LOOP), LOOP, 0, 0 } };
    bare_pass(.target = fft_a[0], .extra_targets = { fft_b[0] }, .shader = fx.evolve, .inputs = { h0 },
              BARE_PARAMS(ep));
    int src = 0;
    for (int dir = 1; dir >= 0; dir--) {   /* rows, then columns */
        for (int s = 1; s <= LOG_N; s++) {
            FftParams fp = { { (float)(1 << s), (float)dir, N, 0 } };
            bare_pass(.target = fft_a[!src], .extra_targets = { fft_b[!src] }, .shader = fx.fft,
                      .inputs = { fft_a[src], fft_b[src] }, BARE_PARAMS(fp));
            src = !src;
        }
    }
    cur = !cur;
    for (int c = 0; c < CASCADES; c++) {
        AssembleParams ap = { { (float)c, N, cfg.chop, dt }, { 0.62f + 0.08f * (float)c, 2.6f, 2.4f + 2.0f * cfg.foam, 0 } };
        bare_pass(.target = disp[c][cur], .extra_targets = { deriv[c] }, .shader = fx.assemble,
                  .inputs = { fft_a[src], fft_b[src], disp[c][!cur] }, BARE_PARAMS(ap));
    }
}

/* ---- light ----------------------------------------------------------------- */

static float smooth(float a, float b, float x) {
    float t = fminf(fmaxf((x - a) / (b - a), 0), 1);
    return t * t * (3 - 2 * t);
}
static float mixf(float a, float b, float t) { return a + (b - a) * t; }
static void set4(float *d, float a, float b, float c, float e) { d[0] = a, d[1] = b, d[2] = c, d[3] = e; }
static void mix3(float *d, const float *a, const float *b, float t, float scale) {
    for (int i = 0; i < 3; i++) d[i] = mixf(a[i], b[i], t) * scale;
}

/* The sun from the hour, and the sky's colours from the sun and the cloud cover. */
static void light(Frame *f) {
    float day = (cfg.hour - 6) / 12;
    float elev = 52 * PI_F / 180 * sinf(PI_F * day), az = PI_F * day + 0.6f;
    set4(f->sun, cosf(elev) * cosf(az), sinf(elev), cosf(elev) * sinf(az), 0);
    float e = elev * 180 / PI_F, cover = fminf(fmaxf(cfg.clouds, 0), 1);
    float daylight = smooth(-8, 12, e), warm = 1 - smooth(-2, 24, e);
    float level = 0.03f + 0.97f * daylight;
    static const float noon[3] = { 1.0f, 0.95f, 0.88f }, dusk[3] = { 1.0f, 0.48f, 0.2f };
    mix3(f->sun_color, noon, dusk, warm, 5.0f * daylight);
    f->sun[3] = (1 - 0.88f * powf(cover, 0.7f)) * smooth(-3, 4, e);
    f->sun_color[3] = powf(1 - cover, 2.0f);
    static const float clear_z[3] = { 0.16f, 0.27f, 0.48f }, grey_z[3] = { 0.40f, 0.50f, 0.55f };
    static const float clear_h[3] = { 0.62f, 0.72f, 0.84f }, grey_h[3] = { 0.12f, 0.18f, 0.22f };
    static const float cloud_lo[3] = { 0.42f, 0.45f, 0.50f }, cloud_hi[3] = { 0.12f, 0.155f, 0.19f };
    mix3(f->zenith, clear_z, grey_z, cover, level);
    mix3(f->horizon, clear_h, grey_h, cover, level);
    mix3(f->cloud, cloud_lo, cloud_hi, cover, level);
    for (int i = 0; i < 3; i++) {   /* low sun warms the horizon and the clouds' undersides */
        float tint[3] = { 0.30f, 0.12f, 0.0f };
        f->horizon[i] += tint[i] * warm * daylight * (1 - 0.6f * cover);
        f->cloud[i] += tint[i] * 0.5f * warm * daylight;
    }
    f->zenith[3] = cover;
    f->cloud[3] = cfg.coast;
    f->horizon[3] = 2500 + 9000 * (1 - cover);
    set4(f->water, 0.008f, 0.012f, 0.016f, cfg.chop);
    /* coastal water: murky, green-grey, the blue absorbed nearly as fast as the red */
    set4(f->scatter, 0.014f * level, 0.05f * level, 0.046f * level, 0.35f);
    set4(f->absorb, 0.36f, 0.11f, 0.14f, 0.6f + 0.6f * f->sun[3]);
}

/* Ring radius = a (e^(b i) - 1): the first ring `first` metres out, the last at `far`. */
static void grid(float first, float far, float *a, float *b) {
    float lo = 1e-4f, hi = 1;
    for (int i = 0; i < 60; i++) {
        float m = 0.5f * (lo + hi);
        if (first / m * (expf(m * RINGS) - 1) > far) hi = m; else lo = m;
    }
    *b = 0.5f * (lo + hi);
    *a = first / *b;
}

/* ---- the picture ----------------------------------------------------------- */

static void render(const Frame *f) {
    BareImage dc[3] = { disp[0][cur], disp[1][cur], disp[2][cur] };
    bare_pass(.target = probe[cur], .shader = fx.probe, .inputs = { dc[0], dc[1], dc[2], probe[!cur] }, BARE_PARAMS(*f));
    BareImage pr = probe[cur];
    bare_pass(.target = skymap, .shader = fx.skymap, BARE_PARAMS(*f));
    bare_pass(.target = scene, .shader = fx.sky, .inputs = { pr }, BARE_PARAMS(*f));
    /* reversed depth: 1 at the near plane, 0 far away (the most precise way for a float depth buffer) */
    bare_draw(.target = scene, .depth = depth, .clear_depth = true, .depth_clear = BARE_ZERO,
              .depth_test = BARE_DEPTH_GREATER, .shader = fx.ocean, .vertex_count = RINGS * SEGMENTS * 6,
              .inputs = { dc[0], dc[1], dc[2], pr, deriv[0], deriv[1], deriv[2], skymap, detail }, BARE_PARAMS(*f));
    bare_draw(.target = scene, .depth = depth, .depth_read_only = true, .depth_test = BARE_DEPTH_GREATER,
              .shader = fx.spray, .vertex_count = 6, .instance_count = SPRAY, .blend = BARE_BLEND_OVER,
              .inputs = { dc[0], dc[1], dc[2], pr }, BARE_PARAMS(*f));
    bare_pass(.target = hdr, .shader = fx.water,
              .inputs = { scene, depth, dc[0], dc[1], dc[2], deriv[1], deriv[2], pr }, BARE_PARAMS(*f));

    float focus = cam.y < -0.3 ? 2.0f : 4.5f;
    DofParams dp = { { (float)width, (float)height, 1.0f / (float)width, 1.0f / (float)height },
                     { focus, 9.0f * (float)height / 900, 1.2f * (float)height / 900, 22.0f * (float)height / 900 },
                     { f->fwd[3], f->right[3], f->up[3], 0 } };
    bare_pass(.target = lens, .shader = fx.dof, .inputs = { hdr, depth }, BARE_PARAMS(dp));
    bare_draw(.target = lens, .depth = depth, .depth_read_only = true, .depth_test = BARE_DEPTH_GREATER,
              .shader = fx.motes, .vertex_count = 6, .instance_count = 8000, .blend = BARE_BLEND_OVER,
              .inputs = { dc[0], dc[1], dc[2], pr }, BARE_PARAMS(*f));

    BareImage src = lens;
    int sw = width, sh = height;
    for (int i = 0; i < bloom_count; i++) {
        float texel[2] = { 1.0f / (float)sw, 1.0f / (float)sh };
        bare_pass(.target = bloom[i], .shader = fx.down, .inputs = { src }, BARE_PARAMS(texel));
        src = bloom[i], sw = bloom_w[i], sh = bloom_h[i];
    }
    for (int i = bloom_count - 1; i > 0; i--) {
        float texel[2] = { 1.0f / (float)bloom_w[i], 1.0f / (float)bloom_h[i] };
        bare_pass(.target = bloom[i - 1], .shader = fx.up, .inputs = { bloom[i] }, .blend = BARE_BLEND_ADD,
                  BARE_PARAMS(texel));
    }
    PostParams pp = { { f->extra[0], 0.035f, 4.0f, f->extra[1] },
                      { (float)width, (float)height, 1.0f / (float)width, 1.0f / (float)height },
                      { cfg.cinema ? 2.39f : 0, 0.85f, 0.03f, 1 }, { 0.55f, 1.0f, cfg.cinema ? 1.0f : 0, 0 } };
    bare_pass(.target = display, .shader = fx.final, .inputs = { lens, bloom_count ? bloom[0] : lens }, BARE_PARAMS(pp));
}

/* ---- the camera ------------------------------------------------------------ */

static void fly(float dt, BareState view, BareVec2 *last_mouse) {
    BareVec2 m = bare_mouse();
    if (view.pressed) {
        cam.yaw += (m.x - last_mouse->x) * 0.0042f;
        cam.pitch -= (m.y - last_mouse->y) * 0.0042f;
    }
    *last_mouse = m;
    float turn = 1.6f * dt;
    if (bare_key_down(BARE_KEY_LEFT)) cam.yaw -= turn;
    if (bare_key_down(BARE_KEY_RIGHT)) cam.yaw += turn;
    if (bare_key_down(BARE_KEY_UP)) cam.pitch += turn;
    if (bare_key_down(BARE_KEY_DOWN)) cam.pitch -= turn;
    cam.pitch = fminf(fmaxf(cam.pitch, -1.5f), 1.5f);
    float wheel = bare_wheel().y;
    if (wheel != 0 && view.hovered) cam.speed = fminf(fmaxf(cam.speed * powf(1.25f, -wheel / 100), 0.25f), 400);

    float cy = cosf(cam.yaw), sy = sinf(cam.yaw), cp = cosf(cam.pitch), sp = sinf(cam.pitch);
    float fwd[3] = { cy * cp, sp, sy * cp }, right[3] = { -sy, 0, cy };
    float in[3] = { 0, 0, 0 };
    float ws = (float)bare_key_down(BARE_KEY_W) - (float)bare_key_down(BARE_KEY_S);
    float ad = (float)bare_key_down(BARE_KEY_D) - (float)bare_key_down(BARE_KEY_A);
    float ud = (float)bare_key_down(BARE_KEY_SPACE) - (float)bare_key_down(BARE_KEY_SHIFT);
    for (int i = 0; i < 3; i++) in[i] = fwd[i] * ws + right[i] * ad;
    in[1] += ud;
    float boost = bare_key_down(BARE_KEY_CTRL) ? 5.0f : 1.0f;
    /* velocity eases towards where the keys point, so starts and stops are smooth */
    float k = 1 - expf(-dt * 6), target[3];
    for (int i = 0; i < 3; i++) target[i] = in[i] * cam.speed * boost;
    cam.vx += (target[0] - cam.vx) * k, cam.vy += (target[1] - cam.vy) * k, cam.vz += (target[2] - cam.vz) * k;
    cam.x += cam.vx * dt, cam.y += cam.vy * dt, cam.z += cam.vz * dt;
    cam.y = fmax(fmin(cam.y, 3000), -300);
}

static void camera(Frame *f) {
    float cy = cosf(cam.yaw), sy = sinf(cam.yaw), cp = cosf(cam.pitch), sp = sinf(cam.pitch);
    float fov_y = 46 * PI_F / 180, aspect = (float)width / (float)height;
    float ride = cfg.ride ? expf(-fabsf((float)cam.y) / 12) : 0;
    set4(f->cam, (float)cam.x, (float)cam.y, (float)cam.z, ride);
    set4(f->fwd, cy * cp, sp, sy * cp, 0.15f);   /* the near plane is past the dome port (see water.glsl) */
    set4(f->right, -sy, 0, cy, tanf(fov_y / 2) * aspect);
    set4(f->up, -cy * sp, cp, -sy * sp, tanf(fov_y / 2));
    /* the mesh reaches further the higher you are, so the sea meets the horizon */
    float alt = fabsf((float)cam.y);
    float a, b;
    grid(0.035f * (1 + alt / 6), 24000 * (1 + alt / 80), &a, &b);
    set4(f->grid, a, b, RINGS, SEGMENTS);
    set4(f->res, (float)width, (float)height, 1.0f / (float)width, 1.0f / (float)height);
    set4(f->lengths, LENGTHS[0], LENGTHS[1], LENGTHS[2], DETAIL_TILE);
}

/* ---- the panel ------------------------------------------------------------- */

static bool slider(const char *label, float *v, float lo, float hi, const char *fmt) {
    bool moved = false;
    bare_row(.width = BARE_GROW, .gap = 10, .align = BARE_CENTER) {
        bare_row(.width = 92) { bare_text(label, .size = 13, .color = BARE_RGBA(230, 238, 245, 230), .nowrap = true); }
        moved = bare_slider(v, lo, hi, .width = BARE_GROW);
        bare_row(.width = 52, .justify = BARE_END) {
            char buf[32];
            snprintf(buf, sizeof buf, fmt, (double)*v);
            bare_text(buf, .size = 12.5f, .color = BARE_RGBA(200, 214, 224, 200), .nowrap = true);
        }
    }
    return moved;
}

static bool panel(float fps) {
    bool changed = false;
    bare_column(.position = BARE_ABSOLUTE, .x = 18, .y = 18, .width = 330, .padding = 18, .gap = 12, .radius = 22,
                .glass = BARE_GLASS_FROSTED, .color = BARE_RGBA(14, 22, 30, 96), .border = 1,
                .border_color = BARE_RGBA(255, 255, 255, 34), .shadow = { 0, 12, 34, 0, BARE_RGBA(0, 0, 0, 70) },
                .text_color = BARE_RGBA(240, 246, 250, 255)) {
        bare_row(.width = BARE_GROW, .align = BARE_CENTER) {
            bare_text("Open sea", .size = 17, .weight = BARE_SEMIBOLD);
            bare_box(.width = BARE_GROW);
            bare_textf("%.0f fps", (double)fps);
        }
        changed |= slider("Wind", &cfg.wind, 3, 24, "%.0f m/s");
        changed |= slider("Direction", &cfg.direction, 0, 360, "%.0f\xC2\xB0");
        changed |= slider("Swell", &cfg.swell, 0, 1.5f, "%.2f");
        slider("Coast", &cfg.coast, 0, 1, "%.2f");
        slider("Choppiness", &cfg.chop, 0, 1.8f, "%.2f");
        slider("Foam", &cfg.foam, 0, 1.5f, "%.2f");
        slider("Clouds", &cfg.clouds, 0, 1, "%.2f");
        slider("Time of day", &cfg.hour, 5.5f, 19.5f, "%.1f h");
        bare_row(.gap = 22) {
            bare_checkbox("Ride the swell", &cfg.ride);
            bare_checkbox("Cinema", &cfg.cinema);
        }
        const char *where = cam.y < -0.1 ? "below" : "above";
        bare_textf("%s %.1f m  \xC2\xB7  speed %.1f m/s", where, fabs(cam.y), (double)cam.speed);
        bare_text("WASD fly \xC2\xB7 Space / Shift up, down \xC2\xB7 drag to look \xC2\xB7 wheel: speed \xC2\xB7 H: panel",
                  .size = 11.5f, .color = BARE_RGBA(200, 214, 224, 170));
    }
    return changed;
}

/* ---- the app --------------------------------------------------------------- */

int main(int argc, char **argv) {
    parse_args(argc, argv);
    if (!bare_window(1600, 900, "Ocean")) return 1;
    fx.spectrum = bare_shader(&spectrum_shader);
    fx.evolve = bare_shader(&evolve_shader);
    fx.fft = bare_shader(&fft_shader);
    fx.assemble = bare_shader(&assemble_shader);
    fx.probe = bare_shader(&probe_shader);
    fx.skymap = bare_shader(&skymap_shader);
    fx.sky = bare_shader(&sky_shader);
    fx.ocean = bare_shader(&ocean_shader);
    fx.spray = bare_shader(&spray_shader);
    fx.water = bare_shader(&water_shader);
    fx.detail = bare_shader(&detail_shader);
    fx.dof = bare_shader(&dof_shader);
    fx.motes = bare_shader(&motes_shader);
    fx.down = bare_shader(&down_shader);
    fx.up = bare_shader(&up_shader);
    fx.final = bare_shader(&final_shader);
    make_sim();

    bool waves_dirty = true;
    int frame_no = 0;
    float fps = 60;
    BareVec2 last_mouse = bare_mouse();
    while (bare_frame()) {
        BareVec2 win = bare_window_size();
        float scale = bare_pixel_scale();
        int w = (int)lroundf(win.x * scale), h = (int)lroundf(win.y * scale);
        if (w < 1 || h < 1) continue;   /* minimised */
        if (w != width || h != height) make_screen(w, h);
        float dt = fminf(bare_dt(), 0.1f);
        if (dt > 0) fps += (1 / fmaxf(dt, 1e-4f) - fps) * 0.05f;

        BareState view = { 0 };   /* (bare_stack runs its body once; MSVC can't tell) */
        bare_stack(.width = BARE_GROW, .height = BARE_GROW) {
            view = bare_box(.width = BARE_GROW, .height = BARE_GROW, .image = display, .color = BARE_RGB(150, 160, 170),
                            .cursor = BARE_CURSOR_MOVE);
            if (bare_key_pressed(BARE_KEY_H)) cfg.panel = !cfg.panel;
            if (cfg.panel && panel(fps)) waves_dirty = true;
        }
        fly(dt, view, &last_mouse);

        if (waves_dirty) spectrum(), waves_dirty = false;
        float t = (float)fmod(bare_time() + cfg.time, (double)LOOP);
        simulate(t, frame_no == 0 ? 1.0f : dt);   /* the first frame starts with some foam */

        Frame f;
        memset(&f, 0, sizeof f);
        camera(&f);
        light(&f);
        float wd = cfg.direction * PI_F / 180;
        set4(f.wind, cosf(wd), sinf(wd), cfg.wind, cfg.foam);
        set4(f.misc, t, 0.85f, t * 0.004f * cosf(wd) * (0.5f + cfg.wind / 20), t * 0.004f * sinf(wd) * (0.5f + cfg.wind / 20));
        set4(f.extra, 0.8f, (float)frame_no, dt, frame_no < 2 ? 1.0f : 0.0f);
        render(&f);
        frame_no++;
        bare_continuous(true);
    }
    bare_quit();
}
