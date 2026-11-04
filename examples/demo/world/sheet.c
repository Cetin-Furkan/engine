#include "frame/sheet.h"

#include "frame/material.h"
#include "frame/shape.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// CPU sheet. Boxes meet on their edges. A soft patch is a mass-spring grid,
// not the 128³ lattice. A dyn record is a convex polygon. Nothing here
// submits a compute dispatch. The shape pipeline fills the polygons.

enum : uint32_t {
    k_rigid_cap = 16u,
    k_soft_cap  = 2u,
    k_dyn_cap   = 8u,
    k_soft_side = 8u,
    k_soft_pts  = 64u,
    k_dyn_verts = 12u,
    k_passes    = 4u,
};

static const float k_deg       = 0.017453292519943295f;
static const float k_shove     = 640.0f;
static const float k_damp      = 1.8f;
static const float k_spring    = 48.0f;
static const float k_rel       = 6.0f;
static const float k_soft_damp = 2.2f;
static const float k_sleep     = 8.0f;
static const float k_slop      = 0.5f;
static const float k_speed_cap = 4000.0f;
static const float k_strain    = 0.75f;
static const float k_stroke    = 1.25f;

static const char *k_doc     = "examples/demo/world/sheet.world";
static const char *k_doc_old = "world/sheet.world";
static const char *k_anchor  = "examples/demo/world/opening.world";
static const char *k_anchor_old = "world/opening.world";

static const char *k_seed =
    "rigid -220 80 90 50 0\n"
    "rigid 220 80 90 50 18\n"
    "soft 0 -180 6 4 36\n"
    "dyn 0 280 0 4\n"
    "48 0\n"
    "0 40\n"
    "-48 0\n"
    "0 -36\n";

typedef struct Rigid {
    float x, y, hx, hy, yaw;
    float vx, vy;
    bool  alive;
    bool  awake;
} Rigid;

typedef struct Soft {
    float    x[k_soft_pts];
    float    y[k_soft_pts];
    float    vx[k_soft_pts];
    float    vy[k_soft_pts];
    float    rx[k_soft_pts];
    float    ry[k_soft_pts];
    float    ox, oy, spacing;
    uint32_t cols, rows;
    bool     alive;
    bool     awake;
} Soft;

typedef struct Dyn {
    float    lx[k_dyn_verts];
    float    ly[k_dyn_verts];
    float    x, y, yaw;
    float    vx, vy;
    uint32_t n;
    bool     alive;
    bool     awake;
} Dyn;

typedef struct Sheet {
    Rigid rigid[k_rigid_cap];
    Soft  soft[k_soft_cap];
    Dyn   dyn[k_dyn_cap];
} Sheet;

typedef struct Poly {
    float    x[k_dyn_verts];
    float    y[k_dyn_verts];
    uint32_t n;
    float   *cx;
    float   *cy;
    float   *vx;
    float   *vy;
    bool    *awake;
} Poly;

enum : int { k_sel_none = 0, k_sel_rigid = 1, k_sel_soft = 2, k_sel_dyn = 3 };

static Sheet   s_sheet;
static bool    s_hold_save = false;
static bool    s_ready = false;
static bool    s_moved = false;
static bool    s_drew  = false;
static int     s_sel   = k_sel_none;
static uint32_t s_sel_i = 0;

static void mark_moved(void) {
    s_moved = true;
    s_drew = true;
}

static bool resolve_file(const char *name, char *out, size_t cap) {
    int cwd = snprintf(out, cap, "%s", name);
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
        int wrote = snprintf(out, cap, "%s/%s", exe, name);
        if (wrote > 0 && (size_t)wrote < cap && access(out, R_OK) == 0) {
            return true;
        }
    }
    return false;
}

static bool sibling(const char *leaf, char *out, size_t cap) {
    char anchor[1024];
    if (!resolve_file(k_doc, anchor, sizeof anchor) &&
        !resolve_file(k_doc_old, anchor, sizeof anchor) &&
        !resolve_file(k_anchor, anchor, sizeof anchor) &&
        !resolve_file(k_anchor_old, anchor, sizeof anchor)) {
        return false;
    }
    char *slash = strrchr(anchor, '/');
    int wrote = 0;
    if (slash == nullptr) {
        wrote = snprintf(out, cap, "%s", leaf);
    } else {
        *slash = '\0';
        wrote = snprintf(out, cap, "%s/%s", anchor, leaf);
    }
    return wrote > 0 && (size_t)wrote < cap;
}

static void trim(char *s) {
    char *hash = strchr(s, '#');
    if (hash != nullptr) {
        *hash = '\0';
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static bool finite2(float x, float y) {
    return isfinite(x) && isfinite(y);
}

static uint32_t count_alive_rigid(const Sheet *s) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < k_rigid_cap; ++i) {
        if (s->rigid[i].alive) {
            n++;
        }
    }
    return n;
}

static uint32_t count_alive_soft(const Sheet *s) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < k_soft_cap; ++i) {
        if (s->soft[i].alive) {
            n++;
        }
    }
    return n;
}

static uint32_t count_alive_dyn(const Sheet *s) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < k_dyn_cap; ++i) {
        if (s->dyn[i].alive) {
            n++;
        }
    }
    return n;
}

static int add_rigid(Sheet *s, float x, float y, float hx, float hy, float yaw_deg) {
    if (!finite2(x, y) || !isfinite(hx) || !isfinite(hy) || !isfinite(yaw_deg) ||
        hx < 1.0f || hy < 1.0f || hx > 4000.0f || hy > 4000.0f) {
        return -1;
    }
    for (uint32_t i = 0; i < k_rigid_cap; ++i) {
        if (s->rigid[i].alive) {
            continue;
        }
        s->rigid[i] = (Rigid){
            .x = x, .y = y, .hx = hx, .hy = hy, .yaw = yaw_deg * k_deg,
            .alive = true,
        };
        return 0;
    }
    return -1;
}

