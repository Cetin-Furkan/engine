#include "world/world_int.h"
#include "frame/camera.h"
#include "frame/policy.h"
#include "frame/sheet.h"
#include "frame/terrain.h"
#include "input/pointer.h"
#include "ring/ring.h"
#include "engine/app.h"
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>

static constexpr float k_grab_lift_z = 24.0f;

typedef struct GrabState {
    bool     active;
    bool     empty_orbit;
    int      index;
    uint32_t generation;
    int32_t  origin_x;
    int32_t  origin_y;
    float    plane_z;
    float    off_x;
    float    off_y;
    float    last_x;
    float    last_y;
    float    vx;
    float    vy;
    uint64_t last_ns;
} GrabState;

static GrabState  s_grab        = { .index = -1 };
static GizmoState s_gizmo       = {0};
static bool       s_gizmo_ready = false;

static void ensure_gizmo(void) {
    if (!s_gizmo_ready) {
        gizmo_state_init(&s_gizmo);
        s_gizmo_ready = true;
    }
}

GizmoState *world_gizmo(void) {
    ensure_gizmo();
    return &s_gizmo;
}

static void build_gizmo_camera(GizmoCamera *out, bool live) {
    camera_bind(live);
    ViewPush vp = {0};
    view_push_fill(&vp, 0u);
    const float q[4]   = { vp.qx, vp.qy, vp.qz, vp.qw };
    const float cam[3] = { vp.cam_x, vp.cam_y, vp.cam_z };
    gizmo_camera_fill(out, q, cam, vp.zoom, vp.persp);
}

static void build_gizmo_ray(float sx, float sy, GizmoRay *out) {
    CameraRay ray = {0};
    camera_click_ray(sx, sy, &ray);
    out->origin[0] = ray.ox;
    out->origin[1] = ray.oy;
    out->origin[2] = ray.oz;
    out->dir[0]    = ray.dx;
    out->dir[1]    = ray.dy;
    out->dir[2]    = ray.dz;
}

static void grab_clear(void) {
    s_grab = (GrabState){ .index = -1 };
}

static bool grab_move_obj(WorldObj *o, Coord pos, float vx, float vy, bool release) {
    if (o == nullptr || !o->alive) {
        return false;
    }
    if (o->ops == cube_ops()) {
        return cube_kind_grab_move(o, pos, vx, vy, release);
    }
    if (o->ops == solid_ops()) {
        return solid_kind_grab_move(o, pos, vx, vy, release);
    }
    return false;
}

static void gizmo_freeze_selected_physics(void) {
    WorldObj *o = table_selected();
    if (o == nullptr || !o->alive) {
        return;
    }
    EcsTrs wt = {0};
    if (ecs_get_world_trs(world_ecs(), o->entity, &wt)) {
        Coord pos = { wt.pos[0], wt.pos[1], wt.pos[2] };
        (void)grab_move_obj(o, pos, 0.0f, 0.0f, false);
    }
}

static bool grab_screen_from_client(int32_t px, int32_t py, float *sx, float *sy) {
    const WaylandContext *w = wayland_get_context();
    if (w == nullptr || sx == nullptr || sy == nullptr) {
        return false;
    }
    uint32_t sw = w->scene_phys_w != 0u ? w->scene_phys_w : w->scene_opaque_w;
    uint32_t sh = w->scene_phys_h != 0u ? w->scene_phys_h : w->scene_opaque_h;
    if (sw == 0u || sh == 0u) {
        return false;
    }
    *sx = (float)px - (float)sw * 0.5f;
    *sy = (float)sh * 0.5f - (float)py;
    return true;
}

static bool grab_ray_plane_z(const CameraRay *ray, float z, float *wx, float *wy) {
    if (ray == nullptr || wx == nullptr || wy == nullptr || fabsf(ray->dz) < 0.04f) {
        return false;
    }
    float t = (z - ray->oz) / ray->dz;
    if (!isfinite(t) || t <= 0.0f || t > 24000.0f) {
        return false;
    }
    *wx = ray->ox + t * ray->dx;
    *wy = ray->oy + t * ray->dy;
    return isfinite(*wx) && isfinite(*wy);
}

