/* hill.c: a valley of drifting translucent hills under the time and date;
 * Screen+'s "The Hill" wallpaper.
 *
 * A port of the page (yoichi kobayashi's CodePen "GLSL Hills", MIT, see
 * LICENSE.txt, with rocksdanister's clock and settings): a 256 x 256 plane
 * lifted by noise into two ridges, seen from a camera low in the valley, every
 * triangle drawn translucent (fading with distance) in the plane's own order,
 * far rows first; over it the time (Roboto), the day (Dancing Script, turned
 * 4 degrees) and the date (Cinzel), laid out as the page's CSS lays them out:
 * Blink's line heights and baselines, and the block centred by a width the page
 * measures once, before its fonts have loaded (so in the fallback fonts).
 *
 * A Screen+ app wallpaper: settings are the Customise panel (LivelyProperties.json
 * beside the exe), sent by Screen+ on stdin (see ../screenplus/screenplus_wallpaper.h);
 * by hand they're arguments: hill _12hour=false hillColor=#ffaa00 hillOpacityFac=80
 * While Screen+ has it paused it draws nothing. */
#include "bare.h"
#include "hill_shaders.h"
#include "../screenplus/screenplus_wallpaper.h"
#include "../screenplus/canvas2d.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CELLS 256

/* ---- settings ---------------------------------------------------------------- */

static struct {
    float hill[3], bg[3], font[3];
    double opacity;
    bool show_time, show_date, h12, us_date;
} cfg = { { 1, 1, 1 }, { 14 / 255.f, 14 / 255.f, 14 / 255.f }, { 1, 1, 1 }, 1, true, true, true, true };

static bool hex_color(const char *s, float out[3]) {
    unsigned v;
    if (*s == '#') s++;
    if (strlen(s) != 6 || sscanf(s, "%x", &v) != 1) return false;
    out[0] = (v >> 16 & 255) / 255.f, out[1] = (v >> 8 & 255) / 255.f, out[2] = (v & 255) / 255.f;
    return true;
}

static void set_property(const SpMessage *m) {
    const char *n = m->name;
    if (!strcmp(n, "timeToggle")) cfg.show_time = sp_bool(m);
    else if (!strcmp(n, "dateToggle")) cfg.show_date = sp_bool(m);
    else if (!strcmp(n, "_12hour")) cfg.h12 = sp_bool(m);
    else if (!strcmp(n, "mmddyy")) cfg.us_date = !sp_bool(m);   /* the page's: on is day/month */
    else if (!strcmp(n, "fontColor")) hex_color(m->value, cfg.font);
    else if (!strcmp(n, "hillColor")) hex_color(m->value, cfg.hill);
    else if (!strcmp(n, "bgColor")) hex_color(m->value, cfg.bg);
    else if (!strcmp(n, "hillOpacityFac")) cfg.opacity = sp_num(m) / 100;
}

/* ---- the camera: three.js's PerspectiveCamera(45, aspect, 1, 10000) at (0, 16, 128)
 * looking at (0, 28, 0), column-major ------------------------------------------------- */

static void projection(float out[16], double aspect) {
    double zn = 1, zf = 10000, top = zn * tan(3.14159265358979323846 / 180 * 22.5), height = 2 * top;
    double width = aspect * height, left = -0.5 * width, right = left + width, bottom = top - height;
    memset(out, 0, 16 * sizeof *out);
    out[0] = (float)(2 * zn / (right - left));
    out[5] = (float)(2 * zn / (top - bottom));
    out[8] = (float)((right + left) / (right - left));
    out[9] = (float)((top + bottom) / (top - bottom));
    out[10] = (float)(-(zf + zn) / (zf - zn));
    out[11] = -1;
    out[14] = (float)(-2 * zf * zn / (zf - zn));
}

