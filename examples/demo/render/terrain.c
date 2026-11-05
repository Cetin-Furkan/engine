#include "frame/terrain.h"
#include "backend/backend.h"
#include "frame/camera.h"
#include "frame/grid_math.h"
#include "frame/material.h"
#include "frame/pass.h"
#include "vulkan/pipe.h"
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
    k_terrain_quads_side  = 127u,
    k_terrain_tile_verts  = k_terrain_quads_side * k_terrain_quads_side * 6u,
};

typedef struct TerrainPush {
    uint64_t dir_bda;
    uint32_t width;
    uint32_t height;
    float    elev_base_m;
    float    vertical_scale;
    float    marker_x;
    float    marker_y;
    float    marker_active;
    float    cell_med;
    float    coarse;
    uint32_t pad0;
    ViewPush view;
} TerrainPush;

static_assert(offsetof(TerrainPush, elev_base_m) == 16u, "terrain elev_base_m");
static_assert(offsetof(TerrainPush, marker_x) == 24u, "terrain marker_x");
static_assert(offsetof(TerrainPush, view) == 48u, "terrain view");
static_assert(sizeof(TerrainPush) == 96u, "terrain push");

static VulkanContext   *s_vk     = nullptr;
static VkPipelineLayout s_layout = VK_NULL_HANDLE;
static VulkanGfxLive    s_gfx;

static void terrain_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Terrain Error] %s: %s (%d)\n", what, vulkan_vkresult_to_str(res), (int)res);
}

void terrain_shutdown(VulkanContext *vk) {
    vulkan_gfx_disarm(vk, &s_gfx);
    backend_shutdown(vk);
    s_vk = nullptr;
    if (vk != nullptr && vk->device != VK_NULL_HANDLE && s_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, s_layout, nullptr);
    }
    s_layout = VK_NULL_HANDLE;
}

int terrain_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->physical_device == VK_NULL_HANDLE) {
        return -1;
    }
    s_vk = vk;
    if (s_layout != VK_NULL_HANDLE) {
        return 0;
    }
    if (backend_init(vk) != 0) {
        return -1;
    }
    VkFormat depth_format = pass_depth_format();
    VkSampleCountFlagBits samples = pass_raster_samples();
    if (depth_format == VK_FORMAT_UNDEFINED || samples == 0) {
        backend_shutdown(vk);
        return -1;
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
    VkResult res = vkCreatePipelineLayout(vk->device, &layout_info, nullptr, &s_layout);
    if (res != VK_SUCCESS) {
        terrain_fail("vkCreatePipelineLayout", res);
        terrain_shutdown(vk);
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
    printf("[Terrain] Adaptive CDLOD Quadtree renderer armed (Baklan & Beşparmak Dağı, 127x127x6 verts/tile).\n");
    fflush(stdout);
    return 0;
}

void terrain_record_compute(VkCommandBuffer cmd) {
    backend_record_compute(cmd);
}

void terrain_draw(VkCommandBuffer cmd,
                  uint32_t        width,
                  uint32_t        height,
                  float           marker_x,
                  float           marker_y,
                  bool            marker_active) {
    VkPipeline pipeline = vulkan_gfx_use(s_vk, &s_gfx);
    uint64_t dir_bda = backend_directory_bda();
    if (cmd == VK_NULL_HANDLE || pipeline == VK_NULL_HANDLE || dir_bda == 0ull ||
        width == 0u || height == 0u) {
        return;
    }

    TerrainPush push = {
        .dir_bda        = dir_bda,
        .width          = width,
        .height         = height,
        .elev_base_m    = k_besparmak_elev_min,
        .vertical_scale = 1.15f,
        .marker_x       = marker_x,
        .marker_y       = marker_y,
        .marker_active  = marker_active ? 1.0f : 0.0f,
        .pad0           = 0u,
    };
    view_push_fill(&push.view, k_mat_solid0);

    // Dynamically stream needed SSD quadtree tiles based on current camera ground pivot & zoom
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
    vkCmdDraw(cmd, k_terrain_tile_verts, leaf_count, 0u, 0u);
}

bool terrain_reload(void) {
    return backend_reload_pack();
}

uint32_t terrain_active_vertices(void) {
    return backend_active_leaf_count() * k_terrain_tile_verts;
}
