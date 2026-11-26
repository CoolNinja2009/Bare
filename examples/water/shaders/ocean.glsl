#version 450
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"
// ocean.glsl: the sea's surface (a program for bare_draw). The mesh is rings
// around the camera, built from gl_VertexIndex: fine at your feet, kilometres
// apart at the horizon, moved by the displacement maps in the vertex half.
// The fragment half shades it from above (sky reflection, light through the
// crests, sun glint, foam, haze) or from below (Snell's window, total internal
// reflection), whichever side of the water the pixel's ray starts on.
//
// Inputs: 0-2 displacement + foam per cascade, 3 the probe (camera height),
// 4-6 derivatives per cascade, 7 the sky map, 8 the detail texture (detail.glsl).

#ifdef BARE_VERTEX
layout(set = 0, binding = 0) uniform sampler2D uD0;
layout(set = 0, binding = 1) uniform sampler2D uD1;
layout(set = 0, binding = 2) uniform sampler2D uD2;
layout(set = 0, binding = 3) uniform sampler2D uProbe;
layout(location = 0) out vec3 vP;      // the displaced point
layout(location = 1) out vec2 vGrid;   // where it came from: the maps are read there

void main() {
    const ivec2 C[6] = ivec2[](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(0, 1), ivec2(1, 0), ivec2(1, 1));
    int segs = int(uGrid.w);
    int q = gl_VertexIndex / 6;
    ivec2 rs = ivec2(q / segs, q % segs) + C[gl_VertexIndex % 6];
    float ring = float(rs.x);
    float r = uGrid.x * (exp(uGrid.y * ring) - 1.0);
    float th = TAU * float(rs.y) / float(segs);
    setupCamera(uProbe);
    vec2 centre = floor(uCam.xz + 0.5);   // snapped to a metre: the mesh doesn't swim as you move
    vec2 xz = centre + r * vec2(cos(th), sin(th));
    // Cascades whose waves are shorter than the mesh can follow are left to the normals.
    float spacing = max(uGrid.x * uGrid.y * exp(uGrid.y * ring), r * TAU / float(segs));
    vec3 d = textureLod(uD0, xz / uLengths.x, 0.0).xyz * (1.0 - smoothstep(uLengths.x / 40.0, uLengths.x / 10.0, spacing))
           + textureLod(uD1, xz / uLengths.y, 0.0).xyz * (1.0 - smoothstep(uLengths.y / 40.0, uLengths.y / 10.0, spacing))
           + textureLod(uD2, xz / uLengths.z, 0.0).xyz * (1.0 - smoothstep(uLengths.z / 40.0, uLengths.z / 10.0, spacing));
    vP = vec3(xz.x + d.x, d.y, xz.y + d.z);
    vGrid = xz;
    gl_Position = project(vP);
}

#else
layout(set = 2, binding = 0) uniform sampler2D uD0;
layout(set = 2, binding = 1) uniform sampler2D uD1;
layout(set = 2, binding = 2) uniform sampler2D uD2;
layout(set = 2, binding = 3) uniform sampler2D uProbe;
layout(set = 2, binding = 4) uniform sampler2D uN0;
layout(set = 2, binding = 5) uniform sampler2D uN1;
layout(set = 2, binding = 6) uniform sampler2D uN2;
layout(set = 2, binding = 7) uniform sampler2D uSky;
layout(set = 2, binding = 8) uniform sampler2D uDetail;
layout(location = 0) in vec3 vP;
layout(location = 1) in vec2 vGrid;
layout(location = 0) out vec4 o;

vec3 skyMap(vec3 d) { return texture(uSky, skymapUv(d)).rgb; }

// Where p falls in the detail tile, in the wind's frame (the tile's x runs down the wind).
vec2 detailUv(vec2 p, float turn, float scale) {
    float c = cos(turn), s = sin(turn);
    vec2 w = vec2(uWind.x * c - uWind.y * s, uWind.x * s + uWind.y * c);
    return vec2(dot(p, w), dot(p, vec2(-w.y, w.x))) / (uLengths.w * scale);
}

// The tile's slopes back in world space.
vec2 detailSlope(vec2 s, float turn) {
    float c = cos(turn), sn = sin(turn);
    vec2 w = vec2(uWind.x * c - uWind.y * sn, uWind.x * sn + uWind.y * c);
    return s.x * w + s.y * vec2(-w.y, w.x);
}

