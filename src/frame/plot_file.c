#include "frame/plot_file.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum : uint32_t {
    k_text_cap  = 512u * 1024u,
    k_obj_verts = 8192u,
    k_line_idx  = 64u,
};

static char  s_text[k_text_cap];
static float s_vx[k_obj_verts];
static float s_vy[k_obj_verts];
static float s_vz[k_obj_verts];

static bool resolve_plot(const char *name, char *out, size_t cap) {
    int cwd = snprintf(out, cap, "plots/%s", name);
    if (cwd > 0 && (size_t)cwd < cap && access(out, R_OK) == 0) {
        return true;
    }
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1u);
    if (n <= 0 || (size_t)n >= sizeof exe) {
        return false;
    }
    exe[n] = '\0';
    for (uint32_t up = 0; up < 6u; ++up) {
        char *slash = strrchr(exe, '/');
        if (slash == nullptr || slash == exe) {
            break;
        }
        *slash = '\0';
        int wrote = snprintf(out, cap, "%s/plots/%s", exe, name);
        if (wrote > 0 && (size_t)wrote < cap && access(out, R_OK) == 0) {
            return true;
        }
    }
    return false;
}

static const char *next_line(const char **pp, const char *end, const char **line_end) {
    const char *p = *pp;
    const char *line = p;
    while (p < end && *p != '\n' && *p != '\r') {
        p++;
    }
    *line_end = p;
    while (p < end && (*p == '\n' || *p == '\r')) {
        p++;
    }
    *pp = p;
    return line;
}

static const char *skip_space(const char *s, const char *end) {
    while (s < end && (*s == ' ' || *s == '\t')) {
        s++;
    }
    return s;
}

static bool parse_float(const char **pp, const char *end, float *out) {
    const char *p = *pp;
    while (p < end && (*p == ' ' || *p == '\t' || *p == ',' || *p == ';')) {
        p++;
    }
    if (p >= end) {
        return false;
    }
    char *stop = nullptr;
    float v = strtof(p, &stop);
    if (stop == p || !isfinite(v)) {
        return false;
    }
    *out = v;
    *pp = stop;
    return true;
}

static bool parse_index(const char **pp, const char *end, int *out) {
    const char *p = *pp;
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }
    if (p >= end || *p == '#' || *p == '\n' || *p == '\r') {
        return false;
    }
    char *stop = nullptr;
    long v = strtol(p, &stop, 10);
    if (stop == p) {
        return false;
    }
    if (*stop == '/') {
        stop++;
        (void)strtol(stop, &stop, 10);
        if (*stop == '/') {
            stop++;
            (void)strtol(stop, &stop, 10);
        }
    }
    *out = (int)v;
    *pp = stop;
    return true;
}

static int index_of(int idx, uint32_t nverts) {
    if (idx > 0) {
        return idx - 1;
    }
    if (idx < 0) {
        return (int)nverts + idx;
    }
    return -1;
}

static bool emit_point(PlotSample *dst, uint32_t cap, uint32_t *n,
                       float x, float y, float z, float brk) {
    if (*n >= cap) {
        return false;
    }
    dst[*n] = (PlotSample){ x, y, z, brk };
    (*n)++;
    return true;
}

static bool emit_vert(PlotSample *dst, uint32_t cap, uint32_t nverts, uint32_t *n,
                      int idx, float brk) {
    int i = index_of(idx, nverts);
    if (i < 0 || (uint32_t)i >= nverts) {
        return true;
    }
    return emit_point(dst, cap, n, s_vx[i], s_vy[i], s_vz[i], brk);
}

static uint32_t parse_csv(PlotSample *dst, uint32_t cap, const char *text, size_t len,
                          uint32_t *n, bool *capped) {
    const char *p = text;
    const char *end = text + len;
    uint32_t rows = 0;
    bool first = true;
    while (p < end) {
        const char *line_end = nullptr;
        const char *line = next_line(&p, end, &line_end);
        const char *s = skip_space(line, line_end);
        if (s >= line_end || *s == '#') {
            continue;
        }
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        const char *q = s;
        if (!parse_float(&q, line_end, &x) || !parse_float(&q, line_end, &y)) {
            continue;
        }
        if (!parse_float(&q, line_end, &z)) {
            z = 0.0f;
        }
        if (!emit_point(dst, cap, n, x, y, z, first ? 1.0f : 0.0f)) {
            *capped = true;
            break;
        }
        first = false;
        rows++;
    }
    return rows;
}