static int add_soft(Sheet *s, float x, float y, uint32_t cols, uint32_t rows, float spacing) {
    if (!finite2(x, y) || !isfinite(spacing) || cols < 2u || rows < 2u ||
        cols > k_soft_side || rows > k_soft_side || spacing < 4.0f || spacing > 400.0f) {
        return -1;
    }
    for (uint32_t i = 0; i < k_soft_cap; ++i) {
        if (s->soft[i].alive) {
            continue;
        }
        Soft *p = &s->soft[i];
        memset(p, 0, sizeof *p);
        p->ox = x;
        p->oy = y;
        p->cols = cols;
        p->rows = rows;
        p->spacing = spacing;
        p->alive = true;
        for (uint32_t r = 0; r < rows; ++r) {
            for (uint32_t c = 0; c < cols; ++c) {
                uint32_t k = r * cols + c;
                float lx = ((float)c - (float)(cols - 1u) * 0.5f) * spacing;
                float ly = ((float)r - (float)(rows - 1u) * 0.5f) * spacing;
                p->rx[k] = lx;
                p->ry[k] = ly;
                p->x[k] = x + lx;
                p->y[k] = y + ly;
            }
        }
        return 0;
    }
    return -1;
}

static float poly_area(const float *x, const float *y, uint32_t n) {
    float a = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t j = i + 1u < n ? i + 1u : 0u;
        a += x[i] * y[j] - x[j] * y[i];
    }
    return a * 0.5f;
}

static bool poly_convex(const float *x, const float *y, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t i1 = i + 1u < n ? i + 1u : 0u;
        uint32_t i2 = i1 + 1u < n ? i1 + 1u : 0u;
        float ex = x[i1] - x[i];
        float ey = y[i1] - y[i];
        float fx = x[i2] - x[i1];
        float fy = y[i2] - y[i1];
        if (ex * fy - ey * fx < -1.0f) {
            return false;
        }
    }
    return true;
}

static int add_dyn(Sheet *s, float x, float y, float yaw_deg, uint32_t n,
                   const float *lx, const float *ly) {
    if (!finite2(x, y) || !isfinite(yaw_deg) || n < 3u || n > k_dyn_verts) {
        return -1;
    }
    float area = poly_area(lx, ly, n);
    if (!isfinite(area) || fabsf(area) < 1.0f) {
        return -1;
    }
    float tx[k_dyn_verts];
    float ty[k_dyn_verts];
    if (area < 0.0f) {
        for (uint32_t v = 0; v < n; ++v) {
            tx[v] = lx[n - 1u - v];
            ty[v] = ly[n - 1u - v];
        }
    } else {
        memcpy(tx, lx, (size_t)n * sizeof(float));
        memcpy(ty, ly, (size_t)n * sizeof(float));
    }
    if (!poly_convex(tx, ty, n)) {
        return -1;
    }
    for (uint32_t i = 0; i < k_dyn_cap; ++i) {
        if (s->dyn[i].alive) {
            continue;
        }
        Dyn *d = &s->dyn[i];
        memset(d, 0, sizeof *d);
        d->x = x;
        d->y = y;
        d->yaw = yaw_deg * k_deg;
        d->n = n;
        d->alive = true;
        memcpy(d->lx, tx, (size_t)n * sizeof(float));
        memcpy(d->ly, ty, (size_t)n * sizeof(float));
        return 0;
    }
    return -1;
}

static int parse_sheet(Sheet *dst, const char *text, size_t len, const char *name) {
    memset(dst, 0, sizeof *dst);
    bool pending = false;
    uint32_t left = 0;
    uint32_t have = 0;
    float lx[k_dyn_verts];
    float ly[k_dyn_verts];
    float dx = 0.0f;
    float dy = 0.0f;
    float dyaw = 0.0f;
    int number = 0;
    const char *cursor = text;
    const char *end = text + len;
    while (cursor < end) {
        const char *eol = memchr(cursor, '\n', (size_t)(end - cursor));
        size_t linelen = eol != nullptr ? (size_t)(eol - cursor) : (size_t)(end - cursor);
        number++;
        if (linelen >= 256u) {
            fprintf(stderr, "[Sheet] %s:%d is too long\n", name, number);
            return -1;
        }
        char line[256];
        memcpy(line, cursor, linelen);
        line[linelen] = '\0';
        trim(line);
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p != '\0') {
            if (pending) {
                char tail = 0;
                float vx = 0.0f;
                float vy = 0.0f;
                if (sscanf(p, "%f %f %c", &vx, &vy, &tail) != 2 || !finite2(vx, vy)) {
                    fprintf(stderr, "[Sheet] %s:%d needs a local x y\n", name, number);
                    return -1;
                }
                lx[have] = vx;
                ly[have] = vy;
                have++;
                left--;
                if (left == 0u) {
                    if (add_dyn(dst, dx, dy, dyaw, have, lx, ly) != 0) {
                        fprintf(stderr, "[Sheet] %s:%d shape was refused\n", name, number);
                        return -1;
                    }
                    pending = false;
                }
            } else if (strncmp(p, "rigid", 5) == 0) {
                float x = 0, y = 0, hx = 0, hy = 0, yaw = 0;
                char tail = 0;
                if (sscanf(p, "rigid %f %f %f %f %f %c", &x, &y, &hx, &hy, &yaw, &tail) != 5 ||
                    add_rigid(dst, x, y, hx, hy, yaw) != 0) {
                    fprintf(stderr, "[Sheet] %s:%d rigid needs x y hx hy yaw\n", name, number);
                    return -1;
                }
            } else if (strncmp(p, "soft", 4) == 0) {
                float x = 0, y = 0, spacing = 0;
                unsigned cols = 0, rows = 0;
                char tail = 0;
                if (sscanf(p, "soft %f %f %u %u %f %c", &x, &y, &cols, &rows, &spacing, &tail) != 5 ||
                    add_soft(dst, x, y, cols, rows, spacing) != 0) {
                    fprintf(stderr, "[Sheet] %s:%d soft needs x y cols rows spacing\n", name, number);
                    return -1;
                }
            } else if (strncmp(p, "dyn", 3) == 0) {
                unsigned n = 0;
                char tail = 0;
                if (sscanf(p, "dyn %f %f %f %u %c", &dx, &dy, &dyaw, &n, &tail) != 4 ||
                    n < 3u || n > k_dyn_verts) {
                    fprintf(stderr, "[Sheet] %s:%d dyn needs x y yaw n\n", name, number);
                    return -1;
                }
                pending = true;
                left = n;
                have = 0;
            } else {
                fprintf(stderr, "[Sheet] %s:%d unknown record\n", name, number);
                return -1;
            }
        }
        if (eol == nullptr) {
            break;
        }
        cursor = eol + 1;
    }
    if (pending) {
        fprintf(stderr, "[Sheet] %s ended before the shape closed\n", name);
        return -1;
    }
    return 0;
}

