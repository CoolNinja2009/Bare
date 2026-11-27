#version 450
// mesh.glsl: the jellyfish's skins (three.js ShaderMaterials of the page): the
// bulb (bulb-frag), its gel (gel-frag) and the tail and mouth (tail-frag), one
// instance a triangle of the mesh. Each vertex sits between its last two
// physics steps (lerp_pos_vertex: mix(positionPrev, position, stepProgress)).
// WebGL drew them with back faces culled (FrontSide); a bare_draw has no
// culling, so the vertex shader works out the whole triangle and drops it when
// it faces away.
layout(set = BARE_PARAMS_SET, binding = 0) uniform Params {
    mat4 projection;
    mat4 model_view;
    mat4 view;       // the camera's (viewMatrix)
    vec4 diffuse;    // rgb, opacity
    vec4 diffuse_b;  // rgb, scale (tail-frag)
    vec4 misc;       // mode (0 bulb, 1 gel, 2 tail), stepProgress, time
};

#ifdef BARE_VERTEX
layout(location = 0) in vec4 i_p0;   // position xyz, uv.x
layout(location = 1) in vec4 i_q0;   // positionPrev xyz, uv.y
layout(location = 2) in vec4 i_p1;
layout(location = 3) in vec4 i_q1;
layout(location = 4) in vec4 i_p2;
layout(location = 5) in vec4 i_q2;
layout(location = 0) out vec2 vUv;
layout(location = 1) out vec3 vNormal;

vec4 clip_of(vec4 p, vec4 q) {
    vec4 mvPosition = model_view * vec4(mix(q.xyz, p.xyz, misc.y), 1.0);
    return projection * mvPosition;
}

void main() {
    int k = gl_VertexIndex;
    vec4 c0 = clip_of(i_p0, i_q0), c1 = clip_of(i_p1, i_q1), c2 = clip_of(i_p2, i_q2);
    // front faces wind anticlockwise on screen: the sign of the homogeneous determinant
    float facing = determinant(mat3(c0.xyw, c1.xyw, c2.xyw));
    vec4 p = k == 0 ? i_p0 : k == 1 ? i_p1 : i_p2;
    vec4 q = k == 0 ? i_q0 : k == 1 ? i_q1 : i_q2;
    vec4 clip = k == 0 ? c0 : k == 1 ? c1 : c2;
    vUv = vec2(p.w, q.w);
    vNormal = normalize(p.xyz);
    // GL's clip space to this one: depth 0..1
    gl_Position = facing > 0.0 ? vec4(clip.xy, (clip.z + clip.w) * 0.5, clip.w) : vec4(2.0, 2.0, 2.0, 1.0);
}
#else
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec3 vNormal;
layout(location = 0) out vec4 o_color;

const vec3 eye = vec3(0.0, 0.0, 1.0);

float oscillate(float vMin, float vMax, float t) {
  float halfRange = (vMax - vMin) / vMax * 0.5;
  return (sin(t) * halfRange + (1.0 - halfRange)) * vMax;
}

float accumulate_bulb(vec2 uv, float saturation, float scale) {
  saturation -= sin(uv.x * 60.0) * 0.25 + sin(uv.x * 50.0 * scale) * 0.25 + 0.75;

  saturation -= sin(uv.y * sin(uv.x         * 5.0) * 5.0 * scale) * 0.05;
  saturation -= sin(uv.y * sin((1.0 - uv.x) * 5.0) * 5.0 * scale) * 0.05;

  saturation -= sin(uv.y * sin(uv.y + cos(uv.x)       * 2.0) * 3.0 * scale) * 0.15;
  saturation -= sin(uv.y * sin(uv.y + cos(1.0 - uv.x) * 2.0) * 3.0 * scale) * 0.15;

  saturation -= sin((uv.y - 1.5) * sin(uv.y + cos(uv.x - 1.0) * 2.0) * 4.0 * scale) * 0.15;
  saturation -= sin((uv.y - 1.5) * sin(uv.y + cos(uv.x)       * 2.0) * 3.0 * scale) * 0.15;

  saturation -= sin(uv.y * 5.0) * 0.15 + sin(uv.y * 2.5) * 1.25;

  return saturation;
}

float accumulate_tail(vec2 uv, float saturation, float scale) {
  saturation -= sin(uv.y * 12.0 * scale) * 0.8 + uv.y * 1.5 + sin(uv.x * 20.0 * scale) * 0.1 + 0.85;

  saturation -= sin(uv.y * sin(uv.x         * 5.0) * 5.0 * scale) * 0.05;
  saturation -= sin(uv.y * sin((1.0 - uv.x) * 5.0) * 5.0 * scale) * 0.05;

  saturation -= sin(uv.y * sin(uv.y + cos(uv.x)       * 2.0) * 10.0 * scale) * 0.15;
  saturation -= sin(uv.y * sin(uv.y + cos(1.0 - uv.x) * 2.0) * 10.0 * scale) * 0.15;

  return saturation;
}

void main() {
  vec3 normal = normalize(mat3(view) * vNormal);
  float rim = 1.0 - max(dot(eye, normal), 0.0);
  float opacity = diffuse.a;
  if (misc.x < 0.5) {   // bulb
    float time = misc.z;
    float saturation = 0.0;
    vec2 uv0 = vUv;
    vec2 uv1 = uv0 + rim;
    vec2 uv2 = vec2(-rim * 0.25);
    vec2 uv3 = vec2(rim, uv0.y);
    float scale0 = oscillate( 8.0, 15.0, time * 0.25 + 0.5);
    float scale1 = oscillate(12.0, 20.0, time * 0.125);
    float scale2 = 1.0;
    float scale3 = 1.0;
    saturation += max(accumulate_bulb(uv0, 2.0, scale0), -0.5);
    saturation += max(accumulate_bulb(uv1, 2.0, scale1),  0.25);
    saturation += max(accumulate_bulb(uv2, 1.0, scale2), -0.25);
    saturation += max(accumulate_bulb(uv3, 1.0, scale3), -0.25);
    o_color = vec4(mix(diffuse.rgb, diffuse_b.rgb, smoothstep(-0.5, 0.5, saturation)),
                   (1.0 - smoothstep(-0.5, 2.5, saturation)) * opacity);
  } else if (misc.x < 1.5) {   // gel, added: premultiplied for BARE_BLEND_ADD (blendFunc SRC_ALPHA, ONE)
    float rimLight = 0.25 + smoothstep(0.25, 1.0, rim) * 0.5 + smoothstep(0.90, 1.0, rim) * 0.8;
    vec3 c = clamp(diffuse.rgb * vec3(rimLight), 0.0, 1.0);
    o_color = vec4(c * opacity, opacity);
  } else {   // tail and mouth
    float scale = diffuse_b.a;
    vec2 uv = vUv;
    float saturation = 0.0;
    saturation += accumulate_tail(uv, 2.0, scale);
    saturation += max(accumulate_tail(vec2(rim), 0.75, scale * 0.25), -0.25);
    o_color = vec4(mix(diffuse_b.rgb, diffuse.rgb, saturation) * opacity, clamp(saturation, 0.2, 1.0) * opacity);
  }
}
#endif