static uint32_t parse_obj(PlotSample *dst, uint32_t cap, const char *text, size_t len,
                          uint32_t *n, bool *capped) {
    uint32_t nverts = 0;
    uint32_t links = 0;
    bool any_chain = false;
    const char *p = text;
    const char *end = text + len;
    while (p < end) {
        const char *line_end = nullptr;
        const char *line = next_line(&p, end, &line_end);
        const char *s = skip_space(line, line_end);
        if (s >= line_end || *s == '#') {
            continue;
        }
        if (s + 1 < line_end && s[0] == 'v' && (s[1] == ' ' || s[1] == '\t')) {
            const char *q = s + 1;
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            if (!parse_float(&q, line_end, &x) || !parse_float(&q, line_end, &y)) {
                continue;
            }
            if (!parse_float(&q, line_end, &z)) {
                z = 0.0f;
            }
            if (nverts < k_obj_verts) {
                s_vx[nverts] = x;
                s_vy[nverts] = y;
                s_vz[nverts] = z;
                nverts++;
            } else {
                *capped = true;
            }
            continue;
        }
        bool is_l = s + 1 < line_end && s[0] == 'l' && (s[1] == ' ' || s[1] == '\t');
        bool is_f = s + 1 < line_end && s[0] == 'f' && (s[1] == ' ' || s[1] == '\t');
        // A face belongs to the mesh. Marking the chain keeps a faces-only
        // file from also becoming a polyline through every vertex.
        if (is_f) {
            any_chain = true;
            continue;
        }
        if (!is_l) {
            continue;
        }
        any_chain = true;
        const char *q = s + 1;
        int idx[k_line_idx];
        uint32_t ni = 0;
        int one = 0;
        while (ni < k_line_idx && parse_index(&q, line_end, &one)) {
            idx[ni++] = one;
        }
        if (ni == k_line_idx && parse_index(&q, line_end, &one)) {
            *capped = true;
        }
        if (ni < 2u) {
            continue;
        }
        for (uint32_t i = 0; i < ni; ++i) {
            if (!emit_vert(dst, cap, nverts, n, idx[i], i == 0u ? 1.0f : 0.0f)) {
                *capped = true;
                return links;
            }
        }
        links++;
    }
    uint32_t made = links;
    if (!any_chain && nverts >= 2u) {
        for (uint32_t i = 0; i < nverts; ++i) {
            if (!emit_point(dst, cap, n, s_vx[i], s_vy[i], s_vz[i], i == 0u ? 1.0f : 0.0f)) {
                *capped = true;
                break;
            }
        }
        made = 1u;
    }
    return made;
}

static int read_text(const char *path, size_t *out_len, RingCqeFn on_cqe, void *on_user) {
    RingContext *ring = ring_get_context();
    if (ring == nullptr) {
        return -EINVAL;
    }
    size_t n = 0;
    int rc = ring_read_file_all(ring, path, s_text, k_text_cap, &n, on_cqe, on_user);
    if (rc == 0 && out_len != nullptr) {
        *out_len = n;
    }
    return rc;
}

static void note_blend(void) {
    char path[1024];
    if (!resolve_plot("mesh.blend", path, sizeof path)) {
        return;
    }
    printf("[Plot] A .blend file is Blender's database. Export an OBJ to plots/mesh.obj.\n");
    fflush(stdout);
}

enum : uint32_t { k_kind_csv = 0u, k_kind_obj = 1u };

static bool load_one(const char *leaf, const char *missing, uint32_t kind,
                     uint32_t (*parse)(PlotSample *, uint32_t, const char *, size_t, uint32_t *, bool *),
                     PlotSample *dst, uint32_t cap, uint32_t *n, bool *capped,
                     RingCqeFn on_cqe, void *on_user) {
    char path[1024];
    if (!resolve_plot(leaf, path, sizeof path)) {
        printf("%s\n", missing);
        fflush(stdout);
        return false;
    }
    size_t len = 0;
    int rc = read_text(path, &len, on_cqe, on_user);
    if (rc != 0) {
        fprintf(stderr, "[Plot Error] %s: %s\n", path, strerror(rc < 0 ? -rc : rc));
        return false;
    }
    uint32_t before = *n;
    uint32_t made = parse(dst, cap, s_text, len, n, capped);
    if (kind == k_kind_csv) {
        printf("[Plot] CSV %u points from %s.\n", made, path);
    } else {
        printf("[Plot] OBJ %u points, %u chains, from %s.\n", *n - before, made, path);
    }
    fflush(stdout);
    return true;
}

