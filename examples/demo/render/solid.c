#include "frame/solid.h"
#include "frame/camera.h"
#include "frame/pass.h"
#include "frame/material.h"
#include "vulkan/bda.h"
#include "vulkan/pipe.h"
#include <stddef.h>
#include <stdio.h>

alignas(uint32_t) static const unsigned char k_vert_spv[] = {
#embed "solid.vert.spv"
};
alignas(uint32_t) static const unsigned char k_frag_spv[] = {
#embed "solid.frag.spv"
};
alignas(uint32_t) static const unsigned char k_bake_spv[] = {
#embed "solid.comp.spv"
};

// Keep these ranges in lockstep with shaders/solid.comp.
enum { k_solid_verts = 5964u, k_local = 64u };

static_assert(sizeof k_vert_spv % 4u == 0u, "solid vert spirv");
static_assert(sizeof k_frag_spv % 4u == 0u, "solid frag spirv");
static_assert(sizeof k_bake_spv % 4u == 0u, "solid bake spirv");

typedef struct SolidPush {
    uint64_t verts;
    uint32_t width;
    uint32_t height;
    float    yaw_deg;
    float    origin_x;
    float    origin_y;
    float    depth_bias;
    float    selected;
    uint32_t pad;
    uint32_t tail0;
    uint32_t tail1;
    ViewPush view;
} SolidPush;

typedef struct BakePush {
    uint64_t verts;
} BakePush;

static_assert(offsetof(SolidPush, yaw_deg) == 16u, "solid yaw");
static_assert(offsetof(SolidPush, origin_x) == 20u, "solid origin x");
static_assert(offsetof(SolidPush, origin_y) == 24u, "solid origin y");
static_assert(offsetof(SolidPush, depth_bias) == 28u, "solid depth");
static_assert(offsetof(SolidPush, selected) == 32u, "solid selected");
static_assert(offsetof(SolidPush, view) == 48u, "solid view");
static_assert(sizeof(SolidPush) == 96u, "solid push");
static_assert(sizeof(BakePush) == 8u, "solid bake push");

// [kind][0 faceted, 1 rounded]
static const uint32_t k_first[6][2] = {
    { 0u, 12u },
    { 204u, 228u },
    { 612u, 672u },
    { 1632u, 1704u },
    { 2280u, 2316u },
    { 2604u, 2892u },
};
static const uint32_t k_count[6][2] = {
    { 12u, 192u },
    { 24u, 384u },
    { 60u, 960u },
    { 72u, 576u },
    { 36u, 288u },
    { 288u, 3072u },
};

static VkPipelineLayout s_layout = VK_NULL_HANDLE;
static VkPipeline       s_pipeline = VK_NULL_HANDLE;
static VkPipelineLayout s_bake_layout = VK_NULL_HANDLE;
static VkPipeline       s_bake = VK_NULL_HANDLE;
static VkBuffer         s_buffer = VK_NULL_HANDLE;
static VkDeviceMemory   s_memory = VK_NULL_HANDLE;
static VkDeviceAddress  s_addr = 0;
static void solid_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Solid Error] %s: %s (%d)\n", what, vulkan_vkresult_to_str(res), (int)res);
}

static int create_da_buffer(VulkanContext *vk, VkDeviceSize size, VkBufferUsageFlags usage,
                            VkBuffer *buffer, VkDeviceMemory *memory, VkDeviceAddress *addr) {
    DaBuffer buf = {0};
    if (da_buffer_create(vk, size, usage, false, &buf) != 0) {
        return -1;
    }
    *buffer = buf.buffer;
    *memory = buf.memory;
    *addr = buf.address;
    return 0;
}

static int bake_mesh(VulkanContext *vk) {
    if (vk->cmd_pool == VK_NULL_HANDLE || vk->queues.graphics_queue == VK_NULL_HANDLE ||
        s_bake == VK_NULL_HANDLE) {
        fprintf(stderr, "[Solid Error] Mesh bake needs the graphics queue and command pool\n");
        return -1;
    }
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo alloc_info = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = vk->cmd_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkResult res = vkAllocateCommandBuffers(vk->device, &alloc_info, &cmd);
    if (res != VK_SUCCESS) {
        solid_fail("vkAllocateCommandBuffers bake", res);
        return -1;
    }
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    res = vkBeginCommandBuffer(cmd, &begin_info);
    if (res != VK_SUCCESS) {
        solid_fail("vkBeginCommandBuffer bake", res);
        vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cmd);
        return -1;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_bake);
    BakePush bake_push = { .verts = s_addr };
    vulkan_cmd_push(cmd, s_bake_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof bake_push, &bake_push);
    vkCmdDispatch(cmd, (k_solid_verts + k_local - 1u) / k_local, 1u, 1u);
    VkMemoryBarrier2 mesh = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    };
    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &mesh,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
    res = vkEndCommandBuffer(cmd);
    if (res != VK_SUCCESS) {
        solid_fail("vkEndCommandBuffer bake", res);
        vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cmd);
        return -1;
    }
    VkCommandBufferSubmitInfo cmd_submit = {
        .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd,
    };
    VkSubmitInfo2 submit_info = {
        .sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos    = &cmd_submit,
    };
    res = vkQueueSubmit2(vk->queues.graphics_queue, 1, &submit_info, VK_NULL_HANDLE);
    if (res != VK_SUCCESS) {
        solid_fail("vkQueueSubmit2 bake", res);
        vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cmd);
        return -1;
    }
    res = vkQueueWaitIdle(vk->queues.graphics_queue);
    vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cmd);
    if (res != VK_SUCCESS) {
        solid_fail("vkQueueWaitIdle bake", res);
        return -1;
    }
    return 0;
}

