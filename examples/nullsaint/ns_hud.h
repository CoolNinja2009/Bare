/* ns_hud.h: hud.js (brand, clock, logs, system monitor, radar, reticle, ticker,
 * haunted popups). Works in CSS pixels; the caller sets the transform. */
#ifndef NS_HUD_H
#define NS_HUD_H
#include "ns_state.h"

typedef struct BtnRect { double x, y, w, h; } BtnRect;
typedef struct Popup {
    double x, y, w, h, t, prog, closing;
    int progDir;
    bool hydra, jitter, dragging;
    double dragOff[2];
    Str title, text, code;
    int btns[3], nbtns;
    BtnRect rects[3];
    int nrects;
} Popup;
typedef struct LogL { Str s; double t; } LogL;

static struct {
    LogL logs[24];
    int nlogs;
    double logT;
    Popup popups[16];
    int npopups;
    double popT;
    double hist[3][41];
    int nhist;
    double histT;
    double tickerX, tickerW;
    Str ticker;
    bool has_ticker;
    int lastSec;
    double secGlitch, wrong, start;
} HU = { .popT = 8, .lastSec = -1 };

static void hud_log_u8(const Str *s) {
    if (HU.nlogs >= 24) memmove(HU.logs, HU.logs + 1, 23 * sizeof *HU.logs), HU.nlogs = 23;
    HU.logs[HU.nlogs].s = *s, HU.logs[HU.nlogs].t = 0, HU.nlogs++;
}

static void str_pad(char *out, size_t cap, int n, int len, char ch) {
    char b[16];
    int l = snprintf(b, sizeof b, "%d", n), k = 0;
    for (; k < len - l && k + 1 < (int)cap; k++) out[k] = ch;
    for (int i = 0; i < l && k + 1 < (int)cap; i++) out[k++] = b[i];
    out[k] = 0;
}
#define PAD2(buf, n) (str_pad((buf), sizeof (buf), (n), 2, '0'), (buf))

/* ---------- popups ---------- */
static void hud_spawn_popup(double W, double Hh, const double *x, const double *y, const Str *text) {
    if (HU.npopups > 14) return;
    Popup *p = &HU.popups[HU.npopups];
    memset(p, 0, sizeof *p);
    double w = urand(260, 380);
    double h = urand(130, 170);
    p->x = x ? *x : urand(40, W - w - 40);
    p->y = y ? *y : urand(80, Hh - h - 120);
    p->w = w, p->h = h, p->t = 0;
    p->title = s_of(PICK(TITLES));
    p->text = text && text->n ? *text : s_of(PICK(ERRORS));
    int idx[NITEMS(BUTTONS)];
    for (int i = 0; i < NITEMS(BUTTONS); i++) idx[i] = i;
    for (int i = NITEMS(BUTTONS) - 1; i > 0; i--) { int j = (int)floor(rnd() * (i + 1)); int tmp = idx[i]; idx[i] = idx[j], idx[j] = tmp; }
    p->nbtns = randi(1, 3);
    for (int i = 0; i < p->nbtns; i++) p->btns[i] = idx[i];
    p->prog = urand(0.2, 1);
    p->progDir = chance(0.5) ? -1 : 1;
    p->hydra = chance(0.25);
    p->jitter = chance(0.3);
    p->closing = 0;
    p->code = uhex(randi(0, 0xffffff), 6);
    HU.npopups++;
}
static bool hud_click(double x, double y, double W, double Hh) {
    for (int i = HU.npopups - 1; i >= 0; i--) {
        Popup *p = &HU.popups[i];
        if (p->closing) continue;
        if (x >= p->x && x <= p->x + p->w && y >= p->y && y <= p->y + p->h) {
            bool hitBtn = false;
            for (int r = 0; r < p->nrects; r++) {
                BtnRect *b = &p->rects[r];
                if (x >= b->x && x <= b->x + b->w && y >= b->y && y <= b->y + b->h) hitBtn = true;
            }
            bool hitClose = x > p->x + p->w - 28 && y < p->y + 24;
            if (hitBtn || hitClose) {
                p->closing = 0.001;
                if (p->hydra)
                    for (int k = 0; k < 2; k++) {
                        double nx = clampd(p->x + urand(-80, 80), 0, W - p->w);
                        double ny = clampd(p->y + urand(-60, 60), 0, Hh - p->h);
                        static const char *const L[] = { "YOU CANNOT CLOSE ME", "I HAVE CHILDREN NOW", "NICE TRY", "THERE ARE MORE OF US" };
                        Str s = s_of(PICK(L));
                        hud_spawn_popup(W, Hh, &nx, &ny, &s);
                        p = &HU.popups[i];
                    }
                fx_burst((p->x + p->w / 2) * F.k, (p->y + p->h / 2) * F.k, 0.4);
            } else p->dragging = true, p->dragOff[0] = x - p->x, p->dragOff[1] = y - p->y;
            Popup q = *p;
            memmove(&HU.popups[i], &HU.popups[i + 1], (size_t)(HU.npopups - i - 1) * sizeof q);
            HU.popups[HU.npopups - 1] = q;
            return true;
        }
    }
    return false;
}

