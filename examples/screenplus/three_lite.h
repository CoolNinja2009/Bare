/* three_lite.h: the part of three.js r147 that Screen+'s 3D samples use, drawn
 * with shaders/three.glsl (add it to the example's bare_add_shaders; it becomes
 * three_shader): a .glb's nodes, meshes and PBR materials as GLTFLoader builds
 * them, the camera and light maths, a spot light's PCF-soft shadow map, and a
 * render that draws opaque then transparent meshes as WebGLRenderer does.
 *
 *     TlScene s;
 *     tl_load_glb(&s, bytes, size);                // nodes, meshes, materials
 *     TlObject *o = tl_find(&s, "Sofa");           // by three's sanitized node name
 *     tl_render(&s, &camera, &lights, target, depth, &opts);
 *
 * All maths is in doubles, as JavaScript's; matrices are column-major (three's
 * .elements). Textures become bare_texture()s: mipmapped, sRGB for colour maps. */
#ifndef SCREENPLUS_THREE_LITE_H
#define SCREENPLUS_THREE_LITE_H
#include "bare.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TL_PI
#define TL_PI 3.141592653589793
#endif

/* ---- maths --------------------------------------------------------------- */

typedef struct { double e[16]; } TlM4;

static inline TlM4 tl_m4_identity(void) {
    TlM4 m = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };
    return m;
}
static inline TlM4 tl_m4_mul(const TlM4 *a, const TlM4 *b) {   /* a * b */
    TlM4 r;
    for (int c = 0; c < 4; c++)
        for (int rw = 0; rw < 4; rw++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += a->e[k * 4 + rw] * b->e[c * 4 + k];
            r.e[c * 4 + rw] = s;
        }
    return r;
}
/* Matrix4.compose */
static inline TlM4 tl_m4_compose(const double p[3], const double q[4], const double s[3]) {
    TlM4 m;
    double x = q[0], y = q[1], z = q[2], w = q[3];
    double x2 = x + x, y2 = y + y, z2 = z + z;
    double xx = x * x2, xy = x * y2, xz = x * z2, yy = y * y2, yz = y * z2, zz = z * z2;
    double wx = w * x2, wy = w * y2, wz = w * z2;
    m.e[0] = (1 - (yy + zz)) * s[0]; m.e[1] = (xy + wz) * s[0]; m.e[2] = (xz - wy) * s[0]; m.e[3] = 0;
    m.e[4] = (xy - wz) * s[1]; m.e[5] = (1 - (xx + zz)) * s[1]; m.e[6] = (yz + wx) * s[1]; m.e[7] = 0;
    m.e[8] = (xz + wy) * s[2]; m.e[9] = (yz - wx) * s[2]; m.e[10] = (1 - (xx + yy)) * s[2]; m.e[11] = 0;
    m.e[12] = p[0]; m.e[13] = p[1]; m.e[14] = p[2]; m.e[15] = 1;
    return m;
}
/* Matrix4.invert */
static inline TlM4 tl_m4_invert(const TlM4 *m) {
    const double *te = m->e;
    double n11 = te[0], n21 = te[1], n31 = te[2], n41 = te[3], n12 = te[4], n22 = te[5], n32 = te[6], n42 = te[7],
           n13 = te[8], n23 = te[9], n33 = te[10], n43 = te[11], n14 = te[12], n24 = te[13], n34 = te[14], n44 = te[15];
    double t11 = n23 * n34 * n42 - n24 * n33 * n42 + n24 * n32 * n43 - n22 * n34 * n43 - n23 * n32 * n44 + n22 * n33 * n44;
    double t12 = n14 * n33 * n42 - n13 * n34 * n42 - n14 * n32 * n43 + n12 * n34 * n43 + n13 * n32 * n44 - n12 * n33 * n44;
    double t13 = n13 * n24 * n42 - n14 * n23 * n42 + n14 * n22 * n43 - n12 * n24 * n43 - n13 * n22 * n44 + n12 * n23 * n44;
    double t14 = n14 * n23 * n32 - n13 * n24 * n32 - n14 * n22 * n33 + n12 * n24 * n33 + n13 * n22 * n34 - n12 * n23 * n34;
    double det = n11 * t11 + n21 * t12 + n31 * t13 + n41 * t14;
    TlM4 r;
    if (det == 0) { memset(&r, 0, sizeof r); return r; }
    double d = 1 / det;
    double *o = r.e;
    o[0] = t11 * d;
    o[1] = (n24 * n33 * n41 - n23 * n34 * n41 - n24 * n31 * n43 + n21 * n34 * n43 + n23 * n31 * n44 - n21 * n33 * n44) * d;
    o[2] = (n22 * n34 * n41 - n24 * n32 * n41 + n24 * n31 * n42 - n21 * n34 * n42 - n22 * n31 * n44 + n21 * n32 * n44) * d;
    o[3] = (n23 * n32 * n41 - n22 * n33 * n41 - n23 * n31 * n42 + n21 * n33 * n42 + n22 * n31 * n43 - n21 * n32 * n43) * d;
    o[4] = t12 * d;
    o[5] = (n13 * n34 * n41 - n14 * n33 * n41 + n14 * n31 * n43 - n11 * n34 * n43 - n13 * n31 * n44 + n11 * n33 * n44) * d;
    o[6] = (n14 * n32 * n41 - n12 * n34 * n41 - n14 * n31 * n42 + n11 * n34 * n42 + n12 * n31 * n44 - n11 * n32 * n44) * d;
    o[7] = (n12 * n33 * n41 - n13 * n32 * n41 + n13 * n31 * n42 - n11 * n33 * n42 - n12 * n31 * n43 + n11 * n32 * n43) * d;
    o[8] = t13 * d;
    o[9] = (n14 * n23 * n41 - n13 * n24 * n41 - n14 * n21 * n43 + n11 * n24 * n43 + n13 * n21 * n44 - n11 * n23 * n44) * d;
    o[10] = (n12 * n24 * n41 - n14 * n22 * n41 + n14 * n21 * n42 - n11 * n24 * n42 - n12 * n21 * n44 + n11 * n22 * n44) * d;
    o[11] = (n13 * n22 * n41 - n12 * n23 * n41 - n13 * n21 * n42 + n11 * n23 * n42 + n12 * n21 * n43 - n11 * n22 * n43) * d;
    o[12] = t14 * d;
    o[13] = (n13 * n24 * n31 - n14 * n23 * n31 + n14 * n21 * n33 - n11 * n24 * n33 - n13 * n21 * n34 + n11 * n23 * n34) * d;
    o[14] = (n14 * n22 * n31 - n12 * n24 * n31 - n14 * n21 * n32 + n11 * n24 * n32 + n12 * n21 * n34 - n11 * n22 * n34) * d;
    o[15] = (n12 * n23 * n31 - n13 * n22 * n31 + n13 * n21 * n32 - n11 * n23 * n32 - n12 * n21 * n33 + n11 * n22 * n33) * d;
    return r;
}
/* Matrix4.makeOrthographic */
static inline TlM4 tl_m4_ortho(double left, double right, double top, double bottom, double zn, double zf) {
    TlM4 m;
    memset(&m, 0, sizeof m);
    double w = 1.0 / (right - left), h = 1.0 / (top - bottom), p = 1.0 / (zf - zn);
    m.e[0] = 2 * w; m.e[12] = -((right + left) * w);
    m.e[5] = 2 * h; m.e[13] = -((top + bottom) * h);
    m.e[10] = -2 * p; m.e[14] = -((zf + zn) * p);
    m.e[15] = 1;
    return m;
}
/* PerspectiveCamera.updateProjectionMatrix (no film offset, no view) */
static inline TlM4 tl_m4_perspective(double fov_deg, double aspect, double zn, double zf, double zoom) {
    double top = zn * tan(TL_PI / 180 * 0.5 * fov_deg) / zoom, height = 2 * top, width = aspect * height;
    double left = -0.5 * width, right = left + width, bottom = top - height;
    TlM4 m;
    memset(&m, 0, sizeof m);
    m.e[0] = 2 * zn / (right - left); m.e[8] = (right + left) / (right - left);
    m.e[5] = 2 * zn / (top - bottom); m.e[9] = (top + bottom) / (top - bottom);
    m.e[10] = -(zf + zn) / (zf - zn); m.e[14] = -2 * zf * zn / (zf - zn);
    m.e[11] = -1;
    return m;
}
static inline void tl_v3_norm(double v[3]) {
    double l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) v[0] /= l, v[1] /= l, v[2] /= l;
}
static inline void tl_v3_cross(const double a[3], const double b[3], double o[3]) {
    double x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x, o[1] = y, o[2] = z;
}
/* Matrix4.lookAt: the rotation that turns -z toward target */
static inline TlM4 tl_m4_lookat(const double eye[3], const double target[3], const double up[3]) {
    double z[3] = { eye[0] - target[0], eye[1] - target[1], eye[2] - target[2] }, x[3], y[3];
    if (z[0] * z[0] + z[1] * z[1] + z[2] * z[2] == 0) z[2] = 1;
    tl_v3_norm(z);
    tl_v3_cross(up, z, x);
    if (x[0] * x[0] + x[1] * x[1] + x[2] * x[2] == 0) {
        if (fabs(up[2]) == 1) z[0] += 0.0001;
        else z[2] += 0.0001;
        tl_v3_norm(z);
        tl_v3_cross(up, z, x);
    }
    tl_v3_norm(x);
    tl_v3_cross(z, x, y);
    TlM4 m = tl_m4_identity();
    m.e[0] = x[0]; m.e[1] = x[1]; m.e[2] = x[2];
    m.e[4] = y[0]; m.e[5] = y[1]; m.e[6] = y[2];
    m.e[8] = z[0]; m.e[9] = z[1]; m.e[10] = z[2];
    return m;
}
/* Quaternion.setFromEuler, order XYZ (radians) */
static inline void tl_quat_euler(double x, double y, double z, double q[4]) {
    double c1 = cos(x / 2), c2 = cos(y / 2), c3 = cos(z / 2), s1 = sin(x / 2), s2 = sin(y / 2), s3 = sin(z / 2);
    q[0] = s1 * c2 * c3 + c1 * s2 * s3;
    q[1] = c1 * s2 * c3 - s1 * c2 * s3;
    q[2] = c1 * c2 * s3 + s1 * s2 * c3;
    q[3] = c1 * c2 * c3 - s1 * s2 * s3;
}
static inline void tl_m4_point(const TlM4 *m, const double p[3], double o[3]) {   /* applyMatrix4 */
    const double *e = m->e;
    double x = p[0], y = p[1], z = p[2];
    double w = 1 / (e[3] * x + e[7] * y + e[11] * z + e[15]);
    o[0] = (e[0] * x + e[4] * y + e[8] * z + e[12]) * w;
    o[1] = (e[1] * x + e[5] * y + e[9] * z + e[13]) * w;
    o[2] = (e[2] * x + e[6] * y + e[10] * z + e[14]) * w;
}
static inline void tl_m4_dir(const TlM4 *m, const double d[3], double o[3]) {   /* transformDirection */
    const double *e = m->e;
    double x = d[0], y = d[1], z = d[2];
    o[0] = e[0] * x + e[4] * y + e[8] * z;
    o[1] = e[1] * x + e[5] * y + e[9] * z;
    o[2] = e[2] * x + e[6] * y + e[10] * z;
    tl_v3_norm(o);
}
static inline void tl_m4_to_f(const TlM4 *m, float o[16]) {
    for (int i = 0; i < 16; i++) o[i] = (float)m->e[i];
}
/* Matrix3.getNormalMatrix(modelView) into a mat4's upper left */
static inline void tl_normal_matrix(const TlM4 *mv, float o[16]) {
    const double *e = mv->e;
    double a11 = e[0], a21 = e[1], a31 = e[2], a12 = e[4], a22 = e[5], a32 = e[6], a13 = e[8], a23 = e[9], a33 = e[10];
    double t11 = a33 * a22 - a32 * a23, t12 = a32 * a13 - a33 * a12, t13 = a23 * a12 - a22 * a13;
    double det = a11 * t11 + a21 * t12 + a31 * t13, d = det != 0 ? 1 / det : 0;
    /* inverse (column-major i[col*3+row]), then transpose */
    double i[9] = { t11 * d, (a31 * a23 - a33 * a21) * d, (a32 * a21 - a31 * a22) * d,
                    t12 * d, (a33 * a11 - a31 * a13) * d, (a31 * a12 - a32 * a11) * d,
                    t13 * d, (a21 * a13 - a23 * a11) * d, (a22 * a11 - a21 * a12) * d };
    memset(o, 0, 16 * sizeof(float));
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++) o[c * 4 + r] = (float)i[r * 3 + c];
    o[15] = 1;
}
/* Matrix3.setUvTransform into a mat4's upper left (third column at [8..10]) */
static inline void tl_uv_transform(double tx, double ty, double sx, double sy, double rot, double cx, double cy, float o[16]) {
    double c = cos(rot), s = sin(rot);
    memset(o, 0, 16 * sizeof(float));
    o[0] = (float)(sx * c); o[1] = (float)(-sy * s);
    o[4] = (float)(sx * s); o[5] = (float)(sy * c);
    o[8] = (float)(-sx * (c * cx + s * cy) + cx + tx); o[9] = (float)(-sy * (-s * cx + c * cy) + cy + ty); o[10] = 1;
    o[15] = 1;
}

