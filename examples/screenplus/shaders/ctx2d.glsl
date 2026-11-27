#version 450
// ctx2d.glsl: HTML canvas 2D for ports of pages (see ../ctx2d.h), one instance a piece:
//   kind 0: a rectangle of paint (a fillRect, or a run of one coverage along a row of a path)
//   kind 1: a glyph from the atlas (coverage in its red channel), times the paint
//   kind 2: a picture (input 2 + slot, premultiplied) mapped over a rectangle (drawImage), times alpha
// Coverage is the area each pixel shares with the rectangle, times the run's coverage.
// Paints are a solid premultiplied colour, or a gradient from the table: its inverse
// transform (pixels to the canvas's user space), geometry and a row of the ramp
// texture (straight colours, premultiplied here, as Skia's unpremultiplied
// interpolation). Output is premultiplied: blend BARE_BLEND_OVER (source-over) or
// BARE_BLEND_ADD (lighter).
layout(set = BARE_PARAMS_SET, binding = 0) uniform Params {
    vec4 res;        // target width, height; atlas width, height
    vec4 tab[255];   // paints, 4 each: inverse a b c d | e f kind ramp-v | geometry 0-3 | geometry 4-5, alpha
};

#ifdef BARE_VERTEX
layout(location = 0) in vec4 i_a;   // x0 y0 x1 y1, pixels
layout(location = 1) in vec4 i_b;   // kind, coverage, paint (-1: i_c is the colour), picture slot
layout(location = 2) in vec4 i_c;   // premultiplied colour | (pictures) alpha in .a
layout(location = 3) in vec4 i_d;   // atlas pixel = d.xy + the pixel (glyphs) | picture uv x0 y0 x1 y1
layout(location = 0) out vec2 v_pos;
layout(location = 1) flat out vec4 v_a;
layout(location = 2) flat out vec4 v_b;
layout(location = 3) flat out vec4 v_c;
layout(location = 4) flat out vec4 v_d;

void main() {
    const vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(0, 1), vec2(1, 0), vec2(1, 1));
    vec2 p = mix(floor(i_a.xy), ceil(i_a.zw), corners[gl_VertexIndex]);
    v_pos = p, v_a = i_a, v_b = i_b, v_c = i_c, v_d = i_d;
    gl_Position = vec4(p.x / res.x * 2.0 - 1.0, 1.0 - p.y / res.y * 2.0, 0.0, 1.0);
}
#else
layout(set = 2, binding = 0) uniform sampler2D ramp;
layout(set = 2, binding = 1) uniform sampler2D atlas;
layout(set = 2, binding = 2) uniform sampler2D pic0;
layout(set = 2, binding = 3) uniform sampler2D pic1;
layout(set = 2, binding = 4) uniform sampler2D pic2;
layout(set = 2, binding = 5) uniform sampler2D pic3;
layout(set = 2, binding = 6) uniform sampler2D pic4;
layout(set = 2, binding = 7) uniform sampler2D pic5;
layout(location = 0) in vec2 v_pos;
layout(location = 1) flat in vec4 v_a;
layout(location = 2) flat in vec4 v_b;
layout(location = 3) flat in vec4 v_c;
layout(location = 4) flat in vec4 v_d;
layout(location = 0) out vec4 o_color;

const float TAU = 6.283185307179586;

vec4 paint(vec2 px) {
    int i = int(v_b.z) * 4;
    if (i < 0) return v_c;
    vec4 m = tab[i], n = tab[i + 1], g = tab[i + 2], h = tab[i + 3];
    vec2 p = vec2(m.x * px.x + m.z * px.y + n.x, m.y * px.x + m.w * px.y + n.y);
    int kind = int(n.z);
    float t = 0.0;
    if (kind == 1) {   // linear: g = x0 y0 x1 y1
        vec2 d = g.zw - g.xy;
        float dd = dot(d, d);
        t = dd > 0.0 ? dot(p - g.xy, d) / dd : 0.0;
    } else if (kind == 2) {   // radial (two circles): g = x0 y0 r0 x1, h = y1 r1
        vec2 c0 = g.xy, cd = vec2(g.w, h.x) - c0, pd = p - c0;
        float r0 = g.z, dr = h.y - g.z;
        float a = dot(cd, cd) - dr * dr, b = dot(pd, cd) + r0 * dr, c = dot(pd, pd) - r0 * r0;
        if (abs(a) < 1e-6) {
            if (abs(b) < 1e-9) return vec4(0.0);
            t = c / (2.0 * b);
            if (r0 + t * dr < 0.0) return vec4(0.0);
        } else {
            float disc = b * b - a * c;
            if (disc < 0.0) return vec4(0.0);
            float s = sqrt(disc), t1 = (b + s) / a, t2 = (b - s) / a;
            float hi = max(t1, t2), lo = min(t1, t2);
            if (r0 + hi * dr >= 0.0) t = hi;
            else if (r0 + lo * dr >= 0.0) t = lo;
            else return vec4(0.0);
        }
    } else {   // conic: g = start angle, x, y
        vec2 d = p - g.yz;
        t = fract((atan(d.y, d.x) - g.x) / TAU);
    }
    vec4 c = texture(ramp, vec2((clamp(t, 0.0, 1.0) * 255.0 + 0.5) / 256.0, n.w));
    return vec4(c.rgb * c.a, c.a) * h.z;
}

vec4 picture(int slot, vec2 uv) {
    if (slot == 0) return texture(pic0, uv);
    if (slot == 1) return texture(pic1, uv);
    if (slot == 2) return texture(pic2, uv);
    if (slot == 3) return texture(pic3, uv);
    if (slot == 4) return texture(pic4, uv);
    return texture(pic5, uv);
}

void main() {
    vec2 px = floor(v_pos);
    vec2 lo = max(v_a.xy, px), hi = min(v_a.zw, px + 1.0);
    vec2 ov = clamp(hi - lo, 0.0, 1.0);
    float cov = ov.x * ov.y * v_b.y;
    int kind = int(v_b.x);
    vec4 col;
    if (kind == 2) {
        vec2 f = (px + 0.5 - v_a.xy) / max(v_a.zw - v_a.xy, vec2(1e-6));
        col = picture(int(v_b.w), mix(v_d.xy, v_d.zw, f)) * v_c.a;
    } else {
        col = paint(px + 0.5);
        if (kind == 1) cov *= texelFetch(atlas, ivec2(v_d.xy + px), 0).r;
    }
    o_color = col * cov;
}
#endif
