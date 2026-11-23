#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
// Vorticity confinement: pushes velocity along the curl gradient to keep swirls alive.
layout(set = 2, binding = 0) uniform sampler2D u_velocity;
layout(set = 2, binding = 1) uniform sampler2D u_curl;
layout(set = 3, binding = 0) uniform Params { vec2 texel; float curl; float dt; };

void main() {
    float L = texture(u_curl, v_uv - vec2(texel.x, 0.0)).x;
    float R = texture(u_curl, v_uv + vec2(texel.x, 0.0)).x;
    float T = texture(u_curl, v_uv + vec2(0.0, texel.y)).x;
    float B = texture(u_curl, v_uv - vec2(0.0, texel.y)).x;
    float C = texture(u_curl, v_uv).x;
    vec2 force = 0.5 * vec2(abs(T) - abs(B), abs(R) - abs(L));
    force /= length(force) + 0.0001;
    force *= curl * C;
    force.y *= -1.0;
    vec2 vel = texture(u_velocity, v_uv).xy;
    o_color = vec4(vel + force * dt, 0.0, 1.0);
}
