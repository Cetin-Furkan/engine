#include "frame/terrain.h"
#include "backend/backend.h"
#include "frame/camera.h"
#include "frame/grid_math.h"
#include "frame/material.h"
#include "frame/pass.h"
#include "vulkan/pipe.h"
#include "wayland/wayland.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

alignas(uint32_t) static const unsigned char k_vert_spv[] = {
#embed "terrain.vert.spv"
};
alignas(uint32_t) static const unsigned char k_frag_spv[] = {
#embed "terrain.frag.spv"
};

static_assert(sizeof k_vert_spv % 4u == 0u, "terrain vert spirv");
static_assert(sizeof k_frag_spv % 4u == 0u, "terrain frag spirv");

enum : uint32_t {
    k_terrain_quads_side  = 64u,
    k_terrain_skirt_quads = 64u,
    k_terrain_tile_verts  = (k_terrain_quads_side * k_terrain_quads_side + k_terrain_skirt_quads) * 6u,
};

typedef struct TerrainPush {
    uint64_t dir_bda;
    uint64_t indirect_bda;
    uint32_t width;
    uint32_t height;
    float    elev_base_m;
    float    vertical_scale;
    float    marker_x;
    float    marker_y;
    float    marker_active;
    float    marker_b_x;
    float    marker_b_y;
    float    marker_b_active;
    float    cell_med;
    float    coarse;
    uint32_t flags;
    uint32_t pad0;
    ViewPush view;
} TerrainPush;

static_assert(offsetof(TerrainPush, indirect_bda) == 8u, "terrain indirect_bda");
static_assert(offsetof(TerrainPush, elev_base_m) == 24u, "terrain elev_base_m");
static_assert(offsetof(TerrainPush, marker_x) == 32u, "terrain marker_x");
static_assert(offsetof(TerrainPush, marker_b_x) == 44u, "terrain marker_b_x");
static_assert(offsetof(TerrainPush, view) == 72u, "terrain view");
static_assert(sizeof(TerrainPush) == 120u, "terrain push");

static VulkanContext   *s_vk                   = nullptr;
static VkPipelineLayout s_layout               = VK_NULL_HANDLE;
static VulkanGfxLive    s_gfx;
static bool             s_backend_ready        = false;
static bool             s_backend_failed       = false;
static bool             s_world_terrain_active = false;
static uint32_t         s_contour_mode         = 0u;
static uint32_t         s_last_width           = 1920u;
static uint32_t         s_last_height          = 1080u;
static char             s_pack_path[1024];

static void terrain_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Terrain Error] %s: %s (%d)\n", what, vulkan_vkresult_to_str(res), (int)res);
}

static void terrain_release_backend(VulkanContext *vk) {
    vulkan_gfx_disarm(vk, &s_gfx);
    if (s_backend_ready) {
        backend_shutdown(vk);
    }
    s_backend_ready  = false;
    s_backend_failed = false;
    if (vk != nullptr && vk->device != VK_NULL_HANDLE && s_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, s_layout, nullptr);
    }
    s_layout = VK_NULL_HANDLE;
}

void terrain_shutdown(VulkanContext *vk) {
    terrain_release_backend(vk);
    s_vk = nullptr;
}

