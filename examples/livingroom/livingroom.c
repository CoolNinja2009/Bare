/* Living Room: Screen+'s "Living Room" sample (rocksdanister's isometric room,
 * three.js r147) in C. The room is the page's Room.glb, lit as the page lights
 * it (an ambient light, the music light by the TV, the lamp's warm light, and
 * a spot light from above whose PCF-soft shadow falls on an invisible floor),
 * drawn by examples/screenplus/three_lite.h through an orthographic camera in
 * a tilted container. The TV shows the track (album art, or its title), or
 * the "no signal" loop when nothing plays; the clock on the cabinet shows the
 * time; the picture frame the chosen photo. Music drives the TV light.
 *
 * Properties: imgSelect (a photo, "photos/<file>"), xAngleCamera, yAngleCamera,
 * zoomCamera, 30fps (24 fps cap). The room is drawn only when something in it
 * changes. */
#include "bare.h"
#include "../screenplus/screenplus_wallpaper.h"
#include "../screenplus/three_lite.h"
#include "../screenplus/colorthief.h"
#include "generated/livingroom_shaders.h"
#include <time.h>

#define DATE0 1767225600000.0   /* the test clock's epoch (2026-01-01), as in the harness */

static unsigned char *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) free(buf), buf = NULL;
    fclose(f);
    if (size) *size = buf ? (size_t)n : 0;
    return buf;
}

/* A file beside the program (Screen+ passes folder/file paths), else among the assets. */
static unsigned char *read_near(const char *rel, size_t *size) {
    char dir[4096] = "";
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, dir, sizeof dir);
    if (n == 0 || n >= sizeof dir) dir[0] = 0;
    char *slash = strrchr(dir, '\\');
#else
    ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
    dir[n > 0 ? n : 0] = 0;
    char *slash = strrchr(dir, '/');
#endif
    if (slash) {
        slash[1] = 0;
        char path[4400];
        snprintf(path, sizeof path, "%s%s", dir, rel);
        unsigned char *b = read_file(path, size);
        if (b) return b;
    }
    return read_file(sp_asset(rel), size);
}

/* ---- the page's canvases. Kept as straight colour, which is what WebGL's upload (premultiplyAlpha false)
 * gives back: any covered pixel at its full colour, however faint; the opaque screens then ignore alpha. ---- */

typedef struct Canvas { int w, h; unsigned char *px; BareImage tex; bool srgb, repeat; } Canvas;

static void canvas_init(Canvas *c, int w, int h, bool srgb, bool repeat) {
    c->w = w, c->h = h, c->srgb = srgb, c->repeat = repeat;
    c->px = calloc((size_t)w * h, 4);
}
static void canvas_upload(Canvas *c) {
    if (c->tex) bare_image_update(c->tex, c->px);
    else c->tex = bare_texture(.width = c->w, .height = c->h, .rgba8 = c->px, .mipmaps = true, .srgb = c->srgb, .repeat = c->repeat);
}
/* source-over of straight colour (r, g, b, a) onto a straight-alpha pixel */
static void blend_px(unsigned char *d, float r, float g, float b, float a) {
    float da = d[3] / 255.f, oa = a + da * (1 - a);
    if (oa <= 0) { d[0] = d[1] = d[2] = d[3] = 0; return; }
    float c[3] = { r, g, b };
    for (int k = 0; k < 3; k++) {
        float v = (c[k] * a + d[k] / 255.f * da * (1 - a)) / oa;
        d[k] = (unsigned char)fminf(255, floorf(v * 255 + 0.5f));
    }
    d[3] = (unsigned char)floorf(oa * 255 + 0.5f);
}
/* fillText: one line, white-ish colour, coverage from bare_text_pixels; align center; baseline y */
static void canvas_text(Canvas *c, const char *text, BareFont font, float size, float cx, float base_y, float rgb[3]) {
    int w, h, x, y;
    float adv;
    unsigned char *mask = bare_text_pixels(text, font, size, &w, &h, &x, &y, &adv);
    if (!mask) return;
    int ox = (int)floorf(cx - adv / 2 + 0.5f) + x, oy = (int)floorf(base_y + 0.5f) + y;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int px = ox + i, py = oy + j;
            if (px < 0 || py < 0 || px >= c->w || py >= c->h || !mask[j * w + i]) continue;
            blend_px(c->px + ((size_t)py * c->w + px) * 4, rgb[0], rgb[1], rgb[2], mask[j * w + i] / 255.f);
        }
    free(mask);
}

