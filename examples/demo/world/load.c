#include "world/world_int.h"
#include "ring/ring.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// world/opening.world. One record a line. A # starts a comment.
// ground z          the calculated clipmap, on z = 0
// cube x y z        one fillet on the ground. z is 0. Each line is its own cube.
// mesh              the OBJ pipeline, at the coordinate that pipeline fitted

// Demo data lives with the demo. The legacy path is still tried so a
// checkout that keeps world/ at the engine root keeps working.
static const char *k_path = "examples/demo/world/opening.world";
static const char *k_path_legacy = "world/opening.world";

static bool s_file_ground_seen = false;

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
        fprintf(stderr, "[World] %s:%d ground needs an axis\n", k_path, number);
        return -1;
    }
    if (strcmp(axis, "z") != 0) {
        fprintf(stderr, "[World] %s:%d ground %s is not drawn. The ground plane is z.\n",
                k_path, number, axis);
        return -1;
    }
    // The ground is on by default (universal metric grid), so naming it
    // in the file confirms the default instead of duplicating it. Only a
    // second ground record in the same file is an error; ground_set is
    // idempotent for the same plane, so track file records locally.
    if (s_file_ground_seen) {
        fprintf(stderr, "[World] %s:%d names the ground twice\n", k_path, number);
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
    char tail = 0;
    if (sscanf(line, "cube %f %f %f %c", &x, &y, &z, &tail) != 3) {
        fprintf(stderr, "[World] %s:%d cube needs x y z\n", k_path, number);
        return -1;
    }
    if (z != 0.0f) {
        fprintf(stderr, "[World] %s:%d cube z is %.3f. The fillet draw sits on the ground, z = 0.\n",
                k_path, number, (double)z);
        return -1;
    }
    if (!cube_kind_spawn((Coord){ x, y, z })) {
        return -1;
    }
    return 0;
}

static int place_mesh(const char *line, int number) {
    const char *rest = line + 4;
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }
    if (*rest != '\0') {
        fprintf(stderr, "[World] %s:%d mesh takes no arguments. The OBJ path is plots/mesh.obj.\n",
                k_path, number);
        return -1;
    }
    if (mesh_kind_wanted()) {
        fprintf(stderr, "[World] %s:%d names the mesh twice\n", k_path, number);
        return -1;
    }
    mesh_kind_set_wanted(true);
    mesh_kind_raise();
    return 0;
}

// Compile-time placements. Empty by default: the demo boots into the
// universal metric grid with zero objects. Register custom C objects
// here (ops + state pointers, or an init_fn) to spawn them at startup.
static const WorldBlueprint k_startup_objects[] = {
    // { .ops = &my_ops, .data = &my_state, .at = { 0.0f, 0.0f, 0.0f }, .spin = &my_state.spin },
};

static int place_blueprints(void) {
    const size_t n = sizeof(k_startup_objects) / sizeof(k_startup_objects[0]);
    for (size_t i = 0; i < n; ++i) {
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

static int read_world_file(const char *rel_path, char *buf, size_t cap, size_t *out_len) {
    char path[1024];
    if (!resolve_world_file(rel_path, path, sizeof path)) {
        return -1;
    }
    RingContext *ring = ring_get_context();
    if (ring != nullptr && ring->is_initialized) {
        int rc = ring_read_file_all(ring, path, buf, cap, out_len, nullptr, nullptr);
        if (rc == 0) {
            return 0;
        }
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    ssize_t n = read(fd, buf, cap - 1u);
    close(fd);
    if (n < 0) {
        return -1;
    }
    buf[n] = '\0';
    *out_len = (size_t)n;
    return 0;
}

int world_load(void) {
    s_file_ground_seen = false;
    if (place_blueprints() != 0) {
        return -1;
    }
    char buf[4096];
    size_t len = 0;
    if (read_world_file(k_path, buf, sizeof buf, &len) != 0 &&
        read_world_file(k_path_legacy, buf, sizeof buf, &len) != 0) {
        // No scene file: boot empty over the metric grid. The file is
        // an optional placement list, not a boot requirement.
        table_publish();
        return 0;
    }

    int number = 0;
    int rc = 0;
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
                    k_path, number, (unsigned)sizeof line);
            rc = -1;
            break;
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
            } else if (strncmp(p, "cube", 4) == 0) {
                step = place_cube(p, number);
            } else if (strncmp(p, "mesh", 4) == 0) {
                step = place_mesh(p, number);
            } else {
                fprintf(stderr, "[World] %s:%d unknown record\n", k_path, number);
                step = -1;
            }
            if (step != 0) {
                rc = -1;
                break;
            }
        }
        if (eol == nullptr) {
            break;
        }
        cursor = eol + 1;
    }
    if (rc == 0) {
        table_publish();
    }
    return rc;
}
