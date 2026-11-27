/* nullsaint.c: NULL//SAINT, a haunted cyberpunk city that glitches back at you;
 * Screen+'s "NULL//SAINT" wallpaper as a Bare app.
 *
 * A line-by-line port of the page (samples/NullSaint/js): the 2D scene is drawn
 * with ctx2d.h (a Canvas 2D subset on the GPU), then run through the page's
 * WebGL post pipeline (shaders/, from shaders.js) and the HUD is drawn on top,
 * as the page's second canvas is. Every Math.random call happens in the page's
 * order, so with SCREENPLUS_SEED and BARE_FIXED_DT it draws the page's frames.
 *
 * Settings are the Customise panel (LivelyProperties.json beside the exe),
 * sent by Screen+ on stdin; by hand they're arguments: nullsaint theme=3 boot=false
 *
 * Click: shockwave / burst / tear. Keys: 1-9 0 - = trigger events, Space a
 * burst, B the boot sequence, S a whisper, W a worm.
 *
 * Not ported: the photo and anime-girl overlays, the sound synth, the Saint's
 * particle-swarm form (drawn as the face), the settings deck (Customise has them). */
#include "bare.h"
#include "nullsaint_shaders.h"
#include "../screenplus/screenplus_wallpaper.h"
#include "../screenplus/ctx2d.h"
#include "ns_util.h"
#include "ns_config.h"
#include "ns_state.h"
#include "ns_city.h"
#include "ns_scene.h"
#include "ns_fx.h"
#include "ns_director.h"
#include "ns_hud.h"

/* ---- frame-wide pieces main.js owns ---- */
static struct { double a; Rgb c; } FLASH = { 0, { { 255, 255, 255 } } };
static void flash(double a, Rgb c) {
    double cap = CB(safe) ? 0.08 : 1;
    if (a * cap > FLASH.a) FLASH.a = fmin(a, cap), FLASH.c = c;
}

