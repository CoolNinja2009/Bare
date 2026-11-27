/* Music TV: Screen+'s "Music TV" sample (rocksdanister's retro TV, three.js
 * r147) in C. The TV is the page's screen_low.glb, lit by the page's studio
 * HDR (scene.environment, prefiltered as PMREMGenerator does: pmrem.h) and a
 * faint spot light from above whose PCF-soft shadow falls on an invisible
 * floor, drawn by examples/screenplus/three_lite.h through a perspective
 * camera in a tilted container. The screen shows the track (album art and
 * title, or the title alone) with the music's spectrum as bars over it, or
 * the "no signal" loop when nothing plays.
 *
 * Properties: xAngleCamera, yAngleCamera, zoomCamera, 30fps (else 60). The TV
 * is drawn only when something on it changes. */
#include "bare.h"
#include "../screenplus/screenplus_wallpaper.h"
#include "../screenplus/three_lite.h"
#include "../screenplus/colorthief.h"
#include "../screenplus/pmrem.h"
#include "generated/musictv_shaders.h"

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

/* ---- the page's 300 x 300 canvases ---- */

typedef struct Canvas { int w, h; unsigned char *px; BareImage tex; } Canvas;

static void canvas_init(Canvas *c, int w, int h) {
    c->w = w, c->h = h;
    c->px = calloc((size_t)w * h, 4);
}
static void canvas_upload(Canvas *c) {   /* CanvasTexture: Nearest both ways, no mipmaps, linear encoding */
    if (c->tex) bare_image_update(c->tex, c->px);
    else c->tex = bare_texture(.width = c->w, .height = c->h, .rgba8 = c->px, .nearest = true);
}
/* source-over of straight colour onto a straight-alpha pixel (the artist canvas uploads unpremultiplied) */
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
/* fillText, textAlign center, alphabetic baseline at base_y */
static void canvas_text(Canvas *c, const char *text, BareFont font, float size, float cx, float base_y, const float rgb[3]) {
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
/* drawImage(img, dx, dy, dw, dh): bilinear, box-filtered when shrinking */
static void canvas_draw_image(Canvas *c, const unsigned char *img, int iw, int ih, double dx, double dy, double dw, double dh) {
    int x0 = (int)fmax(0, floor(dx)), y0 = (int)fmax(0, floor(dy));
    int x1 = (int)fmin(c->w, ceil(dx + dw)), y1 = (int)fmin(c->h, ceil(dy + dh));
    double scx = iw / dw, scy = ih / dh;
    int nx = scx > 1 ? (int)ceil(scx) : 1, ny = scy > 1 ? (int)ceil(scy) : 1;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float acc[4] = { 0, 0, 0, 0 };
            for (int j = 0; j < ny; j++)
                for (int i = 0; i < nx; i++) {
                    double u = (x + (i + 0.5) / nx - dx) * scx, v = (y + (j + 0.5) / ny - dy) * scy;
                    float s[4];
                    sample_rgba(img, iw, ih, (float)u, (float)v, s);
                    for (int k = 0; k < 4; k++) acc[k] += s[k];
                }
            for (int k = 0; k < 4; k++) acc[k] /= (float)(nx * ny);
            blend_px(c->px + ((size_t)y * c->w + x) * 4, acc[0], acc[1], acc[2], acc[3]);
        }
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

static struct { double x_angle, y_angle, zoom; bool cap30; } cfg = { 10, 0, 0, true };
static TlScene scene;
static TlObject *video_screen;
static Canvas artist, viz;
static BareFont arial;
static bool signal_lost = false;
static float album[3] = { 225, 225, 225 };
static bool dirty = true;

/* livelyAudioListener: the spectrum as square bars, filled with a gradient from black (y 250) through the
 * album colour to white (y 150). Bars are axis-aligned rectangles, so coverage is their exact area in each
 * pixel. The canvas uploads premultiplied (premultiplyAlpha). */
static void draw_viz(const float *a, int n) {
    double maxv = 1;
    for (int i = 0; i < n; i++)
        if (a[i] > maxv) maxv = a[i];
    int W = viz.w, H = viz.h;
    double mh = H * 0.5, start = W * 0.1, vw = W * 0.8, mid = H - H / 6.0, off = vw / n;
    static float cov[300 * 300];
    memset(cov, 0, sizeof cov);
    for (int k = 0; k < n; k++) {
        double x0 = start + off * k, x1 = x0 + off, y0 = mid - a[k] / maxv * mh, y1 = mid;
        if (y1 - y0 <= 0) continue;
        for (int x = (int)fmax(0, floor(x0)); x < (int)fmin(W, ceil(x1)); x++) {
            double wx = fmin(x + 1, x1) - fmax(x, x0);
            if (wx <= 0) continue;
            for (int y = (int)fmax(0, floor(y0)); y < (int)fmin(H, ceil(y1)); y++) {
                double wy = fmin(y + 1, y1) - fmax(y, y0);
                if (wy > 0) cov[y * 300 + x] += (float)(wx * wy);
            }
        }
    }
    for (int y = 0; y < H; y++) {
        double t = (mid - (y + 0.5)) / (mid - mh);   /* createLinearGradient(0, midY, 0, max_height) */
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        float col[3];
        for (int q = 0; q < 3; q++)
            col[q] = (float)(t < 0.5 ? album[q] * (t / 0.5) : album[q] + (255 - album[q]) * ((t - 0.5) / 0.5));
        for (int x = 0; x < W; x++) {
            unsigned char *d = viz.px + ((size_t)y * W + x) * 4;
            float c = fminf(1, cov[y * 300 + x]);
            int al = (int)floorf(c * 255 + 0.5f);
            for (int q = 0; q < 3; q++) d[q] = (unsigned char)((int)floorf(col[q] + 0.5f) * al / 255);
            d[3] = (unsigned char)al;
        }
    }
    canvas_upload(&viz);
    dirty = true;
}

static void on_track(const char *json) {
    if (!json || !strcmp(json, "null") || !*json) {
        signal_lost = true;
        grad_set(45, 45, 45);
        dirty = true;
        return;
    }
    signal_lost = false;
    char title[512] = "";
    sp_json(json, "Title", title, sizeof title);
    char *thumb = sp_json_alloc(json, "Thumbnail");
    float size = strlen(title) > 20 ? 16.f : 25.f;
    if (!thumb || !*thumb || !strcmp(thumb, "null")) {
        memset(artist.px, 0, (size_t)artist.w * artist.h * 4);
        grad_set(45, 45, 45);
        const float c[3] = { 225 / 255.f, 225 / 255.f, 225 / 255.f };
        if (arial) canvas_text(&artist, title, arial, size, 150, 275, c);
        album[0] = album[1] = album[2] = 225;
    } else {
        int n = 0;
        unsigned char *bytes = sp_base64(thumb, &n);
        int w = 0, h = 0;
        unsigned char *img = bytes ? bare_image_decode(bytes, n, &w, &h) : NULL;
        free(bytes);
        if (img) {   /* albumImg.onload */
            memset(artist.px, 0, (size_t)artist.w * artist.h * 4);
            int c[3];
            if (colorthief_get(img, w, h, c)) album[0] = (float)c[0], album[1] = (float)c[1], album[2] = (float)c[2];
            const float rgb[3] = { album[0] / 255, album[1] / 255, album[2] / 255 };
            if (arial) canvas_text(&artist, title, arial, size, 150, 275, rgb);
            canvas_draw_image(&artist, img, w, h, 50, 50, 200, 200);
            free(img);
            grad_set(album[0], album[1], album[2]);
        }
    }
    free(thumb);
    canvas_upload(&artist);
    dirty = true;
}

static void on_audio(const float *bands, int n) {
    if (signal_lost) return;
    float lv[128];   /* Lively's 128 values: each of the 64 bands twice */
    if (n > 64) n = 64;
    for (int i = 0; i < 2 * n; i++) lv[i] = bands[i >> 1];
    draw_viz(lv, 2 * n);
}

static void on_property(const SpMessage *m) {
    const char *n = m->name;
    if (!strcmp(n, "xAngleCamera")) cfg.x_angle = sp_num(m);
    else if (!strcmp(n, "yAngleCamera")) cfg.y_angle = sp_num(m);
    else if (!strcmp(n, "zoomCamera")) cfg.zoom = sp_num(m);
    else if (!strcmp(n, "30fps")) cfg.cap30 = sp_bool(m);
    dirty = true;
}

/* the camera: PerspectiveCamera(52.5, aspect, .25, 1000) in a container turned x then y and moved zoomCamera
 * along z; OrbitControls keeps it 12.5 (minDistance) from its target (0, 0, -0.1), looking at it in world space */
static TlCamera make_camera(int w, int h) {
    TlCamera c;
    c.projection = tl_m4_perspective(52.5, (double)w / h, 0.25, 1000, 1);
    double q[4], one[3] = { 1, 1, 1 }, pos[3] = { 0, 0, cfg.zoom };
    tl_quat_euler(TL_PI / 180 * cfg.x_angle, TL_PI / 180 * cfg.y_angle, 0, q);
    TlM4 parent = tl_m4_compose(pos, q, one);
    double local[3] = { 0, 12.5 * cos(TL_PI / 2), -0.1 + 12.5 }, world_pos[3], target[3] = { 0, 0, -0.1 };
    tl_m4_point(&parent, local, world_pos);
    TlM4 rot = tl_m4_lookat(world_pos, target, (double[3]){ 0, 1, 0 });
    rot.e[12] = world_pos[0], rot.e[13] = world_pos[1], rot.e[14] = world_pos[2];
    c.world = rot;
    c.view = tl_m4_invert(&rot);
    c.ortho = false;
    return c;
}

static void quat_mul(const double a[4], const double b[4], double o[4]) {
    double x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    double y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    double z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    double w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x, o[1] = y, o[2] = z, o[3] = w;
}

/* screenArtist.clone(false) at the screen's world place, `dz` nearer in world z: kept under the screen's parent,
 * with the world offset taken into the parent's space */
static TlObject *screen_copy(int screen_i, double dz, const char *name) {
    TlObject *v = tl_add(&scene);
    TlObject *src = &scene.obj[screen_i];
    int order = v->order;
    *v = *src;
    v->order = order;
    if (v->parent >= 0) {
        TlM4 inv = tl_m4_invert(&scene.obj[v->parent].world);
        const double *e = inv.e;   /* the offset through the inverse's 3x3 (not normalized) */
        for (int k = 0; k < 3; k++) v->pos[k] += e[8 + k] * dz;
    } else v->pos[2] += dz;
    snprintf(v->name, sizeof v->name, "%s", name);
    return v;
}

int main(int argc, char **argv) {
    SpArgs sp = sp_args(argc, argv);
    if (!bare_window_with(.width = sp.width ? sp.width : 1280, .height = sp.height ? sp.height : 720, .title = "Music TV",
                          .parent = sp.parent))
        return 1;
    bare_background(BARE_BLACK);
    sp_want(SP_WANT_AUDIO | SP_WANT_TRACK);

    size_t glb_size;
    unsigned char *glb = read_file(sp_asset("screen_low.glb"), &glb_size);
    if (!glb || !tl_load_glb(&scene, glb, glb_size)) {
        fprintf(stderr, "musictv: can't read screen_low.glb\n");
        return 1;
    }
    free(glb);
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

    /* gltf.scene: scale .01, turned -30 degrees about y, at (0, -5, -6); its one root takes that on */
    for (int i = 0; i < scene.n; i++) {
        TlObject *o = &scene.obj[i];
        if (o->verts) o->cast_shadow = true;
        if (o->parent >= 0) continue;
        double qy[4], q[4];
        tl_quat_euler(0, TL_PI / 180 * -30, 0, qy);
        quat_mul(qy, o->quat, q);
        double p[3];
        TlM4 rs = tl_m4_compose((double[3]){ 0, 0, 0 }, qy, (double[3]){ 0.01, 0.01, 0.01 });
        tl_m4_point(&rs, o->pos, p);
        o->pos[0] = p[0], o->pos[1] = p[1] - 5, o->pos[2] = p[2] - 6;
        memcpy(o->quat, q, sizeof q);
        for (int k = 0; k < 3; k++) o->scale[k] *= 0.01;
    }
    tl_update(&scene);

    canvas_init(&artist, 300, 300);
    canvas_init(&viz, 300, 300);
    canvas_upload(&artist);
    canvas_upload(&viz);
    TlObject *screen = tl_find(&scene, "SCREEN");
    int screen_i = screen ? (int)(screen - scene.obj) : -1;
    if (screen_i >= 0) {
        TlObject *v = screen_copy(screen_i, 0.001, "visualizer");
        tl_material_default(&v->mat, TL_BASIC);
        v->mat.map = viz.tex;
        v->mat.transparent = true;
        screen = &scene.obj[screen_i];
        tl_material_default(&screen->mat, TL_BASIC);
        screen->mat.map = artist.tex;
    }
    /* the no-signal video: 24 frames of 300 x 300, nearest when shrunk (magFilter stays linear: the page sets
     * "maxFilter"), offset (0, -0.12), clamped */
    static BareImage frames[64];
    int nframes = 0;
    {
        size_t n;
        int vw = 0, vh = 0;
        unsigned char *b = read_file(sp_asset("no_signal.png"), &n);
        unsigned char *px = b ? bare_image_decode(b, (int)n, &vw, &vh) : NULL;
        free(b);
        if (px && vw > 0) {
            nframes = vh / vw;
            if (nframes > 64) nframes = 64;
            for (int f = 0; f < nframes; f++)
                frames[f] = bare_texture(.width = vw, .height = vw, .rgba8 = px + (size_t)f * vw * vw * 4,
                                         .nearest = getenv("TV_VIDEO_NEAREST") != NULL);
        }
        free(px);
        if (screen_i >= 0 && nframes) {
            TlObject *v = screen_copy(screen_i, 0.002, "no_signal");
            tl_material_default(&v->mat, TL_BASIC);
            v->mat.map = frames[0];
            tl_uv_transform(0, -0.12, 1, 1, 0, 0, 0, v->mat.uv);
            v->visible = false;
            video_screen = v;
        }
    }
    /* the floor that only shows the shadow: PlaneGeometry(100, 100) at (0, -5, -4), turned -90 degrees about x */
    {
        TlObject *plane = tl_add(&scene);
        static float quad[6 * 8];
        const float c[4][2] = { { -50, 50 }, { 50, 50 }, { -50, -50 }, { 50, -50 } };
        const int idx[6] = { 0, 2, 1, 2, 3, 1 };
        for (int k = 0; k < 6; k++) {
            float *d = quad + k * 8;
            d[0] = c[idx[k]][0], d[1] = c[idx[k]][1], d[2] = 0;
            d[3] = idx[k] & 1 ? 1.f : 0.f, d[7] = idx[k] < 2 ? 1.f : 0.f;
            d[4] = 0, d[5] = 0, d[6] = 1;
        }
        plane->verts = quad, plane->count = 6;
        plane->pos[1] = -5, plane->pos[2] = -4;
        tl_quat_euler(TL_PI / 180 * -90, 0, 0, plane->quat);
        tl_material_default(&plane->mat, TL_SHADOW);
        plane->mat.color[0] = plane->mat.color[1] = plane->mat.color[2] = 0;
        plane->mat.transparent = true;
        plane->receive_shadow = true;
        snprintf(plane->name, sizeof plane->name, "floor");
    }
    tl_update(&scene);
    video_screen = tl_find(&scene, "no_signal");

    TlLights lights;
    memset(&lights, 0, sizeof lights);
    lights.spot = (TlSpotLight){ .on = true, .cast_shadow = true, .pos = { -10, 75, 0 }, .target = { 0, 0, 0 },
                                 .color = { 1, 1, 1 }, .intensity = 0.1f, .distance = 0, .angle = (float)(TL_PI / 3),
                                 .penumbra = 0, .decay = 2, .bias = 0, .map_size = 512 };

    TlRenderer r = { .shader = bare_shader(&three_shader), .exposure = 0.8f, .tone_mapping = true };
    r.shadow_size = 512;
    r.shadow_map = bare_canvas(.width = 512, .height = 512, .format = BARE_RGBA32F, .bottom_up = true, .nearest = true);
    r.shadow_depth = bare_canvas(.width = 512, .height = 512, .format = BARE_DEPTH);
    r.flip_facing = getenv("TL_NOFLIP") == NULL;
    if (!getenv("TV_NOENV")) r.env_map = tl_pmrem_from_hdr(bare_shader(&pmrem_shader), sp_asset("colorful_studio_1k.hdr"), r.env);
    BareShader page = bare_shader(&three_page_shader);
    int ss = getenv("TL_SS") ? atoi(getenv("TL_SS")) : 1;
    if (ss < 1) ss = 1;

    bool fixed = getenv("BARE_FIXED_DT") != NULL, paused = false;
    long frame = 0;
    int cw = 0, ch = 0;
    BareImage target = 0, depth = 0, out = 0;
    double start = -1, last_draw = -1;

    while (bare_frame()) {
        SpMessage m;
        while (sp_next(&m)) {
            if (m.kind == SP_PROPERTY) on_property(&m);
            else if (m.kind == SP_PAUSE) paused = sp_bool(&m);
            else if (m.kind == SP_TRACK) on_track(m.text);
            else if (m.kind == SP_AUDIO) on_audio(m.bands, m.nbands);
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
        double interval = cfg.cap30 ? 1 / 30.0 : 1 / 60.0;
        bool grading = grad.t0 >= 0 && now - grad.t0 < 3;
        bool animating = grading || (signal_lost && video_screen);
        if (!dirty && !animating) { bare_continuous(false); continue; }
        if (!fixed && last_draw >= 0 && now - last_draw < interval - 0.002) {
            bare_wake_in((float)(last_draw + interval - 0.002 - now));
            continue;
        }
        last_draw = now;
        dirty = false;

        if (video_screen) {
            video_screen->visible = signal_lost;
            int f = nframes ? (int)floor(fmod(now - start, nframes / 24.0) * 24) % nframes : 0;
            video_screen->mat.map = frames[f];
        }
        TlCamera cam = make_camera(w, h);
        float clear[4] = { 0, 0, 0, 0 };
        tl_render(&scene, &r, &cam, &lights, target, depth, clear);
        float g[3];
        grad_now(g);
        struct { float misc[4], inner[4], outer[4]; } pp = {
            { 1, (float)ss, (float)w, (float)h }, { g[0] / 255, g[1] / 255, g[2] / 255, 1 }, { 0, 0, 0, 1 } };
        bare_draw(.target = out, .shader = page, .inputs = { target }, .vertex_count = 6, BARE_PARAMS(pp));
        bare_continuous(animating);
        if (!fixed) bare_wake_in((float)interval);
    }
    bare_quit();
    return 0;
}