static int terrain_ensure_ready(void) {
    if (s_backend_ready) {
        return 0;
    }
    if (s_backend_failed || s_vk == nullptr || s_vk->device == VK_NULL_HANDLE ||
        s_vk->physical_device == VK_NULL_HANDLE) {
        return -1;
    }
    VkFormat depth_format = pass_depth_format();
    VkSampleCountFlagBits samples = pass_raster_samples();
    if (depth_format == VK_FORMAT_UNDEFINED || samples == 0) {
        s_backend_failed = true;
        return -1;
    }
    if (backend_init(s_vk) != 0) {
        s_backend_failed = true;
        return -1;
    }
    printf("[Terrain] Active map archive: %s\n", backend_active_pack_path());
    if (backend_is_v2_archive()) {
        if (!camera_refresh_turntable_home_z()) {
            float cz = backend_sample_elevation_world_z(0.0f, 0.0f);
            camera_set_turntable_mode(true);
            camera_set_home_pose(0.0f, 0.0f, cz, -0.22f, 0.48f, 0.062f);
        }
    } else {
        float ov_wx = 0.0f;
        float ov_wy = 0.0f;
        if (backend_overlay_get_center(&ov_wx, &ov_wy) &&
            (ov_wx != 1595.0f || ov_wy != -650.0f)) {
            float ov_wz = backend_sample_elevation_world_z(ov_wx, ov_wy);
            camera_set_home_pose(ov_wx, ov_wy, ov_wz, 0.0f, 0.42f, 0.48f);
        } else {
            float focus_z = backend_sample_elevation_world_z(1595.0f, -650.0f);
            camera_set_home_pose(1595.0f, -650.0f, focus_z, -0.18f, 0.52f, 0.16f);
        }
    }
    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0u,
        .size       = sizeof(TerrainPush),
    };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1u,
        .pPushConstantRanges    = &push,
    };
    VkResult res = vkCreatePipelineLayout(s_vk->device, &layout_info, nullptr, &s_layout);
    if (res != VK_SUCCESS) {
        terrain_fail("vkCreatePipelineLayout", res);
        backend_shutdown(s_vk);
        s_backend_failed = true;
        return -1;
    }

    VulkanGfxPipe gfx = {
        .vert          = k_vert_spv,
        .vert_bytes    = sizeof k_vert_spv,
        .frag          = k_frag_spv,
        .frag_bytes    = sizeof k_frag_spv,
        .layout        = s_layout,
        .depth_format  = depth_format,
        .samples       = samples,
        .cull          = VK_CULL_MODE_NONE,
        .depth_test    = VK_TRUE,
        .depth_write   = VK_TRUE,
        .depth_compare = VK_COMPARE_OP_GREATER_OR_EQUAL,
        .blend         = VK_FALSE,
    };
    vulkan_gfx_arm(&s_gfx, &gfx, pass_hdr_format(), nullptr);
    s_backend_ready = true;
    const char *active_pack = backend_active_pack_path();
    const char *pack_base   = strrchr(active_pack, '/');
    pack_base               = (pack_base != nullptr && pack_base[1] != '\0') ? (pack_base + 1) : active_pack;
    printf("[Terrain] Adaptive CDLOD Quadtree renderer armed (%s, 64/32/16 adaptive mesh, indirect=%s).\n",
           pack_base,
           (s_vk->device_info.supports_draw_indirect_count &&
            s_vk->device_info.supports_multi_draw_indirect &&
            s_vk->device_info.supports_draw_indirect_first_instance)
               ? "vkCmdDrawIndirectCount"
               : "vkCmdDrawIndirect");
    fflush(stdout);
    return 0;
}

int terrain_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->physical_device == VK_NULL_HANDLE) {
        return -1;
    }
    s_vk             = vk;
    s_backend_failed = false;
    return 0;
}

void terrain_record_compute(VkCommandBuffer cmd, bool live) {
    if (cmd == VK_NULL_HANDLE || terrain_ensure_ready() != 0) {
        return;
    }
    camera_bind(live);
    ViewPush view = {0};
    view_push_fill(&view, k_mat_solid0);

    // Stream needed SSD quadtree tiles before compute filter + GPU frustum culling run
    (void)backend_update_camera_zoom(view.cam_x, view.cam_y, view.zoom);

    uint32_t vp_w = s_last_width;
    uint32_t vp_h = s_last_height;
    const WaylandContext *wl = wayland_get_context();
    if (wl != nullptr) {
        if (wl->scene_opaque_w > 0u && wl->scene_opaque_h > 0u) {
            vp_w = wl->scene_opaque_w;
            vp_h = wl->scene_opaque_h;
        } else if (wl->configure_width > 0u && wl->configure_height > 0u) {
            vp_w = wl->configure_width;
            vp_h = wl->configure_height;
        }
    }

    TerrainCullParams cull = {
        .width          = vp_w,
        .height         = vp_h,
        .elev_base_m    = backend_elev_base_m(),
        .vertical_scale = backend_vertical_scale(),
        .verts_per_tile = k_terrain_tile_verts,
        .qx             = view.qx,
        .qy             = view.qy,
        .qz             = view.qz,
        .qw             = view.qw,
        .cam_x          = view.cam_x,
        .cam_y          = view.cam_y,
        .cam_z          = view.cam_z,
        .zoom           = view.zoom,
    };
    backend_record_compute(cmd, &cull);
}

