/* tunnel.c: an endless tunnel lined with a picture; Screen+'s "Music Tunnel" wallpaper.
 *
 * A port of rocksdanister's Music Tunnel (MIT, see LICENSE.txt): one fragment
 * pass maps the screen to a square (or round) tunnel and reads the picture by
 * depth and angle, then the page's CSS goes over it: blur(5px) and scale(1.09).
 * While a song plays (Screen+'s now-playing data, the page's
 * livelyCurrentTrack) the tunnel is lined with its album art, the centre fades
 * to the art's main colour and the art, title and artist sit in the middle.
 *
 * WebGL sampled the picture with mirrored repeat and trilinear mipmaps; Bare
 * pictures have one level, so the levels are made here (2x2 averages, as
 * glGenerateMipmap) and laid side by side in one image, and tunnel.glsl picks
 * and blends them as the GPU would.
 *
 * A Screen+ app wallpaper: settings are the Customise panel (LivelyProperties.json
 * beside the exe), sent by Screen+ on stdin (see ../screenplus/screenplus_wallpaper.h);
 * by hand they're arguments: tunnel isSquare=false speed=0.3 blurIntensity=0
 * While Screen+ has it paused it draws nothing. */
#include "bare.h"
#include "tunnel_shaders.h"
#include "../screenplus/screenplus_wallpaper.h"
#include "../screenplus/mips.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RESET_AFTER 21600.0   /* the page restarts its clock every 6 hours */

/* ---- settings ---------------------------------------------------------------- */

static struct {
    bool square, fps_lock;
    double speed, color_radius, blur, parallax, scaling;
} cfg = { true, false, 0.1, 1, 5, 0, 1 };   /* the page's own until Screen+ sends the panel's */

static void set_property(const SpMessage *m) {
    const char *n = m->name;
    if (!strcmp(n, "isSquare")) cfg.square = sp_bool(m);
    else if (!strcmp(n, "speed")) cfg.speed = sp_num(m);
    else if (!strcmp(n, "colorRadius")) cfg.color_radius = sp_num(m);
    else if (!strcmp(n, "blurIntensity")) cfg.blur = sp_num(m);
    else if (!strcmp(n, "parallaxIntensity")) cfg.parallax = sp_num(m);
    else if (!strcmp(n, "displayScaling")) cfg.scaling = fmin(1, fmax(0.1, sp_num(m)));
    else if (!strcmp(n, "fpsLock")) cfg.fps_lock = sp_bool(m);
}

/* ---- the page's colour pick (Color Thief's median cut, then the most contrast) ---- */

typedef struct Box { int r0, r1, g0, g1, b0, b1, count; } Box;
#define QBITS 5
#define QSIZE (1 << QBITS)

static int *histo;

static int box_count(Box *b) {
    int n = 0;
    for (int r = b->r0; r <= b->r1; r++)
        for (int g = b->g0; g <= b->g1; g++)
            for (int bl = b->b0; bl <= b->b1; bl++) n += histo[(r << (2 * QBITS)) | (g << QBITS) | bl];
    return b->count = n;
}

static void box_avg(const Box *b, int out[3]) {
    double s[3] = { 0, 0, 0 }, n = 0;
    for (int r = b->r0; r <= b->r1; r++)
        for (int g = b->g0; g <= b->g1; g++)
            for (int bl = b->b0; bl <= b->b1; bl++) {
                int h = histo[(r << (2 * QBITS)) | (g << QBITS) | bl];
                s[0] += h * (r + 0.5), s[1] += h * (g + 0.5), s[2] += h * (bl + 0.5), n += h;
            }
    for (int i = 0; i < 3; i++) out[i] = n ? (int)(s[i] / n * (256 / QSIZE)) : 0;
}

static bool box_split(Box *b, Box *out) {
    int dr = b->r1 - b->r0, dg = b->g1 - b->g0, db = b->b1 - b->b0;
    if (b->count < 2 || (dr == 0 && dg == 0 && db == 0)) return false;
    int axis = dr >= dg && dr >= db ? 0 : dg >= db ? 1 : 2;
    int lo = axis == 0 ? b->r0 : axis == 1 ? b->g0 : b->b0, hi = axis == 0 ? b->r1 : axis == 1 ? b->g1 : b->b1;
    int half = b->count / 2, acc = 0, cut = lo;
    for (int v = lo; v < hi; v++) {
        Box s = *b;
        if (axis == 0) s.r0 = s.r1 = v; else if (axis == 1) s.g0 = s.g1 = v; else s.b0 = s.b1 = v;
        acc += box_count(&s);
        cut = v;
        if (acc >= half) break;
    }
    *out = *b;
    if (axis == 0) b->r1 = cut, out->r0 = cut + 1;
    else if (axis == 1) b->g1 = cut, out->g0 = cut + 1;
    else b->b1 = cut, out->b0 = cut + 1;
    box_count(b), box_count(out);
    return true;
}