/* ---- fonts: the page's stacks, from the app's assets, then the system ---- */
/* one load per file and face: the stacks share their fallbacks (Yu Gothic alone is 14 MB) */
static struct { char path[260]; int index; CxFace *face; } FACE_CACHE[32];
static int face_cached;
static CxFace *face_try(const char *const *paths, int n, int index) {
    for (int i = 0; i < n; i++) {
        const char *p = paths[i];
        if (!p) continue;
        for (int c = 0; c < face_cached; c++)
            if (FACE_CACHE[c].index == index && !strcmp(FACE_CACHE[c].path, p)) return FACE_CACHE[c].face;
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        fclose(f);
        CxFace *face = cx_face_load(p, index);
        if (!face) continue;
        if (face_cached < (int)(sizeof FACE_CACHE / sizeof *FACE_CACHE) && strlen(p) < sizeof FACE_CACHE[0].path) {
            strcpy(FACE_CACHE[face_cached].path, p);
            FACE_CACHE[face_cached].index = index, FACE_CACHE[face_cached++].face = face;
        }
        return face;
    }
    return NULL;
}
static char *asset_dup(const char *name) {
    const char *p = sp_asset(name);
    size_t n = strlen(p) + 1;
    char *s = (char *)malloc(n);
    if (s) memcpy(s, p, n);
    return s;
}
#define TRY(fam, idx, ...) do { const char *const P_[] = { __VA_ARGS__ }; CxFace *f_ = face_try(P_, (int)(sizeof P_ / sizeof *P_), idx); if (f_) cx_family_add(&FONTS[fam], f_); } while (0)
static void fonts_load(void) {
    char *jb = asset_dup("fonts/JetBrainsMono-VariableFont_wght.ttf"), *jbb = asset_dup("fonts/JetBrainsMono-Bold.ttf");
    char *orb[4] = { asset_dup("fonts/Orbitron-Regular.ttf"), asset_dup("fonts/Orbitron-SemiBold.ttf"),
                     asset_dup("fonts/Orbitron-Bold.ttf"), asset_dup("fonts/Orbitron-Black.ttf") };
    const char *ui[4] = { "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/seguisb.ttf", "C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/seguibl.ttf" };
#define LINUX_SANS "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf"
#define LINUX_SANS_B "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf"
#define CJK "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc"
#define CJK_B "/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc", "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc", "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Bold.ttc"
    /* mono: "JetBrains Mono", Consolas, monospace (and Chrome's fallbacks for what they lack) */
    TRY(F_MONO, 0, jb, "C:/Windows/Fonts/consola.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf");
    TRY(F_MONO_B, 0, jbb, "C:/Windows/Fonts/consolab.ttf", "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf");
    TRY(F_MONO, 0, "C:/Windows/Fonts/segoeui.ttf", LINUX_SANS);
    TRY(F_MONO_B, 0, "C:/Windows/Fonts/segoeuib.ttf", LINUX_SANS_B);
    /* display: Orbitron at the weight asked for, then Segoe UI */
    static const int DISP[4] = { F_DISPLAY, F_DISPLAY_600, F_DISPLAY_700, F_DISPLAY_900 };
    for (int i = 0; i < 4; i++) {
        TRY(DISP[i], 0, orb[i], "C:/Windows/Fonts/Orbitron-Regular.ttf");
        TRY(DISP[i], 0, ui[i], i >= 2 ? "C:/Windows/Fonts/segoeuib.ttf" : "C:/Windows/Fonts/segoeui.ttf", LINUX_SANS);
        /* sans-serif = Arial (box drawing), then system fallback picks Cambria Math for math */
        TRY(DISP[i], 0, i == 3 ? "C:/Windows/Fonts/ariblk.ttf" : i == 2 ? "C:/Windows/Fonts/arialbd.ttf" : "C:/Windows/Fonts/arial.ttf");
        TRY(DISP[i], 1, "C:/Windows/Fonts/cambria.ttc");
    }
    /* jp: "Noto Sans JP", "Yu Gothic", Meiryo */
    TRY(F_JP, 0, "C:/Windows/Fonts/YuGothR.ttc", CJK);
    TRY(F_JP_B, 0, "C:/Windows/Fonts/YuGothB.ttc", CJK_B, CJK);
    TRY(F_JP, 0, "C:/Windows/Fonts/segoeui.ttf", LINUX_SANS);
    TRY(F_JP_B, 0, "C:/Windows/Fonts/segoeuib.ttf", LINUX_SANS_B);
    /* every stack: symbols, then CJK, then Cambria (math) */
    for (int k = 0; k < F_COUNT; k++) {
        TRY(k, 0, "C:/Windows/Fonts/seguisym.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
        if (k != F_JP && k != F_JP_B) TRY(k, 0, (k == F_MONO_B || k >= F_DISPLAY_700) ? "C:/Windows/Fonts/YuGothB.ttc" : "C:/Windows/Fonts/YuGothR.ttc", CJK);
        TRY(k, 0, "C:/Windows/Fonts/cambria.ttc");
    }
    free(jb), free(jbb);
    for (int i = 0; i < 4; i++) free(orb[i]);
}

/* ---- GPU state ---- */
typedef struct Target { BareImage img; int w, h; } Target;
static struct {
    BareShader copy, mosh, bright, blur, final, present;
    CxEngine e;
    CxCtx scene, hud, atlas_ctx;
    BareImage scene_img, atlas, out, view;
    Target a, b, bl1, bl2;
    int sw, sh, vw, vh;
    bool cleared;
    double dpr, hud_dpr;
} R;

typedef struct { float res[2], mouse[2], mvel[2], time, mosh, melt, trail, seed, freeze, swirl, pad_[3]; } MoshParams;
typedef struct { float thr, pad_[3]; } BrightParams;
typedef struct { float dir[2], pad_[2]; } BlurParams;
typedef struct {
    float shock[24], flash[4], tint[4];
    float res[2], mouse[2], mvel[2], time, seed;
    float rgb, scan, curve, grain, block, tear, vhs, sort, bloomK, vig, wave, pix, post, dither, mask, ascii;
    float asciiCell, lens, invert, shuffle, hue, stat, bars, zoom;
    int colorMode, mirror;
    float zero, pad_;
} FinalParams;

static Target target(int w, int h, bool bottom_up) { return (Target){ bare_canvas(.width = w, .height = h, .bottom_up = bottom_up), w, h }; }
static void drop(Target *t) { if (t->img) bare_image_free(t->img); t->img = 0; }

static void make_atlas(void) {
    static const char *const CH[16] = { " ", ".", ":", "-", "=", "+", "*", "x", "o", "%", "#", "&", "8", "@", "\xE2\x96\x93", "\xE2\x96\x88" };
    const int cw = 32, ch = 48;
    R.atlas = bare_canvas(.width = cw * 16, .height = ch);
    cx_ctx_init(&R.atlas_ctx, &R.e, R.atlas, cw * 16, ch);
    CxCtx *x = &R.atlas_ctx;
    cx_fill_color(x, cx_rgba(0, 0, 0, 1)), cx_fill_rect(x, 0, 0, cw * 16, ch);
    cx_fill_color(x, cx_rgba(255, 255, 255, 1)), cx_text_align(x, CX_CENTER), cx_text_baseline(x, CX_MIDDLE);
    ufont(x, 40, F_MONO_B);
    for (int i = 0; i < 16; i++) fill_u8(x, CH[i], i * cw + cw / 2.0, ch / 2.0 + 2);
}

/* main.js resize(): the scene at the quality's scale, the HUD at the display's */
static void resize(double cw, double ch) {
    int q = (int)C(quality);
    if (q < 0 || q > 2) q = 1;
    static const double QS[3] = { 0.5, 0.75, 1 };
    double scale = QS[q] * (q == 2 ? fmin(R.dpr, 1.5) : 1);
    int sw = (int)fmax(64, floor(cw * scale + 0.5)), sh = (int)fmax(64, floor(ch * scale + 0.5));
    if (R.scene_img) bare_image_free(R.scene_img);
    R.scene_img = bare_canvas(.width = sw, .height = sh);
    if (R.scene.e) cx_ctx_reset(&R.scene, R.scene_img, sw, sh);
    else cx_ctx_init(&R.scene, &R.e, R.scene_img, sw, sh);
    F.ctx = &R.scene;
    F.W = sw, F.H = sh, F.cssW = cw, F.cssH = ch, F.k = sw / cw;
    drop(&R.a), drop(&R.b), drop(&R.bl1), drop(&R.bl2);
    if (R.out) bare_image_free(R.out);
    R.a = target(sw, sh, true), R.b = target(sw, sh, true);
    int bw = sw >> 2 > 2 ? sw >> 2 : 2, bh = sh >> 2 > 2 ? sh >> 2 : 2;
    R.bl1 = target(bw, bh, true), R.bl2 = target(bw, bh, true);
    R.out = bare_canvas(.width = sw, .height = sh, .bottom_up = true);
    R.sw = sw, R.sh = sh, R.cleared = false;
    R.hud_dpr = fmin(R.dpr, 2);
    int vw = (int)floor(cw * R.dpr + 0.5), vh = (int)floor(ch * R.dpr + 0.5);
    if (R.view) bare_image_free(R.view);
    R.view = bare_canvas(.width = vw, .height = vh);
    if (R.hud.e) cx_ctx_reset(&R.hud, R.view, vw, vh);
    else cx_ctx_init(&R.hud, &R.e, R.view, vw, vh);
    R.vw = vw, R.vh = vh;
}

static void post_render(double rawDt, const Glitch *G, const double flash4[4]) {
    double mvx = (I.vx * rawDt) / F.cssW, mvy = -(I.vy * rawDt) / F.cssH;
    double trail = C(trail) / 100;
    float seed = (float)(G->seedJump + D.burstSeed);
    if (!R.cleared) { bare_pass(.target = R.b.img, .shader = R.copy, .inputs = { R.scene_img }); R.cleared = true; }
    MoshParams mp = { { (float)F.W, (float)F.H }, { (float)(I.sx / F.cssW), (float)(1 - I.sy / F.cssH) }, { (float)(mvx * trail), (float)(mvy * trail) },
                      (float)F.t, (float)clampd(G->mosh, 0, 1), (float)clampd(G->melt, 0, 1), (float)trail, seed, (float)G->freeze, (float)G->swirl, { 0 } };
    bare_pass(.target = R.a.img, .shader = R.mosh, .inputs = { R.scene_img, R.b.img }, BARE_PARAMS(mp));
    BrightParams bp = { (float)(0.55 - G->bloom * 0.15), { 0 } };
    bare_pass(.target = R.bl1.img, .shader = R.bright, .inputs = { R.a.img }, BARE_PARAMS(bp));
    for (int i = 0; i < 2; i++) {
        BlurParams h = { { (float)((1 + i) / (double)R.bl1.w), 0 }, { 0 } };
        bare_pass(.target = R.bl2.img, .shader = R.blur, .inputs = { R.bl1.img }, BARE_PARAMS(h));
        BlurParams v = { { 0, (float)((1 + i) / (double)R.bl1.h) }, { 0 } };
        bare_pass(.target = R.bl1.img, .shader = R.blur, .inputs = { R.bl2.img }, BARE_PARAMS(v));
    }
    FinalParams fp;
    memset(&fp, 0, sizeof fp);
    fx_shock_uniform(fp.shock);
    for (int i = 0; i < 4; i++) fp.flash[i] = (float)flash4[i];
    fp.tint[0] = (float)(F.P.a.v[0] / 255), fp.tint[1] = (float)(F.P.a.v[1] / 255), fp.tint[2] = (float)(F.P.a.v[2] / 255);
    fp.res[0] = (float)F.W, fp.res[1] = (float)F.H;
    fp.mouse[0] = (float)(I.sx / F.cssW), fp.mouse[1] = (float)(1 - I.sy / F.cssH);
    fp.mvel[0] = (float)mvx, fp.mvel[1] = (float)mvy;
    fp.time = (float)F.t, fp.seed = seed;
    fp.rgb = (float)G->rgb, fp.scan = (float)G->scan, fp.curve = (float)G->curve, fp.grain = (float)G->grain;
    fp.block = (float)G->blocks, fp.tear = (float)G->tear, fp.vhs = (float)G->vhs, fp.sort = (float)clampd(G->sort, 0, 1);
    fp.bloomK = (float)(G->bloom * 1.4), fp.vig = (float)G->vig, fp.wave = (float)G->wave, fp.pix = (float)clampd(G->pix, 0, 1);
    fp.post = (float)clampd(G->post, 0, 1), fp.dither = CB(dither) ? 1.0f : 0.0f, fp.mask = CB(mask) ? 1.0f : 0.0f;
    fp.ascii = (float)clampd(G->ascii, 0, 1), fp.asciiCell = (float)fmax(6, floor(9 * F.k + 0.5));
    fp.lens = (float)G->lens, fp.invert = (float)G->invert, fp.shuffle = (float)G->shuffle, fp.hue = (float)G->hue;
    fp.stat = (float)G->stat, fp.bars = (float)G->bars, fp.zoom = (float)G->zoom;
    fp.colorMode = G->colorMode >= 0 ? G->colorMode : (int)C(colorMode);
    fp.mirror = G->mirror >= 0 ? G->mirror : (int)C(mirror);
    bare_pass(.target = R.out, .shader = R.final, .inputs = { R.a.img, R.bl1.img, R.atlas }, BARE_PARAMS(fp));
    Target t = R.a;
    R.a = R.b, R.b = t;
}

/* ---- Screen+ messages ---- */
static bool host_paused;
static void on_system(const char *json) {
    char v[64];
    SYS.live = true;
    SYS.cpu = sp_json(json, "CurrentCpu", v, sizeof v) && *v && strcmp(v, "null") ? atof(v) : NAN;
    SYS.gpu = sp_json(json, "CurrentGpu3D", v, sizeof v) && *v && strcmp(v, "null") ? atof(v) : NAN;
    SYS.ram = sp_json(json, "CurrentRamAvail", v, sizeof v) && *v && strcmp(v, "null") ? atof(v) : NAN;
    SYS.ramTotal = sp_json(json, "TotalRam", v, sizeof v) && *v && strcmp(v, "null") ? atof(v) : NAN;
    SYS.net = sp_json(json, "CurrentNetDown", v, sizeof v) && *v && strcmp(v, "null") ? atof(v) : NAN;
}
static void on_property(const SpMessage *m) {
    int k = cfg_set(m->name, m->value);
    if (k == -1) return;
    F.P = palette();
    if (k == K_quality || k == -2) resize(F.cssW, F.cssH);
    if (k == K_boot && CB(boot)) director_start_boot();
}

static int key_char(void) {
    static const struct { BareKey k; int c; } KEYMAP[] = {
        { BARE_KEY_1, '1' }, { BARE_KEY_2, '2' }, { BARE_KEY_3, '3' }, { BARE_KEY_4, '4' }, { BARE_KEY_5, '5' }, { BARE_KEY_6, '6' },
        { BARE_KEY_7, '7' }, { BARE_KEY_8, '8' }, { BARE_KEY_9, '9' }, { BARE_KEY_0, '0' }, { BARE_KEY_MINUS, '-' }, { BARE_KEY_EQUALS, '=' },
        { BARE_KEY_SPACE, ' ' }, { BARE_KEY_B, 'b' }, { BARE_KEY_S, 's' }, { BARE_KEY_W, 'w' }, { BARE_KEY_H, 'h' },
    };
    for (size_t i = 0; i < sizeof KEYMAP / sizeof *KEYMAP; i++) if (bare_key_pressed(KEYMAP[i].k)) return KEYMAP[i].c;
    return 0;
}

int main(int argc, char **argv) {
    SpArgs sp = sp_args(argc, argv);
    if (!bare_window_with(.width = sp.width ? sp.width : 1280, .height = sp.height ? sp.height : 720,
                          .title = "NULL//SAINT", .parent = sp.parent)) return 1;
    bare_background(BARE_RGB(0, 0, 0));
    sp_want(SP_WANT_AUDIO | SP_WANT_SYSTEM);
    cfg_defaults();
    noise_init(), glyphs_init(), meshes_init(), fx_init();
    fonts_load();
    R.copy = bare_shader(&ns_copy_shader), R.mosh = bare_shader(&ns_mosh_shader), R.bright = bare_shader(&ns_bright_shader);
    R.blur = bare_shader(&ns_blur_shader), R.final = bare_shader(&ns_final_shader), R.present = bare_shader(&ns_present_shader);
    cx_engine_init(&R.e, bare_shader(&ctx2d_shader), 0.65);
    make_atlas();
    const bool fixed = getenv("BARE_FIXED_DT") != NULL;   /* a test: the page's virtual clock, no input */
    BareVec2 win = bare_window_size();
    R.dpr = bare_pixel_scale() > 0 ? bare_pixel_scale() : 1;
    resize(fmax(1, floor(win.x + 0.5)), fmax(1, floor(win.y + 0.5)));
    F.P = palette(), F.speed = 1, F.gi = 1, F.blink = 1, F.dt = 0.016;
    I.x = I.sx = F.cssW / 2, I.y = I.sy = F.cssH / 2, I.inside = true;
    if (CB(boot)) director_start_boot();
    double last = 0;
    long frame_no = 0;
    BareVec2 last_mouse = bare_mouse();

    while (bare_frame()) {
        SpMessage m;
        while (sp_next(&m)) {
            if (m.kind == SP_PROPERTY) on_property(&m);
            else if (m.kind == SP_PAUSE) host_paused = sp_bool(&m);
            else if (m.kind == SP_AUDIO) {
                for (int i = 0; i < 128; i++) A.raw[i] = i < m.nbands ? m.bands[i] : 0;
                A.lastData = F.now_ms;
            } else if (m.kind == SP_SYSTEM && m.text) on_system(m.text);
        }
        win = bare_window_size();
        double cw = fmax(1, floor(win.x + 0.5)), ch = fmax(1, floor(win.y + 0.5));
        float dpr = bare_pixel_scale();
        if (cw != F.cssW || ch != F.cssH || (dpr > 0 && dpr != R.dpr)) { if (dpr > 0) R.dpr = dpr; resize(cw, ch); }
        BareState box = bare_box(.width = BARE_GROW, .height = BARE_GROW, .image = R.view);
        if (host_paused) { bare_continuous(false); continue; }
        bare_continuous(true);

        frame_no++;
        double now = fixed ? frame_no * (1000.0 / 60) : bare_time() * 1000;
        static const int CAPS[3] = { 30, 60, 0 };
        int fi = (int)C(fps);
        int cap = CAPS[fi >= 0 && fi < 3 ? fi : 1];
        double elapsed = now - last;
        if (cap && elapsed < 1000.0 / cap - 2) continue;
        last = now;
        double rawDt = fmin(0.1, elapsed / 1000);
        F.now_ms = now;
        timers_run(now);

        if (!fixed) {   /* input.js: pointer moves, clicks and keys */
            BareVec2 mo = bare_mouse();
            if (mo.x != last_mouse.x || mo.y != last_mouse.y) I.x = mo.x, I.y = mo.y, I.lastMove = now, I.inside = true;
            last_mouse = mo;
            I.down = box.pressed;
            if (box.clicked && I.nclicks < 16) I.clicks[I.nclicks].x = mo.x, I.clicks[I.nclicks].y = mo.y, I.nclicks++, I.lastMove = now;
            int kc = key_char();
            if (kc && I.nkeys < 16) I.keys[I.nkeys++] = kc, I.lastMove = now;
        }

        F.speed = C(speed) / 100;
        F.dt = rawDt * F.speed;
        F.t += F.dt;
        F.frame++;
        input_update(rawDt);
        audio_update(rawDt, F.t);
        sys_update(rawDt);
        F.mx = I.sx * F.k, F.my = I.sy * F.k;
        double par = C(parallax) / 100;
        F.par = (I.sx / F.cssW - 0.5) * par * 140 * F.k;
        F.parY = (I.sy / F.cssH - 0.5) * par * 70 * F.k;
        F.blink = S.blink;
        Glitch *G = director_update();

        for (int i = 0; i < I.nclicks; i++) {
            Click c = I.clicks[i];
            ghosts_note_click(c.x, c.y, F.t);
            if (hud_click(c.x, c.y, F.cssW, F.cssH)) continue;
            double x = c.x * F.k, y = c.y * F.k;
            if (CB(saint)) saint_poke(x, y);
            fx_click(x, y);
        }
        I.nclicks = 0;
        for (int i = 0; i < I.nkeys; i++) {
            if (I.keys[i] == 'h') { CFG[K_hud] = !CB(hud); continue; }
            director_key(I.keys[i]);
        }
        I.nkeys = 0;

        CxCtx *ctx = &R.scene;
        F.ctx = ctx;
        cx_set_transform(ctx, 1, 0, 0, 1, 0, 0);
        cx_alpha(ctx, 1), cx_op(ctx, CX_OVER);
        city_draw(&R.e);
        holo_artifact();
        glyphs_draw();
        saint_draw();
        ghosts_draw();
        fx_draw();
        director_draw();
        bool booting = director_draw_boot();
        if (CB(hud) && CB(hudGlitch) && !booting) {
            cx_save(ctx), cx_set_transform(ctx, F.k, 0, 0, F.k, 0, 0);
            hud_draw(ctx, F.cssW, F.cssH);
            cx_restore(ctx);
        }
        cx_flush(&R.e);

        FLASH.a = fmax(0, FLASH.a - rawDt * 3);
        double fl[4];
        if (G->flash[3] > FLASH.a) memcpy(fl, G->flash, sizeof fl);
        else fl[0] = FLASH.c.v[0] / 255, fl[1] = FLASH.c.v[1] / 255, fl[2] = FLASH.c.v[2] / 255, fl[3] = FLASH.a;
        post_render(rawDt, G, fl);
        bare_pass(.target = R.view, .shader = R.present, .inputs = { R.out });

        CxCtx *hc = &R.hud;
        if (!booting && !(CB(hud) && CB(hudGlitch))) {
            cx_set_transform(hc, R.hud_dpr, 0, 0, R.hud_dpr, 0, 0);
            hud_draw(hc, F.cssW, F.cssH);
        }
        cx_flush(&R.e);
        cx_frame_end(&R.e);
    }
    return 0;
}
