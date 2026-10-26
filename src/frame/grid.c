#include "frame/grid.h"
#include "frame/cube.h"
#include "vulkan/bda.h"
#include "vulkan/memory.h"
#include <stdio.h>

alignas(uint32_t) static const unsigned char k_grid_spv[] = {
#embed "grid.comp.spv"
};

enum : uint32_t {
    k_n        = 128u,
    k_cells    = k_n * k_n * k_n,
    k_local    = 64u,
    k_field_bytes = k_cells * (uint32_t)sizeof(float),
    k_arena_bytes = k_field_bytes * 2u + (64u * 1024u),
};

typedef struct GridPush {
    uint64_t src;
    uint64_t dst;
    uint64_t tint;
    uint64_t verts;
    uint32_t dt_ms;
    uint32_t seed;
    uint32_t mode;
    uint32_t nverts;
} GridPush;

static_assert(sizeof(GridPush) == 48u, "grid push");
static_assert(sizeof k_grid_spv % 4u == 0u, "grid spirv");

static BdaArena         s_arena;
static VkDeviceAddress  s_field[2];
static VkDeviceAddress  s_tint;
static int              s_read = 0;
static bool             s_step_open = false;
static bool             s_held = false;
static bool             s_hold_logged = false;
static VkPipelineLayout s_layout = VK_NULL_HANDLE;
static VkPipeline       s_pipe = VK_NULL_HANDLE;

static void grid_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Grid Error] %s: %s (%d)\n", what, vulkan_vkresult_to_str(res), (int)res);
}

static void barrier_field(VkCommandBuffer cmd) {
    VkMemoryBarrier2 barrier = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    };
    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

static void dispatch_grid(VkCommandBuffer cmd, uint32_t mode, uint32_t dt_ms, uint32_t seed,
                          VkDeviceAddress src, VkDeviceAddress dst) {
    GridPush push = {
        .src    = src,
        .dst    = dst,
        .tint   = s_tint,
        .verts  = cube_mesh_addr(),
        .dt_ms  = dt_ms,
        .seed   = seed,
        .mode   = mode,
        .nverts = cube_vertex_count(),
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipe);
    vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    uint32_t n = mode == 0u ? k_cells : cube_vertex_count();
    vkCmdDispatch(cmd, (n + k_local - 1u) / k_local, 1u, 1u);
}

static int submit_idle(VulkanContext *vk, VkCommandBuffer cmd) {
    VkResult res = vkEndCommandBuffer(cmd);
    if (res != VK_SUCCESS) {
        grid_fail("vkEndCommandBuffer", res);
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
        grid_fail("vkQueueSubmit2", res);
        return -1;
    }
    res = vkQueueWaitIdle(vk->queues.graphics_queue);
    if (res != VK_SUCCESS) {
        grid_fail("vkQueueWaitIdle", res);
        return -1;
    }
    return 0;
}

void grid_shutdown(VulkanContext *vk) {
    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        if (s_pipe != VK_NULL_HANDLE) {
            vkDestroyPipeline(vk->device, s_pipe, nullptr);
        }
        if (s_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(vk->device, s_layout, nullptr);
        }
        bda_arena_destroy(vk, &s_arena);
    }
    s_pipe = VK_NULL_HANDLE;
    s_layout = VK_NULL_HANDLE;
    s_field[0] = 0;
    s_field[1] = 0;
    s_tint = 0;
    s_read = 0;
    s_step_open = false;
    s_held = false;
    s_hold_logged = false;
}

