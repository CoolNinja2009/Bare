#version 450
// blur.glsl: one pass of a Gaussian blur (run it across, then down), over the
// whole target, reading an input of the same size; outside it is transparent.
layout(set = BARE_PARAMS_SET, binding = 0) uniform Params {
    vec4 dir;      // x, y step, radius in texels
    vec4 w[16];    // weights for offsets 0..63, normalised
};

#ifdef BARE_VERTEX
void main() {
    const vec2 c[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(0, 1), vec2(1, 0), vec2(1, 1));
    gl_Position = vec4(c[gl_VertexIndex] * 2.0 - 1.0, 0.0, 1.0);
}
#else
layout(set = 2, binding = 0) uniform sampler2D src;
layout(location = 0) out vec4 o_color;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy), size = textureSize(src, 0), d = ivec2(dir.xy);
    int r = int(dir.z);
    vec4 s = vec4(0.0);
    for (int i = -r; i <= r; i++) {
        ivec2 q = p + d * i;
        if (all(greaterThanEqual(q, ivec2(0))) && all(lessThan(q, size))) {
            int k = abs(i);
            s += texelFetch(src, q, 0) * w[k >> 2][k & 3];
        }
    }
    o_color = s;
}
#endif
