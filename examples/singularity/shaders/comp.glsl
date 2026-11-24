#version 450
// comp.glsl: the full-resolution picture: lensing by the cursor and the wave,
// stars, bloom, tone mapping. The original's COMP_FS line for line.
//
// Every buffer, this pass's target included, is a bottom_up canvas: rows run
// as in WebGL, so gl_FragCoord, and the 2x2 quads that derivatives come from,
// are WebGL's.
//
// Two spots are written as WebGL compiles them on Windows, so the bits match:
// ANGLE expands pow() with a whole exponent into multiplies, and folds
// constant expressions in 32-bit floats (glslang folds in doubles).
layout(location = 0) out vec4 oColor;
layout(set = 2, binding = 0) uniform sampler2D uScene;
layout(set = 2, binding = 1) uniform sampler2D uDir;
layout(set = 2, binding = 2) uniform sampler2D uBloom;
layout(set = 3, binding = 0) uniform Params {
    layout(row_major) mat3 uCamRot;     // the camera, to rebuild each pixel's unbent ray exactly
    vec3 uLens;       // cursor px, Einstein radius px (0 = off)
    float uFocal;
    vec4 uWave;       // gravitational wave: front radius, amplitude, width, wavelength (px)
    vec2 uRes;
    vec2 uHole;       // the hole on screen, px
    vec2 uSceneSize;  // the ray-traced buffer, px
    vec2 uShift;
    float uPxAng;     // radians per pixel
    float uStarS2;    // star size: squared radius in px, grows with the screen so 8K looks like 1080p
    float uExposure, uBloomK;
};

float hash13(vec3 p3) {
  p3 = fract(p3 * 0.1031);
  p3 += dot(p3, p3.zyx + 31.32);
  return fract((p3.x + p3.y) * p3.z);
}
vec3 hash33(vec3 p3) {
  p3 = fract(p3 * vec3(0.1031, 0.1030, 0.0973));
  p3 += dot(p3, p3.yxz + 33.33);
  return fract((p3.xxy + p3.yxx) * p3.zyx);
}

// Upscales the ray-traced buffer with a 5-tap Catmull-Rom filter: bicubic sharpness, so the
// shadow's edge and the photon ring stay crisp on 4K and 8K screens.
vec4 sceneTex(vec2 uv) {
  vec2 pos = uv * uSceneSize;
  vec2 t1 = floor(pos - 0.5) + 0.5, f = pos - t1;
  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
  vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
  vec2 w3 = f * f * (-0.5 + 0.5 * f);
  vec2 w12 = w1 + w2;
  vec2 t0 = (t1 - 1.0) / uSceneSize, t3 = (t1 + 2.0) / uSceneSize, t12 = (t1 + w2 / w12) / uSceneSize;
  vec4 c = texture(uScene, vec2(t12.x, t0.y)) * (w12.x * w0.y)
         + texture(uScene, vec2(t0.x, t12.y)) * (w0.x * w12.y)
         + texture(uScene, t12) * (w12.x * w12.y)
         + texture(uScene, vec2(t3.x, t12.y)) * (w3.x * w12.y)
         + texture(uScene, vec2(t12.x, t3.y)) * (w12.x * w3.y);
  c /= w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
  return vec4(max(c.rgb, 0.0), clamp(c.a, 0.0, 1.0));
}

