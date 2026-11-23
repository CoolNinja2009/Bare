#version 450
// bare.glsl: Bare's one shader (ARCHITECTURE §10). Compiled once per stage:
// -DBARE_VERTEX or -DBARE_FRAGMENT. Every rect, glyph and image is one instance
// of a quad. Coordinates are physical pixels, y down.
//
// Bindings follow SDL3 GPU's SPIR-V rules: vertex uniforms set 1, fragment
// samplers set 2.

#ifdef BARE_VERTEX

layout(location = 0) in vec4 i_rect;          // x, y, w, h
layout(location = 1) in vec4 i_radii;         // tl, tr, br, bl
layout(location = 2) in vec4 i_fill;          // rgba, straight alpha (tint for images, colour for glyphs)
layout(location = 3) in vec4 i_border_color;
layout(location = 4) in vec4 i_shadow_color;
layout(location = 5) in vec4 i_shadow;        // offset x, offset y, blur, spread
layout(location = 6) in vec4 i_uv;            // u0, v0, u1, v1
layout(location = 7) in vec4 i_params;        // border width, mode (0 box, 1 glyph, 2 image), -, -

layout(set = 1, binding = 0) uniform Viewport { vec4 u_viewport; };   // width, height

layout(location = 0) out vec2 v_pos;
layout(location = 1) flat out vec4 v_rect;
layout(location = 2) flat out vec4 v_radii;
layout(location = 3) flat out vec4 v_fill;
layout(location = 4) flat out vec4 v_border_color;
layout(location = 5) flat out vec4 v_shadow_color;
layout(location = 6) flat out vec4 v_shadow;
layout(location = 7) flat out vec4 v_uv;
layout(location = 8) flat out vec4 v_params;

void main() {
    // Triangle strip: 0 = top-left, 1 = top-right, 2 = bottom-left, 3 = bottom-right.
    vec2 corner = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));

    // Grow the quad to cover the anti-aliased edge and the shadow. Glyphs are
    // drawn exactly (their quads are pixel-aligned bitmaps).
    float margin = 1.0;
    if (i_params.y > 0.5 && i_params.y < 1.5) margin = 0.0;
    if (i_shadow_color.a > 0.0) {
        float sigma = i_shadow.z * 0.5;
        margin = max(margin, i_shadow.w + 3.0 * sigma + max(abs(i_shadow.x), abs(i_shadow.y)) + 1.0);
    }
    vec2 p = i_rect.xy - margin + corner * (i_rect.zw + 2.0 * margin);

    v_pos = p;
    v_rect = i_rect;
    v_radii = i_radii;
    v_fill = i_fill;
    v_border_color = i_border_color;
    v_shadow_color = i_shadow_color;
    v_shadow = i_shadow;
    v_uv = i_uv;
    v_params = i_params;
    gl_Position = vec4(p.x / u_viewport.x * 2.0 - 1.0, 1.0 - p.y / u_viewport.y * 2.0, 0.0, 1.0);
}

#endif

#ifdef BARE_FRAGMENT

layout(location = 0) in vec2 v_pos;
layout(location = 1) flat in vec4 v_rect;
layout(location = 2) flat in vec4 v_radii;
layout(location = 3) flat in vec4 v_fill;
layout(location = 4) flat in vec4 v_border_color;
layout(location = 5) flat in vec4 v_shadow_color;
layout(location = 6) flat in vec4 v_shadow;
layout(location = 7) flat in vec4 v_uv;
layout(location = 8) flat in vec4 v_params;

layout(set = 2, binding = 0) uniform sampler2D u_tex;

layout(location = 0) out vec4 o_color;   // premultiplied alpha

// Signed distance to a rounded box centred at 0 with half-size h and
// per-corner radii r (tl, tr, br, bl). y points down.
float sd_round_box(vec2 p, vec2 h, vec4 r) {
    float rr = p.x > 0.0 ? (p.y > 0.0 ? r.z : r.y) : (p.y > 0.0 ? r.w : r.x);
    vec2 q = abs(p) - h + rr;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - rr;
}

// erf approximation (Abramowitz & Stegun 7.1.27), max error ~5e-4.
float erf_approx(float x) {
    float s = sign(x), a = abs(x);
    float t = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
    t *= t;
    return s - s / (t * t);
}

vec4 premul(vec4 c) { return vec4(c.rgb * c.a, c.a); }

void main() {
    vec2 half_size = v_rect.zw * 0.5;
    vec2 p = v_pos - (v_rect.xy + half_size);
    vec2 uv = mix(v_uv.xy, v_uv.zw, (v_pos - v_rect.xy) / max(v_rect.zw, vec2(1.0)));
    int mode = int(v_params.y + 0.5);

    if (mode == 1) {   // glyph: R8 coverage from the atlas
        o_color = premul(v_fill) * texture(u_tex, uv).r;
        return;
    }

    float d = sd_round_box(p, half_size, v_radii);
    float outer = clamp(0.5 - d, 0.0, 1.0);   // 1 px anti-aliasing (units are pixels)

    vec4 inside;
    if (mode == 2) {   // image, straight-alpha RGBA, tinted, rounded mask
        inside = premul(texture(u_tex, uv) * v_fill);
    } else {
        inside = premul(v_fill);
        float bw = v_params.x;
        if (bw > 0.0) {
            float di = sd_round_box(p, max(half_size - bw, vec2(0.0)), max(v_radii - bw, vec4(0.0)));
            inside = mix(premul(v_border_color), inside, clamp(0.5 - di, 0.0, 1.0));
        }
    }
    vec4 color = inside * outer;

    if (v_shadow_color.a > 0.0) {
        // A Gaussian-blurred rounded box: erf of the distance to the offset,
        // spread shape (exact along straight edges, close at corners).
        // Like CSS box-shadow, it isn't drawn under the box itself.
        float sigma = max(v_shadow.z * 0.5, 0.5);
        float sd = sd_round_box(p - v_shadow.xy, max(half_size + v_shadow.w, vec2(0.0)),
                                max(v_radii + v_shadow.w, vec4(0.0)));
        float a = v_shadow_color.a * (0.5 - 0.5 * erf_approx(sd / (sigma * 1.41421356))) * (1.0 - outer);
        color += vec4(v_shadow_color.rgb * a, a) * (1.0 - color.a);
    }
    o_color = color;
}

#endif