/* ---- JSON (enough for glTF) ---------------------------------------------- */

typedef struct TlJ {
    char type;            /* o object, a array, s string, n number, t true, f false, z null */
    double num;
    char *str, *key;      /* key: this value's name in its object */
    struct TlJ *kid;      /* first child */
    struct TlJ *next;
    int n;                /* children */
} TlJ;

static inline void tl_j_ws(const char **p) { while (**p && isspace((unsigned char)**p)) (*p)++; }
static inline char *tl_j_string(const char **p) {
    const char *s = ++*p;
    size_t cap = 16, n = 0;
    char *o = malloc(cap);
    while (*s && *s != '"') {
        char c = *s++;
        if (c == '\\') {
            c = *s++;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'u') { c = '?'; for (int k = 0; k < 4 && *s; k++) s++; }
        }
        if (n + 2 > cap) o = realloc(o, cap *= 2);
        o[n++] = c;
    }
    o[n] = 0;
    *p = *s ? s + 1 : s;
    return o;
}
static inline TlJ *tl_j_parse(const char **p) {
    tl_j_ws(p);
    TlJ *j = calloc(1, sizeof *j);
    char c = **p;
    if (c == '{' || c == '[') {
        j->type = c == '{' ? 'o' : 'a';
        (*p)++;
        TlJ **tail = &j->kid;
        for (;;) {
            tl_j_ws(p);
            if (**p == '}' || **p == ']') { (*p)++; break; }
            if (!**p) break;
            char *key = NULL;
            if (j->type == 'o') {
                key = tl_j_string(p);
                tl_j_ws(p);
                if (**p == ':') (*p)++;
            }
            TlJ *v = tl_j_parse(p);
            v->key = key;
            *tail = v, tail = &v->next, j->n++;
            tl_j_ws(p);
            if (**p == ',') (*p)++;
        }
    } else if (c == '"') {
        j->type = 's', j->str = tl_j_string(p);
    } else if (c == 't' || c == 'f' || c == 'n') {
        j->type = c == 'n' ? 'z' : c;
        while (isalpha((unsigned char)**p)) (*p)++;
    } else {
        char *end;
        j->type = 'n', j->num = strtod(*p, &end);
        *p = end > *p ? end : *p + 1;
    }
    return j;
}
static inline void tl_j_free(TlJ *j) {
    while (j) {
        TlJ *nx = j->next;
        tl_j_free(j->kid);
        free(j->str), free(j->key), free(j);
        j = nx;
    }
}
static inline TlJ *tl_j_get(const TlJ *o, const char *key) {
    if (!o || o->type != 'o') return NULL;
    for (TlJ *k = o->kid; k; k = k->next)
        if (!strcmp(k->key, key)) return k;
    return NULL;
}
static inline TlJ *tl_j_at(const TlJ *a, int i) {
    if (!a || a->type != 'a') return NULL;
    TlJ *k = a->kid;
    while (k && i-- > 0) k = k->next;
    return k;
}
static inline double tl_j_num(const TlJ *j, double def) { return j && j->type == 'n' ? j->num : def; }
static inline int tl_j_int(const TlJ *j, int def) { return j && j->type == 'n' ? (int)j->num : def; }

