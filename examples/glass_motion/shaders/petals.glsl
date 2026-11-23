#version 450
// Big soft petals of colour over deep green leaves, swaying.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 3, binding = 0) uniform Params { float time; float aspect; };

float blob(vec2 p, vec2 c, vec2 r, float a) {
    vec2 d = p - c;
    float ca = cos(a), sa = sin(a);
    d = vec2(ca * d.x + sa * d.y, -sa * d.x + ca * d.y) / r;
    return exp(-dot(d, d) * 2.0);
}

void main() {
    vec2 p = vec2(v_uv.x * aspect, v_uv.y);
    float s = sin(time * 0.6), c = cos(time * 0.45);
    vec3 col = mix(vec3(0.05, 0.16, 0.09), vec3(0.16, 0.30, 0.15), v_uv.y);
    col = mix(col, vec3(0.30, 0.45, 0.22), blob(p, vec2(0.15 + 0.02 * s, 0.75), vec2(0.20, 0.55), 0.5));   // leaves
    col = mix(col, vec3(0.02, 0.09, 0.05), blob(p, vec2(0.40, 0.95), vec2(0.35, 0.18), -0.3));
    col = mix(col, vec3(1.00, 0.55, 0.05), blob(p, vec2(0.36 + 0.02 * c, 0.42), vec2(0.08, 0.20), 0.2));   // orange
    col = mix(col, vec3(1.00, 0.80, 0.20), blob(p, vec2(0.40 + 0.02 * c, 0.30), vec2(0.05, 0.10), 0.1));
    col = mix(col, vec3(0.35, 0.25, 0.95), blob(p, vec2(0.18 + 0.03 * s, 0.50), vec2(0.28, 0.04), -0.35)); // blue blade
    col = mix(col, vec3(0.80, 0.20, 0.55), blob(p, vec2(0.28, 0.16 + 0.02 * s), vec2(0.10, 0.05), 0.6));
    col = mix(col, vec3(0.90, 0.92, 0.95), 0.6 * blob(p, vec2(0.0, 0.0), vec2(0.6, 0.12), 0.0));     // bright sky at the top
    o_color = vec4(col, 1.0);
}
