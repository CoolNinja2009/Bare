#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
// Keeps what is brighter than the threshold, with a soft knee.
layout(set = 2, binding = 0) uniform sampler2D u_source;
layout(set = 3, binding = 0) uniform Params { vec4 curve; };   // knee curve in xyz, threshold in w

void main() {
    vec3 c = texture(u_source, v_uv).rgb;
    float br = max(c.r, max(c.g, c.b));
    float rq = clamp(br - curve.x, 0.0, curve.y);
    rq = curve.z * rq * rq;
    c *= max(rq, br - curve.w) / max(br, 0.0001);
    o_color = vec4(c, 0.0);
}