/* ---- scene ---------------------------------------------------------------- */

enum { TL_BASIC, TL_STANDARD, TL_PHYSICAL, TL_SHADOW };
enum { TL_FRONT, TL_BACK, TL_DOUBLE };

typedef struct TlMaterial {
    int kind;
    float color[3], opacity, emissive[3];
    float roughness, metalness, ior, specular_intensity, specular_color[3], normal_scale[2], ao_intensity, env_intensity;
    BareImage map, rm_map, normal_map, emissive_map, ao_map;
    float uv[16];         /* the map's uvTransform */
    int side;
    bool transparent, depth_write, depth_test;
    bool tone_mapped;
} TlMaterial;

typedef struct TlObject {
    char name[64];
    int parent;
    double pos[3], quat[4], scale[3];
    TlM4 world;
    float *verts;         /* pos.xyz u, normal.xyz v: 8 floats a vertex, triangles */
    int count;
    TlMaterial mat;
    bool visible, cast_shadow, receive_shadow;
    int order;            /* scene graph order (three's render list id tiebreak) */
} TlObject;

typedef struct TlScene {
    TlObject *obj;
    int n, cap;
    BareImage white;
    struct { int source; bool srgb; BareImage img; } *tex;
    int ntex;
} TlScene;

static inline TlObject *tl_add(TlScene *s) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 32;
        s->obj = realloc(s->obj, (size_t)s->cap * sizeof *s->obj);
    }
    TlObject *o = &s->obj[s->n];
    memset(o, 0, sizeof *o);
    o->parent = -1;
    o->quat[3] = 1;
    o->scale[0] = o->scale[1] = o->scale[2] = 1;
    o->visible = true;
    o->order = s->n++;
    return o;
}
static inline TlObject *tl_find(TlScene *s, const char *name) {
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->obj[i].name, name)) return &s->obj[i];
    return NULL;
}
static inline void tl_material_default(TlMaterial *m, int kind) {
    memset(m, 0, sizeof *m);
    m->kind = kind;
    m->color[0] = m->color[1] = m->color[2] = 1;
    m->opacity = 1;
    m->roughness = 1;
    m->metalness = 0;
    m->ior = 1.5f;
    m->specular_intensity = 1;
    m->specular_color[0] = m->specular_color[1] = m->specular_color[2] = 1;
    m->normal_scale[0] = m->normal_scale[1] = 1;
    m->ao_intensity = 1;
    m->env_intensity = 1;
    m->depth_write = m->depth_test = true;
    m->tone_mapped = true;
    tl_uv_transform(0, 0, 1, 1, 0, 0, 0, m->uv);
}
/* world matrices, parents first (objects are added parents first) */
static inline void tl_update(TlScene *s) {
    for (int i = 0; i < s->n; i++) {
        TlObject *o = &s->obj[i];
        TlM4 local = tl_m4_compose(o->pos, o->quat, o->scale);
        o->world = o->parent >= 0 ? tl_m4_mul(&s->obj[o->parent].world, &local) : local;
    }
}

