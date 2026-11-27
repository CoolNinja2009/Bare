#version 450
// three.glsl: three.js r147's WebGLRenderer for the 3D Screen+ samples (Living
// Room, Music TV), one program for its materials: MeshBasicMaterial,
// MeshStandardMaterial / MeshPhysicalMaterial (its physical lighting chunks,
// as written: GGX, multiscattering, legacy light falloff), ShadowMaterial, and
// the depth material that fills a spot light's shadow map. ACES Filmic tone
// mapping and the sRGB output encoding happen here, as three does when it
// draws to the page. Positions are WebGL's clip space (z -1..1 here mapped to
// Bare's 0..1); draw into a bottom_up canvas so derivatives and facing match.
layout(set = BARE_PARAMS_SET, binding = 0) uniform Params {
    mat4 projection;
    mat4 model_view;
    mat4 model;
    mat4 normal_matrix;   // mat3 in the upper left
    mat4 spot_shadow;     // the spot light's shadow.matrix (world to 0..1 map coords)
    mat4 uv_transform;    // the map's uvTransform (mat3 in the upper left)
    mat4 view;            // the camera's view matrix (environment lookups)
    vec4 color;           // diffuse rgb, opacity
    vec4 emissive;        // rgb; w: envMapIntensity
    vec4 mat;             // roughness, metalness, normalScale x, y
    vec4 mat_b;            // ior (0 = none), specularIntensity, aoMapIntensity, kind (0 basic, 1 standard, 2 shadow, 3 depth)
    vec4 spec_color;      // specularColor rgb; w: physical (IOR, SPECULAR)
    vec4 flags;           // map, roughness/metalness map, normal map, emissive map
    vec4 flags2;          // ao map, side (0 front, 1 back, 2 double), receiveShadow, orthographic
    vec4 ambient;         // ambientLightColor (intensity * PI); w: envmap on
    vec4 point_pos[4];    // view space xyz, distance
    vec4 point_col[4];    // color (intensity * PI), decay
    vec4 spot_pos;        // view space xyz, distance
    vec4 spot_dir;        // view space direction to the light, decay
    vec4 spot_col;        // color (intensity * PI), coneCos
    vec4 spot_misc;       // penumbraCos, shadow map size (0 = no shadow), bias, spot on
    vec4 misc;            // point lights, toneMappingExposure, tone mapping on, facing flip
    vec4 env;             // PMREM: cube uv texel width, height, max mip; w: transparent (else OPAQUE: alpha 1)
};

#ifdef BARE_VERTEX
layout(location = 0) in vec4 pos_u;
layout(location = 1) in vec4 nrm_v;
layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vViewPosition;
layout(location = 2) out vec2 vUv;
layout(location = 3) out vec2 vUv2;
layout(location = 4) out vec4 vSpotLightCoord;

void main() {
    vec2 uv = vec2(pos_u.w, nrm_v.w);
    vUv = (mat3(uv_transform) * vec3(uv, 1.0)).xy;
    vUv2 = uv;
    vec3 transformedNormal = mat3(normal_matrix) * nrm_v.xyz;
    vNormal = normalize(transformedNormal);
    vec4 mvPosition = model_view * vec4(pos_u.xyz, 1.0);
    vec4 p = projection * mvPosition;
    vViewPosition = -mvPosition.xyz;
    vec4 worldPosition = model * vec4(pos_u.xyz, 1.0);
    vSpotLightCoord = spot_shadow * worldPosition;
    gl_Position = vec4(p.xy, (p.z + p.w) * 0.5, p.w);
}
#else
layout(set = 2, binding = 0) uniform sampler2D map;
layout(set = 2, binding = 1) uniform sampler2D roughnessMap;   // metalness in .b, roughness in .g
layout(set = 2, binding = 2) uniform sampler2D normalMap;
layout(set = 2, binding = 3) uniform sampler2D emissiveMap;
layout(set = 2, binding = 4) uniform sampler2D aoMap;
layout(set = 2, binding = 5) uniform sampler2D spotShadowMap;  // depth in .r (three packs it in RGBA8)
layout(set = 2, binding = 6) uniform sampler2D envMap;         // PMREM cube uv
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vViewPosition;
layout(location = 2) in vec2 vUv;
layout(location = 3) in vec2 vUv2;
layout(location = 4) in vec4 vSpotLightCoord;
layout(location = 0) out vec4 o_color;

