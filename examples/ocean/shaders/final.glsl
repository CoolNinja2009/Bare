#version 450
// final.glsl: exposure, bloom, an ACES-like tone curve, then a grade (cooler,
// a little desaturated, blacks lifted, an S-curve and a teal split tone), a
// soft vignette, film grain and,
// optionally, a cinema letterbox; into the 8-bit picture the window shows.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
layout(set = 2, binding = 0) uniform sampler2D uHdr;
layout(set = 2, binding = 1) uniform sampler2D uBloom;
layout(set = 3, binding = 0) uniform Params {
    vec4 uPost;    // exposure, bloom, grain, frame
    vec4 uRes;     // size, 1 / size
    vec4 uGrade;   // letterbox aspect (0 = none), saturation, lift, coolness
    vec4 uTone;    // contrast (an S-curve on the display values), split tone (teal shadows, cyan-white highlights),
                   // lens shade (0 = an even vignette, 1 = the cinema lens: dark on the left and along the bottom)
};

vec3 aces(vec3 x) {   // Narkowicz's fit
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

float srgb(float c) { return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }

void main() {
    if (uGrade.x > 0.0 && abs(v_uv.y - 0.5) > 0.5 * uRes.x * uRes.w / uGrade.x) { o = vec4(0.0, 0.0, 0.0, 1.0); return; }
    vec3 c = texture(uHdr, v_uv).rgb + texture(uBloom, v_uv).rgb * uPost.y;
    c = aces(c * uPost.x);
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(l), c, uGrade.y);
    c *= vec3(1.0 - 0.05 * uGrade.w, 1.0, 1.0 + 0.05 * uGrade.w);
    c = uGrade.z * vec3(0.8, 0.95, 1.0) + c * (1.0 - uGrade.z);
    vec2 q = v_uv - 0.5;
    c *= 1.0 - 0.5 * dot(q, q) * (1.0 - uTone.z);
    c = vec3(srgb(c.r), srgb(c.g), srgb(c.b));
    // the film look: an S-curve for depth, shadows pushed teal and highlights cyan-white
    c = mix(c, c * c * (3.0 - 2.0 * c), uTone.x);
    float y = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c += uTone.y * (vec3(-0.03, 0.006, 0.02) * (1.0 - y) * (1.0 - y) + vec3(-0.02, 0.01, 0.012) * y * y);
    // The lens: its light falls off around a point up and to the right of the
    // middle, so the left side and the bottom sink into shadow (as with a wide
    // lens partly shaded by its hood), measured off the reference footage.
    vec2 s = (v_uv - vec2(0.6, 0.45)) * vec2(1.0, 0.9);
    c *= 1.0 - uTone.z * 0.58 * smoothstep(0.28, 0.8, length(s));
    vec2 p = gl_FragCoord.xy + fract(uPost.w * 0.61803) * 517.0;
    float n = fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453) + fract(sin(dot(p, vec2(39.3468, 11.135))) * 24634.6345) - 1.0;
    o = vec4(c + n * uPost.z / 255.0, 1.0);
}
