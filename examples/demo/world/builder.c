#include "world/world_int.h"
#include "backend/backend.h"
#include "frame/camera.h"
#include <math.h>
#include <stdio.h>

static constexpr float k_two_pi = 6.28318530717958647692f;
static constexpr float k_pi     = 3.14159265358979323846f;
static constexpr float k_rad_to_milli_deg = 57295.7795130823208768f;

void world_builder_begin(WorldBuilder *wb, bool smooth_solids) {
    if (wb == nullptr) {
        return;
    }
    *wb = (WorldBuilder){
        .cubes_placed      = 0u,
        .solids_placed     = 0u,
        .blueprints_placed = 0u,
        .smooth_solids     = smooth_solids,
        .failed            = false,
    };
    ground_set(2, 0.0f);
    solid_kind_set_smooth(smooth_solids);
}

bool world_builder_add_blueprint(WorldBuilder *wb, WorldBlueprint bp) {
    if (wb != nullptr && wb->failed) {
        return false;
    }
    bool ok = false;
    if (bp.init_fn != nullptr) {
        ok = bp.init_fn(bp.at, bp.data);
    } else if (bp.ops != nullptr) {
        ok = (table_add(bp.ops, bp.data, bp.at, bp.spin) >= 0);
    }
    if (!ok) {
        if (wb != nullptr) {
            wb->failed = true;
        }
        return false;
    }
    if (wb != nullptr) {
        wb->blueprints_placed++;
    }
    return true;
}

bool world_builder_add_cube(WorldBuilder *wb, Coord at, int32_t yaw_milli,
                            bool spinning, bool infinite, bool glow, float restitution) {
    if (wb != nullptr && wb->failed) {
        return false;
    }
    if (!isfinite(at.x) || !isfinite(at.y) || !isfinite(at.z) || !isfinite(restitution)) {
        if (wb != nullptr) {
            wb->failed = true;
        }
        return false;
    }
    if (!cube_kind_spawn_elastic(at, yaw_milli, spinning, infinite, glow, restitution)) {
        if (wb != nullptr) {
            wb->failed = true;
        }
        return false;
    }
    if (wb != nullptr) {
        wb->cubes_placed++;
    }
    return true;
}

bool world_builder_add_solid(WorldBuilder *wb, uint32_t kind, Coord at, int32_t yaw_milli,
                             bool spinning, bool infinite, bool glow, float restitution) {
    if (wb != nullptr && wb->failed) {
        return false;
    }
    if (kind >= 6u || !isfinite(at.x) || !isfinite(at.y) || !isfinite(at.z) ||
        !isfinite(restitution)) {
        if (wb != nullptr) {
            wb->failed = true;
        }
        return false;
    }
    if (!solid_kind_place_elastic(kind, at, yaw_milli, spinning, infinite, glow, restitution)) {
        if (wb != nullptr) {
            wb->failed = true;
        }
        return false;
    }
    if (wb != nullptr) {
        wb->solids_placed++;
    }
    return true;
}

bool world_builder_add_cube_ring(WorldBuilder *wb, Coord center, float radius,
                                 uint32_t count, float restitution, bool glow) {
    if (count == 0u || count > 16u || !isfinite(radius) || radius < 80.0f) {
        if (wb != nullptr) {
            wb->failed = true;
        }
        return false;
    }
    for (uint32_t i = 0u; i < count; ++i) {
        float t = (float)i / (float)count;
        float angle = t * k_two_pi;
        Coord pos = {
            .x = center.x + cosf(angle) * radius,
            .y = center.y + sinf(angle) * radius,
            .z = center.z,
        };
        int32_t yaw_milli = (int32_t)lrintf(angle * k_rad_to_milli_deg);
        float item_bounce = restitution;
        if (restitution < 0.0f) {
            // Automatic restitution gradient around the ring: [0.18 .. 0.90]
            item_bounce = 0.18f + 0.72f * t;
        }
        bool item_glow = glow ? ((i & 1u) == 0u) : false;
        if (!world_builder_add_cube(wb, pos, yaw_milli, true, (i % 3u) == 0u,
                                    item_glow, item_bounce)) {
            return false;
        }
    }
    return true;
}