#define PI 3.141592653589793
#define RECIPROCAL_PI 0.3183098861837907
#define EPSILON 1e-6
#define saturate(a) clamp(a, 0.0, 1.0)
float pow2(const in float x) { return x * x; }
vec3 inverseTransformDirection(in vec3 dir, in mat4 matrix) { return normalize((vec4(dir, 0.0) * matrix).xyz); }

struct IncidentLight { vec3 color; vec3 direction; bool visible; };
struct ReflectedLight { vec3 directDiffuse; vec3 directSpecular; vec3 indirectDiffuse; vec3 indirectSpecular; };
struct GeometricContext { vec3 position; vec3 normal; vec3 viewDir; };
struct PhysicalMaterial { vec3 diffuseColor; float roughness; vec3 specularColor; float specularF90; };

// bsdfs
vec3 BRDF_Lambert(const in vec3 diffuseColor) { return RECIPROCAL_PI * diffuseColor; }
vec3 F_Schlick(const in vec3 f0, const in float f90, const in float dotVH) {
    float fresnel = exp2((-5.55473 * dotVH - 6.98316) * dotVH);
    return f0 * (1.0 - fresnel) + (f90 * fresnel);
}
float V_GGX_SmithCorrelated(const in float alpha, const in float dotNL, const in float dotNV) {
    float a2 = pow2(alpha);
    float gv = dotNL * sqrt(a2 + (1.0 - a2) * pow2(dotNV));
    float gl = dotNV * sqrt(a2 + (1.0 - a2) * pow2(dotNL));
    return 0.5 / max(gv + gl, EPSILON);
}
float D_GGX(const in float alpha, const in float dotNH) {
    float a2 = pow2(alpha);
    float denom = pow2(dotNH) * (a2 - 1.0) + 1.0;
    return RECIPROCAL_PI * a2 / pow2(denom);
}
vec3 BRDF_GGX(const in vec3 lightDir, const in vec3 viewDir, const in vec3 normal, const in vec3 f0, const in float f90, const in float roughness) {
    float alpha = pow2(roughness);
    vec3 halfDir = normalize(lightDir + viewDir);
    float dotNL = saturate(dot(normal, lightDir));
    float dotNV = saturate(dot(normal, viewDir));
    float dotNH = saturate(dot(normal, halfDir));
    float dotVH = saturate(dot(viewDir, halfDir));
    vec3 F = F_Schlick(f0, f90, dotVH);
    float V = V_GGX_SmithCorrelated(alpha, dotNL, dotNV);
    float D = D_GGX(alpha, dotNH);
    return F * (V * D);
}

// lights_pars_begin (legacy lights: physicallyCorrectLights is off)
float getDistanceAttenuation(const in float lightDistance, const in float cutoffDistance, const in float decayExponent) {
    if (cutoffDistance > 0.0 && decayExponent > 0.0) return pow(saturate(-lightDistance / cutoffDistance + 1.0), decayExponent);
    return 1.0;
}
float getSpotAttenuation(const in float coneCosine, const in float penumbraCosine, const in float angleCosine) {
    if (penumbraCosine == coneCosine) return angleCosine > coneCosine ? 1.0 : 0.0;   // smoothstep(e, e, x) on ANGLE
    return smoothstep(coneCosine, penumbraCosine, angleCosine);
}

