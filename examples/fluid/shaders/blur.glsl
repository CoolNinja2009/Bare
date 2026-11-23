#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
// One direction of a 5-tap Gaussian blur done with 3 bilinear taps.
layout(set = 2, binding = 0) uniform sampler2D u_source;
layout(set = 3, binding = 0) uniform Params { vec2 texel; };   // (1/w, 0) or (0, 1/h)

void main() {
    vec2 off = texel * 1.33333333;
    vec4 sum = texture(u_source, v_uv) * 0.29411764;
    sum += texture(u_source, v_uv - off) * 0.35294117;
    sum += texture(u_source, v_uv + off) * 0.35294117;
    o_color = sum;
}
