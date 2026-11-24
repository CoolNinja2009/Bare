#version 450
// up.glsl: one step back up the bloom chain, added onto the level above.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
layout(set = 2, binding = 0) uniform sampler2D uSrc;
layout(set = 3, binding = 0) uniform Params { vec2 uTexel; };

void main() {
  vec2 vUv = v_uv;
  vec3 c = texture(uSrc, vUv + uTexel * vec2(-0.5, -0.5)).rgb + texture(uSrc, vUv + uTexel * vec2(0.5, -0.5)).rgb
         + texture(uSrc, vUv + uTexel * vec2(-0.5, 0.5)).rgb + texture(uSrc, vUv + uTexel * vec2(0.5, 0.5)).rgb;
  o = vec4(c * 0.25, 1.0);
}