/* PropertyBinding.sanitizeNodeName: whitespace to _, no [ ] . : / */
static inline void tl_sanitize(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (; *in && n + 1 < cap; in++) {
        char c = *in;
        if (strchr("[].:/", c)) continue;
        out[n++] = isspace((unsigned char)c) ? '_' : c;
    }
    out[n] = 0;
}

typedef struct TlGlb {
    TlJ *json;
    const unsigned char *bin;
    size_t bin_size;
} TlGlb;

static inline const unsigned char *tl_accessor(const TlGlb *g, int index, int *count, int *comps, int *ctype,
                                               int *stride, bool *normalized) {
    TlJ *a = tl_j_at(tl_j_get(g->json, "accessors"), index);
    if (!a) return NULL;
    TlJ *bv = tl_j_at(tl_j_get(g->json, "bufferViews"), tl_j_int(tl_j_get(a, "bufferView"), 0));
    const char *type = tl_j_get(a, "type") ? tl_j_get(a, "type")->str : "SCALAR";
    *comps = !strcmp(type, "VEC2") ? 2 : !strcmp(type, "VEC3") ? 3 : !strcmp(type, "VEC4") ? 4 : 1;
    *ctype = tl_j_int(tl_j_get(a, "componentType"), 5126);
    *count = tl_j_int(tl_j_get(a, "count"), 0);
    int csize = *ctype == 5126 || *ctype == 5125 ? 4 : *ctype == 5123 || *ctype == 5122 ? 2 : 1;
    *stride = tl_j_int(tl_j_get(bv, "byteStride"), csize * *comps);
    *normalized = tl_j_get(a, "normalized") && tl_j_get(a, "normalized")->type == 't';
    size_t off = (size_t)tl_j_int(tl_j_get(bv, "byteOffset"), 0) + (size_t)tl_j_int(tl_j_get(a, "byteOffset"), 0);
    return off < g->bin_size ? g->bin + off : NULL;
}
static inline double tl_read(const unsigned char *p, int ctype, bool normalized) {
    switch (ctype) {
    case 5126: { float f; memcpy(&f, p, 4); return f; }
    case 5125: { unsigned v; memcpy(&v, p, 4); return v; }
    case 5123: { unsigned short v; memcpy(&v, p, 2); return normalized ? v / 65535.0 : v; }
    case 5122: { short v; memcpy(&v, p, 2); return normalized ? fmax(v / 32767.0, -1) : v; }
    case 5121: return normalized ? p[0] / 255.0 : p[0];
    case 5120: return normalized ? fmax((signed char)p[0] / 127.0, -1) : (signed char)p[0];
    }
    return 0;
}