void world_interact_reset(void) {
    ensure_gizmo();
    gizmo_end_drag(&s_gizmo);
    s_gizmo.hovered = GIZMO_HANDLE_NONE;
    grab_clear();
}

bool world_interact_release(void) {
    if (!s_grab.active) {
        s_grab.empty_orbit = false;
        return false;
    }
    int      idx = s_grab.index;
    uint32_t gen = s_grab.generation;
    float    vx  = s_grab.vx;
    float    vy  = s_grab.vy;
    grab_clear();
    WorldObj *o = table_at(idx);
    if (o == nullptr || !o->alive || o->generation != gen) {
        return false;
    }
    return grab_move_obj(o, o->at, vx, vy, true);
}

bool world_interact_busy(void) {
    ensure_gizmo();
    return s_grab.active || s_grab.empty_orbit || s_gizmo.dragging;
}

bool world_hover(float screen_x, float screen_y) {
    ensure_gizmo();
    if (world_selected_id() == 0u || policy_mode() == MODE_2D || terrain_is_active()) {
        if (s_gizmo.hovered != GIZMO_HANDLE_NONE) {
            s_gizmo.hovered = GIZMO_HANDLE_NONE;
            return true;
        }
        return false;
    }
    EcsEntity sel = world_selected_entity();
    if (sel == ECS_NULL_ENTITY) {
        if (s_gizmo.hovered != GIZMO_HANDLE_NONE) {
            s_gizmo.hovered = GIZMO_HANDLE_NONE;
            return true;
        }
        return false;
    }
    table_sync_ecs(true);
    GizmoCamera gcam = {0};
    GizmoRay    gray = {0};
    build_gizmo_camera(&gcam, true);
    build_gizmo_ray(screen_x, screen_y, &gray);
    return gizmo_hover(&s_gizmo, world_ecs(), sel, &gcam, &gray);
}

bool world_click(float screen_x, float screen_y) {
    ensure_gizmo();
    table_sync_ecs(true);
    WorkspaceMode mode = policy_mode();
    if (mode != MODE_2D && !terrain_is_active()) {
        EcsEntity sel = world_selected_entity();
        if (sel != ECS_NULL_ENTITY && s_gizmo.mode != GIZMO_MODE_NONE) {
            GizmoCamera gcam = {0};
            GizmoRay    gray = {0};
            build_gizmo_camera(&gcam, true);
            build_gizmo_ray(screen_x, screen_y, &gray);
            if (gizmo_pick_handle(&s_gizmo, world_ecs(), sel, &gcam, &gray) != GIZMO_HANDLE_NONE) {
                return false;
            }
        }
    }
    CameraRay ray;
    camera_click_ray(screen_x, screen_y, &ray);
    if (mode != MODE_3D && policy_allows(PERM_OBJ_SELECT)) {
        Coord ground = {0};
        if (coord_ground_hit(&ray, &ground, nullptr) && sheet_click(ground.x, ground.y)) {
            table_clear_selection();
            return true;
        }
    }
    if (mode != MODE_2D) {
        Coord hit = {0};
        int index = table_pick(&ray, &hit, nullptr);
        if (index >= 0) {
            if (!policy_allows(PERM_OBJ_SELECT)) {
                return false;
            }
            sheet_clear();
            bool changed = table_select(index);
            WorldObj *o = table_at(index);
            if (o != nullptr && o->ops != nullptr && o->ops->on_click != nullptr &&
                o->ops->on_click(o, hit)) {
                changed = true;
            }
            return changed;
        }
    } else if (face_chain_count() == 0u) {
        Coord hit = {0};
        int index = table_pick(&ray, &hit, nullptr);
        if (index >= 0) {
            WorldObj *o = table_at(index);
            if (o != nullptr && o->ops == face_ops()) {
                if (!policy_allows(PERM_OBJ_SELECT)) {
                    return false;
                }
                sheet_clear();
                bool changed = table_select(index);
                if (o->ops->on_click != nullptr && o->ops->on_click(o, hit)) {
                    changed = true;
                }
                return changed;
            }
        }
    }
    Coord ground = {0};
    if (!coord_ground_hit(&ray, &ground, nullptr)) {
        return false;
    }
    if (!policy_allows(PERM_DRAW)) {
        return false;
    }
    return face_spawn(ground);
}

