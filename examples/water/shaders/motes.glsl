#version 450
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"
// motes.glsl: what hangs in the water (a program for bare_draw), drawn after
// the depth of field so each is its own blur circle: specks drifting in a
// box that repeats around the camera, and, under foam, bubbles rising.
// Out of focus they swell into faint discs, as in footage; the water between
// them and the eye dims them. Hidden above the surface.
//
// Inputs (vertex half): 0-2 displacement + foam per cascade, 3 the probe.

#ifdef BARE_VERTEX
layout(set = 0, binding = 0) uniform sampler2D uD0;
layout(set = 0, binding = 1) uniform sampler2D uD1;
layout(set = 0, binding = 2) uniform sampler2D uD2;
layout(set = 0, binding = 3) uniform sampler2D uProbe;
layout(location = 0) out vec2 vQ;
layout(location = 1) out vec4 vColor;   // premultiplied

const float BOX = 14.0;
const float FOCUS = 1.6, BLUR = 18.0;   // the lens: focus (m), blur (px) at half that distance

void main() {
    const vec2 CORNER[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(-1, 1), vec2(-1, 1), vec2(1, -1), vec2(1, 1));
    vec2 c = CORNER[gl_VertexIndex];
    uint id = uint(gl_InstanceIndex);
    vec4 r = vec4(u01(pcg(id * 4u + 1u)), u01(pcg(id * 4u + 2u)), u01(pcg(id * 4u + 3u)), u01(pcg(id * 4u + 4u)));
    bool bubble = (id & 3u) == 0u;
    setupCamera(uProbe);
    float t = uMisc.x;
    vec3 drift = bubble ? vec3(0.0, t * (0.25 + 0.35 * r.w), 0.0)
                        : vec3(sin(t * 0.13 + r.w * 6.0), sin(t * 0.21 + r.x * 6.0) * 0.4 - t * 0.02, cos(t * 0.11 + r.y * 6.0)) * 0.35;
    vec3 p = gCam + mod(r.xyz * BOX + drift - gCam, BOX) - 0.5 * BOX;
    vec3 d = dispAt(uD0, uD1, uD2, p.xz);   // close enough near the camera
    float below = d.y - p.y;
    gl_Position = vec4(0.0);
    if (below < 0.03) return;
    float foam = textureLod(uD0, p.xz / uLengths.x, 0.0).w + textureLod(uD1, p.xz / uLengths.y, 0.0).w;
    if (bubble && below > 0.25 + foam * 1.2 + 0.4 * r.w) return;   // bubbles stay under the foam
    vec3 v = p - gCam;
    float dist = length(v);
    if (dist < uFwd.w * 1.5 || dot(v, gFwd) < uFwd.w) return;
    float pxPerM = uRes.y / (2.0 * uUp.w * dist);
    float sizePx = (bubble ? 0.004 + 0.006 * r.w : 0.0015 + 0.003 * r.w) * pxPerM;
    float cocPx = abs(1.0 - FOCUS / dist) * BLUR * (FOCUS / max(dist, 0.3));
    float px = max(max(sizePx, cocPx), 1.2);
    float energy = (max(sizePx, 0.7) / px) * (max(sizePx, 0.7) / px);
    vec3 light = underwaterLight(vec3(0.0, 1.0, 0.0), below) * (bubble ? 7.0 : 3.0 + 4.0 * r.z);
    vec3 fog = exp(-uAbsorb.rgb * dist);
    float a = clamp(energy * (bubble ? 1.0 : 0.6), 0.0, 1.0) * clamp(1.0 - dist / (0.5 * BOX), 0.0, 1.0);
    vColor = vec4(light * fog * a, a * (bubble ? 0.5 : 0.25));
    vQ = c;
    gl_Position = project(p + (c.x * gRight + c.y * gUp) * (px / pxPerM));
}

#else
layout(location = 0) in vec2 vQ;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 o;

void main() {
    float d = dot(vQ, vQ);
    if (d > 1.0) discard;
    float disc = smoothstep(1.0, 0.75, d) * (0.75 + 0.25 * d);   // a lens's disc: a brighter rim
    o = vColor * disc;
}
#endif
