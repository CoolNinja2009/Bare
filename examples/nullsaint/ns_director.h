/* ns_director.h: director.js (glitch budget, random events, beat hits, idle rot, boot). */
#ifndef NS_DIRECTOR_H
#define NS_DIRECTOR_H
#include "ns_state.h"

enum { EV_BREACH, EV_MELT, EV_SHUFFLE, EV_INVERT, EV_PANIC, EV_WORM, EV_ASCII, EV_BLACKOUT, EV_TAKEOVER, EV_SIGNAL, EV_MIRROR, EV_FREEZE, EV_COUNT };
typedef struct EvDef { int key; double w, d0, d1; bool unsafe; const char *name; } EvDef;
static const EvDef EV[EV_COUNT] = {
    { K_evBreach, 3, 4, 6, false, "SECURITY BREACH" },
    { K_evMelt, 2, 6, 9, false, "MELTDOWN" },
    { K_evShuffle, 3, 1.5, 3, false, "TILE SHUFFLE" },
    { K_evInvert, 2, 0.8, 1.5, true, "NEGATIVE" },
    { K_evPanic, 1, 3, 4.5, false, "KERNEL PANIC" },
    { K_evWorm, 3, 0.5, 0.5, false, "WORM" },
    { K_evAscii, 2, 4, 7, false, "ASCII COLLAPSE" },
    { K_evBlackout, 1, 3.5, 5, false, "BLACKOUT" },
    { K_evTakeover, 1, 6, 8, false, "SAINT TAKEOVER" },
    { K_evSignal, 2, 1.5, 3, false, "SIGNAL LOST" },
    { K_evMirror, 2, 4, 6, false, "KALEIDOSCOPE" },
    { K_evFreeze, 2, 1.5, 3, false, "TIME FREEZE" },
};
typedef struct Ev { int id; double t, dur; int seed; Str code; double pct; } Ev;

/* setTimeout() on the frame clock: the breach popups */
typedef struct Timer { double at; int id; } Timer;

static struct {
    Ev active[EV_COUNT];
    int nactive;
    double cool, burst, burstSeed, rot;
    bool wasRotten;
    bool booting;
    double boot_t;
    Glitch G;
    Timer timers[32];
    int ntimers, timer_id;
} D = { .cool = 8 };

static void saint_reroll(void);
static void hud_log_u8(const Str *s);

static Glitch glitch_fresh(void) {
    Glitch g;
    memset(&g, 0, sizeof g);
    g.flash[0] = g.flash[1] = g.flash[2] = 1;
    g.colorMode = -1, g.mirror = -1;
    return g;
}
static void director_kick(double amount) { D.burst = fmin(1.5, D.burst + amount); D.burstSeed = rnd() * 100; }
static void set_timeout(double ms) {
    if (D.ntimers < 32) D.timers[D.ntimers].at = F.now_ms + ms, D.timers[D.ntimers].id = ++D.timer_id, D.ntimers++;
}
/* run the timers due by now_ms, as the page's event loop does before each frame */
static void timers_run(double now_ms) {
    for (;;) {
        int best = -1;
        for (int i = 0; i < D.ntimers; i++)
            if (best < 0 || D.timers[i].at < D.timers[best].at || (D.timers[i].at == D.timers[best].at && D.timers[i].id < D.timers[best].id)) best = i;
        if (best < 0 || D.timers[best].at > now_ms) break;
        double at = D.timers[best].at;
        D.timers[best] = D.timers[--D.ntimers];
        double keep = F.now_ms;
        F.now_ms = at;
        static const char *const L[] = { "INTRUSION DETECTED", "FIREWALL ON FIRE", "SOMEONE IS INSIDE THE WALLPAPER", "ACCESS GRANTED TO: IT" };
        Str s = s_of(PICK(L));
        hud_spawn_popup(F.cssW, F.cssH, NULL, NULL, &s);
        F.now_ms = keep;
    }
}

