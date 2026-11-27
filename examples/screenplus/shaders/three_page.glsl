#version 450
// three_page.glsl: the page around a three.js canvas made with alpha: true:
// the body's CSS radial-gradient (ellipse, farthest-corner, interpolated in
// sRGB) and the WebGL canvas (premultiplied, 8-bit) composited over it at the
// container's opacity. The scene canvas may be drawn k times larger (k x k
// supersampling, averaged as a multisample resolve averages its samples).
layout(set = BARE_PARAMS_SET, binding = 0) uniform Params {
    vec4 misc;    // opacity, supersampling k, target w, h
    vec4 inner;   // the gradient's first stop, sRGB 0..1
    vec4 outer;   // its last stop
};

#ifdef BARE_VERTEX
void main() {
    const vec2 c[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(0, 1), vec2(1, 0), vec2(1, 1));
    gl_Position = vec4(c[gl_VertexIndex] * 2.0 - 1.0, 0.0, 1.0);
}
#else
layout(set = 2, binding = 0) uniform sampler2D scene;   // bottom_up, k times the target
layout(location = 0) out vec4 o_color;

void main() {
    vec2 size = misc.zw, p = gl_FragCoord.xy;
    vec2 d = (p - size * 0.5) / (size * 0.5);
    float t = clamp(length(d) / sqrt(2.0), 0.0, 1.0);
    // Skia dithers gradients: a 4 x 4 ordered pattern (rows counted from the top), then truncates to 8 bits
    ivec2 q = ivec2(int(p.x), int(size.y - p.y));
    float dither = float(((q.y & 1) << 3) | ((q.x & 1) << 2) | (((q.y >> 1) & 1) << 1) | ((q.x >> 1) & 1)) / 16.0 + 1.0 / 32.0;
    vec3 bg = floor(mix(inner.rgb, outer.rgb, t) * 255.0 + dither) / 255.0;
    int k = int(misc.y + 0.5);
    ivec2 base = ivec2(floor(p)) * k;
    vec4 c = vec4(0.0);
    for (int y = 0; y < k; y++)
        for (int x = 0; x < k; x++) c += texelFetch(scene, base + ivec2(x, y), 0);
    c /= float(k * k);
    c = floor(c * 255.0 + 0.5) / 255.0;   // the resolved 8-bit canvas
    float op = misc.x;
    o_color = vec4(c.rgb * op + bg * (1.0 - c.a * op), 1.0);
}
#endif
