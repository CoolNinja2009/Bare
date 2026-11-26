#version 450
// cubes.glsl: two spinning cubes that pass through each other, drawn with a
// depth buffer so each face hides what's behind it, intersections included.
// No vertex data: 36 vertices per cube from gl_VertexIndex, one cube per instance.
layout(set = BARE_PARAMS_SET, binding = 0) uniform Params { vec4 res; };   // width, height, time, -

#ifdef BARE_VERTEX
layout(location = 0) out vec3 v_color;

void main() {
    const ivec3 axes[6] = ivec3[](ivec3(0, 1, 2), ivec3(0, 2, 1), ivec3(1, 2, 0), ivec3(0, 1, 2), ivec3(0, 2, 1), ivec3(1, 2, 0));
    const vec2 corners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(-1, 1), vec2(-1, 1), vec2(1, -1), vec2(1, 1));
    int face = gl_VertexIndex / 6;
    vec2 c = corners[gl_VertexIndex % 6];
    vec3 p;
    ivec3 a = axes[face];
    p[a.x] = c.x, p[a.y] = c.y, p[a.z] = face < 3 ? 1.0 : -1.0;
    float t = res.z * (gl_InstanceIndex == 0 ? 0.7 : -0.5) + float(gl_InstanceIndex);
    mat3 ry = mat3(cos(t), 0, -sin(t), 0, 1, 0, sin(t), 0, cos(t));
    mat3 rx = mat3(1, 0, 0, 0, cos(t * 0.6), sin(t * 0.6), 0, -sin(t * 0.6), cos(t * 0.6));
    vec3 n = rx * ry * vec3(a.z == 0 ? 1 : 0, a.z == 1 ? 1 : 0, a.z == 2 ? 1 : 0) * (face < 3 ? 1.0 : -1.0);
    vec3 q = rx * ry * p * 0.55 + vec3(gl_InstanceIndex == 0 ? -0.35 : 0.35, 0.0, 4.0);
    vec3 base = gl_InstanceIndex == 0 ? vec3(0.95, 0.45, 0.3) : vec3(0.3, 0.6, 1.0);
    v_color = base * (0.35 + 0.65 * max(dot(n, normalize(vec3(-0.4, 0.7, -0.6))), 0.0));
    float aspect = res.x / res.y, f = 2.2;
    // depth 0 at the near plane (1 m), towards 1 far away: the default LESS test keeps the nearest
    gl_Position = vec4(q.x * f / aspect, q.y * f, q.z - 1.0, q.z);
}
#else
layout(location = 0) in vec3 v_color;
layout(location = 0) out vec4 o_color;

void main() { o_color = vec4(v_color, 1.0); }
#endif
