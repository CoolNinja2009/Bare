#version 450
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"
// spray.glsl: spray torn off breaking crests (a program for bare_draw), one
// instance per droplet cloud, no vertex data. Each instance lives over and
// over at a new random spot near the camera (fixed in the world while you
// move); it only flies where the sea is breaking right now (fresh foam, which
// builds above 1 while a crest folds), thrown up and down the wind, then falls
// and spreads into mist. Depth-tested against the sea, not written.
//
// Inputs (vertex half): 0-2 displacement + foam per cascade, 3 the probe.

#ifdef BARE_VERTEX
layout(set = 0, binding = 0) uniform sampler2D uD0;
layout(set = 0, binding = 1) uniform sampler2D uD1;
layout(set = 0, binding = 2) uniform sampler2D uD2;
layout(set = 0, binding = 3) uniform sampler2D uProbe;
layout(location = 0) out vec2 vQ;
layout(location = 1) out vec4 vColor;   // premultiplied

const float RANGE = 90.0;   // metres around the camera

void main() {
    const vec2 CORNER[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(-1, 1), vec2(-1, 1), vec2(1, -1), vec2(1, 1));
    vec2 c = CORNER[gl_VertexIndex];
    uint id = uint(gl_InstanceIndex);
    float h1 = u01(pcg(id * 3u + 1u)), h2 = u01(pcg(id * 3u + 2u)), h3 = u01(pcg(id * 7u + 5u));
    float life = 1.1 + 1.6 * h1;
    float cyc = uMisc.x / life + h2, round_ = floor(cyc), age = fract(cyc) * life;
    uint seed = pcg(id ^ pcg(uint(int(round_)) + 0x51ed27u));
    vec4 r = vec4(u01(seed), u01(pcg(seed + 1u)), u01(pcg(seed + 2u)), u01(pcg(seed + 3u)));
    setupCamera(uProbe);
    vec3 cam = gCam;
    // a spot in a 2 RANGE square that repeats over the world, kept around the camera
    vec2 rel = mod(r.xy * 2.0 * RANGE - cam.xz, 2.0 * RANGE) - RANGE;
    vec2 xz = cam.xz + rel;
    vec4 d0 = textureLod(uD0, xz / uLengths.x, 0.0), d1 = textureLod(uD1, xz / uLengths.y, 0.0);
    vec4 d2 = textureLod(uD2, xz / uLengths.z, 0.0);
    float fresh = smoothstep(1.0, 1.6, d0.w + 0.7 * d1.w + 0.2 * d2.w);
    float wind = smoothstep(4.0, 16.0, uWind.z);
    float strength = fresh * wind * uWind.w;
    gl_Position = vec4(0.0);   // not flying: a degenerate quad
    if (strength <= r.z || length(rel) > RANGE) return;
    vec3 base = vec3(xz.x + d0.x + d1.x + d2.x, d0.y + d1.y + d2.y, xz.y + d0.z + d1.z + d2.z);
    vec3 v0 = vec3(uWind.x, 0.0, uWind.y) * uWind.z * (0.18 + 0.25 * r.w)
            + vec3(r.w - 0.5, 0.0, h3 - 0.5) * 1.6 + vec3(0.0, 1.2 + 3.8 * r.w * r.w, 0.0);
    float drag = 1.0 / 1.4;   // air slows it, gravity pulls it back down
    float k = (1.0 - exp(-age * drag)) / drag;
    vec3 p = base + v0 * k + vec3(uWind.x, 0.0, uWind.y) * uWind.z * 0.12 * (age - k) - vec3(0.0, 4.9 * age * age * 0.6, 0.0);
    float t = age / life;
    float size = mix(0.06, 0.7, sqrt(t)) * (0.6 + 0.8 * h3) * (0.7 + 0.6 * wind);
    // billboard
    vec3 world = p + (c.x * gRight + c.y * gUp) * size;
    vQ = c;
    vec3 amb = skyAmbient();
    vec3 light = amb * 1.05 + uSunColor.rgb * uSun.w * (0.35 + 1.2 * pow(max(dot(normalize(p - cam), uSun.xyz), 0.0), 6.0));
    float a = strength * (1.0 - t) * (1.0 - t) * smoothstep(0.0, 0.08, t) * 0.55 * (1.0 - smoothstep(0.7, 1.0, length(rel) / RANGE));
    a *= clamp(length(p - cam) / 0.6, 0.0, 1.0);   // nothing sticks to the lens
    vColor = vec4(light * 0.92 * a, a);
    gl_Position = project(world);
}

#else
layout(location = 0) in vec2 vQ;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 o;

void main() {
    float d = dot(vQ, vQ);
    if (d > 1.0) discard;
    float m = (1.0 - d) * (1.0 - d);
    o = vColor * m;
}
#endif