uint32_t plot_file_load(PlotSample *dst, uint32_t cap, bool *found, bool *capped,
                        RingCqeFn on_cqe, void *on_user) {
    if (found != nullptr) {
        *found = false;
    }
    if (capped != nullptr) {
        *capped = false;
    }
    if (dst == nullptr || cap == 0u) {
        return 0;
    }
    bool hit_cap = false;
    note_blend();
    uint32_t n = 0;
    bool csv = load_one("data.csv", "[Plot] plots/data.csv was not found.", k_kind_csv,
                        parse_csv, dst, cap, &n, &hit_cap, on_cqe, on_user);
    bool obj = load_one("mesh.obj", "[Plot] plots/mesh.obj was not found.", k_kind_obj,
                        parse_obj, dst, cap, &n, &hit_cap, on_cqe, on_user);
    if (found != nullptr) {
        *found = csv || obj;
    }
    if (hit_cap) {
        printf("[Plot] Point cap is %u. The rest was left out.\n", cap);
        fflush(stdout);
        if (capped != nullptr) {
            *capped = true;
        }
    }
    return n;
}

// Face clip for the OBJ mesh. The click-face clip in world.c uses a 16 px
// minimum, which drops a unit-sized file face. This one scales with the face.
static float s_nx[k_obj_verts];
static float s_ny[k_obj_verts];
static float s_nz[k_obj_verts];
static uint32_t s_tri_idx[k_obj_tri_cap * 3u];

static const float k_clay_r = 0.76f;
static const float k_clay_g = 0.58f;
static const float k_clay_b = 0.42f;

static float cross2(float ax, float ay, float bx, float by, float cx, float cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

static bool segments_cross(float ax, float ay, float bx, float by,
                           float cx, float cy, float dx, float dy) {
    float a1 = cross2(ax, ay, bx, by, cx, cy);
    float a2 = cross2(ax, ay, bx, by, dx, dy);
    float a3 = cross2(cx, cy, dx, dy, ax, ay);
    float a4 = cross2(cx, cy, dx, dy, bx, by);
    return (a1 * a2 < 0.0f) && (a3 * a4 < 0.0f);
}

static bool self_cross(const float *x, const float *y, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t i2 = (i + 1u) % n;
        for (uint32_t j = i + 1u; j < n; ++j) {
            uint32_t j2 = (j + 1u) % n;
            if (i2 == j || i == j2 || i2 == j2) {
                continue;
            }
            if (segments_cross(x[i], y[i], x[i2], y[i2], x[j], y[j], x[j2], y[j2])) {
                return true;
            }
        }
    }
    return false;
}

static float poly_span(const float *x, const float *y, uint32_t n) {
    float m = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        float ax = fabsf(x[i]);
        float ay = fabsf(y[i]);
        if (ax > m) {
            m = ax;
        }
        if (ay > m) {
            m = ay;
        }
    }
    return m;
}

static float rel_eps(float span) {
    float e = span * span * 1.0e-10f;
    if (e < 1.0e-20f) {
        e = 1.0e-20f;
    }
    return e;
}

