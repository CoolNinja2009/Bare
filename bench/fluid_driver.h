/* fluid_driver.h: force-included into examples/fluid/fluid.c to build
 * fluid_bench (see CMakeLists.txt), so the benchmark measures the real example
 * unchanged. Each frame it moves the mouse along a fixed path (the fluid never
 * settles), hides the settings panel on frame 2, and writes the frame rate
 * into the window title, where bench.ps1 reads it. */
#ifndef BARE_FLUID_DRIVER_H
#define BARE_FLUID_DRIVER_H

#include "bare.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>

static void bench_drive(void) {
    static int frame, frames;
    static double last;
    SDL_Event e;
    if (++frame == 2) {   /* Tab hides the panel: the browser version has none */
        SDL_zero(e);
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.key = SDLK_TAB;
        e.key.scancode = SDL_SCANCODE_TAB;
        e.key.down = true;
        SDL_PushEvent(&e);
        e.type = SDL_EVENT_KEY_UP;
        e.key.down = false;
        SDL_PushEvent(&e);
    }
    /* The same path the page's ?stir mode follows (prepared by bench.ps1). */
    float a = (float)frame * 0.05f;
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.x = 640 + 300 * cosf(a);
    e.motion.y = 360 + 200 * sinf(a * 1.3f);
    SDL_PushEvent(&e);

    frames++;
    if (bare_time() - last >= 1.0) {
        char title[32];
        snprintf(title, sizeof title, "fps %d", frames);
        bare_title(title);
        frames = 0;
        last = bare_time();
    }
}

static bool bench_frame(void) {
    bool open = (bare_frame)();
    if (open) bench_drive();
    return open;
}

#define bare_frame() bench_frame()

#endif
