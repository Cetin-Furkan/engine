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

static VulkanContext   *s_vk               = nullptr;
static VkPipelineLayout s_layout           = VK_NULL_HANDLE;
static VulkanGfxLive    s_gfx;
static bool             s_backend_ready    = false;
static bool             s_backend_failed   = false;
static bool             s_contours_enabled = false;
static uint32_t         s_last_width       = 1920u;
static uint32_t         s_last_height      = 1080u;

static void terrain_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Terrain Error] %s: %s (%d)\n", what, vulkan_vkresult_to_str(res), (int)res);
}

void terrain_shutdown(VulkanContext *vk) {
    vulkan_gfx_disarm(vk, &s_gfx);
    if (s_backend_ready) {
        backend_shutdown(vk);
    }
    s_backend_ready  = false;
    s_backend_failed = false;
    s_vk = nullptr;
    if (vk != nullptr && vk->device != VK_NULL_HANDLE && s_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, s_layout, nullptr);
    }
    s_layout = VK_NULL_HANDLE;
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
    float ov_wx = 0.0f;
    float ov_wy = 0.0f;
    if (backend_overlay_get_center(&ov_wx, &ov_wy) &&
        (ov_wx != 1595.0f || ov_wy != -650.0f)) {
        float ov_wz = backend_sample_elevation_world_z(ov_wx, ov_wy);
        camera_set_home_pose(ov_wx, ov_wy, ov_wz, 0.0f, 0.42f, 0.48f);
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
        .depth_compare = VK_COMPARE_OP_LESS_OR_EQUAL,
        .blend         = VK_FALSE,
    };
    vulkan_gfx_arm(&s_gfx, &gfx, VK_FORMAT_B8G8R8A8_SRGB, nullptr);
    s_backend_ready = true;
    printf("[Terrain] Adaptive CDLOD Quadtree renderer armed (Baklan & Beşparmak Dağı, 64/32/16 adaptive mesh, indirect=%s).\n",
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
        .elev_base_m    = k_besparmak_elev_min,
        .vertical_scale = 1.15f,
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

    VkPipeline pipeline = vulkan_gfx_use(s_vk, &s_gfx);
    uint64_t dir_bda      = backend_directory_bda();
    uint64_t indirect_bda = backend_indirect_bda();
    if (pipeline == VK_NULL_HANDLE || dir_bda == 0ull) {
        return;
    }

    TerrainPush push = {
        .dir_bda         = dir_bda,
        .indirect_bda    = indirect_bda,
        .width           = width,
        .height          = height,
        .elev_base_m     = k_besparmak_elev_min,
        .vertical_scale  = 1.15f,
        .marker_x        = marker_x,
        .marker_y        = marker_y,
        .marker_active   = marker_active ? 1.0f : 0.0f,
        .marker_b_x      = marker_b_x,
        .marker_b_y      = marker_b_y,
        .marker_b_active = marker_b_active ? 1.0f : 0.0f,
        .flags           = terrain_contours_enabled() ? 1u : 0u,
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

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
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
                                   k_gmpack_active_cut_cap,
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

void terrain_set_contours(bool enabled) {
    s_contours_enabled = enabled;
}

bool terrain_toggle_contours(void) {
    s_contours_enabled = !s_contours_enabled;
    printf("[Terrain] Topographic contour overlay (20m minor / 100m index): %s\n",
           s_contours_enabled ? "ON" : "OFF");
    fflush(stdout);
    return true;
}

bool terrain_contours_enabled(void) {
    return s_contours_enabled;
}
