/* spring.c: a damped spring advanced in closed form (ARCHITECTURE §7).
 *
 * Mass 1, stiffness k = (2π / response)², damping c = 2 ζ sqrt(k), where ζ is
 * the damping ratio. The state is the displacement from the target and the
 * velocity. Each step uses the exact solution of the spring's equation for
 * the elapsed time, so the result doesn't depend on how time is split into
 * frames and no step size can make it unstable. */
#include "internal.h"
#include <math.h>

#define TAU 6.28318530717958647692

void bare__spring_step(float *x, float *v, float response, float damping, float dt) {
    if (dt <= 0.0f) return;
    double w0 = TAU / (double)response;   /* natural angular frequency */
    double z = damping;
    double x0 = *x, v0 = *v, t = dt;
    double xt, vt;

    if (fabs(z - 1.0) < 1e-4) {
        /* Critically damped: x = (A + B t) e^(-w0 t). */
        double a = x0, b = v0 + w0 * x0;
        double e = exp(-w0 * t);
        xt = (a + b * t) * e;
        vt = (b - w0 * (a + b * t)) * e;
    } else if (z < 1.0) {
        /* Underdamped: decaying oscillation at wd. */
        double wd = w0 * sqrt(1.0 - z * z);
        double a = x0, b = (v0 + z * w0 * x0) / wd;
        double e = exp(-z * w0 * t), c = cos(wd * t), s = sin(wd * t);
        xt = e * (a * c + b * s);
        vt = e * ((b * wd - z * w0 * a) * c - (a * wd + z * w0 * b) * s);
    } else {
        /* Overdamped: two decaying exponentials. */
        double r = sqrt(z * z - 1.0);
        double r1 = -w0 * (z - r), r2 = -w0 * (z + r);
        double c2 = (v0 - r1 * x0) / (r2 - r1), c1 = x0 - c2;
        double e1 = exp(r1 * t), e2 = exp(r2 * t);
        xt = c1 * e1 + c2 * e2;
        vt = c1 * r1 * e1 + c2 * r2 * e2;
    }
    *x = (float)xt;
    *v = (float)vt;
}
