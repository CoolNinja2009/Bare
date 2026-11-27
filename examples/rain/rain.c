/* rain.c: rain (or snow) on a window over a picture in 3D; Screen+'s "Rain" wallpaper.
 *
 * A port of the page (Rainy Window by rocksdanister, CC BY-NC-SA 3.0, after
 * "Heartfelt" by Martijn Steinrucken; modified for Screen+): one fragment pass,
 * shaders/rain.glsl, which is the page's rain.frag, over the picture and its
 * depth map (assets/depth/<picture>.png; 255 near), then the page's CSS: the
 * canvas scaled 1.09 and faded in. From the depth map the page makes a landing
 * map, where a drop at each distance meets the picture; so does this. The
 * mouse moves the picture in depth (smoothed), or slides it when it has no
 * depth map. The page's looping rain sound is not played.
 *
 * A Screen+ app wallpaper: settings are the Customise panel (LivelyProperties.json
 * beside the exe), sent by Screen+ on stdin (see ../screenplus/screenplus_wallpaper.h);
 * by hand they're arguments: rain weather=1 rainIntensity=80 mediaSelect=media/other.jpg
 * While Screen+ has it paused it draws nothing. */
#include "bare.h"
#include "rain_shaders.h"
#include "../screenplus/screenplus_wallpaper.h"
#include "../screenplus/mips.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RESET_AFTER 21600.0   /* the page restarts its clock every 6 hours */
#define LAND_BINS 32

/* ---- settings ---------------------------------------------------------------- */

static struct {
    double intensity, speed, brightness, normal, zoom, blur, depth_amt, scene, splash, parallax, scaling;
    int iterations, weather;
    bool panning, post, lightning, fill, fps_lock;
    char media[1024];
} cfg = { 0.4, 0.25, 0.8, 0.5, 2.61, 0.5, 0.02, 0.6, 0.3, 1, 1, 16, 0, false, true, false, true, false, "" };

static bool media_changed;

static void set_property(const SpMessage *m) {
    const char *n = m->name;
    double v = sp_num(m);
    if (!strcmp(n, "blurIntensity")) cfg.blur = v / 100;
    else if (!strcmp(n, "blurQuality")) { static const int it[] = { 1, 16, 32, 64 }; cfg.iterations = it[(int)v & 3]; }
    else if (!strcmp(n, "rainIntensity")) cfg.intensity = v / 100;
    else if (!strcmp(n, "rainSpeed")) cfg.speed = v / 100;
    else if (!strcmp(n, "brightness")) cfg.brightness = v / 100;
    else if (!strcmp(n, "rainNormal")) cfg.normal = v / 100;
    else if (!strcmp(n, "rainZoom")) cfg.zoom = v / 100;
    else if (!strcmp(n, "weather")) cfg.weather = (int)v;
    else if (!strcmp(n, "sceneAmount")) cfg.scene = v / 100;
    else if (!strcmp(n, "splashAmount")) cfg.splash = v / 100;
    else if (!strcmp(n, "depthStrength")) cfg.depth_amt = 0.06 * v / 100;
    else if (!strcmp(n, "mediaScaling")) cfg.fill = v != 0;
    else if (!strcmp(n, "animateChk")) cfg.panning = sp_bool(m);
    else if (!strcmp(n, "lightningChk")) cfg.lightning = sp_bool(m);
    else if (!strcmp(n, "postProcessingChk")) cfg.post = sp_bool(m);
    else if (!strcmp(n, "parallaxIntensity")) cfg.parallax = v;
    else if (!strcmp(n, "fpsLock")) cfg.fps_lock = sp_bool(m);
    else if (!strcmp(n, "displayScaling")) cfg.scaling = fmin(1, fmax(0.1, v));
    else if (!strcmp(n, "mediaSelect")) {
        snprintf(cfg.media, sizeof cfg.media, "%s", m->value);
        media_changed = true;
    }
}

/* ---- the picture, its depth map and landing map ------------------------------------ */

static Mips pic;
static int pic_w, pic_h;
static BareImage depth, land;

static unsigned char *load_file_image(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *bytes = n > 0 ? malloc((size_t)n) : NULL, *rgba = NULL;
    if (bytes && fread(bytes, 1, (size_t)n, f) == (size_t)n) rgba = bare_image_decode(bytes, (int)n, w, h);
    free(bytes);
    fclose(f);
    return rgba;
}