static void note_loaded(const char *name) {
    printf("[Sheet] %s. %u rigid, %u soft, %u shape.\n", name, count_alive_rigid(&s_sheet),
           count_alive_soft(&s_sheet), count_alive_dyn(&s_sheet));
    fflush(stdout);
}

int sheet_load_memory(const char *text, size_t n) {
    if (text == nullptr) {
        return -1;
    }
    Sheet next;
    if (parse_sheet(&next, text, n, "memory") != 0) {
        return -1;
    }
    s_sheet = next;
    s_ready = true;
    s_sel = k_sel_none;
    s_moved = false;
    return 0;
}

int sheet_load_path(const char *path) {
    if (path == nullptr || path[0] == '\0') {
        return -1;
    }
    if (access(path, R_OK) != 0) {
        return 1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "[Sheet] %s could not be read.\n", path);
        return -1;
    }
    char buf[8192];
    size_t used = 0;
    while (used + 1u < sizeof buf) {
        ssize_t got = read(fd, buf + used, sizeof buf - 1u - used);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            fprintf(stderr, "[Sheet] %s could not be read.\n", path);
            return -1;
        }
        if (got == 0) {
            break;
        }
        used += (size_t)got;
    }
    close(fd);
    if (used + 1u >= sizeof buf) {
        fprintf(stderr, "[Sheet] %s does not fit.\n", path);
        return -1;
    }
    buf[used] = '\0';
    Sheet next;
    if (parse_sheet(&next, buf, used, path) != 0) {
        return -1;
    }
    s_sheet = next;
    s_ready = true;
    s_sel = k_sel_none;
    s_moved = false;
    note_loaded(path);
    return 0;
}

static int write_all(const char *path, const char *body, size_t used) {
    char tmp[1100];
    int tmp_n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (tmp_n < 0 || (size_t)tmp_n >= sizeof tmp) {
        fprintf(stderr, "[Sheet] The sheet path does not fit.\n");
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "[Sheet] %s could not be opened.\n", tmp);
        return -1;
    }
    size_t left = used;
    const char *cursor = body;
    while (left > 0u) {
        ssize_t wrote = write(fd, cursor, left);
        if (wrote < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            unlink(tmp);
            fprintf(stderr, "[Sheet] %s could not be written.\n", tmp);
            return -1;
        }
        cursor += wrote;
        left -= (size_t)wrote;
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        unlink(tmp);
        fprintf(stderr, "[Sheet] %s could not be written.\n", tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        fprintf(stderr, "[Sheet] %s could not be replaced.\n", path);
        return -1;
    }
    printf("[Sheet] Saved %s\n", path);
    fflush(stdout);
    return 0;
}

static void rebase_soft(Soft *p) {
    uint32_t count = p->cols * p->rows;
    if (!p->alive || count == 0u) {
        return;
    }
    float cx = 0.0f;
    float cy = 0.0f;
    for (uint32_t k = 0; k < count; ++k) {
        cx += p->x[k];
        cy += p->y[k];
    }
    cx /= (float)count;
    cy /= (float)count;
    p->ox = cx;
    p->oy = cy;
    for (uint32_t r = 0; r < p->rows; ++r) {
        for (uint32_t c = 0; c < p->cols; ++c) {
            uint32_t k = r * p->cols + c;
            float lx = ((float)c - (float)(p->cols - 1u) * 0.5f) * p->spacing;
            float ly = ((float)r - (float)(p->rows - 1u) * 0.5f) * p->spacing;
            p->rx[k] = lx;
            p->ry[k] = ly;
            p->x[k] = cx + lx;
            p->y[k] = cy + ly;
            p->vx[k] = 0.0f;
            p->vy[k] = 0.0f;
        }
    }
    p->awake = false;
}

int sheet_save_path(const char *path) {
    if (!s_ready || path == nullptr) {
        return 0;
    }
    for (uint32_t i = 0; i < k_soft_cap; ++i) {
        if (s_sheet.soft[i].alive) {
            rebase_soft(&s_sheet.soft[i]);
        }
    }
    char body[8192];
    size_t used = 0;
    int step = snprintf(body, sizeof body,
                        "# Resting sheet. sheet.world is not overwritten.\n");
    if (step < 0 || (size_t)step >= sizeof body) {
        return -1;
    }
    used = (size_t)step;
    for (uint32_t i = 0; i < k_rigid_cap; ++i) {
        const Rigid *b = &s_sheet.rigid[i];
        if (!b->alive) {
            continue;
        }
        step = snprintf(body + used, sizeof body - used, "rigid %.9g %.9g %.9g %.9g %.9g\n",
                        (double)b->x, (double)b->y, (double)b->hx, (double)b->hy,
                        (double)(b->yaw / k_deg));
        if (step < 0 || (size_t)step >= sizeof body - used) {
            fprintf(stderr, "[Sheet] The sheet does not fit.\n");
            return -1;
        }
        used += (size_t)step;
    }
    for (uint32_t i = 0; i < k_soft_cap; ++i) {
        const Soft *p = &s_sheet.soft[i];
        if (!p->alive) {
            continue;
        }
        step = snprintf(body + used, sizeof body - used, "soft %.9g %.9g %u %u %.9g\n",
                        (double)p->ox, (double)p->oy, p->cols, p->rows, (double)p->spacing);
        if (step < 0 || (size_t)step >= sizeof body - used) {
            fprintf(stderr, "[Sheet] The sheet does not fit.\n");
            return -1;
        }
        used += (size_t)step;
    }
    for (uint32_t i = 0; i < k_dyn_cap; ++i) {
        const Dyn *d = &s_sheet.dyn[i];
        if (!d->alive) {
            continue;
        }
        step = snprintf(body + used, sizeof body - used, "dyn %.9g %.9g %.9g %u\n",
                        (double)d->x, (double)d->y, (double)(d->yaw / k_deg), d->n);
        if (step < 0 || (size_t)step >= sizeof body - used) {
            fprintf(stderr, "[Sheet] The sheet does not fit.\n");
            return -1;
        }
        used += (size_t)step;
        for (uint32_t v = 0; v < d->n; ++v) {
            step = snprintf(body + used, sizeof body - used, "%.9g %.9g\n",
                            (double)d->lx[v], (double)d->ly[v]);
            if (step < 0 || (size_t)step >= sizeof body - used) {
                fprintf(stderr, "[Sheet] The sheet does not fit.\n");
                return -1;
            }
            used += (size_t)step;
        }
    }
    return write_all(path, body, used);
}

