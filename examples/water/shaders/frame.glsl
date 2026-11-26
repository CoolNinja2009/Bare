// frame.glsl: what the scene shaders share: the frame's params (camera, sun,
// sky, water), the sky, and the water's height (included, not compiled alone).
//
// The camera has no matrices: points are projected on its basis directly, with
// reversed infinite depth (near / distance: 1 at the near plane, 0 at infinity),
// drawn against a depth canvas cleared to 0 with BARE_DEPTH_GREATER.
#ifndef OCEAN_FRAME
#define OCEAN_FRAME
#include "common.glsl"

layout(set = BARE_PARAMS_SET, binding = 0) uniform Frame {
    vec4 uCam;       // xyz: position (y is above the swell while riding it), w: how much it rides the swell
    vec4 uRight;     // xyz, w: tan(horizontal fov / 2)
    vec4 uUp;        // xyz, w: tan(vertical fov / 2)
    vec4 uFwd;       // xyz, w: near plane (m)
    vec4 uSun;       // xyz: towards the sun, w: direct sunlight through the clouds (0..1)
    vec4 uSunColor;  // rgb: the sun's light, w: how much of its disc shows
    vec4 uZenith;    // rgb: sky overhead, w: cloud cover (0..1)
    vec4 uHorizon;   // rgb: sky at the horizon, w: haze distance (m)
    vec4 uCloud;     // rgb: the clouds' undersides, w: how much coast shows on the horizon
    vec4 uWater;     // rgb: deep water's colour, w: choppiness
    vec4 uScatter;   // rgb: light scattered in the water, w: crest glow
    vec4 uAbsorb;    // rgb: absorption per metre, w: light shafts
    vec4 uLengths;   // the three cascades' sizes (m), w: the detail tile's (m)
    vec4 uWind;      // xy: direction, z: speed (m/s), w: foam
    vec4 uRes;       // target size, 1 / size
    vec4 uGrid;      // the mesh: ring radius = x * (exp(y * ring) - 1), z rings, w segments
    vec4 uMisc;      // x: time (s), y: how much the camera leans with the water, zw: cloud drift
    vec4 uExtra;     // x: exposure, y: frame number, z: dt (s), w: 1 = the camera jumped (no easing)
};

// The camera this frame: where it is and how it's turned, once the swell has
// lifted and tilted it (the probe's height and slope). setupCamera() first.
vec3 gCam, gRight, gUp, gFwd;

vec3 rotateTo(vec3 v, vec3 n) {   // the rotation taking +y to n (Rodrigues), applied to v
    vec3 axis = vec3(n.z, 0.0, -n.x);
    float s = length(axis), c = n.y;
    if (s < 1e-6) return v;
    axis /= s;
    return v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0 - c);
}

void setupCamera(sampler2D probe) {
    vec4 pr = texelFetch(probe, ivec2(0), 0);   // height, its velocity, slope along x and z
    gCam = uCam.xyz + vec3(0.0, uCam.w * pr.x, 0.0);
    // a float leans with the water under it (uMisc.y: how much)
    vec3 n = normalize(vec3(-pr.z * uCam.w * uMisc.y, 1.0, -pr.w * uCam.w * uMisc.y));
    gRight = rotateTo(uRight.xyz, n);
    gUp = rotateTo(uUp.xyz, n);
    gFwd = rotateTo(uFwd.xyz, n);
}

// A world point to clip space.
vec4 project(vec3 p) {
    vec3 v = p - gCam;
    return vec4(dot(v, gRight) / uRight.w, dot(v, gUp) / uUp.w, uFwd.w, dot(v, gFwd));
}

// The ray through a pixel (fragment coordinates, y down), unnormalised: 1 along the view axis.
vec3 pixelRay(vec2 frag) {
    vec2 ndc = vec2(frag.x * uRes.z * 2.0 - 1.0, 1.0 - frag.y * uRes.w * 2.0);
    return gFwd + ndc.x * uRight.w * gRight + ndc.y * uUp.w * gUp;
}

