/* system.c: live CPU, GPU, memory and network charts over a motherboard;
 * Screen+'s "Simple System" wallpaper.
 *
 * A port of rocksdanister's page (MIT): four Chart.js 2.8 line charts with
 * chartjs-plugin-streaming 1.8, drawn here as Chart.js draws them (its layout,
 * "nice" ticks, time-scale grid, pixel alignment, colours and fonts), on the
 * page's background: a white to grey radial gradient under the picture. The
 * picture's graphics card is repainted as an RTX 5090-style card
 * (paint_gpu.py). The data is Screen+'s system information (the page's
 * livelySystemInformation): a new point every second, the charts scrolling
 * over the last 20 seconds at 30 frames a second.
 *
 * A Screen+ app wallpaper; it has no settings. By hand: system --system={"NameCpu":"...","CurrentCpu":40,...}
 * While Screen+ has it paused it draws nothing. */
#include "bare.h"
#include "system_shaders.h"
#include "../screenplus/screenplus_wallpaper.h"
#include "../screenplus/canvas2d.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DURATION 20000.0   /* realtime: duration, refresh, delay (ms) */
#define REFRESH 1000.0
#define DELAY 1000.0
#define MAX_POINTS 64
#define FONT 12.0f         /* Chart.js's default font size; 'Helvetica Neue', Helvetica, Arial: Arial here */
#define LINE_H 14.4        /* 1.2 x the font */

/* ---- the data ------------------------------------------------------------------- */

static struct {
    bool have;
    char cpu_name[160], gpu_name[160], net_name[160], mem_name[64];
    double cpu, gpu, down, up, ram_avail, ram_total;
} sys = { .ram_total = 1 };

typedef struct Series { double x[MAX_POINTS], y[MAX_POINTS]; int n; } Series;

typedef struct Chart {
    const char *title;
    double left, top;       /* the container: fractions of the window */
    bool aspect;            /* the network chart keeps Chart.js's 2:1 (no maintainAspectRatio: false) */
    bool suggested;         /* ticks.suggestedMax rather than max */
    int nsets;
    Series sets[2];
    C2dColor colors[2];
} Chart;

static Chart charts[4];   /* cpu, gpu, ram, net, as the page's containers */

static void js_number(double v, char *out, size_t cap) {   /* Number.prototype.toString for ordinary values */
    if (v == floor(v) && fabs(v) < 1e21) { snprintf(out, cap, "%.0f", v); return; }
    for (int p = 1; p <= 17; p++) {
        snprintf(out, cap, "%.*g", p, v);
        if (strtod(out, NULL) == v) return;
    }
}

static void on_system(const char *json) {
    char v[160];
    if (sp_json(json, "NameCpu", v, sizeof v)) snprintf(sys.cpu_name, sizeof sys.cpu_name, "%s", v);
    if (sp_json(json, "NameGpu", v, sizeof v)) snprintf(sys.gpu_name, sizeof sys.gpu_name, "%s", v);
    if (sp_json(json, "NameNetCard", v, sizeof v)) snprintf(sys.net_name, sizeof sys.net_name, "%s", v);
    if (sp_json(json, "CurrentCpu", v, sizeof v)) sys.cpu = atof(v);
    if (sp_json(json, "CurrentGpu3D", v, sizeof v)) sys.gpu = atof(v);
    if (sp_json(json, "CurrentNetDown", v, sizeof v)) sys.down = atof(v) * 8 / (1024 * 1024);
    if (sp_json(json, "CurrentNetUp", v, sizeof v)) sys.up = atof(v) * 8 / (1024 * 1024);
    if (sp_json(json, "CurrentRamAvail", v, sizeof v)) sys.ram_avail = atof(v);
    if (sp_json(json, "TotalRam", v, sizeof v) && atof(v) > 0) sys.ram_total = atof(v);
    char gb[48];
    js_number(sys.ram_total / 1024, gb, sizeof gb);
    snprintf(sys.mem_name, sizeof sys.mem_name, "Memory (%s GB)", gb);
    sys.have = true;
}

/* The streaming plugin's onRefresh, then its removal of points older than the
 * window (keeping two outside it). */
