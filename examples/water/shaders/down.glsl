#version 450
// down.glsl: one step down the bloom chain (a 4-tap box at the corners of the source texel).
// As in examples/singularity.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
layout(set = 2, binding = 0) uniform sampler2D uSrc;
layout(set = 3, binding = 0) uniform Params { vec2 uTexel; };

void main() {
  vec2 vUv = v_uv;
  vec3 c = texture(uSrc, vUv + uTexel * vec2(-1.0, -1.0)).rgb + texture(uSrc, vUv + uTexel * vec2(1.0, -1.0)).rgb
         + texture(uSrc, vUv + uTexel * vec2(-1.0, 1.0)).rgb + texture(uSrc, vUv + uTexel * vec2(1.0, 1.0)).rgb;
  o = vec4(c * 0.25, 1.0);
}