bool world_pointer_drag_poll(void) {
    ensure_gizmo();
    if ((pointer_buttons() & ENG_BUTTON_LEFT) != 0u) {
        return false;
    }
    if (s_gizmo.dragging) {
        gizmo_end_drag(&s_gizmo);
        table_sync_ecs(true);
        (void)world_save();
        return true;
    }
    if (s_grab.active) {
        return world_interact_release();
    }
    s_grab.empty_orbit = false;
    return false;
}

bool world_pointer_drag(float dx, float dy, uint32_t buttons) {
    ensure_gizmo();
    if ((buttons & ENG_BUTTON_LEFT) == 0u || policy_mode() == MODE_2D ||
        terrain_is_active() || pointer_key_down(KEY_T)) {
        bool giz_end = false;
        if (s_gizmo.dragging) {
            gizmo_end_drag(&s_gizmo);
            giz_end = true;
        }
        bool released = world_interact_release();
        return camera_pointer_drag(dx, dy, buttons) || released || giz_end;
    }
    int32_t ox = 0;
    int32_t oy = 0;
    bool have_origin = pointer_left_drag_origin(&ox, &oy);

    if (!s_grab.active && !s_grab.empty_orbit) {
        EcsEntity sel = world_selected_entity();
        if (!s_gizmo.dragging && sel != ECS_NULL_ENTITY && s_gizmo.mode != GIZMO_MODE_NONE &&
            have_origin) {
            float osx = 0.0f, osy = 0.0f;
            if (grab_screen_from_client(ox, oy, &osx, &osy)) {
                table_sync_ecs(true);
                GizmoCamera gcam = {0};
                GizmoRay    oray = {0};
                build_gizmo_camera(&gcam, true);
                build_gizmo_ray(osx, osy, &oray);
                if (gizmo_begin_drag(&s_gizmo, world_ecs(), sel, &gcam, &oray)) {
                    gizmo_freeze_selected_physics();
                }
            }
        }
        if (s_gizmo.dragging) {
            int32_t cx = 0, cy = 0;
            float   csx = 0.0f, csy = 0.0f;
            if (!pointer_client_xy(&cx, &cy) || !grab_screen_from_client(cx, cy, &csx, &csy)) {
                return false;
            }
            GizmoCamera gcam = {0};
            GizmoRay    cray = {0};
            build_gizmo_camera(&gcam, true);
            build_gizmo_ray(csx, csy, &cray);
            bool snap    = pointer_key_down(KEY_LEFTCTRL) || pointer_key_down(KEY_RIGHTCTRL);
            bool changed = gizmo_update_drag(&s_gizmo, world_ecs(), &gcam, &cray, snap);
            table_sync_ecs(true);
            gizmo_freeze_selected_physics();
            return changed;
        }
    }

    if (s_grab.empty_orbit) {
        if (!have_origin || (ox == s_grab.origin_x && oy == s_grab.origin_y)) {
            return camera_pointer_drag(dx, dy, ENG_BUTTON_RIGHT);
        }
        s_grab.empty_orbit = false;
    }
    if (!s_grab.active) {
        float sx = 0.0f;
        float sy = 0.0f;
        if (policy_allows(PERM_OBJ_SELECT) && have_origin &&
            grab_screen_from_client(ox, oy, &sx, &sy)) {
            CameraRay ray = {0};
            camera_click_ray(sx, sy, &ray);
            int idx = table_pick(&ray, nullptr, nullptr);
            if (idx >= 0) {
                WorldObj *o = table_at(idx);
                if (o != nullptr && o->alive &&
                    (o->ops == cube_ops() || o->ops == solid_ops())) {
                    float px0 = o->at.x;
                    float py0 = o->at.y;
                    (void)grab_ray_plane_z(&ray, o->at.z, &px0, &py0);
                    sheet_clear();
                    (void)table_select(idx);
                    s_grab = (GrabState){
                        .active      = true,
                        .empty_orbit = false,
                        .index       = idx,
                        .generation  = o->generation,
                        .origin_x    = ox,
                        .origin_y    = oy,
                        .plane_z     = o->at.z,
                        .off_x       = o->at.x - px0,
                        .off_y       = o->at.y - py0,
                        .last_x      = o->at.x,
                        .last_y      = o->at.y,
                        .vx          = 0.0f,
                        .vy          = 0.0f,
                        .last_ns     = ring_mono_ns(),
                    };
                }
            }
        }
        if (!s_grab.active) {
            s_grab.empty_orbit = true;
            s_grab.origin_x    = ox;
            s_grab.origin_y    = oy;
            return camera_pointer_drag(dx, dy, ENG_BUTTON_RIGHT);
        }
    }

    WorldObj *o = table_at(s_grab.index);
    if (o == nullptr || !o->alive || o->generation != s_grab.generation) {
        grab_clear();
        return false;
    }
    int32_t cx = 0, cy = 0;
    float   sx = 0.0f, sy = 0.0f;
    if (!pointer_client_xy(&cx, &cy) || !grab_screen_from_client(cx, cy, &sx, &sy)) {
        return false;
    }
    CameraRay ray = {0};
    camera_click_ray(sx, sy, &ray);
    float wx = 0.0f, wy = 0.0f;
    if (!grab_ray_plane_z(&ray, s_grab.plane_z, &wx, &wy)) {
        return false;
    }
    float nx = wx + s_grab.off_x;
    float ny = wy + s_grab.off_y;
    uint64_t now_ns = ring_mono_ns();
    float dt = (now_ns > s_grab.last_ns) ? ((float)(now_ns - s_grab.last_ns) * 1.0e-9f) : 0.016f;
    if (dt < 0.004f) {
        dt = 0.004f;
    } else if (dt > 0.080f) {
        dt = 0.080f;
    }
    float ivx = (nx - s_grab.last_x) / dt;
    float ivy = (ny - s_grab.last_y) / dt;
    s_grab.vx = s_grab.vx * 0.55f + ivx * 0.45f;
    s_grab.vy = s_grab.vy * 0.55f + ivy * 0.45f;
    s_grab.last_x  = nx;
    s_grab.last_y  = ny;
    s_grab.last_ns = now_ns;
    Coord target = { nx, ny, s_grab.plane_z + k_grab_lift_z };
    return grab_move_obj(o, target, 0.0f, 0.0f, false);
}