static double luminance(const int c[3]) {
    double a[3];
    for (int i = 0; i < 3; i++) {
        double v = c[i] / 255.0;
        a[i] = v <= 0.03928 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
    }
    return a[0] * 0.2126 + a[1] * 0.7152 + a[2] * 0.0722;
}

/* main: the colour of the text (most contrast with the dominant colour); shadow: the dominant colour. */
static void pick_colors(const unsigned char *rgba, int w, int h, int main_c[3], int shadow[3]) {
    histo = calloc(QSIZE * QSIZE * QSIZE, sizeof *histo);
    if (!histo) return;
    for (int i = 0; i < w * h; i += 10) {   /* Color Thief's quality 10: every 10th pixel */
        const unsigned char *p = rgba + (size_t)i * 4;
        if (p[3] < 125 || (p[0] > 250 && p[1] > 250 && p[2] > 250)) continue;
        histo[((p[0] >> (8 - QBITS)) << (2 * QBITS)) | ((p[1] >> (8 - QBITS)) << QBITS) | (p[2] >> (8 - QBITS))]++;
    }
    Box boxes[8];
    int n = 1;
    boxes[0] = (Box){ 0, QSIZE - 1, 0, QSIZE - 1, 0, QSIZE - 1, 0 };
    box_count(&boxes[0]);
    while (n < 6) {   /* getPalette(img, 6) */
        int best = -1;
        for (int i = 0; i < n; i++) if (best < 0 || boxes[i].count > boxes[best].count) best = i;
        if (best < 0 || !box_split(&boxes[best], &boxes[n])) break;
        n++;
    }
    int pal[8][3], np = 0;
    for (int i = 0; i < n; i++) if (boxes[i].count) box_avg(&boxes[i], pal[np++]);
    free(histo);
    if (!np) return;
    int dom = 0;   /* getColor: the biggest box */
    for (int i = 1; i < np; i++) if (boxes[i].count > boxes[dom].count) dom = i;
    memcpy(shadow, pal[dom], sizeof pal[dom]);
    double ld = luminance(shadow), best = -999;
    memcpy(main_c, pal[0], sizeof pal[0]);
    for (int i = 0; i < np; i++) {
        double l = luminance(pal[i]), c = (fmax(l, ld) + 0.05) / (fmin(l, ld) + 0.05);
        if (c > best) best = c, memcpy(main_c, pal[i], sizeof pal[i]);
    }
}

/* ---- the tunnel's two pictures and the cross-fade between them ------------------ */

typedef struct Params {
    float res[4], opts[4], center_color[4], info[4], info1[4];
    float lv0[MAX_LEVELS][4], lv1[MAX_LEVELS][4];
} Params;

static Mips tex[2];             /* u_tex0, u_tex1 */
static bool tex_on[2];
static bool use0;               /* the page's isTexture0: which slot the next picture goes into */
static double blend, blend_from, blend_to, blend_start = -1;
static unsigned char *bg_pixels;
static int bg_w, bg_h;

static void set_texture(const unsigned char *rgba, int w, int h, double now) {
    int slot = use0 ? 0 : 1;
    free_mips(&tex[slot]);
    tex[slot] = make_mips(rgba, w, h);
    tex_on[slot] = tex[slot].atlas != 0;
    blend_from = use0 ? 1 : 0, blend_to = use0 ? 0 : 1, blend_start = now;
    use0 = !use0;
}

/* ---- the song --------------------------------------------------------------------- */

static struct {
    bool playing;
    char title[256], artist[256];
    BareImage art;
    int main_c[3], shadow[3];
    double shown, shown_from, shown_at;   /* the container's opacity, on its 1 s ease */
} track = { .shadow = { 255, 255, 255 } };

static double css_ease(double t) {   /* cubic-bezier(0.25, 0.1, 0.25, 1) */
    double lo = 0, hi = 1;
    for (int i = 0; i < 30; i++) {
        double m = (lo + hi) / 2, x = 3 * (1 - m) * (1 - m) * m * 0.25 + 3 * (1 - m) * m * m * 0.25 + m * m * m;
        if (x < t) lo = m; else hi = m;
    }
    double m = (lo + hi) / 2;
    return 3 * (1 - m) * (1 - m) * m * 0.1 + 3 * (1 - m) * m * m * 1.0 + m * m * m;
}

