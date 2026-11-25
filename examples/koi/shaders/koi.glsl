#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// koi.glsl: koi body strips and fin quads (a program for bare_draw). Positions
// are in pond units: x 0..aspect, y 0..1 from the bottom.
layout(set = BARE_PARAMS_SET, binding = 0) uniform Params {
  float uAspect, uShadow; vec2 uShadowOff;
  float uShadowK, uTime, uCaus, uAmb;
  vec3 uSun; float uFog;
  vec3 uDeep; float pad_;
};

#ifdef BARE_VERTEX
layout(location = 0) in vec4 aPosUv;   // position, uv
layout(location = 1) in vec4 aInfo;    // seed, variety, depth, part (0 body, 1 tail, 2 fin)
layout(location = 0) out vec2 vUv;
layout(location = 1) out vec2 vP;
layout(location = 2) flat out vec4 vInfo;
void main() {
  vec2 p = aPosUv.xy + uShadowOff * uShadow * (1.25 - aInfo.z);
  vUv = aPosUv.zw; vInfo = aInfo; vP = p;
  gl_Position = vec4(p.x / uAspect * 2. - 1., p.y * 2. - 1., 0., 1.);
}
#else
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec2 vP;
layout(location = 2) flat in vec4 vInfo;
layout(location = 0) out vec4 o;
const vec3 W = vec3(.96, .94, .90), R = vec3(.86, .19, .06), K = vec3(.045, .045, .055), G = vec3(1., .71, .24);
const vec3 PL = vec3(.88, .90, .93), B = vec3(.40, .50, .60), C = vec3(.56, .38, .20), O = vec3(.98, .46, .14);
vec3 finTint(int k) {
  if (k == 2) return mix(K, W, .45);
  if (k == 3) return G * 1.05;
  if (k == 4) return PL;
  if (k == 5) return mix(O, W, .3);
  if (k == 6) return C * 1.15;
  return W;
}
vec3 skin(float u, float v, float sd, int k, out float metal) {
  metal = 0.;
  vec2 q = vec2(u * 3.2, v * .8) + sd * vec2(31.7, 17.3);
  float n1 = fbm(q), n2 = fbm(q * 1.8 + 9.1);
  float side = smoothstep(.55, .98, abs(v));
  vec3 c;
  if (k <= 1) {   // kohaku, sanke
    float m = smoothstep(.47, .53, n1 - .12 * side + .06);
    if (fract(sd * 7.3) < .18) m = 1. - smoothstep(.7, .95, length(vec2((u - .11) / .075, v / .55)));   // tancho
    c = mix(W, R, m);
    if (k == 1) c = mix(c, K, smoothstep(.64, .68, n2) * (1. - side * .5));
  } else if (k == 2) {   // showa
    float rr = smoothstep(.5, .55, n1), ww = smoothstep(.56, .6, n2);
    c = mix(mix(K, R, rr), W, ww * (1. - rr * .6));
  } else if (k == 3) { c = G * (.9 + .2 * n1); metal = 1.; }
  else if (k == 4) { c = PL * (.95 + .08 * n1); metal = 1.; }
  else if (k == 5) { c = mix(B, O, side * .9); }
  else { c = C * (.9 + .2 * n1); metal = .35; }
  return c;
}
void main() {
  float u = vUv.x, v = vUv.y;
  int part = int(vInfo.w + .5), vr = int(vInfo.y + .5);
  float sd = vInfo.x, depth = vInfo.z;
  float a;
  vec3 col;
  if (part == 0) {
    a = clamp((1. - abs(v)) / max(fwidth(v), 1e-4) * .8, 0., 1.);
    if (uShadow > .5) { o = vec4(0, 0, 0, a * uShadowK * (1. - v * v)); return; }
    float metal;
    col = skin(u, v, sd, vr, metal);
    if (u > .17) {   // scales
      float row = floor(u * 36.);
      vec2 sq = vec2(fract(u * 36.), fract(v * 3.2 + .5 * mod(row, 2.))) - .5;
      float e = smoothstep(.3, .5, length(sq * vec2(1., 1.3)));
      col *= 1. - e * (vr == 5 ? .35 : .08) * smoothstep(.17, .25, u);
    }
    float sh = sqrt(max(0., 1. - v * v));
    col *= .48 + .6 * sh;
    col += vec3(pow(sh, 16.) * (.07 + .22 * metal));
    col += metal * .22 * pow(sh, 3.) * (.5 + .5 * sin(u * 14. - uTime * 1.7 + sd * 20.)) * col;
    float ey = length(vec2((u - .075) / .02, (abs(v) - .6) / .15));
    col = mix(col, vec3(.03), 1. - smoothstep(.8, 1., ey));
    col *= mix(.8, 1., smoothstep(0., .08, u));
  } else {
    float w, inside, rays;
    if (part == 1) {   // tail: widening, forked
      w = mix(.22, 1., smoothstep(0., .8, u));
      float tip = 1. - .42 * (1. - abs(v)) + .04 * sin(v * 9. + uTime * 3.);
      inside = smoothstep(0., .1, w - abs(v)) * smoothstep(0., .06, tip - u);
      rays = .5 + .5 * sin(atan(v * w, u + .05) * 26.);
    } else {           // pectoral fin: a rounded paddle
      w = .25 + .75 * smoothstep(0., .45, u);
      float r = length(vec2((u - .45) / .55, v / w));
      inside = 1. - smoothstep(.85, 1., r);
      rays = .5 + .5 * sin(atan(v, u + .1) * 18.);
    }
    if (uShadow > .5) { o = vec4(0, 0, 0, inside * uShadowK * .45); return; }
    col = finTint(vr) * (.78 + .3 * rays);
    a = inside * (.5 + .2 * rays) * (1. - .35 * u);
  }
  col *= uAmb * mix(vec3(1), uSun, .3);
  col += col * uSun * caustic(vP * 6., uTime) * uCaus * (1. - depth) * .9;
  col = mix(col, uDeep * uAmb, clamp((1. - exp(-depth * uFog)) * .85, 0., .95));
  o = vec4(col, a);
}
#endif
