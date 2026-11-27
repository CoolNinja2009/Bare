#version 450
// canvas2d.glsl: canvas 2D's fills, strokes and text for ports of pages, one
// instance a shape (see ../canvas2d.h):
//   kind 0: a rectangle, coverage = the exact area of each pixel it covers
//   kind 1: a line segment of some width with butt ends, coverage from distance
//   kind 2: a glyph mask from the atlas, coverage = the mask
//   kind 3: a picture (input 1, straight alpha) stretched over a rectangle, times the colour
//   kind 4: a triangle, grown by some width with round corners (fill and stroke), coverage from distance
//   kind 5: a glyph mask turned by an angle (radians, clockwise on screen) about a point (in clip), filtered
//   kind 6: a rectangle tinted by one channel (b.y) of the picture, which lies at pic on the target (a glow)
//   kind 7: a glyph mask as premultiplied colour (mask layers, blended BARE_BLEND_OVER)
//   kind 8: a ring (centre a.xy, radius a.z, width a.w) with b.y dots (radius b.z, turned b.w) on its inner edge
// each also clipped to a rectangle (the area a pixel shares with it).
// Colours are straight RGBA; blend with BARE_BLEND_ALPHA, as canvas's source-over.
layout(set = BARE_PARAMS_SET, binding = 0) uniform Params { vec4 res; vec4 pic; };   // canvas w, h, atlas w, h; picture x, y, w, h (kind 6)

#ifdef BARE_VERTEX
layout(location = 0) in vec4 i_a;       // rect: x0 y0 x1 y1 | line: x0 y0 x1 y1 | glyph: x y w h
layout(location = 1) in vec4 i_b;       // kind, line width (triangle: growth), (glyph) atlas x, y | (triangle) x2 y2
layout(location = 2) in vec4 i_color;
layout(location = 3) in vec4 i_clip;    // x0 y0 x1 y1: drawn only inside (canvas clip()), anti-aliased
layout(location = 0) out vec2 v_pos;    // canvas pixels
layout(location = 1) flat out vec4 v_a;
layout(location = 2) flat out vec4 v_b;
layout(location = 3) out vec4 v_color;
layout(location = 4) out vec2 v_uv;
layout(location = 5) flat out vec4 v_clip;

void main() {
    const vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(0, 1), vec2(1, 0), vec2(1, 1));
    vec2 c = corners[gl_VertexIndex];
    vec2 p;
    int kind = int(i_b.x);
    if (kind == 1) {   // a quad around the segment, a pixel wider on every side
        vec2 d = i_a.zw - i_a.xy;
        float len = max(length(d), 1e-4);
        vec2 t = d / len, n = vec2(-t.y, t.x);
        float hw = i_b.y * 0.5 + 1.0;
        p = i_a.xy + t * ((c.x * (len + 2.0)) - 1.0) + n * ((c.y * 2.0 - 1.0) * hw);
    } else if (kind == 0 || kind == 6) {
        p = mix(floor(i_a.xy), ceil(i_a.zw), c);
    } else if (kind == 8) {
        float e = i_a.z + i_b.z + 1.0;
        p = i_a.xy + (c * 2.0 - 1.0) * e;
    } else if (kind == 4) {   // its bounds, a pixel wider than it grows
        vec2 lo = min(min(i_a.xy, i_a.zw), i_b.zw) - i_b.y - 1.0, hi = max(max(i_a.xy, i_a.zw), i_b.zw) + i_b.y + 1.0;
        p = mix(lo, hi, c);
    } else if (kind == 2 || kind == 5 || kind == 7) {
        p = i_a.xy + c * i_a.zw;
        v_uv = (i_b.zw + c * i_a.zw) / res.zw;
        if (kind == 5) {   // a pixel of margin, so the filtered edge isn't cut
            vec2 cc = c * 2.0 - 1.0;
            p += cc;
            v_uv += cc / res.zw;
            float s = sin(i_b.y), k = cos(i_b.y);
            vec2 d = p - i_clip.xy;
            p = i_clip.xy + vec2(d.x * k - d.y * s, d.x * s + d.y * k);
        }
    } else {
        p = i_a.xy + c * i_a.zw;
        v_uv = c;
    }
    v_pos = p;
    v_a = i_a;
    v_b = i_b;
    v_color = i_color;
    v_clip = i_clip;
    gl_Position = vec4(p.x / res.x * 2.0 - 1.0, 1.0 - p.y / res.y * 2.0, 0.0, 1.0);
}
#else
layout(set = 2, binding = 0) uniform sampler2D atlas;
layout(set = 2, binding = 1) uniform sampler2D picture;
layout(location = 0) in vec2 v_pos;
layout(location = 1) flat in vec4 v_a;
layout(location = 2) flat in vec4 v_b;
layout(location = 3) in vec4 v_color;
layout(location = 4) in vec2 v_uv;
layout(location = 5) flat in vec4 v_clip;
layout(location = 0) out vec4 o_color;