bool world_builder_add_cube_tower(WorldBuilder *wb, Coord base, float step_z,
                                  uint32_t count, float restitution) {
    if (count == 0u || count > 12u || !isfinite(step_z) || step_z < 120.0f) {
        if (wb != nullptr) {
            wb->failed = true;
        }
        return false;
    }
    for (uint32_t i = 0u; i < count; ++i) {
        Coord pos = {
            .x = base.x,
            .y = base.y,
            .z = base.z + (float)i * step_z,
        };
        int32_t yaw_milli = (int32_t)(18000 + i * 22500u);
        float b = restitution >= 0.0f ? restitution : (0.25f + 0.14f * (float)i);
        if (!world_builder_add_cube(wb, pos, yaw_milli, true, false, (i + 1u) == count, b)) {
            return false;
        }
    }
    return true;
}

bool world_builder_add_solid_arc(WorldBuilder *wb, Coord center, float radius,
                                 float dz, float bounce_min, float bounce_max) {
    if (!isfinite(radius) || radius < 80.0f || !isfinite(dz) ||
        !isfinite(bounce_min) || !isfinite(bounce_max)) {
        if (wb != nullptr) {
            wb->failed = true;
        }
        return false;
    }
    for (uint32_t k = 0u; k < 6u; ++k) {
        float u = (float)k / 5.0f;
        float theta = k_pi * (0.12f + 0.76f * u);
        Coord pos = {
            .x = center.x - cosf(theta) * radius,
            .y = center.y + sinf(theta) * (radius * 0.55f),
            .z = center.z + (float)k * dz,
        };
        float b = bounce_min + (bounce_max - bounce_min) * u;
        int32_t yaw_milli = (int32_t)(12000u + k * 15000u);
        bool inf = (k >= 3u);
        bool glow = (k == 2u || k == 5u);
        if (!world_builder_add_solid(wb, k, pos, yaw_milli, true, inf, glow, b)) {
            return false;
        }
    }
    return true;
}

int world_builder_commit(const WorldBuilder *wb) {
    if (wb == nullptr || wb->failed) {
        return -1;
    }
    table_publish();
    return 0;
}

int world_build_procedural_cathedral(void) {
    WorldBuilder wb;
    world_builder_begin(&wb, true);

    // 1. Outer Ring of 8 ground colonnade cubes with graded restitution [0.18 .. 0.90]
    (void)world_builder_add_cube_ring(&wb, (Coord){ 0.0f, 0.0f, 0.0f }, 560.0f, 8u, -1.0f, true);

    // 2. Twin elevated kinetic drop towers on the flanks
    (void)world_builder_add_cube_tower(&wb, (Coord){ -320.0f, -220.0f, 260.0f }, 220.0f, 2u, 0.78f);
    (void)world_builder_add_cube_tower(&wb, (Coord){  320.0f, -220.0f, 260.0f }, 220.0f, 2u, 0.86f);

    // 3. Ascending helical arc of all 6 analytical non-cube solids (Tetrahedron -> Torus)
    //    spanning restitution [0.18 .. 0.92] and heights z = [320 .. 720]
    (void)world_builder_add_solid_arc(&wb, (Coord){ 0.0f, 80.0f, 320.0f }, 440.0f, 80.0f, 0.18f, 0.92f);

    if (world_builder_commit(&wb) != 0) {
        return -1;
    }
    printf("[WorldBuilder] Procedurally constructed World 7: %u cubes, %u non-cube solids (smooth=%s)\n",
           wb.cubes_placed, wb.solids_placed, wb.smooth_solids ? "yes" : "no");
    fflush(stdout);
    return 0;
}

int world_build_besparmak_terrain(void) {
    WorldBuilder wb;
    world_builder_begin(&wb, true);
    ground_set_visible(false);
    if (!terrain_kind_spawn()) {
        return -1;
    }
    if (world_builder_commit(&wb) != 0) {
        return -1;
    }
    float focus_z = backend_sample_elevation_world_z(1595.0f, -650.0f);
    camera_set_turntable_mode(true);
    camera_set_home_pose(1595.0f, -650.0f, focus_z, -0.18f, 0.52f, 0.16f);
    printf("[WorldBuilder] Constructed World 8: Baklan & Beşparmak Dağı 24x24 km Metric DEM + Ultra-HD Satellite Map (256x256 Tiles, LOD 0..4)\n");
    fflush(stdout);
    return 0;
}
