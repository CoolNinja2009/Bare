#version 450
// ribbon.glsl: a triangle strip with a colour per vertex. Positions are in
// pixels, (0, 0) at the canvas's top-left.
layout(set = BARE_PARAMS_SET, binding = 0) uniform Params { vec4 res; };   // width, height, -, -

#ifdef BARE_VERTEX
layout(location = 0) in vec2 a_pos;     // per vertex: slot 0
layout(location = 1) in vec4 a_color;   // per vertex: slot 1
layout(location = 0) out vec4 v_color;

void main() {
    v_color = a_color;
    gl_Position = vec4(a_pos.x / res.x * 2.0 - 1.0, 1.0 - a_pos.y / res.y * 2.0, 0.0, 1.0);
}
#else
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main() { o_color = v_color; }
#endif