/* bilinear sample of an RGBA8 picture (clamped), straight colour 0..1 */
static void sample_rgba(const unsigned char *px, int w, int h, float x, float y, float o[4]) {
    x -= 0.5f, y -= 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0;
    for (int k = 0; k < 4; k++) o[k] = 0;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            int xx = x0 + i, yy = y0 + j;
            xx = xx < 0 ? 0 : xx >= w ? w - 1 : xx;
            yy = yy < 0 ? 0 : yy >= h ? h - 1 : yy;
            float wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            for (int k = 0; k < 4; k++) o[k] += px[((size_t)yy * w + xx) * 4 + k] / 255.f * wgt;
        }
}
/* drawImage(img, sx, sy, sw, sh, dx, dy, dw, dh), box-filtered when shrinking, times `bright` (filter: brightness) */
static void canvas_draw_image(Canvas *c, const unsigned char *img, int iw, int ih, double sx, double sy, double sw, double sh,
                              double dx, double dy, double dw, double dh, float bright) {
    int x0 = (int)fmax(0, floor(dx)), y0 = (int)fmax(0, floor(dy));
    int x1 = (int)fmin(c->w, ceil(dx + dw)), y1 = (int)fmin(c->h, ceil(dy + dh));
    double scx = sw / dw, scy = sh / dh;
    int nx = scx > 1 ? (int)ceil(scx) : 1, ny = scy > 1 ? (int)ceil(scy) : 1;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float acc[4] = { 0, 0, 0, 0 };
            for (int j = 0; j < ny; j++)
                for (int i = 0; i < nx; i++) {
                    double u = sx + (x + (i + 0.5) / nx - dx) * scx, v = sy + (y + (j + 0.5) / ny - dy) * scy;
                    float s[4];
                    sample_rgba(img, iw, ih, (float)u, (float)v, s);
                    for (int k = 0; k < 4; k++) acc[k] += s[k];
                }
            for (int k = 0; k < 4; k++) acc[k] /= (float)(nx * ny);
            blend_px(c->px + ((size_t)y * c->w + x) * 4, acc[0] * bright, acc[1] * bright, acc[2] * bright, acc[3]);
        }
}
/* the page's drawImageProp(ctx, img): cover, centred */
static void canvas_cover(Canvas *c, const unsigned char *img, int iw, int ih, float bright) {
    double w = c->w, h = c->h, r = fmin(w / iw, h / ih), nw = iw * r, nh = ih * r, ar = 1;
    if (floor(nw + 0.5) < w) ar = w / nw;
    if (fabs(ar - 1) < 1e-14 && floor(nh + 0.5) < h) ar = h / nh;
    nw *= ar, nh *= ar;
    double cw = iw / (nw / w), ch = ih / (nh / h), cx = (iw - cw) * 0.5, cy = (ih - ch) * 0.5;
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cw > iw) cw = iw;
    if (ch > ih) ch = ih;
    canvas_draw_image(c, img, iw, ih, cx, cy, cw, ch, 0, 0, w, h, bright);
}

/* filter: blur(sigma px) of a picture stretched over the whole canvas: the canvas stores premultiplied colour
 * with the blur's transparent surroundings, and the upload divides alpha back out, so each pixel is the blur's
 * weighted average of the picture within the canvas */
static void canvas_blur_cover(Canvas *c, const unsigned char *img, int iw, int ih, float sigma, float bright) {
    int w = c->w, h = c->h, r = (int)ceilf(sigma * 3);
    float *src = malloc(sizeof(float) * 3 * w * h), *tmp = malloc(sizeof(float) * 3 * w * h), *k = malloc(sizeof(float) * (2 * r + 1));
    for (int i = -r; i <= r; i++) k[i + r] = expf(-(float)(i * i) / (2 * sigma * sigma));
    double sx = (double)iw / w, sy = (double)ih / h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float o[4];
            sample_rgba(img, iw, ih, (float)((x + 0.5) * sx), (float)((y + 0.5) * sy), o);
            for (int q = 0; q < 3; q++) src[(y * w + x) * 3 + q] = o[q];
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float acc[3] = { 0, 0, 0 }, ws = 0;
            for (int i = -r; i <= r; i++) {
                int xx = x + i;
                if (xx < 0 || xx >= w) continue;
                for (int q = 0; q < 3; q++) acc[q] += src[(y * w + xx) * 3 + q] * k[i + r];
                ws += k[i + r];
            }
            for (int q = 0; q < 3; q++) tmp[(y * w + x) * 3 + q] = acc[q] / ws;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float acc[3] = { 0, 0, 0 }, ws = 0;
            for (int i = -r; i <= r; i++) {
                int yy = y + i;
                if (yy < 0 || yy >= h) continue;
                for (int q = 0; q < 3; q++) acc[q] += tmp[(yy * w + x) * 3 + q] * k[i + r];
                ws += k[i + r];
            }
            unsigned char *d = c->px + ((size_t)y * w + x) * 4;
            for (int q = 0; q < 3; q++) d[q] = (unsigned char)fminf(255, floorf(acc[q] / ws * bright * 255 + 0.5f));
            d[3] = 255;
        }
    free(src), free(tmp), free(k);
}

