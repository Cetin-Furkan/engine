#include "world/world_int.h"
#include "ring/ring.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// opening.world is the boot document and is never overwritten.
// live.world is the placement the process last saved.
// ground z                 the hit plane. The grid is already on.
// smooth 0|1               every solid, faceted or rounded
// cube x y z               center. A cube that has not been shoved keeps this point.
// cube x y z yaw N spin 0|1 inf 0|1 glow 0|1
// solid Name x y z yaw N spin 0|1 inf 0|1 glow 0|1
// mesh                     the OBJ, one turn from yaw 0
// mesh yaw N spin 0|1 inf 0|1 glow 0|1
// gone cube x y z yaw N spin 0|1 inf 0|1 glow 0|1
// gone solid Name x y z yaw N spin 0|1 inf 0|1 glow 0|1
// gone mesh yaw N spin 0|1 inf 0|1 glow 0|1
// A gone line is not drawn. Z puts the newest one back.
// Click-built faces and the heat field are not records.

static const char *k_path = "examples/demo/world/opening.world";
static const char *k_path_legacy = "world/opening.world";
static const char *k_live = "examples/demo/world/live.world";
static const char *k_live_legacy = "world/live.world";

static const char *k_preset_worlds[k_world_preset_max] = {
    // World 1: 3D-Only Click-to-Draw Studio (open central court framed by glowing pillars)
    "ground z\n"
    "smooth 0\n"
    "solid Tetrahedron -520 260 0 yaw 25000 spin 1 inf 0 glow 1 bounce 0.45\n"
    "solid Octahedron 520 260 0 yaw 45000 spin 1 inf 0 glow 1 bounce 0.68\n"
    "solid Cone -520 -260 0 yaw 15000 spin 1 inf 0 glow 1 bounce 0.56\n"
    "solid Torus 520 -260 0 yaw 65000 spin 1 inf 0 glow 1 bounce 0.82\n"
    "cube 0 340 0 yaw 38000 spin 1 inf 0 glow 0 bounce 0.50\n",

    // World 2: 3D-Only Gravity Drop & Elastic Non-Cube Solids (elevated drop with varied bounce)
    "ground z\n"
    "smooth 1\n"
    "solid Icosahedron -460 150 640 yaw 18000 spin 1 inf 1 glow 1 bounce 0.90\n"
    "solid Torus -160 190 560 yaw 42000 spin 1 inf 1 glow 1 bounce 0.84\n"
    "solid Octahedron 160 190 500 yaw 30000 spin 1 inf 1 glow 0 bounce 0.68\n"
    "solid Cone 460 150 440 yaw 12000 spin 1 inf 0 glow 0 bounce 0.56\n"
    "solid Tetrahedron -310 -190 400 yaw 55000 spin 1 inf 0 glow 0 bounce 0.38\n"
    "solid Cylinder 310 -190 360 yaw 20000 spin 1 inf 0 glow 0 bounce 0.20\n"
    "cube 0 -180 600 yaw 38000 spin 1 inf 0 glow 1 bounce 0.85\n"
    "cube -620 -60 480 yaw 14000 spin 1 inf 0 glow 0 bounce 0.48\n"
    "cube 620 -60 380 yaw 62000 spin 1 inf 0 glow 0 bounce 0.08\n",

    // World 3: 2D-Only Gravity & Soft Bodies (3D scene stays dormant in 2D-only mode)
    "ground z\n"
    "cube 0 0 0\n",

    // World 4: 2D-Only Click-to-Draw Polygon Workshop (3D scene stays dormant in 2D-only mode)
    "ground z\n"
    "cube 0 0 0\n",

    // World 5: Hybrid (3D + 2D) Click-to-Draw + Gravity & Multi-Elasticity
    "ground z\n"
    "smooth 1\n"
    "solid Icosahedron -420 120 580 yaw 22000 spin 1 inf 1 glow 1 bounce 0.90\n"
    "solid Torus 420 120 520 yaw 48000 spin 1 inf 1 glow 1 bounce 0.84\n"
    "solid Octahedron -420 -160 440 yaw 15000 spin 1 inf 0 glow 0 bounce 0.66\n"
    "solid Cone 420 -160 380 yaw 35000 spin 1 inf 0 glow 0 bounce 0.52\n"
    "cube 0 40 620 yaw 38000 spin 1 inf 0 glow 1 bounce 0.82\n",

    // World 6: 3D + 2D Switchable Restitution Staircase & Collision Arena
    "ground z\n"
    "smooth 1\n"
    "solid Cylinder -550 140 520 yaw 0 spin 1 inf 0 glow 0 bounce 0.14\n"
    "solid Tetrahedron -330 140 520 yaw 15000 spin 1 inf 0 glow 0 bounce 0.32\n"
    "solid Cone -110 140 520 yaw 30000 spin 1 inf 0 glow 0 bounce 0.50\n"
    "solid Octahedron 110 140 520 yaw 45000 spin 1 inf 1 glow 0 bounce 0.68\n"
    "solid Torus 330 140 520 yaw 60000 spin 1 inf 1 glow 1 bounce 0.82\n"
    "solid Icosahedron 550 140 520 yaw 75000 spin 1 inf 1 glow 1 bounce 0.92\n"
    "cube -380 -210 520 yaw 38000 spin 1 inf 0 glow 0 bounce 0.20\n"
    "cube 0 -210 520 yaw 38000 spin 1 inf 0 glow 0 bounce 0.58\n"
    "cube 380 -210 520 yaw 38000 spin 1 inf 0 glow 1 bounce 0.88\n",
};

