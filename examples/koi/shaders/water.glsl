#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// water.glsl: the surface: refraction, sky and tree reflection, sun or moon, glints, night glow.
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 o;
layout(set = 2, binding = 0) uniform sampler2D uScene;
layout(set = 2, binding = 1) uniform sampler2D uSim;
layout(set = 3, binding = 0) uniform Params {
  vec2 uSimTexel, uSunPos;
  float uAspect, uTime, uNK, uRefr;
  float uRefl, uSunI, uMoon, uBio;
  float uNight, uFlash, uExposure, uTrees;
  float uAutumn, uOvercast, uAmb, pad0_;
  vec3 uTop; float pad1_;
  vec3 uHor; float pad2_;
  vec3 uSun; float pad3_;
  vec3 uL; float pad4_;
};
void main() {
  vec2 uv = vUv;
  vec4 s = texture(uSim, uv);
  float l = texture(uSim, uv - vec2(uSimTexel.x, 0.)).r, r = texture(uSim, uv + vec2(uSimTexel.x, 0.)).r;
  float d = texture(uSim, uv - vec2(0., uSimTexel.y)).r, u = texture(uSim, uv + vec2(0., uSimTexel.y)).r;
  vec2 g = vec2(r - l, u - d) * uNK;
  vec3 n = normalize(vec3(-g, 1.));
  vec2 ia = vec2(1. / uAspect, 1.);
  vec3 col = texture(uScene, uv + n.xy * uRefr * ia).rgb;

  // what the surface reflects: the top of the screen sees towards the horizon
  vec2 rp = uv + n.xy * .3 * ia;
  vec2 wp = vec2(rp.x * uAspect, rp.y);
  vec3 sky = mix(uTop, uHor, smoothstep(.05, 1.05, rp.y));
  float cl = fbm(wp * 1.8 + vec2(uTime * .008, uTime * .002));
  sky = mix(sky, mix(uHor, vec3(1), .45) * (.35 + .65 * max(uAmb, .15)), smoothstep(.5, .8, cl) * (.45 + .4 * uOvercast) * (1. - uNight * .7));
  sky += vec3(step(.9975, hash12(floor(wp * 260.)))) * uNight * (1. - uOvercast) * .8;
  float sd = length((rp - uSunPos) * vec2(uAspect, 1.));
  sky += uSun * uSunI * (1. - uOvercast * .85) * (exp(-sd * sd / .003) * .9 * (1. - uMoon) + exp(-sd * sd / .05) * .22 + uMoon * smoothstep(.034, .03, sd) * .9);
  // overhanging branches in two corners, leaves with sky between them
  float b1 = 1. - length((wp - vec2(-.05, 1.08)) * vec2(.85, 1.3));
  float b2 = 1. - length((wp - vec2(uAspect + .08, .92)) * vec2(1.25, .9));
  float leaf = fbm(wp * 7.) * .6 + fbm(wp * 23. + 5.) * .4;
  float tree = smoothstep(.52, .56, max(b1, b2) * .9 + leaf * .55 - .15) * uTrees;
  vec3 tc = mix(vec3(.10, .18, .07), vec3(.55, .17, .05), uAutumn) * (.25 + .75 * uAmb) * (.7 + .5 * leaf);
  sky = mix(sky, tc, tree);
  float slope = length(g);
  float fres = clamp(uRefl * (.7 + 3. * slope), 0., .9);
  col = mix(col, sky, fres);
  // ripples catch the light on the side facing it, shade on the other
  col *= 1. + clamp(dot(g, -uL.xy), -.5, .5) * .9 * (.4 + .6 * uSunI) * (1. - uOvercast * .5);
  col += mix(uHor, vec3(1), .5) * smoothstep(.05, .5, slope) * .06 * (.3 + uAmb);

  // glints where ripples face the light
  float spec = pow(max(dot(n, normalize(uL + vec3(0, 0, 1))), 0.), 700.);
  col += uSun * spec * 3.5 * uSunI * (1. - uOvercast * .8) * (1. - tree);
  // wakes glow at night
  col += vec3(.12, .75, .85) * (smoothstep(.02, .3, abs(s.g)) * .35 + smoothstep(.1, .8, length(g)) * .45) * uBio * .5;
  col += uFlash * (.25 + fres) * vec3(.85, .9, 1.);
  vec2 vq = uv - .5;
  col *= 1. - .3 * pow(length(vq * vec2(1., .9)) * 1.25, 2.6);
  col *= uExposure;
  col += (hash12(gl_FragCoord.xy) - .5) / 255.;
  o = vec4(col, 1.);
}