// signed distance to a triangle (negative inside)
float tri_sd(vec2 p, vec2 a, vec2 b, vec2 c) {
    vec2 e0 = b - a, e1 = c - b, e2 = a - c, v0 = p - a, v1 = p - b, v2 = p - c;
    vec2 q0 = v0 - e0 * clamp(dot(v0, e0) / max(dot(e0, e0), 1e-12), 0.0, 1.0);
    vec2 q1 = v1 - e1 * clamp(dot(v1, e1) / max(dot(e1, e1), 1e-12), 0.0, 1.0);
    vec2 q2 = v2 - e2 * clamp(dot(v2, e2) / max(dot(e2, e2), 1e-12), 0.0, 1.0);
    float s = sign(e0.x * e2.y - e0.y * e2.x);
    vec2 d = min(min(vec2(dot(q0, q0), s * (v0.x * e0.y - v0.y * e0.x)), vec2(dot(q1, q1), s * (v1.x * e1.y - v1.y * e1.x))),
                 vec2(dot(q2, q2), s * (v2.x * e2.y - v2.y * e2.x)));
    return -sqrt(d.x) * sign(d.y);
}

void main() {
    int kind = int(v_b.x);
    float cov;
    vec2 px = floor(v_pos);   // this pixel: px .. px + 1
    if (kind == 0) {
        vec2 lo = max(v_a.xy, px), hi = min(v_a.zw, px + 1.0);
        vec2 s = max(hi - lo, 0.0);
        cov = s.x * s.y;
    } else if (kind == 1) {
        vec2 d = v_a.zw - v_a.xy;
        float len = max(length(d), 1e-4);
        vec2 t = d / len, n = vec2(-t.y, t.x);
        vec2 q = px + 0.5 - v_a.xy;
        float along = dot(q, t), across = abs(dot(q, n));
        float hw = v_b.y * 0.5;
        // a pixel's box filter, separately across and along the stroke
        float ca = clamp(min(across + 0.5, hw) - max(across - 0.5, -hw), 0.0, 1.0);
        float cl = clamp(min(along + 0.5, len) - max(along - 0.5, 0.0), 0.0, 1.0);
        cov = ca * cl;
    } else if (kind == 4) {
        cov = clamp(0.5 + v_b.y - tri_sd(px + 0.5, v_a.xy, v_a.zw, v_b.zw), 0.0, 1.0);
    } else if (kind == 2) {
        cov = texture(atlas, v_uv).r;
    } else if (kind == 7) {
        o_color = v_color * texture(atlas, v_uv).r;
        return;
    } else if (kind == 6) {
        vec2 lo = max(v_a.xy, px), hi = min(v_a.zw, px + 1.0);
        vec2 s = max(hi - lo, 0.0);
        vec4 g = texture(picture, (v_pos - pic.xy) / pic.zw);
        int ch = int(v_b.y);
        cov = s.x * s.y * (ch == 0 ? g.r : ch == 1 ? g.g : ch == 2 ? g.b : g.a);
    } else if (kind == 8) {
        vec2 q = px + 0.5 - v_a.xy;
        float d = length(q), r = v_a.z, bw = v_a.w;
        float ring = clamp(r - d + 0.5, 0.0, 1.0) - clamp(r - bw - d + 0.5, 0.0, 1.0);
        float dot_ = 0.0, ri = r - bw;
        int n = int(v_b.y);
        for (int k = 0; k < n; k++) {
            float a = v_b.w + 6.28318530718 * float(k) / float(n);
            vec2 e = vec2(sin(a), -cos(a)) * ri;
            dot_ = max(dot_, clamp(v_b.z - length(q - e) + 0.5, 0.0, 1.0));
        }
        cov = 1.0 - (1.0 - ring) * (1.0 - 0.75 * dot_);
    } else if (kind == 5) {
        o_color = vec4(v_color.rgb, v_color.a * texture(atlas, v_uv).r);
        return;
    } else {
        vec4 t = texture(picture, v_uv) * v_color;
        vec2 clo = max(v_clip.xy, px), chi = min(v_clip.zw, px + 1.0);
        vec2 cs = max(chi - clo, 0.0);
        o_color = vec4(t.rgb, t.a * cs.x * cs.y);
        return;
    }
    vec2 clo = max(v_clip.xy, px), chi = min(v_clip.zw, px + 1.0);
    vec2 cs = max(chi - clo, 0.0);
    o_color = vec4(v_color.rgb, v_color.a * cov * cs.x * cs.y);
}
#endif
