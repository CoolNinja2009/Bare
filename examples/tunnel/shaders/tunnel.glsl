#version 450
// tunnel.glsl: the page's tunnel.frag (rocksdanister's Music Tunnel), with
// WebGL's texture sampling done by hand: mirrored repeat and trilinear
// mipmaps from a mip atlas (every level of a picture side by side), the level
// picked from the coordinates' screen derivatives as the GPU does.
layout(set = 2, binding = 0) uniform sampler2D u_atlas0;
layout(set = 2, binding = 1) uniform sampler2D u_atlas1;
layout(set = 3, binding = 0) uniform Params {
    vec4 res;          // width, height, time, speed
    vec4 opts;         // blend, square, center, center radius
    vec4 center_color;
    vec4 info;         // levels of tex0, levels of tex1, atlas0 size (w, h)
    vec4 info1;        // atlas1 size (w, h), has tex0, has tex1
    vec4 lv0[12];      // per level: x, y, w, h in atlas pixels
    vec4 lv1[12];
};
layout(location = 0) out vec4 o_color;

const float kPi = 3.1415927;

float mirror1(float x) { float m = mod(x, 2.0); return m > 1.0 ? 2.0 - m : m; }

vec3 level_texel(sampler2D atlas, vec2 asize, vec4 r, vec2 uv) {
    // uv mirrored into 0..1, flipped (three.js flipY: row 0 of the picture at v = 1)
    vec2 p = vec2(mirror1(uv.x), 1.0 - mirror1(uv.y)) * r.zw;
    p = clamp(p, vec2(0.5), r.zw - 0.5);   // mirrored edges repeat the edge texel: the same as clamping
    return textureLod(atlas, (r.xy + p) / asize, 0.0).rgb;
}

vec3 sample_mip(sampler2D atlas, vec2 asize, int nlv, vec2 uv, vec2 dx, vec2 dy, bool use1) {
    vec4 r0 = use1 ? lv1[0] : lv0[0];
    vec2 tx = dx * r0.zw, ty = dy * r0.zw;
    float rho = max(length(tx), length(ty));
    float lod = clamp(log2(max(rho, 1e-8)), 0.0, float(nlv - 1));
    int l0 = int(floor(lod));
    int l1 = min(l0 + 1, nlv - 1);
    float f = lod - float(l0);
    vec4 a = use1 ? lv1[l0] : lv0[l0];
    vec4 b = use1 ? lv1[l1] : lv0[l1];
    return mix(level_texel(atlas, asize, a, uv), level_texel(atlas, asize, b, uv), f);
}

void main() {
    vec2 p = (2. * gl_FragCoord.xy - res.xy) / res.y;
    float a = atan(p.y, p.x);
    float r = 0.;
    if (opts.y > 0.5) {
        vec2 p2 = p * p, p4 = p2 * p2, p8 = p4 * p4;
        r = pow(p8.x + p8.y, 1.0 / 8.0);
    } else {
        r = length(p);
    }
    vec2 uv = vec2(0.3 / r + 0.2 * res.z * res.w, 0.5 + a / kPi);
    vec2 dx = dFdx(uv), dy = dFdy(uv);
    vec3 col = info1.z > 0.5 ? sample_mip(u_atlas0, info.zw, int(info.x), uv, dx, dy, false) : vec3(0.0);
    vec3 col1 = info1.w > 0.5 ? sample_mip(u_atlas1, info1.xy, int(info.y), uv, dx, dy, true) : vec3(0.0);
    col = mix(col, col1, opts.x);
    if (opts.z > 0.5) {
        float fadeAmount = 1.0 - smoothstep(0.0, opts.w, r);
        col = mix(col, center_color.rgb, fadeAmount);
    }
    o_color = vec4(col, 1.);
}