static void refresh(double now) {
    double vals[4][2] = { { sys.cpu }, { sys.gpu }, { (sys.ram_total - sys.ram_avail) * 100 / sys.ram_total },
                          { sys.down, sys.up } };
    double min = now - (DURATION + DELAY);
    for (int c = 0; c < 4; c++)
        for (int s = 0; s < charts[c].nsets; s++) {
            Series *se = &charts[c].sets[s];
            if (se->n == MAX_POINTS) memmove(se->x, se->x + 1, sizeof se->x[0] * --se->n), memmove(se->y, se->y + 1, sizeof se->y[0] * se->n);
            se->x[se->n] = now, se->y[se->n] = vals[c][s], se->n++;
            int i = 0;
            while (i < se->n && se->x[i] <= min) i++;
            int count = i > 2 ? i - 2 : 0;
            if (count) {
                memmove(se->x, se->x + count, sizeof se->x[0] * (se->n - count));
                memmove(se->y, se->y + count, sizeof se->y[0] * (se->n - count));
                se->n -= count;
            }
        }
}

/* ---- Chart.js --------------------------------------------------------------------- */

static BareFont arial, arial_bold;
static float middle;   /* textBaseline 'middle' for 12 px Arial */

static double nice_num(double range) {   /* helpers.niceNum, rounding up */
    double exponent = floor(log10(range)), fraction = range / pow(10, exponent), nice;
    if (fraction <= 1) nice = 1;
    else if (fraction <= 2) nice = 2;
    else if (fraction <= 5) nice = 5;
    else nice = 10;
    return nice * pow(10, exponent);
}

static double align_px(double pixel, double width) {   /* helpers._alignPixel at a device pixel ratio of 1 */
    return floor(pixel - width / 2 + 0.5) + width / 2;
}

/* moment's 'h:mm:ss.SSS a' in local time, Chart.js's example label for the time scale's capacity. */
static void time_label(double ms, char *out, size_t cap) {
    time_t s = (time_t)floor(ms / 1000);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &s);
#else
    localtime_r(&s, &tmv);
#endif
    int h = tmv.tm_hour % 12;
    snprintf(out, cap, "%d:%02d:%02d.%03d %s", h ? h : 12, tmv.tm_min, tmv.tm_sec, (int)fmod(ms, 1000), tmv.tm_hour < 12 ? "am" : "pm");
}

