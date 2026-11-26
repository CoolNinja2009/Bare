#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// assemble.glsl: one cascade's surface, from the transformed spectra:
// displacement (choppy x and z, height) and the derivatives for normals; and
// its foam, which builds up where the surface folds over (the Jacobian of the
// displacement drops below a threshold) and fades over seconds. Foam stays
// where it formed while the crest moves on, as on the sea.
layout(location = 0) out vec4 o0;   // choppiness * Dx, Dy, choppiness * Dz, foam
layout(location = 1) out vec4 o1;   // Dy_x, Dy_z, choppiness * Dx_x, choppiness * Dz_z
layout(set = 2, binding = 0) uniform sampler2D uA;      // the FFT atlas: (Dx, Dy, Dz, Dxz)
layout(set = 2, binding = 1) uniform sampler2D uB;      //                (Dy_x, Dy_z, Dx_x, Dz_z)
layout(set = 2, binding = 2) uniform sampler2D uPrev;   // last frame's o0 (its foam)
layout(set = 3, binding = 0) uniform Params {
    vec4 uC;      // cascade, N, choppiness, dt
    vec4 uFoam;   // fold threshold, build rate, fade time (s), -
};

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    int c = int(uC.x), N = int(uC.y);
    ivec2 q = ivec2(c * N + p.x, p.y);
    vec4 a = texelFetch(uA, q, 0), b = texelFetch(uB, q, 0);
    float l = uC.z;
    float jxx = 1.0 + l * b.z, jzz = 1.0 + l * b.w, jxz = l * a.w;
    float J = jxx * jzz - jxz * jxz;
    float foam = texelFetch(uPrev, p, 0).w * exp(-uC.w / max(uFoam.z, 0.05));
    foam += clamp((uFoam.x - J) * 2.0, 0.0, 1.0) * uFoam.y * uC.w;
    o0 = vec4(l * a.x, a.y, l * a.z, min(foam, 2.0));
    o1 = vec4(b.x, b.y, l * b.z, l * b.w);
}