/* A glTF texture as a bare_texture (mipmapped, repeat as the file's sampler), cached. */
static inline BareImage tl_texture(TlScene *s, const TlGlb *g, const TlJ *texinfo, bool srgb) {
    if (!texinfo) return 0;
    int ti = tl_j_int(tl_j_get(texinfo, "index"), -1);
    TlJ *t = tl_j_at(tl_j_get(g->json, "textures"), ti);
    if (!t) return 0;
    int src = tl_j_int(tl_j_get(t, "source"), -1);
    for (int i = 0; i < s->ntex; i++)
        if (s->tex[i].source == src && s->tex[i].srgb == srgb) return s->tex[i].img;
    TlJ *im = tl_j_at(tl_j_get(g->json, "images"), src);
    TlJ *bv = im ? tl_j_at(tl_j_get(g->json, "bufferViews"), tl_j_int(tl_j_get(im, "bufferView"), -1)) : NULL;
    if (!bv) return 0;
    size_t off = (size_t)tl_j_int(tl_j_get(bv, "byteOffset"), 0), len = (size_t)tl_j_int(tl_j_get(bv, "byteLength"), 0);
    if (off + len > g->bin_size) return 0;
    int w, h;
    unsigned char *px = bare_image_decode(g->bin + off, (int)len, &w, &h);
    if (!px) return 0;
    TlJ *smp = tl_j_at(tl_j_get(g->json, "samplers"), tl_j_int(tl_j_get(t, "sampler"), -1));
    bool repeat = tl_j_int(tl_j_get(smp, "wrapS"), 10497) == 10497;
    BareImage img = bare_texture(.width = w, .height = h, .rgba8 = px, .mipmaps = true, .srgb = srgb, .repeat = repeat);
    free(px);
    s->tex = realloc(s->tex, (size_t)(s->ntex + 1) * sizeof *s->tex);
    s->tex[s->ntex].source = src, s->tex[s->ntex].srgb = srgb, s->tex[s->ntex].img = img;
    s->ntex++;
    return img;
}

/* GLTFLoader's material for a glTF material (index -1: its default) */
static inline void tl_gltf_material(TlScene *s, const TlGlb *g, int index, bool derivative_tangents, TlMaterial *m) {
    TlJ *md = tl_j_at(tl_j_get(g->json, "materials"), index);
    TlJ *ext = tl_j_get(md, "extensions");
    bool unlit = tl_j_get(ext, "KHR_materials_unlit") != NULL;
    bool physical = !unlit && (tl_j_get(ext, "KHR_materials_ior") || tl_j_get(ext, "KHR_materials_specular") ||
                               tl_j_get(ext, "KHR_materials_clearcoat") || tl_j_get(ext, "KHR_materials_transmission"));
    tl_material_default(m, unlit ? TL_BASIC : physical ? TL_PHYSICAL : TL_STANDARD);
    if (!md) { m->metalness = 1; return; }
    TlJ *pbr = tl_j_get(md, "pbrMetallicRoughness");
    TlJ *bc = tl_j_get(pbr, "baseColorFactor");
    if (bc) for (int i = 0; i < 4; i++) {
        double v = tl_j_num(tl_j_at(bc, i), 1);
        if (i < 3) m->color[i] = (float)v; else m->opacity = (float)v;
    }
    m->map = tl_texture(s, g, tl_j_get(pbr, "baseColorTexture"), true);
    if (!unlit) {
        m->metalness = (float)tl_j_num(tl_j_get(pbr, "metallicFactor"), 1);
        m->roughness = (float)tl_j_num(tl_j_get(pbr, "roughnessFactor"), 1);
        m->rm_map = tl_texture(s, g, tl_j_get(pbr, "metallicRoughnessTexture"), false);
        TlJ *nt = tl_j_get(md, "normalTexture");
        if (nt) {
            m->normal_map = tl_texture(s, g, nt, false);
            float sc = (float)tl_j_num(tl_j_get(nt, "scale"), 1);
            m->normal_scale[0] = sc, m->normal_scale[1] = derivative_tangents ? -sc : sc;
        }
        TlJ *ot = tl_j_get(md, "occlusionTexture");
        if (ot) {
            m->ao_map = tl_texture(s, g, ot, false);
            m->ao_intensity = (float)tl_j_num(tl_j_get(ot, "strength"), 1);
        }
        TlJ *ef = tl_j_get(md, "emissiveFactor");
        if (ef) for (int i = 0; i < 3; i++) m->emissive[i] = (float)tl_j_num(tl_j_at(ef, i), 0);
        m->emissive_map = tl_texture(s, g, tl_j_get(md, "emissiveTexture"), true);
        TlJ *ior = tl_j_get(ext, "KHR_materials_ior");
        if (ior) m->ior = (float)tl_j_num(tl_j_get(ior, "ior"), 1.5);
        TlJ *sp = tl_j_get(ext, "KHR_materials_specular");
        if (sp) {
            m->specular_intensity = (float)tl_j_num(tl_j_get(sp, "specularFactor"), 1);
            TlJ *sc = tl_j_get(sp, "specularColorFactor");
            if (sc) for (int i = 0; i < 3; i++) m->specular_color[i] = (float)tl_j_num(tl_j_at(sc, i), 1);
        }
    }
    if (tl_j_get(md, "doubleSided") && tl_j_get(md, "doubleSided")->type == 't') m->side = TL_DOUBLE;
    TlJ *am = tl_j_get(md, "alphaMode");
    if (am && am->str && !strcmp(am->str, "BLEND")) m->transparent = true, m->depth_write = false;
}

