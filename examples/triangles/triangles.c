/* triangles.c: a mesh of flat-shaded triangles lit by a light that follows the
 * mouse; Screen+'s "Triangles & Light" wallpaper.
 *
 * A port of Maksim Surguy's triangle pattern generator (MIT, see LICENSE.txt),
 * on Matthew Wagerfield's Flat Surface Shader and ironwallaby's Delaunay
 * triangulation: random points over a plane 1.2 times the window, triangulated,
 * each triangle shaded by one point light (ambient plus diffuse, by its normal
 * and the direction to the light) and drawn as canvas draws it there: filled
 * and stroked a pixel wide in its colour. The arithmetic is the page's, down to
 * its Float32Arrays, so the colours come out the same.
 *
 * A Screen+ app wallpaper: settings are the Customise panel (LivelyProperties.json
 * beside the exe), sent by Screen+ on stdin (see ../screenplus/screenplus_wallpaper.h);
 * by hand they're arguments: triangles meshSlices=600 lightDiffuse=#ff8800
 * While Screen+ has it paused it draws nothing. */
#include "bare.h"
#include "triangles_shaders.h"
#include "../screenplus/screenplus_wallpaper.h"
#include "../screenplus/canvas2d.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- FSS colours: Float32Array rgba, from hex ------------------------------------ */

typedef struct Color { float rgba[4]; } Color;

static void color_set(Color *c, const char *hex) {
    if (*hex == '#') hex++;
    size_t len = strlen(hex), size = len / 3;
    if (size == 0 || size > 2) return;
    for (int i = 0; i < 3; i++) {
        char part[3] = { 0 };
        memcpy(part, hex + i * size, size);
        c->rgba[i] = (float)(strtol(part, NULL, 16) / 255.0);
    }
}

static int hexify(float channel) { return (int)ceil((double)channel * 255); }

/* ---- the plane: random points, Delaunay triangles --------------------------------- */

typedef struct Tri { float a[3], b[3], c[3], centroid[3], normal[3]; } Tri;

static Tri *tris;
static int ntris;
static double plane_w, plane_h;

typedef struct Circle { int i, j, k; double x, y, r; } Circle;

#define EPSILON (1.0 / 1048576.0)

static bool circumcircle(double (*v)[2], int i, int j, int k, Circle *out) {
    double x1 = v[i][0], y1 = v[i][1], x2 = v[j][0], y2 = v[j][1], x3 = v[k][0], y3 = v[k][1];
    double fy12 = fabs(y1 - y2), fy23 = fabs(y2 - y3), xc, yc, m1, m2, mx1, mx2, my1, my2;
    if (fy12 < EPSILON && fy23 < EPSILON) return false;   /* "Eek! Coincident points!" */
    if (fy12 < EPSILON) {
        m2 = -((x3 - x2) / (y3 - y2)), mx2 = (x2 + x3) / 2.0, my2 = (y2 + y3) / 2.0;
        xc = (x2 + x1) / 2.0, yc = m2 * (xc - mx2) + my2;
    } else if (fy23 < EPSILON) {
        m1 = -((x2 - x1) / (y2 - y1)), mx1 = (x1 + x2) / 2.0, my1 = (y1 + y2) / 2.0;
        xc = (x3 + x2) / 2.0, yc = m1 * (xc - mx1) + my1;
    } else {
        m1 = -((x2 - x1) / (y2 - y1)), m2 = -((x3 - x2) / (y3 - y2));
        mx1 = (x1 + x2) / 2.0, mx2 = (x2 + x3) / 2.0, my1 = (y1 + y2) / 2.0, my2 = (y2 + y3) / 2.0;
        xc = (m1 * mx1 - m2 * mx2 + my2 - my1) / (m1 - m2);
        yc = fy12 > fy23 ? m1 * (xc - mx1) + my1 : m2 * (xc - mx2) + my2;
    }
    double dx = x2 - xc, dy = y2 - yc;
    *out = (Circle){ i, j, k, xc, yc, dx * dx + dy * dy };
    return true;
}

typedef struct List { void *p; int n, cap; size_t size; } List;
static void *list_push(List *l) {
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 256;
        void *np = realloc(l->p, (size_t)cap * l->size);
        if (!np) return NULL;
        l->p = np, l->cap = cap;
    }
    return (char *)l->p + (size_t)l->n++ * l->size;
}
static void list_remove(List *l, int at, int count) {   /* splice(at, count): nothing past the end */
    if (at >= l->n) return;
    if (count > l->n - at) count = l->n - at;
    memmove((char *)l->p + (size_t)at * l->size, (char *)l->p + (size_t)(at + count) * l->size,
            (size_t)(l->n - at - count) * l->size);
    l->n -= count;
}

