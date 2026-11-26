#version 450
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"
// skymap.glsl: the upper half of the sky into a small map, for what the water
// reflects and lets through (the background is drawn per pixel by sky.glsl).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;

void main() {
    float az = (v_uv.x - 0.5) * TAU, elev = (1.0 - v_uv.y) * 0.5 * PI;
    vec3 d = vec3(cos(elev) * cos(az), sin(elev), cos(elev) * sin(az));
    o = vec4(sky(d, 4), 1.0);
}
