#include "world/world_int.h"
#include "frame/camera.h"
#include "frame/policy.h"
#include "frame/sheet.h"
#include "frame/terrain.h"
#include "lighting/lighting.h"
#include <stdio.h>

static uint32_t s_world_id = 0u;
static char     s_custom_file[1024];
static char     s_cli_model[1024];
static float    s_clear_rgba[4] = { 0.015f, 0.018f, 0.024f, 1.0f };

void world_set_clear_color(float r, float g, float b, float a) {
    s_clear_rgba[0] = r;
    s_clear_rgba[1] = g;
    s_clear_rgba[2] = b;
    s_clear_rgba[3] = a;
}

void world_clear_color(float out_rgba[4]) {
    if (out_rgba == nullptr) {
        return;
    }
    out_rgba[0] = s_clear_rgba[0];
    out_rgba[1] = s_clear_rgba[1];
    out_rgba[2] = s_clear_rgba[2];
    out_rgba[3] = s_clear_rgba[3];
}

void world_select(uint32_t world_id) {
    if (world_id > k_world_preset_max) {
        world_id = 0u;
    }
    s_world_id = world_id;
    s_custom_file[0] = '\0';
    sheet_select_world(s_world_id);
}

bool world_select_file(const char *path) {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    int wrote = snprintf(s_custom_file, sizeof s_custom_file, "%s", path);
    if (wrote <= 0 || (size_t)wrote >= sizeof s_custom_file) {
        s_custom_file[0] = '\0';
        return false;
    }
    s_world_id = k_world_custom_file;
    sheet_select_world(9u);
    return true;
}

bool world_select_model(const char *path, bool standalone_studio) {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    int wrote = snprintf(s_cli_model, sizeof s_cli_model, "%s", path);
    if (wrote <= 0 || (size_t)wrote >= sizeof s_cli_model) {
        s_cli_model[0] = '\0';
        return false;
    }
    if (standalone_studio) {
        s_world_id = k_world_model_studio;
        s_custom_file[0] = '\0';
    }
    return true;
}

const char *world_cli_model_path(void) {
    return s_cli_model[0] != '\0' ? s_cli_model : nullptr;
}

const char *world_custom_path(void) {
    return s_custom_file[0] != '\0' ? s_custom_file : nullptr;
}

uint32_t world_selected_id(void) {
    return s_world_id;
}

const char *world_selected_title(void) {
    switch (s_world_id) {
    case 0u:
        return "Pure Empty Engine (Zero Objects, Zero Overlays)";
    case 1u:
        return "3D-Only Click-to-Draw Studio (Points -> Lines -> Faces)";
    case 2u:
        return "3D-Only Gravity & Elastic Non-Cube Solids (Squash-and-Stretch)";
    case 3u:
        return "2D-Only Gravity, Deformable Soft Bodies & Elastic Polygons";
    case 4u:
        return "2D-Only Click-to-Draw Polygon Workshop & Elastic Sheet";
    case 5u:
        return "Hybrid 3D+2D Click-to-Draw, Gravity & Multi-Elasticity";
    case 6u:
        return "3D+2D Switchable Restitution Staircase & Bumper Lab";
    case 7u:
        return "Procedural C23 WorldBuilder: Kinetic Cathedral & Cascade";
    case 8u:
        return "Baklan & Beşparmak Dağı SSD-Streamed DEM + Satellite Map (Denizli, TR)";
    case k_world_custom_file:
        return s_custom_file[0] != '\0' ? s_custom_file : "Custom .world Document";
    case k_world_model_studio:
        return s_cli_model[0] != '\0' ? s_cli_model : "glTF 2.0 / Blender PBR Studio";
    case 9u:
    default:
        return "Interactive Full Sandbox (Switchable 3D / 2D / Hybrid + Plots)";
    }
}