/* the body's --gradientColor1, a registered <color> with transition 3s (ease) */
static struct { float from[3], to[3]; double t0; } grad = { { 45, 45, 45 }, { 45, 45, 45 }, -1 };
static double now_s;
static double ease(double x) {   /* cubic-bezier(.25, .1, .25, 1) */
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    double lo = 0, hi = 1;
    for (int i = 0; i < 40; i++) {
        double t = (lo + hi) / 2, bx = 3 * (1 - t) * (1 - t) * t * 0.25 + 3 * (1 - t) * t * t * 0.25 + t * t * t;
        if (bx < x) lo = t; else hi = t;
    }
    double t = (lo + hi) / 2;
    return 3 * (1 - t) * (1 - t) * t * 0.1 + 3 * (1 - t) * t * t + t * t * t;
}
static void grad_now(float out[3]) {
    double p = grad.t0 < 0 ? 1 : ease((now_s - grad.t0) / 3);
    for (int i = 0; i < 3; i++) out[i] = (float)(grad.from[i] + (grad.to[i] - grad.from[i]) * p);
}
static void grad_set(float r, float g, float b) {
    if (r == grad.to[0] && g == grad.to[1] && b == grad.to[2]) return;
    grad_now(grad.from);
    grad.to[0] = r, grad.to[1] = g, grad.to[2] = b;
    grad.t0 = now_s;
}

/* ---- state ---- */

static struct {
    double x_angle, y_angle, zoom;
    bool cap24;
    char photo[512];
} cfg = { -30, 30, 1.25, true, "photos/jellyfish.jpg" };

static TlScene scene;
static TlObject *video_screen, *soundbar;
static Canvas artist, clock_c, photo;
static BareFont digib, arial;
static TlLights lights;
static bool signal_lost = false, signal_known = false;
static double music_intensity = 50;   /* musicLightIntensity * audioIntensity */
static float album[3] = { 225, 225, 225 };
static bool dirty = true;

static void set_light_color(float r, float g, float b) {
    lights.point[0].color[0] = r, lights.point[0].color[1] = g, lights.point[0].color[2] = b;
    if (soundbar) soundbar->mat.color[0] = r, soundbar->mat.color[1] = g, soundbar->mat.color[2] = b;
}

static void load_photo(const char *rel) {
    size_t n;
    unsigned char *bytes = read_near(rel, &n);
    if (!bytes) return;
    int w, h;
    unsigned char *img = bare_image_decode(bytes, (int)n, &w, &h);
    free(bytes);
    if (!img) return;
    memset(photo.px, 0, (size_t)photo.w * photo.h * 4);   /* clearRect */
    canvas_cover(&photo, img, w, h, 0.9f);                 /* filter: brightness(90%) */
    free(img);
    /* the gradient from rgba(55,55,55,.75) at the right edge to transparent at the left */
    for (int y = 0; y < photo.h; y++)
        for (int x = 0; x < photo.w; x++) {
            float t = (photo.w - (x + 0.5f)) / photo.w;   /* 0 at x = width; colours interpolate unpremultiplied */
            float a = 0.75f * (1 - t), v = 55.f / 255 * (1 - t);
            if (a > 0) blend_px(photo.px + ((size_t)y * photo.w + x) * 4, v, v, v, a);
        }
    canvas_upload(&photo);
    dirty = true;
}

