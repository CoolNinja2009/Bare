#version 450
// sim.glsl: the water, a height field stepped by the wave equation (velocity
// form); drops are added to the height. r = height, g = velocity.
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 o;
layout(set = 2, binding = 0) uniform sampler2D uPrev;
layout(set = 3, binding = 0) uniform Params { float uAspect, uDamp; int uDrops; float pad_; vec4 uDrop[32]; };
void main() {
  ivec2 sz = textureSize(uPrev, 0);
  ivec2 c = ivec2(gl_FragCoord.xy);
  vec2 s = texelFetch(uPrev, c, 0).rg;
  float l = texelFetch(uPrev, ivec2(max(c.x - 1, 0), c.y), 0).r;
  float r = texelFetch(uPrev, ivec2(min(c.x + 1, sz.x - 1), c.y), 0).r;
  float d = texelFetch(uPrev, ivec2(c.x, max(c.y - 1, 0)), 0).r;
  float u = texelFetch(uPrev, ivec2(c.x, min(c.y + 1, sz.y - 1)), 0).r;
  float h = s.x, v = s.y;
  v += ((l + r + d + u) * .25 - h) * 2.;
  v *= uDamp;
  h += v;
  h *= .9993;
  vec2 p = vec2(vUv.x * uAspect, vUv.y);
  for (int i = 0; i < 32; i++) {
    if (i >= uDrops) break;
    vec4 dr = uDrop[i];
    float dist = length(p - dr.xy);
    if (dist < dr.z) h += dr.w * (.5 + .5 * cos(3.14159265 * dist / dr.z));
  }
  o = vec4(clamp(h, -6., 6.), clamp(v, -6., 6.), 0., 1.);
}