/* Loads a .glb's default scene; objects keep the file's node order (parents first). */
static inline bool tl_load_glb(TlScene *s, const unsigned char *data, size_t size) {
    memset(s, 0, sizeof *s);
    unsigned char white[4] = { 255, 255, 255, 255 };
    s->white = bare_image_from_pixels(1, 1, white);
    if (size < 20 || memcmp(data, "glTF", 4)) return false;
    unsigned jlen;
    memcpy(&jlen, data + 12, 4);
    char *js = malloc(jlen + 1);
    memcpy(js, data + 20, jlen);
    js[jlen] = 0;
    const char *p = js;
    TlGlb g = { tl_j_parse(&p), NULL, 0 };
    free(js);
    size_t boff = 20 + jlen;
    if (boff + 8 <= size) {
        unsigned blen;
        memcpy(&blen, data + boff, 4);
        g.bin = data + boff + 8, g.bin_size = blen;
    }
    TlJ *nodes = tl_j_get(g.json, "nodes"), *meshes = tl_j_get(g.json, "meshes");
    TlJ *scene = tl_j_at(tl_j_get(g.json, "scenes"), tl_j_int(tl_j_get(g.json, "scene"), 0));
    /* depth-first from the scene's roots, as three adds them */
    int stack[256], parents[256], sp = 0;
    TlJ *roots = tl_j_get(scene, "nodes");
    for (int i = roots ? roots->n - 1 : -1; i >= 0; i--) stack[sp] = tl_j_int(tl_j_at(roots, i), 0), parents[sp++] = -1;
    while (sp > 0) {
        int ni = stack[--sp], parent = parents[sp];
        TlJ *nd = tl_j_at(nodes, ni);
        if (!nd) continue;
        TlObject *o = tl_add(s);
        int me = (int)(o - s->obj);
        o->parent = parent;
        if (tl_j_get(nd, "name")) tl_sanitize(tl_j_get(nd, "name")->str, o->name, sizeof o->name);
        TlJ *t = tl_j_get(nd, "translation"), *r = tl_j_get(nd, "rotation"), *sc = tl_j_get(nd, "scale");
        for (int k = 0; k < 3; k++) {
            if (t) o->pos[k] = tl_j_num(tl_j_at(t, k), 0);
            if (sc) o->scale[k] = tl_j_num(tl_j_at(sc, k), 1);
        }
        if (r) for (int k = 0; k < 4; k++) o->quat[k] = tl_j_num(tl_j_at(r, k), k == 3);
        TlJ *mesh = tl_j_at(meshes, tl_j_int(tl_j_get(nd, "mesh"), -1));
        TlJ *prims = tl_j_get(mesh, "primitives");
        for (int pi = 0; prims && pi < prims->n; pi++) {
            TlJ *pr = tl_j_at(prims, pi);
            TlJ *attr = tl_j_get(pr, "attributes");
            TlObject *target = o;
            if (prims->n > 1) {   /* a Group of meshes */
                target = tl_add(s);
                o = &s->obj[me];
                target->parent = me;
                snprintf(target->name, sizeof target->name, "%.50s_%d", o->name, pi);
            }
            int pc, pcomp, pct, pstr, nc = 0, ncomp, nct, nstr = 0, uc = 0, ucomp, uct, ustr = 0, ic = 0, icomp, ict, istr = 0;
            bool pn, nn, un, in;
            const unsigned char *P = tl_accessor(&g, tl_j_int(tl_j_get(attr, "POSITION"), -1), &pc, &pcomp, &pct, &pstr, &pn);
            const unsigned char *N = tl_j_get(attr, "NORMAL") ? tl_accessor(&g, tl_j_int(tl_j_get(attr, "NORMAL"), -1), &nc, &ncomp, &nct, &nstr, &nn) : NULL;
            const unsigned char *U = tl_j_get(attr, "TEXCOORD_0") ? tl_accessor(&g, tl_j_int(tl_j_get(attr, "TEXCOORD_0"), -1), &uc, &ucomp, &uct, &ustr, &un) : NULL;
            const unsigned char *I = tl_j_get(pr, "indices") ? tl_accessor(&g, tl_j_int(tl_j_get(pr, "indices"), -1), &ic, &icomp, &ict, &istr, &in) : NULL;
            if (!P) continue;
            int count = I ? ic : pc;
            target->verts = malloc((size_t)count * 8 * sizeof(float));
            target->count = count;
            for (int v = 0; v < count; v++) {
                int idx = I ? (int)tl_read(I + (size_t)v * istr, ict, false) : v;
                float *d = target->verts + (size_t)v * 8;
                for (int k = 0; k < 3; k++) d[k] = (float)tl_read(P + (size_t)idx * pstr + k * 4, pct, pn);
                for (int k = 0; k < 3; k++) d[4 + k] = N ? (float)tl_read(N + (size_t)idx * nstr + k * (nct == 5126 ? 4 : nct == 5122 ? 2 : 1), nct, nn) : 0;
                int us = uct == 5126 ? 4 : uct == 5123 ? 2 : 1;
                d[3] = U ? (float)tl_read(U + (size_t)idx * ustr, uct, un) : 0;
                d[7] = U ? (float)tl_read(U + (size_t)idx * ustr + us, uct, un) : 0;
            }
            bool tangents = tl_j_get(attr, "TANGENT") != NULL;
            tl_gltf_material(s, &g, tl_j_int(tl_j_get(pr, "material"), -1), !tangents, &target->mat);
        }
        TlJ *kids = tl_j_get(nd, "children");
        for (int i = kids ? kids->n - 1 : -1; i >= 0 && sp < 256; i--)
            stack[sp] = tl_j_int(tl_j_at(kids, i), 0), parents[sp++] = me;
    }
    tl_j_free(g.json);
    tl_update(s);
    return true;
}

/* ---- render ---------------------------------------------------------------- */

typedef struct TlCamera {
    TlM4 projection, world, view;   /* view = world inverted */
    bool ortho;
} TlCamera;

typedef struct TlPointLight { double pos[3]; float color[3], intensity, distance, decay; } TlPointLight;
typedef struct TlSpotLight {
    bool on, cast_shadow;
    double pos[3], target[3];
    float color[3], intensity, distance, angle, penumbra, decay, bias;
    int map_size;
} TlSpotLight;
typedef struct TlLights {
    float ambient[3];                /* AmbientLight color * intensity */
    TlPointLight point[4];
    int npoints;
    TlSpotLight spot;
} TlLights;