void terrain_draw(VkCommandBuffer cmd,
                  uint32_t        width,
                  uint32_t        height,
                  float           marker_x,
                  float           marker_y,
                  bool            marker_active,
                  float           marker_b_x,
                  float           marker_b_y,
                  bool            marker_b_active) {
    if (cmd == VK_NULL_HANDLE || width == 0u || height == 0u || terrain_ensure_ready() != 0) {
        return;
    }
    s_last_width  = width;
    s_last_height = height;

    uint64_t dir_bda      = backend_directory_bda();
    uint64_t indirect_bda = backend_indirect_bda();
    if (dir_bda == 0ull) {
        return;
    }

    float elev_base = backend_elev_base_m();
    float v_scale   = backend_vertical_scale();
    uint32_t push_flags = (s_contour_mode & 3u) |
                          (backend_is_real_measured_map() ? 4u : 0u);

    TerrainPush push = {
        .dir_bda         = dir_bda,
        .indirect_bda    = indirect_bda,
        .width           = width,
        .height          = height,
        .elev_base_m     = elev_base,
        .vertical_scale  = v_scale,
        .marker_x        = marker_x,
        .marker_y        = marker_y,
        .marker_active   = marker_active ? 1.0f : 0.0f,
        .marker_b_x      = marker_b_x,
        .marker_b_y      = marker_b_y,
        .marker_b_active = marker_b_active ? 1.0f : 0.0f,
        .flags           = push_flags,
        .pad0            = 0u,
    };
    view_push_fill(&push.view, k_mat_solid0);

    // Ensure quadtree LOD cut is up to date if draw was invoked without a prior compute record
    (void)backend_update_camera_zoom(push.view.cam_x, push.view.cam_y, push.view.zoom);

    uint32_t leaf_count = backend_active_leaf_count();
    if (leaf_count == 0u) {
        return;
    }

    PlaneGrid grid = plane_grid_for(width, height, push.view.zoom);
    push.cell_med  = grid.cell_med;
    push.coarse    = grid.coarse;

    if (!vulkan_gfx_bind(s_vk, cmd, &s_gfx)) {
        return;
    }
    vulkan_cmd_push(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    sizeof push, &push);

    VkDeviceSize base_off = 0u;
    VkBuffer ind_buf = backend_indirect_buffer(&base_off);
    if (ind_buf != VK_NULL_HANDLE && indirect_bda != 0ull) {
        if (s_vk->device_info.supports_draw_indirect_count &&
            s_vk->device_info.supports_multi_draw_indirect &&
            s_vk->device_info.supports_draw_indirect_first_instance) {
            vkCmdDrawIndirectCount(cmd,
                                   ind_buf,
                                   base_off + offsetof(GpuMathIndirectBuffer, cmds),
                                   ind_buf,
                                   base_off + offsetof(GpuMathIndirectBuffer, draw_count),
                                   k_epack_v2_cut_cap,
                                   sizeof(GpuMathDrawCmd));
        } else {
            vkCmdDrawIndirect(cmd,
                              ind_buf,
                              base_off + offsetof(GpuMathIndirectBuffer, single_cmd),
                              1u,
                              sizeof(GpuMathDrawCmd));
        }
    } else {
        vkCmdDraw(cmd, k_terrain_tile_verts, leaf_count, 0u, 0u);
    }

    backend_record_draw_surfels(cmd,
                                width,
                                height,
                                pass_hdr_format(),
                                pass_depth_format(),
                                pass_raster_samples(),
                                elev_base,
                                v_scale,
                                &push.view,
                                sizeof push.view);
}

void terrain_set_pack_path(const char *path) {
    const char *norm = (path != nullptr && path[0] != '\0') ? path : "";
    bool changed = (strcmp(s_pack_path, norm) != 0);
    if (changed && s_backend_ready && s_vk != nullptr) {
        if (s_vk->device != VK_NULL_HANDLE) {
            (void)vkDeviceWaitIdle(s_vk->device);
        }
        terrain_release_backend(s_vk);
    }
    if (norm[0] != '\0') {
        snprintf(s_pack_path, sizeof s_pack_path, "%s", norm);
        backend_set_pack_path(s_pack_path);
    } else {
        s_pack_path[0] = '\0';
        backend_set_pack_path(nullptr);
    }
}

void terrain_set_active(bool active) {
    s_world_terrain_active = active;
}

bool terrain_is_active(void) {
    return s_world_terrain_active || s_backend_ready;
}

bool terrain_reload(void) {
    if (!s_backend_ready) {
        return false;
    }
    return backend_reload_pack();
}

uint32_t terrain_active_vertices(void) {
    if (!s_backend_ready) {
        return 0u;
    }
    return backend_visible_leaf_count() * k_terrain_tile_verts;
}

void terrain_set_contour_mode(uint32_t mode) {
    s_contour_mode = mode % 3u;
}

uint32_t terrain_toggle_contour_mode(void) {
    s_contour_mode = (s_contour_mode + 1u) % 3u;
    const char *label = (s_contour_mode == 0u) ? "OFF"
                      : (s_contour_mode == 1u) ? "Zoom-Adaptive Contours (0.5m/1m/5m/20m/100m)"
                                               : "Hypsometric Elevation Tint + Precision Contours";
    printf("[Terrain] Topographic overlay mode %u: %s\n", s_contour_mode, label);
    fflush(stdout);
    return s_contour_mode;
}

uint32_t terrain_contour_mode(void) {
    return s_contour_mode;
}

void terrain_set_contours(bool enabled) {
    s_contour_mode = enabled ? 1u : 0u;
}

bool terrain_toggle_contours(void) {
    (void)terrain_toggle_contour_mode();
    return true;
}

bool terrain_contours_enabled(void) {
    return s_contour_mode != 0u;
}
