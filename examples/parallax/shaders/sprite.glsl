#version 450
// sprite.glsl: one CSS box of the page as the compositor draws it: a box
// (box_w x box_h CSS pixels) under a 2D transform, painted with a picture at
// some rectangle of it (background-size and -position; across, repeated, for
// repeat-x) or with a colour, at some opacity. Pictures are premultiplied mip
// atlases (see ../../screenplus/mips.h), sampled trilinearly as a scaled
// picture is; the result goes over with BARE_BLEND_OVER.
layout(set = BARE_PARAMS_SET, binding = 0) uniform Params {
    vec4 m;        // the transform's 2 x 2 part: a b c d (x' = a x + c y + e, y' = b x + d y + f)
    vec4 t;        // e, f, box w, h
    vec4 img;      // the picture's rectangle in the box: x, y, w, h
    vec4 opt;      // opacity, repeat across, colour (1 solid, 2 fading out down), picture levels
    vec4 color;    // the solid colour, premultiplied
    vec4 sizes;    // atlas w, h, canvas w, h
    vec4 lv[12];   // per level: x, y, w, h in atlas pixels
};

#ifdef BARE_VERTEX
layout(location = 0) out vec2 v_local;

void main() {
    const vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(0, 1), vec2(1, 0), vec2(1, 1));
    vec2 l = corners[gl_VertexIndex] * t.zw;
    vec2 p = vec2(m.x * l.x + m.z * l.y + t.x, m.y * l.x + m.w * l.y + t.y);
    v_local = l;
    gl_Position = vec4(p.x / sizes.z * 2.0 - 1.0, 1.0 - p.y / sizes.w * 2.0, 0.0, 1.0);
}
#else
layout(set = 2, binding = 0) uniform sampler2D u_atlas;
layout(location = 0) in vec2 v_local;
layout(location = 0) out vec4 o_color;

// one level, bilinear, its texels clamped at the edges (wrapped across when repeating)
vec4 level(vec4 r, vec2 uv, bool wrap) {
    vec2 p = uv * r.zw - 0.5, f = fract(p);
    ivec2 i0 = ivec2(floor(p)), sz = ivec2(r.zw);
    vec4 s[4];
    for (int k = 0; k < 4; k++) {
        ivec2 q = i0 + ivec2(k & 1, k >> 1);
        q.x = wrap ? (q.x % sz.x + sz.x) % sz.x : clamp(q.x, 0, sz.x - 1);
        q.y = clamp(q.y, 0, sz.y - 1);
        s[k] = texelFetch(u_atlas, ivec2(r.xy) + q, 0);
    }
    return mix(mix(s[0], s[1], f.x), mix(s[2], s[3], f.x), f.y);
}

void main() {
    if (opt.z > 1.5) { o_color = color * opt.x * (1.0 - v_local.y / t.w); return; }   // a gradient to transparent, down
    if (opt.z > 0.5) { o_color = color * opt.x; return; }
    vec2 uv = (v_local - img.xy) / img.zw;
    vec2 dx = dFdx(uv) * lv[0].zw, dy = dFdy(uv) * lv[0].zw;
    bool wrap = opt.y > 0.5;
    if ((!wrap && (uv.x < 0.0 || uv.x > 1.0)) || uv.y < 0.0 || uv.y > 1.0) discard;
    int nlv = int(opt.w);
    float lod = clamp(log2(max(max(length(dx), length(dy)), 1e-8)), 0.0, float(nlv - 1));
    int l0 = int(floor(lod)), l1 = min(l0 + 1, nlv - 1);
    if (wrap) uv.x = fract(uv.x);
    vec4 c = mix(level(lv[l0], uv, wrap), level(lv[l1], uv, wrap), lod - float(l0));
    o_color = c * opt.x;
}
#endif
