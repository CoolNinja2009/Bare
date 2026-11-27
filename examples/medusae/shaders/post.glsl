#version 450
// post.glsl: the page's EffectComposer passes after the scene, each over a
// whole target: BloomPass's convolution (25 taps, across then down, into
// 512 x 512), its copy added back at 0.8 (CopyShader, blendFunc SRC_ALPHA, ONE
// on an RGB target: 0.64 of the colour), the VignetteShader, and the browser
// scaling the 1.5x canvas down to the page (bilinear).
layout(set = BARE_PARAMS_SET, binding = 0) uniform Params {
    vec4 misc;       // mode (0 convolution, 1 bloom add, 2 vignette, 3 scale), target w, h
    vec4 inc;        // convolution: uImageIncrement
    vec4 color;      // vignette: (1 - darkness) * color, offset
    vec4 k[7];       // convolution: cKernel[25]
};

#ifdef BARE_VERTEX
void main() {
    const vec2 c[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(0, 1), vec2(1, 0), vec2(1, 1));
    gl_Position = vec4(c[gl_VertexIndex] * 2.0 - 1.0, 0.0, 1.0);
}
#else
layout(set = 2, binding = 0) uniform sampler2D tDiffuse;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 vUv = gl_FragCoord.xy / misc.yz;
    int mode = int(misc.x + 0.5);
    if (mode == 0) {
        vec2 imageCoord = vUv - 12.0 * inc.xy;
        vec4 sum = vec4(0.0);
        for (int i = 0; i < 25; i++) {
            sum += texture(tDiffuse, imageCoord) * k[i >> 2][i & 3];
            imageCoord += inc.xy;
        }
        o_color = vec4(sum.rgb, 1.0);
    } else if (mode == 1) {
        o_color = vec4(texture(tDiffuse, vUv).rgb * 0.64, 1.0);
    } else if (mode == 2) {
        vec4 texel = texelFetch(tDiffuse, ivec2(gl_FragCoord.xy), 0);
        vec2 uv = (vUv - vec2(0.5)) * vec2(color.w);
        o_color = vec4(mix(texel.rgb, color.rgb, dot(uv, uv)), 1.0);
    } else {
        o_color = vec4(texture(tDiffuse, vUv).rgb, 1.0);
    }
}
#endif