static void ev_start(Ev *e) {
    switch (e->id) {
    case EV_BREACH:
        for (int i = 0; i < 3; i++) set_timeout(i * 400);
        saint_say_u8("I AM INSIDE");
        break;
    case EV_WORM: {
        int n = randi(1, 3);
        for (int i = 0; i < n; i++) fx_worm();
        break;
    }
    case EV_BLACKOUT: {
        static const char *const L[] = { "I SEE YOU", "THERE YOU ARE", "DONT TURN AROUND" };
        saint_say_u8(PICK(L));
        break;
    }
    case EV_TAKEOVER: {
        static const char *const L[] = { "THIS SCREEN BELONGS TO ME", "LOOK AT ME", "I AM THE WALLPAPER NOW", "KNEEL" };
        saint_say_u8(PICK(L));
        saint_reroll();
        break;
    }
    default: break;
    }
}
static void director_trigger(int id) {
    if (id < 0 || id >= EV_COUNT) return;
    if (EV[id].unsafe && CB(safe)) return;
    for (int i = 0; i < D.nactive; i++) if (D.active[i].id == id) return;
    Ev *e = &D.active[D.nactive++];
    memset(e, 0, sizeof *e);
    e->id = id;
    e->dur = urand(EV[id].d0, EV[id].d1);
    e->seed = randi(0, 65535);
    if (id == EV_PANIC) {
        double v = floor(urand(0, 0xffffffff + 1.0));   /* U.randi(0, 0xffffffff) */
        Str h = uhex(v, 8);
        e->code = s_of("0x"), s_cats(&e->code, &h);
        e->pct = 0;
    } else ev_start(e);
    Str line = ts_of(F.t);
    s_cat(&line, " EVENT "), s_cat(&line, EV[id].name);
    hud_log_u8(&line);
}
static void director_random_event(void) {
    int pool[EV_COUNT], n = 0;
    double sum = 0;
    for (int i = 0; i < EV_COUNT; i++) {
        if (!CFG[EV[i].key] || (EV[i].unsafe && CB(safe))) continue;
        bool on = false;
        for (int j = 0; j < D.nactive; j++) if (D.active[j].id == i) on = true;
        if (on) continue;
        pool[n++] = i, sum += EV[i].w;
    }
    if (!n) return;
    double r = rnd() * sum;
    for (int i = 0; i < n; i++) { r -= EV[pool[i]].w; if (r <= 0) { director_trigger(pool[i]); return; } }
}

