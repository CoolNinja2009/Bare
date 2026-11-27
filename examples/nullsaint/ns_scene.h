/* ns_scene.h: glyphs.js (glyph rain), saint.js (the watcher), holo.js (billboards,
 * the wireframe artifact), ghosts.js (phantom cursors). */
#ifndef NS_SCENE_H
#define NS_SCENE_H
#include "ns_state.h"

/* ---------- glyph rain ---------- */
typedef struct Col {
    double x, y, sp, depth, corrupt, hue;
    int len;
    uint16_t chars[40];
    bool reverse, has_word;
    Str word;
} Col;
static struct {
    Col cols[400];
    int ncols;
    char key[256];
    const Set *set;
    Set custom;
    double size;
} GL;

static const Set *charset(void) {
    static const char *const ORDER[] = { "katakana", "hex", "binary", "runes", "box", "math", "hangul", "braille", "mixed", "custom" };
    int k = (int)C(glyphSet);
    if (k == 9) {
        GL.custom.n = 0;
        const Str *s = CS(glyphCustom);
        for (int i = 0; i < s->n && GL.custom.n < 400; i++) {
            uint16_t c = s->c[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
            GL.custom.c[GL.custom.n++] = c;
        }
        if (!GL.custom.n) GL.custom.c[0] = '0', GL.custom.c[1] = '1', GL.custom.n = 2;
        return &GL.custom;
    }
    (void)ORDER;
    const Set *sets[9] = { &G_KATAKANA, &G_HEX, &G_BINARY, &G_RUNES, &G_BOX, &G_MATH, &G_HANGUL, &G_BRAILLE, &G_MIXED };
    return sets[k >= 0 && k < 9 ? k : 0];
}
static void glyphs_key(char *out, size_t cap) {
    snprintf(out, cap, "%g|%g|%g|%g|%g|%s|%g", F.W, F.H, C(glyphSize), C(glyphDensity), C(glyphSet), U8(CS(glyphCustom)), F.k);
}
static void new_col(Col *c, double x, bool scatter) {
    int len = randi(6, 28);
    for (int i = 0; i < len; i++) c->chars[i] = GL.set->c[pick_i(GL.set->n)];
    double depth = urand(0.35, 1);
    c->x = floor(x / GL.size + 0.5) * GL.size;
    c->y = scatter ? urand(-F.H, F.H) : urand(-F.H * 0.5, -40);
    double sp = urand(0.5, 1.6);
    c->sp = sp * depth, c->len = len, c->depth = depth;
    c->reverse = chance(0.03);
    c->corrupt = 0, c->has_word = false;
    c->hue = urand(0, 1);
}
static void glyphs_build(void) {
    double size = fmax(6, C(glyphSize) * F.k);
    double density = C(glyphDensity) / 100;
    int n = (int)floor((F.W / size) * density);
    if (n > 400) n = 400;
    GL.set = charset();
    GL.size = size;
    GL.ncols = 0;
    for (int i = 0; i < n; i++) { double x = urand(0, F.W); new_col(&GL.cols[GL.ncols++], x, true); }
    glyphs_key(GL.key, sizeof GL.key);
}
static void glyphs_inject(const Str *word) {
    if (!GL.ncols) return;
    Col *c = &GL.cols[pick_i(GL.ncols)];
    Str w = *word;
    s_upper(&w);
    c->word = s_slice(&w, 0, 24), c->has_word = true;
    if (c->word.n + 2 > c->len) c->len = c->word.n + 2;
    if (c->len > 40) c->len = 40;
}
static void glyphs_draw(void) {
    CxCtx *ctx = F.ctx;
    double W = F.W, H = F.H, dt = F.dt, t = F.t;
    Pal P = F.P;
    if (!CB(glyphs) || C(glyphDensity) <= 0) return;
    char key[256];
    glyphs_key(key, sizeof key);
    if (strcmp(key, GL.key)) glyphs_build();
    double size = GL.size;
    double speed = (C(glyphSpeed) / 50) * 160 * F.speed * (1 + A.bass * 0.8);
    double repel = (C(glyphRepel) / 100) * size * 9;
    double rr = size * 10;
    int gs = (int)C(glyphSet);
    ufont(ctx, size, gs == 0 || gs == 6 ? F_JP : F_MONO);
    cx_text_align(ctx, CX_CENTER), cx_text_baseline(ctx, CX_TOP);
    int mode = (int)C(glyphColor);
    Rgb base = mode == 0 ? P.a : mode == 1 ? P.b : P.c;
    for (int ci = 0; ci < GL.ncols; ci++) {
        Col *c = &GL.cols[ci];
        int dir = c->reverse ? -1 : 1;
        c->y += dir * speed * c->sp * dt;
        if ((dir > 0 && c->y - c->len * size > H) || (dir < 0 && c->y < -size * 2)) {
            double x = urand(0, W);
            new_col(c, x, false);
            if (c->reverse) c->y = H + c->len * size;
            continue;
        }
        if (chance(dt * 6)) { int i = randi(0, c->len - 1); c->chars[i % 40] = GL.set->c[pick_i(GL.set->n)]; }
        if (chance(dt * 0.05 * F.gi)) c->corrupt = 1;
        c->corrupt = fmax(0, c->corrupt - dt * 0.7);
        double alphaD = 0.25 + c->depth * 0.75;
        for (int i = 0; i < c->len; i++) {
            double gy = c->y - i * size * dir;
            if (gy < -size || gy > H + size) continue;
            double gx = c->x;
            double dx = gx - F.mx, dy = gy - F.my, d2 = dx * dx + dy * dy;
            if (repel > 0 && d2 < rr * rr) {
                double d = sqrt(d2) + 1, f = (1 - d / rr);
                gx += (dx / d) * repel * f * f;
            }
            double k = (double)i / c->len;
            double a = (1 - k) * alphaD;
            Rgb col;
            if (i == 0) col = WHITE;
            else if (mode == 3) col = hsl(c->hue + k * 0.3 + t * 0.05, 1, 0.6);
            else if (mode == 4) col = mixc(base, P.warn, clampd(A.spec[(i * 2) % 64] * 2, 0, 1));
            else col = base;
            if (c->corrupt > 0 && chance(0.3)) col = P.warn;
            cx_fill_color(ctx, rgba(col, a));
            Str ch;
            ch.n = 1, ch.c[0] = c->chars[i % c->len % 40];
            if (c->has_word && i < c->word.n) { ch.c[0] = c->word.c[i]; cx_fill_color(ctx, rgba(P.c, 0.95)); }
            if (c->corrupt > 0.5 && chance(0.2)) ch.c[0] = NOISE.c[pick_i(NOISE.n)];
            double jx = c->corrupt > 0 ? urand(-3, 3) * c->corrupt : 0;
            fill_str(ctx, &ch, gx + jx, gy);
        }
        if (c->has_word && c->y - c->len * size > H) c->has_word = false;
    }
}
static void glyphs_burst(double x, double y, double power) {
    (void)y;
    for (int i = 0; i < GL.ncols; i++) {
        Col *c = &GL.cols[i];
        if (fabs(c->x - x) < 200 * power) { c->corrupt = 1; c->y += urand(-60, 60) * power; }
    }
}

/* ---------- the Saint ---------- */
typedef struct Tear { double x, y, vy; Rgb c; } Tear;
static struct {
    double x, y, lx, ly, blink, blinkT, mouth, anger, takeover, glitch, R, nextTalk, wordT;
    double blinkPhase;
    bool has_phase, dbl;
    int form, lastStyle;
    bool has_form;
    bool speaking;
    Str sp_text;
    double sp_t;
    int sp_shown;
    bool sp_zalgo;
    Str queue[3];
    int nqueue;
    Tear tears[64];
    int ntears;
} S = { .blink = 1, .blinkT = 3, .nextTalk = 6, .lastStyle = -1 };

static double g2(double x, double y, double cx, double cy, double sx, double sy) {
    return exp(-(((x - cx) * (x - cx)) / (sx * sx) + ((y - cy) * (y - cy)) / (sy * sy)));
}
static double face_z(double u, double v, double mouth) {
    double r2 = (u * u) / 0.6 + (v * v) / 1.0;
    if (r2 >= 1) return -1;
    double z = sqrt(1 - r2) * 0.55;
    z -= 0.22 * (g2(u, v, -0.3, -0.12, 0.15, 0.1) + g2(u, v, 0.3, -0.12, 0.15, 0.1));
    z += 0.07 * g2(u, v, 0, -0.32, 0.5, 0.06);
    z += 0.28 * g2(u, v, 0, 0.12, 0.07, 0.22) + 0.12 * g2(u, v, 0, 0.3, 0.11, 0.06);
    z += 0.08 * (g2(u, v, -0.42, 0.12, 0.12, 0.15) + g2(u, v, 0.42, 0.12, 0.12, 0.15));
    z -= (0.1 + mouth * 0.25) * g2(u, v, 0, 0.5, 0.24, 0.025 + mouth * 0.07);
    z += 0.07 * g2(u, v, 0, 0.82, 0.2, 0.1);
    return z;
}
static void rot3(double x, double y, double z, double yaw, double pitch, double o[3]) {
    double c = cos(yaw), s = sin(yaw);
    double x1 = x * c + z * s, z1 = -x * s + z * c;
    c = cos(pitch), s = sin(pitch);
    o[0] = x1, o[1] = y * c - z1 * s, o[2] = y * s + z1 * c;
}
static void saint_place(void) {
    double W = F.W, H = F.H, t = F.t, dt = F.dt;
    double tx = W * 0.5, ty = H * 0.44;
    int pos = (int)C(saintPos);
    if (pos == 1) tx = W * 0.27;
    else if (pos == 2) tx = W * 0.73;
    else if (pos == 3) tx = lerp(W * 0.5, F.mx, 0.55), ty = lerp(H * 0.45, F.my, 0.4);
    else if (pos == 4) tx = W * (0.25 + 0.5 * noise1(t * 0.05)), ty = H * (0.3 + 0.3 * noise1(t * 0.04 + 50));
    if (S.takeover > 0) tx = lerp(tx, W / 2, S.takeover), ty = lerp(ty, H / 2, S.takeover);
    if (!S.x) S.x = tx, S.y = ty;
    S.x = damp(S.x, tx, pos == 3 ? 1.2 : 2, dt), S.y = damp(S.y, ty, pos == 3 ? 1.2 : 2, dt);
}
static Str pick_line(void) {
    Str own[64];
    int nown = 0;
    const Str *w = CS(saintWords);
    Str cur;
    cur.n = 0;
    for (int i = 0; i <= w->n; i++) {
        if (i == w->n || w->c[i] == '|') {
            Str tr = s_trim(&cur);
            if (tr.n && nown < 64) own[nown++] = tr;
            cur.n = 0;
        } else s_push(&cur, w->c[i]);
    }
    Str op = CS(operator_)->n ? *CS(operator_) : s_of("OPERATOR");
    s_upper(&op);
    if (!nown && chance(0.12)) {
        struct tm lt;
        ns_time(sp_now_ms(), false, &lt);
        int hr = lt.tm_hour;
        return s_of(hr < 5 ? "WHY ARE YOU AWAKE" : hr < 12 ? "GOOD MORNING, SUBJECT" : hr < 18 ? "THE DAY IS A LIE" : "THE NIGHT LIKES YOU");
    }
    if (nown) return own[pick_i(nown)];
    int n = NITEMS(WHISPERS) + 3, k = pick_i(n);
    if (k < NITEMS(WHISPERS)) return s_of(WHISPERS[k]);
    Str s;
    s.n = 0;
    if (k == NITEMS(WHISPERS)) s_cat(&s, "HELLO, "), s_cats(&s, &op);
    else if (k == NITEMS(WHISPERS) + 1) s_cats(&s, &op), s_cat(&s, ", I REMEMBER YOU");
    else s_cat(&s, "I KNOW YOUR NAME, "), s_cats(&s, &op);
    return s;
}
static Str longest_word(const Str *text) {
    /* text.split(' ').sort((a, b) => b.length - a.length)[0]: a stable sort, so the first longest */
    Str best, cur;
    best.n = 0, cur.n = 0;
    bool first = true;
    for (int i = 0; i <= text->n; i++) {
        if (i == text->n || text->c[i] == ' ') {
            if (first || cur.n > best.n) best = cur;
            first = false;
            cur.n = 0;
        } else s_push(&cur, text->c[i]);
    }
    return best;
}
static void saint_say(const Str *text) {
    if (S.speaking) { if (S.nqueue < 3) S.queue[S.nqueue++] = *text; return; }
    S.speaking = true;
    S.sp_text = *text, s_upper(&S.sp_text);
    S.sp_t = 0, S.sp_shown = 0;
    S.sp_zalgo = chance(0.25);
    if (chance(0.5)) { Str w = longest_word(text); glyphs_inject(w.n ? &w : text); }
    if (S.form == 2) S.wordT = 3.5;
}
static void saint_say_u8(const char *text) { Str s = s_of(text); saint_say(&s); }
static bool saint_poke(double x, double y) {
    double d = dist(x, y, S.x, S.y);
    if (d < S.R * 1.2) {
        static const char *const L[] = { "DO NOT TOUCH ME", "THAT HURT", "AGAIN?", "I FELT THAT", "STOP", "WHY" };
        S.anger = 1, S.glitch = 1;
        saint_say_u8(PICK(L));
        return true;
    }
    return false;
}
static void saint_update(void) {
    double dt = F.dt, t = F.t, W = F.W, H = F.H;
    saint_place();
    double track = C(saintTrack) / 100;
    double tx = clampd((F.mx - S.x) / (W * 0.5), -1, 1) * track, ty = clampd((F.my - S.y) / (H * 0.5), -1, 1) * track;
    bool idle = I.idle > 4;
    double wx = idle ? (noise1(t * 0.3) - 0.5) * 1.2 : tx, wy = idle ? (noise1(t * 0.27 + 9) - 0.5) * 0.8 : ty;
    S.lx = damp(S.lx, wx, 5, dt), S.ly = damp(S.ly, wy, 5, dt);
    if (CB(saintBlink)) {
        S.blinkT -= dt;
        if (S.blinkT <= 0) { S.blinkT = urand(2, 7); S.blinkPhase = 0, S.has_phase = true; if (chance(0.2)) S.dbl = true; }
        if (S.has_phase) {
            S.blinkPhase += dt * 7;
            S.blink = fabs(cos(fmin(S.blinkPhase, PI)));
            if (S.blinkPhase >= PI) {
                if (S.dbl) S.blinkPhase = -0.6;
                else S.has_phase = false;
                S.dbl = false, S.blink = 1;
            }
            if (S.has_phase && S.blinkPhase < 0) S.blink = 1;
        }
    } else S.blink = 1;
    S.anger = fmax(0, S.anger - dt * 0.25);
    S.glitch = fmax(0, S.glitch - dt * 1.5);
    if (I.speed > 2500 * F.k) S.anger = fmin(1, S.anger + dt * 1.2);
    if (CB(saintTalk)) {
        S.nextTalk -= dt;
        if (S.nextTalk <= 0 && !S.speaking) {
            S.nextTalk = urand(9, 22) / (0.5 + C(chaos) / 100);
            Str l = pick_line();
            saint_say(&l);
        }
    }
    if (S.speaking) {
        S.sp_t += dt;
        int shown = (int)fmin(S.sp_text.n, floor(S.sp_t * 18));
        S.sp_shown = shown;
        bool talking = shown < S.sp_text.n;
        S.mouth = damp(S.mouth, talking ? 0.4 + 0.6 * fabs(sin(t * 22)) : A.bass * 0.6, 18, dt);
        if (S.sp_t > S.sp_text.n / 18.0 + 3.5) {
            S.speaking = false;
            if (S.nqueue) {
                Str q = S.queue[0];
                memmove(&S.queue[0], &S.queue[1], (size_t)(S.nqueue - 1) * sizeof S.queue[0]);
                S.nqueue--;
                saint_say(&q);
            }
        }
    } else S.mouth = damp(S.mouth, A.bass * 0.7, 12, dt);
}

static void eye_glow(double x, double y, double r, double alpha) {
    CxCtx *ctx = F.ctx;
    double open = S.blink;
    Rgb c = S.anger > 0.4 ? F.P.warn : F.P.c;
    cx_save(ctx), cx_op(ctx, CX_LIGHTER);
    CxGrad g = cx_radial(x, y, 0, x, y, r * 4);
    cx_stop(&g, 0, rgba(c, 0.55 * alpha * open)), cx_stop(&g, 1, rgba(c, 0));
    cx_fill_grad(ctx, &g), cx_fill_rect(ctx, x - r * 4, y - r * 4, r * 8, r * 8);
    cx_restore(ctx);
    cx_fill_color(ctx, rgba(WHITE, alpha));
    cx_begin(ctx), cx_ellipse(ctx, x, y, r * 1.1, fmax(0.5, r * 0.75 * open), 0, 0, TAU, false), cx_fill(ctx);
    cx_fill_color(ctx, cx_rgba(0, 0, 0, 1));
    cx_begin(ctx), cx_ellipse(ctx, x + S.lx * r * 0.4, y + S.ly * r * 0.3, r * (0.32 + A.bass * 0.2), fmax(0.3, r * 0.4 * open), 0, 0, TAU, false), cx_fill(ctx);
}

static void draw_face(double R, double alpha) {
    CxCtx *ctx = F.ctx;
    Pal P = F.P;
    double t = F.t;
    double yaw = S.lx * 0.55, pitch = -S.ly * 0.35;
    const int lines = 44, steps = 64;
    Rgb lineCol = S.anger > 0.4 ? mixc(P.a, P.warn, S.anger) : P.a;
    Rgb shadeC = scalec(P.bg, 0.5);
    cx_line_width(ctx, fmax(1, R * 0.006));
    cx_line_join(ctx, CX_ROUND);
    static double pts[80][2];
    for (int li = 0; li < lines; li++) {
        double v = -1.02 + (2.04 * li) / (lines - 1);
        double jitter = 0;
        if (S.glitch > 0 || chance(0.01 * F.gi)) {
            if (chance(0.25)) jitter = urand(-0.3, 0.3) * R * (S.glitch + 0.2);
        }
        int np = 0;
        bool started = false;
        for (int si = 0; si <= steps; si++) {
            double u = -0.85 + (1.7 * si) / steps;
            double z = face_z(u, v, S.mouth);
            if (z < 0) { if (started) break; continue; }
            z += A.spec[(si + li) % 64] * 0.04 + 0.01 * sin(t * 3 + li * 0.5 + si * 0.3) * F.gi;
            double p[3];
            rot3(u * R, v * R, z * R, yaw, pitch, p);
            pts[np][0] = S.x + p[0] + jitter, pts[np][1] = S.y + p[1] - p[2] * 0.12, np++;
            started = true;
        }
        if (np < 2) continue;
        cx_begin(ctx);
        cx_move(ctx, pts[0][0], pts[0][1]);
        for (int i = 0; i < np; i++) cx_line(ctx, pts[i][0], pts[i][1]);
        double drop = (2.2 * R) / lines;
        for (int i = np - 1; i >= 0; i--) cx_line(ctx, pts[i][0], pts[i][1] + drop);
        cx_close(ctx);
        cx_fill_color(ctx, rgba(shadeC, 0.82 * alpha)), cx_fill(ctx);
        cx_begin(ctx);
        cx_move(ctx, pts[0][0], pts[0][1]);
        for (int i = 0; i < np; i++) cx_line(ctx, pts[i][0], pts[i][1]);
        double k = (double)li / lines;
        cx_stroke_color(ctx, rgba(mixc(lineCol, P.b, k * 0.6), alpha * (0.55 + 0.45 * sin(k * PI))));
        cx_stroke(ctx);
    }
    for (int side = -1; side <= 1; side += 2) {
        double z = face_z(side * 0.3, -0.12, 0) + 0.05;
        double p[3];
        rot3(side * 0.3 * R, -0.12 * R, z * R, yaw, pitch, p);
        double ex = S.x + p[0] + S.lx * R * 0.05, ey = S.y + p[1] - p[2] * 0.12 + S.ly * R * 0.03;
        eye_glow(ex, ey, R * 0.07, alpha);
    }
    if (S.mouth > 0.15) {
        double p[3];
        rot3(0, 0.5 * R, (face_z(0, 0.5, S.mouth) + 0.02) * R, yaw, pitch, p);
        cx_fill_color(ctx, rgba(P.c, 0.5 * S.mouth * alpha));
        cx_begin(ctx), cx_ellipse(ctx, S.x + p[0], S.y + p[1] - p[2] * 0.12, R * 0.18, R * 0.04 * S.mouth + 1, 0, 0, TAU, false), cx_fill(ctx);
    }
    const int n = 28;
    ufont(ctx, R * 0.06, F_JP), cx_text_align(ctx, CX_CENTER), cx_text_baseline(ctx, CX_MIDDLE);
    for (int i = 0; i < n; i++) {
        double a = ((double)i / n) * TAU + t * 0.15;
        double hx = S.x + cos(a) * R * 1.05, hy = S.y - R * 0.95 + sin(a) * R * 0.18;
        cx_fill_color(ctx, rgba(P.c, (0.25 + 0.45 * (sin(a) * 0.5 + 0.5)) * alpha));
        Str ch;
        ch.n = 1, ch.c[0] = G_KATAKANA.c[(i * 7 + (int)floor(t * 2)) % 60];
        fill_str(ctx, &ch, hx, hy);
    }
}

static void eye_path(CxCtx *ctx, double cx, double cy, double w, double h) {
    cx_move(ctx, cx - w, cy);
    cx_bezier(ctx, cx - w * 0.5, cy - h * 1.35, cx + w * 0.5, cy - h * 1.35, cx + w, cy);
    cx_bezier(ctx, cx + w * 0.5, cy + h * 1.35, cx - w * 0.5, cy + h * 1.35, cx - w, cy);
    cx_close(ctx);
}
static void draw_eye(double R, double alpha) {
    CxCtx *ctx = F.ctx;
    Pal P = F.P;
    double t = F.t;
    double W2 = R * 1.5, H2 = R * 0.75 * S.blink;
    double cx = S.x, cy = S.y;
    Rgb ang = S.anger > 0.4 ? P.warn : P.a;
    for (int i = 4; i >= 0; i--) {
        double k = 1 + i * 0.09 + sin(t * 2 + i) * 0.01;
        cx_stroke_color(ctx, rgba(i ? P.b : ang, (i ? 0.15 : 0.9) * alpha));
        cx_line_width(ctx, i ? 1 : fmax(2, R * 0.015));
        double off = 0;
        if (i && F.gi > 0 && chance(0.05)) off = urand(-20, 20);
        cx_begin(ctx), eye_path(ctx, cx + off, cy, W2 * k, fmax(1, H2 * k)), cx_stroke(ctx);
    }
    cx_save(ctx);
    cx_begin(ctx), eye_path(ctx, cx, cy, W2, fmax(1, H2)), cx_clip(ctx);
    CxGrad sg = cx_radial(cx, cy, R * 0.2, cx, cy, W2);
    cx_stop(&sg, 0, rgba(mixc(P.text, WHITE, 0.5), 0.92 * alpha)), cx_stop(&sg, 1, rgba(mixc(P.bg2, P.a, 0.4), 0.95 * alpha));
    cx_fill_grad(ctx, &sg), cx_fill_rect(ctx, cx - W2, cy - R, W2 * 2, R * 2);
    Rng vr = rng_new(777);
    cx_stroke_color(ctx, rgba(P.warn, 0.35 * alpha)), cx_line_width(ctx, 1);
    for (int i = 0; i < 14; i++) {
        double x = cx + (rng(&vr) > 0.5 ? 1 : -1) * W2, y = cy + rng_range(&vr, -H2, H2);
        cx_begin(ctx), cx_move(ctx, x, y);
        for (int k = 0; k < 6; k++) {
            if (rng(&vr) > 0.5) x += (cx - x) * 0.25;
            else y += rng_range(&vr, -R * 0.08, R * 0.08);
            cx_line(ctx, x, y);
        }
        cx_stroke(ctx);
    }
    double ix = cx + S.lx * R * 0.55, iy = cy + S.ly * R * 0.28, IR = R * 0.48;
    CxGrad ig = cx_radial(ix, iy, IR * 0.2, ix, iy, IR);
    cx_stop(&ig, 0, rgba(P.c, alpha)), cx_stop(&ig, 0.55, rgba(ang, alpha)), cx_stop(&ig, 1, rgba(scalec(P.bg, 0.5), alpha));
    cx_fill_grad(ctx, &ig), cx_begin(ctx), cx_arc(ctx, ix, iy, IR, 0, TAU, false), cx_fill(ctx);
    for (int ring = 0; ring < 3; ring++) {
        double rr = IR * (0.55 + ring * 0.15);
        int segs = 12 + ring * 8, dir = ring % 2 ? -1 : 1;
        cx_stroke_color(ctx, rgba(ring == 1 ? P.b : P.text, 0.5 * alpha)), cx_line_width(ctx, fmax(1, R * 0.008));
        for (int s = 0; s < segs; s++) {
            if ((s + ring) % 3 == 0) continue;
            double a0 = ((double)s / segs) * TAU + t * 0.4 * dir * (1 + ring * 0.3), a1 = a0 + (TAU / segs) * 0.6;
            cx_begin(ctx), cx_arc(ctx, ix, iy, rr, a0, a1, false), cx_stroke(ctx);
        }
    }
    cx_stroke_color(ctx, rgba(P.text, 0.6 * alpha));
    for (int s = 0; s < 60; s++) {
        double a = (s / 60.0) * TAU - t * 0.1, r0 = IR * 0.92, r1 = IR * (s % 5 ? 0.97 : 1.02);
        cx_begin(ctx), cx_move(ctx, ix + cos(a) * r0, iy + sin(a) * r0), cx_line(ctx, ix + cos(a) * r1, iy + sin(a) * r1), cx_stroke(ctx);
    }
    double nearv = clampd(1 - dist(F.mx, F.my, cx, cy) / (R * 3), 0, 1);
    double PR = IR * (0.28 + A.bass * 0.25 + nearv * 0.15 - S.anger * 0.12);
    cx_fill_color(ctx, cx_rgba(0, 0, 0, 1)), cx_begin(ctx);
    if (S.anger > 0.5) cx_ellipse(ctx, ix, iy, PR * 0.35, PR * 1.3, 0, 0, TAU, false);
    else cx_arc(ctx, ix, iy, PR, 0, TAU, false);
    cx_fill(ctx);
    cx_fill_color(ctx, rgba(WHITE, 0.8 * alpha)), cx_fill_rect(ctx, ix - PR * 0.6, iy - PR * 0.7, PR * 0.35, PR * 0.25);
    cx_fill_color(ctx, rgba(P.b, 0.9 * alpha)), cx_fill_rect(ctx, ix + (F.mx - cx) * 0.02 - 2, iy + (F.my - cy) * 0.02 - 2, 4, 4);
    CxGrad lg = cx_linear(0, cy - H2, 0, cy + H2);
    cx_stop(&lg, 0, cx_rgba(0, 0, 0, 0.55)), cx_stop(&lg, 0.3, cx_rgba(0, 0, 0, 0)), cx_stop(&lg, 0.8, cx_rgba(0, 0, 0, 0)), cx_stop(&lg, 1, cx_rgba(0, 0, 0, 0.4));
    cx_fill_grad(ctx, &lg), cx_fill_rect(ctx, cx - W2, cy - H2, W2 * 2, H2 * 2);
    double sy = cy - H2 + fmod(t * 0.6, 1) * H2 * 2;
    cx_fill_color(ctx, rgba(P.b, 0.25 * alpha)), cx_fill_rect(ctx, cx - W2, sy, W2 * 2, 2);
    cx_restore(ctx);
    if (chance(F.dt * 0.8) && S.ntears < 64) {
        Tear *d = &S.tears[S.ntears];
        d->x = cx + urand(-R * 0.4, R * 0.4);
        d->y = cy + H2 * 0.8, d->vy = 0;
        d->c = chance(0.5) ? P.warn : P.b;
        S.ntears++;
    }
    for (int i = S.ntears - 1; i >= 0; i--) {
        Tear *d = &S.tears[i];
        d->vy += 400 * F.dt, d->y += d->vy * F.dt;
        if (d->y > F.H) { memmove(&S.tears[i], &S.tears[i + 1], (size_t)(S.ntears - i - 1) * sizeof *d); S.ntears--; continue; }
        cx_fill_color(ctx, rgba(d->c, 0.8 * alpha)), cx_fill_rect(ctx, d->x, d->y, 3, 6);
    }
}

static void draw_speech(double R, double alpha) {
    if (!S.speaking) return;
    CxCtx *ctx = F.ctx;
    Pal P = F.P;
    double W = F.W;
    Str txt = s_slice(&S.sp_text, 0, S.sp_shown);
    Str shown = corrupt(&txt, 0.04 + S.glitch * 0.3);
    if (S.sp_zalgo) shown = zalgo(&shown, 2);
    double size = fmax(12, R * 0.09);
    ufont(ctx, size, F_DISPLAY_700);
    double tw = measure_str(ctx, &S.sp_text);
    double bx = S.x + R * 0.95, by = S.y - R * 0.55;
    if (bx + tw + size * 2 > W) bx = S.x - R * 0.95 - tw - size * 2;
    double bw = tw + size * 1.6, bh = size * 2;
    cx_fill_color(ctx, rgba(scalec(P.bg, 0.5), 0.8 * alpha));
    chamfer(ctx, bx, by, bw, bh, size * 0.5), cx_fill(ctx);
    cx_stroke_color(ctx, rgba(S.anger > 0.4 ? P.warn : P.a, 0.9 * alpha)), cx_line_width(ctx, 1.5), cx_stroke(ctx);
    cx_begin(ctx), cx_move(ctx, bx < S.x ? bx + bw : bx, by + bh / 2), cx_line(ctx, S.x + (bx < S.x ? -R * 0.2 : R * 0.2), S.y + R * 0.45), cx_stroke(ctx);
    cx_text_align(ctx, CX_LEFT), cx_text_baseline(ctx, CX_MIDDLE);
    glitch_text(ctx, &shown, bx + size * 0.8, by + bh / 2, P.a, P.b, 1 + S.glitch * 4, alpha);
    ufont(ctx, size * 0.45, F_MONO);
    cx_fill_color(ctx, rgba(P.c, 0.8 * alpha));
    Str tag = s_of("SAINT://"), h = uhex(S.sp_text.n * 4099 + 0xbeef, 4);
    s_cats(&tag, &h);
    fill_str(ctx, &tag, bx + size * 0.6, by - size * 0.35);
}

static void saint_draw(void) {
    double W = F.W, H = F.H;
    if (!CB(saint)) return;
    int style = (int)C(saintStyle);
    if (style == 3) { if (!S.has_form || S.lastStyle != 3) S.form = randi(0, 2), S.has_form = true; }
    else S.form = style, S.has_form = true;
    S.lastStyle = style;
    saint_update();
    double R = fmin(W, H) * (0.12 + (C(saintSize) / 100) * 0.3) * (1 + S.takeover * 1.4) * (1 + A.bass * 0.05);
    S.R = R;
    double alpha = clampd(C(saintAlpha) / 100 + S.takeover, 0, 1);
    CxCtx *ctx = F.ctx;
    cx_save(ctx);
    if (S.glitch > 0.2 && chance(0.4)) cx_translate(ctx, urand(-10, 10) * S.glitch, 0);
    if (S.form == 0) draw_face(R, alpha);
    else if (S.form == 1) draw_eye(R, alpha);
    else draw_face(R, alpha);   /* the particle swarm: drawn as the face (not ported) */
    cx_restore(ctx);
    draw_speech(R, alpha);
}
static void saint_reroll(void) { if (C(saintStyle) == 3) S.form = randi(0, 2); }

/* ---------- holograms ---------- */
typedef struct Board { double x, y, w, h, skew, depth, ph, swap, t, glitch; Ad ad; int c, kind, mesh; } Board;
static struct {
    Board boards[12];
    int nboards;
    char key[256];
    double rx, ry, rz, ax;
} HO;

static void holo_key(char *out, size_t cap) { snprintf(out, cap, "%g|%g|%g|%s", F.W, F.H, C(adCount), U8(CS(seed))); }
static void holo_build(void) {
    Str sd = *CS(seed);
    s_cat(&sd, "ads");
    Rng r = rng_new(str_seed(&sd));
    int n = (int)C(adCount);
    HO.nboards = 0;
    double W = F.W, H = F.H;
    for (int i = 0; i < n && i < 12; i++) {
        Board *b = &HO.boards[HO.nboards++];
        double w = W * rng_range(&r, 0.07, 0.15), h = w * rng_range(&r, 0.45, 0.9);
        b->x = W * ((i + 0.5) / n) + rng_range(&r, -W * 0.04, W * 0.04);
        b->y = H * rng_range(&r, 0.32, 0.62);
        b->w = w, b->h = h;
        b->skew = rng_range(&r, -0.25, 0.25);
        b->depth = rng_range(&r, 0.3, 0.7);
        b->ad = ad_line();
        b->c = rng_pick(&r, 3);
        b->kind = rng_int(&r, 0, 4);
        b->ph = rng(&r) * 100;
        b->swap = rng_range(&r, 6, 14);
        b->t = 0, b->glitch = 0;
        b->mesh = rng_pick(&r, 5);
    }
    holo_key(HO.key, sizeof HO.key);
}
static Rgb pal_abc(int i) { return i == 0 ? F.P.a : i == 1 ? F.P.b : F.P.c; }

static void board_content(Board *b, double x, double y, double w, double h, Rgb col) {
    CxCtx *ctx = F.ctx;
    Pal P = F.P;
    double t = F.t;
    double fl = b->glitch > 0 ? (chance(0.5) ? 0.2 : 1) : 0.85 + 0.15 * sin(t * 40 + b->ph);
    cx_save(ctx);
    cx_begin(ctx), cx_rect(ctx, x, y, w, h), cx_clip(ctx);
    CxGrad g = cx_linear(x, y, x, y + h);
    cx_stop(&g, 0, rgba(col, 0.18 * fl)), cx_stop(&g, 1, rgba(col, 0.05 * fl));
    cx_fill_grad(ctx, &g), cx_fill_rect(ctx, x, y, w, h);
    cx_text_align(ctx, CX_CENTER), cx_text_baseline(ctx, CX_MIDDLE);
    double cx = x + w / 2, cy = y + h / 2;
    if (b->kind == 0) {
        ufont(ctx, h * 0.2, F_DISPLAY_700), cx_fill_color(ctx, rgba(col, fl));
        Str br = corrupt(&b->ad.brand, b->glitch * 0.4);
        fill_str(ctx, &br, cx, y + h * 0.25);
        ufont(ctx, h * 0.13, F_DISPLAY), cx_fill_color(ctx, rgba(P.text, 0.9 * fl));
        fill_str(ctx, &b->ad.product, cx, cy + h * 0.05);
        ufont(ctx, h * 0.09, F_MONO), cx_fill_color(ctx, rgba(P.c, 0.8 * fl));
        fill_str(ctx, &b->ad.tag, cx, y + h * 0.8);
    } else if (b->kind == 1) {
        cx_stroke_color(ctx, rgba(col, 0.9 * fl)), cx_line_width(ctx, 1.5);
        static double pts[128][3];
        draw_mesh(ctx, &MESH[b->mesh], t * 0.7 + b->ph, t * 1.1, 0, cx - w * 0.2, cy, h * 0.3, b->glitch * 10, pts);
        ufont(ctx, h * 0.15, F_DISPLAY_700), cx_fill_color(ctx, rgba(P.text, fl)), cx_text_align(ctx, CX_LEFT);
        fill_str(ctx, &b->ad.brand, cx, cy - h * 0.12);
        ufont(ctx, h * 0.08, F_MONO), cx_fill_color(ctx, rgba(P.c, fl));
        char buf[64];
        snprintf(buf, sizeof buf, "\xC2\xA5%d.99", js_i32(uhash(b->ph, 0) * 99999));
        fill_u8(ctx, buf, cx, cy + h * 0.12);
    } else if (b->kind == 2) {
        ufont(ctx, h * 0.62, F_JP_B), cx_fill_color(ctx, rgba(col, fl));
        fill_u8(ctx, SIGNS[js_i32(b->ph) % NITEMS(SIGNS)], cx, cy - h * 0.06);
        ufont(ctx, h * 0.11, F_MONO), cx_fill_color(ctx, rgba(P.text, 0.9 * fl));
        Str one = s_of("  ");
        s_cats(&one, &b->ad.tag), s_cat(&one, "  //  "), s_cats(&one, &b->ad.brand);
        Str cap;
        cap.n = 0;
        for (int k = 0; k < 3; k++) s_cats(&cap, &one);
        double off = fmod(t * 40, measure_str(ctx, &cap) / 3);
        cx_text_align(ctx, CX_LEFT), fill_str(ctx, &cap, x - off, y + h * 0.88);
    } else if (b->kind == 3) {
        cx_stroke_color(ctx, rgba(col, fl)), cx_line_width(ctx, 1);
        for (int i = 0; i < 12; i++) {
            double yy = y + h * 0.15 + i * h * 0.055;
            double ww = w * 0.22 * sqrt(fmax(0, 1 - pow((i - 6) / 6.5, 2)));
            cx_begin(ctx), cx_move(ctx, cx - w * 0.25 - ww, yy), cx_line(ctx, cx - w * 0.25 + ww, yy), cx_stroke(ctx);
        }
        ufont(ctx, h * 0.13, F_DISPLAY_700), cx_fill_color(ctx, rgba(P.warn, fl)), cx_text_align(ctx, CX_LEFT);
        fill_u8(ctx, "WANTED", cx - w * 0.02, cy - h * 0.15);
        ufont(ctx, h * 0.08, F_MONO), cx_fill_color(ctx, rgba(P.text, fl));
        static const char *const FOR[] = { "DREAMING", "BEING", "BLINKING", "REMEMBERING" };
        char buf[64];
        snprintf(buf, sizeof buf, "FOR: %s", PICK(FOR));
        fill_u8(ctx, buf, cx - w * 0.02, cy + h * 0.05);
        fill_u8(ctx, "REWARD: YOUR NAME", cx - w * 0.02, cy + h * 0.2);
    } else {
        cx_fill_color(ctx, rgba(col, fl));
        const int bars = 24;
        for (int i = 0; i < bars; i++) {
            double v = fmax(A.spec[i * 2], 0.05 + 0.05 * sin(t * 3 + i));
            double bh = v * h * 0.7;
            cx_fill_rect(ctx, x + ((double)i / bars) * w + 1, y + h * 0.85 - bh, w / bars - 2, bh);
        }
        ufont(ctx, h * 0.1, F_MONO), cx_fill_color(ctx, rgba(P.text, fl));
        fill_u8(ctx, "NOW PLAYING: YOUR THOUGHTS", cx, y + h * 0.1);
    }
    cx_fill_color(ctx, cx_rgba(0, 0, 0, 0.25));
    for (double yy = y; yy < y + h; yy += 3) cx_fill_rect(ctx, x, yy, w, 1);
    cx_restore(ctx);
    cx_stroke_color(ctx, rgba(col, 0.8 * fl)), cx_line_width(ctx, 1.5);
    brackets(ctx, x - 4, y - 4, w + 8, h + 8, fmin(w, h) * 0.15);
}

static void holo_boards(void) {
    CxCtx *ctx = F.ctx;
    double dt = F.dt;
    if (!CB(ads) || C(adCount) <= 0) return;
    char key[256];
    holo_key(key, sizeof key);
    if (strcmp(key, HO.key)) holo_build();
    for (int i = 0; i < HO.nboards; i++) {
        Board *b = &HO.boards[i];
        b->t += dt;
        if (b->t > b->swap) { b->t = 0, b->glitch = 0.6; b->ad = ad_line(); b->kind = randi(0, 4); b->mesh = pick_i(5); }
        if (chance(dt * 0.1 * F.gi)) b->glitch = 0.4;
        b->glitch = fmax(0, b->glitch - dt);
        Rgb col = pal_abc(b->c);
        double x = b->x - F.par * b->depth, y = b->y - F.parY * b->depth * 0.4;
        cx_save(ctx);
        cx_translate(ctx, x, y);
        cx_transform(ctx, 1, b->skew * 0.3, 0, 1, 0, 0);
        if (b->glitch > 0 && chance(0.5)) cx_translate(ctx, urand(-8, 8), 0);
        cx_save(ctx), cx_op(ctx, CX_LIGHTER);
        CxGrad pg = cx_linear(0, b->h * 0.5, 0, b->h * 1.8);
        cx_stop(&pg, 0, rgba(col, 0.1)), cx_stop(&pg, 1, rgba(col, 0));
        cx_fill_grad(ctx, &pg);
        cx_begin(ctx), cx_move(ctx, -b->w / 2, b->h / 2), cx_line(ctx, b->w / 2, b->h / 2), cx_line(ctx, b->w * 0.05, b->h * 1.8), cx_line(ctx, -b->w * 0.05, b->h * 1.8), cx_fill(ctx);
        cx_restore(ctx);
        board_content(b, -b->w / 2, -b->h / 2, b->w, b->h, col);
        cx_restore(ctx);
    }
}

static void holo_artifact(void) {
    CxCtx *ctx = F.ctx;
    Pal P = F.P;
    double t = F.t, dt = F.dt, W = F.W, H = F.H;
    if (!CB(holo)) return;
    double sp = 0.3 + A.mid * 2;
    HO.rx += dt * 0.37 * sp, HO.ry += dt * 0.53 * sp, HO.rz += dt * 0.11 * sp;
    int shape = (int)C(holoShape);
    if (shape == 5) shape = (int)floor(t / 12);
    shape = ((shape % 5) + 5) % 5;
    const Mesh *mesh = &MESH[shape];
    double Sz = fmin(W, H) * (0.05 + C(holoSize) / 100 * 0.18) * (1 + A.bass * 0.25);
    double saintX = CB(saint) ? S.x : W / 2;
    double tx = saintX < W / 2 ? W * 0.8 : W * 0.2;
    HO.ax = HO.ax ? damp(HO.ax, tx, 0.5, dt) : tx;
    double cx = HO.ax - F.par * 0.5, cy = H * 0.24 + sin(t * 0.6) * H * 0.03 - F.parY * 0.2;
    cx_save(ctx);
    cx_op(ctx, CX_LIGHTER);
    static double pts[128][3];
    for (int e = 3; e >= 0; e--) {
        cx_stroke_color(ctx, rgba(e ? P.b : P.a, e ? 0.12 : 0.9));
        cx_line_width(ctx, e ? 1 : 1.6);
        int n = draw_mesh(ctx, mesh, HO.rx - e * 0.08, HO.ry - e * 0.12, HO.rz, cx, cy, Sz, F.gi * 6, pts);
        if (!e) {
            cx_fill_color(ctx, rgba(P.c, 0.9));
            for (int i = 0; i < n; i++) cx_fill_rect(ctx, pts[i][0] - 1.5, pts[i][1] - 1.5, 3, 3);
            ufont(ctx, 10, F_MONO), cx_fill_color(ctx, rgba(P.text, 0.6));
            int stepn = (int)(n / 4) > 1 ? (int)(n / 4) : 1;
            for (int i = 0; i < n; i += stepn) {
                char z[32], buf[64];
                js_fixed(z, sizeof z, pts[i][2], 2);
                snprintf(buf, sizeof buf, "v%d:%s", i, z);
                fill_u8(ctx, buf, pts[i][0] + 5, pts[i][1] - 5);
            }
        }
    }
    cx_stroke_color(ctx, rgba(P.b, 0.35)), cx_line_width(ctx, 1);
    cx_begin(ctx), cx_ellipse(ctx, cx, cy + Sz * 1.25, Sz * 1.1, Sz * 0.22, 0, 0, TAU, false), cx_stroke(ctx);
    cx_begin(ctx), cx_ellipse(ctx, cx, cy + Sz * 1.25, Sz * 0.7, Sz * 0.14, 0, t, t + 4, false), cx_stroke(ctx);
    CxGrad pg = cx_linear(0, cy + Sz * 1.25, 0, cy - Sz);
    cx_stop(&pg, 0, rgba(P.b, 0.12)), cx_stop(&pg, 1, rgba(P.b, 0));
    cx_fill_grad(ctx, &pg);
    cx_begin(ctx), cx_move(ctx, cx - Sz * 0.15, cy + Sz * 1.25), cx_line(ctx, cx + Sz * 0.15, cy + Sz * 1.25), cx_line(ctx, cx + Sz * 1.1, cy - Sz), cx_line(ctx, cx - Sz * 1.1, cy - Sz), cx_fill(ctx);
    cx_restore(ctx);
    ufont(ctx, 11, F_MONO), cx_fill_color(ctx, rgba(P.c, 0.75)), cx_text_align(ctx, CX_CENTER);
    char buf[96], up[16];
    snprintf(up, sizeof up, "%s", MESH_NAMES[shape]);
    for (char *p = up; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    snprintf(buf, sizeof buf, "ARTIFACT_%s // ORIGIN UNKNOWN", up);
    fill_u8(ctx, buf, cx, cy + Sz * 1.6);
}

/* ---------- phantom cursors ---------- */
typedef struct Rec { double x, y, t; } Rec;
typedef struct GClick { double x, y, t; bool done; } GClick;
typedef struct Ghost { Rec seg[200]; int nseg; GClick clicks[32]; int nclicks; double p, t0, ago; bool mirror; int hue; double trail[24][2]; int ntrail; } Ghost;
static struct {
    Rec rec[2000];
    int nrec;
    Rec clicks[256];
    int nclicks;
    double recT, next;
    Ghost list[6];
    int nlist;
} GH = { .next = 25 };

static void ghosts_note_click(double x, double y, double t) {
    if (GH.nclicks < 256) GH.clicks[GH.nclicks].x = x, GH.clicks[GH.nclicks].y = y, GH.clicks[GH.nclicks].t = t, GH.nclicks++;
}
static void ghosts_record(void) {
    GH.recT -= F.dt / fmax(0.1, F.speed);
    if (GH.recT > 0) return;
    GH.recT = 1.0 / 20;
    if (GH.nrec < 2000) GH.rec[GH.nrec].x = I.x / F.cssW, GH.rec[GH.nrec].y = I.y / F.cssH, GH.rec[GH.nrec].t = F.t, GH.nrec++;
    int drop = 0;
    while (drop < GH.nrec && F.t - GH.rec[drop].t > 90) drop++;
    if (drop) memmove(GH.rec, GH.rec + drop, (size_t)(GH.nrec - drop) * sizeof *GH.rec), GH.nrec -= drop;
    drop = 0;
    while (drop < GH.nclicks && F.t - GH.clicks[drop].t > 90) drop++;
    if (drop) memmove(GH.clicks, GH.clicks + drop, (size_t)(GH.nclicks - drop) * sizeof *GH.clicks), GH.nclicks -= drop;
}
static void ghosts_spawn(void) {
    if (GH.nrec < 120) return;
    for (int tries = 0; tries < 8; tries++) {
        int len = randi(80, 200);
        int start = randi(0, fmax(0, GH.nrec - len - 40));
        int end = start + len > GH.nrec ? GH.nrec : start + len;
        double moved = 0;
        for (int i = start + 1; i < end; i++) moved += hypot(GH.rec[i].x - GH.rec[i - 1].x, GH.rec[i].y - GH.rec[i - 1].y);
        if (moved < 0.4) continue;
        Ghost *g = &GH.list[GH.nlist];
        g->nseg = end - start;
        memcpy(g->seg, GH.rec + start, (size_t)g->nseg * sizeof *g->seg);
        double t0 = g->seg[0].t, t1 = g->seg[g->nseg - 1].t;
        g->nclicks = 0;
        for (int i = 0; i < GH.nclicks && g->nclicks < 32; i++)
            if (GH.clicks[i].t >= t0 && GH.clicks[i].t <= t1) {
                GClick *c = &g->clicks[g->nclicks++];
                c->x = GH.clicks[i].x, c->y = GH.clicks[i].y, c->t = GH.clicks[i].t, c->done = false;
            }
        g->p = 0, g->t0 = t0, g->ago = F.t - t0;
        g->mirror = chance(0.3);
        g->hue = pick_i(3);
        g->ntrail = 0;
        GH.nlist++;
        return;
    }
}
static void ghost_arrow(CxCtx *ctx, double x, double y, double s, Rgb col, double a) {
    cx_save(ctx), cx_translate(ctx, x, y), cx_scale(ctx, s, s);
    cx_begin(ctx), cx_move(ctx, 0, 0), cx_line(ctx, 0, 17), cx_line(ctx, 4.5, 13), cx_line(ctx, 7.5, 20), cx_line(ctx, 10, 19), cx_line(ctx, 7, 12), cx_line(ctx, 12.5, 12), cx_close(ctx);
    cx_fill_color(ctx, cx_rgba(0, 0, 0, 0.6 * a)), cx_fill(ctx);
    cx_stroke_color(ctx, rgba(col, a)), cx_line_width(ctx, 1.4), cx_stroke(ctx);
    cx_restore(ctx);
}
static void ghosts_draw(void) {
    ghosts_record();
    if (!CB(ghosts)) { GH.nlist = 0; return; }
    GH.next -= F.dt;
    if (GH.next <= 0) { GH.next = urand(18, 45) / (0.4 + C(chaos) / 100); if (GH.nlist < C(ghostCount) && GH.nlist < 6) ghosts_spawn(); }
    CxCtx *ctx = F.ctx;
    double W = F.W, H = F.H;
    for (int i = GH.nlist - 1; i >= 0; i--) {
        Ghost *g = &GH.list[i];
        double dur = g->seg[g->nseg - 1].t - g->t0;
        g->p += F.dt;
        if (g->p > dur) { memmove(&GH.list[i], &GH.list[i + 1], (size_t)(GH.nlist - i - 1) * sizeof *g); GH.nlist--; continue; }
        double tt = g->t0 + g->p;
        int k = 0;
        while (k < g->nseg - 2 && g->seg[k + 1].t < tt) k++;
        Rec a0 = g->seg[k], a1 = k + 1 < g->nseg ? g->seg[k + 1] : a0;
        double f = a1.t > a0.t ? (tt - a0.t) / (a1.t - a0.t) : 0;
        double nx = lerp(a0.x, a1.x, f), ny = lerp(a0.y, a1.y, f);
        if (g->mirror) nx = 1 - nx;
        double x = nx * W, y = ny * H;
        if (g->ntrail == 24) memmove(g->trail, g->trail + 1, 23 * sizeof g->trail[0]), g->ntrail--;
        g->trail[g->ntrail][0] = x, g->trail[g->ntrail][1] = y, g->ntrail++;
        double fade = fmin(fmin(1, g->p * 2), (dur - g->p) * 2);
        Rgb col = pal_abc(g->hue);
        cx_stroke_color(ctx, rgba(col, 0.35 * fade)), cx_line_width(ctx, 2 * F.k);
        cx_begin(ctx);
        for (int j = 0; j < g->ntrail; j++) j ? cx_line(ctx, g->trail[j][0], g->trail[j][1]) : cx_move(ctx, g->trail[j][0], g->trail[j][1]);
        cx_stroke(ctx);
        double jx = chance(0.1) ? urand(-6, 6) * F.k : 0;
        ghost_arrow(ctx, x + jx, y, F.k * 1.3, col, fade);
        ufont(ctx, 11 * F.k, F_MONO), cx_text_align(ctx, CX_LEFT), cx_fill_color(ctx, rgba(col, 0.8 * fade));
        char buf[96];
        snprintf(buf, sizeof buf, g->mirror ? "you (reflected), %ds ago" : "you, %ds ago", js_i32(g->ago));
        fill_u8(ctx, buf, x + 18 * F.k, y + 26 * F.k);
        for (int c = 0; c < g->nclicks; c++) {
            GClick *cl = &g->clicks[c];
            if (!cl->done && cl->t <= tt) {
                cl->done = true;
                double cx = (g->mirror ? 1 - cl->x / F.cssW : cl->x / F.cssW) * W, cy = (cl->y / F.cssH) * H;
                fx_shock(cx, cy, 0.5), fx_ring(cx, cy, 0.5);
            }
        }
    }
}
#endif