int sheet_load(void) {
    char path[1024];
    if (sibling("sheet.live", path, sizeof path)) {
        int live = sheet_load_path(path);
        if (live == 0) {
            return 0;
        }
    }
    if (resolve_file(k_doc, path, sizeof path) || resolve_file(k_doc_old, path, sizeof path)) {
        int doc = sheet_load_path(path);
        if (doc == 0) {
            return 0;
        }
    }
    if (sheet_load_memory(k_seed, strlen(k_seed)) != 0) {
        return -1;
    }
    printf("[Sheet] No sheet file. Using the built-in sheet.\n");
    fflush(stdout);
    return 0;
}

void sheet_hold_save(bool hold) {
    s_hold_save = hold;
}

int sheet_save(void) {
    if (s_hold_save) {
        return 0;
    }
    char path[1024];
    if (!sibling("sheet.live", path, sizeof path)) {
        fprintf(stderr, "[Sheet] No directory for the sheet.\n");
        return -1;
    }
    return sheet_save_path(path);
}

bool sheet_reload(void) {
    char path[1024];
    if (!resolve_file(k_doc, path, sizeof path) && !resolve_file(k_doc_old, path, sizeof path)) {
        fprintf(stderr, "[Sheet] sheet.world is not there to read again.\n");
        return false;
    }
    return sheet_load_path(path) == 0;
}

void sheet_reset(void) {
    s_ready = false;
    s_sel = k_sel_none;
    s_moved = false;
    (void)sheet_load();
}

static void rot(float yaw, float lx, float ly, float *ox, float *oy) {
    float c = cosf(yaw);
    float s = sinf(yaw);
    *ox = c * lx - s * ly;
    *oy = s * lx + c * ly;
}

static void box_verts(const Rigid *b, float *x, float *y) {
    float lx[4] = { -b->hx, b->hx, b->hx, -b->hx };
    float ly[4] = { -b->hy, -b->hy, b->hy, b->hy };
    for (uint32_t i = 0; i < 4u; ++i) {
        float wx = 0.0f;
        float wy = 0.0f;
        rot(b->yaw, lx[i], ly[i], &wx, &wy);
        x[i] = b->x + wx;
        y[i] = b->y + wy;
    }
}

static void dyn_verts(const Dyn *d, float *x, float *y) {
    for (uint32_t i = 0; i < d->n; ++i) {
        float wx = 0.0f;
        float wy = 0.0f;
        rot(d->yaw, d->lx[i], d->ly[i], &wx, &wy);
        x[i] = d->x + wx;
        y[i] = d->y + wy;
    }
}

static bool inside_poly(const float *x, const float *y, uint32_t n, float px, float py) {
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t j = i + 1u < n ? i + 1u : 0u;
        float ex = x[j] - x[i];
        float ey = y[j] - y[i];
        float c = ex * (py - y[i]) - ey * (px - x[i]);
        if (c < -0.75f) {
            return false;
        }
    }
    return true;
}

static void project(const Poly *p, float nx, float ny, float *mn, float *mx) {
    float d = p->x[0] * nx + p->y[0] * ny;
    *mn = d;
    *mx = d;
    for (uint32_t i = 1; i < p->n; ++i) {
        d = p->x[i] * nx + p->y[i] * ny;
        if (d < *mn) {
            *mn = d;
        }
        if (d > *mx) {
            *mx = d;
        }
    }
}

static bool axis_gap(const Poly *a, const Poly *b, float nx, float ny, float *best, float *ox, float *oy) {
    float len = hypotf(nx, ny);
    if (len < 1.0e-6f) {
        return true;
    }
    nx /= len;
    ny /= len;
    float a0 = 0, a1 = 0, b0 = 0, b1 = 0;
    project(a, nx, ny, &a0, &a1);
    project(b, nx, ny, &b0, &b1);
    float overlap = fminf(a1, b1) - fmaxf(a0, b0);
    if (overlap <= k_slop) {
        return false;
    }
    if (overlap < *best) {
        float dir = ((b0 + b1) - (a0 + a1)) >= 0.0f ? 1.0f : -1.0f;
        *best = overlap;
        *ox = nx * dir;
        *oy = ny * dir;
    }
    return true;
}

static bool sat_hit(const Poly *a, const Poly *b, float *nx, float *ny, float *pen) {
    if (a->n < 3u || b->n < 3u) {
        return false;
    }
    float best = 1.0e30f;
    float ox = 1.0f;
    float oy = 0.0f;
    for (uint32_t i = 0; i < a->n; ++i) {
        uint32_t j = i + 1u < a->n ? i + 1u : 0u;
        if (!axis_gap(a, b, a->y[i] - a->y[j], a->x[j] - a->x[i], &best, &ox, &oy)) {
            return false;
        }
    }
    for (uint32_t i = 0; i < b->n; ++i) {
        uint32_t j = i + 1u < b->n ? i + 1u : 0u;
        if (!axis_gap(a, b, b->y[i] - b->y[j], b->x[j] - b->x[i], &best, &ox, &oy)) {
            return false;
        }
    }
    *nx = ox;
    *ny = oy;
    *pen = best;
    return true;
}

