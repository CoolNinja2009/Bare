#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
// Four-tap box filter, used going down and coming back up the bloom chain.
layout(set = 2, binding = 0) uniform sampler2D u_source;
layout(set = 3, binding = 0) uniform Params { vec2 texel; float intensity; };

void main() {
    vec4 sum = texture(u_source, v_uv - vec2(texel.x, 0.0));
    sum += texture(u_source, v_uv + vec2(texel.x, 0.0));
    sum += texture(u_source, v_uv + vec2(0.0, texel.y));
    sum += texture(u_source, v_uv - vec2(0.0, texel.y));
    o_color = sum * 0.25 * intensity;
}