/* The page's landingMap: for every column and every distance (LAND_BINS steps,
 * 0 near .. 1 far), where a drop falling at that distance first meets something
 * as near as it is; row b holds that height (0..1 from the bottom, 16 bits in R
 * and G; A 0: it never lands). The depth map is at most 1024 wide here, as the
 * page draws it (wider ones are point-sampled down, close to the browser's). */
static BareImage landing_map(const unsigned char *rgba, int iw, int ih) {
    int W = iw < 1024 ? iw : 1024, H = (int)fmax(1, floor(ih * (double)W / iw + 0.5));
    unsigned char *out = calloc((size_t)W * LAND_BINS, 4);
    if (!out) return 0;
    for (int x = 0; x < W; x++) {
        double nearest = 1;
        int reached = LAND_BINS;
        int sx = (int)((x + 0.5) * iw / W);
        for (int y = 0; y < H && reached > 0; y++) {
            int sy = (int)((y + 0.5) * ih / H);
            nearest = fmin(nearest, 1 - rgba[((size_t)sy * iw + sx) * 4] / 255.0);
            int first = (int)fmax(0, ceil((nearest + 0.04) * LAND_BINS - 0.5));
            if (first >= reached) continue;
            int v = (int)floor((1 - (y + 0.5) / H) * 65535 + 0.5);
            for (int b = first; b < reached; b++) {
                unsigned char *o = out + ((size_t)b * W + x) * 4;
                o[0] = (unsigned char)(v >> 8), o[1] = (unsigned char)(v & 255), o[3] = 255;
            }
            reached = first;
        }
    }
    BareImage im = bare_image_from_pixels(W, LAND_BINS, out);
    free(out);
    return im;
}

/* "media/x.jpg" (or a bare name, which is in media/) beside the exe, or a full path. */
static const char *media_path(const char *v, char *out, size_t cap) {
    FILE *f = fopen(v, "rb");
    if (f) { fclose(f); snprintf(out, cap, "%s", v); return out; }
    char rel[1100];
    snprintf(rel, sizeof rel, "%s%s", strchr(v, '/') || strchr(v, '\\') ? "" : "media/", v);
    for (char *c = rel; *c; c++) if (*c == '\\') *c = '/';
    snprintf(out, cap, "%s", sp_asset(rel));
    return out;
}

static void load_media(void) {
    free_mips(&pic);
    if (depth) bare_image_free(depth), depth = 0;
    if (land) bare_image_free(land), land = 0;
    char path[4300];
    media_path(cfg.media, path, sizeof path);
    unsigned char *rgba = load_file_image(path, &pic_w, &pic_h);
    if (rgba) pic = make_mips(rgba, pic_w, pic_h);
    free(rgba);
    /* depth/<name of the picture>.png */
    const char *name = path;
    for (const char *c = path; *c; c++) if (*c == '/' || *c == '\\') name = c + 1;
    char base[1024], rel[1100];
    snprintf(base, sizeof base, "%.1000s", name);
    char *dot = strrchr(base, '.');
    if (dot && dot != base) *dot = 0;
    snprintf(rel, sizeof rel, "depth/%s.png", base);
    int dw, dh;
    unsigned char *d = load_file_image(sp_asset(rel), &dw, &dh);
    if (d) {
        depth = bare_image_from_pixels(dw, dh, d);
        land = landing_map(d, dw, dh);
        free(d);
    }
}

typedef struct Params {
    float p[7][4];
    float lv[MAX_LEVELS][4];
} Params;