// lights_physical_pars_fragment
vec2 DFGApprox(const in vec3 normal, const in vec3 viewDir, const in float roughness) {
    float dotNV = saturate(dot(normal, viewDir));
    const vec4 c0 = vec4(-1, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * dotNV)) * r.x + r.y;
    vec2 fab = vec2(-1.04, 1.04) * a004 + r.zw;
    return fab;
}
void computeMultiscattering(const in vec3 normal, const in vec3 viewDir, const in vec3 specularColor, const in float specularF90, const in float roughness, inout vec3 singleScatter, inout vec3 multiScatter) {
    vec2 fab = DFGApprox(normal, viewDir, roughness);
    vec3 Fr = specularColor;
    vec3 FssEss = Fr * fab.x + specularF90 * fab.y;
    float Ess = fab.x + fab.y;
    float Ems = 1.0 - Ess;
    vec3 Favg = Fr + (1.0 - Fr) * 0.047619;
    vec3 Fms = FssEss * Favg / (1.0 - Ems * Favg);
    singleScatter += FssEss;
    multiScatter += Fms * Ems;
}
void RE_Direct(const in IncidentLight directLight, const in GeometricContext geometry, const in PhysicalMaterial material, inout ReflectedLight reflectedLight) {
    float dotNL = saturate(dot(geometry.normal, directLight.direction));
    vec3 irradiance = dotNL * directLight.color;
    reflectedLight.directSpecular += irradiance * BRDF_GGX(directLight.direction, geometry.viewDir, geometry.normal, material.specularColor, material.specularF90, material.roughness);
    reflectedLight.directDiffuse += irradiance * BRDF_Lambert(material.diffuseColor);
}
void RE_IndirectSpecular(const in vec3 radiance, const in vec3 irradiance, const in GeometricContext geometry, const in PhysicalMaterial material, inout ReflectedLight reflectedLight) {
    vec3 singleScattering = vec3(0.0);
    vec3 multiScattering = vec3(0.0);
    vec3 cosineWeightedIrradiance = irradiance * RECIPROCAL_PI;
    computeMultiscattering(geometry.normal, geometry.viewDir, material.specularColor, material.specularF90, material.roughness, singleScattering, multiScattering);
    vec3 totalScattering = singleScattering + multiScattering;
    vec3 diffuse = material.diffuseColor * (1.0 - max(max(totalScattering.r, totalScattering.g), totalScattering.b));
    reflectedLight.indirectSpecular += radiance * singleScattering;
    reflectedLight.indirectSpecular += multiScattering * cosineWeightedIrradiance;
    reflectedLight.indirectDiffuse += diffuse * cosineWeightedIrradiance;
}
float computeSpecularOcclusion(const in float dotNV, const in float ambientOcclusion, const in float roughness) {
    return saturate(pow(dotNV + ambientOcclusion, exp2(-16.0 * roughness - 1.0)) - 1.0 + ambientOcclusion);
}

// normalmap_pars_fragment
vec3 perturbNormal2Arb(vec3 eye_pos, vec3 surf_norm, vec3 mapN, float faceDirection) {
    vec3 q0 = dFdx(eye_pos.xyz);
    vec3 q1 = dFdy(eye_pos.xyz);
    vec2 st0 = dFdx(vUv.st);
    vec2 st1 = dFdy(vUv.st);
    vec3 N = surf_norm;
    vec3 q1perp = cross(q1, N);
    vec3 q0perp = cross(N, q0);
    vec3 T = q1perp * st0.x + q0perp * st1.x;
    vec3 B = q1perp * st0.y + q0perp * st1.y;
    float det = max(dot(T, T), dot(B, B));
    float scale = (det == 0.0) ? 0.0 : faceDirection * inversesqrt(det);
    return normalize(T * (mapN.x * scale) + B * (mapN.y * scale) + N * mapN.z);
}