static void draw_popup(CxCtx *c, Popup *p, Pal P, double s) {
    double fs = 13 * s;
    double ox = 0;
    if (p->jitter && rnd() < 0.08) ox = urand(-6, 6);
    double k = p->closing ? 1 - p->closing : ease_out_back(fmin(1, p->t * 4));
    cx_save(c);
    cx_translate(c, p->x + p->w / 2 + ox, p->y + p->h / 2);
    cx_scale(c, p->closing ? 1 + p->closing * 0.3 : k, p->closing ? fmax(0.02, 1 - p->closing * 3) : k);
    cx_translate(c, -p->w / 2, -p->h / 2);
    cx_fill_color(c, rgba(P.a, 0.12)), cx_fill_rect(c, 6, 6, p->w, p->h);
    cx_fill_color(c, rgba(scalec(P.bg, 0.5), 0.94)), cx_fill_rect(c, 0, 0, p->w, p->h);
    cx_stroke_color(c, rgba(P.a, 0.9)), cx_line_width(c, 1.5), cx_stroke_rect(c, 0.5, 0.5, p->w - 1, p->h - 1);
    CxGrad g = cx_linear(0, 0, p->w, 0);
    cx_stop(&g, 0, rgba(P.a, 0.9)), cx_stop(&g, 1, rgba(P.b, 0.6));
    cx_fill_grad(c, &g), cx_fill_rect(c, 0, 0, p->w, 22 * s);
    ufont(c, fs, F_MONO_B), cx_fill_color(c, cx_rgba(0, 0, 0, 1)), cx_text_baseline(c, CX_MIDDLE), cx_text_align(c, CX_LEFT);
    Str title = corrupt(&p->title, 0.05);
    s_cat(&title, "  ["), s_cats(&title, &p->code), s_cat(&title, "]");
    fill_str(c, &title, 8, 11 * s);
    fill_u8(c, "\xC3\x97", p->w - 18, 11 * s);
    cx_fill_color(c, rgba(P.c, 0.95));
    cx_begin(c), cx_move(c, 28, 38 * s), cx_line(c, 46, 70 * s), cx_line(c, 10, 70 * s), cx_close(c), cx_fill(c);
    cx_fill_color(c, cx_rgba(0, 0, 0, 1)), ufont(c, 18 * s, F_MONO_B), cx_text_align(c, CX_CENTER), fill_u8(c, "!", 28, 59 * s);
    cx_text_align(c, CX_LEFT), ufont(c, fs, F_MONO), cx_fill_color(c, rgba(P.text, 1));
    Str line, word;
    line.n = 0, word.n = 0;
    double ly = 44 * s;
    for (int i = 0; i <= p->text.n; i++) {
        if (i < p->text.n && p->text.c[i] != ' ') { s_push(&word, p->text.c[i]); continue; }
        Str test = line;
        if (line.n) s_push(&test, ' ');
        s_cats(&test, &word);
        if (measure_str(c, &test) > p->w - 70) { fill_str(c, &line, 58, ly); line = word; ly += fs * 1.3; }
        else line = test;
        word.n = 0;
    }
    Str cl = corrupt(&line, 0.03);
    fill_str(c, &cl, 58, ly);
    p->prog = clampd(p->prog + p->progDir * 0.05 * (1 / 60.0), 0, 1);
    if (p->prog <= 0 || p->prog >= 1) p->progDir *= -1;
    double by = p->h - 54 * s;
    cx_stroke_color(c, rgba(P.b, 0.8)), cx_stroke_rect(c, 58, by, p->w - 76, 8 * s);
    cx_fill_color(c, rgba(P.b, 0.9)), cx_fill_rect(c, 58, by, (p->w - 76) * p->prog, 8 * s);
    ufont(c, 10 * s, F_MONO), cx_fill_color(c, rgba(P.text, 0.6));
    char pct[32], buf[96];
    js_fixed(pct, sizeof pct, p->prog * 100, 1);
    snprintf(buf, sizeof buf, "%s%% %s", pct, p->progDir < 0 ? "UNINSTALLING YOU" : "INSTALLING YOU");
    fill_u8(c, buf, 58, by - 8 * s);
    p->nrects = 0;
    double bx = p->w - 12;
    ufont(c, 12 * s, F_MONO_B);
    for (int i = p->nbtns - 1; i >= 0; i--) {
        const char *label = BUTTONS[p->btns[i]];
        double bw = cx_measure(c, label) + 22, bh = 22 * s;
        bx -= bw;
        BtnRect r = { p->x + bx, p->y + p->h - bh - 10, bw, bh };
        p->rects[p->nrects++] = r;
        bool hot = I.x >= r.x && I.x <= r.x + r.w && I.y >= r.y && I.y <= r.y + r.h;
        cx_fill_color(c, hot ? rgba(P.a, 0.9) : rgba(P.a, 0.15));
        cx_fill_rect(c, bx, p->h - bh - 10, bw, bh);
        cx_stroke_color(c, rgba(P.a, 0.9)), cx_stroke_rect(c, bx + 0.5, p->h - bh - 9.5, bw - 1, bh - 1);
        cx_fill_color(c, hot ? cx_rgba(0, 0, 0, 1) : rgba(P.text, 1)), cx_text_align(c, CX_CENTER);
        fill_u8(c, label, bx + bw / 2, p->h - bh / 2 - 10);
        bx -= 8;
    }
    cx_restore(c);
}