static uint32_t gather(Poly *out) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < k_rigid_cap && n < 24u; ++i) {
        Rigid *b = &s_sheet.rigid[i];
        if (!b->alive) {
            continue;
        }
        Poly *p = &out[n++];
        memset(p, 0, sizeof *p);
        box_verts(b, p->x, p->y);
        p->n = 4;
        p->cx = &b->x;
        p->cy = &b->y;
        p->vx = &b->vx;
        p->vy = &b->vy;
        p->awake = &b->awake;
    }
    for (uint32_t i = 0; i < k_dyn_cap && n < 24u; ++i) {
        Dyn *d = &s_sheet.dyn[i];
        if (!d->alive) {
            continue;
        }
        Poly *p = &out[n++];
        memset(p, 0, sizeof *p);
        dyn_verts(d, p->x, p->y);
        p->n = d->n;
        p->cx = &d->x;
        p->cy = &d->y;
        p->vx = &d->vx;
        p->vy = &d->vy;
        p->awake = &d->awake;
    }
    return n;
}

static void separate(Poly *a, Poly *b) {
    float nx = 0.0f;
    float ny = 0.0f;
    float pen = 0.0f;
    if (!sat_hit(a, b, &nx, &ny, &pen)) {
        return;
    }
    float half = pen * 0.5f;
    *a->cx -= nx * half;
    *a->cy -= ny * half;
    *b->cx += nx * half;
    *b->cy += ny * half;
    float va = *a->vx * nx + *a->vy * ny;
    float vb = *b->vx * nx + *b->vy * ny;
    if (vb - va < 0.0f) {
        float avg = 0.5f * (va + vb);
        *a->vx += (avg - va) * nx;
        *a->vy += (avg - va) * ny;
        *b->vx += (avg - vb) * nx;
        *b->vy += (avg - vb) * ny;
    }
    *a->awake = true;
    *b->awake = true;
    mark_moved();
}

static void cap_speed(float *vx, float *vy) {
    float sp = hypotf(*vx, *vy);
    if (sp > k_speed_cap) {
        float s = k_speed_cap / sp;
        *vx *= s;
        *vy *= s;
    }
}

static void spring_pair(Soft *p, uint32_t a, uint32_t b, float rest, float *fx, float *fy) {
    float dx = p->x[b] - p->x[a];
    float dy = p->y[b] - p->y[a];
    float len = hypotf(dx, dy);
    if (len < 1.0e-3f) {
        return;
    }
    float ux = dx / len;
    float uy = dy / len;
    float vrel = (p->vx[b] - p->vx[a]) * ux + (p->vy[b] - p->vy[a]) * uy;
    float f = k_spring * (len - rest) + k_rel * vrel;
    fx[a] += f * ux;
    fy[a] += f * uy;
    fx[b] -= f * ux;
    fy[b] -= f * uy;
}

static float soft_strain(const Soft *p) {
    float worst = 0.0f;
    for (uint32_t r = 0; r < p->rows; ++r) {
        for (uint32_t c = 0; c + 1u < p->cols; ++c) {
            uint32_t a = r * p->cols + c;
            float dx = p->x[a + 1u] - p->x[a];
            float dy = p->y[a + 1u] - p->y[a];
            float err = fabsf(hypotf(dx, dy) - p->spacing);
            if (err > worst) {
                worst = err;
            }
        }
    }
    for (uint32_t r = 0; r + 1u < p->rows; ++r) {
        for (uint32_t c = 0; c < p->cols; ++c) {
            uint32_t a = r * p->cols + c;
            uint32_t b = a + p->cols;
            float dx = p->x[b] - p->x[a];
            float dy = p->y[b] - p->y[a];
            float err = fabsf(hypotf(dx, dy) - p->spacing);
            if (err > worst) {
                worst = err;
            }
        }
    }
    float diag = p->spacing * 1.41421356237f;
    for (uint32_t r = 0; r + 1u < p->rows; ++r) {
        for (uint32_t c = 0; c + 1u < p->cols; ++c) {
            uint32_t a = r * p->cols + c;
            uint32_t b = a + p->cols + 1u;
            float dx = p->x[b] - p->x[a];
            float dy = p->y[b] - p->y[a];
            float err = fabsf(hypotf(dx, dy) - diag);
            if (err > worst) {
                worst = err;
            }
            dx = p->x[a + p->cols] - p->x[a + 1u];
            dy = p->y[a + p->cols] - p->y[a + 1u];
            err = fabsf(hypotf(dx, dy) - diag);
            if (err > worst) {
                worst = err;
            }
        }
    }
    return worst;
}

static void push_soft_out(Soft *p) {
    uint32_t n = p->cols * p->rows;
    Poly solids[24];
    uint32_t sn = gather(solids);
    for (uint32_t k = 0; k < n; ++k) {
        for (uint32_t s = 0; s < sn; ++s) {
            const Poly *poly = &solids[s];
            if (!inside_poly(poly->x, poly->y, poly->n, p->x[k], p->y[k])) {
                continue;
            }
            float best = 1.0e30f;
            float nx = 0.0f;
            float ny = 1.0f;
            for (uint32_t e = 0; e < poly->n; ++e) {
                uint32_t j = e + 1u < poly->n ? e + 1u : 0u;
                float ex = poly->x[j] - poly->x[e];
                float ey = poly->y[j] - poly->y[e];
                float len = hypotf(ex, ey);
                if (len < 1.0e-4f) {
                    continue;
                }
                float ox = ey / len;
                float oy = -ex / len;
                float dist = (p->x[k] - poly->x[e]) * ox + (p->y[k] - poly->y[e]) * oy;
                float pen = -dist;
                if (pen < best) {
                    best = pen;
                    nx = ox;
                    ny = oy;
                }
            }
            if (best < 1.0e20f && best > 0.0f) {
                p->x[k] += nx * (best + k_slop);
                p->y[k] += ny * (best + k_slop);
                float vn = p->vx[k] * nx + p->vy[k] * ny;
                if (vn < 0.0f) {
                    p->vx[k] -= vn * nx;
                    p->vy[k] -= vn * ny;
                }
                p->awake = true;
                mark_moved();
            }
        }
    }
}

