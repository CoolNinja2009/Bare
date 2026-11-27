#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
// The page's WebGL canvas, scaled by CSS to the window: a bilinear stretch of
// the bottom-up output into the top-down window canvas the HUD is drawn on.
layout(set = 2, binding = 0) uniform sampler2D uSrc;
void main() { o = vec4(texture(uSrc, vec2(v_uv.x, 1.0 - v_uv.y)).rgb, 1.0); }
