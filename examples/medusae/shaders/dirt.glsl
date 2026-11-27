#version 450
// dirt.glsl: the page's LensDirtPass: quads of its dirt picture (a 10 x 10
// grid of blobs, one cell a quad) over the frame where a click nudged the
// jellyfish, each fading as its alpha decays; AlphaMaterial (white, opacity
// 0.5), added (blendFunc SRC_ALPHA, ONE). Camera: x -1..1 across, y -h/w..h/w.
layout(set = BARE_PARAMS_SET, binding = 0) uniform Params {
    vec4 misc;       // the camera's half height (h / w)
};

#ifdef BARE_VERTEX
layout(location = 0) in vec4 i_c01;   // corners 0, 1: x y x y
layout(location = 1) in vec4 i_c23;   // corners 2, 3
layout(location = 2) in vec4 i_uv;    // the cell: u0 v0 u1 v1 (v up)
layout(location = 3) in vec4 i_alpha;
layout(location = 0) out vec2 vUv;
layout(location = 1) out float vAlpha;

void main() {
    const int corner[6] = int[](0, 1, 2, 2, 3, 0);
    int c = corner[gl_VertexIndex];
    vec2 p = c == 0 ? i_c01.xy : c == 1 ? i_c01.zw : c == 2 ? i_c23.xy : i_c23.zw;
    vec2 uv = vec2(c == 1 || c == 2 ? i_uv.z : i_uv.x, c >= 2 ? i_uv.w : i_uv.y);
    vUv = vec2(uv.x, 1.0 - uv.y);   // the picture's rows run down
    vAlpha = i_alpha.x;
    gl_Position = vec4(p.x, p.y / misc.x, 0.5, 1.0);
}
#else
layout(set = 2, binding = 0) uniform sampler2D map;
layout(location = 0) in vec2 vUv;
layout(location = 1) in float vAlpha;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 d = vec4(1.0, 1.0, 1.0, 0.5 * vAlpha) * texture(map, vUv);
    float a = clamp(d.a, 0.0, 1.0);
    o_color = vec4(clamp(d.rgb, 0.0, 1.0) * a, a);
}
#endif
