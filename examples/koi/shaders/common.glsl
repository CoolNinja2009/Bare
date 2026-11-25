// common.glsl: noise, cells and caustics shared by the koi shaders (the page's COMMON).
float hash12(vec2 p) { vec3 p3 = fract(vec3(p.xyx) * .1031); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.x + p3.y) * p3.z); }
vec2 hash22(vec2 p) { vec3 p3 = fract(vec3(p.xyx) * vec3(.1031, .1030, .0973)); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.xx + p3.yz) * p3.zy); }
float noise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  vec2 u = f * f * (3. - 2. * f);
  return mix(mix(hash12(i), hash12(i + vec2(1, 0)), u.x), mix(hash12(i + vec2(0, 1)), hash12(i + vec2(1, 1)), u.x), u.y);
}
float fbm(vec2 p) {
  float s = 0., a = .5;
  for (int i = 0; i < 5; i++) { s += a * noise(p); p = mat2(1.6, 1.2, -1.2, 1.6) * p; a *= .5; }
  return s;
}
// distance between the two nearest moving cell points: thin bright lines where cells meet
float cells(vec2 p, float t) {
  vec2 i = floor(p), f = fract(p);
  float d1 = 8., d2 = 8.;
  for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
    vec2 g = vec2(x, y);
    vec2 o = .5 + .42 * sin(t + 6.2831 * hash22(i + g));
    vec2 r = g + o - f;
    float d = dot(r, r);
    if (d < d1) { d2 = d1; d1 = d; } else if (d < d2) d2 = d;
  }
  return sqrt(d2) - sqrt(d1);
}
float caustic(vec2 p, float t) {
  vec2 w = p + .3 * vec2(sin(p.y * 1.3 + t * .37), cos(p.x * 1.1 - t * .31)) + .45 * vec2(noise(p * 1.7 + t * .15), noise(p * 1.7 + 7.3 - t * .13));
  float a = cells(w, t * .7), b = cells(w * 1.7 + 3.7, -t * .55);
  return pow(1. - smoothstep(0., .2, a), 3.) * .75 + pow(1. - smoothstep(0., .16, b), 3.) * .4;
}