static bool any_hot(void) {
    Poly solids[24];
    uint32_t n = gather(solids);
    for (uint32_t i = 0; i < n; ++i) {
        if (hypotf(*solids[i].vx, *solids[i].vy) >= k_sleep) {
            return true;
        }
        for (uint32_t j = i + 1u; j < n; ++j) {
            float nx, ny, pen;
            if (sat_hit(&solids[i], &solids[j], &nx, &ny, &pen)) {
                return true;
            }
        }
    }
    for (uint32_t i = 0; i < k_soft_cap; ++i) {
        Soft *p = &s_sheet.soft[i];
        if (!p->alive) {
            continue;
        }
        uint32_t count = p->cols * p->rows;
        for (uint32_t k = 0; k < count; ++k) {
            if (hypotf(p->vx[k], p->vy[k]) >= k_sleep) {
                return true;
            }
        }
        if (soft_strain(p) > k_strain) {
            return true;
        }
    }
    return false;
}

static void fall_asleep(void) {
    if (any_hot()) {
        return;
    }
    for (uint32_t i = 0; i < k_rigid_cap; ++i) {
        s_sheet.rigid[i].awake = false;
        s_sheet.rigid[i].vx = 0.0f;
        s_sheet.rigid[i].vy = 0.0f;
    }
    for (uint32_t i = 0; i < k_dyn_cap; ++i) {
        s_sheet.dyn[i].awake = false;
        s_sheet.dyn[i].vx = 0.0f;
        s_sheet.dyn[i].vy = 0.0f;
    }
    for (uint32_t i = 0; i < k_soft_cap; ++i) {
        Soft *p = &s_sheet.soft[i];
        p->awake = false;
        uint32_t count = p->cols * p->rows;
        for (uint32_t k = 0; k < count && k < k_soft_pts; ++k) {
            p->vx[k] = 0.0f;
            p->vy[k] = 0.0f;
        }
    }
}

bool sheet_integrate(float dt) {
    if (!s_ready || !(dt > 0.0f) || dt > 0.05f) {
        return sheet_busy();
    }
    float damp = expf(-k_damp * dt);
    float sd = expf(-k_soft_damp * dt);
    for (uint32_t i = 0; i < k_rigid_cap; ++i) {
        Rigid *b = &s_sheet.rigid[i];
        if (!b->alive || !b->awake) {
            continue;
        }
        b->vx *= damp;
        b->vy *= damp;
        cap_speed(&b->vx, &b->vy);
        b->x += b->vx * dt;
        b->y += b->vy * dt;
        mark_moved();
    }
    for (uint32_t i = 0; i < k_dyn_cap; ++i) {
        Dyn *d = &s_sheet.dyn[i];
        if (!d->alive || !d->awake) {
            continue;
        }
        d->vx *= damp;
        d->vy *= damp;
        cap_speed(&d->vx, &d->vy);
        d->x += d->vx * dt;
        d->y += d->vy * dt;
        mark_moved();
    }
    for (uint32_t i = 0; i < k_soft_cap; ++i) {
        Soft *p = &s_sheet.soft[i];
        if (!p->alive || !p->awake) {
            continue;
        }
        float fx[k_soft_pts];
        float fy[k_soft_pts];
        uint32_t count = p->cols * p->rows;
        memset(fx, 0, sizeof fx);
        memset(fy, 0, sizeof fy);
        for (uint32_t r = 0; r < p->rows; ++r) {
            for (uint32_t c = 0; c + 1u < p->cols; ++c) {
                spring_pair(p, r * p->cols + c, r * p->cols + c + 1u, p->spacing, fx, fy);
            }
        }
        for (uint32_t r = 0; r + 1u < p->rows; ++r) {
            for (uint32_t c = 0; c < p->cols; ++c) {
                spring_pair(p, r * p->cols + c, (r + 1u) * p->cols + c, p->spacing, fx, fy);
            }
        }
        float diag = p->spacing * 1.41421356237f;
        for (uint32_t r = 0; r + 1u < p->rows; ++r) {
            for (uint32_t c = 0; c + 1u < p->cols; ++c) {
                uint32_t a = r * p->cols + c;
                spring_pair(p, a, a + p->cols + 1u, diag, fx, fy);
                spring_pair(p, a + 1u, a + p->cols, diag, fx, fy);
            }
        }
        for (uint32_t k = 0; k < count; ++k) {
            p->vx[k] = (p->vx[k] + fx[k] * dt) * sd;
            p->vy[k] = (p->vy[k] + fy[k] * dt) * sd;
            cap_speed(&p->vx[k], &p->vy[k]);
            p->x[k] += p->vx[k] * dt;
            p->y[k] += p->vy[k] * dt;
            if (!finite2(p->x[k], p->y[k])) {
                p->x[k] = p->ox + p->rx[k];
                p->y[k] = p->oy + p->ry[k];
                p->vx[k] = 0.0f;
                p->vy[k] = 0.0f;
            }
        }
        mark_moved();
    }
    for (uint32_t pass = 0; pass < k_passes; ++pass) {
        Poly solids[24];
        uint32_t n = gather(solids);
        for (uint32_t i = 0; i < n; ++i) {
            for (uint32_t j = i + 1u; j < n; ++j) {
                separate(&solids[i], &solids[j]);
            }
        }
    }
    for (uint32_t i = 0; i < k_soft_cap; ++i) {
        if (s_sheet.soft[i].alive) {
            push_soft_out(&s_sheet.soft[i]);
        }
    }
    fall_asleep();
    return sheet_busy();
}

bool sheet_busy(void) {
    if (!s_ready) {
        return false;
    }
    for (uint32_t i = 0; i < k_rigid_cap; ++i) {
        if (s_sheet.rigid[i].alive && s_sheet.rigid[i].awake) {
            return true;
        }
    }
    for (uint32_t i = 0; i < k_dyn_cap; ++i) {
        if (s_sheet.dyn[i].alive && s_sheet.dyn[i].awake) {
            return true;
        }
    }
    for (uint32_t i = 0; i < k_soft_cap; ++i) {
        if (s_sheet.soft[i].alive && s_sheet.soft[i].awake) {
            return true;
        }
    }
    return false;
}

