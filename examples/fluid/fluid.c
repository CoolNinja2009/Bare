/* fluid.c: a GPU fluid simulation on Bare canvases; Screen+'s "Fluids" wallpaper.
 *
 * A port of WebGL Fluid Simulation by Pavel Dobryakov
 * (github.com/PavelDoGreat/WebGL-Fluid-Simulation, MIT licence, (c) 2017
 * Pavel Dobryakov) with the settings of its Lively Wallpaper edition. The
 * shaders in shaders/ are the original ones in GLSL 450.
 *
 * Move the mouse to stir. Space: a burst of colour.
 *
 * A Screen+ app wallpaper: settings are the Customise panel (LivelyProperties.json
 * beside the exe), sent by Screen+ on stdin (see ../screenplus/screenplus_wallpaper.h);
 * by hand they're arguments: fluid quality=1 vorticity=40 bgColor=#102030
 *
 * Once the fluid has settled the app stops simulating and Bare sleeps, so it
 * costs no CPU or GPU until you touch it again; a browser keeps going forever.
 * While Screen+ has it paused it draws nothing. Not ported: the audio
 * visualiser and the background picture. */
#include "bare.h"
#include "fluid_shaders.h"
#include "../screenplus/screenplus_wallpaper.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- settings ------------------------------------------------------------ */

typedef struct Settings {
    int quality, sim_quality;     /* 0..3 (low to high) */
    float splat_radius, curl, density_dissipation, velocity_dissipation, pressure;
    float bloom_intensity, bloom_threshold, sunrays_weight;
    bool shading, colorful, bloom, sunrays, random_splats;
    BareColor background;
} Settings;

static Settings cfg = {
    .quality = 3, .sim_quality = 2,
    .splat_radius = 0.2f, .curl = 20, .density_dissipation = 1, .velocity_dissipation = 1, .pressure = 0.1f,
    .bloom_intensity = 0.15f, .bloom_threshold = 0.75f, .sunrays_weight = 0.4f,
    .shading = true, .colorful = true, .bloom = true, .sunrays = true,
    .background = { 0, 0, 0, 255 },
};