static bool s_file_ground_seen = false;
static bool s_smooth_seen = false;
static bool s_loading = false;
static bool s_loaded = false;
static char s_source[1024];

static const char *source_name(void) {
    return s_source[0] != '\0' ? s_source : k_path;
}

static bool flag_bit(int v) {
    return v == 0 || v == 1;
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

static int place_ground(const char *line, int number) {
    char axis[8] = {0};
    char tail = 0;
    if (sscanf(line, "ground %7s %c", axis, &tail) != 1) {
        fprintf(stderr, "[World] %s:%d ground needs an axis\n", source_name(), number);
        return -1;
    }
    if (strcmp(axis, "z") != 0) {
        fprintf(stderr, "[World] %s:%d ground %s is not drawn. The ground plane is z.\n",
                source_name(), number, axis);
        return -1;
    }
    // The ground is on by default (universal metric grid), so naming it
    // in the file confirms the default instead of duplicating it. Only a
    // second ground record in the same file is an error; ground_set is
    // idempotent for the same plane, so track file records locally.
    if (s_file_ground_seen) {
        fprintf(stderr, "[World] %s:%d names the ground twice\n", source_name(), number);
        return -1;
    }
    s_file_ground_seen = true;
    ground_set(2, 0.0f);
    return 0;
}

static int place_cube(const char *line, int number) {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    int yaw = 0;
    int spinning = 0;
    int infinite = 0;
    int glow = 0;
    float bounce = 0.0f;
    char tail = 0;
    int posed_b = sscanf(line, "cube %f %f %f yaw %d spin %d inf %d glow %d bounce %f %c",
                         &x, &y, &z, &yaw, &spinning, &infinite, &glow, &bounce, &tail);
    int posed = sscanf(line, "cube %f %f %f yaw %d spin %d inf %d glow %d %c",
                       &x, &y, &z, &yaw, &spinning, &infinite, &glow, &tail);
    int short_b = sscanf(line, "cube %f %f %f bounce %f %c", &x, &y, &z, &bounce, &tail);
    bool have_pose = false;
    if (posed_b == 8) {
        if (!flag_bit(spinning) || !flag_bit(infinite) || !flag_bit(glow) || !isfinite(bounce)) {
            fprintf(stderr, "[World] %s:%d cube pose flags must be 0 or 1 and bounce finite\n",
                    source_name(), number);
            return -1;
        }
        have_pose = true;
    } else if (posed > 3) {
        if (posed != 7 || !flag_bit(spinning) || !flag_bit(infinite) || !flag_bit(glow)) {
            fprintf(stderr, "[World] %s:%d cube pose needs yaw spin inf glow, each flag 0 or 1\n",
                    source_name(), number);
            return -1;
        }
        bounce = 0.0f;
        have_pose = true;
    } else if (short_b == 4) {
        if (!isfinite(bounce)) {
            fprintf(stderr, "[World] %s:%d cube bounce is not finite\n", source_name(), number);
            return -1;
        }
        yaw = 38000;
        spinning = 1;
        infinite = 0;
        glow = 0;
        have_pose = true;
    } else if (sscanf(line, "cube %f %f %f %c", &x, &y, &z, &tail) != 3) {
        fprintf(stderr, "[World] %s:%d cube needs x y z\n", source_name(), number);
        return -1;
    }
    if (!isfinite(x) || !isfinite(y) || !isfinite(z)) {
        fprintf(stderr, "[World] %s:%d cube position is not finite\n", source_name(), number);
        return -1;
    }
    bool ok = false;
    if (have_pose) {
        ok = cube_kind_spawn_elastic((Coord){ x, y, z }, (int32_t)yaw,
                                     spinning != 0, infinite != 0, glow != 0, bounce);
    } else {
        ok = cube_kind_spawn((Coord){ x, y, z });
    }
    if (!ok) {
        return -1;
    }
    return 0;
}

static int place_solid(const char *line, int number) {
    char name[32] = {0};
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    int yaw = 0;
    int spinning = 0;
    int infinite = 0;
    int glow = 0;
    float bounce = -1.0f;
    char tail = 0;
    int got_b = sscanf(line, "solid %31s %f %f %f yaw %d spin %d inf %d glow %d bounce %f %c",
                       name, &x, &y, &z, &yaw, &spinning, &infinite, &glow, &bounce, &tail);
    int got = (got_b == 9) ? 9 : sscanf(line, "solid %31s %f %f %f yaw %d spin %d inf %d glow %d %c",
                                        name, &x, &y, &z, &yaw, &spinning, &infinite, &glow, &tail);
    if ((got != 8 && got != 9) || !flag_bit(spinning) || !flag_bit(infinite) || !flag_bit(glow) ||
        (got == 9 && !isfinite(bounce))) {
        fprintf(stderr, "[World] %s:%d solid needs Name x y z yaw spin inf glow [bounce]\n",
                source_name(), number);
        return -1;
    }
    int kind = solid_kind_index(name);
    if (kind < 0) {
        fprintf(stderr, "[World] %s:%d solid %s is not a kind\n", source_name(), number, name);
        return -1;
    }
    if (!solid_kind_place_elastic((uint32_t)kind, (Coord){ x, y, z }, (int32_t)yaw,
                                  spinning != 0, infinite != 0, glow != 0,
                                  got == 9 ? bounce : -1.0f)) {
        return -1;
    }
    return 0;
}

static int place_mesh(const char *line, int number) {
    if (mesh_kind_wanted()) {
        fprintf(stderr, "[World] %s:%d names the mesh twice\n", source_name(), number);
        return -1;
    }
    int yaw = 0;
    int spinning = 0;
    int infinite = 0;
    int glow = 0;
    char tail = 0;
    int posed = sscanf(line, "mesh yaw %d spin %d inf %d glow %d %c",
                       &yaw, &spinning, &infinite, &glow, &tail);
    if (posed == 4) {
        if (!flag_bit(spinning) || !flag_bit(infinite) || !flag_bit(glow)) {
            fprintf(stderr, "[World] %s:%d mesh flags must be 0 or 1\n", source_name(), number);
            return -1;
        }
        if (!mesh_kind_pose((int32_t)yaw, spinning != 0, infinite != 0, glow != 0)) {
            return -1;
        }
        return 0;
    }
    const char *rest = line + 4;
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }
    if (posed > 4 || strncmp(rest, "yaw", 3) == 0) {
        fprintf(stderr, "[World] %s:%d mesh pose needs yaw spin inf glow\n", source_name(), number);
        return -1;
    }
    if (*rest != '\0') {
        fprintf(stderr, "[World] %s:%d mesh takes no arguments, or a yaw pose. The OBJ path is plots/mesh.obj.\n",
                source_name(), number);
        return -1;
    }
    mesh_kind_set_wanted(true);
    mesh_kind_raise();
    return 0;
}