// Where a pixel's ray starts: its point on the near plane. Nothing nearer is
// drawn, so whichever side of the water that point is on, the ray starts
// there: the waterline can cut across the picture, as across a dome port
// (the near plane is a few centimetres tall).
vec3 lensPoint(vec3 cam, vec2 frag) { return cam + pixelRay(frag) * uFwd.w; }

/* ---- the sky ---------------------------------------------------------------- */

vec3 skyBase(vec3 d) {
    float y = max(d.y, 0.0);
    vec3 c = mix(uHorizon.rgb, uZenith.rgb, pow(y, 0.45));
    float mu = max(dot(d, uSun.xyz), 0.0);
    // the sun through the deck: a broad bright patch, tighter where the cloud is thin
    // (an overcast deck lets through only uSun.w of it)
    c += uSunColor.rgb * (0.25 + 0.75 * uSun.w) * (0.08 * pow(mu, 3.0) + 0.30 * pow(mu, 20.0) * (0.4 + uSunColor.w) + 0.8 * pow(mu, 400.0) * uSunColor.w);
    return c;
}

// The cloud deck: rgb, coverage.
vec4 clouds(vec3 d, int octaves) {
    if (d.y <= 0.002) return vec4(0.0);
    float t = 1.0 / (d.y + 0.035);              // distance to the deck, in deck heights
    vec2 p = d.xz * t * 0.8 + uMisc.zw;
    float n = fbm(p, octaves);
    float cover = uZenith.w;
    // the deck breaks up towards the sun, as overcast often thins over the brightest part of the sky
    vec2 sh = normalize(uSun.xz + 1e-4), dh = normalize(d.xz + 1e-4);
    cover -= 0.3 * smoothstep(0.2, 1.0, dot(sh, dh)) * smoothstep(0.0, 0.4, fbm(p * 0.25 + 2.0, 3));
    float dens = smoothstep(0.62 - 0.55 * cover, 1.05 - 0.4 * cover, n + 0.12 * fbm(p * 3.1 + 5.0, 3));
    float n2 = fbm(p + uSun.xz * 0.12, octaves);   // a step towards the sun: thin edges catch the light
    float lit = clamp(0.5 + (n - n2) * 3.0, 0.0, 1.0);
    float mu = max(dot(d, uSun.xyz), 0.0);
    vec3 bright = uZenith.rgb * 1.3 + uSunColor.rgb * (0.12 + 0.5 * pow(mu, 6.0)) * (0.1 + 0.9 * uSun.w);
    // thick cloud is dark underneath; thin cloud glows with the light through it
    float thin = (1.0 - dens) * (0.5 + 0.5 * lit);
    vec3 col = mix(uCloud.rgb * (1.4 - 0.5 * dens), bright, thin);
    col += uSunColor.rgb * pow(mu, 12.0) * (1.0 - dens) * 0.8 * uSun.w;   // silver linings
    float fade = smoothstep(0.0, 0.16, d.y) * exp(-t * 0.012);
    return vec4(col, dens * fade);
}

// A far coast: low hills along part of the horizon, blue with distance.
float coastHeight(vec3 d) {
    float az = atan(d.z, d.x);
    float band = smoothstep(1.7, 2.3, az) * (1.0 - smoothstep(3.3, 4.2, az + (az < 0.0 ? TAU : 0.0)));
    float h = fbm(vec2(az * 5.0, 3.7), 5) - 0.32 + 0.35 * fbm(vec2(az * 1.3, 9.1), 2);
    return max(h, 0.0) * 0.03 * band * uCloud.w;
}