// shadowmap_pars_fragment, PCFSoftShadowMap; the map is nearest-filtered
float texture2DCompare(vec2 uv, float compare) {
    float size = spot_misc.y;
    ivec2 t = clamp(ivec2(floor(uv * size)), ivec2(0), ivec2(int(size) - 1));
    return step(compare, texelFetch(spotShadowMap, t, 0).r);
}
float getShadow(float shadowMapSize, float shadowBias, vec4 shadowCoord) {
    float shadow = 1.0;
    shadowCoord.xyz /= shadowCoord.w;
    shadowCoord.z += shadowBias;
    bool inFrustum = shadowCoord.x >= 0.0 && shadowCoord.x <= 1.0 && shadowCoord.y >= 0.0 && shadowCoord.y <= 1.0;
    if (inFrustum && shadowCoord.z <= 1.0) {
        vec2 texelSize = vec2(1.0) / shadowMapSize;
        float dx = texelSize.x;
        float dy = texelSize.y;
        vec2 uv = shadowCoord.xy;
        vec2 f = fract(uv * shadowMapSize + 0.5);
        uv -= f * texelSize;
        float z = shadowCoord.z;
        shadow = (
            texture2DCompare(uv, z) +
            texture2DCompare(uv + vec2(dx, 0.0), z) +
            texture2DCompare(uv + vec2(0.0, dy), z) +
            texture2DCompare(uv + texelSize, z) +
            mix(texture2DCompare(uv + vec2(-dx, 0.0), z), texture2DCompare(uv + vec2(2.0 * dx, 0.0), z), f.x) +
            mix(texture2DCompare(uv + vec2(-dx, dy), z), texture2DCompare(uv + vec2(2.0 * dx, dy), z), f.x) +
            mix(texture2DCompare(uv + vec2(0.0, -dy), z), texture2DCompare(uv + vec2(0.0, 2.0 * dy), z), f.y) +
            mix(texture2DCompare(uv + vec2(dx, -dy), z), texture2DCompare(uv + vec2(dx, 2.0 * dy), z), f.y) +
            mix(mix(texture2DCompare(uv + vec2(-dx, -dy), z), texture2DCompare(uv + vec2(2.0 * dx, -dy), z), f.x),
                mix(texture2DCompare(uv + vec2(-dx, 2.0 * dy), z), texture2DCompare(uv + vec2(2.0 * dx, 2.0 * dy), z), f.x),
                f.y)
        ) * (1.0 / 9.0);
    }
    return shadow;
}

// cube_uv_reflection_fragment (PMREM), for scene.environment
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
    uv.y += 4.0 * (exp2(env.z) - faceSize);
    uv.x *= env.x;
    uv.y *= env.y;
    return texture(envMap, uv).rgb;   // the PMREM target is LINEAR, no mipmaps
}
#define cubeUV_r0 1.0
#define cubeUV_v0 0.339
#define cubeUV_m0 -2.0
#define cubeUV_r1 0.8
#define cubeUV_v1 0.276
#define cubeUV_m1 -1.0
#define cubeUV_r4 0.4
#define cubeUV_v4 0.046
#define cubeUV_m4 2.0
#define cubeUV_r5 0.305
#define cubeUV_v5 0.016
#define cubeUV_m5 3.0
#define cubeUV_r6 0.21
#define cubeUV_v6 0.0038
#define cubeUV_m6 4.0
float roughnessToMip(float roughness) {
    float mip = 0.0;
    if (roughness >= cubeUV_r1) mip = (cubeUV_r0 - roughness) * (cubeUV_m1 - cubeUV_m0) / (cubeUV_r0 - cubeUV_r1) + cubeUV_m0;
    else if (roughness >= cubeUV_r4) mip = (cubeUV_r1 - roughness) * (cubeUV_m4 - cubeUV_m1) / (cubeUV_r1 - cubeUV_r4) + cubeUV_m1;
    else if (roughness >= cubeUV_r5) mip = (cubeUV_r4 - roughness) * (cubeUV_m5 - cubeUV_m4) / (cubeUV_r4 - cubeUV_r5) + cubeUV_m4;
    else if (roughness >= cubeUV_r6) mip = (cubeUV_r5 - roughness) * (cubeUV_m6 - cubeUV_m5) / (cubeUV_r5 - cubeUV_r6) + cubeUV_m5;
    else mip = -2.0 * log2(1.16 * roughness);
    return mip;
}
vec4 textureCubeUV(vec3 sampleDir, float roughness) {
    float mip = clamp(roughnessToMip(roughness), cubeUV_m0, env.z);
    float mipF = fract(mip);
    float mipInt = floor(mip);
    vec3 color0 = bilinearCubeUV(sampleDir, mipInt);
    if (mipF == 0.0) return vec4(color0, 1.0);
    vec3 color1 = bilinearCubeUV(sampleDir, mipInt + 1.0);
    return vec4(mix(color0, color1, mipF), 1.0);
}
// envmap_physical_pars_fragment
vec3 getIBLIrradiance(const in vec3 normal) {
    vec3 worldNormal = inverseTransformDirection(normal, view);
    vec4 envMapColor = textureCubeUV(worldNormal, 1.0);
    return PI * envMapColor.rgb * emissive.w;
}
vec3 getIBLRadiance(const in vec3 viewDir, const in vec3 normal, const in float roughness) {
    vec3 reflectVec = reflect(-viewDir, normal);
    reflectVec = normalize(mix(reflectVec, normal, roughness * roughness));
    reflectVec = inverseTransformDirection(reflectVec, view);
    vec4 envMapColor = textureCubeUV(reflectVec, roughness);
    return envMapColor.rgb * emissive.w;
}