static void draw_clock(double date_ms) {
    time_t t = (time_t)(date_ms / 1000);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char s[16];
    snprintf(s, sizeof s, "%02d:%02d", lt.tm_hour, lt.tm_min);
    memset(clock_c.px, 0, (size_t)clock_c.w * clock_c.h * 4);
    float white[3] = { 1, 1, 1 };
    /* textBaseline middle: half way between the font's ascent (900/1100 em) and descent (200/1100) */
    if (digib) canvas_text(&clock_c, s, digib, 125, 150, 150 + 125 * (900.0f - 200.0f) / 1100 / 2, white);
    canvas_upload(&clock_c);
    dirty = true;
}

static void on_track(const char *json) {
    signal_known = true;
    if (!json || !strcmp(json, "null") || !*json) {
        signal_lost = true;
        set_light_color(45 / 255.f, 45 / 255.f, 45 / 255.f);
        grad_set(45, 45, 45);
        dirty = true;
        return;
    }
    signal_lost = false;
    char title[512] = "";
    sp_json(json, "Title", title, sizeof title);
    char *thumb = sp_json_alloc(json, "Thumbnail");
    memset(artist.px, 0, (size_t)artist.w * artist.h * 4);
    if (!thumb || !*thumb || !strcmp(thumb, "null")) {
        float c[3] = { 225 / 255.f, 225 / 255.f, 225 / 255.f };
        if (arial) canvas_text(&artist, title, arial, strlen(title) > 20 ? 27 : 54, 320, 25 + 180, c);
        album[0] = album[1] = album[2] = 225;
        set_light_color(45 / 255.f, 45 / 255.f, 45 / 255.f);
        grad_set(45, 45, 45);
    } else {
        int n = 0;
        unsigned char *bytes = sp_base64(thumb, &n);
        int w = 0, h = 0;
        unsigned char *img = bytes ? bare_image_decode(bytes, n, &w, &h) : NULL;
        free(bytes);
        if (img) {
            int c[3];
            if (colorthief_get(img, w, h, c)) album[0] = (float)c[0], album[1] = (float)c[1], album[2] = (float)c[2];
            canvas_blur_cover(&artist, img, w, h, 20, 0.9f);   /* filter: blur(20px) brightness(90%), stretched */
            canvas_draw_image(&artist, img, w, h, 0, 0, w, h, 170, 30, 300, 300, 1);
            free(img);
            set_light_color(album[0] / 255, album[1] / 255, album[2] / 255);
            grad_set(album[0], album[1], album[2]);
        }
    }
    free(thumb);
    canvas_upload(&artist);
    dirty = true;
}

static void on_audio(const float *bands) {
    if (signal_lost) return;
    double bass = 0;
    for (int i = 0; i <= 40; i++) bass += bands[i] * 2;
    bass /= 40 * 2 * 0.1;
    double ai = floor(bass * 0.5 * 5) * 10 / 100;
    if (ai > 1) ai = 1;
    if (ai < 0) ai = 0;
    if (50 * ai != music_intensity) music_intensity = 50 * ai, dirty = true;
}

static void on_property(const SpMessage *m) {
    const char *n = m->name;
    if (!strcmp(n, "imgSelect")) {
        snprintf(cfg.photo, sizeof cfg.photo, "%s", m->value);
        load_photo(cfg.photo);
    } else if (!strcmp(n, "xAngleCamera")) cfg.x_angle = sp_num(m);
    else if (!strcmp(n, "yAngleCamera")) cfg.y_angle = sp_num(m);
    else if (!strcmp(n, "zoomCamera")) cfg.zoom = sp_num(m);
    else if (!strcmp(n, "30fps")) cfg.cap24 = sp_bool(m);
    dirty = true;
}

/* the camera: an OrthographicCamera (frustum 20, near .25, far 1000) in a container turned x then y;
 * OrbitControls keeps it 18 from its target (0, 0, -0.1) and turns it to look at that point in world space */