typedef struct TlParams {
    float projection[16], model_view[16], model[16], normal_matrix[16], spot_shadow[16], uv_transform[16], view[16];
    float color[4], emissive[4], mat[4], mat_b[4], spec_color[4], flags[4], flags2[4], ambient[4];
    float point_pos[4][4], point_col[4][4];
    float spot_pos[4], spot_dir[4], spot_col[4], spot_misc[4], misc[4], env[4];
} TlParams;

typedef struct TlRenderer {
    BareShader shader;
    BareImage shadow_map, shadow_depth;   /* the spot light's (R32F depth, bottom_up) */
    int shadow_size;
    float exposure;
    bool tone_mapping;
    bool flip_facing;                     /* gl_FrontFacing reads backwards in this backend */
    BareImage env_map;                    /* PMREM cube uv, 0 = none */
    BareImage white;                      /* bound for maps a material lacks (tl_render sets it) */
    float env[3];                         /* texel width, height, max mip */
} TlRenderer;

static inline TlM4 tl_spot_shadow_camera(const TlSpotLight *sp, TlM4 *proj_out) {
    TlM4 rot = tl_m4_lookat(sp->pos, sp->target, (double[3]){ 0, 1, 0 });
    rot.e[12] = sp->pos[0], rot.e[13] = sp->pos[1], rot.e[14] = sp->pos[2];
    double zf = sp->distance > 0 ? sp->distance : 500;
    *proj_out = tl_m4_perspective(180 / TL_PI * 2 * sp->angle, 1, 0.5, zf, 1);
    return tl_m4_invert(&rot);
}

static inline void tl_fill_lights(TlParams *p, const TlCamera *cam, const TlLights *l, const TlRenderer *r, const TlM4 *shadow_matrix) {
    for (int i = 0; i < 3; i++) p->ambient[i] = l->ambient[i] * (float)TL_PI;
    p->ambient[3] = r->env_map ? 1.f : 0.f;
    for (int i = 0; i < l->npoints && i < 4; i++) {
        double v[3];
        tl_m4_point(&cam->view, l->point[i].pos, v);
        for (int k = 0; k < 3; k++) p->point_pos[i][k] = (float)v[k], p->point_col[i][k] = l->point[i].color[k] * l->point[i].intensity * (float)TL_PI;
        p->point_pos[i][3] = l->point[i].distance;
        p->point_col[i][3] = l->point[i].decay;
    }
    p->misc[0] = (float)l->npoints;
    p->misc[1] = r->exposure;
    p->misc[2] = r->tone_mapping ? 1.f : 0.f;
    p->misc[3] = r->flip_facing ? 1.f : 0.f;
    const TlSpotLight *sp = &l->spot;
    if (sp->on) {
        double v[3], d[3] = { sp->pos[0] - sp->target[0], sp->pos[1] - sp->target[1], sp->pos[2] - sp->target[2] };
        tl_m4_point(&cam->view, sp->pos, v);
        double dv[3];
        tl_m4_dir(&cam->view, d, dv);
        for (int k = 0; k < 3; k++) p->spot_pos[k] = (float)v[k], p->spot_dir[k] = (float)dv[k], p->spot_col[k] = sp->color[k] * sp->intensity * (float)TL_PI;
        p->spot_pos[3] = sp->distance;
        p->spot_dir[3] = sp->decay;
        p->spot_col[3] = (float)cos(sp->angle);
        p->spot_misc[0] = (float)cos(sp->angle * (1 - sp->penumbra));
        p->spot_misc[1] = sp->cast_shadow && r->shadow_map ? (float)r->shadow_size : 0.f;
        p->spot_misc[2] = sp->bias;
        p->spot_misc[3] = 1;
        tl_m4_to_f(shadow_matrix, p->spot_shadow);
    }
    tl_m4_to_f(&cam->view, p->view);
    p->env[0] = r->env[0], p->env[1] = r->env[1], p->env[2] = r->env[2];
}

static inline void tl_fill_material(TlParams *p, const TlMaterial *m) {
    for (int i = 0; i < 3; i++) p->color[i] = m->color[i], p->emissive[i] = m->emissive[i], p->spec_color[i] = m->specular_color[i];
    p->color[3] = m->opacity;
    p->emissive[3] = m->env_intensity;
    p->mat[0] = m->roughness, p->mat[1] = m->metalness, p->mat[2] = m->normal_scale[0], p->mat[3] = m->normal_scale[1];
    int kind = m->kind == TL_BASIC ? 0 : m->kind == TL_SHADOW ? 2 : 1;
    p->mat_b[0] = m->ior, p->mat_b[1] = m->specular_intensity, p->mat_b[2] = m->ao_intensity, p->mat_b[3] = (float)kind;
    p->spec_color[3] = m->kind == TL_PHYSICAL ? 1.f : 0.f;
    p->flags[0] = m->map ? 1.f : 0.f, p->flags[1] = m->rm_map ? 1.f : 0.f;
    p->flags[2] = m->normal_map ? 1.f : 0.f, p->flags[3] = m->emissive_map ? 1.f : 0.f;
    p->flags2[0] = m->ao_map ? 1.f : 0.f;
    p->flags2[1] = (float)m->side;
    memcpy(p->uv_transform, m->uv, sizeof m->uv);
    p->env[3] = m->transparent ? 1.f : 0.f;
}