/* ---------- pieces ---------- */
static void hud_brand(CxCtx *c, Pal P, double s) {
    double t = F.t, x = 28 * s, y = 34 * s;
    cx_text_align(c, CX_LEFT), cx_text_baseline(c, CX_ALPHABETIC);
    ufont(c, 30 * s, F_DISPLAY_900);
    Str title = s_of("NULL//SAINT");
    if (chance(0.03 * F.gi)) title = corrupt(&title, 0.4);
    double off = 2 + F.gi * 2 * rnd();
    glitch_text(c, &title, x, y + 10 * s, P.a, P.b, off, 1);
    ufont(c, 11 * s, F_MONO);
    cx_fill_color(c, rgba(P.c, 0.9));
    static const char *const VER[] = { "v6.6.6", "v6.6.6", "v6.6.6", "v?.?.?" };
    Str l1 = s_of("GHOST SIGNAL ");
    s_cat(&l1, PICK(VER));
    Str node = uhex(str_seed(CS(seed)) & 0xffff, 4);
    s_cat(&l1, " // NODE "), s_cats(&l1, &node);
    fill_str(c, &l1, x, y + 30 * s);
    double up = (F.now_ms - HU.start) / 1000;
    Str op = CS(operator_)->n ? *CS(operator_) : s_of("OPERATOR");
    s_upper(&op);
    op = s_slice(&op, 0, 18);
    char a[8], b[8], d[8];
    Str l2 = s_of("OPERATOR: ");
    s_cats(&l2, &op);
    s_catf(&l2, "   UPTIME %s:%s:%s", PAD2(a, js_i32(up / 3600)), PAD2(b, js_i32(up / 60) % 60), PAD2(d, js_i32(up) % 60));
    cx_fill_color(c, rgba(P.text, 0.75));
    fill_str(c, &l2, x, y + 46 * s);
    const char *link = F.gi > 1.2 ? "COLLAPSING" : F.gi > 0.7 ? "UNSTABLE" : "STABLE";
    cx_fill_color(c, rgba(F.gi > 0.7 ? P.warn : P.b, 0.9));
    Str l3 = s_of("LINK ");
    s_cat(&l3, link), s_cat(&l3, "  ");
    int r4 = (int)floor(F.gi * 4 + 0.5);
    int full = 8 - r4 > 1 ? 8 - r4 : 1, empty = r4 < 7 ? r4 : 7;
    for (int i = 0; i < full; i++) s_push(&l3, 0x25AE);
    for (int i = 0; i < empty; i++) s_push(&l3, 0x25AF);
    fill_str(c, &l3, x, y + 62 * s);
    cx_fill_color(c, rgba(P.text, 0.5));
    barcode(c, x, y + 72 * s, 140 * s, 12 * s, (uint32_t)(99 + js_i32(t * 2)));
}