/* indices sorted by x, largest first, keeping ties in order (V8's sort is stable) */
static void sort_indices(int *idx, int n, double (*v)[2]) {
    int *tmp = malloc((size_t)n * sizeof *tmp);
    if (!tmp) return;
    for (int width = 1; width < n; width *= 2)
        for (int lo = 0; lo < n; lo += 2 * width) {
            int mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n;
            int a = lo, b = mid, o = lo;
            while (a < mid && b < hi) tmp[o++] = v[idx[b]][0] - v[idx[a]][0] < 0 ? idx[b++] : idx[a++];
            while (a < mid) tmp[o++] = idx[a++];
            while (b < hi) tmp[o++] = idx[b++];
            memcpy(idx + lo, tmp + lo, (size_t)(hi - lo) * sizeof *idx);
        }
    free(tmp);
}

/* Delaunay.triangulate: triplets of vertex indices into *out (count in *nout). */
static void triangulate(double (*verts)[2], int n, int **out, int *nout) {
    *out = NULL, *nout = 0;
    if (n < 3) return;
    double (*v)[2] = malloc((size_t)(n + 3) * sizeof *v);
    int *indices = malloc((size_t)n * sizeof *indices);
    if (!v || !indices) { free(v), free(indices); return; }
    memcpy(v, verts, (size_t)n * sizeof *v);
    for (int i = 0; i < n; i++) indices[i] = i;
    sort_indices(indices, n, v);
    double xmin = INFINITY, ymin = INFINITY, xmax = -INFINITY, ymax = -INFINITY;
    for (int i = n; i--;) {
        if (v[i][0] < xmin) xmin = v[i][0];
        if (v[i][0] > xmax) xmax = v[i][0];
        if (v[i][1] < ymin) ymin = v[i][1];
        if (v[i][1] > ymax) ymax = v[i][1];
    }
    double dx = xmax - xmin, dy = ymax - ymin, dmax = fmax(dx, dy), xmid = xmin + dx * 0.5, ymid = ymin + dy * 0.5;
    v[n][0] = xmid - 20 * dmax, v[n][1] = ymid - dmax;
    v[n + 1][0] = xmid, v[n + 1][1] = ymid + 20 * dmax;
    v[n + 2][0] = xmid + 20 * dmax, v[n + 2][1] = ymid - dmax;
    List open = { .size = sizeof(Circle) }, closed = { .size = sizeof(Circle) }, edges = { .size = sizeof(int) };
    Circle cc;
    if (circumcircle(v, n, n + 1, n + 2, &cc)) *(Circle *)list_push(&open) = cc;
    for (int i = n; i--; edges.n = 0) {
        int c = indices[i];
        for (int j = open.n; j--;) {
            Circle *o = (Circle *)open.p + j;
            double ddx = v[c][0] - o->x;
            if (ddx > 0.0 && ddx * ddx > o->r) {
                *(Circle *)list_push(&closed) = *((Circle *)open.p + j);
                list_remove(&open, j, 1);
                continue;
            }
            double ddy = v[c][1] - o->y;
            if (ddx * ddx + ddy * ddy - o->r > EPSILON) continue;
            int tri[6] = { o->i, o->j, o->j, o->k, o->k, o->i };
            for (int k = 0; k < 6; k++) *(int *)list_push(&edges) = tri[k];
            list_remove(&open, j, 1);
        }
        /* dedup */
        /* (after a pair is removed the page reads on past the shorter list: undefined there, -1 here) */
#define EDGE(x) ((x) < edges.n ? ((int *)edges.p)[x] : -1)
        for (int j = edges.n; j;) {
            int b = EDGE(j - 1), a = EDGE(j - 2);
            j -= 2;
            for (int k = j; k;) {
                int nn = EDGE(k - 1), m = EDGE(k - 2);
                k -= 2;
                if ((a == m && b == nn) || (a == nn && b == m)) {
                    list_remove(&edges, j, 2);
                    list_remove(&edges, k, 2);
                    break;
                }
            }
        }
#undef EDGE
        for (int j = edges.n; j;) {
            int b = ((int *)edges.p)[--j], a = ((int *)edges.p)[--j];
            if (circumcircle(v, a, b, c, &cc)) *(Circle *)list_push(&open) = cc;
        }
    }
    for (int i = open.n; i--;) *(Circle *)list_push(&closed) = ((Circle *)open.p)[i];
    int *res = malloc((size_t)closed.n * 3 * sizeof *res), nres = 0;
    if (res)
        for (int i = closed.n; i--;) {
            Circle *t = (Circle *)closed.p + i;
            if (t->i < n && t->j < n && t->k < n) res[nres++] = t->i, res[nres++] = t->j, res[nres++] = t->k;
        }
    free(open.p), free(closed.p), free(edges.p), free(v), free(indices);
    *out = res, *nout = nres;
}

