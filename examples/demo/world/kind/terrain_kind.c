#include "world/world_int.h"
#include "backend/backend.h"
#include "frame/policy.h"
#include "frame/terrain.h"
#include <math.h>
#include <stdio.h>

typedef struct TerrainKindState {
    Spin              spin;
    TerrainSampleInfo last_pick;
    bool              up;
    bool              marker_live;
    bool              marker_shown;
    float             marker_x;
    float             marker_y;
    float             shown_x;
    float             shown_y;
} TerrainKindState;

static TerrainKindState s_terrain;
static const WorldOps   s_ops;

static bool terrain_hit(const WorldObj *o, const CameraRay *ray, float *t, Coord *at) {
    (void)o;
    if (ray == nullptr || policy_allows(PERM_DRAW)) {
        return false;
    }
    TerrainSampleInfo info = {0};
    if (!backend_ray_pick_terrain(ray->ox, ray->oy, ray->oz,
                                  ray->dx, ray->dy, ray->dz, &info)) {
        return false;
    }
    float dx = info.world_x - ray->ox;
    float dy = info.world_y - ray->oy;
    float dz = info.world_z - ray->oz;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (t != nullptr) {
        *t = dist;
    }
    if (at != nullptr) {
        *at = (Coord){ info.world_x, info.world_y, info.world_z };
    }
    s_terrain.last_pick = info;
    return true;
}

static bool terrain_on_click(WorldObj *o, Coord hit) {
    (void)o;
    TerrainSampleInfo info = s_terrain.last_pick;
    if (!backend_sample_world_xy(hit.x, hit.y, &info)) {
        return false;
    }
    s_terrain.marker_x    = info.world_x;
    s_terrain.marker_y    = info.world_y;
    s_terrain.marker_live = true;
    printf("[Terrain Survey] Baklan / Beşparmak Dağı | WGS84: %.5f°N, %.5f°E | Elevation: %.1f m | Slope: %.1f° | Quadtree Tile: LOD %u (grid %u,%u | node #%u)\n",
           info.latitude_deg,
           info.longitude_deg,
           (double)info.elevation_m,
           (double)info.slope_deg,
           (unsigned)info.lod,
           (unsigned)info.tile_gx,
           (unsigned)info.tile_gy,
           info.node_index);
    fflush(stdout);
    return true;
}

static void terrain_draw_obj(const WorldObj *o, VkCommandBuffer cmd,
                             uint32_t width, uint32_t height, bool live) {
    (void)o;
    float mx = live ? s_terrain.marker_x : s_terrain.shown_x;
    float my = live ? s_terrain.marker_y : s_terrain.shown_y;
    bool  ma = live ? s_terrain.marker_live : s_terrain.marker_shown;
    terrain_draw(cmd, width, height, mx, my, ma);
}

static bool terrain_advance(WorldObj *o, uint32_t time_ms) {
    (void)o;
    (void)time_ms;
    return s_terrain.marker_live != s_terrain.marker_shown ||
           s_terrain.marker_x != s_terrain.shown_x ||
           s_terrain.marker_y != s_terrain.shown_y;
}

static void terrain_commit_obj(WorldObj *o) {
    (void)o;
    s_terrain.marker_shown = s_terrain.marker_live;
    s_terrain.shown_x      = s_terrain.marker_x;
    s_terrain.shown_y      = s_terrain.marker_y;
}

static bool terrain_busy(const WorldObj *o) {
    (void)o;
    return s_terrain.marker_live != s_terrain.marker_shown ||
           s_terrain.marker_x != s_terrain.shown_x ||
           s_terrain.marker_y != s_terrain.shown_y;
}

static void terrain_record_comp(WorldObj *o, VkCommandBuffer cmd, uint32_t heat_dt, bool live) {
    (void)o;
    (void)heat_dt;
    (void)live;
    terrain_record_compute(cmd);
}

static uint32_t terrain_vertices(const WorldObj *o) {
    (void)o;
    return terrain_active_vertices();
}

static const char *terrain_name(const WorldObj *o) {
    (void)o;
    return "Terrain";
}

static const WorldOps s_ops = {
    .hit            = terrain_hit,
    .on_click       = terrain_on_click,
    .draw           = terrain_draw_obj,
    .advance        = terrain_advance,
    .commit         = terrain_commit_obj,
    .busy           = terrain_busy,
    .vertices       = terrain_vertices,
    .name           = terrain_name,
    .record_compute = terrain_record_comp,
};

void terrain_kind_reset(void) {
    s_terrain = (TerrainKindState){0};
}

bool terrain_kind_spawn(void) {
    if (s_terrain.up) {
        return true;
    }
    s_terrain = (TerrainKindState){0};
    s_terrain.up = true;
    s_terrain.spin = spin_pose(0, false, false, false);
    // Pre-place survey marker on the main 1,691 m summit ridge of Beşparmak Dağı (metric world coords)
    s_terrain.marker_x     = 1595.0f;
    s_terrain.marker_y     = -103.0f;
    s_terrain.shown_x      = 1595.0f;
    s_terrain.shown_y      = -103.0f;
    s_terrain.marker_live  = true;
    s_terrain.marker_shown = true;

    int idx = table_add(&s_ops, &s_terrain, (Coord){ 0.0f, 0.0f, 0.0f }, &s_terrain.spin);
    if (idx < 0) {
        s_terrain.up = false;
        return false;
    }
    return true;
}

const WorldOps *terrain_ops(void) {
    return &s_ops;
}