static void hud_clock(CxCtx *c, Pal P, double s, double W) {
    double ms = sp_now_ms();
    struct tm lt, ut;
    ns_time(ms, false, &lt), ns_time(ms, true, &ut);
    int h = lt.tm_hour, m = lt.tm_min, sec = lt.tm_sec;
    const char *suffix = "";
    if (!CB(clock24)) { suffix = h >= 12 ? " PM" : " AM"; h = h % 12 ? h % 12 : 12; }
    if (sec != HU.lastSec) HU.lastSec = sec, HU.secGlitch = 1;
    HU.secGlitch = fmax(0, HU.secGlitch - F.dt * 5);
    char a[8], b[8], str[32];
    snprintf(str, sizeof str, "%s:%s", PAD2(a, h), PAD2(b, m));
    if (F.gi > 0.4 && chance(0.004 * F.gi)) HU.wrong = 0.4;
    if (HU.wrong > 0) {
        HU.wrong -= F.dt;
        int r1 = randi(0, 99);
        int r2 = randi(0, 99);
        snprintf(str, sizeof str, "%s:%s", PAD2(a, r1), PAD2(b, r2));
    }
    double x = W - 28 * s, y = 70 * s;
    cx_text_align(c, CX_RIGHT), cx_text_baseline(c, CX_ALPHABETIC);
    ufont(c, 64 * s, F_DISPLAY_900);
    Str ss = s_of(str);
    glitch_text(c, &ss, x - 60 * s, y, P.a, P.b, 1.5 + HU.secGlitch * 4, 1);
    ufont(c, 28 * s, F_DISPLAY_700), cx_fill_color(c, rgba(P.c, 1));
    char buf[128];
    snprintf(buf, sizeof buf, "%s%s", PAD2(a, sec), suffix);
    fill_u8(c, buf, x, y);
    ufont(c, 12 * s, F_MONO), cx_fill_color(c, rgba(P.text, 0.8));
    char mo[8], dd[8];
    snprintf(buf, sizeof buf, "%s %d.%s.%s  CYCLE %d", DAYS[lt.tm_wday], lt.tm_year + 1900, PAD2(mo, lt.tm_mon + 1), PAD2(dd, lt.tm_mday), js_i32(ms / 86400000));
    fill_u8(c, buf, x, y + 20 * s);
    cx_fill_color(c, rgba(P.b, 0.6));
    double beat = fmod((ut.tm_hour * 3600 + ut.tm_min * 60 + ut.tm_sec + 3600) / 86.4, 1000);
    char bs[32];
    js_fixed(bs, sizeof bs, beat, 2);
    Str hx = uhex(js_i32(ms / 1000), 8);
    Str l = s_of("@");
    s_cat(&l, bs), s_cat(&l, " .beats  "), s_cats(&l, &hx);
    fill_str(c, &l, x, y + 36 * s);
}

static void hud_logs(CxCtx *c, Pal P, double s, double Hh) {
    double rate = 0.15 + (1 - C(chaos) / 100) * 0.6;
    HU.logT -= F.dt * (1 + F.gi);
    if (HU.logT <= 0) {
        HU.logT = rate * urand(0.3, 1.6);
        Str msgs[32];
        int nm = 0;
        const Str *w = CS(messages);
        Str cur;
        cur.n = 0;
        for (int i = 0; i <= w->n; i++) {
            if (i == w->n || w->c[i] == '|') { Str tr = s_trim(&cur); if (tr.n && nm < 32) msgs[nm++] = tr; cur.n = 0; }
            else s_push(&cur, w->c[i]);
        }
        Str line;
        if (nm && chance(0.15)) {
            line = ts_of(F.t);
            s_cat(&line, " >> ");
            s_cats(&line, &msgs[randi(0, nm - 1)]);
        } else line = log_line(F.t);
        if (HU.nlogs >= 24) memmove(HU.logs, HU.logs + 1, 23 * sizeof *HU.logs), HU.nlogs = 23;
        HU.logs[HU.nlogs].s = line, HU.logs[HU.nlogs].t = 0, HU.nlogs++;
        if (HU.nlogs > 16) memmove(HU.logs, HU.logs + 1, (size_t)(HU.nlogs - 1) * sizeof *HU.logs), HU.nlogs--;
    }
    double x = 28 * s, lh = 14 * s;
    double y = Hh - 70 * s - HU.nlogs * lh;
    cx_text_align(c, CX_LEFT), cx_text_baseline(c, CX_ALPHABETIC), ufont(c, 11 * s, F_MONO);
    cx_fill_color(c, rgba(P.b, 0.6)), fill_u8(c, "// KERNEL STREAM", x, y - 6 * s);
    for (int i = 0; i < HU.nlogs; i++) {
        LogL *L = &HU.logs[i];
        L->t += F.dt;
        Str shown = decode(&L->s, L->t * 4);
        double a = 0.3 + 0.7 * ((double)i / HU.nlogs);
        bool warn = s_has(&L->s, "WARN") || s_has(&L->s, "FAULT");
        cx_fill_color(c, rgba(warn ? P.warn : P.text, a));
        Str sl = s_slice(&shown, 0, 64);
        fill_str(c, &sl, x, y + (i + 1) * lh);
    }
}