static bool plane_normal(const float *vx, const float *vy, const float *vz,
                         const uint32_t *poly, uint32_t n,
                         float *nx, float *ny, float *nz) {
    double anx = 0.0;
    double any = 0.0;
    double anz = 0.0;
    float reach = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t a = poly[i];
        uint32_t b = poly[(i + 1u) % n];
        float ax = vx[a];
        float ay = vy[a];
        float az = vz[a];
        float bx = vx[b];
        float by = vy[b];
        float bz = vz[b];
        anx += (double)(ay - by) * (double)(az + bz);
        any += (double)(az - bz) * (double)(ax + bx);
        anz += (double)(ax - bx) * (double)(ay + by);
        float dx = ax - bx;
        float dy = ay - by;
        float dz = az - bz;
        float len = sqrtf(dx * dx + dy * dy + dz * dz);
        if (len > reach) {
            reach = len;
        }
    }
    double alen = sqrt(anx * anx + any * any + anz * anz);
    double gate = (double)reach * (double)reach * 1.0e-8;
    if (gate < 1.0e-20) {
        gate = 1.0e-20;
    }
    if (alen > gate) {
        *nx = (float)(anx / alen);
        *ny = (float)(any / alen);
        *nz = (float)(anz / alen);
        return true;
    }
    for (uint32_t i = 0; i < n; ++i) {
        for (uint32_t j = i + 1u; j < n; ++j) {
            for (uint32_t k = j + 1u; k < n; ++k) {
                float ex = vx[poly[j]] - vx[poly[i]];
                float ey = vy[poly[j]] - vy[poly[i]];
                float ez = vz[poly[j]] - vz[poly[i]];
                float fx = vx[poly[k]] - vx[poly[i]];
                float fy = vy[poly[k]] - vy[poly[i]];
                float fz = vz[poly[k]] - vz[poly[i]];
                float cx = ey * fz - ez * fy;
                float cy = ez * fx - ex * fz;
                float cz = ex * fy - ey * fx;
                float cl = sqrtf(cx * cx + cy * cy + cz * cz);
                if (cl <= 1.0e-12f) {
                    continue;
                }
                *nx = cx / cl;
                *ny = cy / cl;
                *nz = cz / cl;
                return true;
            }
        }
    }
    return false;
}

static bool project_poly(const float *vx, const float *vy, const float *vz,
                         const uint32_t *poly, uint32_t n,
                         float *px, float *py) {
    float nx = 0.0f;
    float ny = 0.0f;
    float nz = 0.0f;
    if (!plane_normal(vx, vy, vz, poly, n, &nx, &ny, &nz)) {
        return false;
    }
    float tx = 1.0f;
    float ty = 0.0f;
    float tz = 0.0f;
    if (fabsf(nx) > 0.9f) {
        tx = 0.0f;
        ty = 1.0f;
        tz = 0.0f;
    }
    float ux = ny * tz - nz * ty;
    float uy = nz * tx - nx * tz;
    float uz = nx * ty - ny * tx;
    float ul = sqrtf(ux * ux + uy * uy + uz * uz);
    if (ul < 1.0e-12f) {
        return false;
    }
    ux /= ul;
    uy /= ul;
    uz /= ul;
    float vxu = ny * uz - nz * uy;
    float vyu = nz * ux - nx * uz;
    float vzu = nx * uy - ny * ux;
    float ox = vx[poly[0]];
    float oy = vy[poly[0]];
    float oz = vz[poly[0]];
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t a = poly[i];
        float dx = vx[a] - ox;
        float dy = vy[a] - oy;
        float dz = vz[a] - oz;
        px[i] = dx * ux + dy * uy + dz * uz;
        py[i] = dx * vxu + dy * vyu + dz * vzu;
    }
    return true;
}

