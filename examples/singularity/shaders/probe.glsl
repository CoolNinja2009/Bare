#version 450
// TEMPORARY (pixel comparison): the bits of one channel of a buffer, as bytes.
layout(location = 0) out vec4 oColor;
layout(set = 2, binding = 0) uniform sampler2D uScene;
layout(set = 2, binding = 1) uniform sampler2D uDir;
layout(set = 2, binding = 2) uniform sampler2D uBloom;
layout(set = 3, binding = 0) uniform Params { vec2 uRes; float uMode; float pad; };
void main() {
  ivec2 p = ivec2(gl_FragCoord.x, uRes.y - gl_FragCoord.y);
  int m = int(uMode);
  float v = 0.0;
  if (m < 4) { if (all(lessThan(p, textureSize(uScene, 0)))) v = texelFetch(uScene, p, 0)[m]; }
  else if (m < 7) { if (all(lessThan(p, textureSize(uDir, 0)))) v = texelFetch(uDir, p, 0)[m - 4]; }
  else if (m >= 10) { if (all(lessThan(p, textureSize(uDir, 0)))) v = texelFetch(uDir, p, 0)[m - 10]; }
  else { if (all(lessThan(p, textureSize(uBloom, 0)))) v = texelFetch(uBloom, p, 0)[m - 7]; }
  uint u = floatBitsToUint(v);
  uint k = (m < 4 || m >= 7 && m < 10) ? packHalf2x16(vec2(v, 0.0)) : m < 7 ? (u & 16777215u) : (u >> 24);
  oColor = vec4(vec3(uvec3(k & 255u, (k >> 8) & 255u, (k >> 16) & 255u)) / 255.0, 1.0);
}