static double track_opacity(double now) {
    double t = fmin(1, (now - track.shown_at) / 1.0), e = css_ease(t), to = track.playing ? 1 : 0;
    return track.shown_from + (to - track.shown_from) * e;
}

static void on_track(const char *json, double now) {
    bool on = json && strncmp(json, "null", 4) != 0;
    if (!on) {
        if (bg_pixels) set_texture(bg_pixels, bg_w, bg_h, now);
        track.shown_from = track_opacity(now), track.shown_at = now, track.playing = false;
        return;
    }
    sp_json(json, "Title", track.title, sizeof track.title);
    sp_json(json, "Artist", track.artist, sizeof track.artist);
    char *thumb = sp_json_alloc(json, "Thumbnail");
    int size = 0, w = 0, h = 0;
    unsigned char *bytes = thumb ? sp_base64(thumb, &size) : NULL;
    unsigned char *rgba = bytes ? bare_image_decode(bytes, size, &w, &h) : NULL;
    if (track.art) bare_image_free(track.art);
    track.art = 0;
    if (rgba) {
        set_texture(rgba, w, h, now);
        track.art = bare_image_from_pixels(w, h, rgba);
        pick_colors(rgba, w, h, track.main_c, track.shadow);
    } else if (bg_pixels) {
        set_texture(bg_pixels, bg_w, bg_h, now);
        track.art = bare_image_from_pixels(bg_w, bg_h, bg_pixels);
        pick_colors(bg_pixels, bg_w, bg_h, track.main_c, track.shadow);
    }
    free(rgba), free(bytes), free(thumb);
    track.shown_from = track_opacity(now), track.shown_at = now, track.playing = true;
}

static BareColor rgb3(const int c[3], double a) {
    return BARE_RGBA(c[0], c[1], c[2], (int)lround(a * 255));
}