static void ev_update(Ev *e, double k, Glitch *G) {
    Pal P = F.P;
    switch (e->id) {
    case EV_BREACH: {
        double p = sin(k * PI);
        G->blocks += 0.6 * p, G->tear += 0.5 * p, G->rgb += 0.6 * p, G->hudShake = p;
        G->banner_on = true;
        G->banner_text = s_of("SECURITY BREACH");
        Str h = uhex(e->seed, 4);
        G->banner_sub = s_of("SECTOR "), s_cats(&G->banner_sub, &h), s_cat(&G->banner_sub, " COMPROMISED // DO NOT PANIC // PANIC");
        G->banner_has_sub = true;
        G->banner_a = fmin(fmin(1, k * 6), (1 - k) * 6), G->banner_c = P.warn;
        if (!CB(safe)) {
            G->flash[0] = P.warn.v[0] / 255, G->flash[1] = P.warn.v[1] / 255, G->flash[2] = P.warn.v[2] / 255;
            G->flash[3] = fmax(0, sin(F.t * 8)) * 0.12 * p;
        }
        break;
    }
    case EV_MELT: {
        double p = sin(k * PI);
        G->melt += p * 0.9, G->sort += p * 0.6;
        if (k < 0.25) {
            G->banner_on = true, G->banner_text = s_of("THERMAL EVENT"), G->banner_sub = s_of("THE CITY IS MELTING. THIS IS NORMAL."), G->banner_has_sub = true;
            G->banner_a = fmin(fmin(1, k * 8), (0.25 - k) * 8), G->banner_c = P.c;
        }
        break;
    }
    case EV_SHUFFLE: G->shuffle = 0.55, G->seedJump = floor(F.t * 5), G->rgb += 0.3; break;
    case EV_INVERT: G->invert = (int)floor(F.t * 8) % 2 ? 1 : 0; break;
    case EV_PANIC:
        G->scan += 0.3, G->grain += 0.2, G->curve += 0.2;
        if (k > 0.9 && !CB(safe)) G->flash[0] = G->flash[1] = G->flash[2] = 1, G->flash[3] = (1 - k) * 4;
        break;
    case EV_ASCII: G->ascii = fmax(G->ascii, smoothd(0, 0.15, k) * (1 - smoothd(0.85, 1, k))); break;
    case EV_BLACKOUT: G->grain += 0.3, G->vig += 0.5; break;
    case EV_TAKEOVER: {
        double p = sin(k * PI);
        S.takeover = CB(saint) ? p : 0;
        G->swirl += p * 0.6, G->zoom += p * 0.05, G->rgb += p * 0.4, G->mosh += p * 0.3;
        break;
    }
    case EV_SIGNAL: {
        double p = fmin(fmin(1, k * 8), (1 - k) * 8);
        G->stat = fmax(G->stat, p * (k < 0.5 ? 0.85 : 0.3));
        G->bars = k >= 0.5 ? p * 0.9 : 0;
        G->vhs += p;
        break;
    }
    case EV_MIRROR: G->mirror = k < 0.08 || k > 0.92 ? -1 : 3, G->hue += sin(k * PI) * 1.5; break;
    case EV_FREEZE:
        G->freeze = k < 0.85 ? 0.985 : 0, G->grain += 0.2;
        if (k < 0.85) {
            G->banner_on = true, G->banner_text = s_of("\xE2\x96\x8C\xE2\x96\x8C PAUSED BY SOMETHING"), G->banner_has_sub = false;
            G->banner_a = 0.6 + 0.4 * sin(F.t * 10), G->banner_c = P.b;
        } else G->banner_on = false;
        break;
    default: break;
    }
}
static void ev_draw(Ev *e, double k) {
    CxCtx *ctx = F.ctx;
    double W = F.W, H = F.H;
    Pal P = F.P;
    if (e->id == EV_PANIC) {
        if (k > 0.92) return;
        double inc = urand(5, 60);
        e->pct = fmin(100, e->pct + F.dt * inc);
        cx_fill_color(ctx, rgba(mixc(P.warn, BLACK, 0.55), 1)), cx_fill_rect(ctx, 0, 0, W, H);
        double s = F.k;
        cx_fill_color(ctx, rgba(WHITE, 1)), cx_text_align(ctx, CX_LEFT), cx_text_baseline(ctx, CX_TOP);
        ufont(ctx, 110 * s, F_DISPLAY_900), fill_u8(ctx, ":(", W * 0.12, H * 0.16);
        ufont(ctx, 30 * s, F_DISPLAY_600);
        static const char *const L[] = { "Your reality ran into a problem and needs to restart.", "We are collecting some of your memories, and then", "we will forget them for you." };
        for (int i = 0; i < 3; i++) fill_u8(ctx, L[i], W * 0.12, H * 0.36 + i * 42 * s);
        ufont(ctx, 30 * s, F_DISPLAY_600);
        char buf[96];
        snprintf(buf, sizeof buf, "%d%% forgotten", js_i32(e->pct));
        fill_u8(ctx, buf, W * 0.12, H * 0.36 + 150 * s);
        ufont(ctx, 16 * s, F_MONO);
        Str stop = s_of("Stop code: REALITY_EXCEPTION_NOT_HANDLED  ");
        s_cats(&stop, &e->code);
        fill_str(ctx, &stop, W * 0.12, H * 0.36 + 220 * s);
        fill_u8(ctx, "What failed: you.sys", W * 0.12, H * 0.36 + 246 * s);
        qrish(ctx, W * 0.12, H * 0.36 + 290 * s, 110 * s, 21, 666);
    } else if (e->id == EV_BLACKOUT) {
        double a = fmin(fmin(1, k * 5), (1 - k) * 5);
        cx_fill_color(ctx, rgba(BLACK, 0.97 * a)), cx_fill_rect(ctx, 0, 0, W, H);
        double sx = CB(saint) ? S.x : F.mx, sy = CB(saint) ? S.y : F.my - 120 * F.k;
        double R = (S.R ? S.R : 200) * 0.32;
        for (int sd = -1; sd <= 1; sd += 2) {
            double ex = sx + sd * R, ey = sy - R * 0.2;
            CxGrad g = cx_radial(ex, ey, 0, ex, ey, R * 0.7);
            cx_stop(&g, 0, rgba(P.warn, a)), cx_stop(&g, 0.15, rgba(P.warn, 0.6 * a)), cx_stop(&g, 1, rgba(P.warn, 0));
            cx_fill_grad(ctx, &g), cx_fill_rect(ctx, ex - R, ey - R, R * 2, R * 2);
            cx_fill_color(ctx, rgba(WHITE, a * S.blink));
            cx_begin(ctx), cx_ellipse(ctx, ex + (F.mx - ex) * 0.01, ey, R * 0.12, R * 0.12 * S.blink + 0.5, 0, 0, TAU, false), cx_fill(ctx);
        }
        ufont(ctx, 18 * F.k, F_MONO_B);
        double r = rnd();
        cx_fill_color(ctx, rgba(P.warn, a * (0.6 + 0.4 * r)));
        cx_text_align(ctx, CX_CENTER);
        Str t = s_of("I SEE YOU"), z = zalgo(&t, 2);
        fill_str(ctx, &z, F.mx, F.my + 40 * F.k);
    } else if (e->id == EV_SIGNAL) {
        double p = fmin(fmin(1, k * 8), (1 - k) * 8);
        ufont(ctx, 48 * F.k, F_DISPLAY_900), cx_text_align(ctx, CX_CENTER), cx_text_baseline(ctx, CX_MIDDLE);
        cx_fill_color(ctx, rgba(WHITE, p));
        if (k < 0.5) fill_u8(ctx, "NO SIGNAL", W / 2, H / 2);
        else {
            Str s = s_of("CH 00 // ");
            s_cat(&s, PICK(WHISPERS));
            fill_str(ctx, &s, W / 2, H / 2);
        }
    }
}

