#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// detail.glsl: the sea's fine print, baked once into a tiling, mipmapped
// canvas a few metres across (x runs down the wind):
//   rg  slopes of ripples 3-30 cm long, too short for the FFT cascades:
//       a hundred waves whose wave numbers are whole numbers of cycles per
//       tile, so the tile repeats without a seam
//   b   foam: a white sheet pitted with round holes of three sizes, the
//       small ones clustered, the whole warped so nothing lines up
//   a   foam lace: ridges of noise, the streaks foam is drawn into
// The mip chain and anisotropic filtering keep it calm far away.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
layout(set = 3, binding = 0) uniform Params {
    vec4 uTile;   // size (m), ripple steepness, -, -
};

// Voronoi with n cells per tile, wrapping: distance to the nearest seed, the
// second nearest, and a random number for the nearest's cell.
vec3 cells(vec2 uv, vec2 n, float seed) {
    vec2 p = uv * n, i = floor(p), f = p - i;
    float d1 = 9.0, d2 = 9.0, id = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            vec2 c = i + vec2(x, y);
            vec2 w = mod(c, n);
            vec2 s = vec2(x, y) + hash22(w + seed * 17.31) * 0.9 + 0.05 - f;
            float d = dot(s, s);
            if (d < d1) { d2 = d1; d1 = d; id = hash12(w + seed * 5.7 + 0.3); } else if (d < d2) d2 = d;
        }
    return vec3(sqrt(vec2(d1, d2)), id);
}

// Value noise that wraps every n cells.
float tnoise(vec2 uv, vec2 n) {
    vec2 p = uv * n, i = floor(p), f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    float a = hash12(mod(i, n)), b = hash12(mod(i + vec2(1, 0), n));
    float c = hash12(mod(i + vec2(0, 1), n)), d = hash12(mod(i + vec2(1, 1), n));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float ridges(vec2 uv, vec2 n) {
    float r = 0.0, amp = 0.55;
    for (int o = 0; o < 4; o++) {
        r += amp * pow(1.0 - abs(tnoise(uv + float(o) * 0.37, n) * 2.0 - 1.0), 3.0);
        n *= 2.0;
        amp *= 0.5;
    }
    return r;
}

// A burst bubble: each cell's its own size (some none at all), a crisp edge.
float hole(vec3 c, float lo, float hi) {
    float r = mix(lo, hi, c.z * c.z) * step(0.18, c.z);
    return smoothstep(r, r * 0.8, c.x);
}

void main() {
    vec2 uv = v_uv;
    // ripples: steepness spread evenly, directions mostly with the wind
    const int WAVES = 160;
    vec2 slope = vec2(0.0);
    for (int i = 0; i < WAVES; i++) {
        uint s = uint(i) * 3u + 11u;
        float th = (u01(pcg(s)) - 0.5) * 3.2;
        if (u01(pcg(s + 1u)) < 0.15) th += PI;                      // a few run against it
        float n = exp(mix(log(13.0), log(130.0), u01(pcg(s + 2u))));  // cycles per tile
        vec2 k = floor(n * vec2(cos(th), sin(th)) + 0.5);
        if (dot(k, k) < 1.0) continue;
        float phase = TAU * u01(pcg(s + 3u * 7u + 5u));
        // the shortest carry less: a grain of tiny equal ripples reads as noise
        float steep = uTile.y * inversesqrt(float(WAVES)) * (0.6 + 0.8 * u01(pcg(s * 5u + 1u))) * pow(13.0 / n, 0.12);
        // (the cycle's fraction first: GPUs' cos of large angles differ, D3D12 from Vulkan)
        slope += steep * normalize(k) * cos(TAU * fract(dot(k, uv)) + phase);
    }
    // foam: holes where the bubbles have burst, on a warped grid so the cells aren't seen
    vec2 warp = vec2(tnoise(uv, vec2(9.0, 15.0)), tnoise(uv + 0.5, vec2(9.0, 15.0))) - 0.5;
    vec2 wv = uv + warp * 0.045;
    float cluster = tnoise(uv, vec2(6.0, 11.0));
    float holeBig = hole(cells(wv, vec2(12.0, 22.0), 1.0), 0.15, 0.45) * smoothstep(0.3, 0.65, cluster);
    float holeSmall = hole(cells(wv, vec2(38.0, 66.0), 2.0), 0.08, 0.42);
    float holeTiny = hole(cells(wv, vec2(96.0, 170.0), 3.0), 0.05, 0.4);
    float bubbles = (1.0 - holeBig) * (1.0 - 0.75 * holeSmall * (0.4 + 0.6 * cluster)) * (1.0 - 0.45 * holeTiny);
    bubbles *= 0.8 + 0.2 * tnoise(uv, vec2(150.0, 260.0));
    float lace = ridges(uv, vec2(3.0, 7.0));
    o = vec4(slope, clamp(bubbles, 0.0, 1.0), clamp(lace, 0.0, 1.0));
}
