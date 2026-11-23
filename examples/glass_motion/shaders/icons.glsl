#version 450
// White icons with soft edges: 0 = play (a rounded triangle), 1 = plus.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 3, binding = 0) uniform Params { float kind; float size; };   // size: canvas pixels

float sd_triangle(vec2 p) {   // equilateral, pointing right, circumradius 1
    p = vec2(p.y, -p.x);      // the standard one points up
    const float k = sqrt(3.0);
    p.x = abs(p.x) - 0.866;
    p.y = p.y + 0.5;
    if (p.x + k * p.y > 0.0) p = vec2(p.x - k * p.y, -k * p.x - p.y) / 2.0;
    p.x -= clamp(p.x, -1.732, 0.0);
    return -length(p) * sign(p.y);
}

float sd_box(vec2 p, vec2 h, float r) {
    vec2 q = abs(p) - h + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
    vec2 p = v_uv * 2.0 - 1.0;
    float d;
    if (kind < 0.5) d = sd_triangle(vec2(-p.x - 0.1, p.y) / 0.78) * 0.78 - 0.08;   // mirrored: points right
    else d = min(sd_box(p, vec2(0.72, 0.12), 0.12), sd_box(p, vec2(0.12, 0.72), 0.12));
    float aa = 2.0 / size;
    o_color = vec4(1.0, 1.0, 1.0, clamp(0.5 - d / aa, 0.0, 1.0));
}