static void tri_init(Tri *t, const double p1[2], const double p2[2], const double p3[2]) {
    memset(t, 0, sizeof *t);
    t->a[0] = (float)p1[0], t->a[1] = (float)p1[1];
    t->b[0] = (float)p2[0], t->b[1] = (float)p2[1];
    t->c[0] = (float)p3[0], t->c[1] = (float)p3[1];
    for (int i = 0; i < 3; i++) {   /* computeCentroid */
        t->centroid[i] = (float)((double)t->a[i] + t->b[i] + t->c[i]);
        t->centroid[i] = (float)(t->centroid[i] / 3.0);
    }
    float u[3], v[3];   /* computeNormal */
    for (int i = 0; i < 3; i++) u[i] = (float)((double)t->b[i] - t->a[i]), v[i] = (float)((double)t->c[i] - t->a[i]);
    t->normal[0] = (float)((double)u[1] * v[2] - (double)u[2] * v[1]);
    t->normal[1] = (float)((double)u[2] * v[0] - (double)u[0] * v[2]);
    t->normal[2] = (float)((double)u[0] * v[1] - (double)u[1] * v[0]);
    double len = sqrt((double)t->normal[0] * t->normal[0] + (double)t->normal[1] * t->normal[1] + (double)t->normal[2] * t->normal[2]);
    for (int i = 0; i < 3; i++) t->normal[i] = len != 0 ? (float)(t->normal[i] / len) : 0;
}

/* FSS.Plane, width x height, `howmany` random points plus 36 on its border */
static void make_plane(double width, double height, int howmany) {
    free(tris), tris = NULL, ntris = 0;
    plane_w = width, plane_h = height;
    int n = howmany + 8 + 28;
    double (*v)[2] = malloc((size_t)n * sizeof *v);
    if (!v) return;
    double ox = width * -0.5, oy = height * 0.5;
    for (int i = howmany; i--;) {
        double x = ox + sp_random() * width;
        double y = oy - sp_random() * height;
        v[i][0] = x, v[i][1] = y;
    }
    int k = howmany;
    double fixed[8][2] = { { ox, oy }, { ox + width / 2, oy }, { ox + width, oy }, { ox + width, oy - height / 2 },
                           { ox + width, oy - height }, { ox + width / 2, oy - height }, { ox, oy - height }, { ox, oy - height / 2 } };
    for (int i = 0; i < 8; i++) v[k][0] = fixed[i][0], v[k][1] = fixed[i][1], k++;
    for (int i = 6; i >= 0; i--) {
        double r;
        r = sp_random(), v[k][0] = ox + r * width, v[k][1] = oy, k++;
        r = sp_random(), v[k][0] = ox, v[k][1] = oy - r * height, k++;
        r = sp_random(), v[k][0] = ox + width, v[k][1] = oy - r * height, k++;
        r = sp_random(), v[k][0] = ox + r * width, v[k][1] = oy - height, k++;
    }
    int *t, nt;
    triangulate(v, n, &t, &nt);
    tris = malloc((size_t)(nt / 3 + 1) * sizeof *tris);
    if (tris)
        for (int i = nt; i;) {
            double p1[2], p2[2], p3[2];
            --i, p1[0] = ceil(v[t[i]][0]), p1[1] = ceil(v[t[i]][1]);
            --i, p2[0] = ceil(v[t[i]][0]), p2[1] = ceil(v[t[i]][1]);
            --i, p3[0] = ceil(v[t[i]][0]), p3[1] = ceil(v[t[i]][1]);
            tri_init(&tris[ntris++], p1, p2, p3);
        }
    free(t);
    free(v);
}

/* ---- the scene --------------------------------------------------------------------- */

static struct {
    Color mat_ambient, mat_diffuse, light_ambient, light_diffuse;
    float light[3];
    int slices;
} sc;

/* Mesh.update for one light, FRONT side, then the canvas colour */
static C2dColor shade(const Tri *t) {
    float ray[3], col[4] = { 0, 0, 0, 0 }, slave[4];
    for (int i = 0; i < 3; i++) ray[i] = (float)((double)sc.light[i] - t->centroid[i]);
    double len = sqrt((double)ray[0] * ray[0] + (double)ray[1] * ray[1] + (double)ray[2] * ray[2]);
    for (int i = 0; i < 3; i++) ray[i] = len != 0 ? (float)(ray[i] / len) : 0;
    double illum = (double)t->normal[0] * ray[0] + (double)t->normal[1] * ray[1] + (double)t->normal[2] * ray[2];
    illum = fmax(illum, 0);
    for (int i = 0; i < 4; i++) slave[i] = (float)((double)sc.mat_ambient.rgba[i] * sc.light_ambient.rgba[i]);
    for (int i = 0; i < 4; i++) col[i] = (float)((double)col[i] + slave[i]);
    for (int i = 0; i < 4; i++) slave[i] = (float)((double)sc.mat_diffuse.rgba[i] * sc.light_diffuse.rgba[i]);
    for (int i = 0; i < 4; i++) slave[i] = (float)(slave[i] * illum);
    for (int i = 0; i < 4; i++) col[i] = (float)((double)col[i] + slave[i]);
    for (int i = 0; i < 4; i++) col[i] = col[i] < 0 ? 0 : col[i] > 1 ? 1 : col[i];
    return C2D_RGBA(hexify(col[0]), hexify(col[1]), hexify(col[2]), 1);
}

