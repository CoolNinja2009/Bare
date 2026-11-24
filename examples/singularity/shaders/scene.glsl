#version 450
#extension GL_EXT_control_flow_attributes : require
// scene.glsl: traces every pixel's photon backwards through the hole's gravity
// (units: Schwarzschild radius = 1) into the low-resolution HDR buffers.
//
// The body is the original's SCENE_FS line for line. Its targets are
// bottom_up canvases, so gl_FragCoord is WebGL's and nothing is flipped.
layout(location = 0) out vec4 oColor;   // rgb: light gathered along the ray, a: what still shows through
layout(location = 1) out vec4 oDir;     // how far gravity bent the ray (final minus initial direction)

layout(set = 3, binding = 0) uniform Params {
    layout(row_major) mat3 uCamRot;
    vec3 uCamPos; float uFocal;
    vec4 uPhase;      // disk texture: two flow-phase times and their seeds
    vec3 uTint; float uT0;
    vec2 uRes; vec2 uShift;
    vec2 uFlowW;      // and their crossfade weights
    vec2 uShock;      // radius and strength of the bright ring a meal sends through the disk
    float uStepK, uEscR, uSpectral, uDiskGain;
    float uJets, uJetPhase, uNebula, uBAA;   // uBAA: one traced pixel, in impact parameter (the shadow edge's AA)
    int uSteps;
    int uBodies;      // large bodies in flight, each a chain of FRAGS pieces:
    vec4 uFrag[48];   // xyz position, w brightness (0 = gone)
    vec4 uBodyA[4];   // xyz bounding-sphere centre, w its radius
    vec4 uBodyB[4];   // rgb colour, w tube radius
};

const float B_CRIT = 2.598076;   // critical impact parameter (3*sqrt(3)/2): rays aimed closer fall in
const float R_IN = 2.60;
const float R_OUT = 11.00;
const int FRAGS = 12;
// WebGL folds constant expressions in 32-bit floats, glslang in doubles: where
// the two differ by a bit, the value is written out as WebGL computes it.
const float R_IN_HI = 3.14999986;                        // R_IN + 0.55
const float HAZE_LO = 5.40799952, HAZE_HI = 10.815999;   // R_IN * R_IN * 0.8 and 1.6
const vec3 BAND = vec3(-0.562249899, 0.817818046, 0.122672699);   // normalize(vec3(-0.55, 0.8, 0.12))

float hash13(vec3 p3) {
  p3 = fract(p3 * 0.1031);
  p3 += dot(p3, p3.zyx + 31.32);
  return fract((p3.x + p3.y) * p3.z);
}

// Squared distance between segments p1-q1 (a ray step) and p2-q2 (a piece of a stream);
// t receives where along p2-q2 the closest point lies.
float segDist2(vec3 p1, vec3 q1, vec3 p2, vec3 q2, out float t) {
  vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
  float a = dot(d1, d1), e = max(dot(d2, d2), 1e-8), f = dot(d2, r), c = dot(d1, r), b = dot(d1, d2);
  float den = a * e - b * b;
  float s = den > 1e-10 ? clamp((b * f - c * e) / den, 0.0, 1.0) : 0.0;
  t = (b * s + f) / e;
  if (t < 0.0) { t = 0.0; s = clamp(-c / a, 0.0, 1.0); }
  else if (t > 1.0) { t = 1.0; s = clamp((b - c) / a, 0.0, 1.0); }
  vec3 dd = r + d1 * s - d2 * t;
  return dot(dd, dd);
}

