#include "world/world_int.h"
#include <stdio.h>
#include <string.h>

// world/opening.world. One record a line. A # starts a comment.
// ground z          the calculated clipmap, on z = 0
// cube x y z        one fillet. z is stored. The draw sits on the ground, so z is 0.
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

int world_load(void) {
    s_file_ground_seen = false;
    if (place_blueprints() != 0) {
        return -1;
    }
    FILE *f = fopen(k_path, "r");
    if (f == nullptr) {
        f = fopen(k_path_legacy, "r");
    }
    if (f == nullptr) {
        // No scene file: boot empty over the metric grid. The file is
        // an optional placement list, not a boot requirement.
        table_publish();
        return 0;
    }
    char line[256];
    int number = 0;
    int rc = 0;
    while (fgets(line, (int)sizeof line, f) != nullptr) {
        number++;
        if (strchr(line, '\n') == nullptr && !feof(f)) {
            fprintf(stderr, "[World] %s:%d is longer than %u bytes\n",
                    k_path, number, (unsigned)sizeof line);
            rc = -1;
            break;
        }
        trim(line);
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            continue;
        }
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
    fclose(f);
    if (rc == 0) {
        table_publish();
    }
    return rc;
}