static void view(float out[16]) {
    double eye[3] = { 0, 16, 128 }, target[3] = { 0, 28, 0 }, z[3], x[3], y[3];
    for (int i = 0; i < 3; i++) z[i] = eye[i] - target[i];
    double l = sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    for (int i = 0; i < 3; i++) z[i] /= l;
    x[0] = z[2], x[1] = 0, x[2] = -z[0];   /* cross((0, 1, 0), z) */
    l = sqrt(x[0] * x[0] + x[2] * x[2]);
    for (int i = 0; i < 3; i++) x[i] /= l;
    y[0] = z[1] * x[2] - z[2] * x[1], y[1] = z[2] * x[0] - z[0] * x[2], y[2] = z[0] * x[1] - z[1] * x[0];
    /* the inverse of [x y z | eye]: rows x, y, z and -R^T eye */
    double *r[3] = { x, y, z };
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) out[col * 4 + row] = (float)r[row][col];
        out[12 + row] = (float)-(r[row][0] * eye[0] + r[row][1] * eye[1] + r[row][2] * eye[2]);
    }
    out[3] = out[7] = out[11] = 0, out[15] = 1;
}

typedef struct Params { float projection[16], model_view[16], misc[4], ucolor[4]; } Params;

/* ---- the text --------------------------------------------------------------------- */

static BareFont roboto, dancing, cinzel, fb_mono, fb_cursive, fb_serif;

static BareFont load_font(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return bare_font_load(path);
}

