#version 450
// dof.glsl: depth of field, from the depth canvas. Each pixel's blur circle
// grows with how far it is from the focus distance; a pixel gathers the
// samples around it (a golden-angle spiral) whose own circles reach it, so
// the blurred foreground spills over what's behind it, as through a lens.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
layout(set = 2, binding = 0) uniform sampler2D uSrc;
layout(set = 2, binding = 1) uniform sampler2D uDepth;
layout(set = 3, binding = 0) uniform Params {
    vec4 uRes;    // size, 1 / size
    vec4 uLens;   // farthest focus (m), blur in front (px at half the focus distance), blur far away (px), max (px)
    vec4 uView;   // near plane (m), tan(fov x / 2), tan(fov y / 2), -
};

float distanceAt(vec2 uv) {
    float z = textureLod(uDepth, uv, 0.0).r;
    vec2 ndc = uv * 2.0 - 1.0;
    float len = length(vec3(1.0, ndc.x * uView.y, ndc.y * uView.z));
    return z > 0.0 ? uView.x / z * len : 1e6;
}

float focus;

float coc(vec2 uv) {
    float c = focus / distanceAt(uv) - 1.0;   // > 0 in front of the focus
    return min(c > 0.0 ? c * uLens.y : -c * uLens.z, uLens.w);
}

void main() {
    const int TAPS = 40;
    const float GOLDEN = 2.39996323;
    // the lens focuses on what's in the middle of the picture (the nearest of
    // a few points there), never farther than uLens.x: look down and the
    // water at your feet is sharp; look out and the foreground blurs
    focus = uLens.x;
    for (int i = 0; i < 5; i++) {
        vec2 o = i == 0 ? vec2(0.0) : 0.04 * vec2(i == 1 ? 1.0 : i == 2 ? -1.0 : 0.0, i == 3 ? 1.0 : i == 4 ? -1.0 : 0.0);
        focus = min(focus, distanceAt(vec2(0.5) + o));
    }
    focus = max(focus, 0.6);
    float c0 = coc(v_uv);
    vec3 acc = textureLod(uSrc, v_uv, 0.0).rgb;
    float wsum = 1.0;
    float reach = uLens.w;
    for (int i = 1; i < TAPS; i++) {
        float r = sqrt(float(i) / float(TAPS)) * reach;
        float a = float(i) * GOLDEN;
        vec2 uv = v_uv + vec2(cos(a), sin(a)) * r * uRes.zw;
        float c = coc(uv);
        // a sample counts if its circle reaches here, or this pixel's own circle reaches it
        // (but a sharp background can't spill onto a blurred foreground)
        float w = clamp(max(c, min(c0, c + 2.0)) - r + 1.0, 0.0, 1.0);
        acc += textureLod(uSrc, uv, 0.0).rgb * w;
        wsum += w;
    }
    o = vec4(acc / wsum, 1.0);
}
