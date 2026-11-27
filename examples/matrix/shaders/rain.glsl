#version 450
// rain.glsl: canvas 2D's fillRect and fillText for the rain, one instance each:
// a rectangle of solid colour, or a glyph from the atlas (coverage as alpha).
layout(set = BARE_PARAMS_SET, binding = 0) uniform Params { vec4 res; };   // canvas width, height, atlas width, height

#ifdef BARE_VERTEX
layout(location = 0) in vec4 i_rect;    // per instance: x, y, width, height (pixels, y down)
layout(location = 1) in vec4 i_uv;      // atlas pixels x, y (x < 0: a plain rectangle)
layout(location = 2) in vec4 i_color;   // straight RGBA
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out float v_glyph;

void main() {
    const vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(0, 1), vec2(1, 0), vec2(1, 1));
    vec2 c = corners[gl_VertexIndex];
    vec2 p = i_rect.xy + c * i_rect.zw;
    v_uv = (i_uv.xy + c * i_rect.zw) / res.zw;
    v_color = i_color;
    v_glyph = i_uv.x < 0.0 ? 0.0 : 1.0;
    gl_Position = vec4(p.x / res.x * 2.0 - 1.0, 1.0 - p.y / res.y * 2.0, 0.0, 1.0);
}
#else
layout(set = 2, binding = 0) uniform sampler2D atlas;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in float v_glyph;
layout(location = 0) out vec4 o_color;

void main() {
    float a = v_glyph > 0.5 ? texture(atlas, v_uv).r : 1.0;
    o_color = vec4(v_color.rgb, v_color.a * a);
}
#endif
