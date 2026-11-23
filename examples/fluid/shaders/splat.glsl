#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
// Adds a Gaussian blob of colour (dye) or force (velocity) at a point.
layout(set = 2, binding = 0) uniform sampler2D u_target;
layout(set = 3, binding = 0) uniform Params { vec2 point; float aspect; float radius; vec4 color; };

void main() {
    vec2 p = v_uv - point;
    p.x *= aspect;
    vec3 splat = exp(-dot(p, p) / radius) * color.rgb;
    o_color = vec4(texture(u_target, v_uv).xyz + splat, 1.0);
}
