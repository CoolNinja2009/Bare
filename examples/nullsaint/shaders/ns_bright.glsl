#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
layout(set = 2, binding = 0) uniform sampler2D uSrc;
layout(set = 3, binding = 0) uniform Params { float uThr; };
void main() {
    vec3 c = texture(uSrc, v_uv).rgb;
    float l = max(c.r, max(c.g, c.b));
    o = vec4(c * smoothstep(uThr, 1.0, l) * 1.4, 1.0);
}
