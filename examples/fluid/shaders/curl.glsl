#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
// Vorticity (curl) of the velocity field.
layout(set = 2, binding = 0) uniform sampler2D u_velocity;
layout(set = 3, binding = 0) uniform Params { vec2 texel; };

void main() {
    float L = texture(u_velocity, v_uv - vec2(texel.x, 0.0)).y;
    float R = texture(u_velocity, v_uv + vec2(texel.x, 0.0)).y;
    float T = texture(u_velocity, v_uv + vec2(0.0, texel.y)).x;
    float B = texture(u_velocity, v_uv - vec2(0.0, texel.y)).x;
    o_color = vec4(0.5 * (R - L - T + B), 0.0, 0.0, 1.0);
}
