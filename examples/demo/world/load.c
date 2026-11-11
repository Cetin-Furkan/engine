#include "world/world_int.h"
#include "backend/backend.h"
#include "frame/camera.h"
#include "frame/gimbal.h"
#include "frame/hud.h"
#include "frame/plot.h"
#include "frame/policy.h"
#include "frame/sheet.h"
#include "frame/text.h"
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
// ground z | off           the hit plane or disabled metric grid.
// mode 3d | 2d | hybrid    workspace projection and sheet profile
// draw 0|1                 click-to-draw polygon face mode
// tab 0|1                  Tab 3D/2D/Hybrid mode switching permission
// sheet 0..9               2D physics sheet preset selection
// gimbal 0|1               3D corner orientation gimbal visibility
// hud 0|1                  GPU BDA telemetry readout visibility
// plot 0|1                 3D GPU math & CSV plot visibility
// text 0|1                 SDF text overlay visibility & KEY_T editor
// clear r g b              custom background clear color [0..1]
// camera x y z [yaw pitch [zoom [turntable]]]
// smooth 0|1               every solid, faceted or rounded
// cube x y z               center. A cube that has not been shoved keeps this point.
// cube x y z yaw N spin 0|1 inf 0|1 glow 0|1 [bounce B]
// solid Name x y z yaw N spin 0|1 inf 0|1 glow 0|1 [bounce B]
// ringcube cx cy cz radius count [bounce [glow]]
// gridcube cx cy z dx dy nx ny [bounce [glow]]
// tower x y z step_z count [bounce]
// arcsolid cx cy cz radius dz [b_min b_max]
// solidring cx cy cz radius [b_min b_max]
// helix cx cy cz radius step_z count [bounce]
// mesh                     the OBJ, one turn from yaw 0
// mesh yaw N spin 0|1 inf 0|1 glow 0|1
// gone cube x y z yaw N spin 0|1 inf 0|1 glow 0|1 [bounce B]
// gone solid Name x y z yaw N spin 0|1 inf 0|1 glow 0|1 [bounce B]
// gone mesh yaw N spin 0|1 inf 0|1 glow 0|1
// A gone line is not drawn. Z puts the newest one back.
// Click-built faces and the heat field are not records.

static const char *k_path = "examples/demo/world/opening.world";
static const char *k_path_legacy = "world/opening.world";
static const char *k_live = "examples/demo/world/live.world";
static const char *k_live_legacy = "world/live.world";