static void draw_chart(C2d *c, Chart *ch, double ox, double oy, double w, double h, double now) {
    static const C2dColor GRID = { 0, 0, 0, 0.1f }, ZERO = { 0, 0, 0, 0.25f }, TEXT = { 0x66 / 255.f, 0x66 / 255.f, 0x66 / 255.f, 1 };
    /* the y scale's range and ticks */
    double data_max = -INFINITY;
    for (int s = 0; s < ch->nsets; s++)
        for (int i = 0; i < ch->sets[s].n; i++) data_max = fmax(data_max, ch->sets[s].y[i]);
    double ymin = 0, ymax = ch->suggested ? fmax(isfinite(data_max) ? data_max : 0, 100) : 100;
    if (ymin == ymax) ymax++;

    /* layout: title on top (34.4), the time axis under (its tick marks, 10), the value axis on the left */
    double title_h = LINE_H + 20, area_top = title_h, area_bottom = h - 10, area_h = area_bottom - area_top;
    int max_ticks = (int)fmin(11, ceil(area_h / LINE_H));
    if (max_ticks < 2) max_ticks = 2;
    double spacing = nice_num((ymax - ymin) / (max_ticks - 1));
    int spaces = (int)(ceil(ymax / spacing) - floor(ymin / spacing));
    if (spaces > max_ticks - 1) spacing = nice_num(spaces * spacing / (max_ticks - 1));
    double nice_max = ch->suggested ? ceil(ymax / spacing) * spacing : ymax;
    int nt = (int)lround((nice_max - ymin) / spacing);
    if (fabs((nice_max - ymin) / spacing - nt) > 1e-9) nt = (int)ceil((nice_max - ymin) / spacing);
    ymax = nice_max;
    double ticks[16];
    int ntick = 0;
    ticks[ntick++] = ymin;
    for (int j = 1; j < nt && ntick < 15; j++) ticks[ntick++] = round((ymin + j * spacing) * 1e6) / 1e6;
    ticks[ntick++] = ymax;
    char labels[16][32];
    int decimals = spacing >= 1 ? 0 : (int)-floor(log10(spacing));
    double widest = 0;
    for (int i = 0; i < ntick; i++) {
        snprintf(labels[i], sizeof labels[i], "%.*f", decimals, ticks[i]);
        widest = fmax(widest, c2d_measure(c, arial, FONT, labels[i]));
    }
    double area_left = 10 + widest + FONT * 0.5, area_right = w;
    double ypx_k = area_h / (ymax - ymin);
#define YPX(v) (area_bottom - ((v) - ymin) * ypx_k)

    /* title */
    c2d_noclip(c);
    c2d_text_max(c, arial_bold, FONT, ch->title, (float)(ox + w / 2), (float)(oy + LINE_H / 2 + 10), C2D_CENTER, middle,
                 (float)w, TEXT);

    /* the time axis: ticks every few seconds, aligned on the minute (the plugin forces major ticks on) */
    double max = now - DELAY, min = max - DURATION;
    char ex[40];
    time_label(min, ex, sizeof ex);
    double label_w = c2d_measure(c, arial, FONT, ex) * cos(50 * 3.14159265358979 / 180) + FONT * sin(50 * 3.14159265358979 / 180);
    int capacity = (int)floor((area_right - area_left) / label_w);
    if (capacity < 1) capacity = 1;
    static const int STEPS[] = { 1, 2, 5, 10, 15, 30 };
    double range = (max + REFRESH) - min;
    int step = 30;
    for (int i = 0; i < 6; i++)
        if (ceil(range / (1000.0 * STEPS[i])) <= capacity) { step = STEPS[i]; break; }
    double first = floor(min / 1000) * 1000, minute = floor(first / 60000) * 60000;
    double t = minute + floor((first - minute) / (1000.0 * step)) * step * 1000.0;
    double last = floor((max + REFRESH) / 1000) * 1000;
    if (last < max + REFRESH) last += 1000;
    double xborder = align_px(area_bottom, 1);
    for (int guard = 0; guard < 64; guard++, t += step * 1000.0) {
        double lv = area_left + (t - min) / (max - min) * (area_right - area_left);
        if (lv >= area_left - 1e-7) {
            double x = ox + align_px(lv, 1);
            c2d_line(c, (float)x, (float)(oy + xborder + 0.5), (float)x, (float)(oy + area_bottom + 10), 1, GRID);
            c2d_line(c, (float)x, (float)(oy + area_top), (float)x, (float)(oy + xborder - 0.5), 1, GRID);
        }
        if (t >= last) break;
    }
    c2d_line(c, (float)(ox + align_px(area_left, 1) - 0.5), (float)(oy + xborder), (float)(ox + align_px(area_right, 1) + 0.5),
             (float)(oy + xborder), 1, GRID);

    /* the value axis: grid, tick marks, labels, its border */
    double yborder = align_px(area_left, 1);
    for (int i = ntick - 1; i >= 0; i--) {
        double y = oy + align_px(YPX(ticks[i]), 1);
        C2dColor col = ticks[i] == 0 ? ZERO : GRID;
        c2d_line(c, (float)(ox + area_left - 10), (float)y, (float)(ox + yborder - 0.5), (float)y, 1, col);
        c2d_line(c, (float)(ox + yborder + 0.5), (float)y, (float)(ox + area_right), (float)y, 1, col);
        c2d_text(c, arial, FONT, labels[i], (float)(ox + area_left - 10), (float)(oy + YPX(ticks[i])), C2D_RIGHT, middle, TEXT);
    }
    c2d_line(c, (float)(ox + yborder), (float)(oy + align_px(area_top, 1) - 0.5), (float)(ox + yborder),
             (float)(oy + align_px(area_bottom, 1) + 0.5), 1, GRID);

    /* the lines, clipped to the chart area; the last data set first, as Chart.js */
    c2d_clip(c, (float)(ox + area_left), (float)(oy + area_top), (float)(ox + area_right), (float)(oy + area_bottom));
    for (int s = ch->nsets - 1; s >= 0; s--) {
        Series *se = &ch->sets[s];
        float xy[MAX_POINTS * 2];
        for (int i = 0; i < se->n; i++) {
            xy[i * 2] = (float)(ox + area_left + (se->x[i] - min) / (max - min) * (area_right - area_left));
            xy[i * 2 + 1] = (float)(oy + YPX(se->y[i]));
        }
        c2d_polyline(c, xy, se->n, 3, ch->colors[s]);
    }
    c2d_noclip(c);
#undef YPX
}