void main() {
    setupCamera(uProbe);
    vec3 cam = gCam;
    vec3 toCam = cam - vP;
    float dist = length(toCam);
    vec3 V = toCam / dist;
    // Small cascades fade out where a pixel covers too much of their waves to
    // show them (no mipmaps): the pixel's footprint on the water, stretched at
    // grazing angles.
    float fp = dist * 2.0 * uUp.w * uRes.w / max(abs(V.y), 0.1);
    float w1 = 1.0 - smoothstep(uLengths.y / 64.0, uLengths.y / 10.0, fp);
    float w2 = 1.0 - smoothstep(uLengths.z / 64.0, uLengths.z / 10.0, fp);
    vec4 g = texture(uN0, vGrid / uLengths.x) + w1 * texture(uN1, vGrid / uLengths.y)
           + w2 * texture(uN2, vGrid / uLengths.z);
    // Ripples shorter than the last cascade: two layers of the detail tile at
    // different scales and angles drifting with the wind, so they never line up.
    // They vary as on a real sea: gusts sweep rough patches (cat's paws) across
    // it, calm slicks stay glassy, and the faces the wind hits carry more than
    // the troughs. Far away the mip chain averages them out and roughness
    // takes over.
    float t = uMisc.x;
    vec2 drift = uWind.xy * t * uWind.z * 0.35;
    float gust = vnoise((vGrid - drift) * 0.021) * 0.65 + vnoise((vGrid - drift * 1.3) * 0.067 + 3.1) * 0.35;
    float slick = smoothstep(0.62, 0.78, vnoise((vGrid - drift * 0.2) * 0.011 + 7.7));
    float face = clamp(0.55 + 0.35 * vP.y / max(uWind.z * uWind.z * 0.012, 0.3) - 0.5 * dot(g.xy, uWind.xy), 0.2, 1.4);
    float windy = smoothstep(1.0, 8.0, uWind.z) * mix(0.12, 1.6, smoothstep(0.3, 0.75, gust)) * face * (1.0 - 0.85 * slick);
    vec4 d1 = texture(uDetail, detailUv(vGrid, 0.0, 1.0) - vec2(t * 0.11, 0.0));
    vec4 d2 = texture(uDetail, detailUv(vGrid, 0.9, 0.57) - vec2(t * 0.07, t * 0.013));
    vec2 ripple = (detailSlope(d1.rg, 0.0) + 0.8 * detailSlope(d2.rg, 0.9)) * windy;
    vec3 N = normalize(vec3(-g.x / max(1.0 + g.z, 0.2) - ripple.x, 1.0, -g.y / max(1.0 + g.w, 0.2) - ripple.y));
    float lost = smoothstep(0.004, 0.06, fp);   // ripples the mips have averaged away
    float rough = (0.05 + 0.13 * (1.0 - w2) + 0.16 * (1.0 - w1)) * (1.0 - 0.5 * slick) + 0.05 * lost * windy;
    // Whitecaps come in groups, not evenly: the short cascades' waves break
    // on the crests of the long ones and where a gust is passing, so their
    // foam (which would otherwise repeat with their tiles) is gathered there.
    // Wave groups travel at half the waves' speed; the pattern never repeats.
    vec4 c0 = texture(uD0, vGrid / uLengths.x);
    float hs = max(0.01 * uWind.z * uWind.z, 0.3);   // roughly the sea's significant height
    float crestOf = smoothstep(-0.15, 0.45, c0.y / hs);
    vec2 gdrift = uWind.xy * t * uWind.z * 0.6;
    float groups = vnoise((vGrid - gdrift) * 0.0045 + 11.0) * 0.6 + vnoise((vGrid - gdrift) * 0.013 + 4.0) * 0.4;
    float gather = smoothstep(0.3, 0.66, groups) * (0.35 + 0.65 * crestOf) * (0.6 + 0.6 * smoothstep(0.35, 0.8, gust));
    // (the maps are mipmapped, so foam far away is averaged, not dropped: it stays visible to the horizon)
    float foam = c0.w + (0.8 * mix(0.7, 1.0, w1) * texture(uD1, vGrid / uLengths.y).w
                         + 0.2 * mix(0.3, 1.0, w2) * texture(uD2, vGrid / uLengths.z).w)
               * mix(0.0, 2.8, gather);
    foam *= uWind.w;
    // Foam's look, from the detail tile (drifting slowly, as foam rides the
    // surface): a little foam shows only as lace, bubbles strung along the
    // streaks; more fills the lace in; fresh whitecaps are nearly solid,
    // still pitted with holes.
    vec4 ft = texture(uDetail, detailUv(vGrid, 0.0, 0.8) * vec2(0.55, 1.0) - vec2(t * 0.03, 0.0));
    vec4 fb = texture(uDetail, detailUv(vGrid, 0.0, 3.1) * vec2(0.6, 1.0) + vec2(0.31, 0.17));
    float lace = fb.a * 0.75 + ft.a * 0.35;
    float bub = ft.b;
    float fa = clamp((lace * (0.4 + 0.8 * bub) + foam * 0.65 - 0.62) * 3.0, 0.0, 1.0) * clamp(foam * 3.0, 0.0, 1.0);
    fa = max(fa, clamp((foam - 1.2) * 1.5, 0.0, 1.0) * bub);
    // Far off a pixel spans foam and water alike: show the share it covers,
    // not a threshold the averaged foam never reaches.
    fa = mix(fa, clamp((foam - 0.2) * 0.6, 0.0, 0.6), smoothstep(0.05, 0.4, fp));
    // foam just under the surface, and in the gaps between bubbles, glows pale green
    float under = clamp(foam * 0.8 + 0.4 * lace * foam, 0.0, 1.0) * (1.0 - fa);
    vec3 amb = skyAmbient();
    vec3 L = uSun.xyz;

    // Which side of the water this pixel's ray starts on: the lens can straddle the waterline.
    float bob = texelFetch(uProbe, ivec2(0), 0).x;
    vec3 lp = lensPoint(cam, gl_FragCoord.xy);
    bool above = abs(cam.y - bob) > 5.0 ? cam.y > bob : lp.y > waterHeight(uD0, uD1, uD2, lp.xz);

    vec3 col;
    if (above) {
        float NdV = max(dot(N, V), 1e-3);
        float F = 0.02 + 0.98 * pow(1.0 - NdV, 5.0) * mix(1.0, 0.7, clamp(rough * 3.0 - 0.2, 0.0, 1.0));
        vec3 R = reflect(-V, N);
        // a reflected ray that leaves low is likely to meet the next wave, which
        // is dark: the sea mirrors itself as much as the sky at grazing angles
        float open = smoothstep(0.0, 0.5, R.y);
        R.y = abs(R.y);
        vec3 refl = mix(vec3(dot(uHorizon.rgb, vec3(0.3, 0.4, 0.3))) * 0.3 + uWater.rgb, skyMap(R), 0.12 + 0.88 * open);
        // the body of the water: what little light comes back up from inside
        vec3 body = uWater.rgb * (amb + uSunColor.rgb * uSun.w * max(L.y, 0.0) * 0.4);
        // light through the crests, strongest looking towards the sun
        vec3 Lh = normalize(vec3(L.x, 0.25, L.z));
        float back = pow(max(dot(-V, Lh), 0.0), 3.0);
        float crest = clamp(vP.y * 0.3 + 0.25, 0.0, 1.4) * (1.0 - 0.6 * NdV);
        vec3 sss = vec3(0.05, 0.085, 0.09) * uScatter.w * crest * (uSunColor.rgb * uSun.w * back * 1.6 + amb * 0.35);
        col = mix(body + sss + vec3(0.035, 0.06, 0.06) * under * (amb + uSunColor.rgb * uSun.w * 0.3), refl, F);
        // the sun's glint (GGX)
        vec3 H = normalize(L + V);
        float NdL = max(dot(N, L), 0.0), NdH = max(dot(N, H), 0.0);
        // through cloud the sun is a wide source: its highlight spreads and softens
        float rs = min(rough + 0.35 * (1.0 - uSunColor.w), 0.6);
        float a2 = rs * rs * rs * rs;
        float dd = NdH * NdH * (a2 - 1.0) + 1.0;
        float D = a2 / (PI * dd * dd);
        float vis = 0.5 / (NdL * (NdV * (1.0 - rough) + rough) + NdV * (NdL * (1.0 - rough) + rough) + 1e-4);
        float FL = 0.02 + 0.98 * pow(1.0 - max(dot(H, V), 0.0), 5.0);
        col += uSunColor.rgb * uSun.w * (0.12 + 0.88 * uSunColor.w) * D * vis * FL * NdL * (1.0 - fa);
        // the foam's relief: bubble walls lit, the pits between them shadowed
        float relief = 0.72 + 0.38 * bub;
        vec3 foamCol = vec3(0.86, 0.90, 0.92) * relief * (amb * (0.55 + 0.45 * N.y) + uSunColor.rgb * uSun.w * max(dot(N, L), 0.0) * 0.7);
        col = mix(col, foamCol, fa);
        // haze towards the horizon, the same colour as the sky there
        float h = 1.0 - exp(-dist / uHorizon.w);
        col = mix(col, skyMap(normalize(vec3(-V.x, 0.03, -V.z))), h);
    } else {
        // From below: the sky only within Snell's window; outside it, the
        // surface mirrors the deep (total internal reflection).
        vec3 I = -V;
        vec3 T = refract(I, -N, 1.333);
        vec3 deep = underwaterLight(reflect(I, -N), max(-vP.y, 0.0) + 1.0);
        if (dot(T, T) < 1e-6) {
            col = deep;
        } else {
            float F = 0.02 + 0.98 * pow(1.0 - clamp(dot(T, N), 0.0, 1.0), 5.0);
            col = (1.0 - F) * skyMap(T) + F * deep;
        }
        col = mix(col, amb * vec3(0.42, 0.5, 0.52), fa * 0.85);   // foam blocks the light
    }
    o = vec4(col, 1.0);
}
#endif
