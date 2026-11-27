#version 450
// pmrem.glsl: three.js r147's PMREMGenerator, which prefilters scene.environment
// into the "cube uv" atlas MeshStandardMaterial reads. Each draw fills one
// viewport of a bottom_up RGBA16F canvas (WebGL's viewport, as a quad there):
//   mode 0  EquirectangularToCubeUV (lod 0, six faces)
//   mode 1  SphericalGaussianBlur (one half of a blur, into lodOut's faces)
//   mode 2  RGBE (Radiance .hdr) to linear floats, flipped as RGBELoader's flipY
layout(set = BARE_PARAMS_SET, binding = 0) uniform Params {
    vec4 rect;      // the viewport: x, y (from the bottom), width, height
    vec4 size;      // target width, height; face size (pixels); mode
    vec4 blur;      // samples, latitudinal, dTheta, mipInt
    vec4 pole;      // poleAxis
    vec4 cube;      // CUBEUV_TEXEL_WIDTH, CUBEUV_TEXEL_HEIGHT, CUBEUV_MAX_MIP
    vec4 weights[5];
};

#ifdef BARE_VERTEX
void main() {
    const vec2 c[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1));
    vec2 p = rect.xy + c[gl_VertexIndex] * rect.zw;
    gl_Position = vec4(p / size.xy * 2.0 - 1.0, 0.0, 1.0);
}
#else
layout(set = 2, binding = 0) uniform sampler2D envMap;
layout(location = 0) out vec4 o_color;

#define PI 3.141592653589793
#define RECIPROCAL_PI 0.3183098861837907
#define RECIPROCAL_PI2 0.15915494309189535
#define n 20

// _getCommonVertexShader: RH coordinate system; PMREM face-indexing convention
vec3 getDirection(vec2 uv, float face) {
    uv = 2.0 * uv - 1.0;
    vec3 direction = vec3(uv, 1.0);
    if (face == 0.0) {
        direction = direction.zyx;
    } else if (face == 1.0) {
        direction = direction.xzy;
        direction.xz *= -1.0;
    } else if (face == 2.0) {
        direction.x *= -1.0;
    } else if (face == 3.0) {
        direction = direction.zyx;
        direction.xz *= -1.0;
    } else if (face == 4.0) {
        direction = direction.xzy;
        direction.xy *= -1.0;
    } else if (face == 5.0) {
        direction.z *= -1.0;
    }
    return direction;
}

#define cubeUV_minMipLevel 4.0
#define cubeUV_minTileSize 16.0
float getFace(vec3 direction) {
    vec3 absDirection = abs(direction);
    float face = -1.0;
    if (absDirection.x > absDirection.z) {
        if (absDirection.x > absDirection.y) face = direction.x > 0.0 ? 0.0 : 3.0;
        else face = direction.y > 0.0 ? 1.0 : 4.0;
    } else {
        if (absDirection.z > absDirection.y) face = direction.z > 0.0 ? 2.0 : 5.0;
        else face = direction.y > 0.0 ? 1.0 : 4.0;
    }
    return face;
}
vec2 getUV(vec3 direction, float face) {
    vec2 uv;
    if (face == 0.0) uv = vec2(direction.z, direction.y) / abs(direction.x);
    else if (face == 1.0) uv = vec2(-direction.x, -direction.z) / abs(direction.y);
    else if (face == 2.0) uv = vec2(-direction.x, direction.y) / abs(direction.z);
    else if (face == 3.0) uv = vec2(-direction.z, direction.y) / abs(direction.x);
    else if (face == 4.0) uv = vec2(-direction.x, direction.z) / abs(direction.y);
    else uv = vec2(direction.x, direction.y) / abs(direction.z);
    return 0.5 * (uv + 1.0);
}
vec3 bilinearCubeUV(vec3 direction, float mipInt) {
    float face = getFace(direction);
    float filterInt = max(cubeUV_minMipLevel - mipInt, 0.0);
    mipInt = max(mipInt, cubeUV_minMipLevel);
    float faceSize = exp2(mipInt);
    vec2 uv = getUV(direction, face) * (faceSize - 2.0) + 1.0;
    if (face > 2.0) {
        uv.y += faceSize;
        face -= 3.0;
    }
    uv.x += face * faceSize;
    uv.x += filterInt * 3.0 * cubeUV_minTileSize;
    uv.y += 4.0 * (exp2(cube.z) - faceSize);
    uv.x *= cube.x;
    uv.y *= cube.y;
    return textureLod(envMap, uv, 0.0).rgb;
}

vec3 getSample(vec3 vOutputDirection, float theta, vec3 axis) {
    float cosTheta = cos(theta);
    vec3 sampleDirection = vOutputDirection * cosTheta
        + cross(axis, vOutputDirection) * sin(theta)
        + axis * dot(axis, vOutputDirection) * (1.0 - cosTheta);
    return bilinearCubeUV(sampleDirection, blur.w);
}

float weight(int i) { return weights[i >> 2][i & 3]; }

void main() {
    vec2 local = gl_FragCoord.xy - rect.xy;
    int mode = int(size.w + 0.5);
    if (mode == 2) {   // RGBE: three's RGBEByteToRGBFloat, rows flipped (flipY)
        ivec2 q = ivec2(local);
        vec4 rgbe = texelFetch(envMap, ivec2(q.x, int(rect.w) - 1 - q.y), 0) * 255.0;
        float scale = exp2(rgbe.a - 128.0) / 255.0;
        o_color = vec4(min(rgbe.rgb * scale, vec3(65504.0)), 1.0);
        return;
    }
    // the face plane: three columns, faces 0-2 below and 3-5 above, uv from -texel to 1 + texel
    float s = size.z;
    float col = min(floor(local.x / s), 2.0), row = local.y >= s ? 1.0 : 0.0;
    float face = col + 3.0 * row;
    vec2 uv = (local - vec2(col, row) * s - 1.0) / (s - 2.0);
    vec3 vOutputDirection = getDirection(uv, face);

    if (mode == 0) {   // EquirectangularToCubeUV
        vec3 outputDirection = normalize(vOutputDirection);
        float u = atan(outputDirection.z, outputDirection.x) * RECIPROCAL_PI2 + 0.5;
        float v = asin(clamp(outputDirection.y, -1.0, 1.0)) * RECIPROCAL_PI + 0.5;
        o_color = vec4(textureLod(envMap, vec2(u, v), 0.0).rgb, 1.0);
        return;
    }
    // SphericalGaussianBlur
    vec3 axis = blur.y > 0.5 ? pole.xyz : cross(pole.xyz, vOutputDirection);
    if (all(equal(axis, vec3(0.0)))) axis = vec3(vOutputDirection.z, 0.0, -vOutputDirection.x);
    axis = normalize(axis);
    int samples = int(blur.x + 0.5);
    o_color = vec4(0.0, 0.0, 0.0, 1.0);
    o_color.rgb += weight(0) * getSample(vOutputDirection, 0.0, axis);
    for (int i = 1; i < n; i++) {
        if (i >= samples) break;
        float theta = blur.z * float(i);
        o_color.rgb += weight(i) * getSample(vOutputDirection, -1.0 * theta, axis);
        o_color.rgb += weight(i) * getSample(vOutputDirection, theta, axis);
    }
}
#endif
