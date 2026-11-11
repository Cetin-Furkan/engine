#include "world/world_int.h"
#include "backend/backend.h"
#include "frame/camera.h"
#include "frame/policy.h"
#include "frame/terrain.h"
#include "input/pointer.h"
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>

typedef struct TerrainKindState {
    Spin              spin;
    TerrainSampleInfo last_pick;
    bool              up;
    bool              marker_live;
    bool              marker_shown;
    bool              marker_b_live;
    bool              marker_b_shown;
    bool              next_is_b;
    bool              overlay_dirty;
    uint32_t          last_time_ms;
    uint32_t          wasd_hold_ms;
    float             marker_x;
    float             marker_y;
    float             shown_x;
    float             shown_y;
    float             marker_b_x;
    float             marker_b_y;
    float             shown_b_x;
    float             shown_b_y;
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

    TerrainSurveyStats s_stats = {0};
    (void)backend_survey_get_stats(&s_stats);
    printf("[Terrain Survey] Baklan & Beşparmak Dağı | WGS84: %.5f°N, %.5f°E | Elevation: %.1f m | Slope: %.1f° | Quadtree Tile: LOD %u (grid %u,%u | node #%u) | Survey Fusion: %u photo(s), %u GCP(s), %u tie-pt(s) (ZNCC %.3f), %u MVS posts\n",
           info.latitude_deg,
           info.longitude_deg,
           (double)info.elevation_m,
           (double)info.slope_deg,
           (unsigned)info.lod,
           (unsigned)info.tile_gx,
           (unsigned)info.tile_gy,
           info.node_index,
           s_stats.photos_loaded,
           s_stats.survey_points_loaded,
           s_stats.tiepoints_triangulated,
           (double)s_stats.mean_mvs_ncc,
           s_stats.mvs_elev_posts_refined);

    if (!s_terrain.marker_live || !s_terrain.next_is_b) {
        s_terrain.marker_x      = info.world_x;
        s_terrain.marker_y      = info.world_y;
        s_terrain.marker_live   = true;
        s_terrain.marker_b_live = false;
        s_terrain.next_is_b     = true;
    } else {
        s_terrain.marker_b_x    = info.world_x;
        s_terrain.marker_b_y    = info.world_y;
        s_terrain.marker_b_live = true;
        s_terrain.next_is_b     = false;

        TerrainProfileInfo prof = {0};
        if (backend_compute_terrain_profile(s_terrain.marker_x, s_terrain.marker_y,
                                            s_terrain.marker_b_x, s_terrain.marker_b_y,
                                            &prof)) {
            printf("[Terrain Profile] A(%.4f°N,%.4f°E @ %.1fm) -> B(%.4f°N,%.4f°E @ %.1fm) | Horiz: %.1f m (%.2f km) | 3D Path: %.1f m | Bearing: %.1f° | dElev: %+.1f m (+%.1f/-%.1f m, min %.1f..max %.1f m) | Grade: avg %+.1f%% (max %.1f%% / %.1f°)\n",
                   prof.point_a.latitude_deg,
                   prof.point_a.longitude_deg,
                   (double)prof.point_a.elevation_m,
                   prof.point_b.latitude_deg,
                   prof.point_b.longitude_deg,
                   (double)prof.point_b.elevation_m,
                   (double)prof.horiz_dist_m,
                   (double)(prof.horiz_dist_m * 0.001f),
                   (double)prof.dist_3d_m,
                   (double)prof.bearing_deg,
                   (double)prof.elev_delta_m,
                   (double)prof.elev_gain_m,
                   (double)prof.elev_loss_m,
                   (double)prof.min_elev_m,
                   (double)prof.max_elev_m,
                   (double)prof.avg_grade_pct,
                   (double)prof.max_grade_pct,
                   (double)prof.max_slope_deg);
        }
    }
    fflush(stdout);
    return true;
}