static bool ear_clip_rel(const float *x, const float *y, uint32_t n,
                         uint32_t *idx, uint32_t *tri_n, float eps) {
    if (n < 3u || n > k_line_idx || idx == nullptr || tri_n == nullptr) {
        return false;
    }
    float area2 = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t j = (i + 1u) % n;
        area2 += x[i] * y[j] - x[j] * y[i];
    }
    if (fabsf(area2 * 0.5f) <= eps) {
        return false;
    }
    float orient = area2 > 0.0f ? 1.0f : -1.0f;
    float inside = eps * 0.1f;
    uint32_t v[k_line_idx];
    for (uint32_t i = 0; i < n; ++i) {
        v[i] = i;
    }
    uint32_t left = n;
    uint32_t out = 0;
    uint32_t guard = 0;
    uint32_t out_cap = (n - 2u) * 3u;
    while (left > 3u) {
        if (guard++ > n * n) {
            return false;
        }
        bool clipped = false;
        for (uint32_t i = 0; i < left; ++i) {
            uint32_t ip = (i + left - 1u) % left;
            uint32_t in = (i + 1u) % left;
            uint32_t a = v[ip];
            uint32_t b = v[i];
            uint32_t c = v[in];
            float cr = cross2(x[a], y[a], x[b], y[b], x[c], y[c]);
            if (cr * orient <= eps) {
                continue;
            }
            bool ear = true;
            for (uint32_t k = 0; k < left; ++k) {
                if (k == ip || k == i || k == in) {
                    continue;
                }
                uint32_t p = v[k];
                float c1 = cross2(x[a], y[a], x[b], y[b], x[p], y[p]);
                float c2 = cross2(x[b], y[b], x[c], y[c], x[p], y[p]);
                float c3 = cross2(x[c], y[c], x[a], y[a], x[p], y[p]);
                if (c1 * orient > inside && c2 * orient > inside && c3 * orient > inside) {
                    ear = false;
                    break;
                }
            }
            if (!ear) {
                continue;
            }
            if (out + 3u > out_cap) {
                return false;
            }
            idx[out++] = a;
            idx[out++] = b;
            idx[out++] = c;
            for (uint32_t k = i + 1u; k < left; ++k) {
                v[k - 1u] = v[k];
            }
            left--;
            clipped = true;
            break;
        }
        if (!clipped) {
            return false;
        }
    }
    if (out + 3u > out_cap) {
        return false;
    }
    idx[out++] = v[0];
    idx[out++] = v[1];
    idx[out++] = v[2];
    *tri_n = out / 3u;
    return *tri_n == n - 2u;
}

// File winding is kept. A clockwise face stays clockwise and the rasterizer
// culls it from the front view. Returns the number of triangles appended.
static uint32_t add_polygon(const float *vx, const float *vy, const float *vz,
                            const uint32_t *poly, uint32_t n,
                            uint32_t *tris, uint32_t *tri_n, uint32_t tri_cap,
                            bool *crossed, bool *capped) {
    if (n < 3u || n > k_line_idx || vx == nullptr || poly == nullptr ||
        tris == nullptr || tri_n == nullptr) {
        return 0;
    }
    float px[k_line_idx];
    float py[k_line_idx];
    if (!project_poly(vx, vy, vz, poly, n, px, py)) {
        return 0;
    }
    float eps = rel_eps(poly_span(px, py, n));
    if (n > 3u && self_cross(px, py, n)) {
        if (crossed != nullptr) {
            *crossed = true;
        }
        return 0;
    }
    uint32_t local[(k_line_idx - 2u) * 3u];
    uint32_t tn = 0;
    if (n == 3u) {
        float area2 = px[0] * py[1] - px[1] * py[0] + px[1] * py[2] - px[2] * py[1] +
                      px[2] * py[0] - px[0] * py[2];
        if (fabsf(area2 * 0.5f) <= eps) {
            return 0;
        }
        local[0] = 0;
        local[1] = 1;
        local[2] = 2;
        tn = 1u;
    } else if (!ear_clip_rel(px, py, n, local, &tn, eps)) {
        return 0;
    }
    if (*tri_n + tn > tri_cap) {
        if (capped != nullptr) {
            *capped = true;
        }
        return 0;
    }
    uint32_t base = *tri_n * 3u;
    for (uint32_t i = 0; i < tn * 3u; ++i) {
        tris[base + i] = poly[local[i]];
    }
    *tri_n += tn;
    return tn;
}

static bool tri_faces_up(const float *vx, const float *vy, const float *vz,
                         uint32_t i0, uint32_t i1, uint32_t i2) {
    float ex = vx[i1] - vx[i0];
    float ey = vy[i1] - vy[i0];
    float ez = vz[i1] - vz[i0];
    float fx = vx[i2] - vx[i0];
    float fy = vy[i2] - vy[i0];
    float fz = vz[i2] - vz[i0];
    float cx = ey * fz - ez * fy;
    float cy = ez * fx - ex * fz;
    float cz = ex * fy - ey * fx;
    float len = sqrtf(cx * cx + cy * cy + cz * cz);
    if (len < 1.0e-8f) {
        return false;
    }
    return fabsf(cx / len) < 1.0e-3f && fabsf(cy / len) < 1.0e-3f && (cz / len) > 0.9f;
}

