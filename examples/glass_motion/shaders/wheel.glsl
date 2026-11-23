#version 450
// A colour wheel: hue around, saturation outward, white in the middle.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

vec3 hsv(float h, float s, float v) {
    vec3 k = clamp(abs(fract(h + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
    return v * mix(vec3(1.0), k, s);
}

void main() {
    vec2 d = v_uv * 2.0 - 1.0;
    float r = length(d);
    float h = fract(0.27 - atan(d.x, -d.y) / 6.28318530718);   // clockwise from the top: yellow, red, magenta, blue, cyan, green
    o_color = vec4(hsv(h, clamp(r, 0.0, 1.0), 1.0), 1.0);
}