/* ---- the background: radial-gradient(white, #4d4d4d) under the picture, cover ---- */

static unsigned char *pic;
static int pic_w, pic_h;

/* One axis of a resize with a triangle filter as wide as the scale (Pillow's
 * BILINEAR, a browser's smooth downscale): rows of n source values to m. */
static void resample(const float *src, int n, int stride, float *dst, int m, int dstride, double scale, double offset) {
    double support = fmax(1, 1 / scale);
    for (int i = 0; i < m; i++) {
        double center = (i + 0.5 - offset) / scale, sum[4] = { 0 }, wsum = 0;
        int a = (int)floor(center - support), b = (int)ceil(center + support);
        for (int j = a; j <= b; j++) {
            double wgt = 1 - fabs((j + 0.5 - center) / support);
            if (wgt <= 0) continue;
            int jj = j < 0 ? 0 : j >= n ? n - 1 : j;
            for (int k = 0; k < 4; k++) sum[k] += src[jj * stride + k] * wgt;
            wsum += wgt;
        }
        for (int k = 0; k < 4; k++) dst[i * dstride + k] = (float)(wsum > 0 ? sum[k] / wsum : 0);
    }
}

static BareImage make_background(int w, int h) {
    unsigned char *out = malloc((size_t)w * h * 4);
    float *tmp = NULL, *img = NULL, *srcf = NULL;
    if (!out) return 0;
    if (pic) {   /* the picture, cover-scaled and centred, premultiplied while it is filtered */
        double scale = fmax((double)w / pic_w, (double)h / pic_h);
        double offx = (w - pic_w * scale) / 2, offy = (h - pic_h * scale) / 2;
        srcf = malloc((size_t)pic_w * pic_h * 4 * sizeof *srcf);
        tmp = malloc((size_t)w * pic_h * 4 * sizeof *tmp);
        img = malloc((size_t)w * h * 4 * sizeof *img);
        if (srcf && tmp && img) {
            for (int i = 0; i < pic_w * pic_h; i++) {
                float a = pic[i * 4 + 3] / 255.f;
                for (int k = 0; k < 3; k++) srcf[i * 4 + k] = pic[i * 4 + k] * a;
                srcf[i * 4 + 3] = a * 255;
            }
            for (int y = 0; y < pic_h; y++) resample(srcf + (size_t)y * pic_w * 4, pic_w, 4, tmp + (size_t)y * w * 4, w, 4, scale, offx);
            for (int x = 0; x < w; x++) resample(tmp + x * 4, pic_h, w * 4, img + x * 4, h, w * 4, scale, offy);
        } else free(img), img = NULL;
    }
    double rx = w / 2.0 * sqrt(2.0), ry = h / 2.0 * sqrt(2.0);   /* ellipse farthest-corner */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            double dx = (x + 0.5 - w / 2.0) / rx, dy = (y + 0.5 - h / 2.0) / ry, t = fmin(1, sqrt(dx * dx + dy * dy));
            double g = 255 + (0x4d - 255) * t;
            unsigned char *o = out + ((size_t)y * w + x) * 4;
            double c[3] = { g, g, g };
            if (img) {
                const float *p = img + ((size_t)y * w + x) * 4;
                double a = p[3] / 255;
                for (int k = 0; k < 3; k++) c[k] = p[k] + c[k] * (1 - a);
            }
            for (int k = 0; k < 3; k++) o[k] = (unsigned char)fmin(255, fmax(0, floor(c[k] + 0.5)));
            o[3] = 255;
        }
    free(srcf), free(tmp), free(img);
    BareImage im = bare_image_from_pixels(w, h, out);
    free(out);
    return im;
}

