#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// evolve.glsl: every wave moved on to time t, as the eight spectra the
// surface needs: displacement x / height / z, and their derivatives (for
// normals and for where the surface folds over, which is where foam forms).
// They're real once transformed, so two share each complex number (A + iB):
// four complex spectra, two in each target.
layout(location = 0) out vec4 o0;   // (Dx + i Dy, Dz + i Dxz)
layout(location = 1) out vec4 o1;   // (Dy_x + i Dy_z, Dx_x + i Dz_z)
layout(set = 2, binding = 0) uniform sampler2D uH0;
layout(set = 3, binding = 0) uniform Params {
    vec4 uLen;    // the cascades' sizes (m), N
    vec4 uTime;   // t (s, wraps at the loop), the loop (s)
};

vec2 pack(vec2 a, vec2 b) { return vec2(a.x - b.y, a.y + b.x); }

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    int N = int(uLen.w);
    int c = p.x / N;
    ivec2 mn = ivec2(p.x - c * N, p.y);
    float L = c == 0 ? uLen.x : c == 1 ? uLen.y : uLen.z;
    vec2 k = TAU / L * vec2(mn.x < N / 2 ? mn.x : mn.x - N, mn.y < N / 2 ? mn.y : mn.y - N);
    float kl = length(k);
    if (kl < 1e-6) { o0 = vec4(0.0); o1 = vec4(0.0); return; }
    vec4 h0 = texelFetch(uH0, p, 0);
    // Frequencies rounded to whole turns of the loop, so the sea repeats exactly
    // and t can wrap without a jump (and stays small enough for floats).
    float w0 = TAU / uTime.y;
    float w = floor(sqrt(G * kl) / w0) * w0;
    float turns = fract(w / TAU * uTime.x);
    vec2 e = vec2(cos(TAU * turns), sin(TAU * turns));
    vec2 h = cmul(h0.xy, e) + cmul(h0.zw, conj2(e));
    vec2 ih = vec2(-h.y, h.x);
    vec2 dx = -k.x / kl * ih, dz = -k.y / kl * ih;
    vec2 dyx = k.x * ih, dyz = k.y * ih;
    vec2 dxx = k.x * k.x / kl * h, dzz = k.y * k.y / kl * h, dxz = k.x * k.y / kl * h;
    o0 = vec4(pack(dx, h), pack(dz, dxz));
    o1 = vec4(pack(dyx, dyz), pack(dxx, dzz));
}
