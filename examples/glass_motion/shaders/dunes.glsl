#version 450
// Sand dunes at dusk: ridges one behind another, each lit on one face and in
// shade on the other, drifting slowly.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 3, binding = 0) uniform Params { float time; float aspect; };

vec3 sand(float k) { return mix(vec3(0.30, 0.21, 0.14), vec3(0.95, 0.83, 0.66), clamp(k, 0.0, 1.0)); }

void main() {
    vec2 p = vec2(v_uv.x * aspect, v_uv.y);
    vec3 col = mix(vec3(0.86, 0.75, 0.60), vec3(0.70, 0.56, 0.42), v_uv.y);   // hazy distance
    for (int i = 0; i < 7; i++) {
        float fi = float(i);
        float f = 2.2 + 0.45 * fi;
        float x = p.x * f + fi * 1.9 + time * (0.05 + 0.02 * fi);
        float ridge = 0.06 + fi * 0.15 + 0.045 * sin(x) + 0.025 * sin(x * 2.3 + 1.1 + fi);
        float slope = 0.045 * cos(x) + 0.0575 * cos(x * 2.3 + 1.1 + fi);
        float d = p.y - ridge;                       // > 0 below the crest
        if (d > 0.0) {
            float lit = smoothstep(-0.03, 0.03, slope);              // which face the low sun hits
            float fall = exp(-d * (5.0 + 2.0 * fi));                 // the face darkens away from the crest
            float k = mix(0.25, 0.9, lit) * (0.55 + 0.45 * fall) + 0.08 * fi / 7.0;
            vec3 c = sand(k);
            c += vec3(0.08, 0.06, 0.04) * smoothstep(0.012, 0.0, d) * lit;   // a bright crest line
            col = mix(col, c, smoothstep(0.0, 0.0035, d));
        }
    }
    o_color = vec4(col, 1.0);
}
