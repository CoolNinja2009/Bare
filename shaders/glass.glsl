#version 450
// glass.glsl: liquid glass (BARE_CMD_GLASS). One quad per glass command,
// covering its shapes plus their shadow. The fragment shader samples a copy of
// everything drawn so far (u_backdrop, with mipmaps for frost) and bends it
// through a curved rim.
//
// Model: each shape is a slab of glass whose edge is rounded over `depth`
// pixels with a circular profile. Where the surface tilts, light bends toward
// the middle (as through a convex lens), so near the rim you see backdrop from
// further in, and the flat middle stays clear. Red, green and blue bend by
// slightly different amounts (dispersion). A light from the top left catches
// the tilted rim (shine). Shapes melt together with a smooth minimum of their
// distance fields (merge). Zoom magnifies the backdrop about the centre of the
// nearest shape, like a loupe; amount fades the glass (not its shadow) so it
// can condense out of the backdrop and dissolve back into it.
//
// Units: physical pixels, y down. SDL3 GPU SPIR-V bindings: vertex uniforms
// set 1, fragment samplers set 2, fragment uniforms set 3.

#ifdef BARE_VERTEX

layout(set = 1, binding = 0) uniform Quad {
    vec4 u_viewport;   // width, height
    vec4 u_bounds;     // x, y, w, h
};

layout(location = 0) out vec2 v_pos;

void main() {
    vec2 corner = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    vec2 p = u_bounds.xy + corner * u_bounds.zw;
    v_pos = p;
    gl_Position = vec4(p.x / u_viewport.x * 2.0 - 1.0, 1.0 - p.y / u_viewport.y * 2.0, 0.0, 1.0);
}

#endif

#ifdef BARE_FRAGMENT

layout(location = 0) in vec2 v_pos;
layout(location = 0) out vec4 o_color;   // premultiplied alpha

layout(set = 2, binding = 0) uniform sampler2D u_backdrop;

layout(set = 3, binding = 0) uniform Glass {
    vec4 u_shapes[8];      // x, y, w, h
    vec4 u_radii[2];       // radius of shape i = u_radii[i / 4][i % 4]
    vec4 u_p0;             // count, merge, depth, refraction
    vec4 u_p1;             // blur, dispersion, shine, opacity
    vec4 u_tint;           // rgb, strength
    vec4 u_shadow;         // offset x, offset y, blur, spread
    vec4 u_shadow_color;   // straight alpha
    vec4 u_target;         // backdrop width, height
    vec4 u_p2;             // amount, zoom, light angle (radians clockwise), 0
};