bool plot_file_mesh_check(void) {
    float vx[4];
    float vy[4];
    float vz[4];
    uint32_t poly[4];
    uint32_t tris[6];
    uint32_t ntri = 0;
    bool crossed = false;
    bool capped = false;

    vx[0] = 0.0f;
    vy[0] = 0.0f;
    vz[0] = 0.0f;
    vx[1] = 1.0f;
    vy[1] = 0.0f;
    vz[1] = 0.0f;
    vx[2] = 0.0f;
    vy[2] = 1.0f;
    vz[2] = 0.0f;
    poly[0] = 0;
    poly[1] = 1;
    poly[2] = 2;
    uint32_t got = add_polygon(vx, vy, vz, poly, 3u, tris, &ntri, 2u, &crossed, &capped);
    if (got != 1u || ntri != 1u || crossed || capped ||
        !tri_faces_up(vx, vy, vz, tris[0], tris[1], tris[2])) {
        return false;
    }

    ntri = 0;
    crossed = false;
    vx[0] = 0.0f;
    vy[0] = 0.0f;
    vz[0] = 0.0f;
    vx[1] = 1.0f;
    vy[1] = 0.0f;
    vz[1] = 0.0f;
    vx[2] = 1.0f;
    vy[2] = 1.0f;
    vz[2] = 0.0f;
    vx[3] = 0.0f;
    vy[3] = 1.0f;
    vz[3] = 0.0f;
    poly[3] = 3;
    got = add_polygon(vx, vy, vz, poly, 4u, tris, &ntri, 2u, &crossed, &capped);
    if (got != 2u || ntri != 2u || crossed || capped ||
        !tri_faces_up(vx, vy, vz, tris[0], tris[1], tris[2]) ||
        !tri_faces_up(vx, vy, vz, tris[3], tris[4], tris[5])) {
        return false;
    }

    ntri = 0;
    crossed = false;
    vx[0] = 0.0f;
    vy[0] = 0.0f;
    vz[0] = 0.0f;
    vx[1] = 1.0f;
    vy[1] = 1.0f;
    vz[1] = 0.0f;
    vx[2] = 0.0f;
    vy[2] = 1.0f;
    vz[2] = 0.0f;
    vx[3] = 1.0f;
    vy[3] = 0.0f;
    vz[3] = 0.0f;
    got = add_polygon(vx, vy, vz, poly, 4u, tris, &ntri, 2u, &crossed, &capped);
    return got == 0u && ntri == 0u;
}

static uint32_t parse_mesh_faces(const char *text, size_t len, uint32_t *nverts,
                                 uint32_t tri_cap, bool *capped, bool *crossed) {
    uint32_t count = 0;
    uint32_t ntri = 0;
    const char *p = text;
    const char *end = text + len;
    while (p < end) {
        const char *line_end = nullptr;
        const char *line = next_line(&p, end, &line_end);
        const char *s = skip_space(line, line_end);
        if (s >= line_end || *s == '#') {
            continue;
        }
        if (s + 1 < line_end && s[0] == 'v' && (s[1] == ' ' || s[1] == '\t')) {
            const char *q = s + 1;
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            if (!parse_float(&q, line_end, &x) || !parse_float(&q, line_end, &y)) {
                continue;
            }
            if (!parse_float(&q, line_end, &z)) {
                z = 0.0f;
            }
            if (count < k_obj_verts) {
                s_vx[count] = x;
                s_vy[count] = y;
                s_vz[count] = z;
                count++;
            }
            continue;
        }
        if (!(s + 1 < line_end && s[0] == 'f' && (s[1] == ' ' || s[1] == '\t'))) {
            continue;
        }
        const char *q = s + 1;
        uint32_t poly[k_line_idx];
        uint32_t ni = 0;
        bool bad = false;
        bool overflow = false;
        int one = 0;
        while (parse_index(&q, line_end, &one)) {
            if (ni == k_line_idx) {
                overflow = true;
                break;
            }
            int vi = index_of(one, count);
            if (vi < 0 || (uint32_t)vi >= count) {
                bad = true;
                break;
            }
            poly[ni++] = (uint32_t)vi;
        }
        if (bad || overflow || ni < 3u) {
            continue;
        }
        (void)add_polygon(s_vx, s_vy, s_vz, poly, ni, s_tri_idx, &ntri, tri_cap, crossed, capped);
        if (capped != nullptr && *capped) {
            break;
        }
    }
    *nverts = count;
    return ntri;
}