bool sheet_simulate(uint32_t sim_dt_ms) {
    if (!s_ready || sim_dt_ms == 0u) {
        return sheet_busy();
    }
    uint32_t left = sim_dt_ms;
    while (left > 0u) {
        uint32_t step = left > 8u ? 8u : left;
        (void)sheet_integrate((float)step / 1000.0f);
        left -= step;
    }
    if (s_moved && !sheet_busy()) {
        printf("[Sheet] The sheet is resting.\n");
        fflush(stdout);
        (void)sheet_save();
        s_moved = false;
    }
    return sheet_busy();
}

bool sheet_take_changed(void) {
    bool changed = s_drew;
    s_drew = false;
    return changed;
}

static bool selected_rigid(uint32_t i) {
    return s_sel == k_sel_rigid && s_sel_i == i;
}

static bool selected_soft(uint32_t i) {
    return s_sel == k_sel_soft && s_sel_i == i;
}

static bool selected_dyn(uint32_t i) {
    return s_sel == k_sel_dyn && s_sel_i == i;
}

static void shove_linear(float *vx, float *vy, float cx, float cy, float hx, float hy) {
    float dx = cx - hx;
    float dy = cy - hy;
    float len = hypotf(dx, dy);
    if (len < 1.0f) {
        dx = 1.0f;
        dy = 0.0f;
        len = 1.0f;
    }
    *vx += dx / len * k_shove;
    *vy += dy / len * k_shove;
}

static bool click_rigid(float x, float y) {
    for (uint32_t i = 0; i < k_rigid_cap; ++i) {
        Rigid *b = &s_sheet.rigid[i];
        if (!b->alive) {
            continue;
        }
        float lx = 0.0f;
        float ly = 0.0f;
        rot(-b->yaw, x - b->x, y - b->y, &lx, &ly);
        if (fabsf(lx) <= b->hx && fabsf(ly) <= b->hy) {
            s_sel = k_sel_rigid;
            s_sel_i = i;
            shove_linear(&b->vx, &b->vy, b->x, b->y, x, y);
            b->awake = true;
            mark_moved();
            return true;
        }
    }
    return false;
}

static bool click_dyn(float x, float y) {
    for (uint32_t i = 0; i < k_dyn_cap; ++i) {
        Dyn *d = &s_sheet.dyn[i];
        if (!d->alive) {
            continue;
        }
        float vx[k_dyn_verts];
        float vy[k_dyn_verts];
        dyn_verts(d, vx, vy);
        if (!inside_poly(vx, vy, d->n, x, y)) {
            continue;
        }
        s_sel = k_sel_dyn;
        s_sel_i = i;
        shove_linear(&d->vx, &d->vy, d->x, d->y, x, y);
        d->awake = true;
        mark_moved();
        return true;
    }
    return false;
}

static bool click_soft(float x, float y) {
    float best = 1.0e30f;
    uint32_t bi = 0;
    uint32_t bk = 0;
    bool hit = false;
    for (uint32_t i = 0; i < k_soft_cap; ++i) {
        Soft *p = &s_sheet.soft[i];
        if (!p->alive) {
            continue;
        }
        uint32_t count = p->cols * p->rows;
        float reach = p->spacing * 0.75f;
        for (uint32_t k = 0; k < count; ++k) {
            float dx = x - p->x[k];
            float dy = y - p->y[k];
            float d2 = dx * dx + dy * dy;
            if (d2 < reach * reach && d2 < best) {
                best = d2;
                bi = i;
                bk = k;
                hit = true;
            }
        }
    }
    if (!hit) {
        return false;
    }
    Soft *p = &s_sheet.soft[bi];
    s_sel = k_sel_soft;
    s_sel_i = bi;
    uint32_t count = p->cols * p->rows;
    uint32_t col = bk % p->cols;
    uint32_t row = bk / p->cols;
    for (uint32_t k = 0; k < count; ++k) {
        uint32_t c = k % p->cols;
        uint32_t r = k / p->cols;
        int dc = (int)c - (int)col;
        int dr = (int)r - (int)row;
        if (dc < 0) {
            dc = -dc;
        }
        if (dr < 0) {
            dr = -dr;
        }
        if (dc + dr > 1) {
            continue;
        }
        float scale = (k == bk) ? k_shove : k_shove * 0.45f;
        float dx = p->x[k] - x;
        float dy = p->y[k] - y;
        float len = hypotf(dx, dy);
        if (len < 1.0f) {
            dx = 1.0f;
            dy = 0.0f;
            len = 1.0f;
        }
        p->vx[k] += dx / len * scale;
        p->vy[k] += dy / len * scale;
    }
    p->awake = true;
    mark_moved();
    return true;
}

bool sheet_click(float x, float y) {
    if (!s_ready || !finite2(x, y)) {
        return false;
    }
    if (click_rigid(x, y) || click_dyn(x, y) || click_soft(x, y)) {
        printf("[Sheet] Shoved.\n");
        fflush(stdout);
        return true;
    }
    return false;
}

void sheet_clear(void) {
    s_sel = k_sel_none;
}

bool sheet_has_selection(void) {
    if (s_sel == k_sel_rigid) {
        return s_sel_i < k_rigid_cap && s_sheet.rigid[s_sel_i].alive;
    }
    if (s_sel == k_sel_soft) {
        return s_sel_i < k_soft_cap && s_sheet.soft[s_sel_i].alive;
    }
    if (s_sel == k_sel_dyn) {
        return s_sel_i < k_dyn_cap && s_sheet.dyn[s_sel_i].alive;
    }
    return false;
}

bool sheet_cancel(void) {
    if (!sheet_has_selection()) {
        return false;
    }
    sheet_clear();
    return true;
}

bool sheet_delete(void) {
    if (!sheet_has_selection()) {
        return false;
    }
    if (s_sel == k_sel_rigid) {
        s_sheet.rigid[s_sel_i].alive = false;
        s_sheet.rigid[s_sel_i].awake = false;
    } else if (s_sel == k_sel_soft) {
        s_sheet.soft[s_sel_i].alive = false;
        s_sheet.soft[s_sel_i].awake = false;
    } else if (s_sel == k_sel_dyn) {
        s_sheet.dyn[s_sel_i].alive = false;
        s_sheet.dyn[s_sel_i].awake = false;
    }
    sheet_clear();
    s_moved = false;
    s_drew = true;
    printf("[Sheet] Removed.\n");
    fflush(stdout);
    (void)sheet_save();
    return true;
}