void solid_shutdown(VulkanContext *vk) {
    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        if (s_bake != VK_NULL_HANDLE) {
            vkDestroyPipeline(vk->device, s_bake, nullptr);
        }
        if (s_bake_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(vk->device, s_bake_layout, nullptr);
        }
        if (s_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(vk->device, s_pipeline, nullptr);
        }
        if (s_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(vk->device, s_layout, nullptr);
        }
        if (s_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(vk->device, s_buffer, nullptr);
        }
        if (s_memory != VK_NULL_HANDLE) {
            vkFreeMemory(vk->device, s_memory, nullptr);
        }
    }
    s_bake = VK_NULL_HANDLE;
    s_bake_layout = VK_NULL_HANDLE;
    s_pipeline = VK_NULL_HANDLE;
    s_layout = VK_NULL_HANDLE;
    s_buffer = VK_NULL_HANDLE;
    s_memory = VK_NULL_HANDLE;
    s_addr = 0;
}

int solid_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->physical_device == VK_NULL_HANDLE) {
        return -1;
    }
    if (s_pipeline != VK_NULL_HANDLE) {
        return 0;
    }
    VkFormat depth_format = pass_depth_format();
    VkSampleCountFlagBits samples = pass_raster_samples();
    if (depth_format == VK_FORMAT_UNDEFINED || samples == 0) {
        fprintf(stderr, "[Solid Error] The render pass has no depth format yet\n");
        return -1;
    }
    VkPhysicalDeviceProperties props = {0};
    vkGetPhysicalDeviceProperties(vk->physical_device, &props);
    if (props.limits.maxPushConstantsSize < sizeof(SolidPush)) {
        fprintf(stderr, "[Solid Error] Push constants are %u bytes, a solid needs %zu\n",
                props.limits.maxPushConstantsSize, sizeof(SolidPush));
        return -1;
    }
    if (k_first[5][1] + k_count[5][1] != k_solid_verts) {
        fprintf(stderr, "[Solid Error] Mesh ranges do not cover %u verts\n", k_solid_verts);
        return -1;
    }
    if (create_da_buffer(vk, (VkDeviceSize)k_solid_verts * 36u,
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         &s_buffer, &s_memory, &s_addr) != 0) {
        solid_shutdown(vk);
        return -1;
    }

    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset     = 0,
        .size       = sizeof(SolidPush),
    };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push,
    };
    VkResult res = vkCreatePipelineLayout(vk->device, &layout_info, nullptr, &s_layout);
    if (res != VK_SUCCESS) {
        solid_fail("vkCreatePipelineLayout", res);
        solid_shutdown(vk);
        return -1;
    }
    VkFormat color_format = VK_FORMAT_B8G8R8A8_SRGB;
    VulkanGfxPipe gfx = {
        .vert          = k_vert_spv,
        .vert_bytes    = sizeof k_vert_spv,
        .frag          = k_frag_spv,
        .frag_bytes    = sizeof k_frag_spv,
        .layout        = s_layout,
        .depth_format  = depth_format,
        .samples       = samples,
        .cull          = VK_CULL_MODE_BACK_BIT,
        .depth_test    = VK_TRUE,
        .depth_write   = VK_TRUE,
        .depth_compare = VK_COMPARE_OP_LESS,
        .blend         = VK_FALSE,
    };
    if (vulkan_gfx_pipelines(vk, &gfx, &color_format, nullptr, 1, &s_pipeline) != 0) {
        solid_shutdown(vk);
        return -1;
    }

    VkPushConstantRange bake_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = sizeof(BakePush),
    };
    VkPipelineLayoutCreateInfo bake_layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &bake_range,
    };
    res = vkCreatePipelineLayout(vk->device, &bake_layout_info, nullptr, &s_bake_layout);
    if (res != VK_SUCCESS) {
        solid_fail("vkCreatePipelineLayout bake", res);
        solid_shutdown(vk);
        return -1;
    }
    s_bake = vulkan_compute_pipeline(vk, k_bake_spv, sizeof k_bake_spv, s_bake_layout);
    if (s_bake == VK_NULL_HANDLE) {
        solid_shutdown(vk);
        return -1;
    }
    if (bake_mesh(vk) != 0) {
        solid_shutdown(vk);
        return -1;
    }
    printf("[Solid] 6 meshes, %u verts, faceted and rounded, baked on the GPU\n", k_solid_verts);
    fflush(stdout);
    return 0;
}

void solid_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                uint32_t kind, bool curved, float yaw_deg,
                float origin_x, float origin_y, bool selected) {
    if (cmd == VK_NULL_HANDLE || s_pipeline == VK_NULL_HANDLE || width == 0 || height == 0 ||
        kind >= 6u) {
        return;
    }
    uint32_t cage = curved ? 1u : 0u;
    SolidPush draw_push = {
        .verts      = s_addr,
        .width      = width,
        .height     = height,
        .yaw_deg    = yaw_deg,
        .origin_x   = origin_x,
        .origin_y   = origin_y,
        .depth_bias = 0.0f,
        .selected   = selected ? 1.0f : 0.0f,
        .pad        = 0,
    };
    view_push_fill(&draw_push.view, k_mat_solid0 + kind);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline);
    vulkan_cmd_push(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT, sizeof draw_push, &draw_push);
    vkCmdDraw(cmd, k_count[kind][cage], 1, k_first[kind][cage], 0);
}

uint32_t solid_vertex_count(void) {
    return k_solid_verts;
}

uint32_t solid_vertex_count_for(uint32_t kind, bool curved) {
    if (kind >= 6u) {
        return 0;
    }
    return k_count[kind][curved ? 1u : 0u];
}
