#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// fft.glsl: one stage of an inverse FFT (Stockham, radix 2: natural order in
// and out, so no bit reversal). log2(N) stages along rows, then log2(N) down
// the columns, turn the spectra into the surface. Rows hold three cascades
// side by side; each is its own transform.
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
layout(set = 2, binding = 0) uniform sampler2D uA;
layout(set = 2, binding = 1) uniform sampler2D uB;
layout(set = 3, binding = 0) uniform Params {
    vec4 uStage;   // span of this stage (2, 4, ... N), 1 = along rows / 0 = columns, N
};

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    int size = int(uStage.x), N = int(uStage.z);
    bool rows = uStage.y > 0.5;
    int i = rows ? p.x % N : p.y;
    int half_ = size / 2;
    int even = (i / size) * half_ + i % half_;
    ivec2 pe = rows ? ivec2(p.x - i + even, p.y) : ivec2(p.x, even);
    ivec2 po = rows ? ivec2(p.x - i + even + N / 2, p.y) : ivec2(p.x, even + N / 2);
    float a = TAU * float(i) / float(size);
    vec2 w = vec2(cos(a), sin(a));
    vec4 ae = texelFetch(uA, pe, 0), ao = texelFetch(uA, po, 0);
    vec4 be = texelFetch(uB, pe, 0), bo = texelFetch(uB, po, 0);
    o0 = vec4(ae.xy + cmul(w, ao.xy), ae.zw + cmul(w, ao.zw));
    o1 = vec4(be.xy + cmul(w, bo.xy), be.zw + cmul(w, bo.zw));
}