static bool dirty = true, regen;

static void set_property(const SpMessage *m) {
    const char *n = m->name;
    if (!strcmp(n, "meshAmbient")) color_set(&sc.mat_ambient, m->value);
    else if (!strcmp(n, "meshDiffuse")) color_set(&sc.mat_diffuse, m->value);
    else if (!strcmp(n, "meshSlices")) {
        int s = (int)sp_num(m);
        if (s != sc.slices) sc.slices = s, regen = true;
    } else if (!strcmp(n, "lightAmbient")) color_set(&sc.light_ambient, m->value);
    else if (!strcmp(n, "lightDiffuse")) color_set(&sc.light_diffuse, m->value);
    else if (!strcmp(n, "lightDistance")) sc.light[2] = (float)sp_num(m);
    else return;
    dirty = true;
}

int main(int argc, char **argv) {
    SpArgs sp = sp_args(argc, argv);
    if (!bare_window_with(.width = sp.width ? sp.width : 1280, .height = sp.height ? sp.height : 720, .title = "Triangles & Light", .parent = sp.parent)) return 1;
    bare_background(BARE_RGB(0x15, 0x16, 0x18));
    C2d c;
    c2d_init(&c, bare_shader(&canvas2d_shader));
    /* the page's own until Screen+ sends the panel's */
    for (int i = 0; i < 4; i++)
        sc.mat_ambient.rgba[i] = sc.mat_diffuse.rgba[i] = sc.light_ambient.rgba[i] = sc.light_diffuse.rgba[i] = 1;
    color_set(&sc.mat_ambient, "#555555"), color_set(&sc.mat_diffuse, "#FFFFFF");
    color_set(&sc.light_ambient, "#880066"), color_set(&sc.light_diffuse, "#FF8800");
    sc.light[0] = 0, sc.light[1] = 200, sc.light[2] = 100;
    sc.slices = 250;
    BareImage canvas = 0;
    int cw = 0, ch = 0;
    bool moved = false;
    BareVec2 last_mouse = { -1e9f, -1e9f };

    while (bare_frame()) {
        BareVec2 win = bare_window_size();
        int w = (int)fmax(1, floor(win.x + 0.5)), h = (int)fmax(1, floor(win.y + 0.5));
        if (w != cw || h != ch) {   /* the page makes its mesh twice as it starts (createMesh, resize) */
            if (canvas) bare_image_free(canvas);
            canvas = bare_canvas(.width = w, .height = h);
            if (!cw) make_plane(1.2 * w, 1.2 * h, sc.slices);
            make_plane(1.2 * w, 1.2 * h, sc.slices);
            cw = w, ch = h, dirty = true;
        }
        SpMessage m;
        while (sp_next(&m)) {
            if (m.kind == SP_PROPERTY) set_property(&m);
            if (regen) make_plane(1.2 * w, 1.2 * h, sc.slices), regen = false;
        }
        BareVec2 mo = bare_mouse();   /* the light follows the mouse once it moves */
        if (mo.x != last_mouse.x || mo.y != last_mouse.y) {
            if (last_mouse.x > -1e8f) moved = true;
            last_mouse = mo;
            if (moved) sc.light[0] = (float)(mo.x - w / 2.0), sc.light[1] = (float)(h / 2.0 - mo.y), dirty = true;
        }
        bare_box(.width = BARE_GROW, .height = BARE_GROW, .image = canvas);
        if (dirty) {
            double hw = w * 0.5, hh = h * 0.5;
            for (int t = ntris - 1; t >= 0; t--) {
                const Tri *tr = &tris[t];
                c2d_triangle(&c, (float)(tr->a[0] + hw), (float)(hh - tr->a[1]), (float)(tr->b[0] + hw), (float)(hh - tr->b[1]),
                             (float)(tr->c[0] + hw), (float)(hh - tr->c[1]), 0.5f, shade(tr));
            }
            static const float bg[4] = { 0x15 / 255.f, 0x16 / 255.f, 0x18 / 255.f, 1 };
            c2d_flush(&c, canvas, w, h, true, bg);
            dirty = false;
        }
        bare_continuous(false);
    }
    bare_quit();
}
