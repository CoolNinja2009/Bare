/* matrix.c: falling katakana; Screen+'s "Matrix Rain Customizable" wallpaper.
 *
 * A port of the page (parambirs / khuong, after thecodeplayer.com's matrix
 * rain): a canvas the size of the window in CSS pixels, never cleared. Every
 * 50 ms a 5% black veil goes over it, then each 14 px column gets a dark
 * square and a random character at its drop, and the drop moves down a row;
 * past the bottom it restarts at the top now and then. The trails are what
 * the veil leaves, rounding included (an 8-bit canvas, as the page's).
 *
 * Text is canvas 2D's fillText: the characters are drawn once into an atlas
 * (Arial for digits, Yu Gothic for katakana, the fonts the page falls back
 * to on Windows) and every square and character is one instance of rain.glsl.
 *
 * A Screen+ app wallpaper: settings are the Customise panel (LivelyProperties.json
 * beside the exe), sent by Screen+ on stdin (see ../screenplus/screenplus_wallpaper.h);
 * by hand they're arguments: matrix rainBow=true rainbowSpeed=80 matrixColor=#ff0000
 * While Screen+ has it paused it draws nothing. */
#include "bare.h"
#include "matrix_shaders.h"
#include "../screenplus/screenplus_wallpaper.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FONT_SIZE 14
#define STEP 0.05      /* setInterval(draw, 50) */
#define MAX_COLUMNS 2048

/* ---- settings ---------------------------------------------------------------- */

static struct {
    int r, g, b;
    double rainbow_speed;
    bool rainbow;
} cfg = { 0x00, 0xFF, 0x46, 0.5, false };

static void set_property(const SpMessage *m) {
    if (!strcmp(m->name, "matrixColor")) {
        unsigned v;
        const char *s = m->value[0] == '#' ? m->value + 1 : m->value;
        if (strlen(s) == 6 && sscanf(s, "%x", &v) == 1) cfg.r = (int)(v >> 16 & 255), cfg.g = (int)(v >> 8 & 255), cfg.b = (int)(v & 255);
    } else if (!strcmp(m->name, "rainBow")) cfg.rainbow = sp_bool(m);
    else if (!strcmp(m->name, "rainbowSpeed")) cfg.rainbow_speed = sp_num(m) / 100;
}

/* ---- the characters ------------------------------------------------------------ */

static const char *const CHARS[] = {
    "゠", "ア", "イ", "ウ", "エ", "オ", "カ", "キ", "ク", "ケ", "コ", "サ", "シ", "ス", "セ", "ソ", "タ", "チ", "ツ", "テ",
    "ト", "ナ", "ニ", "ヌ", "ネ", "ノ", "ハ", "ヒ", "フ", "ヘ", "ホ", "マ", "ミ", "ム", "メ", "モ", "ヤ", "ユ", "ヨ", "ラ",
    "リ", "ル", "レ", "ワ", "ヰ", "ヱ", "ヲ", "ン", "ヺ", "・", "ー", "ヽ", "ヿ", "0", "1", "2", "3", "4", "5", "6",
    "7", "8", "9",
};
#define NCHARS (int)(sizeof CHARS / sizeof CHARS[0])
#define CELL 32   /* atlas cell, pixels */

static struct { int w, h, x, y; } glyph[NCHARS];   /* mask size, its corner from the pen */
static BareImage atlas;
static int atlas_w, atlas_h;

static BareFont load_first(const char *const *paths) {
    for (; *paths; paths++) {
        FILE *f = fopen(*paths, "rb");
        if (!f) continue;
        fclose(f);
        return bare_font_load(*paths);
    }
    return 0;
}

