#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
// Multiplies a texture by a value (the solver's warm start for pressure).
layout(set = 2, binding = 0) uniform sampler2D u_source;
layout(set = 3, binding = 0) uniform Params { float value; };

void main() { o_color = value * texture(u_source, v_uv); }