// tonemapping_pars_fragment, encodings_pars_fragment
vec3 RRTAndODTFit(vec3 v) {
    vec3 a = v * (v + 0.0245786) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}
vec3 ACESFilmicToneMapping(vec3 color) {
    const mat3 ACESInputMat = mat3(vec3(0.59719, 0.07600, 0.02840), vec3(0.35458, 0.90834, 0.13383), vec3(0.04823, 0.01566, 0.83777));
    const mat3 ACESOutputMat = mat3(vec3(1.60475, -0.10208, -0.00327), vec3(-0.53108, 1.10813, -0.07276), vec3(-0.07367, -0.00605, 1.07602));
    color *= misc.y / 0.6;
    color = ACESInputMat * color;
    color = RRTAndODTFit(color);
    color = ACESOutputMat * color;
    return saturate(color);
}
vec4 LinearTosRGB(in vec4 value) {
    return vec4(mix(pow(value.rgb, vec3(0.41666)) * 1.055 - vec3(0.055), value.rgb * 12.92, vec3(lessThanEqual(value.rgb, vec3(0.0031308)))), value.a);
}
vec4 finish(vec4 c) {
    if (misc.z > 0.5) c.rgb = ACESFilmicToneMapping(c.rgb);
    return LinearTosRGB(c);
}

