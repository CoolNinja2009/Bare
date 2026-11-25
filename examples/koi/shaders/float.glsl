#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// float.glsl: what floats on the water, one instance each, riding the waves:
// lily pads, lotus, petals, leaves, food, fireflies (a program for bare_draw).
layout(set = BARE_PARAMS_SET, binding = 0) uniform Params {
  float uAspect, uShadow, uNK, uShadowK;
  vec2 uShadowOff, uSimTexel;
  vec3 uSun; float uAmb;
  vec3 uL; float pad_;
};

#ifdef BARE_VERTEX
layout(location = 0) in vec4 aA;   // x, y, rotation, half size
layout(location = 1) in vec4 aB;   // type, seed, alpha, extra
layout(set = 0, binding = 0) uniform sampler2D uSim;   // .inputs[0]
layout(location = 0) out vec2 vQ;
layout(location = 1) out vec2 vTilt;
layout(location = 2) flat out vec4 vB;
const vec2 CORNER[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
void main() {
  vec2 c = CORNER[gl_VertexIndex];
  vec2 uv = vec2(aA.x / uAspect, aA.y);
  float l = texture(uSim, uv - vec2(uSimTexel.x, 0.)).r, r = texture(uSim, uv + vec2(uSimTexel.x, 0.)).r;
  float d = texture(uSim, uv - vec2(0., uSimTexel.y)).r, u = texture(uSim, uv + vec2(0., uSimTexel.y)).r;
  vec2 g = vec2(r - l, u - d) * uNK;
  float cr = cos(aA.z), sr = sin(aA.z);
  vec2 p = aA.xy + mat2(cr, sr, -sr, cr) * c * aA.w - g * .004 + uShadowOff * uShadow;
  vQ = c; vB = aB; vTilt = g;
  gl_Position = vec4(p.x / uAspect * 2. - 1., p.y * 2. - 1., 0., 1.);
}
#else
layout(location = 0) in vec2 vQ;
layout(location = 1) in vec2 vTilt;
layout(location = 2) flat in vec4 vB;
layout(location = 0) out vec4 o;
float aa(float d) { return clamp(d / max(fwidth(d), 1e-4) + .5, 0., 1.); }
void main() {
  int type = int(vB.x + .5);
  float sd = vB.y, ex = vB.w;
  vec2 q = vQ;
  float r = length(q), ang = atan(q.y, q.x);
  vec3 col = vec3(0);
  float a = 0.;
  if (type == 0) {   // lily pad
    float rr = .97 - .03 * noise(vec2(ang * 3., sd * 10.));
    float notch = 1. - aa(.16 - abs(ang)) * step(.03, r);
    a = aa(rr - r) * notch;
    col = mix(vec3(.15, .38, .12), vec3(.30, .50, .15), ex);
    col = mix(col, vec3(.55, .48, .16), smoothstep(.85, 1., ex) * smoothstep(.5, 1., r + .3 * noise(q * 4. + sd)));
    col *= 1. - .16 * pow(abs(cos(ang * 11.)), 30.) * smoothstep(.1, .9, r);
    col *= .9 + .2 * noise(q * 6. + sd * 7.);
    col = mix(col, col * 1.35 + vec3(.04, .03, 0), smoothstep(.84, .95, r / rr));
    col *= 1. - .4 * smoothstep(.94, 1., r / rr);
  } else if (type == 1) {   // lotus seen from above: two rings of pointed petals
    float f1 = abs(cos(ang * 4.)), f2 = abs(cos(ang * 4. + 1.5708));
    float outer = aa(mix(.42, 1., pow(f1, 2.5)) - r);
    float inner = aa(mix(.3, .68, pow(f2, 2.)) - r);
    vec3 pc = ex < .5 ? vec3(1., .62, .78) : ex < 1.5 ? vec3(.98, .96, .92) : ex < 2.5 ? vec3(.92, .36, .62) : vec3(1., .92, .6);
    col = pc * (.7 + .3 * f1) * (.75 + .25 * r);
    col = mix(col, mix(pc, vec3(1), .5) * (.8 + .2 * f2), inner);
    float core = aa(.2 - r);
    col = mix(col, mix(vec3(1., .85, .25), vec3(.85, .55, .1), step(.6, hash12(floor(q * 18.)))), core);
    a = max(outer, inner);
  } else if (type == 2) {   // cherry petal
    float e = length(vec2(q.x / .62, q.y));
    a = aa(1. - e) * aa(length(q - vec2(0, 1.15)) - .36);
    col = mix(vec3(.95, .55, .66), vec3(1., .84, .88), smoothstep(-1., .4, q.y));
  } else if (type == 3) {   // maple leaf
    float lim = (.42 + .58 * pow(.5 + .5 * cos(5. * ang - 7.854), 1.4)) * (1. - .08 * abs(sin(ang * 25.)));
    float stem = aa(.06 - abs(q.x)) * step(q.y, -.2) * step(-1., q.y);
    a = max(aa(lim - r), stem);
    vec3 lc = ex < .5 ? vec3(.85, .18, .06) : ex < 1.5 ? vec3(.95, .45, .08) : ex < 2.5 ? vec3(.95, .72, .15) : vec3(.55, .12, .05);
    col = lc * (.85 + .25 * noise(q * 5. + sd * 9.));
    col *= 1. - .3 * pow(abs(cos(ang * 2.5)), 60.) * step(.1, r);
  } else if (type == 4) {   // food pellet
    a = aa(1. - r);
    col = vec3(.45, .28, .12) * (.8 + .4 * smoothstep(.3, -.6, q.x + q.y));
  } else {   // firefly (drawn additively)
    float g = exp(-r * r * 90.) * 1.6 + exp(-r * r * 8.) * .35;
    o = vec4(vec3(.8, 1., .4) * g * vB.z, 1.);
    return;
  }
  a *= vB.z;
  if (uShadow > .5) { o = vec4(0, 0, 0, a * uShadowK); return; }
  vec3 n = normalize(vec3(-vTilt, 1.));
  float lit = .55 + .55 * max(dot(n, uL), 0.);
  col *= uAmb * mix(vec3(1), uSun, .4) * lit;
  col += uSun * uAmb * pow(max(dot(n, normalize(uL + vec3(0, 0, 1))), 0.), 160.) * .35 * float(type == 0);
  o = vec4(col, a);
}
#endif
