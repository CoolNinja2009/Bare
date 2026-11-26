#version 450
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"
// sky.glsl: the background, one ray per pixel; the sea is drawn over it.
layout(location = 0) out vec4 o;
layout(set = 2, binding = 0) uniform sampler2D uProbe;

void main() {
    setupCamera(uProbe);
    o = vec4(sky(normalize(pixelRay(gl_FragCoord.xy)), 6), 1.0);
}
