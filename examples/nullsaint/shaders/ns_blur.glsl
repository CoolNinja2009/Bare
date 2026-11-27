#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
layout(set = 2, binding = 0) uniform sampler2D uSrc;
layout(set = 3, binding = 0) uniform Params { vec2 uDir; };
void main() {
    vec3 s = texture(uSrc, v_uv).rgb * 0.227027;
    s += texture(uSrc, v_uv + uDir * 1.3846).rgb * 0.3162162;
    s += texture(uSrc, v_uv - uDir * 1.3846).rgb * 0.3162162;
    s += texture(uSrc, v_uv + uDir * 3.2307).rgb * 0.0702703;
    s += texture(uSrc, v_uv - uDir * 3.2307).rgb * 0.0702703;
    o = vec4(s, 1.0);
}