static TlCamera make_camera(int w, int h) {
    TlCamera c;
    double aspect = (double)w / h, fs = 20, zoom = cfg.zoom;
    double left = -aspect * fs / 2, right = aspect * fs / 2, top = fs / 2, bottom = -fs / 2;
    double dx = (right - left) / (2 * zoom), dy = (top - bottom) / (2 * zoom), cx = (right + left) / 2, cy = (top + bottom) / 2;
    c.projection = tl_m4_ortho(cx - dx, cx + dx, cy + dy, cy - dy, 0.25, 1000);
    double q[4], one[3] = { 1, 1, 1 }, zero[3] = { 0, 0, 0 };
    tl_quat_euler(TL_PI / 180 * cfg.x_angle, TL_PI / 180 * cfg.y_angle, 0, q);
    TlM4 parent = tl_m4_compose(zero, q, one);
    double local[3] = { 0, 18 * cos(TL_PI / 2), -0.1 + 18 }, world_pos[3], target[3] = { 0, 0, -0.1 };
    tl_m4_point(&parent, local, world_pos);
    TlM4 rot = tl_m4_lookat(world_pos, target, (double[3]){ 0, 1, 0 });
    rot.e[12] = world_pos[0], rot.e[13] = world_pos[1], rot.e[14] = world_pos[2];
    c.world = rot;
    c.view = tl_m4_invert(&rot);
    c.ortho = true;
    return c;
}

