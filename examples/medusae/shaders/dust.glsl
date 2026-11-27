#version 450
// dust.glsl: the page's drifting dust (dust-vert, dust-frag): one instance a
// point, drawn as the square WebGL rasterises for gl_PointSize, its picture
// (a 64 x 64 dot, mipmapped as WebGL's generateMipmap does) sampled
// trilinearly from an atlas of its levels; added (blendFunc SRC_ALPHA, ONE).
layout(set = BARE_PARAMS_SET, binding = 0) uniform Params {
    mat4 projection;
    mat4 model_view;
    vec4 misc;       // time, area, size, scale
    vec4 size;       // target w, h; opacity
};

#ifdef BARE_VERTEX
layout(location = 0) in vec4 position;
layout(location = 0) out float centerDist;
layout(location = 1) out vec2 pointCoord;
layout(location = 2) out float pointSize;

void main() {
  float time = misc.x, area = misc.y;
  float offsetY = mod(position.y - 1.0 * time, area) - area * 0.5;
  vec3 offsetPosition = vec3(
    position.x + sin(cos(offsetY * 0.1) + sin(offsetY * 0.1 + position.x * 0.1) * 2.0),
    offsetY,
    position.z + sin(cos(offsetY * 0.1) + sin(offsetY * 0.1 + position.z * 0.1) * 2.0));

  centerDist = length(offsetPosition);

  vec4 mvPosition = model_view * vec4(offsetPosition, 1.0);
  float ps = misc.z * (misc.w / length(mvPosition.xyz));
  vec4 c = projection * mvPosition;
  const vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(0, 1), vec2(1, 0), vec2(1, 1));
  vec2 k = corners[gl_VertexIndex];
  pointCoord = vec2(k.x, 1.0 - k.y);   // gl_PointCoord: y down
  pointSize = ps;
  vec2 off = (k * 2.0 - 1.0) * ps / size.xy;   // half the square, in NDC
  bool behind = c.w <= 0.0 || c.z < -c.w || c.z > c.w;
  gl_Position = behind ? vec4(2.0, 2.0, 2.0, 1.0) : vec4(c.xy + off * c.w, (c.z + c.w) * 0.5, c.w);
}
#else
layout(set = 2, binding = 0) uniform sampler2D u_atlas;   // levels 64, 32, ... 1 side by side
layout(location = 0) in float centerDist;
layout(location = 1) in vec2 pointCoord;
layout(location = 2) in float pointSize;
layout(location = 0) out vec4 o_color;

vec4 level(int l, vec2 uv) {
    int s = 64 >> l, x0 = 128 - (128 >> l);
    vec2 p = uv * float(s) - 0.5, f = fract(p);
    ivec2 i0 = ivec2(floor(p));
    vec4 t[4];
    for (int k = 0; k < 4; k++) {
        ivec2 q = clamp(i0 + ivec2(k & 1, k >> 1), ivec2(0), ivec2(s - 1));
        t[k] = texelFetch(u_atlas, ivec2(x0, 0) + q, 0);
    }
    return mix(mix(t[0], t[1], f.x), mix(t[2], t[3], f.x), f.y);
}

void main() {
  float area = misc.y, opacity = size.z;
  vec4 diffuseColor = vec4(1.0, 1.0, 1.0, opacity);
  float radius = area * 0.5;
  float illumination = max(0.0, (radius - centerDist) / radius);
  vec2 uv = vec2(pointCoord.x, 1.0 - pointCoord.y);
  float lod = clamp(log2(64.0 / pointSize), 0.0, 6.0);
  int l0 = int(floor(lod)), l1 = min(l0 + 1, 6);
  diffuseColor *= mix(level(l0, uv), level(l1, uv), lod - float(l0));
  float a = clamp(illumination * illumination * diffuseColor.a, 0.0, 1.0);
  o_color = vec4(clamp(diffuseColor.rgb, 0.0, 1.0) * a, a);
}
#endif
