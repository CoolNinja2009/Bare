#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// base.glsl: the pond floor, drawn once per size: colour + depth (alpha).
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 o;
layout(set = 3, binding = 0) uniform Params { float uAspect, pad_; vec2 uSeed; };
void main() {
  vec2 p = vec2(vUv.x * uAspect, vUv.y);
  float e = min(min(p.x, uAspect - p.x), min(p.y, 1. - p.y));
  float depth = clamp(.08 + .92 * smoothstep(0., .4, e) + .55 * (fbm(p * 1.4 + uSeed) - .5), 0., 1.);
  vec3 L = normalize(vec3(.4, .5, .8));
  // pebbles
  vec2 q = p * 32. + uSeed;
  vec2 i = floor(q), f = fract(q);
  float d1 = 8., d2 = 8.;
  vec2 id = vec2(0), rc = vec2(0);
  for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
    vec2 g = vec2(x, y);
    vec2 r = g + hash22(i + g) * .8 + .1 - f;
    float d = dot(r, r);
    if (d < d1) { d2 = d1; d1 = d; id = i + g; rc = r; } else if (d < d2) d2 = d;
  }
  float edge = sqrt(d2) - sqrt(d1);
  float h1 = hash12(id), h2 = hash12(id + 7.1), h3 = hash12(id + 3.3);
  vec3 stone = mix(vec3(.40, .38, .34), vec3(.56, .51, .43), h1);
  stone = mix(stone, vec3(.27, .29, .28), step(.72, h2));
  stone = mix(stone, vec3(.63, .50, .34), step(.9, h3));
  stone *= .9 + .2 * noise(q * 3.);
  float lit = .6 + .5 * dot(normalize(vec3(-rc * 2.2, 1.)), L);
  float gap = smoothstep(.06, .2, edge) * smoothstep(.62, .35, sqrt(d1));
  vec3 mud = mix(vec3(.19, .18, .13), vec3(.29, .26, .18), fbm(p * 8. + uSeed));
  vec3 col = mix(mud, stone * lit, gap);
  col = mix(col, mud * .85, smoothstep(.35, .85, depth + .25 * (noise(p * 6.) - .5)));
  // boulders in the shallows
  vec2 bq = p * 4.2 + uSeed * .37;
  vec2 bi = floor(bq), bf = fract(bq);
  float bd = 8.;
  vec2 bid = vec2(0), brc = vec2(0);
  for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
    vec2 g = vec2(x, y);
    vec2 r = g + hash22(bi + g + 17.) - bf;
    float d = dot(r, r);
    if (d < bd) { bd = d; bid = bi + g; brc = r; }
  }
  float br = .36 + .1 * hash12(bid) + .05 * (noise(p * 30.) - .5);
  float boulder = (1. - smoothstep(br - .03, br, sqrt(bd))) * step(.45, hash12(bid + 4.)) * (1. - smoothstep(.3, .55, depth));
  vec3 bn = normalize(vec3(-brc * 2.6, 1.));
  vec3 rock = mix(vec3(.36, .36, .34), vec3(.46, .44, .40), noise(p * 40.)) * (.45 + .7 * max(dot(bn, L), 0.));
  rock = mix(rock, rock * vec3(.6, .85, .45), smoothstep(.45, .7, fbm(p * 14.)) * .8);   // moss
  col = mix(col * (1. - .45 * smoothstep(br + .08, br, sqrt(bd)) * step(.45, hash12(bid + 4.))), rock, boulder);
  depth = mix(depth, depth * .4, boulder);
  // algae
  float alg = smoothstep(.5, .8, fbm(p * 3. + 11.));
  col = mix(col, col * vec3(.55, .8, .42), alg * .75);
  o = vec4(col, depth);
}