bool world_gizmo_cycle_mode(void) {
    ensure_gizmo();
    gizmo_cycle_mode(&s_gizmo);
    printf("[Gizmo] Mode: %s [%s]\n",
           gizmo_mode_name(s_gizmo.mode), gizmo_space_name(s_gizmo.space));
    fflush(stdout);
    return true;
}

bool world_gizmo_set_mode(GizmoMode mode) {
    ensure_gizmo();
    if (s_gizmo.mode == mode) {
        return false;
    }
    gizmo_set_mode(&s_gizmo, mode);
    printf("[Gizmo] Mode: %s [%s]\n",
           gizmo_mode_name(s_gizmo.mode), gizmo_space_name(s_gizmo.space));
    fflush(stdout);
    return true;
}

bool world_gizmo_toggle_space(void) {
    ensure_gizmo();
    gizmo_toggle_space(&s_gizmo);
    printf("[Gizmo] Space: %s\n", gizmo_space_name(s_gizmo.space));
    fflush(stdout);
    return true;
}

bool world_gizmo_toggle_debug_draw(void) {
    ensure_gizmo();
    bool on = gizmo_toggle_debug_draw(&s_gizmo);
    printf("[Debug3D] World AABBs + Hierarchy Arrows + Local Triads: %s\n", on ? "ON" : "OFF");
    fflush(stdout);
    return true;
}