static inline void tl_draw_object(const TlRenderer *r, TlParams *p, const TlObject *o, const TlCamera *cam, BareImage target,
                                  BareImage depth, int side, bool clear, const float clear_color[4]) {
    const TlMaterial *m = &o->mat;
    TlM4 mv = tl_m4_mul(&cam->view, &o->world);
    tl_m4_to_f(&mv, p->model_view);
    tl_m4_to_f(&o->world, p->model);
    tl_normal_matrix(&mv, p->normal_matrix);
    tl_fill_material(p, m);
    p->flags2[1] = (float)side;
    p->flags2[2] = o->receive_shadow ? 1.f : 0.f;
    BareImage w = r->shadow_map ? r->shadow_map : 0;
    BareDraw d = { .target = target, .shader = r->shader, .params = p, .params_size = (int)sizeof *p,
                   .vertices = o->verts, .vertex_size = 32, .vertex_count = o->count,
                   .blend = m->transparent ? BARE_BLEND_ALPHA : BARE_BLEND_NONE,
                   .depth = depth, .depth_test = m->depth_test ? BARE_DEPTH_LESS_EQUAL : BARE_DEPTH_ALWAYS,
                           .depth_read_only = !m->depth_write };
    BareImage in[7] = { m->map, m->rm_map, m->normal_map, m->emissive_map, m->ao_map, w, r->env_map };
    for (int i = 0; i < 7; i++) d.inputs[i] = in[i] ? in[i] : r->white;
    if (clear) {
        d.clear = true, d.clear_depth = true;
        memcpy(d.clear_color, clear_color, sizeof d.clear_color);
    }
    bare_draw_(&d, __FILE__, __LINE__);
}

/* three's opaque then transparent lists: opaque in scene order, transparent
 * back to front by the projected z of their origin; double-sided transparent
 * meshes twice (back faces, then front). Clears target (to clear_color) and
 * depth first. The spot light's shadow map is drawn first when it casts. */
static inline void tl_render(TlScene *s, TlRenderer *r, const TlCamera *cam, const TlLights *l, BareImage target,
                             BareImage depth, const float clear_color[4]) {
    static TlParams p;
    memset(&p, 0, sizeof p);
    r->white = s->white;
    TlM4 shadow_matrix = tl_m4_identity();
    if (l->spot.on && l->spot.cast_shadow && r->shadow_map) {
        TlM4 sproj, sview = tl_spot_shadow_camera(&l->spot, &sproj);
        TlM4 bias = { { 0.5, 0, 0, 0, 0, 0.5, 0, 0, 0, 0, 0.5, 0, 0.5, 0.5, 0.5, 1 } };
        TlM4 t = tl_m4_mul(&bias, &sproj);
        shadow_matrix = tl_m4_mul(&t, &sview);
        TlCamera sc = { sproj, tl_m4_invert(&sview), sview, false };
        (void)sc.world;
        TlParams sp;
        memset(&sp, 0, sizeof sp);
        tl_m4_to_f(&sproj, sp.projection);
        sp.misc[3] = r->flip_facing ? 1.f : 0.f;
        bool first = true;
        for (int i = 0; i < s->n; i++) {
            TlObject *o = &s->obj[i];
            if (!o->visible || !o->cast_shadow || !o->verts) continue;
            TlMaterial dm;
            tl_material_default(&dm, TL_BASIC);
            TlM4 mv = tl_m4_mul(&sc.view, &o->world);
            tl_m4_to_f(&mv, sp.model_view);
            tl_m4_to_f(&o->world, sp.model);
            sp.mat_b[3] = 3;   /* depth */
            sp.flags2[1] = (float)(o->mat.side == TL_FRONT ? TL_BACK : o->mat.side == TL_BACK ? TL_FRONT : TL_DOUBLE);
            tl_uv_transform(0, 0, 1, 1, 0, 0, 0, sp.uv_transform);
            BareDraw d = { .target = r->shadow_map, .shader = r->shader, .params = &sp, .params_size = (int)sizeof sp,
                           .vertices = o->verts, .vertex_size = 32, .vertex_count = o->count,
                           .depth = r->shadow_depth, .depth_test = BARE_DEPTH_LESS_EQUAL };
            for (int k = 0; k < 7; k++) d.inputs[k] = s->white;
            if (first) {
                d.clear = true, d.clear_depth = true;
                d.clear_color[0] = d.clear_color[1] = d.clear_color[2] = d.clear_color[3] = 1;
                first = false;
            }
            bare_draw_(&d, __FILE__, __LINE__);
        }
    }
    tl_m4_to_f(&cam->projection, p.projection);
    p.flags2[3] = cam->ortho ? 1.f : 0.f;
    tl_fill_lights(&p, cam, l, r, &shadow_matrix);

    bool clear = true;
    for (int pass = 0; pass < 2; pass++) {
        int order[1024], n = 0;
        double z[1024];
        TlM4 vp = tl_m4_mul(&cam->projection, &cam->view);
        for (int i = 0; i < s->n && n < 1024; i++) {
            TlObject *o = &s->obj[i];
            if (!o->visible || !o->verts || o->mat.transparent != (pass == 1)) continue;
            double c[3] = { o->world.e[12], o->world.e[13], o->world.e[14] }, q[3];
            tl_m4_point(&vp, c, q);
            order[n] = i, z[n++] = q[2];
        }
        if (pass == 1)   /* reversePainterSortStable: zf first, then id */
            for (int a = 1; a < n; a++)
                for (int b = a; b > 0 && z[b - 1] < z[b]; b--) {
                    double tz = z[b]; z[b] = z[b - 1]; z[b - 1] = tz;
                    int to = order[b]; order[b] = order[b - 1]; order[b - 1] = to;
                }
        for (int k = 0; k < n; k++) {
            TlObject *o = &s->obj[order[k]];
            if (pass == 1 && o->mat.side == TL_DOUBLE) {
                tl_draw_object(r, &p, o, cam, target, depth, TL_BACK, clear, clear_color);
                clear = false;
                tl_draw_object(r, &p, o, cam, target, depth, TL_FRONT, false, clear_color);
            } else {
                tl_draw_object(r, &p, o, cam, target, depth, o->mat.side, clear, clear_color);
            }
            clear = false;
        }
    }
}

#endif