vec3 sky(vec3 d, int octaves) {
    if (d.y < 0.0) return uHorizon.rgb * mix(0.92, 0.7, clamp(-d.y * 4.0, 0.0, 1.0));
    vec3 c = skyBase(d);
    vec4 cl = clouds(d, octaves);
    float mu = max(dot(d, uSun.xyz), 0.0);
    c += uSunColor.rgb * smoothstep(0.99986, 0.99993, mu) * 30.0 * uSunColor.w;   // the disc
    c = mix(c, cl.rgb, cl.a);
    // Overcast, the low sky is the deck seen edge on, far off and dark, with
    // only a thin pale strip at the horizon where it lifts off the sea.
    float low = 1.0 - smoothstep(0.0, 0.3, d.y);
    float strip = 1.0 - smoothstep(0.0, 0.012, d.y);
    c = mix(c, uCloud.rgb * 1.4 + uHorizon.rgb * 0.25, low * uZenith.w * 0.8);
    c = mix(c, uHorizon.rgb * 1.3, strip * 0.7);
    float hill = coastHeight(d);
    if (d.y < hill) {
        float k = d.y / max(hill, 1e-4);   // hazier towards the foot, as mist lies on the water
        vec3 land = mix(uCloud.rgb * vec3(0.8, 0.9, 1.0), uHorizon.rgb * 0.8, 0.3 + 0.5 * (1.0 - k));
        c = mix(c, land, smoothstep(0.0, 0.0015, hill - d.y));
    }
    return c;
}

// The sky map (skymap.glsl): the upper half of the sky by angle, x around, y from zenith (0) to horizon (1).
vec2 skymapUv(vec3 d) {
    float elev = asin(clamp(d.y, 0.0, 1.0));
    return vec2(atan(d.z, d.x) / TAU + 0.5, clamp(1.0 - elev / (0.5 * PI), 0.004, 0.996));
}

vec3 skyAmbient() { return mix(uHorizon.rgb, uZenith.rgb, 0.55) + uSunColor.rgb * uSun.w * 0.08; }

/* ---- water ------------------------------------------------------------------ */

// Bilinear filtering in full floats, wrapping. The texture units blend with
// 8-bit weights: 1/256 of a 1.8 m texel is 7 mm, which shows as steps along
// the waterline on a lens a few centimetres tall.
vec4 bilinear(sampler2D t, vec2 uv) {
    ivec2 n = textureSize(t, 0);
    vec2 p = uv * vec2(n) - 0.5;
    vec2 f = fract(p);
    ivec2 i = ivec2(floor(p));
    ivec2 i0 = (i % n + n) % n, i1 = (i0 + 1) % n;
    vec4 a = texelFetch(t, i0, 0), b = texelFetch(t, ivec2(i1.x, i0.y), 0);
    vec4 c = texelFetch(t, ivec2(i0.x, i1.y), 0), d = texelFetch(t, i1, 0);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// The three cascades' displacement (x, height, z) at a point of the undisplaced plane.
vec3 dispAt(sampler2D d0, sampler2D d1, sampler2D d2, vec2 xz) {
    return bilinear(d0, xz / uLengths.x).xyz + bilinear(d1, xz / uLengths.y).xyz + bilinear(d2, xz / uLengths.z).xyz;
}

// The surface's height above a world point: the displaced point that lands there, found by iterating.
float waterHeight(sampler2D d0, sampler2D d1, sampler2D d2, vec2 xz) {
    vec2 p = xz;
    for (int i = 0; i < 4; i++) p = xz - dispAt(d0, d1, d2, p).xz;
    return dispAt(d0, d1, d2, p).y;
}

// Sunlight once it's under the surface (heading down).
vec3 sunUnderwater() { return refract(-uSun.xyz, vec3(0.0, 1.0, 0.0), 1.0 / 1.333); }

// Light scattered towards the eye from direction `dir` at `depth` metres down.
vec3 underwaterLight(vec3 dir, float depth) {
    vec3 reach = exp(-uAbsorb.rgb * max(depth, 0.0) * 0.8);
    float up = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    float sunward = pow(max(dot(dir, -sunUnderwater()), 0.0), 5.0);
    return uScatter.rgb * reach * (0.18 + 0.82 * up * up + 1.4 * sunward * uSun.w);
}

#endif