/* ---------- boot ---------- */
static const char *const BOOT[] = {
    "NULL//SAINT BIOS 6.6.6  (C) NOBODY, ALL RIGHTS DISSOLVED",
    "CPU: 1x SOUL @ UNKNOWN GHZ ............ OK",
    "MEMORY TEST: 640K OK  32G OK  \xE2\x88\x9E FAIL",
    "DETECTING SKY ......................... CRACKED",
    "MOUNTING /city ON /dev/dream ........... OK",
    "LOADING NEON DRIVERS ................... OK",
    "CALIBRATING RAIN ....................... 0xBEEF",
    "SUMMONING SAINT [##########----] 71%",
    "WARNING: SOMETHING IS ALREADY WATCHING",
    "HANDING CONTROL TO OPERATOR ...",
};
static void director_start_boot(void) { D.booting = true, D.boot_t = 0; }
static bool director_draw_boot(void) {
    if (!D.booting) return false;
    D.boot_t += F.dt;
    CxCtx *ctx = F.ctx;
    double W = F.W, H = F.H, bt = D.boot_t;
    Pal P = F.P;
    const double T = 5.2;
    if (bt > T) {
        D.booting = false;
        flash(CB(safe) ? 0.1 : 0.7, P.text);
        director_kick(1.2);
        fx_shock(W / 2, H / 2, 1.2);
        Str op = CS(operator_)->n ? *CS(operator_) : s_of("OPERATOR");
        s_upper(&op);
        Str hi = s_of("HELLO, ");
        s_cats(&hi, &op);
        saint_say(&hi);
        return false;
    }
    double fade = smoothd(T - 0.6, T, bt);
    cx_fill_color(ctx, rgba(BLACK, 1 - fade * 0.7)), cx_fill_rect(ctx, 0, 0, W, H);
    double s = F.k, lh = 22 * s;
    ufont(ctx, 15 * s, F_MONO), cx_text_align(ctx, CX_LEFT), cx_text_baseline(ctx, CX_TOP);
    int shown = (int)floor(bt / 0.28);
    int nb = NITEMS(BOOT);
    for (int i = 0; i < (shown < nb ? shown : nb); i++) {
        Str line = s_of(BOOT[i]);
        bool warn = s_starts(&line, "WARNING") || s_has(&line, "FAIL") || s_has(&line, "CRACKED");
        cx_fill_color(ctx, rgba(warn ? P.warn : P.b, 1 - fade));
        if (i == shown - 1) { Str d = decode(&line, fmod(bt, 0.28) / 0.2); fill_str(ctx, &d, 40 * s, 40 * s + i * lh); }
        else fill_str(ctx, &line, 40 * s, 40 * s + i * lh);
    }
    if ((int)floor(bt * 3) % 2) { cx_fill_color(ctx, rgba(P.b, 1 - fade)), cx_fill_rect(ctx, 40 * s, 40 * s + (shown < nb ? shown : nb) * lh, 10 * s, 16 * s); }
    if (bt > 3) {
        double k = smoothd(3, 3.8, bt);
        double big = fmin(W / 7, 160 * s);
        ufont(ctx, big, F_DISPLAY_900), cx_text_align(ctx, CX_CENTER), cx_text_baseline(ctx, CX_MIDDLE);
        Str name = s_of("NULL//SAINT"), dn = decode(&name, k);
        double off = 4 + rnd() * 10;
        glitch_text(ctx, &dn, W / 2, H / 2, P.a, P.b, off, 1 - fade);
        ufont(ctx, 16 * s, F_MONO), cx_fill_color(ctx, rgba(P.text, (1 - fade) * k));
        Str sub = s_of("GHOST SIGNAL ESTABLISHED // YOU ARE NOT ALONE"), ds = decode(&sub, k);
        fill_str(ctx, &ds, W / 2, H / 2 + big * 0.6);
    }
    return true;
}