static int hex(char c) {
    c = (char)(c >= 'A' && c <= 'F' ? c + 32 : c);
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* The page's Lively properties, scaled as its livelyPropertyListener does. */
static void set_property(const SpMessage *m) {
    const char *n = m->name;
    double v = sp_num(m);
    if (!strcmp(n, "quality")) cfg.quality = 3 - (int)fmin(3, fmax(0, v));   /* High .. Very low */
    else if (!strcmp(n, "simResolution")) cfg.sim_quality = (int)fmin(3, fmax(0, v));
    else if (!strcmp(n, "densityDiffusion")) cfg.density_dissipation = (float)(v / 10);
    else if (!strcmp(n, "velocityDiffusion")) cfg.velocity_dissipation = (float)(v / 100);
    else if (!strcmp(n, "pressure")) cfg.pressure = (float)(v / 100);
    else if (!strcmp(n, "vorticity")) cfg.curl = (float)v;
    else if (!strcmp(n, "splatRadius")) cfg.splat_radius = (float)(v / 100);
    else if (!strcmp(n, "shading")) cfg.shading = sp_bool(m);
    else if (!strcmp(n, "colorful")) cfg.colorful = sp_bool(m);
    else if (!strcmp(n, "bloomEnable")) cfg.bloom = sp_bool(m);
    else if (!strcmp(n, "bloomIntensity")) cfg.bloom_intensity = (float)(v / 100);
    else if (!strcmp(n, "bloomThreshold")) cfg.bloom_threshold = (float)(v / 100);
    else if (!strcmp(n, "sunRaysEnable")) cfg.sunrays = sp_bool(m);
    else if (!strcmp(n, "sunRaysWeight")) cfg.sunrays_weight = (float)(v / 100);
    else if (!strcmp(n, "randomSplats")) cfg.random_splats = sp_bool(m);
    else if (!strcmp(n, "bgColor") && m->value[0] == '#' && strlen(m->value) == 7) {
        int c[6];
        for (int i = 0; i < 6; i++)
            if ((c[i] = hex(m->value[1 + i])) < 0) return;
        cfg.background = BARE_RGBA(c[0] * 16 + c[1], c[2] * 16 + c[3], c[4] * 16 + c[5], 255);
    }
}

static const int DYE_RES[4] = { 128, 256, 512, 1024 };
static const int SIM_RES[4] = { 32, 64, 128, 256 };
enum { PRESSURE_ITERATIONS = 20, BLOOM_LEVELS = 8, BLOOM_RES = 256, SUNRAYS_RES = 196 };
#define SPLAT_FORCE     6000.0f
#define BLOOM_SOFT_KNEE 0.7f

/* ---- GPU state ----------------------------------------------------------- */

typedef struct Target { BareImage image; int w, h; } Target;
typedef struct Pair { Target read, write; } Pair;   /* ping-pong: read last step, write this one */

static struct {
    BareShader curl, vorticity, divergence, fade, pressure, gradient, advect, splat,
               bloom_prefilter, bloom_blur, sunrays_mask, sunrays, blur, display;
} fx;

static Pair dye, velocity, pressure;
static Target divergence, curl, bloom, bloom_levels[BLOOM_LEVELS], sunrays, sunrays_temp, display;
static int bloom_count;
static int width, height;            /* the display, in physical pixels */
static int dye_res, sim_res;         /* what the targets were made for */

/* Shader params, laid out as the shaders' std140 blocks. */
typedef struct { float texel[2]; } TexelParams;
typedef struct { float texel[2], curl, dt; } VorticityParams;
typedef struct { float texel[2], dt, dissipation; } AdvectParams;
typedef struct { float point[2], aspect, radius, color[4]; } SplatParams;
typedef struct { float texel[2], intensity; } BlurParams;
typedef struct { float texel[2], shading, bloom, sunrays; } DisplayParams;

static Target target(int w, int h, BareFormat format, bool nearest) {
    return (Target){ bare_canvas(.width = w, .height = h, .format = format, .nearest = nearest), w, h };
}

static void drop(Target *t) {
    if (t->image) bare_image_free(t->image);
    *t = (Target){ 0 };
}

static void swap(Pair *p) {
    Target t = p->read;
    p->read = p->write;
    p->write = t;
}

/* Resizes a pair, carrying its contents over so a resize doesn't wipe the fluid. */
static void resize_pair(Pair *p, int w, int h, BareFormat format) {
    if (p->read.w == w && p->read.h == h) return;
    Target t = target(w, h, format, false);
    if (p->read.image) {
        float one = 1.0f;
        bare_pass(.target = t.image, .shader = fx.fade, .inputs = { p->read.image }, BARE_PARAMS(one));
    }
    drop(&p->read);
    drop(&p->write);
    p->read = t;
    p->write = target(w, h, format, false);
}

/* `res` pixels on the short side, the display's aspect ratio on the long one. */
static void resolution(int res, int *w, int *h) {
    float aspect = (float)width / (float)height;
    if (aspect < 1) aspect = 1 / aspect;
    int lo = res, hi = (int)lroundf((float)res * aspect);
    *w = width > height ? hi : lo;
    *h = width > height ? lo : hi;
}

static void init_targets(void) {
    int sw, sh, dw, dh;
    resolution(SIM_RES[cfg.sim_quality], &sw, &sh);
    resolution(DYE_RES[cfg.quality], &dw, &dh);
    resize_pair(&dye, dw, dh, BARE_RGBA16F);
    resize_pair(&velocity, sw, sh, BARE_RG16F);

    drop(&divergence), drop(&curl), drop(&pressure.read), drop(&pressure.write);
    divergence = target(sw, sh, BARE_R16F, true);
    curl = target(sw, sh, BARE_R16F, true);
    pressure.read = target(sw, sh, BARE_R16F, true);
    pressure.write = target(sw, sh, BARE_R16F, true);

    int bw, bh;
    resolution(BLOOM_RES, &bw, &bh);
    drop(&bloom);
    bloom = target(bw, bh, BARE_RGBA16F, false);
    for (int i = 0; i < bloom_count; i++) drop(&bloom_levels[i]);
    bloom_count = 0;
    for (int i = 0; i < BLOOM_LEVELS; i++) {
        int w = bw >> (i + 1), h = bh >> (i + 1);
        if (w < 2 || h < 2) break;
        bloom_levels[bloom_count++] = target(w, h, BARE_RGBA16F, false);
    }

    int rw, rh;
    resolution(SUNRAYS_RES, &rw, &rh);
    drop(&sunrays), drop(&sunrays_temp);
    sunrays = target(rw, rh, BARE_R16F, false);
    sunrays_temp = target(rw, rh, BARE_R16F, false);

    if (display.w != width || display.h != height) {
        drop(&display);
        display = target(width, height, BARE_RGBA8, false);
    }
    dye_res = cfg.quality;
    sim_res = cfg.sim_quality;
}

/* ---- simulation ---------------------------------------------------------- */

typedef struct Rgb { float r, g, b; } Rgb;

static float random01(void) { return (float)rand() / (float)RAND_MAX; }

static Rgb hsv(float h, float s, float v) {
    int i = (int)floorf(h * 6);
    float f = h * 6 - (float)i, p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    switch (i % 6) {
    case 0: return (Rgb){ v, t, p };
    case 1: return (Rgb){ q, v, p };
    case 2: return (Rgb){ p, v, t };
    case 3: return (Rgb){ p, q, v };
    case 4: return (Rgb){ t, p, v };
    default: return (Rgb){ v, p, q };
    }
}

static Rgb random_color(void) {
    Rgb c = hsv(random01(), 1, 1);
    return (Rgb){ c.r * 0.15f, c.g * 0.15f, c.b * 0.15f };
}

/* Adds force (dx, dy) and colour c around (x, y), all in 0..1 texture space. */
static void splat(float x, float y, float dx, float dy, Rgb c) {
    float aspect = (float)width / (float)height;
    float radius = cfg.splat_radius / 100;
    if (aspect > 1) radius *= aspect;
    SplatParams p = { { x, y }, aspect, radius, { dx, dy, 0, 0 } };
    bare_pass(.target = velocity.write.image, .shader = fx.splat, .inputs = { velocity.read.image }, BARE_PARAMS(p));
    swap(&velocity);
    p.color[0] = c.r, p.color[1] = c.g, p.color[2] = c.b;
    bare_pass(.target = dye.write.image, .shader = fx.splat, .inputs = { dye.read.image }, BARE_PARAMS(p));
    swap(&dye);
}

static void random_splats(int count) {
    for (int i = 0; i < count; i++) {
        Rgb c = cfg.colorful ? random_color() : (Rgb){ 0.15f, 0.15f, 0.15f };
        c.r *= 10, c.g *= 10, c.b *= 10;
        splat(random01(), random01(), 1000 * (random01() - 0.5f), 1000 * (random01() - 0.5f), c);
    }
}

static void step(float dt) {
    TexelParams tp = { { 1.0f / (float)velocity.read.w, 1.0f / (float)velocity.read.h } };

    bare_pass(.target = curl.image, .shader = fx.curl, .inputs = { velocity.read.image }, BARE_PARAMS(tp));
    VorticityParams vp = { { tp.texel[0], tp.texel[1] }, cfg.curl, dt };
    bare_pass(.target = velocity.write.image, .shader = fx.vorticity,
              .inputs = { velocity.read.image, curl.image }, BARE_PARAMS(vp));
    swap(&velocity);

    bare_pass(.target = divergence.image, .shader = fx.divergence, .inputs = { velocity.read.image }, BARE_PARAMS(tp));
    bare_pass(.target = pressure.write.image, .shader = fx.fade, .inputs = { pressure.read.image },
              BARE_PARAMS(cfg.pressure));
    swap(&pressure);
    for (int i = 0; i < PRESSURE_ITERATIONS; i++) {
        bare_pass(.target = pressure.write.image, .shader = fx.pressure,
                  .inputs = { pressure.read.image, divergence.image }, BARE_PARAMS(tp));
        swap(&pressure);
    }
    bare_pass(.target = velocity.write.image, .shader = fx.gradient,
              .inputs = { pressure.read.image, velocity.read.image }, BARE_PARAMS(tp));
    swap(&velocity);

    AdvectParams ap = { { tp.texel[0], tp.texel[1] }, dt, cfg.velocity_dissipation };
    bare_pass(.target = velocity.write.image, .shader = fx.advect,
              .inputs = { velocity.read.image, velocity.read.image }, BARE_PARAMS(ap));
    swap(&velocity);
    ap.dissipation = cfg.density_dissipation;
    bare_pass(.target = dye.write.image, .shader = fx.advect,
              .inputs = { velocity.read.image, dye.read.image }, BARE_PARAMS(ap));
    swap(&dye);
}

static void apply_bloom(void) {
    if (bloom_count < 2) return;
    float knee = cfg.bloom_threshold * BLOOM_SOFT_KNEE + 0.0001f;
    float curve[4] = { cfg.bloom_threshold - knee, knee * 2, 0.25f / knee, cfg.bloom_threshold };
    bare_pass(.target = bloom.image, .shader = fx.bloom_prefilter, .inputs = { dye.read.image }, BARE_PARAMS(curve));

    Target last = bloom;
    for (int i = 0; i < bloom_count; i++) {   /* down the chain... */
        BlurParams p = { { 1.0f / (float)last.w, 1.0f / (float)last.h }, 1 };
        bare_pass(.target = bloom_levels[i].image, .shader = fx.bloom_blur, .inputs = { last.image }, BARE_PARAMS(p));
        last = bloom_levels[i];
    }
    for (int i = bloom_count - 2; i >= 0; i--) {   /* ...and back up, adding each level */
        BlurParams p = { { 1.0f / (float)last.w, 1.0f / (float)last.h }, 1 };
        bare_pass(.target = bloom_levels[i].image, .shader = fx.bloom_blur, .inputs = { last.image },
                  .blend = BARE_BLEND_ADD, BARE_PARAMS(p));
        last = bloom_levels[i];
    }
    BlurParams p = { { 1.0f / (float)last.w, 1.0f / (float)last.h }, cfg.bloom_intensity };
    bare_pass(.target = bloom.image, .shader = fx.bloom_blur, .inputs = { last.image }, BARE_PARAMS(p));
}

static void apply_sunrays(void) {
    bare_pass(.target = dye.write.image, .shader = fx.sunrays_mask, .inputs = { dye.read.image });
    bare_pass(.target = sunrays.image, .shader = fx.sunrays, .inputs = { dye.write.image },
              BARE_PARAMS(cfg.sunrays_weight));
    TexelParams h = { { 1.0f / (float)sunrays.w, 0 } }, v = { { 0, 1.0f / (float)sunrays.h } };
    bare_pass(.target = sunrays_temp.image, .shader = fx.blur, .inputs = { sunrays.image }, BARE_PARAMS(h));
    bare_pass(.target = sunrays.image, .shader = fx.blur, .inputs = { sunrays_temp.image }, BARE_PARAMS(v));
}

static void render(void) {
    if (cfg.bloom) apply_bloom();
    if (cfg.sunrays) apply_sunrays();
    DisplayParams p = { { 1.0f / (float)width, 1.0f / (float)height }, cfg.shading, cfg.bloom, cfg.sunrays };
    bare_pass(.target = display.image, .shader = fx.display,
              .inputs = { dye.read.image, bloom.image, sunrays.image }, BARE_PARAMS(p));
}

/* How long until velocity (and dye, if it fades) is too small to see: then
 * the picture stops changing and the app can sleep. */
static double settle_seconds(void) {
    if (cfg.velocity_dissipation < 0.05f) return INFINITY;   /* it never stops moving */
    double t = 7.0 / cfg.velocity_dissipation;
    if (cfg.density_dissipation > 0.05f && 7.0 / cfg.density_dissipation > t) t = 7.0 / cfg.density_dissipation;
    return t;
}

/* ---- the app ------------------------------------------------------------- */

int main(int argc, char **argv) {
    SpArgs sa = sp_args(argc, argv);
    if (!bare_window_with(.width = 1280, .height = 720, .title = "Fluids", .parent = sa.parent)) return 1;
    fx.curl = bare_shader(&curl_shader);
    fx.vorticity = bare_shader(&vorticity_shader);
    fx.divergence = bare_shader(&divergence_shader);
    fx.fade = bare_shader(&fade_shader);
    fx.pressure = bare_shader(&pressure_shader);
    fx.gradient = bare_shader(&gradient_shader);
    fx.advect = bare_shader(&advect_shader);
    fx.splat = bare_shader(&splat_shader);
    fx.bloom_prefilter = bare_shader(&bloom_prefilter_shader);
    fx.bloom_blur = bare_shader(&bloom_blur_shader);
    fx.sunrays_mask = bare_shader(&sunrays_mask_shader);
    fx.sunrays = bare_shader(&sunrays_shader);
    fx.blur = bare_shader(&blur_shader);
    fx.display = bare_shader(&display_shader);

    bool started = false, paused = false;
    double last_active = 0, last_pointer_reset = -10, color_timer = 0, next_random = 3.5;
    Rgb pointer_color = random_color();
    BareVec2 last_mouse = bare_mouse();

    while (bare_frame()) {
        bool dirty = false;   /* the picture needs redrawing even if the fluid is still */
        SpMessage msg;
        while (sp_next(&msg)) {
            if (msg.kind == SP_PROPERTY) set_property(&msg), dirty = true;
            else if (msg.kind == SP_PAUSE) paused = sp_bool(&msg);
        }
        double now = bare_time();
        float dt = fminf(bare_dt(), 1.0f / 60.0f);

        /* Targets follow the window's size in physical pixels. */
        BareVec2 win = bare_window_size();
        float scale = bare_pixel_scale();
        int w = (int)lroundf(win.x * scale), h = (int)lroundf(win.y * scale);
        if (w < 1 || h < 1) continue;   /* minimised */
        if (w != width || h != height || dye_res != cfg.quality || sim_res != cfg.sim_quality) {
            width = w, height = h;
            init_targets();
            dirty = true;
        }
        if (!started) {   /* a few splats to start with, as the original does */
            random_splats(5 + rand() % 20);
            started = true;
            last_active = now;
        }

        /* The fluid fills the window, over the background colour. */
        BareState canvas = bare_box(.width = BARE_GROW, .height = BARE_GROW, .image = display.image,
                                    .color = cfg.background);
        if (paused) {   /* the picture stays as it is; nothing runs until Screen+ resumes it */
            bare_continuous(false);
            continue;
        }

        if (bare_key_pressed(BARE_KEY_SPACE)) random_splats(5 + rand() % 20), last_active = now;
        if (cfg.random_splats && now >= next_random) {   /* the page's 3.5 s timer */
            random_splats(5 + rand() % 20), last_active = now;
            next_random = now + 3.5;
        }

        /* Stir with the mouse, like the original: every move splats, and the
         * pointer picks a new colour after a second without moving. */
        BareVec2 m = bare_mouse();
        if ((m.x != last_mouse.x || m.y != last_mouse.y) && canvas.hovered && win.x > 0 && win.y > 0) {
            if (now - last_pointer_reset > 1.0) {
                last_pointer_reset = now;
                pointer_color = random_color();
            } else {
                float aspect = win.x / win.y;
                float dx = (m.x - last_mouse.x) / win.x, dy = (m.y - last_mouse.y) / win.y;
                if (aspect < 1) dx *= aspect;
                if (aspect > 1) dy /= aspect;
                splat(m.x / win.x, m.y / win.y, dx * SPLAT_FORCE, dy * SPLAT_FORCE, pointer_color);
            }
            last_active = now;
        }
        last_mouse = m;

        if (cfg.colorful && (color_timer += dt * 10) >= 1) {
            color_timer = fmod(color_timer, 1.0);
            pointer_color = random_color();
        }

        /* Simulate until the fluid settles, then let Bare sleep. */
        bool running = now - last_active < settle_seconds();
        if (running) step(dt);
        if (running || dirty || now == last_active) render();
        bare_continuous(running);
        if (!running && cfg.random_splats) bare_wake_in((float)fmax(0, next_random - now));
    }
    bare_quit();
}