static void paint(VkCommandBuffer cmd, uint32_t width, uint32_t height, const float *x, const float *y,
                  uint32_t n, const uint16_t *idx, uint32_t tri, float cx, float cy, float yaw,
                  float fr, float fg, float fb, bool hot) {
    float er = hot ? 1.0f : 0.90f;
    float eg = hot ? 0.82f : 0.94f;
    float eb = hot ? 0.35f : 0.98f;
    shape_draw_polygon(cmd, width, height, x, y, n, idx, tri, true, cx, cy, yaw,
                       0.020f, 0.0215f, 0.023f, fr, fg, fb, er, eg, eb, k_stroke, 0.0f, k_mat_none);
}

void sheet_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height) {
    if (!s_ready || cmd == VK_NULL_HANDLE) {
        return;
    }
    uint16_t quad[6] = { 0, 1, 2, 0, 2, 3 };
    for (uint32_t i = 0; i < k_rigid_cap; ++i) {
        const Rigid *b = &s_sheet.rigid[i];
        if (!b->alive) {
            continue;
        }
        float x[4] = { b->x - b->hx, b->x + b->hx, b->x + b->hx, b->x - b->hx };
        float y[4] = { b->y - b->hy, b->y - b->hy, b->y + b->hy, b->y + b->hy };
        paint(cmd, width, height, x, y, 4, quad, 2, b->x, b->y, b->yaw,
              0.22f, 0.48f, 0.78f, selected_rigid(i));
    }
    for (uint32_t i = 0; i < k_soft_cap; ++i) {
        const Soft *p = &s_sheet.soft[i];
        if (!p->alive || p->cols < 2u || p->rows < 2u) {
            continue;
        }
        bool hot = selected_soft(i);
        for (uint32_t r = 0; r + 1u < p->rows; ++r) {
            for (uint32_t c = 0; c + 1u < p->cols; ++c) {
                uint32_t i00 = r * p->cols + c;
                uint32_t i10 = i00 + 1u;
                uint32_t i11 = i00 + p->cols + 1u;
                uint32_t i01 = i00 + p->cols;
                float x[4] = { p->x[i00], p->x[i10], p->x[i11], p->x[i01] };
                float y[4] = { p->y[i00], p->y[i10], p->y[i11], p->y[i01] };
                paint(cmd, width, height, x, y, 4, quad, 2, 0.0f, 0.0f, 0.0f,
                      0.30f, 0.62f, 0.38f, hot);
            }
        }
    }
    for (uint32_t i = 0; i < k_dyn_cap; ++i) {
        const Dyn *d = &s_sheet.dyn[i];
        if (!d->alive || d->n < 3u) {
            continue;
        }
        float x[k_dyn_verts];
        float y[k_dyn_verts];
        uint16_t idx[k_dyn_verts * 3u];
        for (uint32_t v = 0; v < d->n; ++v) {
            x[v] = d->x + d->lx[v];
            y[v] = d->y + d->ly[v];
        }
        uint32_t tri = d->n - 2u;
        for (uint32_t t = 0; t < tri; ++t) {
            idx[t * 3u] = 0;
            idx[t * 3u + 1u] = (uint16_t)(t + 1u);
            idx[t * 3u + 2u] = (uint16_t)(t + 2u);
        }
        paint(cmd, width, height, x, y, d->n, idx, tri, d->x, d->y, d->yaw,
              0.62f, 0.36f, 0.68f, selected_dyn(i));
    }
}

uint32_t sheet_rigid_count(void) {
    return count_alive_rigid(&s_sheet);
}

uint32_t sheet_soft_count(void) {
    return count_alive_soft(&s_sheet);
}

uint32_t sheet_dyn_count(void) {
    return count_alive_dyn(&s_sheet);
}

void sheet_rigid_pos(uint32_t index, float *x, float *y) {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < k_rigid_cap; ++i) {
        if (!s_sheet.rigid[i].alive) {
            continue;
        }
        if (seen == index) {
            if (x != nullptr) {
                *x = s_sheet.rigid[i].x;
            }
            if (y != nullptr) {
                *y = s_sheet.rigid[i].y;
            }
            return;
        }
        seen++;
    }
}

void sheet_dyn_local(uint32_t index, uint32_t vert, float *x, float *y) {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < k_dyn_cap; ++i) {
        if (!s_sheet.dyn[i].alive) {
            continue;
        }
        if (seen == index) {
            if (vert < s_sheet.dyn[i].n) {
                if (x != nullptr) {
                    *x = s_sheet.dyn[i].lx[vert];
                }
                if (y != nullptr) {
                    *y = s_sheet.dyn[i].ly[vert];
                }
            }
            return;
        }
        seen++;
    }
}

void sheet_soft_pos(uint32_t index, uint32_t point, float *x, float *y) {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < k_soft_cap; ++i) {
        const Soft *p = &s_sheet.soft[i];
        if (!p->alive) {
            continue;
        }
        if (seen == index) {
            uint32_t count = p->cols * p->rows;
            if (point < count) {
                if (x != nullptr) {
                    *x = p->x[point];
                }
                if (y != nullptr) {
                    *y = p->y[point];
                }
            }
            return;
        }
        seen++;
    }
}

void sheet_soft_layout(uint32_t index, uint32_t *cols, uint32_t *rows, float *spacing) {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < k_soft_cap; ++i) {
        const Soft *p = &s_sheet.soft[i];
        if (!p->alive) {
            continue;
        }
        if (seen == index) {
            if (cols != nullptr) {
                *cols = p->cols;
            }
            if (rows != nullptr) {
                *rows = p->rows;
            }
            if (spacing != nullptr) {
                *spacing = p->spacing;
            }
            return;
        }
        seen++;
    }
}

void sheet_soft_move(uint32_t index, uint32_t point, float dx, float dy) {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < k_soft_cap; ++i) {
        Soft *p = &s_sheet.soft[i];
        if (!p->alive) {
            continue;
        }
        if (seen == index) {
            uint32_t count = p->cols * p->rows;
            if (point < count) {
                p->x[point] += dx;
                p->y[point] += dy;
                p->awake = true;
            }
            return;
        }
        seen++;
    }
}