static int place_gone(const char *line, int number) {
    const char *rest = line + 4;
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }
    WorldRemoved g = {0};
    char tail = 0;
    if (strncmp(rest, "cube", 4) == 0) {
        int yaw = 0;
        int spinning = 0;
        int infinite = 0;
        int glow = 0;
        int got = sscanf(rest, "cube %f %f %f yaw %d spin %d inf %d glow %d %c",
                         &g.at.x, &g.at.y, &g.at.z, &yaw, &spinning, &infinite, &glow, &tail);
        if (got != 7 || !flag_bit(spinning) || !flag_bit(infinite) || !flag_bit(glow)) {
            fprintf(stderr, "[World] %s:%d gone cube needs x y z yaw spin inf glow\n",
                    source_name(), number);
            return -1;
        }
        if (!isfinite(g.at.x) || !isfinite(g.at.y) || !isfinite(g.at.z)) {
            fprintf(stderr, "[World] %s:%d cube position is not finite\n", source_name(), number);
            return -1;
        }
        g.kind = k_removed_cube;
        g.yaw_milli = (int32_t)yaw;
        g.spinning = spinning != 0;
        g.infinite = infinite != 0;
        g.glow = glow != 0;
    } else if (strncmp(rest, "solid", 5) == 0) {
        char name[32] = {0};
        int yaw = 0;
        int spinning = 0;
        int infinite = 0;
        int glow = 0;
        int got = sscanf(rest, "solid %31s %f %f %f yaw %d spin %d inf %d glow %d %c",
                         name, &g.at.x, &g.at.y, &g.at.z, &yaw, &spinning, &infinite, &glow, &tail);
        if (got != 8 || !flag_bit(spinning) || !flag_bit(infinite) || !flag_bit(glow)) {
            fprintf(stderr, "[World] %s:%d gone solid needs Name x y z yaw spin inf glow\n",
                    source_name(), number);
            return -1;
        }
        int kind = solid_kind_index(name);
        if (kind < 0) {
            fprintf(stderr, "[World] %s:%d solid %s is not a kind\n", source_name(), number, name);
            return -1;
        }
        g.kind = k_removed_solid;
        g.solid = kind;
        g.yaw_milli = (int32_t)yaw;
        g.spinning = spinning != 0;
        g.infinite = infinite != 0;
        g.glow = glow != 0;
    } else if (strncmp(rest, "mesh", 4) == 0) {
        int yaw = 0;
        int spinning = 0;
        int infinite = 0;
        int glow = 0;
        int got = sscanf(rest, "mesh yaw %d spin %d inf %d glow %d %c",
                         &yaw, &spinning, &infinite, &glow, &tail);
        if (got != 4 || !flag_bit(spinning) || !flag_bit(infinite) || !flag_bit(glow)) {
            fprintf(stderr, "[World] %s:%d gone mesh needs yaw spin inf glow\n",
                    source_name(), number);
            return -1;
        }
        g.kind = k_removed_mesh;
        g.yaw_milli = (int32_t)yaw;
        g.spinning = spinning != 0;
        g.infinite = infinite != 0;
        g.glow = glow != 0;
    } else {
        fprintf(stderr, "[World] %s:%d gone needs a cube, a solid, or a mesh\n",
                source_name(), number);
        return -1;
    }
    world_removed_push(&g);
    return 0;
}

