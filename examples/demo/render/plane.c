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

// One 6-vertex quad on the true z = 0 plane, no vertex buffer. The
// metric decades are analytic in the fragment shader. The old 3-level
// clipmap sat at z = -170 behind the meshes; this plane is at the
// world ground and depth-tests (without writing) against real geometry.
typedef struct PlanePush {
    uint32_t width;
    uint32_t height;
    float    cell_med;
    float    level_frac;
    float    origin_x;
    float    origin_y;
    float    span;
    float    span_mod;
    float    span_steps;
    float    pad2;
    uint32_t pad3;
    uint32_t pad4;
    ViewPush view;
} PlanePush;

static_assert(offsetof(PlanePush, cell_med) == 8u, "plane cell_med");
static_assert(offsetof(PlanePush, origin_x) == 16u, "plane origin");
static_assert(offsetof(PlanePush, span) == 24u, "plane span");
static_assert(offsetof(PlanePush, span_mod) == 28u, "plane span mod");
static_assert(offsetof(PlanePush, span_steps) == 32u, "plane span steps");
static_assert(offsetof(PlanePush, view) == 48u, "plane view");
static_assert(sizeof(PlanePush) == 96u, "plane push");

static VkPipelineLayout s_layout = VK_NULL_HANDLE;
static VkPipeline       s_pipeline = VK_NULL_HANDLE;

static void plane_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Plane Error] %s: %s (%d)\n", what, vulkan_vkresult_to_str(res), (int)res);
}

void plane_shutdown(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        s_layout = VK_NULL_HANDLE;
        s_pipeline = VK_NULL_HANDLE;
        return;
    }
    if (s_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk->device, s_pipeline, nullptr);
        s_pipeline = VK_NULL_HANDLE;
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
    if (s_pipeline != VK_NULL_HANDLE) {
        return 0;
    }
    VkFormat depth_format = pass_depth_format();
    VkSampleCountFlagBits samples = pass_raster_samples();
    if (depth_format == VK_FORMAT_UNDEFINED || samples == 0) {
        fprintf(stderr, "[Plane Error] The render pass has no depth format yet\n");
        return -1;
    }
    VkPhysicalDeviceProperties props = {0};
    vkGetPhysicalDeviceProperties(vk->physical_device, &props);
    if (props.limits.maxPushConstantsSize < sizeof(PlanePush)) {
        fprintf(stderr, "[Plane Error] Push constants are %u bytes, the plane needs %zu\n",
                props.limits.maxPushConstantsSize, sizeof(PlanePush));
        return -1;
    }

    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset     = 0,
        .size       = sizeof(PlanePush),
    };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push,
    };
    VkResult res = vkCreatePipelineLayout(vk->device, &layout_info, nullptr, &s_layout);
    if (res != VK_SUCCESS) {
        s_layout = VK_NULL_HANDLE;
        plane_fail("vkCreatePipelineLayout", res);
        plane_shutdown(vk);
        return -1;
    }

    // Tests against meshes but never writes depth, so geometry centered
    // on z = 0 is never sliced by its own ground.
    VkFormat color_format = VK_FORMAT_B8G8R8A8_SRGB;
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
        .depth_write   = VK_FALSE,
        .depth_compare = VK_COMPARE_OP_LESS_OR_EQUAL,
        .blend         = VK_FALSE,
    };
    if (vulkan_gfx_pipelines(vk, &gfx, &color_format, nullptr, 1, &s_pipeline) != 0) {
        plane_shutdown(vk);
        return -1;
    }
    printf("[Plane] Universal 6-vertex metric grid on z = 0 (live 10^k subdivisions).\n");
    fflush(stdout);
    return 0;
}

void plane_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height) {
    if (cmd == VK_NULL_HANDLE || s_pipeline == VK_NULL_HANDLE || width == 0 || height == 0) {
        return;
    }
    ViewPush view;
    view_push_fill(&view, 0u);
    PlaneGrid grid = plane_grid_for(width, height, view.zoom);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline);

    PlanePush push = {
        .width      = width,
        .height     = height,
        .cell_med   = grid.cell_med,
        .level_frac = grid.level_frac,
        .origin_x   = plane_grid_wrap(view.cam_x, grid.coarse),
        .origin_y   = plane_grid_wrap(view.cam_y, grid.coarse),
        .span       = grid.span,
        .span_mod   = grid.span_mod,
        .span_steps = grid.span_steps,
        .view       = view,
    };
    vulkan_cmd_push(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT, sizeof push, &push);
    vkCmdDraw(cmd, 6u, 1u, 0u, 0u);
}
