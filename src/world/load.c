#include "world/world_int.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

// world/opening.world. One record a line. A # starts a comment.
// ground z          the calculated clipmap, on z = 0
// cube x y z        one fillet. z is stored. The draw sits on the ground, so z is 0.
// mesh              the OBJ pipeline, at the coordinate that pipeline fitted

static const char *k_path = "world/opening.world";

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
    if (coord_ground_set()) {
        fprintf(stderr, "[World] %s:%d names the ground twice\n", k_path, number);
        return -1;
    }
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

int world_load(void) {
    FILE *f = fopen(k_path, "r");
    if (f == nullptr) {
        fprintf(stderr, "[World] %s was not opened: %s\n", k_path, strerror(errno));
        return -1;
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
