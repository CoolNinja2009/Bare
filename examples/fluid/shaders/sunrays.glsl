#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
// Radial blur of the mask towards the centre: god rays.
layout(set = 2, binding = 0) uniform sampler2D u_mask;
layout(set = 3, binding = 0) uniform Params { float weight; };

void main() {
    const int ITERATIONS = 16;
    const float DENSITY = 0.3, DECAY = 0.95, EXPOSURE = 0.7;
    vec2 coord = v_uv;
    vec2 dir = (v_uv - 0.5) * (DENSITY / float(ITERATIONS));
    float decay = 1.0;
    float color = texture(u_mask, v_uv).a;
    for (int i = 0; i < ITERATIONS; i++) {
        coord -= dir;
        color += texture(u_mask, coord).a * decay * weight;
        decay *= DECAY;
    }
    o_color = vec4(color * EXPOSURE, 0.0, 0.0, 1.0);
}