void world_apply_policy(void) {
    switch (s_world_id) {
    case 0u:
        policy_apply_world(MODE_3D, PERM_NONE, 0xFFFFu, true);
        camera_set_scene_3d(false, false, false);
        return;
    case 1u:
        policy_apply_world(MODE_3D, PERM_DRAW, PERM_MODE_SWITCH | PERM_PLOT_CYCLE, false);
        camera_set_scene_3d(true, true, false);
        camera_set_home_pose(0.0f, 0.0f, 60.0f, -0.46f, 0.96f, 0.76f);
        break;
    case 2u:
        policy_apply_world(MODE_3D, PERM_NONE, PERM_MODE_SWITCH | PERM_PLOT_CYCLE, false);
        camera_set_scene_3d(true, true, false);
        camera_set_home_pose(0.0f, 0.0f, 70.0f, -0.46f, 0.96f, 0.74f);
        break;
    case 3u:
        policy_apply_world(MODE_2D, PERM_NONE, PERM_MODE_SWITCH | PERM_PLOT_CYCLE, false);
        camera_set_scene_3d(false, false, false);
        (void)camera_snap_front();
        camera_commit();
        break;
    case 4u:
        policy_apply_world(MODE_2D, PERM_DRAW, PERM_MODE_SWITCH | PERM_PLOT_CYCLE, false);
        camera_set_scene_3d(false, false, false);
        (void)camera_snap_front();
        camera_commit();
        break;
    case 5u:
        policy_apply_world(MODE_HYBRID, PERM_DRAW, PERM_PLOT_CYCLE, false);
        camera_set_scene_3d(true, true, false);
        camera_set_home_pose(0.0f, 0.0f, 60.0f, -0.46f, 0.96f, 0.72f);
        break;
    case 6u:
        policy_apply_world(MODE_3D, PERM_NONE, PERM_PLOT_CYCLE, false);
        camera_set_scene_3d(true, true, false);
        camera_set_home_pose(0.0f, 0.0f, 70.0f, -0.42f, 0.94f, 0.70f);
        break;
    case 7u:
        policy_apply_world(MODE_HYBRID, PERM_DRAW, PERM_PLOT_CYCLE, false);
        camera_set_scene_3d(true, true, false);
        camera_set_home_pose(0.0f, 0.0f, 90.0f, -0.46f, 0.94f, 0.62f);
        break;
    case 8u:
        policy_apply_world(MODE_3D, PERM_NONE, PERM_PLOT_CYCLE | PERM_OBJ_SPAWN | PERM_MODE_SWITCH, false);
        camera_set_scene_3d(true, false, true);
        camera_set_home_pose(1595.0f, -650.0f, 520.0f, -0.18f, 0.52f, 0.16f);
        printf("[Bird's-Eye Overlay] Active at 50%% transparency | Keys: 'O'=+Transparency, 'P'=-Transparency, 'W/A/S/D'=Move parallel to map, 'Shift+WASD' (or I/U, J/N, ]/[)=0.5%% 2D Stretch, 'B'=Top-Down Bird's-Eye View, 'Enter'=Upload & Bake onto Map\n");
        break;
    case k_world_custom_file:
        policy_boot();
        camera_set_scene_3d(true, true, false);
        camera_set_home_pose(0.0f, 0.0f, 60.0f, -0.46f, 0.96f, 0.74f);
        return;
    case k_world_model_studio:
        policy_apply_world(MODE_3D, PERM_NONE, PERM_MODE_SWITCH | PERM_PLOT_CYCLE, false);
        camera_set_scene_3d(true, true, false);
        camera_set_home_pose(0.0f, 0.0f, 125.0f, -0.52f, 1.04f, 1.05f);
        break;
    case 9u:
    default:
        policy_boot();
        camera_set_scene_3d(true, true, false);
        camera_set_home_pose(0.0f, 0.0f, 60.0f, -0.46f, 0.96f, 0.74f);
        break;
    }
    const WorkspacePolicy *pol = policy_active();
    printf("[World] Active #%u: %s | Mode=%s (Tab switch=%s, click-draw=%s)\n",
           s_world_id, world_selected_title(), pol->name,
           policy_allows(PERM_MODE_SWITCH) ? "on" : "locked",
           policy_allows(PERM_DRAW) ? "on" : "off");
    fflush(stdout);
}

bool world_switch_to(uint32_t world_id) {
    if (world_id > k_world_preset_max) {
        return false;
    }
    (void)world_save();
    (void)sheet_save();
    world_select(world_id);
    world_reset();
    world_apply_policy();
    if (world_load() != 0) {
        return false;
    }
    printf("[World] Runtime hot-switched to World %u: %s\n",
           s_world_id, world_selected_title());
    fflush(stdout);
    return true;
}

bool world_reload(void) {
    if (s_world_id == 0u) {
        return false;
    }
    if (s_world_id == 8u) {
        bool ok = terrain_reload();
        lighting_invalidate_history();
        printf("[World] Hot-reloaded World 8 (%s) terrain & survey dataset\n",
               world_selected_title());
        fflush(stdout);
        return ok;
    }
    world_reset();
    world_apply_policy();
    if (world_load() != 0) {
        return false;
    }
    const char *src = world_source_path();
    printf("[World] Hot-reloaded World %u (%s) from %s\n",
           s_world_id, world_selected_title(),
           src != nullptr ? src : "builtin");
    fflush(stdout);
    return true;
}