int grid_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->cmd_pool == VK_NULL_HANDLE ||
        vk->queues.graphics_queue == VK_NULL_HANDLE) {
        return -1;
    }
    if (s_pipe != VK_NULL_HANDLE) {
        grid_shutdown(vk);
    }
    if (cube_mesh_addr() == 0 || cube_vertex_count() == 0u) {
        fprintf(stderr, "[Grid Error] The cube mesh is not ready\n");
        return -1;
    }
    if (bda_arena_init(vk, &s_arena, k_arena_bytes) != 0) {
        return -1;
    }
    BdaSlice a = bda_arena_alloc(&s_arena, k_field_bytes, 256u);
    BdaSlice b = bda_arena_alloc(&s_arena, k_field_bytes, 256u);
    BdaSlice tint = bda_arena_alloc(&s_arena, cube_vertex_count() * (uint32_t)sizeof(float), 256u);
    if (a.gpu == 0 || b.gpu == 0 || tint.gpu == 0) {
        grid_shutdown(vk);
        return -1;
    }
    s_field[0] = a.gpu;
    s_field[1] = b.gpu;
    s_tint = tint.gpu;

    VkPushConstantRange range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = sizeof(GridPush),
    };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &range,
    };
    VkResult res = vkCreatePipelineLayout(vk->device, &layout_info, nullptr, &s_layout);
    if (res != VK_SUCCESS) {
        grid_fail("vkCreatePipelineLayout", res);
        grid_shutdown(vk);
        return -1;
    }
    VkShaderModuleCreateInfo module_info = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof k_grid_spv,
        .pCode    = (const uint32_t *)k_grid_spv,
    };
    VkShaderModule module = VK_NULL_HANDLE;
    res = vkCreateShaderModule(vk->device, &module_info, nullptr, &module);
    if (res != VK_SUCCESS) {
        grid_fail("vkCreateShaderModule", res);
        grid_shutdown(vk);
        return -1;
    }
    VkPipelineShaderStageCreateInfo stage = {
        .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = module,
        .pName  = "main",
    };
    VkComputePipelineCreateInfo pipe = {
        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage  = stage,
        .layout = s_layout,
    };
    res = vkCreateComputePipelines(vk->device, VK_NULL_HANDLE, 1, &pipe, nullptr, &s_pipe);
    vkDestroyShaderModule(vk->device, module, nullptr);
    if (res != VK_SUCCESS) {
        grid_fail("vkCreateComputePipelines", res);
        grid_shutdown(vk);
        return -1;
    }

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo alloc_info = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = vk->cmd_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    res = vkAllocateCommandBuffers(vk->device, &alloc_info, &cmd);
    if (res != VK_SUCCESS) {
        grid_fail("vkAllocateCommandBuffers", res);
        grid_shutdown(vk);
        return -1;
    }
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    res = vkBeginCommandBuffer(cmd, &begin_info);
    if (res != VK_SUCCESS) {
        grid_fail("vkBeginCommandBuffer", res);
        vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cmd);
        grid_shutdown(vk);
        return -1;
    }
    dispatch_grid(cmd, 0u, 0u, 1u, 0, s_field[0]);
    barrier_field(cmd);
    dispatch_grid(cmd, 1u, 0u, 0u, 0, s_field[0]);
    barrier_field(cmd);
    if (submit_idle(vk, cmd) != 0) {
        vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cmd);
        grid_shutdown(vk);
        return -1;
    }
    vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cmd);
    s_read = 0;
    s_step_open = false;
    s_held = false;
    s_hold_logged = false;
    printf("[Grid] 128^3 diffusion, center ball, one sample per cube vertex.\n");
    fflush(stdout);
    return 0;
}

void grid_record(VkCommandBuffer cmd, uint32_t dt_ms) {
    if (cmd == VK_NULL_HANDLE || dt_ms == 0u || s_held || s_step_open ||
        s_pipe == VK_NULL_HANDLE || s_field[0] == 0 || s_field[1] == 0) {
        return;
    }
    int dst = s_read ^ 1;
    dispatch_grid(cmd, 0u, dt_ms, 0u, s_field[s_read], s_field[dst]);
    barrier_field(cmd);
    dispatch_grid(cmd, 1u, dt_ms, 0u, 0, s_field[dst]);
    barrier_field(cmd);
    s_step_open = true;
}

void grid_record_ok(void) {
    if (!s_step_open) {
        return;
    }
    s_read ^= 1;
    s_step_open = false;
}

void grid_record_abort(void) {
    s_step_open = false;
}

void grid_hold(void) {
    if (s_held) {
        return;
    }
    s_held = true;
    s_step_open = false;
    if (s_hold_logged || s_tint == 0) {
        return;
    }
    s_hold_logged = true;
    printf("[Grid] 128^3 holds.\n");
    fflush(stdout);
}

void grid_wake(void) {
    s_held = false;
}

uint64_t grid_tint_addr(void) {
    return s_tint;
}