bool world_parent_selected(void) {
    EcsWorld *w = world_ecs();
    EcsEntity sel = world_selected_entity();
    if (sel == ECS_NULL_ENTITY) {
        return false;
    }
    table_sync_ecs(true);
    EcsEntity ents[ECS_MAX_ENTITIES];
    uint32_t  n = ecs_query_mask(w, ECS_COMP_TRANSFORM | ECS_COMP_HIERARCHY, ents, ECS_MAX_ENTITIES);
    if (n < 2u) {
        return false;
    }
    EcsEntity cur_parent = ecs_get_parent(w, sel);
    EcsTrs    sel_trs    = {0};
    (void)ecs_get_world_trs(w, sel, &sel_trs);

    EcsEntity best      = ECS_NULL_ENTITY;
    float     best_dist = 1.0e30f;
    for (uint32_t i = 0u; i < n; ++i) {
        EcsEntity cand = ents[i];
        if (cand == sel || cand == cur_parent || ecs_is_descendant(w, cand, sel)) {
            continue;
        }
        EcsTrs ctrs = {0};
        if (!ecs_get_world_trs(w, cand, &ctrs)) {
            continue;
        }
        float dx = ctrs.pos[0] - sel_trs.pos[0];
        float dy = ctrs.pos[1] - sel_trs.pos[1];
        float dz = ctrs.pos[2] - sel_trs.pos[2];
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best_dist) {
            best_dist = d2;
            best      = cand;
        }
    }
    if (best == ECS_NULL_ENTITY || !ecs_set_parent(w, sel, best, true)) {
        return false;
    }
    ecs_propagate_transforms(w);
    printf("[ECS] Parented '%s' (#%u) -> '%s' (#%u) [depth=%u, world pose preserved]\n",
           ecs_get_name(w, sel), (unsigned)(sel & ECS_INDEX_MASK),
           ecs_get_name(w, best), (unsigned)(best & ECS_INDEX_MASK),
           ecs_get_depth(w, sel));
    fflush(stdout);
    return true;
}

bool world_unparent_selected(void) {
    EcsWorld *w = world_ecs();
    EcsEntity sel = world_selected_entity();
    if (sel == ECS_NULL_ENTITY || ecs_get_parent(w, sel) == ECS_NULL_ENTITY) {
        return false;
    }
    table_sync_ecs(true);
    if (!ecs_set_parent(w, sel, ECS_NULL_ENTITY, true)) {
        return false;
    }
    ecs_propagate_transforms(w);
    table_sync_ecs(true);
    printf("[ECS] Detached '%s' (#%u) -> root [world pose preserved]\n",
           ecs_get_name(w, sel), (unsigned)(sel & ECS_INDEX_MASK));
    fflush(stdout);
    return true;
}

bool world_select_cycle(int dir) {
    EcsWorld *w = world_ecs();
    EcsEntity ents[ECS_MAX_ENTITIES];
    uint32_t  n = ecs_query_mask(w, ECS_COMP_TRANSFORM | ECS_COMP_RENDER, ents, ECS_MAX_ENTITIES);
    if (n == 0u) {
        return false;
    }
    EcsEntity sel = world_selected_entity();
    int cur = -1;
    for (uint32_t i = 0u; i < n; ++i) {
        if (ents[i] == sel) {
            cur = (int)i;
            break;
        }
    }
    int next = (cur < 0) ? 0 : (cur + (dir >= 0 ? 1 : -1) + (int)n) % (int)n;
    int32_t slot = ecs_get_user_slot(w, ents[next]);
    if (slot < 0) {
        return false;
    }
    return table_select((int)slot);
}

void world_ecs_stats(uint32_t *out_entities, uint32_t *out_hier_links) {
    EcsWorld *w = world_ecs();
    uint32_t total = ecs_entity_count(w);
    uint32_t links = 0u;
    for (uint32_t i = 0u; i < total; ++i) {
        EcsEntity e = ecs_entity_at(w, i);
        if (ecs_get_parent(w, e) != ECS_NULL_ENTITY) {
            links++;
        }
    }
    if (out_entities != nullptr) {
        *out_entities = total;
    }
    if (out_hier_links != nullptr) {
        *out_hier_links = links;
    }
}
