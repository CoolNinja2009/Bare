#version 450
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"
// water.glsl: the scene seen through the lens. Where the lens is under the
// surface, the water between the eye and what the pixel hit (its depth, read
// from the depth canvas) absorbs and scatters light, and sunlight focused by
// the waves above makes shafts. Where the waterline crosses the lens, a dark
// meniscus.
//
// Inputs: 0 scene, 1 depth, 2-4 displacement per cascade, 5-6 derivatives of
// cascades 1 and 2, 7 the probe.
layout(location = 0) out vec4 o;
layout(set = 2, binding = 0) uniform sampler2D uScene;
layout(set = 2, binding = 1) uniform sampler2D uDepth;
layout(set = 2, binding = 2) uniform sampler2D uD0;
layout(set = 2, binding = 3) uniform sampler2D uD1;
layout(set = 2, binding = 4) uniform sampler2D uD2;
layout(set = 2, binding = 5) uniform sampler2D uN1;
layout(set = 2, binding = 6) uniform sampler2D uN2;
layout(set = 2, binding = 7) uniform sampler2D uProbe;

// How much the surface's curvature focuses light at a point `depth` below it.
float laplacian(sampler2D n, vec2 p, float L, float e) {
    vec2 a = textureLod(n, (p + vec2(e, 0.0)) / L, 0.0).xy, b = textureLod(n, (p - vec2(e, 0.0)) / L, 0.0).xy;
    vec2 c = textureLod(n, (p + vec2(0.0, e)) / L, 0.0).xy, d = textureLod(n, (p - vec2(0.0, e)) / L, 0.0).xy;
    return (a.x - b.x + c.y - d.y) / (2.0 * e);
}

vec3 shafts(vec3 cam, vec3 dir, float far, float surface) {
    vec3 Ld = sunUnderwater();
    const int STEPS = 20;
    float dt = far / float(STEPS);
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy + uExtra.y * vec2(5.588, 3.17), vec2(0.06711056, 0.00583715))));
    float acc = 0.0;
    for (int i = 0; i < STEPS; i++) {
        float t = (float(i) + jitter) * dt;
        vec3 q = cam + dir * t;
        float depth = surface - q.y;
        if (depth <= 0.05) continue;
        vec2 qs = q.xz + Ld.xz * depth / Ld.y;   // the surface point the sunlight came through
        float lap = laplacian(uN1, qs, uLengths.y, 0.6) + 0.6 * laplacian(uN2, qs, uLengths.z, 0.15);
        float focus = clamp(1.0 - lap * min(depth, 12.0) * 0.45, 0.0, 3.0);
        acc += max(focus * focus * focus - 0.8, 0.0) * exp(-uAbsorb.g * (depth + t) * 1.2) * dt;
    }
    float phase = 0.05 + 0.25 * max(dir.y + 0.3, 0.0) + 2.0 * pow(max(dot(dir, -Ld), 0.0), 4.0);
    return uSunColor.rgb * (0.1 + uSun.w) * vec3(0.45, 0.85, 0.9) * acc * phase * 0.004;
}

void main() {
    setupCamera(uProbe);
    vec3 cam = gCam;
    vec2 frag = gl_FragCoord.xy;
    vec3 ray = pixelRay(frag);
    vec3 dir = normalize(ray);
    float bob = texelFetch(uProbe, ivec2(0), 0).x;
    vec3 lp = lensPoint(cam, frag);
    bool near = abs(cam.y - bob) < 5.0;
    float H = near ? waterHeight(uD0, uD1, uD2, lp.xz) : bob;
    float s = lp.y - H;
    ivec2 px = ivec2(frag);
    vec3 col = texelFetch(uScene, px, 0).rgb;
    if (s < 0.0) {
        // the thin film of water on the dome port wobbles what's behind it
        float t = uMisc.x;
        vec2 wob = vec2(sin(frag.y * 0.021 + t * 1.3) + sin(frag.y * 0.05 - t * 2.1) * 0.4,
                        cos(frag.x * 0.017 + t * 1.1)) * 1.6;
        vec2 at = frag + wob;
        col = texture(uScene, at * uRes.zw).rgb;
        float z = texelFetch(uDepth, ivec2(clamp(at, vec2(0.0), uRes.xy - 1.0)), 0).r;
        float dist = z > 0.0 ? uFwd.w / z * length(ray) : 1e5;
        float surface = near ? H : bob;
        float camDepth = max(surface - cam.y, 0.0);
        vec3 T = exp(-uAbsorb.rgb * dist);
        float along = min(dist, 40.0);
        vec3 inscatter = underwaterLight(dir, camDepth - dir.y * along * 0.5);
        col = col * T + inscatter * (1.0 - T);
        if (uAbsorb.w > 0.0) col += shafts(cam, dir, min(dist, 45.0), surface) * uAbsorb.w;
    }
    float m = exp(-(s / 0.0011) * (s / 0.0011));
    col *= 1.0 - 0.85 * m;
    o = vec4(col, 1.0);
}