int main(int argc, char **argv) {
    snprintf(cfg.media, sizeof cfg.media, "media/mountain-surrounded-with-fog-1772973.jpg");
    SpArgs sp = sp_args(argc, argv);
    if (!bare_window_with(.width = sp.width ? sp.width : 1280, .height = sp.height ? sp.height : 720, .title = "Rain", .parent = sp.parent)) return 1;
    bare_background(BARE_BLACK);
    BareShader shader = bare_shader(&rain_shader);
    static const unsigned char blank[4] = { 0, 0, 0, 255 };
    BareImage none = bare_image_from_pixels(1, 1, blank);
    BareImage canvas = 0;
    int cw = 0, ch = 0;
    bool paused = false, moved = false, loaded = false;
    double clock = 0, last = -1, last_draw = -1e9, shown_at = -1;
    double mx = 0, my = 0, tx = 0, ty = 0;
    BareVec2 last_mouse = { -1e9f, -1e9f }, offset = { 0, 0 };

    while (bare_frame()) {
        double now = bare_time();
        SpMessage m;
        while (sp_next(&m)) {
            if (m.kind == SP_PROPERTY) set_property(&m);
            else if (m.kind == SP_PAUSE) paused = sp_bool(&m);
        }
        if (media_changed || !loaded) load_media(), media_changed = false, loaded = true;
        BareVec2 win = bare_window_size();
        int w = (int)fmax(1, floor(win.x * cfg.scaling)), h = (int)fmax(1, floor(win.y * cfg.scaling));
        if (w != cw || h != ch) {
            if (canvas) bare_image_free(canvas);
            canvas = bare_canvas(.width = w, .height = h, .bottom_up = true);
            cw = w, ch = h, last_draw = -1e9;
        }

        /* the mouse: in depth with a depth map, else the page slides its container */
        BareVec2 mo = bare_mouse();
        if (mo.x != last_mouse.x || mo.y != last_mouse.y) {
            if (last_mouse.x > -1e8f) moved = true;
            last_mouse = mo;
        }
        if (moved && depth) {
            tx = (mo.x / win.x - 0.5) * 2, ty = -(mo.y / win.y - 0.5) * 2;
            if (mo.x < 0 || mo.y < 0) tx = ty = 0;
            offset = (BareVec2){ 0, 0 };
        } else if (moved && cfg.parallax != 0)
            offset = (BareVec2){ (float)((win.x - mo.x * cfg.parallax) / 90), (float)((win.y - mo.y * cfg.parallax) / 90) };

        /* the page fades its container in, a tenth every 75 ms */
        double op = shown_at < 0 ? 0 : fmin(1, floor((now - shown_at) / 0.075 + 1e-9) * 0.1);
        bare_stack(.width = BARE_GROW, .height = BARE_GROW, .color = BARE_BLACK, .clip = true) {
            bare_box(.position = BARE_ABSOLUTE, .width = win.x, .height = win.y, .image = canvas, .scale = 1.09f,
                     .offset = offset, .opacity = (float)op);
        }
        if (op < 1 && shown_at >= 0) bare_wake_in(0.075f);

        if (paused) { bare_continuous(false); last = -1; continue; }
        double interval = 1 / (cfg.fps_lock ? 30.0 : 60.0);
        if (now - last_draw < interval - 0.002) {
            bare_wake_in((float)(last_draw + interval - 0.002 - now));
            continue;
        }
        last_draw = now;
        if (last >= 0) clock += now - last;
        last = now;
        if (clock > RESET_AFTER) clock = 0;
        mx += (tx - mx) * 0.12, my += (ty - my) * 0.12;
        if (shown_at < 0) shown_at = now;

        Params p;
        memset(&p, 0, sizeof p);
        float(*q)[4] = p.p;
        q[0][0] = (float)w, q[0][1] = (float)h;
        q[0][2] = (float)(pic.levels ? pic_w : w), q[0][3] = (float)(pic.levels ? pic_h : h);
        q[1][0] = (float)clock, q[1][1] = (float)cfg.speed, q[1][2] = (float)cfg.intensity, q[1][3] = (float)cfg.normal;
        q[2][0] = (float)cfg.brightness, q[2][1] = (float)cfg.blur, q[2][2] = (float)cfg.zoom, q[2][3] = (float)cfg.iterations;
        q[3][0] = cfg.panning, q[3][1] = cfg.post, q[3][2] = cfg.lightning, q[3][3] = cfg.fill;
        q[4][0] = depth ? 1.f : 0.f, q[4][1] = (float)mx, q[4][2] = (float)my, q[4][3] = (float)cfg.depth_amt;
        q[5][0] = (float)cfg.weather, q[5][1] = (float)cfg.scene, q[5][2] = (float)cfg.splash;
        q[5][3] = (float)(pic.levels ? pic.levels : 1);
        q[6][0] = (float)(pic.aw ? pic.aw : 1), q[6][1] = (float)(pic.ah ? pic.ah : 1), q[6][3] = pic.levels ? 1.f : 0.f;
        memcpy(p.lv, pic.lv, sizeof p.lv);
        bare_pass(.target = canvas, .shader = shader, .inputs = { pic.atlas ? pic.atlas : none, depth ? depth : none, land ? land : none },
                  BARE_PARAMS(p));
        bare_continuous(false);
        bare_wake_in((float)interval - 0.002f);
    }
    bare_quit();
}