static const char *k_preset_worlds[6] = {
    // World 1: 3D-Only Click-to-Draw Studio (open central court framed by glowing pillars)
    "mode 3d\n"
    "tab 0\n"
    "draw 1\n"
    "sheet 1\n"
    "ground z\n"
    "smooth 0\n"
    "solid Tetrahedron -520 260 0 yaw 25000 spin 1 inf 0 glow 1 bounce 0.45\n"
    "solid Octahedron 520 260 0 yaw 45000 spin 1 inf 0 glow 1 bounce 0.68\n"
    "solid Cone -520 -260 0 yaw 15000 spin 1 inf 0 glow 1 bounce 0.56\n"
    "solid Torus 520 -260 0 yaw 65000 spin 1 inf 0 glow 1 bounce 0.82\n"
    "cube 0 340 0 yaw 38000 spin 1 inf 0 glow 0 bounce 0.50\n",

    // World 2: 3D-Only Gravity Drop & Elastic Non-Cube Solids (elevated drop with varied bounce)
    "mode 3d\n"
    "tab 0\n"
    "draw 0\n"
    "sheet 2\n"
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

    // World 3: 2D-Only Gravity & Soft Bodies (pure 2D sheet, zero 3D objects)
    "mode 2d\n"
    "tab 0\n"
    "draw 0\n"
    "sheet 3\n"
    "ground z\n",

    // World 4: 2D-Only Click-to-Draw Polygon Workshop (pure 2D sheet, zero 3D objects)
    "mode 2d\n"
    "tab 0\n"
    "draw 1\n"
    "sheet 4\n"
    "ground z\n",

    // World 5: Hybrid (3D + 2D) Click-to-Draw + Gravity & Multi-Elasticity
    "mode hybrid\n"
    "tab 1\n"
    "draw 1\n"
    "sheet 5\n"
    "ground z\n"
    "smooth 1\n"
    "solid Icosahedron -420 120 580 yaw 22000 spin 1 inf 1 glow 1 bounce 0.90\n"
    "solid Torus 420 120 520 yaw 48000 spin 1 inf 1 glow 1 bounce 0.84\n"
    "solid Octahedron -420 -160 440 yaw 15000 spin 1 inf 0 glow 0 bounce 0.66\n"
    "solid Cone 420 -160 380 yaw 35000 spin 1 inf 0 glow 0 bounce 0.52\n"
    "cube 0 40 620 yaw 38000 spin 1 inf 0 glow 1 bounce 0.82\n",

    // World 6: 3D + 2D Switchable Restitution Staircase & Collision Arena
    "mode 3d\n"
    "tab 1\n"
    "draw 0\n"
    "sheet 6\n"
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

const char *world_source_path(void) {
    if (world_selected_id() == 8u && s_source[0] == '\0') {
        const char *mpath = backend_survey_manifest_path();
        if (mpath != nullptr && mpath[0] != '\0' && access(mpath, R_OK) == 0) {
            return mpath;
        }
    }
    if (s_source[0] == '\0' ||
        strncmp(s_source, "builtin:", 8) == 0 ||
        strcmp(s_source, "memory.world") == 0) {
        return nullptr;
    }
    return access(s_source, R_OK) == 0 ? s_source : nullptr;
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
        fprintf(stderr, "[World] %s:%d ground needs an axis or off\n", source_name(), number);
        return -1;
    }
    if (s_file_ground_seen) {
        fprintf(stderr, "[World] %s:%d names the ground twice\n", source_name(), number);
        return -1;
    }
    if (strcmp(axis, "off") == 0 || strcmp(axis, "0") == 0) {
        s_file_ground_seen = true;
        ground_set_visible(false);
        return 0;
    }
    if (strcmp(axis, "z") != 0) {
        fprintf(stderr, "[World] %s:%d ground %s is not drawn. The ground plane is z.\n",
                source_name(), number, axis);
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
    world_adopt_mesh();
    return 0;
}

static int place_gone(const char *line, int number) {
    const char *rest = line + 4;
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }
    WorldRemoved g = {0};
    char tail = 0;
    if (strncmp(rest, "cube", 4) == 0 && (rest[4] == ' ' || rest[4] == '\t' || rest[4] == '\0')) {
        int yaw = 0;
        int spinning = 0;
        int infinite = 0;
        int glow = 0;
        float bounce = 0.0f;
        int got_b = sscanf(rest, "cube %f %f %f yaw %d spin %d inf %d glow %d bounce %f %c",
                           &g.at.x, &g.at.y, &g.at.z, &yaw, &spinning, &infinite, &glow, &bounce, &tail);
        int got = (got_b == 8) ? 8 : sscanf(rest, "cube %f %f %f yaw %d spin %d inf %d glow %d %c",
                                            &g.at.x, &g.at.y, &g.at.z, &yaw, &spinning, &infinite, &glow, &tail);
        if ((got != 7 && got != 8) || !flag_bit(spinning) || !flag_bit(infinite) || !flag_bit(glow) ||
            (got == 8 && !isfinite(bounce))) {
            fprintf(stderr, "[World] %s:%d gone cube needs x y z yaw spin inf glow [bounce]\n",
                    source_name(), number);
            return -1;
        }
        if (!isfinite(g.at.x) || !isfinite(g.at.y) || !isfinite(g.at.z)) {
            fprintf(stderr, "[World] %s:%d cube position is not finite\n", source_name(), number);
            return -1;
        }
        g.kind = k_removed_cube;
        g.yaw_milli = (int32_t)yaw;
        g.restitution = got == 8 ? bounce : 0.0f;
        g.spinning = spinning != 0;
        g.infinite = infinite != 0;
        g.glow = glow != 0;
    } else if (strncmp(rest, "solid", 5) == 0 && (rest[5] == ' ' || rest[5] == '\t' || rest[5] == '\0')) {
        char name[32] = {0};
        int yaw = 0;
        int spinning = 0;
        int infinite = 0;
        int glow = 0;
        float bounce = -1.0f;
        int got_b = sscanf(rest, "solid %31s %f %f %f yaw %d spin %d inf %d glow %d bounce %f %c",
                           name, &g.at.x, &g.at.y, &g.at.z, &yaw, &spinning, &infinite, &glow, &bounce, &tail);
        int got = (got_b == 9) ? 9 : sscanf(rest, "solid %31s %f %f %f yaw %d spin %d inf %d glow %d %c",
                                            name, &g.at.x, &g.at.y, &g.at.z, &yaw, &spinning, &infinite, &glow, &tail);
        if ((got != 8 && got != 9) || !flag_bit(spinning) || !flag_bit(infinite) || !flag_bit(glow) ||
            (got == 9 && !isfinite(bounce))) {
            fprintf(stderr, "[World] %s:%d gone solid needs Name x y z yaw spin inf glow [bounce]\n",
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
        g.restitution = got == 9 ? bounce : -1.0f;
        g.spinning = spinning != 0;
        g.infinite = infinite != 0;
        g.glow = glow != 0;
    } else if (strncmp(rest, "mesh", 4) == 0 && (rest[4] == ' ' || rest[4] == '\t' || rest[4] == '\0')) {
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

static int place_ringcube(const char *line, int number) {
    float cx = 0.0f;
    float cy = 0.0f;
    float cz = 0.0f;
    float radius = 0.0f;
    unsigned count = 0u;
    float bounce = -1.0f;
    int glow = 1;
    char tail = 0;
    int got = sscanf(line, "ringcube %f %f %f %f %u %f %d %c",
                     &cx, &cy, &cz, &radius, &count, &bounce, &glow, &tail);
    if ((got != 5 && got != 6 && got != 7) || (got == 7 && !flag_bit(glow))) {
        fprintf(stderr, "[World] %s:%d ringcube needs cx cy cz radius count [bounce [glow]]\n",
                source_name(), number);
        return -1;
    }
    if (!world_builder_add_cube_ring(nullptr, (Coord){ cx, cy, cz }, radius, count,
                                     got >= 6 ? bounce : -1.0f, got == 7 ? (glow != 0) : true)) {
        fprintf(stderr, "[World] %s:%d ringcube placement refused\n", source_name(), number);
        return -1;
    }
    return 0;
}

static int place_gridcube(const char *line, int number) {
    float cx = 0.0f;
    float cy = 0.0f;
    float cz = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;
    unsigned nx = 0u;
    unsigned ny = 0u;
    float bounce = -1.0f;
    int glow = 1;
    char tail = 0;
    int got = sscanf(line, "gridcube %f %f %f %f %f %u %u %f %d %c",
                     &cx, &cy, &cz, &dx, &dy, &nx, &ny, &bounce, &glow, &tail);
    if ((got != 7 && got != 8 && got != 9) || (got == 9 && !flag_bit(glow))) {
        fprintf(stderr, "[World] %s:%d gridcube needs cx cy z dx dy nx ny [bounce [glow]]\n",
                source_name(), number);
        return -1;
    }
    if (!world_builder_add_cube_grid(nullptr, (Coord){ cx, cy, cz }, dx, dy, nx, ny,
                                     got >= 8 ? bounce : -1.0f, got == 9 ? (glow != 0) : true)) {
        fprintf(stderr, "[World] %s:%d gridcube placement refused\n", source_name(), number);
        return -1;
    }
    return 0;
}

static int place_tower(const char *line, int number) {
    float bx = 0.0f;
    float by = 0.0f;
    float bz = 0.0f;
    float step_z = 0.0f;
    unsigned count = 0u;
    float bounce = -1.0f;
    char tail = 0;
    int got = sscanf(line, "tower %f %f %f %f %u %f %c",
                     &bx, &by, &bz, &step_z, &count, &bounce, &tail);
    if (got != 5 && got != 6) {
        fprintf(stderr, "[World] %s:%d tower needs x y z step_z count [bounce]\n",
                source_name(), number);
        return -1;
    }
    if (!world_builder_add_cube_tower(nullptr, (Coord){ bx, by, bz }, step_z, count,
                                      got == 6 ? bounce : -1.0f)) {
        fprintf(stderr, "[World] %s:%d tower placement refused\n", source_name(), number);
        return -1;
    }
    return 0;
}

static int place_arcsolid(const char *line, int number) {
    float cx = 0.0f;
    float cy = 0.0f;
    float cz = 0.0f;
    float radius = 0.0f;
    float dz = 0.0f;
    float b0 = 0.18f;
    float b1 = 0.92f;
    char tail = 0;
    int got = sscanf(line, "arcsolid %f %f %f %f %f %f %f %c",
                     &cx, &cy, &cz, &radius, &dz, &b0, &b1, &tail);
    if (got != 5 && got != 7) {
        fprintf(stderr, "[World] %s:%d arcsolid needs cx cy cz radius dz [b_min b_max]\n",
                source_name(), number);
        return -1;
    }
    if (!world_builder_add_solid_arc(nullptr, (Coord){ cx, cy, cz }, radius, dz,
                                     got == 7 ? b0 : 0.18f, got == 7 ? b1 : 0.92f)) {
        fprintf(stderr, "[World] %s:%d arcsolid placement refused\n", source_name(), number);
        return -1;
    }
    return 0;
}

static int place_solidring(const char *line, int number) {
    float cx = 0.0f;
    float cy = 0.0f;
    float cz = 0.0f;
    float radius = 0.0f;
    float b0 = 0.18f;
    float b1 = 0.92f;
    char tail = 0;
    int got = sscanf(line, "solidring %f %f %f %f %f %f %c",
                     &cx, &cy, &cz, &radius, &b0, &b1, &tail);
    if (got != 4 && got != 6) {
        fprintf(stderr, "[World] %s:%d solidring needs cx cy cz radius [b_min b_max]\n",
                source_name(), number);
        return -1;
    }
    if (!world_builder_add_solid_ring(nullptr, (Coord){ cx, cy, cz }, radius,
                                      got == 6 ? b0 : 0.18f, got == 6 ? b1 : 0.92f)) {
        fprintf(stderr, "[World] %s:%d solidring placement refused\n", source_name(), number);
        return -1;
    }
    return 0;
}

static int place_helix(const char *line, int number) {
    float cx = 0.0f;
    float cy = 0.0f;
    float cz = 0.0f;
    float radius = 0.0f;
    float step_z = 0.0f;
    unsigned count = 0u;
    float bounce = -1.0f;
    char tail = 0;
    int got = sscanf(line, "helix %f %f %f %f %f %u %f %c",
                     &cx, &cy, &cz, &radius, &step_z, &count, &bounce, &tail);
    if (got != 6 && got != 7) {
        fprintf(stderr, "[World] %s:%d helix needs cx cy cz radius step_z count [bounce]\n",
                source_name(), number);
        return -1;
    }
    if (!world_builder_add_helix(nullptr, (Coord){ cx, cy, cz }, radius, step_z, count,
                                 got == 7 ? bounce : -1.0f)) {
        fprintf(stderr, "[World] %s:%d helix placement refused\n", source_name(), number);
        return -1;
    }
    return 0;
}

static int place_mode(const char *line, int number) {
    char name[16] = {0};
    char tail = 0;
    if (sscanf(line, "mode %15s %c", name, &tail) != 1) {
        fprintf(stderr, "[World] %s:%d mode needs 3d, 2d, or hybrid\n", source_name(), number);
        return -1;
    }
    uint8_t mode = (uint8_t)MODE_3D;
    if (strcmp(name, "3d") == 0 || strcmp(name, "3D") == 0) {
        mode = (uint8_t)MODE_3D;
    } else if (strcmp(name, "2d") == 0 || strcmp(name, "2D") == 0) {
        mode = (uint8_t)MODE_2D;
    } else if (strcmp(name, "hybrid") == 0 || strcmp(name, "Hybrid") == 0 || strcmp(name, "HYBRID") == 0) {
        mode = (uint8_t)MODE_HYBRID;
    } else {
        fprintf(stderr, "[World] %s:%d unknown mode %s (expected 3d, 2d, or hybrid)\n",
                source_name(), number, name);
        return -1;
    }
    WorldBuilder wb = {0};
    world_builder_set_mode(&wb, mode);
    if (wb.failed) {
        fprintf(stderr, "[World] %s:%d mode change refused\n", source_name(), number);
        return -1;
    }
    return 0;
}

static int place_draw(const char *line, int number) {
    int on = 0;
    char tail = 0;
    if (sscanf(line, "draw %d %c", &on, &tail) != 1 || !flag_bit(on)) {
        fprintf(stderr, "[World] %s:%d draw needs 0 or 1\n", source_name(), number);
        return -1;
    }
    WorldBuilder wb = {0};
    world_builder_set_draw(&wb, on != 0);
    if (wb.failed) {
        fprintf(stderr, "[World] %s:%d draw permission change refused\n", source_name(), number);
        return -1;
    }
    return 0;
}

static int place_tab(const char *line, int number) {
    int on = 0;
    char tail = 0;
    if (sscanf(line, "tab %d %c", &on, &tail) != 1 || !flag_bit(on)) {
        fprintf(stderr, "[World] %s:%d tab needs 0 or 1\n", source_name(), number);
        return -1;
    }
    WorldBuilder wb = {0};
    world_builder_set_tab(&wb, on != 0);
    if (wb.failed) {
        fprintf(stderr, "[World] %s:%d tab permission change refused\n", source_name(), number);
        return -1;
    }
    return 0;
}

static int place_gimbal(const char *line, int number) {
    int on = 0;
    char tail = 0;
    if (sscanf(line, "gimbal %d %c", &on, &tail) != 1 || !flag_bit(on)) {
        fprintf(stderr, "[World] %s:%d gimbal needs 0 or 1\n", source_name(), number);
        return -1;
    }
    gimbal_set_visible(on != 0);
    return 0;
}

static int place_hud(const char *line, int number) {
    int on = 0;
    char tail = 0;
    if (sscanf(line, "hud %d %c", &on, &tail) != 1 || !flag_bit(on)) {
        fprintf(stderr, "[World] %s:%d hud needs 0 or 1\n", source_name(), number);
        return -1;
    }
    hud_set_visible(on != 0);
    return 0;
}

static int place_plot(const char *line, int number) {
    int on = 0;
    char tail = 0;
    if (sscanf(line, "plot %d %c", &on, &tail) != 1 || !flag_bit(on)) {
        fprintf(stderr, "[World] %s:%d plot needs 0 or 1\n", source_name(), number);
        return -1;
    }
    world_builder_set_plot(nullptr, on != 0);
    return 0;
}

static int place_text(const char *line, int number) {
    int on = 0;
    char tail = 0;
    if (sscanf(line, "text %d %c", &on, &tail) != 1 || !flag_bit(on)) {
        fprintf(stderr, "[World] %s:%d text needs 0 or 1\n", source_name(), number);
        return -1;
    }
    text_set_visible(on != 0);
    return 0;
}

static int place_clear(const char *line, int number) {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    char tail = 0;
    if (sscanf(line, "clear %f %f %f %c", &r, &g, &b, &tail) != 3 ||
        !isfinite(r) || !isfinite(g) || !isfinite(b) ||
        r < 0.0f || r > 1.0f || g < 0.0f || g > 1.0f || b < 0.0f || b > 1.0f) {
        fprintf(stderr, "[World] %s:%d clear needs r g b in [0..1]\n", source_name(), number);
        return -1;
    }
    world_set_clear_color(r, g, b, 1.0f);
    return 0;
}

static int place_camera(const char *line, int number) {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float zoom = 1.0f;
    int turntable = 0;
    char tail = 0;
    int got = sscanf(line, "camera %f %f %f %f %f %f %d %c",
                     &x, &y, &z, &yaw, &pitch, &zoom, &turntable, &tail);
    if ((got != 3 && got != 5 && got != 6 && got != 7) ||
        !isfinite(x) || !isfinite(y) || !isfinite(z) ||
        (got >= 5 && (!isfinite(yaw) || !isfinite(pitch))) ||
        (got >= 6 && (!isfinite(zoom) || zoom <= 0.0f)) ||
        (got == 7 && !flag_bit(turntable))) {
        fprintf(stderr, "[World] %s:%d camera needs x y z [yaw pitch [zoom [turntable]]]\n",
                source_name(), number);
        return -1;
    }
    if (got >= 5 && (fabsf(yaw) > 6.2831855f || fabsf(pitch) > 1.5707964f)) {
        yaw   *= 0.017453292519943295f;
        pitch *= 0.017453292519943295f;
    }
    camera_set_turntable_mode(got == 7 ? (turntable != 0) : false);
    camera_set_home_pose(x, y, z, got >= 5 ? yaw : 0.0f, got >= 5 ? pitch : 0.0f,
                         got >= 6 ? zoom : 1.0f);
    return 0;
}

// Compile-time placements. Empty by default: the pure engine boots with
// literally nothing. Register custom C objects here or construct via worlds.
#define STARTUP_BLUEPRINTS_COUNT 0
#if STARTUP_BLUEPRINTS_COUNT > 0
static const WorldBlueprint k_startup_objects[STARTUP_BLUEPRINTS_COUNT] = {
    // { .ops = &my_ops, .data = &my_state, .at = { 0.0f, 0.0f, 0.0f }, .spin = &my_state.spin },
};
#endif

static int place_blueprints(void) {
#if STARTUP_BLUEPRINTS_COUNT > 0
    for (size_t i = 0; i < STARTUP_BLUEPRINTS_COUNT; ++i) {
        if (!world_builder_add_blueprint(nullptr, k_startup_objects[i])) {
            return -1;
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
    if (wid == 0u) {
        // World 0 is the pure empty engine canvas: literally nothing is loaded.
        return 4;
    }
    if (wid == k_world_custom_file) {
        const char *custom = world_custom_path();
        if (custom == nullptr || custom[0] == '\0') {
            return -1;
        }
        int rc = read_named(custom, buf, cap, len);
        if (rc != 0) {
            fprintf(stderr, "[World] Custom world file '%s' could not be read.\n", custom);
            return -1;
        }
        return 0;
    }
    if (wid >= 1u && wid <= 8u) {
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
        if (wid == 7u) {
            // World 7 is constructed directly in C23 via WorldBuilder when no
            // external world7.world override file is present.
            return 2;
        }
        if (wid == 8u) {
            // World 8 boots the Baklan & Beşparmak Dağı streamed DEM + satellite map.
            return 3;
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

static bool match_kw(const char *p, const char *kw) {
    size_t n = strlen(kw);
    if (strncmp(p, kw, n) != 0) {
        return false;
    }
    return p[n] == ' ' || p[n] == '\t' || p[n] == '\0';
}

static int place_terrain(const char *line, int number) {
    char tail = 0;
    if (sscanf(line, "terrain %c", &tail) == 1) {
        fprintf(stderr, "[World] %s:%d terrain takes no arguments\n", source_name(), number);
        return -1;
    }
    return terrain_kind_spawn() ? 0 : -1;
}

static int place_contours(const char *line, int number) {
    int on = 0;
    char tail = 0;
    if (sscanf(line, "contours %d %c", &on, &tail) != 1 || !flag_bit(on)) {
        fprintf(stderr, "[World] %s:%d contours needs 0 or 1\n", source_name(), number);
        return -1;
    }
    world_builder_set_contours(nullptr, on != 0);
    return 0;
}

static int place_sheet(const char *line, int number) {
    unsigned id = 0u;
    char tail = 0;
    if (sscanf(line, "sheet %u %c", &id, &tail) != 1 || id > 9u) {
        fprintf(stderr, "[World] %s:%d sheet needs 0..9\n", source_name(), number);
        return -1;
    }
    sheet_select_world(id);
    if (sheet_load() != 0) {
        fprintf(stderr, "[World] %s:%d sheet %u failed to load\n", source_name(), number, id);
        return -1;
    }
    return 0;
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
            if (match_kw(p, "ground")) {
                step = place_ground(p, number);
            } else if (match_kw(p, "mode")) {
                step = place_mode(p, number);
            } else if (match_kw(p, "draw")) {
                step = place_draw(p, number);
            } else if (match_kw(p, "tab")) {
                step = place_tab(p, number);
            } else if (match_kw(p, "sheet")) {
                step = place_sheet(p, number);
            } else if (match_kw(p, "gimbal")) {
                step = place_gimbal(p, number);
            } else if (match_kw(p, "hud")) {
                step = place_hud(p, number);
            } else if (match_kw(p, "plot")) {
                step = place_plot(p, number);
            } else if (match_kw(p, "text")) {
                step = place_text(p, number);
            } else if (match_kw(p, "clear")) {
                step = place_clear(p, number);
            } else if (match_kw(p, "camera")) {
                step = place_camera(p, number);
            } else if (match_kw(p, "smooth")) {
                step = place_smooth(p, number);
            } else if (match_kw(p, "contours")) {
                step = place_contours(p, number);
            } else if (match_kw(p, "ringcube")) {
                step = place_ringcube(p, number);
            } else if (match_kw(p, "gridcube")) {
                step = place_gridcube(p, number);
            } else if (match_kw(p, "tower")) {
                step = place_tower(p, number);
            } else if (match_kw(p, "arcsolid")) {
                step = place_arcsolid(p, number);
            } else if (match_kw(p, "solidring")) {
                step = place_solidring(p, number);
            } else if (match_kw(p, "helix")) {
                step = place_helix(p, number);
            } else if (match_kw(p, "cube")) {
                step = place_cube(p, number);
            } else if (match_kw(p, "solid")) {
                step = place_solid(p, number);
            } else if (match_kw(p, "terrain")) {
                step = place_terrain(p, number);
            } else if (match_kw(p, "mesh")) {
                step = place_mesh(p, number);
            } else if (match_kw(p, "gone")) {
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

int world_load_string(const char *doc, const char *source_label) {
    if (doc == nullptr) {
        return -1;
    }
    size_t len = strlen(doc);
    char buf[16384];
    if (len + 1u >= sizeof buf) {
        return -1;
    }
    memcpy(buf, doc, len + 1u);
    bool outer_loading = s_loading;
    s_loading = true;
    world_removed_clear();
    if (!outer_loading) {
        table_reset();
        ground_reset();
        face_reset();
        table_kinds_reset();
        solid_kind_reset();
        mesh_kind_reset();
        terrain_kind_reset();
        spin_clear();
    }
    ground_set(2, 0.0f);
    gimbal_set_visible(true);
    hud_set_visible(true);
    plot_set_visible(world_selected_id() == 9u);
    text_set_visible(true);
    world_set_clear_color(0.08f, 0.24f, 0.48f, 1.0f);
    s_file_ground_seen = false;
    s_smooth_seen = false;
    snprintf(s_source, sizeof s_source, "%s",
             (source_label != nullptr && source_label[0] != '\0') ? source_label : "memory.world");
    int rc = parse_document(buf, len);
    if (rc == 0) {
        table_publish();
    } else {
        table_reset();
        face_reset();
        table_kinds_reset();
        solid_kind_reset();
        mesh_kind_reset();
        terrain_kind_reset();
        spin_clear();
        world_removed_clear();
    }
    if (!outer_loading) {
        s_loading = false;
        s_loaded = (rc == 0);
    }
    return rc;
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
        if (doc == 4) {
            // Pure empty engine state: zero objects, zero ground, zero overlays.
            ground_set_visible(false);
            gimbal_set_visible(false);
            hud_set_visible(false);
            plot_set_visible(false);
            text_set_visible(false);
            world_set_clear_color(0.015f, 0.018f, 0.024f, 1.0f);
            table_publish();
            printf("[World] Pure Engine booted with literally nothing (0 objects, 0 overlays). Pass --world=1..9 to construct a world.\n");
            fflush(stdout);
            rc = 0;
        } else if (doc == 1) {
            table_publish();
            rc = 0;
        } else if (doc == 2) {
            rc = world_build_procedural_cathedral();
            if (rc == 0) {
                printf("[World] Loaded World %u (%s) via C23 WorldBuilder\n",
                       world_selected_id(), world_selected_title());
                fflush(stdout);
            }
        } else if (doc == 3) {
            rc = world_build_besparmak_terrain();
            if (rc == 0) {
                printf("[World] Loaded World %u (%s) via GPU-Math Terrain Backend\n",
                       world_selected_id(), world_selected_title());
                fflush(stdout);
            }
        } else if (doc == 0) {
            char src[1024];
            snprintf(src, sizeof src, "%s", source_name());
            rc = world_load_string(buf, src);
            if (rc == 0) {
                if (world_selected_id() == k_world_custom_file) {
                    const WorkspacePolicy *pol = policy_active();
                    printf("[World] Active #%u: %s | Mode=%s (Tab switch=%s, click-draw=%s)\n",
                           world_selected_id(), world_selected_title(), pol->name,
                           policy_allows(PERM_MODE_SWITCH) ? "on" : "locked",
                           policy_allows(PERM_DRAW) ? "on" : "off");
                }
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
        float b = cube_kind_restitution(o);
        if (b > 0.0f) {
            return append_line(body, cap, used,
                               "cube %.9g %.9g %.9g yaw %d spin %d inf %d glow %d bounce %.9g\n",
                               (double)o->at.x, (double)o->at.y, (double)o->at.z,
                               yaw, spinning, infinite, glow, (double)b);
        }
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
        float b = solid_kind_restitution(o);
        return append_line(body, cap, used,
                           "solid %s %.9g %.9g %.9g yaw %d spin %d inf %d glow %d bounce %.9g\n",
                           name, (double)o->at.x, (double)o->at.y, (double)o->at.z,
                           yaw, spinning, infinite, glow, (double)b);
    }
    if (o->ops == mesh_ops()) {
        return append_line(body, cap, used, "mesh yaw %d spin %d inf %d glow %d\n",
                           yaw, spinning, infinite, glow);
    }
    if (o->ops == terrain_ops()) {
        return append_line(body, cap, used, "terrain\n");
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
    if (s_loading || !s_loaded || world_selected_id() != 9u) {
        return 0;
    }
    char body[16384];
    size_t used = 0;
    if (append_line(body, sizeof body, &used,
                    "# Live placement. opening.world is not overwritten.\n"
                    "# Click-built faces and the heat field are not stored.\n"
                    "# A gone line waits for Z.\n"
                    "ground %s\n"
                    "smooth %d\n",
                    ground_is_visible() ? "z" : "off",
                    solid_kind_smooth() ? 1 : 0) != 0) {
        fprintf(stderr, "[World] The live placement does not fit.\n");
        return -1;
    }
    if ((!gimbal_is_visible() && append_line(body, sizeof body, &used, "gimbal 0\n") != 0) ||
        (!hud_is_visible() && append_line(body, sizeof body, &used, "hud 0\n") != 0) ||
        (!plot_is_visible() && append_line(body, sizeof body, &used, "plot 0\n") != 0) ||
        (!text_is_visible() && append_line(body, sizeof body, &used, "text 0\n") != 0)) {
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
            if (g->restitution > 0.0f) {
                step = append_line(body, sizeof body, &used,
                                   "gone cube %.9g %.9g %.9g yaw %d spin %d inf %d glow %d bounce %.9g\n",
                                   (double)g->at.x, (double)g->at.y, (double)g->at.z,
                                   (int)g->yaw_milli, g->spinning ? 1 : 0, g->infinite ? 1 : 0,
                                   g->glow ? 1 : 0, (double)g->restitution);
            } else {
                step = append_line(body, sizeof body, &used,
                                   "gone cube %.9g %.9g %.9g yaw %d spin %d inf %d glow %d\n",
                                   (double)g->at.x, (double)g->at.y, (double)g->at.z,
                                   (int)g->yaw_milli, g->spinning ? 1 : 0, g->infinite ? 1 : 0,
                                   g->glow ? 1 : 0);
            }
        } else if (g != nullptr && g->kind == k_removed_solid) {
            const char *name = solid_kind_name((uint32_t)g->solid);
            if (name == nullptr) {
                continue;
            }
            if (g->restitution >= 0.0f) {
                step = append_line(body, sizeof body, &used,
                                   "gone solid %s %.9g %.9g %.9g yaw %d spin %d inf %d glow %d bounce %.9g\n",
                                   name, (double)g->at.x, (double)g->at.y, (double)g->at.z,
                                   (int)g->yaw_milli, g->spinning ? 1 : 0, g->infinite ? 1 : 0,
                                   g->glow ? 1 : 0, (double)g->restitution);
            } else {
                step = append_line(body, sizeof body, &used,
                                   "gone solid %s %.9g %.9g %.9g yaw %d spin %d inf %d glow %d\n",
                                   name, (double)g->at.x, (double)g->at.y, (double)g->at.z,
                                   (int)g->yaw_milli, g->spinning ? 1 : 0, g->infinite ? 1 : 0,
                                   g->glow ? 1 : 0);
            }
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