/* ---------- per frame ---------- */
static Glitch *director_update(void) {
    double dt = F.dt;
    Glitch G = glitch_fresh();
    double k = C(intensity) / 100;
    if (CB(idleRot)) {
        double t0 = C(idleTime);
        double target = smoothd(t0, t0 * 3, I.idle);
        D.rot = target > D.rot ? damp(D.rot, target, 0.3, dt) : damp(D.rot, target, 8, dt);
        if (D.rot > 0.4) D.wasRotten = true;
        if (D.wasRotten && I.idle < 0.5) {
            D.wasRotten = false;
            flash(CB(safe) ? 0.1 : 0.6, F.P.text);
            director_kick(1);
            Str op = CS(operator_)->n ? *CS(operator_) : s_of("OPERATOR");
            s_upper(&op);
            Str hi = s_of("WELCOME BACK, ");
            s_cats(&hi, &op);
            saint_say(&hi);
        }
    } else D.rot = 0;
    F.rot = D.rot;
    double chaos = C(chaos) / 100;
    if (chance(dt * chaos * 0.9 * k)) { double a = urand(0.2, 0.8); director_kick(a); }
    if (CB(beatGlitch) && A.kick) director_kick(0.25 + A.bass * 0.5);
    D.burst = fmax(0, D.burst - dt * 2.8);
    double bu = D.burst;
    if (CB(events) && !D.booting) {
        D.cool -= dt * (0.15 + chaos * 1.6) * (1 + D.rot * 2);
        if (D.cool <= 0) { D.cool = urand(8, 20); director_random_event(); }
    }
    double gi = F.gi = k * (1 + D.rot * 1.5) + bu;
#define V(key) (C(key) / 100)
    G.rgb = V(rgb) * (0.3 + gi * 0.7) + bu * 0.6;
    G.scan = V(scan);
    G.curve = V(curve);
    G.grain = V(grain) * (0.5 + gi * 0.5) + D.rot * 0.4;
    G.blocks = V(blocks) * gi + bu * 0.7;
    G.tear = V(tear) * gi + bu * 0.6;
    G.vhs = V(vhs) * (0.4 + gi * 0.6);
    G.mosh = V(mosh) * gi * 0.8 + D.rot * 0.5 + bu * 0.2;
    G.sort = V(sort) * gi + D.rot * 0.3;
    G.bloom = V(bloom) * (1 + A.bass * 0.6);
    G.vig = V(vignette);
    G.wave = V(wave) * (0.5 + gi * 0.5) + bu * 0.2;
    G.pix = V(pixelate) + D.rot * 0.15;
    G.post = V(posterize);
    G.lens = V(lens);
#undef V
    G.ascii = C(ascii) == 2 ? 1 : 0;
    G.hue = fmod(F.t * C(hueCycle) * 0.004, TAU);
    for (int i = D.nactive - 1; i >= 0; i--) {
        Ev *e = &D.active[i];
        e->t += dt;
        double p = clampd(e->t / e->dur, 0, 1);
        ev_update(e, p, &G);
        if (e->t >= e->dur) {
            int id = e->id;
            memmove(&D.active[i], &D.active[i + 1], (size_t)(D.nactive - i - 1) * sizeof D.active[0]);
            D.nactive--;
            if (id == EV_TAKEOVER) S.takeover = 0;
        }
    }
    if (C(ascii) == 0) G.ascii = 0;
    if (CB(safe)) {
        G.invert = 0, G.flash[3] = fmin(G.flash[3], 0.06);
        G.blocks *= 0.4, G.tear *= 0.4, G.shuffle = 0, G.stat *= 0.3;
    }
    D.G = G;
    F.G = &D.G;
    return &D.G;
}
static void director_draw(void) {
    for (int i = 0; i < D.nactive; i++) ev_draw(&D.active[i], clampd(D.active[i].t / D.active[i].dur, 0, 1));
}
static void director_key(int key) {
    if (!CB(keys)) return;
    const char *K = "1234567890-=";
    const char *p = key ? strchr(K, key) : NULL;
    if (p) director_trigger((int)(p - K));
    else if (key == ' ') { director_kick(1.2); fx_click(F.mx, F.my); }
    else if (key == 'b' || key == 'B') director_start_boot();
    else if (key == 's' || key == 'S') saint_say_u8(PICK(WHISPERS));
    else if (key == 'w' || key == 'W') fx_worm();
}
#endif
