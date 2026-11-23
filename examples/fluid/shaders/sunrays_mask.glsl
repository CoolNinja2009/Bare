#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
// Dark areas let light through; dye blocks it.
layout(set = 2, binding = 0) uniform sampler2D u_source;

void main() {
    vec4 c = texture(u_source, v_uv);
    float br = max(c.r, max(c.g, c.b));
    c.a = 1.0 - min(max(br * 20.0, 0.0), 0.8);
    o_color = c;
}