float sd_round_box(vec2 p, vec2 h, float r) {
    vec2 q = abs(p) - h + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Polynomial smooth minimum: melts a and b together over k pixels.
float smin(float a, float b, float k) {
    if (k <= 0.0) return min(a, b);
    float h = max(k - abs(a - b), 0.0) / k;
    return min(a, b) - h * h * k * 0.25;
}

// Signed distance to the merged glass surface, shapes grown by `grow`.
float field(vec2 p, float grow) {
    int count = int(u_p0.x + 0.5);
    float d = 1e6;
    for (int i = 0; i < 8; i++) {
        if (i >= count) break;
        vec4 s = u_shapes[i];
        vec2 h = s.zw * 0.5;
        float r = min(u_radii[i / 4][i % 4], min(h.x, h.y));
        d = smin(d, sd_round_box(p - (s.xy + h), max(h + grow, vec2(0.0)), max(r + grow, 0.0)), u_p0.y);
    }
    return d;
}

// Centre of the shape nearest to p (the loupe magnifies about it).
vec2 nearest_centre(vec2 p) {
    int count = int(u_p0.x + 0.5);
    float best = 1e6;
    vec2 c = p;
    for (int i = 0; i < 8; i++) {
        if (i >= count) break;
        vec4 s = u_shapes[i];
        vec2 h = s.zw * 0.5;
        float r = min(u_radii[i / 4][i % 4], min(h.x, h.y));
        float d = sd_round_box(p - (s.xy + h), h, r);
        if (d < best) { best = d; c = s.xy + h; }
    }
    return c;
}

float erf_approx(float x) {
    float s = sign(x), a = abs(x);
    float t = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
    t *= t;
    return s - s / (t * t);
}

// Frosted backdrop: a mip level for the blur radius, smoothed by four taps.
vec3 backdrop(vec2 p, float blur) {
    vec2 uv = p / u_target.xy;
    if (blur < 0.5) return texture(u_backdrop, uv).rgb;
    float lod = log2(blur) - 0.5;
    vec2 o = vec2(blur * 0.35) / u_target.xy;
    return 0.25 * (textureLod(u_backdrop, uv + vec2(o.x, o.y), lod).rgb +
                   textureLod(u_backdrop, uv + vec2(-o.y, o.x), lod).rgb +
                   textureLod(u_backdrop, uv + vec2(-o.x, -o.y), lod).rgb +
                   textureLod(u_backdrop, uv + vec2(o.y, -o.x), lod).rgb);
}

void main() {
    vec2 p = v_pos;
    float merge = u_p0.y, depth = max(u_p0.z, 1.0), refraction = u_p0.w;
    float blur = u_p1.x, dispersion = u_p1.y, shine = u_p1.z, opacity = u_p1.w;
    float amount = u_p2.x, zoom = max(u_p2.y, 0.01), angle = u_p2.z;

    // Shadow from the merged shape, not drawn under the glass itself.
    vec4 shadow = vec4(0.0);
    if (u_shadow_color.a > 0.0) {
        float sigma = max(u_shadow.z * 0.5, 0.5);
        float ds = field(p - u_shadow.xy, u_shadow.w);
        float a = u_shadow_color.a * (0.5 - 0.5 * erf_approx(ds / (sigma * 1.41421356)));
        shadow = vec4(u_shadow_color.rgb * a, a);
    }

    float d = field(p, 0.0);
    float cover = clamp(0.5 - d, 0.0, 1.0);
    if (cover <= 0.0) {
        o_color = shadow * opacity;
        return;
    }

    // Surface normal (in the screen plane) from the distance field's gradient.
    const float e = 0.75;
    vec2 g = vec2(field(p + vec2(e, 0.0), 0.0) - field(p - vec2(e, 0.0), 0.0),
                  field(p + vec2(0.0, e), 0.0) - field(p - vec2(0.0, e), 0.0));
    vec2 n = dot(g, g) > 1e-8 ? normalize(g) : vec2(0.0);

    // Rim profile: t = 0 at the edge, 1 where the rim meets the flat top;
    // x = 1 - t. The rim is a quarter circle, and the backdrop shifts by how
    // far the surface has dropped below the top (1 - sqrt(1 - x^2)): nothing
    // on the flat middle, easing in across the rim, strongest at the very edge.
    float t = clamp(-d / depth, 0.0, 1.0);
    float x = 1.0 - t;
    float drop = 1.0 - sqrt(max(1.0 - x * x, 0.0));
    float bend = refraction * depth * 0.55 * drop;

    // Magnify about the shape's centre, then pull the backdrop inward along
    // the normal; each colour by its own amount.
    vec2 c = nearest_centre(p);
    vec2 q = c + (p - c) / zoom;
    float spread = 0.06 * dispersion * x;
    vec3 col;
    col.r = backdrop(q - n * bend * (1.0 + spread), blur).r;
    col.g = backdrop(q - n * bend, blur).g;
    col.b = backdrop(q - n * bend * (1.0 - spread), blur).b;

    // Glass lifts colour slightly and brightens a touch.
    float luma = dot(col, vec3(0.2126, 0.7152, 0.0722));
    col = mix(vec3(luma), col, 1.18) * 1.03 + 0.015;
    col = mix(col, u_tint.rgb, u_tint.a);

    // Light from the top left catches the tilted rim; a weaker bounce on the far side.
    float ca = cos(angle), sa = sin(angle);
    vec2 light = normalize(vec2(-0.55, -0.85));
    light = vec2(light.x * ca - light.y * sa, light.x * sa + light.y * ca);   // clockwise on screen (y down)
    float rim = pow(x, 3.0);
    float facing = max(dot(n, light), 0.0), away = max(dot(n, -light), 0.0);
    float spec = rim * (0.55 * pow(facing, 1.5) + 0.22 * pow(away, 2.0));
    // A fine bright line right at the edge, like a polished bevel.
    spec += smoothstep(2.0, 0.0, -d) * (0.18 + 0.35 * facing);
    col += shine * spec;

    // The rim darkens slightly toward the edge (light travels through more
    // glass there), which gives the slab thickness without a second outline.
    col *= 1.0 - 0.09 * min(refraction, 1.5) * x * x * x;

    vec4 glass = vec4(clamp(col, 0.0, 1.0) * cover, cover) * amount;
    o_color = (glass + shadow * (1.0 - glass.a)) * opacity;
}

#endif