int main(int argc, char **argv) {
    SpArgs sp = sp_args(argc, argv);
    if (!bare_window_with(.width = sp.width ? sp.width : 1280, .height = sp.height ? sp.height : 720, .title = "Music Tunnel", .parent = sp.parent)) return 1;
    sp_want(SP_WANT_TRACK);
    bare_background(BARE_BLACK);
    BareShader tunnel = bare_shader(&tunnel_shader), blur = bare_shader(&blur_shader);
    BareFont montserrat = bare_font_load(sp_asset("Montserrat-Regular.otf"));
    {
        FILE *f = fopen(sp_asset("background.jpg"), "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            unsigned char *bytes = n > 0 ? malloc((size_t)n) : NULL;
            if (bytes && fread(bytes, 1, (size_t)n, f) == (size_t)n) bg_pixels = bare_image_decode(bytes, (int)n, &bg_w, &bg_h);
            free(bytes);
            fclose(f);
        }
    }
    BareImage scene = 0, half = 0, blurred = 0;
    int cw = 0, ch = 0;
    bool paused = false, started = false, moved = false;
    double clock = 0, last = -1, last_draw = -1e9;
    BareVec2 last_mouse = { -1e9f, -1e9f }, offset = { 0, 0 };

    while (bare_frame()) {
        double now = bare_time();
        SpMessage m;
        while (sp_next(&m)) {
            if (m.kind == SP_PROPERTY) set_property(&m);
            else if (m.kind == SP_PAUSE) paused = sp_bool(&m);
            else if (m.kind == SP_TRACK) on_track(m.text, now);
        }
        if (!started) {   /* the page's first picture fades in from nothing over half a second */
            if (bg_pixels) set_texture(bg_pixels, bg_w, bg_h, now);
            started = true;
        }
        BareVec2 win = bare_window_size();
        int w = (int)fmax(1, floor(win.x * cfg.scaling)), h = (int)fmax(1, floor(win.y * cfg.scaling));
        if (w != cw || h != ch) {
            if (scene) bare_image_free(scene), bare_image_free(half), bare_image_free(blurred);
            scene = bare_canvas(.width = w, .height = h, .bottom_up = true);
            half = bare_canvas(.width = w, .height = h, .bottom_up = true);
            blurred = bare_canvas(.width = w, .height = h, .bottom_up = true);
            cw = w, ch = h, last_draw = -1e9;
        }

        /* parallax: the page moves its container once the mouse moves */
        BareVec2 mo = bare_mouse();
        if (mo.x != last_mouse.x || mo.y != last_mouse.y) {
            if (last_mouse.x > -1e8f) moved = true;
            last_mouse = mo;
        }
        if (moved && cfg.parallax != 0)
            offset = (BareVec2){ (float)((win.x - mo.x * cfg.parallax) / 90), (float)((win.y - mo.y * cfg.parallax) / 90) };

        bare_stack(.width = BARE_GROW, .height = BARE_GROW, .color = BARE_BLACK, .clip = true) {
            bare_box(.position = BARE_ABSOLUTE, .width = win.x, .height = win.y, .image = blurred, .scale = 1.09f,
                     .offset = offset);
            double op = track_opacity(now);
            if (op > 0.001) {
                float side = win.x * 0.15f;
                bare_column(.position = BARE_ABSOLUTE, .anchor = BARE_MIDDLE, .align = BARE_CENTER, .gap = 5, .opacity = (float)op) {
                    bare_box(.width = side, .height = side, .image = track.art, .radius = side * 0.025f,
                             .border = 1, .border_color = BARE_RGBA(0, 0, 0, 128),
                             .shadow = { .x = 3, .y = 3, .blur = 18, .color = rgb3(track.shadow, 1) });
                    BareColor tc = rgb3(track.main_c, 1);
                    bare_text(track.title, .font = montserrat, .size = 32, .weight = BARE_BOLD, .color = tc,
                              .align = BARE_CENTER, .shadow = { .x = 1, .y = 0, .blur = 10, .color = rgb3(track.shadow, 1) },
                              );
                    bare_text(track.artist, .font = montserrat, .size = 24, .weight = BARE_BOLD, .color = tc,
                              .opacity = 0.75f, .align = BARE_CENTER,
                              .shadow = { .x = 1, .y = 0, .blur = 10, .color = rgb3(track.shadow, 1) });
                }
            }
        }

        if (paused) { bare_continuous(false); last = -1; continue; }
        double fps = cfg.fps_lock ? 30 : 60, interval = 1 / fps;
        if (now - last_draw < interval - 0.002 && !(blend_start >= 0)) {
            bare_wake_in((float)(last_draw + interval - 0.002 - now));
            continue;
        }
        last_draw = now;
        if (last >= 0) clock += now - last;
        last = now;
        if (clock > RESET_AFTER) clock = 0;

        if (blend_start >= 0) {   /* the page's showTransition: linear over 500 ms */
            double p = fmin(1, (now - blend_start) / 0.5);
            blend = blend_from + (blend_to - blend_from) * p;
            if (p >= 1) {
                int old = blend_to > 0.5 ? 0 : 1;   /* the picture faded out is let go */
                free_mips(&tex[old]);
                tex_on[old] = false;
                blend_start = -1;
            }
        }

        Params p;
        memset(&p, 0, sizeof p);
        p.res[0] = (float)w, p.res[1] = (float)h, p.res[2] = (float)clock, p.res[3] = (float)cfg.speed;
        p.opts[0] = (float)blend, p.opts[1] = cfg.square ? 1.f : 0.f, p.opts[2] = track.playing ? 1.f : 0.f;
        p.opts[3] = (float)cfg.color_radius;
        for (int i = 0; i < 3; i++) p.center_color[i] = track.playing ? track.shadow[i] / 255.f : 0;
        p.info[0] = (float)(tex[0].levels ? tex[0].levels : 1), p.info[1] = (float)(tex[1].levels ? tex[1].levels : 1);
        p.info[2] = (float)(tex[0].aw ? tex[0].aw : 1), p.info[3] = (float)(tex[0].ah ? tex[0].ah : 1);
        p.info1[0] = (float)(tex[1].aw ? tex[1].aw : 1), p.info1[1] = (float)(tex[1].ah ? tex[1].ah : 1);
        p.info1[2] = tex_on[0] ? 1.f : 0.f, p.info1[3] = tex_on[1] ? 1.f : 0.f;
        memcpy(p.lv0, tex[0].lv, sizeof p.lv0);
        memcpy(p.lv1, tex[1].lv, sizeof p.lv1);
        BareImage in0 = tex_on[0] ? tex[0].atlas : tex[1].atlas, in1 = tex_on[1] ? tex[1].atlas : tex[0].atlas;
        if (!in0) {   /* no picture at all yet: black */
            bare_pass(.target = blurred, .shader = blur, .inputs = { scene });
            continue;
        }
        bare_pass(.target = scene, .shader = tunnel, .inputs = { in0, in1 }, BARE_PARAMS(p));
        float sigma = (float)(cfg.blur * cfg.scaling);
        float bx[4] = { 1.f / w, 0, sigma, 0 }, by[4] = { 0, 1.f / h, sigma, 0 };
        bare_pass(.target = half, .shader = blur, .inputs = { scene }, BARE_PARAMS(bx));
        bare_pass(.target = blurred, .shader = blur, .inputs = { half }, BARE_PARAMS(by));
        bare_continuous(false);
        bare_wake_in((float)interval - 0.002f);
    }
    bare_quit();
}
