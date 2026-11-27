#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
// Seeds the feedback history with the scene. The scene canvas runs top-down and
// this target bottom-up (WebGL's), so the scene is sampled with v flipped, as
// the page's UNPACK_FLIP_Y upload does.
layout(set = 2, binding = 0) uniform sampler2D uSrc;
void main() { o = vec4(texture(uSrc, vec2(v_uv.x, 1.0 - v_uv.y)).rgb, 1.0); }
