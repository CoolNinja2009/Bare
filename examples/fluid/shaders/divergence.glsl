#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
// Divergence of the velocity field, with solid walls at the edges.
layout(set = 2, binding = 0) uniform sampler2D u_velocity;
layout(set = 3, binding = 0) uniform Params { vec2 texel; };

void main() {
    vec2 l = v_uv - vec2(texel.x, 0.0), r = v_uv + vec2(texel.x, 0.0);
    vec2 t = v_uv + vec2(0.0, texel.y), b = v_uv - vec2(0.0, texel.y);
    float L = texture(u_velocity, l).x;
    float R = texture(u_velocity, r).x;
    float T = texture(u_velocity, t).y;
    float B = texture(u_velocity, b).y;
    vec2 C = texture(u_velocity, v_uv).xy;
    if (l.x < 0.0) L = -C.x;
    if (r.x > 1.0) R = -C.x;
    if (t.y > 1.0) T = -C.y;
    if (b.y < 0.0) B = -C.y;
    o_color = vec4(0.5 * (R - L + T - B), 0.0, 0.0, 1.0);
}