static int place_smooth(const char *line, int number) {
    int curved = 0;
    char tail = 0;
    if (sscanf(line, "smooth %d %c", &curved, &tail) != 1 || !flag_bit(curved)) {
        fprintf(stderr, "[World] %s:%d smooth needs 0 or 1\n", source_name(), number);
        return -1;
    }
    if (s_smooth_seen) {
        fprintf(stderr, "[World] %s:%d names smooth twice\n", source_name(), number);
        return -1;
    }
    s_smooth_seen = true;
    solid_kind_set_smooth(curved != 0);
    return 0;
}

// Compile-time placements. Empty by default: the demo boots into the
// universal metric grid with zero objects. Register custom C objects
// here (ops + state pointers, or an init_fn) to spawn them at startup.
#define STARTUP_BLUEPRINTS_COUNT 0
#if STARTUP_BLUEPRINTS_COUNT > 0
static const WorldBlueprint k_startup_objects[STARTUP_BLUEPRINTS_COUNT] = {
    // { .ops = &my_ops, .data = &my_state, .at = { 0.0f, 0.0f, 0.0f }, .spin = &my_state.spin },
};
#endif

static int place_blueprints(void) {
#if STARTUP_BLUEPRINTS_COUNT > 0
    for (size_t i = 0; i < STARTUP_BLUEPRINTS_COUNT; ++i) {
        const WorldBlueprint *bp = &k_startup_objects[i];
        if (bp->init_fn != nullptr) {
            if (!bp->init_fn(bp->at, bp->data)) {
                return -1;
            }
        } else if (bp->ops != nullptr) {
            if (table_add(bp->ops, bp->data, bp->at, bp->spin) < 0) {
                return -1;
            }
        }
    }
#endif
    return 0;
}