int main(int argc, char **argv) {
    SpArgs sp = sp_args(argc, argv);
    if (!bare_window_with(.width = sp.width ? sp.width : 1280, .height = sp.height ? sp.height : 720, .title = "Living Room",
                          .parent = sp.parent))
        return 1;
    bare_background(BARE_BLACK);
    sp_want(SP_WANT_AUDIO | SP_WANT_TRACK);

    size_t glb_size;
    unsigned char *glb = read_file(sp_asset("Room.glb"), &glb_size);
    if (!glb || !tl_load_glb(&scene, glb, glb_size)) {
        fprintf(stderr, "livingroom: can't read Room.glb\n");
        return 1;
    }
    free(glb);
    digib = bare_font_load(sp_asset("DS-DIGIB.TTF"));
    static const char *const fonts[] = {
        "C:/Windows/Fonts/arial.ttf", "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    for (size_t i = 0; i < sizeof fonts / sizeof *fonts && !arial; i++) {
        FILE *f = fopen(fonts[i], "rb");
        if (f) fclose(f), arial = bare_font_load(fonts[i]);
    }

    /* the page's model adjustments */
    TlObject *room = tl_find(&scene, "Room");
    if (room) room->cast_shadow = true;
    const char *baked[] = { "Room", "Tv_Cabinet", "Sofa", "Cube003", "Book4009", "Cube001", "1001", "1" };
    for (int i = 0; i < 8; i++) {   /* meshBasicToStandardMesh: a new MeshStandardMaterial with only the map */
        TlObject *o = tl_find(&scene, baked[i]);
        if (!o) continue;
        BareImage map = o->mat.map;
        tl_material_default(&o->mat, TL_STANDARD);
        o->mat.map = map;
    }
    TlObject *wall = tl_find(&scene, "Wall_Frame");
    if (wall) wall->mat.color[0] = wall->mat.color[1] = wall->mat.color[2] = 8 / 255.f;
    soundbar = tl_find(&scene, "SOUNDBAR_LED");
    if (soundbar) {
        tl_material_default(&soundbar->mat, TL_STANDARD);
        soundbar->mat.color[0] = soundbar->mat.color[1] = soundbar->mat.color[2] = 45 / 255.f;
    }
    TlObject *dvd = tl_find(&scene, "Cube001");
    if (dvd) tl_uv_transform(-0.03, 0, 1, 1, 0, 0, 0, dvd->mat.uv);

    canvas_init(&artist, 640, 360, true, false);
    canvas_init(&clock_c, 300, 300, false, false);
    canvas_init(&photo, 360, 640, true, true);
    canvas_upload(&artist);
    canvas_upload(&clock_c);
    canvas_upload(&photo);
    TlObject *screen = tl_find(&scene, "TV_SCREEN"), *clock_o = tl_find(&scene, "CLOCK_SCREEN"), *pic = tl_find(&scene, "PICTURE_FRAME");
    if (screen) {
        tl_material_default(&screen->mat, TL_BASIC);
        screen->mat.map = artist.tex;
    }
    if (clock_o) {
        tl_material_default(&clock_o->mat, TL_BASIC);
        clock_o->mat.map = clock_c.tex;
    }
    if (pic) {
        tl_material_default(&pic->mat, TL_BASIC);
        pic->mat.map = photo.tex;
        tl_uv_transform(-0.003, 0, 1, 1, TL_PI / 180 * -90, 0, 0, pic->mat.uv);
    }
    /* gltf.scene sits at y -2.5: its roots move */
    for (int i = 0; i < scene.n; i++)
        if (scene.obj[i].parent < 0) scene.obj[i].pos[1] -= 2.5;
    /* the no-signal video: a copy of the screen 0.002 in front (world position and rotation, its own scale),
     * nearest-filtered, linear (a VideoTexture isn't sRGB); 24 frames of 300 x 300 stacked */
    int vw = 0, vh = 0, video_i = -1, screen_i = screen ? (int)(screen - scene.obj) : -1;
    {
        size_t n;
        unsigned char *b = read_file(sp_asset("no_signal.png"), &n);
        unsigned char *px = b ? bare_image_decode(b, (int)n, &vw, &vh) : NULL;
        free(b);
        if (screen_i >= 0 && px) {
            TlObject *v = tl_add(&scene);
            video_i = (int)(v - scene.obj);
            TlObject *src = &scene.obj[screen_i];
            int order = v->order;
            *v = *src;
            v->order = order;
            v->pos[2] += 0.002;
            tl_material_default(&v->mat, TL_BASIC);
            v->mat.map = bare_texture(.width = vw, .height = vh, .rgba8 = px, .nearest = true);
            v->visible = false;
            snprintf(v->name, sizeof v->name, "no_signal");
        }
        free(px);
    }
    /* the floor that only shows the shadow: PlaneGeometry(25, 25) at y -2.5, turned -90 degrees about x */
    {
        TlObject *plane = tl_add(&scene);
        static float quad[6 * 8];
        const float c[4][2] = { { -12.5f, 12.5f }, { 12.5f, 12.5f }, { -12.5f, -12.5f }, { 12.5f, -12.5f } };
        const int idx[6] = { 0, 2, 1, 2, 3, 1 };
        for (int k = 0; k < 6; k++) {
            float *d = quad + k * 8;
            d[0] = c[idx[k]][0], d[1] = c[idx[k]][1], d[2] = 0;
            d[3] = idx[k] & 1 ? 1.f : 0.f, d[7] = idx[k] < 2 ? 1.f : 0.f;
            d[4] = 0, d[5] = 0, d[6] = 1;
        }
        plane->verts = quad, plane->count = 6;
        plane->pos[1] = -2.5;
        tl_quat_euler(TL_PI / 180 * -90, 0, 0, plane->quat);
        tl_material_default(&plane->mat, TL_SHADOW);
        plane->mat.color[0] = plane->mat.color[1] = plane->mat.color[2] = 0;
        plane->mat.transparent = true;
        plane->receive_shadow = true;
        snprintf(plane->name, sizeof plane->name, "floor");
    }
    tl_update(&scene);
    video_screen = video_i >= 0 ? &scene.obj[video_i] : NULL;
    soundbar = tl_find(&scene, "SOUNDBAR_LED");

    /* lights */
    lights.ambient[0] = lights.ambient[1] = lights.ambient[2] = 1;
    lights.npoints = 2;
    lights.point[0] = (TlPointLight){ { 0, 1.25, -3.75 }, { 45 / 255.f, 45 / 255.f, 45 / 255.f }, 50, 5, 2 };
    lights.point[1] = (TlPointLight){ { -3.5, 2, 1.5 }, { 0xce / 255.f, 0xcc / 255.f, 0xa7 / 255.f }, 4, 8, 2 };
    lights.spot = (TlSpotLight){ .on = true, .cast_shadow = true, .pos = { 0, 75, 0 }, .target = { 0, 0, 0 },
                                 .color = { 1, 1, 1 }, .intensity = 0.1f, .distance = 0, .angle = (float)(TL_PI / 3),
                                 .penumbra = 0, .decay = 2, .bias = 0, .map_size = 512 };

    TlRenderer r = { .shader = bare_shader(&three_shader), .exposure = 1, .tone_mapping = true };
    r.shadow_size = 512;
    r.shadow_map = bare_canvas(.width = 512, .height = 512, .format = BARE_RGBA32F, .bottom_up = true, .nearest = true);
    r.shadow_depth = bare_canvas(.width = 512, .height = 512, .format = BARE_DEPTH);
    r.flip_facing = getenv("TL_NOFLIP") == NULL;   /* Bare's bottom_up canvases mirror y, which turns the winding round */
    BareShader page = bare_shader(&three_page_shader);
    int ss = getenv("TL_SS") ? atoi(getenv("TL_SS")) : 1;
    if (ss < 1) ss = 1;

    bool fixed = getenv("BARE_FIXED_DT") != NULL, paused = false;
    long frame = 0;
    int cw = 0, ch = 0;
    BareImage target = 0, depth = 0, out = 0;
    double start = -1, last_clock_min = -1, last_draw = -1;
    load_photo(cfg.photo);

    while (bare_frame()) {
        SpMessage m;
        while (sp_next(&m)) {
            if (m.kind == SP_PROPERTY) on_property(&m);
            else if (m.kind == SP_PAUSE) paused = sp_bool(&m);
            else if (m.kind == SP_TRACK) on_track(m.text);
            else if (m.kind == SP_AUDIO) on_audio(m.bands);
        }
        BareVec2 win = bare_window_size();
        int w = (int)fmax(1, floor(win.x + 0.5)), h = (int)fmax(1, floor(win.y + 0.5));
        if (w != cw || h != ch) {
            cw = w, ch = h;
            if (target) bare_image_free(target), bare_image_free(depth), bare_image_free(out);
            target = bare_canvas(.width = w * ss, .height = h * ss, .bottom_up = true);
            depth = bare_canvas(.width = w * ss, .height = h * ss, .format = BARE_DEPTH);
            out = bare_canvas(.width = w, .height = h, .bottom_up = true);
            dirty = true;
        }
        bare_box(.width = BARE_GROW, .height = BARE_GROW, .image = out);
        if (paused) { bare_continuous(false); continue; }
        double now = fixed ? frame / 60.0 : bare_time();
        frame++;
        if (start < 0) start = now;
        now_s = now;
        double date = fixed ? DATE0 + now * 1000 : (double)time(NULL) * 1000;
        /* setInterval(updateClock, 1000): the first draw a second in */
        double minute = floor(date / 60000);
        if (now - start >= 1 && minute != last_clock_min) last_clock_min = minute, draw_clock(date);
        /* #container's fade in: opacity 0 to 1 over 1 s, ease */
        double ft = fmin(1, (now - start) / 1.0);
        double opacity;
        {   /* cubic-bezier(.25, .1, .25, 1) */
            double lo = 0, hi = 1, x = ft;
            for (int i = 0; i < 40; i++) {
                double t = (lo + hi) / 2, bx = 3 * (1 - t) * (1 - t) * t * 0.25 + 3 * (1 - t) * t * t * 0.25 + t * t * t;
                if (bx < x) lo = t; else hi = t;
            }
            double t = (lo + hi) / 2;
            opacity = 3 * (1 - t) * (1 - t) * t * 0.1 + 3 * (1 - t) * t * t * 1 + t * t * t;
        }
        bool fading = ft < 1;
        double interval = cfg.cap24 ? 1 / 24.0 : 1 / 60.0;
        bool grading = grad.t0 >= 0 && now - grad.t0 < 3;
        bool animating = fading || grading || (signal_lost && video_screen);
        if (!dirty && !animating) {
            bare_continuous(false);
            bare_wake_in((float)fmax(0.05, 60 - fmod(date / 1000, 60)));
            continue;
        }
        if (!fixed && last_draw >= 0 && now - last_draw < interval - 0.002) {
            bare_wake_in((float)(last_draw + interval - 0.002 - now));
            continue;
        }
        last_draw = now;
        dirty = false;

        if (video_screen) {
            video_screen->visible = signal_lost;
            int frames = vh / vw > 0 ? vh / vw : 1;
            int f = (int)floor(fmod(now - start, frames / 24.0) * 24) % frames;
            tl_uv_transform(0, (double)f / frames, 1, 1.0 / frames, 0, 0, 0, video_screen->mat.uv);
        }
        lights.point[0].intensity = (float)music_intensity;
        TlCamera cam = make_camera(w, h);
        float clear[4] = { 0, 0, 0, 0 };
        tl_render(&scene, &r, &cam, &lights, target, depth, clear);
        float g[3];
        grad_now(g);
        struct { float misc[4], inner[4], outer[4]; } pp = {
            { (float)opacity, (float)ss, (float)w, (float)h }, { g[0] / 255, g[1] / 255, g[2] / 255, 1 }, { 0, 0, 0, 1 } };
        bare_draw(.target = out, .shader = page, .inputs = { target }, .vertex_count = 6, BARE_PARAMS(pp));
        if (animating) bare_continuous(true);
        else bare_continuous(false);
        if (!fixed) bare_wake_in((float)interval);
    }
    bare_quit();
    return 0;
}