static void load_picture(void) {
    FILE *f = fopen(sp_asset("pcmr.png"), "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *bytes = n > 0 ? malloc((size_t)n) : NULL;
    if (bytes && fread(bytes, 1, (size_t)n, f) == (size_t)n) pic = bare_image_decode(bytes, (int)n, &pic_w, &pic_h);
    free(bytes);
    fclose(f);
}

int main(int argc, char **argv) {
    SpArgs sp = sp_args(argc, argv);
    if (!bare_window_with(.width = sp.width ? sp.width : 1280, .height = sp.height ? sp.height : 720, .title = "Simple System", .parent = sp.parent)) return 1;
    sp_want(SP_WANT_SYSTEM);
    bare_background(BARE_BLACK);
    C2d c;
    c2d_init(&c, bare_shader(&canvas2d_shader));
    arial = bare_font_load("C:/Windows/Fonts/arial.ttf");
    arial_bold = bare_font_load("C:/Windows/Fonts/arialbd.ttf");
    middle = c2d_middle(1854 / 2048.f, 434 / 2048.f, FONT);   /* Arial's ascent and descent */
    load_picture();

    static const C2dColor GREY = { 201 / 255.f, 203 / 255.f, 207 / 255.f, 1 }, LIGHT = { 105 / 255.f, 105 / 255.f, 105 / 255.f, 1 };
    charts[0] = (Chart){ .left = 0.10, .top = 0.15, .nsets = 1, .colors = { GREY } };
    charts[1] = (Chart){ .left = 0.10, .top = 0.525, .nsets = 1, .colors = { GREY } };
    charts[2] = (Chart){ .left = 0.70, .top = 0.25, .nsets = 1, .colors = { GREY } };
    charts[3] = (Chart){ .left = 0.70, .top = 0.625, .aspect = true, .suggested = true, .nsets = 2, .colors = { GREY, LIGHT } };

    BareImage canvas = 0, background = 0;
    int cw = 0, ch = 0;
    bool paused = false, started = false;
    double next_refresh = 0, last_drawn = -1;

    while (bare_frame()) {
        double now = sp_now_ms();
        SpMessage m;
        while (sp_next(&m)) {
            if (m.kind == SP_PAUSE) paused = sp_bool(&m);
            else if (m.kind == SP_SYSTEM) {
                on_system(m.text);
                if (!started) started = true, next_refresh = now + REFRESH;   /* the page makes its charts on the first data */
            }
        }
        BareVec2 win = bare_window_size();
        int w = (int)fmax(1, floor(win.x + 0.5)), h = (int)fmax(1, floor(win.y + 0.5));
        bool resized = w != cw || h != ch;
        if (resized) {
            if (canvas) bare_image_free(canvas), bare_image_free(background);
            canvas = bare_canvas(.width = w, .height = h);
            background = make_background(w, h);
            c2d_picture(&c, background);
            cw = w, ch = h;
        }
        bare_box(.width = BARE_GROW, .height = BARE_GROW, .image = canvas);
        if (paused) { bare_continuous(false); continue; }

        while (started && now >= next_refresh) refresh(next_refresh), next_refresh += REFRESH;
        if (now - next_refresh > 5 * REFRESH) next_refresh = now + REFRESH;   /* back from a long pause */

        /* drawChart: at most 30 times a second */
        double next = last_drawn >= 0 ? last_drawn + 1000.0 / 30 : 0;
        if (next <= now || resized) {
            c2d_image(&c, 0, 0, (float)w, (float)h, 1);
            if (started) {
                charts[0].title = sys.cpu_name, charts[1].title = sys.gpu_name;
                charts[2].title = sys.mem_name, charts[3].title = sys.net_name;
                double cwid = floor(floor(w * 0.2 + 0.5)), chei = floor(h * 0.2 + 0.5);
                for (int i = 0; i < 4; i++)
                    draw_chart(&c, &charts[i], floor(w * charts[i].left + 0.5), floor(h * charts[i].top + 0.5), cwid,
                               charts[i].aspect ? floor(cwid / 2) : chei, now);
            }
            static const float black[4] = { 0, 0, 0, 1 };
            c2d_flush(&c, canvas, w, h, true, black);
            last_drawn = next + 1000.0 / 30 > now && last_drawn >= 0 ? next : now;
        }
        bare_continuous(false);
        double wake = fmin(last_drawn + 1000.0 / 30 - now, started ? next_refresh - now : 1e9);
        if (!started) wake = 1e9;
        bare_wake_in((float)fmax(0.001, wake / 1000));
    }
    bare_quit();
}
