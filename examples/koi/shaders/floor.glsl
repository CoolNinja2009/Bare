#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// floor.glsl: the pond floor each frame: light, caustics the ripples focus, depth haze.
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 o;
layout(set = 2, binding = 0) uniform sampler2D uBase;
layout(set = 2, binding = 1) uniform sampler2D uSim;
layout(set = 3, binding = 0) uniform Params {
  vec2 uSimTexel; float uAspect, uTime;
  float uCaus, uAmb, uFog, uNK;
  vec3 uSun; float pad0_;
  vec3 uDeep; float pad1_;
  vec3 uShallow; float pad2_;
};
void main() {
  vec4 b = texture(uBase, vUv);
  vec2 p = vec2(vUv.x * uAspect, vUv.y);
  float h = texture(uSim, vUv).r;
  float l = texture(uSim, vUv - vec2(uSimTexel.x, 0.)).r, r = texture(uSim, vUv + vec2(uSimTexel.x, 0.)).r;
  float d = texture(uSim, vUv - vec2(0., uSimTexel.y)).r, u = texture(uSim, vUv + vec2(0., uSimTexel.y)).r;
  vec2 g = vec2(r - l, u - d) * uNK;
  float lap = (l + r + u + d - 4. * h) * uNK * .5;
  float depth = b.a;
  float c = caustic(p * 6. + g * (.4 + depth), uTime) * (1. + clamp(-lap * 5., -.6, 3.));
  vec3 light = uAmb * mix(vec3(1), uSun, .35);
  vec3 col = b.rgb * mix(vec3(1), uShallow * 2.2, .55) * light;
  col += b.rgb * uSun * c * uCaus * (1. - depth * .75) * 1.1;
  col = mix(col, uDeep * uAmb, 1. - exp(-depth * uFog));
  o = vec4(col, 1.);
}
