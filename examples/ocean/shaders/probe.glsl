#version 450
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"
// probe.glsl: one pixel: the water's height under the camera, followed by a
// spring (a floating camera lags the swell a little), so riding the swell is
// worked out on the GPU and every scene shader reads it from here (setupCamera).
layout(location = 0) out vec4 o;   // height, velocity
layout(set = 2, binding = 0) uniform sampler2D uD0;
layout(set = 2, binding = 1) uniform sampler2D uD1;
layout(set = 2, binding = 2) uniform sampler2D uD2;
layout(set = 2, binding = 3) uniform sampler2D uPrev;

void main() {
    // a float half a metre across: its height is the average under it, its
    // lean the slope across it, so ripples don't shake it
    const float R = 0.25;
    float c = waterHeight(uD0, uD1, uD2, uCam.xz);
    float xp = waterHeight(uD0, uD1, uD2, uCam.xz + vec2(R, 0.0)), xm = waterHeight(uD0, uD1, uD2, uCam.xz - vec2(R, 0.0));
    float zp = waterHeight(uD0, uD1, uD2, uCam.xz + vec2(0.0, R)), zm = waterHeight(uD0, uD1, uD2, uCam.xz - vec2(0.0, R));
    float target = (c + xp + xm + zp + zm) / 5.0;
    vec2 slope = vec2(xp - xm, zp - zm) / (2.0 * R);
    vec4 prev = texelFetch(uPrev, ivec2(0), 0);
    if (uExtra.w > 0.5) { o = vec4(target, 0.0, slope); return; }
    float dt = min(uExtra.z, 0.05);
    const float w = 2.0 * PI / 0.6, zeta = 0.7;     // a 0.6 s spring: it lags the swell a little
    float v = prev.y + (w * w * (target - prev.x) - 2.0 * zeta * w * prev.y) * dt;
    o = vec4(prev.x + v * dt, v, mix(prev.zw, slope, 1.0 - exp(-dt * 7.0)));
}