void main() {
    int kind = int(mat_b.w + 0.5);
    int side = int(flags2.y + 0.5);
    bool front = misc.w > 0.5 ? !gl_FrontFacing : gl_FrontFacing;
    if ((side == 0 && !front) || (side == 1 && front)) discard;   // three culls these
    if (kind == 3) {   // MeshDepthMaterial (RGBADepthPacking), unpacked
        o_color = vec4(gl_FragCoord.z, 0.0, 0.0, 1.0);
        return;
    }
    float receive = flags2.z;
    if (kind == 2) {   // ShadowMaterial
        float shadow = 1.0;
        if (spot_misc.y > 0.0 && receive > 0.5) shadow *= getShadow(spot_misc.y, spot_misc.z, vSpotLightCoord);
        o_color = finish(vec4(color.rgb, color.a * (1.0 - shadow)));
        return;
    }
    vec4 diffuseColor = vec4(color.rgb, color.a);
    if (flags.x > 0.5) diffuseColor *= texture(map, vUv);
    float outAlpha = env.w > 0.5 ? diffuseColor.a : 1.0;   // output_fragment: OPAQUE
    if (kind == 0) {   // MeshBasicMaterial
        vec3 indirect = vec3(1.0);
        if (flags2.x > 0.5) indirect *= (texture(aoMap, vUv2).r - 1.0) * mat_b.z + 1.0;
        o_color = finish(vec4(indirect * diffuseColor.rgb, outAlpha));
        return;
    }
    float roughnessFactor = mat.x, metalnessFactor = mat.y;
    if (flags.y > 0.5) {
        vec4 t = texture(roughnessMap, vUv);
        roughnessFactor *= t.g;
        metalnessFactor *= t.b;
    }
    float faceDirection = front ? 1.0 : -1.0;
    vec3 normal = normalize(vNormal);
    if (side == 2) normal = normal * faceDirection;
    vec3 geometryNormal = normal;
    if (flags.z > 0.5) {
        vec3 mapN = texture(normalMap, vUv).xyz * 2.0 - 1.0;
        mapN.xy *= mat.zw;
        normal = perturbNormal2Arb(-vViewPosition, normal, mapN, faceDirection);
    }
    vec3 totalEmissiveRadiance = emissive.rgb;
    if (flags.w > 0.5) totalEmissiveRadiance *= texture(emissiveMap, vUv).rgb;

    PhysicalMaterial material;
    material.diffuseColor = diffuseColor.rgb * (1.0 - metalnessFactor);
    vec3 dxy = max(abs(dFdx(geometryNormal)), abs(dFdy(geometryNormal)));
    float geometryRoughness = max(max(dxy.x, dxy.y), dxy.z);
    material.roughness = max(roughnessFactor, 0.0525);
    material.roughness += geometryRoughness;
    material.roughness = min(material.roughness, 1.0);
    if (spec_color.w > 0.5) {   // MeshPhysicalMaterial: IOR, SPECULAR
        float ior = mat_b.x;
        float specularIntensityFactor = mat_b.y;
        vec3 specularColorFactor = spec_color.rgb;
        material.specularF90 = mix(specularIntensityFactor, 1.0, metalnessFactor);
        material.specularColor = mix(min(pow2((ior - 1.0) / (ior + 1.0)) * specularColorFactor, vec3(1.0)) * specularIntensityFactor, diffuseColor.rgb, metalnessFactor);
    } else {
        material.specularColor = mix(vec3(0.04), diffuseColor.rgb, metalnessFactor);
        material.specularF90 = 1.0;
    }

    GeometricContext geometry;
    geometry.position = -vViewPosition;
    geometry.normal = normal;
    geometry.viewDir = flags2.w > 0.5 ? vec3(0, 0, 1) : normalize(vViewPosition);
    ReflectedLight reflectedLight = ReflectedLight(vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0));
    IncidentLight directLight;
    int points = int(misc.x + 0.5);
    for (int i = 0; i < 4; i++) {
        if (i >= points) break;
        vec3 lVector = point_pos[i].xyz - geometry.position;
        directLight.direction = normalize(lVector);
        float lightDistance = length(lVector);
        directLight.color = point_col[i].rgb * getDistanceAttenuation(lightDistance, point_pos[i].w, point_col[i].w);
        directLight.visible = directLight.color != vec3(0.0);
        RE_Direct(directLight, geometry, material, reflectedLight);
    }
    if (spot_misc.w > 0.5) {
        vec3 lVector = spot_pos.xyz - geometry.position;
        directLight.direction = normalize(lVector);
        float angleCos = dot(directLight.direction, spot_dir.xyz);
        float spotAttenuation = getSpotAttenuation(spot_col.w, spot_misc.x, angleCos);
        if (spotAttenuation > 0.0) {
            float lightDistance = length(lVector);
            directLight.color = spot_col.rgb * spotAttenuation;
            directLight.color *= getDistanceAttenuation(lightDistance, spot_pos.w, spot_dir.w);
            directLight.visible = directLight.color != vec3(0.0);
        } else {
            directLight.color = vec3(0.0);
            directLight.visible = false;
        }
        if (spot_misc.y > 0.0 && directLight.visible && receive > 0.5)
            directLight.color *= getShadow(spot_misc.y, spot_misc.z, vSpotLightCoord);
        RE_Direct(directLight, geometry, material, reflectedLight);
    }
    vec3 iblIrradiance = vec3(0.0);
    vec3 irradiance = ambient.rgb;
    vec3 radiance = vec3(0.0);
    if (ambient.w > 0.5) {
        iblIrradiance += getIBLIrradiance(geometry.normal);
        radiance += getIBLRadiance(geometry.viewDir, geometry.normal, material.roughness);
    }
    reflectedLight.indirectDiffuse += irradiance * BRDF_Lambert(material.diffuseColor);
    RE_IndirectSpecular(radiance, iblIrradiance, geometry, material, reflectedLight);
    if (flags2.x > 0.5) {   // aomap_fragment
        float ambientOcclusion = (texture(aoMap, vUv2).r - 1.0) * mat_b.z + 1.0;
        reflectedLight.indirectDiffuse *= ambientOcclusion;
        if (ambient.w > 0.5) {
            float dotNV = saturate(dot(geometry.normal, geometry.viewDir));
            reflectedLight.indirectSpecular *= computeSpecularOcclusion(dotNV, ambientOcclusion, material.roughness);
        }
    }
    vec3 totalDiffuse = reflectedLight.directDiffuse + reflectedLight.indirectDiffuse;
    vec3 totalSpecular = reflectedLight.directSpecular + reflectedLight.indirectSpecular;
    vec3 outgoingLight = totalDiffuse + totalSpecular + totalEmissiveRadiance;
    o_color = finish(vec4(outgoingLight, outAlpha));
}
#endif