static const char *const DAYS[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* The page's three lines for this moment, as Intl formats them (spaces collapsed). */
static void texts(double ms, char clock[32], char day[16], char date[32]) {
    time_t s = (time_t)floor(ms / 1000);
    struct tm t;
#ifdef _WIN32
    localtime_s(&t, &s);
#else
    localtime_r(&s, &t);
#endif
    int h = cfg.h12 ? (t.tm_hour % 12 ? t.tm_hour % 12 : 12) : t.tm_hour;
    snprintf(clock, 32, "- %02d:%02d -", h, t.tm_min);
    snprintf(day, 16, "%s", DAYS[t.tm_wday]);
    if (cfg.us_date) snprintf(date, 32, "%s %02d %02d", MONTHS[t.tm_mon], t.tm_mday, t.tm_year % 100);
    else snprintf(date, 32, "%02d/%02d/%02d", t.tm_mday, t.tm_mon + 1, t.tm_year % 100);
}

/* Blink's LayoutUnit: 1/64 px. Widths of text round up to it, line heights down. */
static double lu_ceil(double v) { return ceil(v * 64 - 1e-6) / 64; }
static double lu_floor(double v) { return floor(v * 64 + 1e-6) / 64; }

typedef struct Line { BareFont font; float size, line_h, box_h; int ascent, descent; const char *text; } Line;

int main(int argc, char **argv) {
    SpArgs sp = sp_args(argc, argv);
    if (!bare_window_with(.width = sp.width ? sp.width : 1280, .height = sp.height ? sp.height : 720, .title = "The Hill", .parent = sp.parent)) return 1;
    bare_background(BARE_BLACK);
    BareShader hill = bare_shader(&hill_shader);
    C2d c;
    c2d_init(&c, bare_shader(&canvas2d_shader));
    roboto = load_font(sp_asset("Roboto-Regular.ttf"));
    dancing = load_font(sp_asset("DancingScript-Regular.otf"));
    cinzel = load_font(sp_asset("Cinzel-Regular.otf"));
    /* Chrome's monospace, cursive and serif on Windows */
    fb_mono = load_font("C:/Windows/Fonts/consola.ttf");
    fb_cursive = load_font("C:/Windows/Fonts/comic.ttf");
    fb_serif = load_font("C:/Windows/Fonts/times.ttf");

    float (*cells)[4] = malloc(sizeof(float[4]) * CELLS * CELLS);
    if (!cells) return 1;
    for (int iy = 0; iy < CELLS; iy++)
        for (int ix = 0; ix < CELLS; ix++) {
            float *p = cells[iy * CELLS + ix];
            p[0] = (float)ix, p[1] = (float)iy, p[2] = p[3] = 0;
        }

    BareImage canvas = 0;
    int cw = 0, ch = 0;
    bool paused = false;
    double clock = 0, last = -1, box_left = 0;
    bool placed = false;

    while (bare_frame()) {
        SpMessage m;
        while (sp_next(&m)) {
            if (m.kind == SP_PROPERTY) set_property(&m);
            else if (m.kind == SP_PAUSE) paused = sp_bool(&m);
        }
        double now = bare_time();
        BareVec2 win = bare_window_size();
        int w = (int)fmax(1, floor(win.x + 0.5)), h = (int)fmax(1, floor(win.y + 0.5));
        char t_clock[32], t_day[16], t_date[32];
        texts(sp_now_ms(), t_clock, t_day, t_date);
        if (w != cw || h != ch) {
            if (canvas) bare_image_free(canvas);
            canvas = bare_canvas(.width = w, .height = h);
            /* clockPos(): the block centred by its width; at first that is measured before the fonts
             * load (the fallbacks'), later on a resize with them */
            double bw = 0;
            BareFont f0 = placed ? roboto : fb_mono, f1 = placed ? dancing : fb_cursive, f2 = placed ? cinzel : fb_serif;
            if (cfg.show_time) bw = fmax(bw, c2d_measure(&c, f0 ? f0 : roboto, 48, t_clock));
            if (cfg.show_date) {
                bw = fmax(bw, c2d_measure(&c, f1 ? f1 : dancing, 80, t_day));
                bw = fmax(bw, c2d_measure(&c, f2 ? f2 : cinzel, 48, t_date));
            }
            box_left = w / 2.0 - floor(lu_ceil(bw) + 0.5) / 2;
            placed = true;
            cw = w, ch = h;
        }
        bare_box(.width = BARE_GROW, .height = BARE_GROW, .image = canvas);
        if (paused) { bare_continuous(false); last = -1; continue; }
        if (last >= 0) clock += now - last;
        last = now;

        Params p;
        projection(p.projection, (double)w / h);
        view(p.model_view);
        p.misc[0] = (float)clock, p.misc[1] = (float)cfg.opacity, p.misc[2] = p.misc[3] = 0;
        memcpy(p.ucolor, cfg.hill, sizeof cfg.hill), p.ucolor[3] = 1;
        bare_draw(.target = canvas, .shader = hill, .vertex_count = 6, BARE_INSTANCES(cells, CELLS * CELLS),
                  .blend = BARE_BLEND_ALPHA, BARE_PARAMS(p), .clear = true,
                  .clear_color = { cfg.bg[0], cfg.bg[1], cfg.bg[2], 1 });

        /* the text: lines one under another from 25% down, each centred in the block */
        Line lines[3] = {
            { roboto, 48, 0, 0, 46, 12, t_clock },     /* Roboto, Dancing Script and Cinzel's rounded */
            { dancing, 80, 24, 32, 74, 22, t_day },    /* Windows ascents and descents, as Blink takes them */
            { cinzel, 48, 0, 0, 48, 10, t_date },
        };
        bool shown[3] = { cfg.show_time, cfg.show_date, cfg.show_date };
        double widths[3], bw = 0;
        for (int i = 0; i < 3; i++) {
            widths[i] = lu_ceil(c2d_measure(&c, lines[i].font, lines[i].size, lines[i].text));
            if (shown[i]) bw = fmax(bw, widths[i]);
        }
        double y = floor(h * 0.25 * 64) / 64;
        C2dColor col = { cfg.font[0], cfg.font[1], cfg.font[2], 1 };
        for (int i = 0; i < 3; i++) {
            if (!shown[i]) continue;
            Line *l = &lines[i];
            double lh = l->line_h > 0 ? l->line_h : lu_floor(l->size * 1.15);   /* normalize.css: line-height 1.15 */
            double box_h = l->box_h > 0 ? l->box_h : lh;
            double half = floor((lh - (l->ascent + l->descent)) / 2);
            double baseline = y + l->ascent + half, x = box_left + lu_floor((bw - widths[i]) / 2);
            if (i == 1)   /* rotate(-4deg) about the middle of its box, at 70% */
                c2d_text_turned(&c, l->font, l->size, l->text, (float)x, (float)baseline, C2D_LEFT, 0,
                                (float)(-4 * 3.14159265358979 / 180), (float)(box_left + bw / 2), (float)(y + box_h / 2),
                                (C2dColor){ col.r, col.g, col.b, 0.7f });
            else c2d_text(&c, l->font, l->size, l->text, (float)x, (float)baseline, C2D_LEFT, 0, col);
            y += box_h;
        }
        c2d_flush(&c, canvas, w, h, false, NULL);
        bare_continuous(true);
    }
    free(cells);
    bare_quit();
}