static void build_atlas(void) {
    static const char *const ARIAL[] = { "C:/Windows/Fonts/arial.ttf", "/usr/share/fonts/TTF/arial.ttf",
                                         "/usr/share/fonts/truetype/msttcorefonts/Arial.ttf", NULL };
    static const char *const KANA[] = { "C:/Windows/Fonts/YuGothR.ttc", "C:/Windows/Fonts/msgothic.ttc",
                                        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                                        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", NULL };
    BareFont arial = load_first(ARIAL), kana = load_first(KANA);
    int cols = 16, rows = (NCHARS + cols - 1) / cols;
    atlas_w = cols * CELL, atlas_h = rows * CELL;
    unsigned char *rgba = calloc((size_t)atlas_w * atlas_h, 4);
    if (!rgba) return;
    for (int i = 0; i < NCHARS; i++) {
        bool digit = CHARS[i][0] >= '0' && CHARS[i][0] <= '9';
        int w, h, x, y;
        unsigned char *mask = bare_text_pixels(CHARS[i], digit ? arial : kana, FONT_SIZE, &w, &h, &x, &y, NULL);
        if (!mask) continue;
        if (w > CELL) w = CELL;
        if (h > CELL) h = CELL;
        glyph[i].w = w, glyph[i].h = h, glyph[i].x = x, glyph[i].y = y;
        int ox = i % cols * CELL, oy = i / cols * CELL;
        for (int r = 0; r < h; r++)
            for (int c = 0; c < w; c++) {
                unsigned char *p = rgba + ((size_t)(oy + r) * atlas_w + ox + c) * 4;
                p[0] = p[1] = p[2] = p[3] = mask[r * w + c];   /* rows of the mask are its full width */
            }
        free(mask);
    }
    atlas = bare_image_from_pixels(atlas_w, atlas_h, rgba);
    free(rgba);
}

/* ---- the rain ------------------------------------------------------------------- */

typedef struct Inst { float rect[4], uv[4], color[4]; } Inst;

static int drops[MAX_COLUMNS], columns;
static double hue = -0.01;
static Inst *inst;
static int ninst, cap_inst;

static void push(float x, float y, float w, float h, float u, float v, float r, float g, float b, float a) {
    if (ninst == cap_inst) {
        int cap = cap_inst ? cap_inst * 2 : 1024;
        Inst *n = realloc(inst, (size_t)cap * sizeof *n);
        if (!n) return;
        inst = n, cap_inst = cap;
    }
    inst[ninst++] = (Inst){ { x, y, w, h }, { u, v, 0, 0 }, { r, g, b, a } };
}

/* One run of the page's draw(), as instances. */
static void draw_step(int width, int height) {
    push(0, 0, (float)width, (float)height, -1, 0, 0, 0, 0, 0.05f);   /* the veil */
    for (int i = 0; i < columns; i++) {
        float x = (float)(i * FONT_SIZE), y = (float)(drops[i] * FONT_SIZE);
        push(x, y, FONT_SIZE, FONT_SIZE, -1, 0, 10 / 255.f, 10 / 255.f, 10 / 255.f, 1);
        int k = (int)floor(sp_random() * NCHARS);
        int r = cfg.r, g = cfg.g, b = cfg.b;
        if (cfg.rainbow) {
            hue += -0.01;   /* hueFw is never set */
            r = (int)floor(127 * sin(cfg.rainbow_speed * hue + 0) + 128);
            g = (int)floor(127 * sin(cfg.rainbow_speed * hue + 2) + 128);
            b = (int)floor(127 * sin(cfg.rainbow_speed * hue + 4) + 128);
        }
        if (glyph[k].w > 0)
            push(x + glyph[k].x, y + glyph[k].y, (float)glyph[k].w, (float)glyph[k].h, (float)(k % 16 * CELL),
                 (float)(k / 16 * CELL), r / 255.f, g / 255.f, b / 255.f, 1);
        drops[i]++;
        if (drops[i] * FONT_SIZE > height && sp_random() > 0.975) drops[i] = 0;
    }
}

int main(int argc, char **argv) {
    SpArgs sp = sp_args(argc, argv);
    if (!bare_window_with(.width = sp.width ? sp.width : 1280, .height = sp.height ? sp.height : 720, .title = "Matrix Rain", .parent = sp.parent)) return 1;
    bare_background(BARE_BLACK);
    BareShader shader = bare_shader(&rain_shader);
    build_atlas();
    BareImage canvas = 0;
    int cw = 0, ch = 0;
    bool paused = false;
    double next = 0;
    bool fresh = false;

    while (bare_frame()) {
        SpMessage m;
        while (sp_next(&m)) {
            if (m.kind == SP_PROPERTY) set_property(&m);
            else if (m.kind == SP_PAUSE) paused = sp_bool(&m);
        }
        double now = bare_time();
        BareVec2 win = bare_window_size();   /* the page's canvas is innerWidth x innerHeight CSS pixels */
        int w = (int)fmax(1, floor(win.x)), h = (int)fmax(1, floor(win.y));
        if (w != cw || h != ch) {   /* the page reloads on resize */
            if (canvas) bare_image_free(canvas);
            canvas = bare_canvas(.width = w, .height = h);
            cw = w, ch = h;
            columns = (w + FONT_SIZE - 1) / FONT_SIZE;
            if (columns > MAX_COLUMNS) columns = MAX_COLUMNS;
            for (int i = 0; i < columns; i++) drops[i] = 1;
            fresh = true;
            next = now + STEP;
        }
        bare_box(.width = BARE_GROW, .height = BARE_GROW, .image = canvas, .color = BARE_BLACK);
        if (paused) { bare_continuous(false); continue; }

        int steps = 0;
        while (now >= next - 1e-9 && steps < 20) next += STEP, steps++;
        if (now >= next) next = now + STEP;   /* far behind (a long hitch): skip ahead */
        if (steps || fresh) {
            ninst = 0;
            if (fresh) push(0, 0, (float)w, (float)h, -1, 0, 0, 0, 0, 1);   /* a new canvas: black */
            fresh = false;
            for (int s = 0; s < steps; s++) draw_step(w, h);
            float res[4] = { (float)w, (float)h, (float)atlas_w, (float)atlas_h };
            bare_draw(.target = canvas, .shader = shader, .inputs = { atlas }, .vertex_count = 6,
                      BARE_INSTANCES(inst, ninst), .blend = BARE_BLEND_ALPHA, BARE_PARAMS(res));
        }
        bare_continuous(false);
        bare_wake_in((float)fmax(0, next - now));
    }
    bare_quit();
}
