#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
// Subtracts the pressure gradient, leaving a divergence-free velocity field.
layout(set = 2, binding = 0) uniform sampler2D u_pressure;
layout(set = 2, binding = 1) uniform sampler2D u_velocity;
layout(set = 3, binding = 0) uniform Params { vec2 texel; };

void main() {
    float L = texture(u_pressure, v_uv - vec2(texel.x, 0.0)).x;
    float R = texture(u_pressure, v_uv + vec2(texel.x, 0.0)).x;
    float T = texture(u_pressure, v_uv + vec2(0.0, texel.y)).x;
    float B = texture(u_pressure, v_uv - vec2(0.0, texel.y)).x;
    vec2 vel = texture(u_velocity, v_uv).xy - vec2(R - L, T - B);
    o_color = vec4(vel, 0.0, 1.0);
}