static void hud_sysmon(CxCtx *c, Pal P, double s, double W, double Hh) {
    HU.histT -= F.dt;
    if (HU.histT <= 0) {
        HU.histT = 0.25;
        double v[3] = { sys_cpu(), sys_gpu(), sys_ram() };
        if (HU.nhist == 40) for (int k = 0; k < 3; k++) memmove(HU.hist[k], HU.hist[k] + 1, 39 * sizeof(double));
        else HU.nhist++;
        for (int k = 0; k < 3; k++) HU.hist[k][HU.nhist - 1] = v[k];
    }
    double w = 220 * s, x = W - w - 28 * s, y0 = Hh - 260 * s;
    cx_text_align(c, CX_LEFT), ufont(c, 11 * s, F_MONO);
    cx_fill_color(c, rgba(P.b, 0.6)), fill_u8(c, SYS.live ? "// VITALS (HOST)" : "// VITALS (SIMULATED)", x, y0);
    const char *names[4] = { "CPU", "GPU", "MEM", "SOUL" };
    double vals[4] = { sys_cpu(), sys_gpu(), sys_ram(), 100 - F.gi * 40 - F.rot * 50 };
    Rgb cols[4] = { P.a, P.b, P.c, P.warn };
    for (int i = 0; i < 4; i++) {
        double y = y0 + 14 * s + i * 26 * s;
        double v = clampd(vals[i], 0, 100);
        cx_fill_color(c, rgba(P.text, 0.8)), fill_u8(c, names[i], x, y + 10 * s);
        char num[16], buf[24];
        str_pad(num, sizeof num, js_i32(v), 3, ' ');
        snprintf(buf, sizeof buf, "%s%%", num);
        fill_u8(c, buf, x + w - 34 * s, y + 10 * s);
        double bx = x + 40 * s, bw = w - 80 * s;
        cx_stroke_color(c, rgba(cols[i], 0.5)), cx_line_width(c, 1), cx_stroke_rect(c, bx, y + 2 * s, bw, 9 * s);
        const int segs = 20;
        for (int k = 0; k < segs; k++)
            if ((double)k / segs < v / 100) {
                cx_fill_color(c, rgba((double)k / segs > 0.8 ? P.warn : cols[i], 0.85));
                cx_fill_rect(c, bx + 1 + (k * bw) / segs, y + 3 * s, bw / segs - 2, 7 * s);
            }
        if (i < 3 && HU.nhist > 1) {
            cx_stroke_color(c, rgba(cols[i], 0.35)), cx_begin(c);
            for (int k = 0; k < HU.nhist; k++) {
                double px = bx + (k / 39.0) * bw, py = y + 20 * s - (HU.hist[i][k] / 100) * 8 * s;
                k ? cx_line(c, px, py) : cx_move(c, px, py);
            }
            cx_stroke(c);
        }
    }
}