static bool resolve_world_file(const char *name, char *out, size_t cap) {
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

// 0 read, 1 absent, -1 present but unusable.
static int read_named(const char *rel_path, char *buf, size_t cap, size_t *out_len) {
    char path[1024];
    if (!resolve_world_file(rel_path, path, sizeof path)) {
        return 1;
    }
    RingContext *ring = ring_get_context();
    if (ring != nullptr && ring->is_initialized) {
        int rc = ring_read_file_all(ring, path, buf, cap, out_len, nullptr, nullptr);
        if (rc == 0) {
            snprintf(s_source, sizeof s_source, "%s", path);
            return 0;
        }
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "[World] %s could not be read.\n", path);
        return -1;
    }
    ssize_t n = read(fd, buf, cap - 1u);
    close(fd);
    if (n < 0 || (size_t)n >= cap - 1u) {
        fprintf(stderr, "[World] %s could not be read.\n", path);
        return -1;
    }
    buf[n] = '\0';
    *out_len = (size_t)n;
    snprintf(s_source, sizeof s_source, "%s", path);
    return 0;
}

static int open_document(char *buf, size_t cap, size_t *len) {
    uint32_t wid = world_selected_id();
    if (wid >= 1u && wid <= k_world_preset_max) {
        char rel[64];
        char rel_legacy[64];
        snprintf(rel, sizeof rel, "examples/demo/world/world%u.world", wid);
        snprintf(rel_legacy, sizeof rel_legacy, "world/world%u.world", wid);
        if (read_named(rel, buf, cap, len) == 0) {
            return 0;
        }
        if (read_named(rel_legacy, buf, cap, len) == 0) {
            return 0;
        }
        const char *preset = k_preset_worlds[wid - 1u];
        size_t plen = strlen(preset);
        if (plen + 1u >= cap) {
            return -1;
        }
        memcpy(buf, preset, plen + 1u);
        *len = plen;
        snprintf(s_source, sizeof s_source, "builtin:world%u.world", wid);
        return 0;
    }
    int live = read_named(k_live, buf, cap, len);
    if (live <= 0) {
        return live;
    }
    live = read_named(k_live_legacy, buf, cap, len);
    if (live <= 0) {
        return live;
    }
    if (read_named(k_path, buf, cap, len) == 0) {
        return 0;
    }
    if (read_named(k_path_legacy, buf, cap, len) == 0) {
        return 0;
    }
    return 1;
}