float vnoise(vec3 p) {
  vec3 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(mix(hash13(i), hash13(i + vec3(1, 0, 0)), f.x),
                 mix(hash13(i + vec3(0, 1, 0)), hash13(i + vec3(1, 1, 0)), f.x), f.y),
             mix(mix(hash13(i + vec3(0, 0, 1)), hash13(i + vec3(1, 0, 1)), f.x),
                 mix(hash13(i + vec3(0, 1, 1)), hash13(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
float fbm(vec3 p) {
  float s = 0.0, a = 0.5;
  [[unroll]] for (int i = 0; i < 4; i++) { s += a * vnoise(p); p = p * 2.03 + vec3(1.7, 9.2, 3.1); a *= 0.5; }
  return s;
}

// Planckian locus (Krystek), brightest channel normalised to 1.
vec3 blackbody(float t) {
  t = clamp(t, 1000.0, 15000.0);
  float u = (0.860117757 + 1.54118254e-4 * t + 1.28641212e-7 * t * t) / (1.0 + 8.42420235e-4 * t + 7.08145163e-7 * t * t);
  float v = (0.317398726 + 4.22806245e-5 * t + 4.20481691e-8 * t * t) / (1.0 - 2.89741816e-5 * t + 1.61456053e-7 * t * t);
  float x = 3.0 * u / (2.0 * u - 8.0 * v + 4.0), y = 2.0 * v / (2.0 * u - 8.0 * v + 4.0);
  vec3 c = vec3(x / y, 1.0, (1.0 - x - y) / y) * mat3(3.2404542, -1.5371385, -0.4985314,
                                                      -0.9692660, 1.8760108, 0.0415560,
                                                       0.0556434, -0.2040259, 1.0572252);
  c = max(c, 0.0);
  return c / max(max(c.r, c.g), max(c.b, 1e-4));
}

// The thin disk where the ray crosses y = 0: premultiplied emission and opacity.
vec4 disk(vec3 hit, float r, vec3 rd) {
  float inner = smoothstep(R_IN - 0.3, R_IN_HI, r);
  float prof = inner * (1.0 - smoothstep(R_OUT * 0.45, R_OUT, r));
  if (prof <= 0.001) return vec4(0.0);

  // Streaks co-rotating at the Keplerian rate, in two crossfaded phases so the
  // differential rotation never winds them into noise.
  float ang = atan(hit.z, hit.x);
  float w = 0.35 * pow(R_IN / r, 1.5);
  float a1 = ang - w * uPhase.x, a2 = ang - w * uPhase.y;
  float n1 = fbm(vec3(cos(a1) * 2.2, sin(a1) * 2.2, r * 2.3) + uPhase.z * vec3(7.31, 3.17, 5.71));
  float n2 = fbm(vec3(cos(a2) * 2.2, sin(a2) * 2.2, r * 2.3) + uPhase.w * vec3(7.31, 3.17, 5.71));
  float n = 0.5 + (n1 * uFlowW.x + n2 * uFlowW.y - 0.5) * inversesqrt(dot(uFlowW, uFlowW));
  float dens = clamp(0.15 + 1.9 * n * n, 0.0, 2.2) * (0.86 + 0.14 * sin(r * 5.0 + n * 9.0));

  // Doppler beaming and gravitational redshift of gas on circular orbits.
  vec3 v = normalize(vec3(-hit.z, 0.0, hit.x));
  float beta = min(sqrt(0.5 / max(r - 1.0, 0.6)), 0.75);
  float g = sqrt(1.0 - beta * beta) / (1.0 - beta * dot(v, -rd)) * sqrt(max(1.0 - 1.0 / r, 0.05));
  float heat = pow(R_IN / r, 0.6);
  vec3 c = blackbody(uT0 * heat * pow(g, 0.9));
  vec3 iri = 0.55 + 0.45 * cos(6.2832 * (vec3(0.0, 0.33, 0.67) + r * 0.06 + n * 0.35 - g * 0.3));
  c = mix(c, iri, uSpectral);
  float I = prof * dens * heat * heat * pow(g, 2.2) * uDiskGain * 2.2;
  I += uShock.y * exp(-(r - uShock.x) * (r - uShock.x) * 2.0) * 4.0 * prof;
  return vec4(c * I, clamp(prof * (0.3 + 0.7 * dens), 0.0, 0.94));
}

vec3 nebula(vec3 d) {
  float b = dot(d, BAND);   // a galactic band running behind the hole
  float band = exp(-b * b * 9.0);
  float n1 = fbm(d * 2.2 + 11.0);
  float n2 = fbm(d * 4.7 + n1 * 1.8 + 3.0);
  vec3 c = mix(vec3(0.32, 0.10, 0.55), vec3(0.05, 0.30, 0.50), smoothstep(0.3, 0.7, n1));
  c = mix(c, vec3(0.85, 0.42, 0.30), smoothstep(0.55, 0.85, n2) * band);
  return c * (0.05 + 0.9 * band) * smoothstep(0.32, 0.85, n2 * 0.6 + n1 * 0.6) * 0.35;
}

void main() {
  vec2 frag = gl_FragCoord.xy;
  vec2 p = (frag - 0.5 * uRes) / uRes.y - uShift;
  vec3 vel = normalize(uCamRot * vec3(p, uFocal));
  vec3 ray0 = vel;
  vec3 pos = uCamPos;
  vec3 lm = cross(pos, vel);
  float h2 = dot(lm, lm);     // squared angular momentum: fixes how hard the hole pulls this ray

  vec3 col = vec3(0.0);
  float T = 1.0, state = 0.0;   // state: 0 running, 1 escaped, 2 fell in
  float jit = hash13(vec3(frag, 3.7));
  float r = length(pos);
  vec3 acc = -(1.5 * h2 / (r * r * r * r * r)) * pos;

  for (int i = 0; i < uSteps; i++) {
    // Leapfrog with a step that shrinks near the hole. It stays deterministic so neighbouring
    // rays bend alike and the stars stay sharp; only the glow sampling below is dithered.
    float dt = clamp(uStepK * (r - 0.9), 0.008, 4.0);
    vel += acc * (dt * 0.5);
    vec3 prev = pos;
    pos += vel * dt;
    r = length(pos);
    acc = -(1.5 * h2 / (r * r * r * r * r)) * pos;
    vel += acc * (dt * 0.5);
    vec3 vp = mix(prev, pos, jit);   // where this step samples the haze and jets

    if ((prev.y > 0.0) != (pos.y > 0.0)) {
      vec3 hit = mix(prev, pos, prev.y / (prev.y - pos.y));
      float rh = length(hit.xz);
      if (rh > R_IN - 0.3 && rh < R_OUT) {
        vec4 d = disk(hit, rh, normalize(vel));
        col += T * d.rgb;
        T *= 1.0 - d.a;
      }
    }

    // Thin glowing haze above and below the disk.
    float rxz2 = dot(vp.xz, vp.xz);
    float hz = exp(-vp.y * vp.y * 2.0) * smoothstep(HAZE_LO, HAZE_HI, rxz2) / (1.0 + rxz2 * 0.12);
    col += T * uTint * (hz * dt * 0.12 * uDiskGain);

    // Relativistic jets along the spin axis, fired when the hole feeds.
    if (uJets > 0.002) {
      float hy = abs(vp.y);
      float w = 0.08 + 0.06 * hy;
      float j = exp(-rxz2 / (w * w)) * smoothstep(1.0, 2.4, hy) * exp(-hy * 0.06) / (0.4 + w);
      float knot = 0.6 + 0.4 * sin(hy * 1.2 - uJetPhase);
      col += T * vec3(0.4, 0.6, 1.0) * (uJets * j * knot * dt * (vp.y > 0.0 ? 1.0 : 0.45));
    }

    // Large bodies: each a chain of pieces drawn as one glowing tube, so a body the tide pulls
    // apart stretches into a continuous stream. Integrated over the whole step so none is skipped.
    for (int b = 0; b < uBodies; b++) {
      vec4 bs = uBodyA[b], bb = uBodyB[b];
      vec3 sg = pos - prev, c = bs.xyz - prev;
      vec3 dq = c - sg * clamp(dot(c, sg) / dot(sg, sg), 0.0, 1.0);
      float reach = bs.w + 3.0 * bb.w;
      if (dot(dq, dq) > reach * reach) continue;   // this step passes nowhere near it
      float glow = 0.0;
      for (int k = 0; k < FRAGS - 1; k++) {
        vec4 f0 = uFrag[b * FRAGS + k], f1 = uFrag[b * FRAGS + k + 1];
        if (f0.w <= 0.0 || f1.w <= 0.0) continue;
        float t;
        float d2 = segDist2(prev, pos, f0.xyz, f1.xyz, t);
        float link = 1.0 - smoothstep(1.5, 3.0, distance(f0.xyz, f1.xyz));   // pieces flung apart break the stream
        glow = max(glow, exp(-d2 / (bb.w * bb.w)) * mix(f0.w, f1.w, t) * link);
      }
      col += T * bb.rgb * (glow * min(length(sg), bb.w * 1.77) / (bb.w * 1.77));
    }

    if (r < 1.0) { state = 2.0; break; }
    if (r > uEscR && dot(pos, vel) > 0.0) { state = 1.0; break; }
    if (T < 0.003) break;
  }
  // The shadow's edge comes from the impact parameter rather than from which rays happened to
  // fall in, so it is smooth at any upscale (8K included); the photon ring hugs it.
  float b = sqrt(h2);
  float edge = smoothstep(B_CRIT - uBAA, B_CRIT + uBAA, b);
  vec3 ring = uTint * (exp(-8.3 * (sqrt(max(b - B_CRIT, 0.0) + uBAA) - sqrt(uBAA))) * 0.5 * uDiskGain);
  vec3 dir = normalize(vel);
  if (state == 2.0 || (state == 0.0 && length(pos) < 4.0)) {
    col += T * edge * ring;
    T = 0.0;
  } else {
    col += T * edge * (ring + nebula(dir) * uNebula);
    T *= edge;
  }
  oColor = vec4(col, T);
  oDir = vec4(col, T);
}
