#version 450
// line.glsl: the page's LineSegments with its TentacleMaterial (tentacle-vert,
// tentacle-frag): one instance a segment, drawn as WebGL drew it, one pixel
// wide and aliased (linewidth is ignored there): a parallelogram one pixel
// tall across an x-major segment (wide across a y-major one), so each column
// (row) lights the pixel whose centre is nearest the line. Ends sit between
// their last two physics steps; centerDist is the raw position's length.
layout(set = BARE_PARAMS_SET, binding = 0) uniform Params {
    mat4 projection;
    mat4 model_view;
    vec4 diffuse;    // rgb, opacity
    vec4 misc;       // area, stepProgress, additive (1: premultiplied for BARE_BLEND_ADD)
    vec4 size;       // target w, h
};

#ifdef BARE_VERTEX
layout(location = 0) in vec4 i_a;    // position
layout(location = 1) in vec4 i_ap;   // positionPrev
layout(location = 2) in vec4 i_b;
layout(location = 3) in vec4 i_bp;
layout(location = 0) out float centerDist;

void main() {
    vec4 ca = projection * (model_view * vec4(mix(i_ap.xyz, i_a.xyz, misc.y), 1.0));
    vec4 cb = projection * (model_view * vec4(mix(i_bp.xyz, i_b.xyz, misc.y), 1.0));
    if (ca.w <= 0.0 || cb.w <= 0.0) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); centerDist = 1.0; return; }
    vec2 sa = ca.xy / ca.w * 0.5 * size.xy, sb = cb.xy / cb.w * 0.5 * size.xy;   // pixels from the centre
    vec2 d = sb - sa;
    // corners: a-, b-, a+, a+, b-, b+
    const int ends[6] = int[](0, 1, 0, 0, 1, 1);
    const float sides[6] = float[](-1.0, -1.0, 1.0, 1.0, -1.0, 1.0);
    int k = gl_VertexIndex;
    bool xmajor = abs(d.x) >= abs(d.y);
    vec2 off = (xmajor ? vec2(0.0, 0.5) : vec2(0.5, 0.0)) * sides[k] * 2.0 / size.xy;   // half a pixel, in NDC
    vec4 c = ends[k] == 0 ? ca : cb;
    centerDist = length(ends[k] == 0 ? i_a.xyz : i_b.xyz);
    gl_Position = vec4(c.xy + off * c.w, (c.z + c.w) * 0.5, c.w);
}
#else
layout(location = 0) in float centerDist;
layout(location = 0) out vec4 o_color;

void main() {
  float area = misc.x, opacity = diffuse.a;
  float illumination = area * 2.0 / (centerDist * centerDist);
  vec3 c = clamp(mix(vec3(1.0), diffuse.rgb, clamp(illumination, 0.0, 1.25)), 0.0, 1.0);
  float a = clamp(opacity * illumination * illumination, 0.0, opacity);
  o_color = misc.z > 0.5 ? vec4(c * a, a) : vec4(c, a);
}
#endif
