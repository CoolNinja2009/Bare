/* easing.c: CSS cubic-bezier timing functions (ARCHITECTURE §7).
 *
 * The curve runs from (0, 0) to (1, 1) through control points (x1, y1) and
 * (x2, y2). Progress `x` is the horizontal coordinate, so the curve's
 * parameter t has to be found from x first: Newton steps, falling back to
 * bisection where the slope is too flat (as browsers do). */
#include "internal.h"
#include <math.h>

/* One coordinate of the curve: 3(1-t)^2 t p1 + 3(1-t) t^2 p2 + t^3, in Horner form. */
static float curve(float p1, float p2, float t) {
    float a = 1.0f - 3.0f * p2 + 3.0f * p1;
    float b = 3.0f * p2 - 6.0f * p1;
    float c = 3.0f * p1;
    return ((a * t + b) * t + c) * t;
}

static float slope(float p1, float p2, float t) {
    float a = 1.0f - 3.0f * p2 + 3.0f * p1;
    float b = 3.0f * p2 - 6.0f * p1;
    float c = 3.0f * p1;
    return (3.0f * a * t + 2.0f * b) * t + c;
}

float bare__bezier(const float b[4], float x) {
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    float x1 = b[0], y1 = b[1], x2 = b[2], y2 = b[3];
    if (x1 == y1 && x2 == y2) return x;   /* linear */

    float t = x;
    for (int i = 0; i < 8; i++) {
        float err = curve(x1, x2, t) - x;
        if (fabsf(err) < 1e-6f) return curve(y1, y2, t);
        float d = slope(x1, x2, t);
        if (fabsf(d) < 1e-6f) break;
        t -= err / d;
    }
    /* Bisection: x(t) is monotonic for x1, x2 in [0, 1]. */
    float lo = 0.0f, hi = 1.0f;
    t = x;
    for (int i = 0; i < 40; i++) {
        float v = curve(x1, x2, t);
        if (fabsf(v - x) < 1e-6f) break;
        if (v < x) lo = t; else hi = t;
        t = (lo + hi) * 0.5f;
    }
    return curve(y1, y2, t);
}