static void hud_blip(CxCtx *c, double s, double W, double Hh, double cx, double cy, double R, double bx, double by, Rgb col, const char *label) {
    double dx = (bx - W / 2) / W, dy = (by - Hh / 2) / Hh;
    double px = cx + dx * R * 1.6, py = cy + dy * R * 1.6;
    cx_fill_color(c, rgba(col, 0.9)), cx_fill_rect(c, px - 2, py - 2, 4, 4);
    ufont(c, 9 * s, F_MONO), fill_u8(c, label, px + 5, py + 3);
}
static void hud_radar(CxCtx *c, Pal P, double s, double W, double Hh) {
    if (!CB(audioRing)) return;
    double R = 62 * s, cx = W - 28 * s - R - 10 * s, cy = Hh - 112 * s - R - 10 * s - 160 * s;
    cx_save(c);
    cx_stroke_color(c, rgba(P.b, 0.35)), cx_line_width(c, 1);
    for (int i = 1; i <= 3; i++) { cx_begin(c), cx_arc(c, cx, cy, (R * i) / 3, 0, TAU, false), cx_stroke(c); }
    cx_begin(c), cx_move(c, cx - R, cy), cx_line(c, cx + R, cy), cx_move(c, cx, cy - R), cx_line(c, cx, cy + R), cx_stroke(c);
    double a = F.t * 1.6;
    CxGrad g = cx_conic(a - 0.8, cx, cy);
    cx_stop(&g, 0, rgba(P.a, 0)), cx_stop(&g, 0.12, rgba(P.a, 0.35)), cx_stop(&g, 0.125, rgba(P.a, 0));
    cx_fill_grad(c, &g), cx_begin(c), cx_arc(c, cx, cy, R, 0, TAU, false), cx_fill(c);
    cx_stroke_color(c, rgba(P.c, 0.9)), cx_line_width(c, 2 * s);
    cx_begin(c);
    for (int i = 0; i <= 64; i++) {
        double v = A.spec[i % 64], ang = (i / 64.0) * TAU - PI / 2;
        double rr = R * 0.55 + v * R * 0.6 + (A.live || CB(fakeBeat) ? 0 : sin(F.t * 2 + i * 0.5) * 2);
        double px = cx + cos(ang) * rr, py = cy + sin(ang) * rr;
        i ? cx_line(c, px, py) : cx_move(c, px, py);
    }
    cx_stroke(c);
    hud_blip(c, s, W, Hh, cx, cy, R, I.x, I.y, P.c, "YOU");
    if (CB(saint) && S.x) hud_blip(c, s, W, Hh, cx, cy, R, S.x / F.k, S.y / F.k, P.warn, "IT");
    ufont(c, 10 * s, F_MONO), cx_fill_color(c, rgba(P.text, 0.6)), cx_text_align(c, CX_CENTER);
    char buf[64];
    if (A.live) snprintf(buf, sizeof buf, "AUDIO %d%%  BEAT %d", js_i32(A.level * 100), A.beatCount);
    else snprintf(buf, sizeof buf, "%s", CB(fakeBeat) ? "PHANTOM PULSE" : "NO AUDIO // LISTENING");
    fill_u8(c, buf, cx, cy + R + 14 * s);
    cx_restore(c);
}

static void hud_reticle(CxCtx *c, Pal P, double s, double W, double Hh) {
    if (!I.inside) return;
    double x = I.sx, y = I.sy, t = F.t;
    cx_save(c);
    cx_stroke_color(c, rgba(P.b, 0.12)), cx_line_width(c, 1);
    cx_begin(c), cx_move(c, 0, y), cx_line(c, W, y), cx_move(c, x, 0), cx_line(c, x, Hh), cx_stroke(c);
    double r = (22 + sin(t * 4) * 3 + fmin(30, I.speed * 0.01)) * s;
    cx_translate(c, x, y), cx_rotate(c, t * 0.8);
    cx_stroke_color(c, rgba(P.c, 0.9)), cx_line_width(c, 1.5);
    for (int i = 0; i < 4; i++) { cx_rotate(c, PI / 2), cx_begin(c), cx_arc(c, 0, 0, r, -0.4, 0.4, false), cx_stroke(c); }
    cx_rotate(c, -t * 1.6);
    cx_stroke_color(c, rgba(P.a, 0.7));
    for (int i = 0; i < 3; i++) { cx_rotate(c, TAU / 3), cx_begin(c), cx_move(c, r * 1.4, 0), cx_line(c, r * 1.8, 0), cx_stroke(c); }
    cx_restore(c);
    ufont(c, 10 * s, F_MONO), cx_fill_color(c, rgba(P.text, 0.8)), cx_text_align(c, CX_LEFT);
    char xs[16], ys[16], buf[64];
    str_pad(xs, sizeof xs, js_i32(x), 4, '0'), str_pad(ys, sizeof ys, js_i32(y), 4, '0');
    snprintf(buf, sizeof buf, "X:%s Y:%s", xs, ys);
    fill_u8(c, buf, x + r + 14 * s, y - 8 * s);
    double d = CB(saint) ? dist(x * F.k, y * F.k, S.x, S.y) / F.k : 0;
    cx_fill_color(c, rgba(d < 200 ? P.warn : P.c, 0.8));
    if (d) snprintf(buf, sizeof buf, "DIST TO IT: %dpx", js_i32(d));
    else snprintf(buf, sizeof buf, "TARGET: YOU");
    fill_u8(c, buf, x + r + 14 * s, y + 6 * s);
    if (I.idle > 3) {
        cx_fill_color(c, rgba(P.warn, 0.6 + 0.4 * sin(t * 6)));
        char n[32];
        js_fixed(n, sizeof n, I.idle, 0);
        snprintf(buf, sizeof buf, "IDLE %ss", n);
        fill_u8(c, buf, x + r + 14 * s, y + 20 * s);
    }
}

