#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
// Feedback: datamosh, melt, cursor smear, time freeze (shaders.js SH.mosh).
layout(set = 2, binding = 0) uniform sampler2D uScene;   // top-down: sampled with v flipped
layout(set = 2, binding = 1) uniform sampler2D uPrev;
layout(set = 3, binding = 0) uniform Params {
    vec2 uRes, uMouse, uMouseVel;
    float uTime, uMosh, uMelt, uTrail, uSeed, uFreeze, uSwirl;
};
float h1(float n) { return fract(sin(n) * 43758.5453123); }
float h2(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123); }
void main() {
    vec2 uv = v_uv;
    vec4 sc = texture(uScene, vec2(uv.x, 1.0 - uv.y));
    vec2 g = vec2(32.0, 18.0);
    vec2 b = floor(uv * g);
    float tq = floor(uTime * 3.0 + uSeed);
    float r = h2(b + tq);
    vec2 mv = (vec2(h2(b + tq + 3.1), h2(b + tq + 7.7)) - 0.5) * 0.012;
    float moshMask = step(1.0 - uMosh * 0.55, r);
    vec2 flow = mv * moshMask;
    float col = h1(floor(uv.x * uRes.x / 3.0) + uSeed);
    flow.y += uMelt * (0.0008 + 0.009 * col * col);
    vec2 c = uv - 0.5;
    float ang = uSwirl * 0.03 * exp(-dot(c, c) * 6.0);
    flow += vec2(-c.y, c.x) * ang;
    float asp = uRes.x / uRes.y;
    vec2 dm = (uv - uMouse) * vec2(asp, 1.0);
    float near = exp(-dot(dm, dm) * 55.0);
    flow -= uMouseVel * near * 0.9;
    vec4 pv = texture(uPrev, uv + flow);
    float sp = clamp(length(uMouseVel) * 60.0, 0.0, 1.0);
    float m = moshMask * uMosh * 0.92 + near * uTrail * sp * 0.93 + uMelt * (0.35 + 0.62 * col) + abs(uSwirl) * 0.6;
    m = clamp(max(m, uFreeze), 0.0, 0.985);
    o = vec4(mix(sc.rgb, pv.rgb, m), 1.0);
}
