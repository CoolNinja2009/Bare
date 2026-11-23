#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
// Final image: dye with shading, sunrays and bloom. The output is straight
// alpha, like every Bare image, so the box's .color shows through behind it.
layout(set = 2, binding = 0) uniform sampler2D u_dye;
layout(set = 2, binding = 1) uniform sampler2D u_bloom;
layout(set = 2, binding = 2) uniform sampler2D u_sunrays;
layout(set = 3, binding = 0) uniform Params { vec2 texel; float shading; float bloom; float sunrays; };

vec3 linear_to_gamma(vec3 c) {
    c = max(c, vec3(0.0));
    return max(1.055 * pow(c, vec3(0.416666667)) - 0.055, vec3(0.0));
}

// Interleaved gradient noise; stands in for the original's blue-noise texture
// (one less file to ship).
float noise(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main() {
    vec3 c = texture(u_dye, v_uv).rgb;
    if (shading > 0.5) {
        vec3 lc = texture(u_dye, v_uv - vec2(texel.x, 0.0)).rgb;
        vec3 rc = texture(u_dye, v_uv + vec2(texel.x, 0.0)).rgb;
        vec3 tc = texture(u_dye, v_uv + vec2(0.0, texel.y)).rgb;
        vec3 bc = texture(u_dye, v_uv - vec2(0.0, texel.y)).rgb;
        vec3 n = normalize(vec3(length(rc) - length(lc), length(tc) - length(bc), length(texel)));
        c *= clamp(n.z + 0.7, 0.7, 1.0);
    }
    vec3 b = vec3(0.0);
    if (bloom > 0.5) b = texture(u_bloom, v_uv).rgb;
    if (sunrays > 0.5) {
        float s = texture(u_sunrays, v_uv).r;
        c *= s;
        b *= s;
    }
    if (bloom > 0.5) c += linear_to_gamma(b);
    // Dither against 8-bit banding. The original adds it to the bloom before
    // linear_to_gamma, whose slope near black turns +-1/255 into visible grain
    // of up to 13/255; added after, it stays at +-1/255 and can't be seen.
    c += (noise(gl_FragCoord.xy) * 2.0 - 1.0) / 255.0;
    // The original blends premultiplied (ONE, ONE_MINUS_SRC_ALPHA) with
    // alpha = brightest channel; un-premultiply so the result is the same.
    float a = clamp(max(c.r, max(c.g, c.b)), 0.0, 1.0);
    o_color = vec4(a > 0.0 ? clamp(c, 0.0, 1.0) / a : vec3(0.0), a);
}
