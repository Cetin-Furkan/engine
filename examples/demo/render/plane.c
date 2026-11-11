#include "frame/plane.h"
#include "frame/camera.h"
#include "frame/grid_math.h"
#include "frame/pass.h"
#include "vulkan/pipe.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>

alignas(uint32_t) static const unsigned char k_vert_spv[] = {
#embed "plane.vert.spv"
};
alignas(uint32_t) static const unsigned char k_frag_spv[] = {
#embed "plane.frag.spv"
};

static_assert(sizeof k_vert_spv % 4u == 0u, "plane vert spirv");
static_assert(sizeof k_frag_spv % 4u == 0u, "plane frag spirv");

// One fullscreen triangle, no vertex buffer. The fragment shader paints
// the metric in screen space, so the sheet stays behind the scene at any
// zoom and does not stand up when the camera yaws. Decades still come
// from plane_grid_for. span is not sampled.
typedef struct PlanePush {
    uint32_t width;
    uint32_t height;
    float    cell_med;
    float    level_frac;
    float    origin_x;
    float    origin_y;
    float    zoom;
    float    coarse;
} PlanePush;

static_assert(offsetof(PlanePush, cell_med) == 8u, "plane cell_med");
static_assert(offsetof(PlanePush, origin_x) == 16u, "plane origin");
static_assert(offsetof(PlanePush, zoom) == 24u, "plane zoom");
static_assert(offsetof(PlanePush, coarse) == 28u, "plane coarse");
static_assert(sizeof(PlanePush) == 32u, "plane push");

static VulkanContext   *s_vk = nullptr;
static VkPipelineLayout s_layout = VK_NULL_HANDLE;
static VulkanGfxLive    s_gfx;
static bool             s_logged = false;

static void plane_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Plane Error] %s: %s (%d)\n", what, vulkan_vkresult_to_str(res), (int)res);
}

void plane_shutdown(VulkanContext *vk) {
    vulkan_gfx_disarm(vk, &s_gfx);
    s_vk = nullptr;
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        s_layout = VK_NULL_HANDLE;
        return;
    }
    if (s_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, s_layout, nullptr);
        s_layout = VK_NULL_HANDLE;
    }
}

int plane_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->physical_device == VK_NULL_HANDLE) {
        return -1;
    }
    s_vk = vk;
    return 0;
}

static bool plane_ensure_ready(void) {
    if (s_layout != VK_NULL_HANDLE) {
        return true;
    }
    if (s_vk == nullptr || s_vk->device == VK_NULL_HANDLE || s_vk->physical_device == VK_NULL_HANDLE) {
        return false;
    }
    VkFormat depth_format = pass_depth_format();
    VkSampleCountFlagBits samples = pass_raster_samples();
    if (depth_format == VK_FORMAT_UNDEFINED || samples == 0) {
        fprintf(stderr, "[Plane Error] The render pass has no depth format yet\n");
        return false;
    }
    VkPhysicalDeviceProperties props = {0};
    vkGetPhysicalDeviceProperties(s_vk->physical_device, &props);
    if (props.limits.maxPushConstantsSize < sizeof(PlanePush)) {
        fprintf(stderr, "[Plane Error] Push constants are %u bytes, the plane needs %zu\n",
                props.limits.maxPushConstantsSize, sizeof(PlanePush));
        return false;
    }

    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(PlanePush),
    };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push,
    };
    VkResult res = vkCreatePipelineLayout(s_vk->device, &layout_info, nullptr, &s_layout);
    if (res != VK_SUCCESS) {
        s_layout = VK_NULL_HANDLE;
        plane_fail("vkCreatePipelineLayout", res);
        return false;
    }

    // Drawn first, with depth test and depth write off. Later geometry
    // replaces these pixels. The clear depth is left for the meshes.
    // The pipeline is created on the first draw of this format.
    VulkanGfxPipe gfx = {
        .vert          = k_vert_spv,
        .vert_bytes    = sizeof k_vert_spv,
        .frag          = k_frag_spv,
        .frag_bytes    = sizeof k_frag_spv,
        .layout        = s_layout,
        .depth_format  = depth_format,
        .samples       = samples,
        .cull          = VK_CULL_MODE_NONE,
        .depth_test    = VK_FALSE,
        .depth_write   = VK_FALSE,
        .depth_compare = VK_COMPARE_OP_ALWAYS,
        .blend         = VK_FALSE,
    };
    vulkan_gfx_arm(&s_gfx, &gfx, VK_FORMAT_B8G8R8A8_SRGB, nullptr);
    if (!s_logged) {
        printf("[Plane] Screen-space metric grid behind the scene (live 10^k subdivisions).\n");
        fflush(stdout);
        s_logged = true;
    }
    return true;
}

void plane_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height) {
    if (cmd == VK_NULL_HANDLE || width == 0 || height == 0 || !plane_ensure_ready()) {
        return;
    }
    VkPipeline pipeline = vulkan_gfx_use(s_vk, &s_gfx);
    if (pipeline == VK_NULL_HANDLE) {
        return;
    }
    ViewPush view;
    view_push_fill(&view, 0u);
    PlaneGrid grid = plane_grid_for(width, height, view.zoom);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    PlanePush push = {
        .width      = width,
        .height     = height,
        .cell_med   = grid.cell_med,
        .level_frac = grid.level_frac,
        .origin_x   = plane_grid_wrap(view.cam_x, grid.coarse),
        .origin_y   = plane_grid_wrap(view.cam_y, grid.coarse),
        .zoom       = view.zoom,
        .coarse     = grid.coarse,
    };
    vulkan_cmd_push(cmd, s_layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof push, &push);
    vkCmdDraw(cmd, 3u, 1u, 0u, 0u);
}
