#version 450
// dots.glsl: soft round dots, one instance each. No vertex data: each dot's
// quad (two triangles) is built from gl_VertexIndex.
layout(set = BARE_PARAMS_SET, binding = 0) uniform Params { vec4 res; };   // width, height, -, -

#ifdef BARE_VERTEX
layout(location = 0) in vec4 i_dot;     // per instance: x, y, radius (pixels), -
layout(location = 1) in vec4 i_color;   // per instance: straight RGBA
layout(location = 0) out vec2 v_local;
layout(location = 1) out vec4 v_color;

void main() {
    const vec2 corners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(-1, 1), vec2(-1, 1), vec2(1, -1), vec2(1, 1));
    v_local = corners[gl_VertexIndex];
    v_color = i_color;
    vec2 p = i_dot.xy + v_local * i_dot.z;
    gl_Position = vec4(p.x / res.x * 2.0 - 1.0, 1.0 - p.y / res.y * 2.0, 0.0, 1.0);
}
#else
layout(location = 0) in vec2 v_local;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main() {
    float d = length(v_local);
    o_color = vec4(v_color.rgb, v_color.a * smoothstep(1.0, 0.6, d));
}
#endif