// One layer of stars on a cube-mapped grid. Stars are point sources: lensing moves them and
// changes their brightness but never smears them, so each is drawn about a pixel wide using
// the local sky-to-screen mapping (dx, dy = sky uv per screen pixel).
vec3 starLayer(vec2 uv, float face, vec2 dx, vec2 dy, float N, float density, float bright, float halo, float seed) {
  vec2 g = uv * N, id = floor(g);
  vec3 h = hash33(vec3(id, face * 7.13 + seed));
  if (h.z > density) return vec3(0.0);
  vec2 dd = g - id - (0.25 + 0.5 * h.xy);    // star offset, in cells
  dx *= N;
  dy *= N;
  float det = dx.x * dy.y - dx.y * dy.x;
  det = det < 0.0 ? min(det, -1e-12) : max(det, 1e-12);
  vec2 o = vec2(dd.x * dy.y - dd.y * dy.x, dx.x * dd.y - dx.y * dd.x) / det;   // the same offset in pixels
  float r2 = dot(o, o);
  float fade = 1.0 - h.z / density;
  float lum = bright * (0.12 + 0.88 * (fade * fade * fade * fade));   // pow(fade, 4.0)
  vec3 tint = mix(vec3(1.0, 0.8, 0.64), vec3(0.72, 0.84, 1.0), fract(h.x * 13.7 + h.y * 7.1));
  return tint * lum * (exp(-r2 / uStarS2) + halo * exp(-r2 / (uStarS2 * 12.0)));   // halo stays inside the cell
}

vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }

void main() {
  vec2 px = gl_FragCoord.xy, src = px;
  // The cursor is a point-mass lens: the thin-lens equation, fading out with distance.
  if (uLens.z > 0.5) {
    vec2 v = px - uLens.xy;
    float d2 = dot(v, v), e2 = uLens.z * uLens.z, x = d2 / (e2 * 30.0);
    src -= v * (e2 / (d2 + 0.03 * e2)) / (1.0 + x * x);
  }
  // After a meal, a quadrupolar gravitational wave races out from the hole.
  if (uWave.y > 0.01) {
    vec2 v = px - uHole;
    float r = length(v) + 1.0, x = r - uWave.x;
    float h = uWave.y * exp(-x * x / (uWave.z * uWave.z)) * sin(6.2832 * x / uWave.w) * sqrt(300.0 / (r + 300.0));
    vec2 n = v / r;
    float c2 = n.x * n.x - n.y * n.y, s2 = 2.0 * n.x * n.y;
    src += h * (c2 * n - s2 * vec2(-n.y, n.x));
  }
  vec2 uv = src / uRes;
  vec4 sc = sceneTex(uv);
  vec3 defl = texture(uDir, uv).xyz;
  vec2 p = (src - 0.5 * uRes) / uRes.y - uShift;
  vec3 dir = normalize(normalize(uCamRot * vec3(p, uFocal)) + defl);   // this pixel's ray, bent as traced

  // Sky coordinates on the cube face, and how the lensed sky maps onto screen pixels.
  vec3 a = abs(dir);
  vec2 sky; float face;
  if (a.x >= a.y && a.x >= a.z) { sky = dir.yz / a.x; face = dir.x > 0.0 ? 0.0 : 1.0; }
  else if (a.y >= a.z) { sky = dir.zx / a.y; face = dir.y > 0.0 ? 2.0 : 3.0; }
  else { sky = dir.xy / a.z; face = dir.z > 0.0 ? 4.0 : 5.0; }
  vec2 dx = dFdx(sky), dy = dFdy(sky);
  float pxSky = uPxAng * (1.0 + 0.5 * dot(sky, sky));   // one unlensed pixel, in sky units
  float mag = clamp(pxSky * pxSky / max(abs(dx.x * dy.y - dx.y * dy.x), 1e-14), 0.0, 8.0);   // lensing magnification

  vec3 col = sc.rgb;
  col += sc.a * mag * (starLayer(sky, face, dx, dy, 260.0, 0.05, 0.5, 0.0, 1.0)
                     + starLayer(sky, face, dx, dy, 110.0, 0.04, 1.4, 0.0, 2.0)
                     + starLayer(sky, face, dx, dy, 42.0, 0.035, 4.5, 0.08, 3.0));
  col += texture(uBloom, uv).rgb * uBloomK;
  vec2 q = px / uRes - 0.5;
  col *= uExposure * (1.0 - 0.45 * dot(q, q));
  col = pow(aces(col), vec3(0.454545438));   // 1.0 / 2.2
  oColor = vec4(col + (hash13(vec3(px, 1.3)) - 0.5) / 255.0, 1.0);
}