static int parse_document(char *buf, size_t len) {
    int number = 0;
    char *cursor = buf;
    char *end = buf + len;
    while (cursor < end) {
        char *eol = memchr(cursor, '\n', (size_t)(end - cursor));
        if (eol != nullptr) {
            *eol = '\0';
        }
        number++;
        char line[256];
        size_t linelen = strlen(cursor);
        if (linelen >= sizeof line) {
            fprintf(stderr, "[World] %s:%d is longer than %u bytes\n",
                    source_name(), number, (unsigned)sizeof line);
            return -1;
        }
        memcpy(line, cursor, linelen + 1u);
        trim(line);
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p != '\0') {
            int step = 0;
            if (strncmp(p, "ground", 6) == 0) {
                step = place_ground(p, number);
            } else if (strncmp(p, "smooth", 6) == 0) {
                step = place_smooth(p, number);
            } else if (strncmp(p, "cube", 4) == 0) {
                step = place_cube(p, number);
            } else if (strncmp(p, "solid", 5) == 0) {
                step = place_solid(p, number);
            } else if (strncmp(p, "mesh", 4) == 0) {
                step = place_mesh(p, number);
            } else if (strncmp(p, "gone", 4) == 0) {
                step = place_gone(p, number);
            } else {
                fprintf(stderr, "[World] %s:%d unknown record\n", source_name(), number);
                step = -1;
            }
            if (step != 0) {
                return -1;
            }
        }
        if (eol == nullptr) {
            break;
        }
        cursor = eol + 1;
    }
    return 0;
}

int world_load(void) {
    s_loading = true;
    world_removed_clear();
    s_file_ground_seen = false;
    s_smooth_seen = false;
    s_source[0] = '\0';
    int rc = -1;
    if (place_blueprints() == 0) {
        char buf[16384];
        size_t len = 0;
        int doc = open_document(buf, sizeof buf, &len);
        if (doc == 1) {
            table_publish();
            rc = 0;
        } else if (doc == 0) {
            rc = parse_document(buf, len);
            if (rc == 0) {
                table_publish();
                if (world_selected_id() >= 1u) {
                    printf("[World] Loaded World %u (%s) from %s\n",
                           world_selected_id(), world_selected_title(), source_name());
                    fflush(stdout);
                }
            }
        }
    }
    s_loading = false;
    s_loaded = (rc == 0);
    return rc;
}

