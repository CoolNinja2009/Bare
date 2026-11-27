#version 450
// blur.glsl: one direction of CSS filter: blur() (a Gaussian, sigma in pixels).
layout(set = 2, binding = 0) uniform sampler2D u_src;
layout(set = 3, binding = 0) uniform Params { vec4 p; };   // direction x, y (texels), sigma, -
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    float sigma = p.z;
    if (sigma < 0.1) { o_color = texture(u_src, v_uv); return; }
    int radius = int(ceil(sigma * 3.0));
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int i = -radius; i <= radius; i++) {
        float w = exp(-float(i * i) / (2.0 * sigma * sigma));
        sum += texture(u_src, v_uv + p.xy * float(i)) * w;
        wsum += w;
    }
    o_color = sum / wsum;
}