static void write_mesh(ObjCorner *dst, uint32_t nverts, uint32_t ntri) {
    if (nverts > k_obj_verts) {
        nverts = k_obj_verts;
    }
    memset(s_nx, 0, (size_t)nverts * sizeof(float));
    memset(s_ny, 0, (size_t)nverts * sizeof(float));
    memset(s_nz, 0, (size_t)nverts * sizeof(float));
    for (uint32_t t = 0; t < ntri; ++t) {
        uint32_t i0 = s_tri_idx[t * 3u];
        uint32_t i1 = s_tri_idx[t * 3u + 1u];
        uint32_t i2 = s_tri_idx[t * 3u + 2u];
        if (i0 >= nverts || i1 >= nverts || i2 >= nverts) {
            continue;
        }
        float ex = s_vx[i1] - s_vx[i0];
        float ey = s_vy[i1] - s_vy[i0];
        float ez = s_vz[i1] - s_vz[i0];
        float fx = s_vx[i2] - s_vx[i0];
        float fy = s_vy[i2] - s_vy[i0];
        float fz = s_vz[i2] - s_vz[i0];
        float cx = ey * fz - ez * fy;
        float cy = ez * fx - ex * fz;
        float cz = ex * fy - ey * fx;
        s_nx[i0] += cx;
        s_ny[i0] += cy;
        s_nz[i0] += cz;
        s_nx[i1] += cx;
        s_ny[i1] += cy;
        s_nz[i1] += cz;
        s_nx[i2] += cx;
        s_ny[i2] += cy;
        s_nz[i2] += cz;
    }
    for (uint32_t c = 0; c < ntri * 3u; ++c) {
        uint32_t vi = s_tri_idx[c];
        float nx = 0.0f;
        float ny = 0.0f;
        float nz = 1.0f;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (vi < nverts) {
            x = s_vx[vi];
            y = s_vy[vi];
            z = s_vz[vi];
            nx = s_nx[vi];
            ny = s_ny[vi];
            nz = s_nz[vi];
            float len = sqrtf(nx * nx + ny * ny + nz * nz);
            if (len < 1.0e-12f) {
                nx = 0.0f;
                ny = 0.0f;
                nz = 1.0f;
            } else {
                nx /= len;
                ny /= len;
                nz /= len;
            }
        }
        dst[c] = (ObjCorner){
            .x = x, .y = y, .z = z, .nx = nx, .ny = ny, .nz = nz,
            .r = k_clay_r, .g = k_clay_g, .b = k_clay_b,
        };
    }
}

int plot_file_load_mesh(ObjCorner *dst, uint32_t corner_cap, bool *found, bool *capped,
                        bool *crossed, RingCqeFn on_cqe, void *on_user) {
    if (found != nullptr) {
        *found = false;
    }
    if (capped != nullptr) {
        *capped = false;
    }
    if (crossed != nullptr) {
        *crossed = false;
    }
    if (dst == nullptr || corner_cap < 3u) {
        return 0;
    }
    char path[1024];
    if (!resolve_plot("mesh.obj", path, sizeof path)) {
        return 0;
    }
    size_t len = 0;
    int rc = read_text(path, &len, on_cqe, on_user);
    if (rc != 0) {
        fprintf(stderr, "[Mesh Error] %s: %s\n", path, strerror(rc < 0 ? -rc : rc));
        return -1;
    }
    if (found != nullptr) {
        *found = true;
    }
    bool hit = false;
    bool cross = false;
    uint32_t tri_cap = corner_cap / 3u;
    if (tri_cap > k_obj_tri_cap) {
        tri_cap = k_obj_tri_cap;
    }
    uint32_t nverts = 0;
    uint32_t ntri = parse_mesh_faces(s_text, len, &nverts, tri_cap, &hit, &cross);
    if (capped != nullptr) {
        *capped = hit;
    }
    if (crossed != nullptr) {
        *crossed = cross;
    }
    if (ntri == 0u) {
        return 0;
    }
    write_mesh(dst, nverts, ntri);
    return (int)(ntri * 3u);
}