static int append_line(char *body, size_t cap, size_t *used, const char *fmt, ...) {
    if (*used >= cap) {
        return -1;
    }
    va_list ap;
    va_start(ap, fmt);
    int wrote = vsnprintf(body + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (wrote < 0 || (size_t)wrote >= cap - *used) {
        return -1;
    }
    *used += (size_t)wrote;
    return 0;
}

static int append_object(char *body, size_t cap, size_t *used, const WorldObj *o) {
    if (o->ops == nullptr || o->ops == face_ops()) {
        return 0;
    }
    if (o->spin == nullptr) {
        fprintf(stderr, "[World] A placement has no spin, so it was not written.\n");
        return 0;
    }
    const Spin *s = o->spin;
    int yaw = (int)s->live_milli;
    int spinning = s->spinning ? 1 : 0;
    int infinite = s->infinite ? 1 : 0;
    int glow = s->glow ? 1 : 0;
    if (o->ops == cube_ops()) {
        return append_line(body, cap, used,
                           "cube %.9g %.9g %.9g yaw %d spin %d inf %d glow %d\n",
                           (double)o->at.x, (double)o->at.y, (double)o->at.z,
                           yaw, spinning, infinite, glow);
    }
    if (o->ops == solid_ops()) {
        const char *name = o->ops->name != nullptr ? o->ops->name(o) : nullptr;
        if (name == nullptr) {
            return -1;
        }
        return append_line(body, cap, used,
                           "solid %s %.9g %.9g %.9g yaw %d spin %d inf %d glow %d\n",
                           name, (double)o->at.x, (double)o->at.y, (double)o->at.z,
                           yaw, spinning, infinite, glow);
    }
    if (o->ops == mesh_ops()) {
        return append_line(body, cap, used, "mesh yaw %d spin %d inf %d glow %d\n",
                           yaw, spinning, infinite, glow);
    }
    const char *name = o->ops->name != nullptr ? o->ops->name(o) : "object";
    fprintf(stderr, "[World] %s is not written to the live placement.\n", name);
    return 0;
}

static bool live_path(char *out, size_t cap) {
    char anchor[1024];
    if (!resolve_world_file(k_path, anchor, sizeof anchor) &&
        !resolve_world_file(k_path_legacy, anchor, sizeof anchor)) {
        return false;
    }
    char *slash = strrchr(anchor, '/');
    int wrote = 0;
    if (slash == nullptr) {
        wrote = snprintf(out, cap, "live.world");
    } else {
        *slash = '\0';
        wrote = snprintf(out, cap, "%s/live.world", anchor);
    }
    return wrote > 0 && (size_t)wrote < cap;
}

int world_save(void) {
    if (s_loading || !s_loaded || world_selected_id() != 0u) {
        return 0;
    }
    char body[16384];
    size_t used = 0;
    if (append_line(body, sizeof body, &used,
                    "# Live placement. opening.world is not overwritten.\n"
                    "# Click-built faces and the heat field are not stored.\n"
                    "# A gone line waits for Z.\n"
                    "ground z\n"
                    "smooth %d\n",
                    solid_kind_smooth() ? 1 : 0) != 0) {
        fprintf(stderr, "[World] The live placement does not fit.\n");
        return -1;
    }
    bool mesh_up = false;
    int count = table_count();
    for (int i = 0; i < count; ++i) {
        const WorldObj *o = table_at(i);
        if (o == nullptr || !o->alive) {
            continue;
        }
        if (o->ops == mesh_ops()) {
            mesh_up = true;
        }
        if (append_object(body, sizeof body, &used, o) != 0) {
            fprintf(stderr, "[World] The live placement does not fit.\n");
            return -1;
        }
    }
    if (!mesh_up && mesh_kind_wanted()) {
        if (append_line(body, sizeof body, &used, "mesh\n") != 0) {
            fprintf(stderr, "[World] The live placement does not fit.\n");
            return -1;
        }
    }
    uint32_t removed_n = world_removed_count();
    for (uint32_t i = 0; i < removed_n; ++i) {
        const WorldRemoved *g = world_removed_at(i);
        int step = -1;
        if (g != nullptr && g->kind == k_removed_cube) {
            step = append_line(body, sizeof body, &used,
                               "gone cube %.9g %.9g %.9g yaw %d spin %d inf %d glow %d\n",
                               (double)g->at.x, (double)g->at.y, (double)g->at.z,
                               (int)g->yaw_milli, g->spinning ? 1 : 0, g->infinite ? 1 : 0,
                               g->glow ? 1 : 0);
        } else if (g != nullptr && g->kind == k_removed_solid) {
            const char *name = solid_kind_name((uint32_t)g->solid);
            if (name == nullptr) {
                continue;
            }
            step = append_line(body, sizeof body, &used,
                               "gone solid %s %.9g %.9g %.9g yaw %d spin %d inf %d glow %d\n",
                               name, (double)g->at.x, (double)g->at.y, (double)g->at.z,
                               (int)g->yaw_milli, g->spinning ? 1 : 0, g->infinite ? 1 : 0,
                               g->glow ? 1 : 0);
        } else if (g != nullptr && g->kind == k_removed_mesh) {
            step = append_line(body, sizeof body, &used,
                               "gone mesh yaw %d spin %d inf %d glow %d\n",
                               (int)g->yaw_milli, g->spinning ? 1 : 0, g->infinite ? 1 : 0,
                               g->glow ? 1 : 0);
        }
        if (step != 0) {
            fprintf(stderr, "[World] The live placement does not fit.\n");
            return -1;
        }
    }
    char path[1024];
    if (!live_path(path, sizeof path)) {
        fprintf(stderr, "[World] No directory for the live placement.\n");
        return -1;
    }
    char tmp[1100];
    int tmp_n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (tmp_n < 0 || (size_t)tmp_n >= sizeof tmp) {
        fprintf(stderr, "[World] The live placement path does not fit.\n");
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "[World] %s could not be opened.\n", tmp);
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
            fprintf(stderr, "[World] %s could not be written.\n", tmp);
            return -1;
        }
        cursor += wrote;
        left -= (size_t)wrote;
    }
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp);
        fprintf(stderr, "[World] %s could not be written.\n", tmp);
        return -1;
    }
    if (close(fd) != 0) {
        unlink(tmp);
        fprintf(stderr, "[World] %s could not be written.\n", tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        fprintf(stderr, "[World] %s could not be replaced.\n", path);
        return -1;
    }
    printf("[World] Saved %s\n", path);
    fflush(stdout);
    return 0;
}