static void terrain_draw_obj(const WorldObj *o, VkCommandBuffer cmd,
                             uint32_t width, uint32_t height, bool live) {
    (void)o;
    float mx  = live ? s_terrain.marker_x : s_terrain.shown_x;
    float my  = live ? s_terrain.marker_y : s_terrain.shown_y;
    bool  ma  = live ? s_terrain.marker_live : s_terrain.marker_shown;
    float mbx = live ? s_terrain.marker_b_x : s_terrain.shown_b_x;
    float mby = live ? s_terrain.marker_b_y : s_terrain.shown_b_y;
    bool  mba = live ? s_terrain.marker_b_live : s_terrain.marker_b_shown;
    terrain_draw(cmd, width, height, mx, my, ma, mbx, mby, mba);
}

static bool terrain_advance(WorldObj *o, uint32_t time_ms) {
    (void)o;
    uint32_t dt_ms = s_terrain.last_time_ms != 0u ? (time_ms - s_terrain.last_time_ms) : 16u;
    if (dt_ms > 200u) {
        dt_ms = 16u;
    }
    s_terrain.last_time_ms = time_ms;

    bool shift = pointer_key_down(KEY_LEFTSHIFT) || pointer_key_down(KEY_RIGHTSHIFT);
    int move_x = (!shift && pointer_key_down(KEY_D) ? 1 : 0) - (!shift && pointer_key_down(KEY_A) ? 1 : 0);
    int move_y = (!shift && pointer_key_down(KEY_W) ? 1 : 0) - (!shift && pointer_key_down(KEY_S) ? 1 : 0);
    if (move_x != 0 || move_y != 0) {
        s_terrain.wasd_hold_ms += dt_ms;
        if (s_terrain.wasd_hold_ms >= 140u) {
            float rx = 1.0f;
            float ry = 0.0f;
            float fx = 0.0f;
            float fy = 1.0f;
            float zoom = 0.25f;
            camera_map_basis(&rx, &ry, &fx, &fy, &zoom);
            float speed = (420.0f / fmaxf(zoom, 0.02f)) * ((float)dt_ms * 0.001f);
            float dx = ((float)move_x * rx + (float)move_y * fx) * speed;
            float dy = ((float)move_x * ry + (float)move_y * fy) * speed;
            if (backend_overlay_move(dx, dy, false)) {
                s_terrain.overlay_dirty = true;
            }
        }
    } else {
        s_terrain.wasd_hold_ms = 0u;
    }

    return s_terrain.overlay_dirty ||
           s_terrain.marker_live != s_terrain.marker_shown ||
           s_terrain.marker_x != s_terrain.shown_x ||
           s_terrain.marker_y != s_terrain.shown_y ||
           s_terrain.marker_b_live != s_terrain.marker_b_shown ||
           s_terrain.marker_b_x != s_terrain.shown_b_x ||
           s_terrain.marker_b_y != s_terrain.shown_b_y;
}

static void terrain_commit_obj(WorldObj *o) {
    (void)o;
    s_terrain.overlay_dirty  = false;
    s_terrain.marker_shown   = s_terrain.marker_live;
    s_terrain.shown_x        = s_terrain.marker_x;
    s_terrain.shown_y        = s_terrain.marker_y;
    s_terrain.marker_b_shown = s_terrain.marker_b_live;
    s_terrain.shown_b_x      = s_terrain.marker_b_x;
    s_terrain.shown_b_y      = s_terrain.marker_b_y;
}

static bool terrain_busy(const WorldObj *o) {
    (void)o;
    return s_terrain.overlay_dirty ||
           s_terrain.marker_live != s_terrain.marker_shown ||
           s_terrain.marker_x != s_terrain.shown_x ||
           s_terrain.marker_y != s_terrain.shown_y ||
           s_terrain.marker_b_live != s_terrain.marker_b_shown ||
           s_terrain.marker_b_x != s_terrain.shown_b_x ||
           s_terrain.marker_b_y != s_terrain.shown_b_y;
}

static void terrain_record_comp(WorldObj *o, VkCommandBuffer cmd, uint32_t heat_dt, bool live) {
    (void)o;
    (void)heat_dt;
    terrain_record_compute(cmd, live);
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
    s_terrain.marker_live    = false;
    s_terrain.marker_shown   = false;
    s_terrain.marker_b_live  = false;
    s_terrain.marker_b_shown = false;
    s_terrain.next_is_b      = false;

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