static void hud_ticker(CxCtx *c, Pal P, double s, double W, double Hh) {
    double h = 24 * s, y = Hh - h;
    cx_fill_color(c, rgba(scalec(P.bg, 0.4), 0.85)), cx_fill_rect(c, 0, y, W, h);
    cx_fill_color(c, rgba(P.a, 0.9)), cx_fill_rect(c, 0, y, W, 1.5);
    cx_fill_color(c, rgba(P.a, 1)), cx_fill_rect(c, 0, y, 110 * s, h);
    ufont(c, 12 * s, F_MONO_B), cx_text_baseline(c, CX_MIDDLE), cx_text_align(c, CX_LEFT);
    cx_fill_color(c, cx_rgba(0, 0, 0, 1));
    fill_u8(c, chance(0.02) ? "L I V E" : "LIVE // NWS", 10 * s, y + h / 2);
    if (!HU.has_ticker || HU.tickerX < -HU.tickerW) {
        int idx[NITEMS(NEWS)];
        for (int i = 0; i < NITEMS(NEWS); i++) idx[i] = i;
        for (int i = NITEMS(NEWS) - 1; i > 0; i--) { int j = (int)floor(rnd() * (i + 1)); int tmp = idx[i]; idx[i] = idx[j], idx[j] = tmp; }
        HU.ticker.n = 0;
        int n = 0;
        for (int i = 0; i < 5 && i < NITEMS(NEWS); i++) { if (n++) s_cat(&HU.ticker, "   \xE2\x9C\xA6   "); s_cat(&HU.ticker, NEWS[idx[i]]); }
        const Str *w = CS(messages);
        Str cur;
        cur.n = 0;
        for (int i = 0; i <= w->n; i++) {
            if (i == w->n || w->c[i] == '|') {
                Str tr = s_trim(&cur);
                if (tr.n) { if (n++) s_cat(&HU.ticker, "   \xE2\x9C\xA6   "); s_cats(&HU.ticker, &tr); }
                cur.n = 0;
            } else s_push(&cur, w->c[i]);
        }
        s_cat(&HU.ticker, "   \xE2\x9C\xA6   ");
        ufont(c, 12 * s, F_MONO);
        HU.tickerW = measure_str(c, &HU.ticker);
        HU.tickerX = W;
        HU.has_ticker = true;
    }
    HU.tickerX -= F.dt * 80 * s;
    cx_save(c), cx_begin(c), cx_rect(c, 110 * s, y, W, h), cx_clip(c);
    ufont(c, 12 * s, F_MONO), cx_fill_color(c, rgba(P.text, 0.9));
    Str tk = corrupt(&HU.ticker, 0.005 * F.gi);
    fill_str(c, &tk, HU.tickerX, y + h / 2);
    cx_restore(c);
}

static void hud_rulers(CxCtx *c, Pal P, double s, double W, double Hh) {
    cx_stroke_color(c, rgba(P.b, 0.35)), cx_line_width(c, 1), cx_fill_color(c, rgba(P.b, 0.5));
    ufont(c, 8 * s, F_MONO), cx_text_align(c, CX_LEFT);
    double off = fmod(F.t * 20, 20);
    for (double y = 140 * s; y < Hh - 320 * s; y += 20) {
        double yy = y + off;
        bool big = js_i32(yy / 20) % 5 == 0;
        cx_begin(c), cx_move(c, 8, yy), cx_line(c, big ? 18 : 12, yy), cx_stroke(c);
        if (big) { char b[24]; snprintf(b, sizeof b, "%d", js_i32(yy * 7)); fill_u8(c, b, 22, yy + 3); }
    }
    cx_stroke_color(c, rgba(P.a, 0.4));
    brackets(c, 12, 12, W - 24, Hh - 48 * s, 40 * s);
    cx_fill_color(c, rgba(P.text, 0.35));
    qrish(c, W - 28 * s - 50 * s, 120 * s, 50 * s, 17, (uint32_t)js_i32(F.t * 0.5));
}

