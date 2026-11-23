#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
// Semi-Lagrangian advection: each pixel fetches what flowed into it, then fades.
layout(set = 2, binding = 0) uniform sampler2D u_velocity;
layout(set = 2, binding = 1) uniform sampler2D u_source;
layout(set = 3, binding = 0) uniform Params { vec2 texel; float dt; float dissipation; };

void main() {
    vec2 coord = v_uv - dt * texture(u_velocity, v_uv).xy * texel;
    o_color = texture(u_source, coord) / (1.0 + dissipation * dt);
}