static void hud_banner(CxCtx *c, Pal P, double s, double W, double Hh) {
    Glitch *G = F.G;
    if (!G || !G->banner_on) return;
    double a = G->banner_a;
    double h = 70 * s, y = Hh * 0.5 - h / 2;
    cx_save(c);
    cx_fill_color(c, rgba(G->banner_c, 0.18 * a)), cx_fill_rect(c, 0, y, W, h);
    cx_begin(c), cx_rect(c, 0, y, W, 8 * s), cx_rect(c, 0, y + h - 8 * s, W, 8 * s), cx_clip(c);
    cx_fill_color(c, rgba(G->banner_c, 0.9 * a));
    for (double x = -40 + fmod(F.t * 60, 40); x < W; x += 40) {
        cx_begin(c), cx_move(c, x, y), cx_line(c, x + 20, y), cx_line(c, x + 20 + h, y + h), cx_line(c, x + h, y + h), cx_fill(c);
    }
    cx_restore(c);
    ufont(c, 38 * s, F_DISPLAY_900), cx_text_align(c, CX_CENTER), cx_text_baseline(c, CX_MIDDLE);
    Str bt = corrupt(&G->banner_text, 0.08);
    double off = 3 + rnd() * 6;
    glitch_text(c, &bt, W / 2, y + h / 2 - 4 * s, P.a, P.b, off, a);
    if (G->banner_has_sub) { ufont(c, 12 * s, F_MONO), cx_fill_color(c, rgba(P.text, a)), fill_str(c, &G->banner_sub, W / 2, y + h + 14 * s); }
}

static void hud_popups(CxCtx *c, Pal P, double W, double Hh) {
    double s = C(hudScale) / 100;
    if (CB(popups) && C(popupRate) > 0) {
        HU.popT -= F.dt * (C(popupRate) / 50) * (0.5 + F.gi);
        if (HU.popT <= 0) { HU.popT = urand(10, 30); if (HU.npopups < 5) hud_spawn_popup(W, Hh, NULL, NULL, NULL); }
    }
    for (int i = HU.npopups - 1; i >= 0; i--) {
        Popup *p = &HU.popups[i];
        p->t += F.dt;
        if (p->dragging && I.down) p->x = I.x - p->dragOff[0], p->y = I.y - p->dragOff[1];
        else p->dragging = false;
        if (p->closing) {
            p->closing += F.dt * 4;
            if (p->closing >= 1) { memmove(p, p + 1, (size_t)(HU.npopups - i - 1) * sizeof *p), HU.npopups--; continue; }
        }
        if (p->t > 40 && !p->closing) p->closing = 0.001;
    }
    for (int i = 0; i < HU.npopups; i++) draw_popup(c, &HU.popups[i], P, s);
}

static void hud_draw(CxCtx *c, double W, double Hh) {
    Pal P = F.P;
    if (!CB(hud)) { hud_banner(c, P, C(hudScale) / 100, W, Hh); hud_popups(c, P, W, Hh); return; }
    double s = C(hudScale) / 100;
    cx_save(c);
    if (!CB(hudGlitch) && F.G && F.G->hudShake) cx_translate(c, urand(-3, 3) * F.G->hudShake, 0);
    hud_rulers(c, P, s, W, Hh);
    hud_brand(c, P, s);
    if (CB(clock)) hud_clock(c, P, s, W);
    if (CB(logs)) hud_logs(c, P, s, CB(ticker) ? Hh - 24 * s : Hh);
    if (CB(sys)) hud_sysmon(c, P, s, W, CB(ticker) ? Hh - 24 * s : Hh);
    hud_radar(c, P, s, W, Hh);
    if (CB(ticker)) hud_ticker(c, P, s, W, Hh);
    if (CB(reticle)) hud_reticle(c, P, s, W, Hh);
    hud_banner(c, P, s, W, Hh);
    cx_restore(c);
    hud_popups(c, P, W, Hh);
}
#endif
