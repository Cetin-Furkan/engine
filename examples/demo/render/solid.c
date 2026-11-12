#include "frame/solid.h"
#include "frame/material.h"
#include "scene/scene.h"
#include "vulkan/bda.h"
#include "vulkan/pipe.h"
#include <stddef.h>
#include <stdio.h>

alignas(uint32_t) static const unsigned char k_bake_spv[] = {
#embed "solid.comp.spv"
};

// Keep these ranges in lockstep with shaders/solid.comp.slang.
enum { k_solid_verts = 5964u, k_local = 64u };

static_assert(sizeof k_bake_spv % 4u == 0u, "solid bake spirv");

typedef struct BakePush {
    uint64_t verts;
} BakePush;

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

static VulkanContext *s_vk          = nullptr;
static bool           s_solid_ready = false;
static uint32_t       s_mesh_ids[6][2];

static void solid_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Solid Error] %s: %s (%d)\n", what, vulkan_vkresult_to_str(res), (int)res);
}

static int bake_mesh(VulkanContext *vk, VkPipeline bake_pipe, VkPipelineLayout bake_layout,
                     VkDeviceAddress out_addr) {
    VkCommandPool pool = vk->compute_cmd_pool != VK_NULL_HANDLE ? vk->compute_cmd_pool : vk->cmd_pool;
    VkQueue queue = vk->queues.compute_queue != VK_NULL_HANDLE ? vk->queues.compute_queue : vk->queues.graphics_queue;
    if (pool == VK_NULL_HANDLE || queue == VK_NULL_HANDLE || bake_pipe == VK_NULL_HANDLE) {
        fprintf(stderr, "[Solid Error] Mesh bake needs compute queue and command pool\n");
        return -1;
    }
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo alloc_info = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1u,
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
        vkFreeCommandBuffers(vk->device, pool, 1u, &cmd);
        return -1;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bake_pipe);
    BakePush bake_push = { .verts = out_addr };
    vulkan_cmd_push(cmd, bake_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof bake_push, &bake_push);
    vkCmdDispatch(cmd, (k_solid_verts + k_local - 1u) / k_local, 1u, 1u);
    VkMemoryBarrier2 host_vis = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
    };
    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1u,
        .pMemoryBarriers    = &host_vis,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
    res = vkEndCommandBuffer(cmd);
    if (res != VK_SUCCESS) {
        solid_fail("vkEndCommandBuffer bake", res);
        vkFreeCommandBuffers(vk->device, pool, 1u, &cmd);
        return -1;
    }
    VkCommandBufferSubmitInfo cmd_submit = {
        .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd,
    };
    VkSubmitInfo2 submit_info = {
        .sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1u,
        .pCommandBufferInfos    = &cmd_submit,
    };
    res = vkQueueSubmit2(queue, 1u, &submit_info, VK_NULL_HANDLE);
    if (res != VK_SUCCESS) {
        solid_fail("vkQueueSubmit2 bake", res);
        vkFreeCommandBuffers(vk->device, pool, 1u, &cmd);
        return -1;
    }
    res = vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(vk->device, pool, 1u, &cmd);
    if (res != VK_SUCCESS) {
        solid_fail("vkQueueWaitIdle bake", res);
        return -1;
    }
    return 0;
}

void solid_shutdown(VulkanContext *vk) {
    (void)vk;
    for (uint32_t k = 0u; k < 6u; ++k) {
        s_mesh_ids[k][0] = K_SCENE_INVALID_MESH;
        s_mesh_ids[k][1] = K_SCENE_INVALID_MESH;
    }
    s_vk          = nullptr;
    s_solid_ready = false;
}

static bool solid_ensure_ready(void) {
    if (s_solid_ready) {
        return true;
    }
    VulkanContext *vk = s_vk;
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->physical_device == VK_NULL_HANDLE) {
        return false;
    }
    if (k_first[5][1] + k_count[5][1] != k_solid_verts) {
        fprintf(stderr, "[Solid Error] Mesh ranges do not cover %u verts\n", k_solid_verts);
        return false;
    }

    DaBuffer bake_buf = {0};
    if (da_buffer_create(vk, (VkDeviceSize)k_solid_verts * sizeof(SceneCorner),
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         true, &bake_buf) != 0 || bake_buf.map == nullptr) {
        da_buffer_destroy(vk, &bake_buf);
        return false;
    }

    VkPushConstantRange bake_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0u,
        .size       = sizeof(BakePush),
    };
    VkPipelineLayoutCreateInfo bake_layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1u,
        .pPushConstantRanges    = &bake_range,
    };
    VkPipelineLayout bake_layout = VK_NULL_HANDLE;
    VkResult res = vkCreatePipelineLayout(vk->device, &bake_layout_info, nullptr, &bake_layout);
    if (res != VK_SUCCESS) {
        solid_fail("vkCreatePipelineLayout bake", res);
        da_buffer_destroy(vk, &bake_buf);
        return false;
    }

    VkPipeline bake_pipe = vulkan_compute_pipeline(vk, k_bake_spv, sizeof k_bake_spv, bake_layout);
    int rc = (bake_pipe != VK_NULL_HANDLE)
        ? bake_mesh(vk, bake_pipe, bake_layout, bake_buf.address)
        : -1;

    if (bake_pipe != VK_NULL_HANDLE) {
        vulkan_pipe_release(vk, bake_pipe);
    }
    vkDestroyPipelineLayout(vk->device, bake_layout, nullptr);

    if (rc != 0) {
        da_buffer_destroy(vk, &bake_buf);
        return false;
    }

    const SceneCorner *baked = (const SceneCorner *)bake_buf.map;
    for (uint32_t k = 0u; k < 6u; ++k) {
        for (uint32_t c = 0u; c < 2u; ++c) {
            uint32_t mid = scene_register_mesh(baked + k_first[k][c], k_count[k][c], 0.30f);
            if (mid == K_SCENE_INVALID_MESH) {
                da_buffer_destroy(vk, &bake_buf);
                return false;
            }
            s_mesh_ids[k][c] = mid;
        }
    }

    // Free transient 36B/vert soup immediately; only 16B/vert quantized meshlets remain in BDA slab.
    da_buffer_destroy(vk, &bake_buf);

    s_solid_ready = true;
    printf("[Solid] 6 procedural solids (12 faceted/rounded cages, %u corners) baked & clusterized into 16B/vert meshlets\n",
           k_solid_verts);
    fflush(stdout);
    return true;
}

int solid_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->physical_device == VK_NULL_HANDLE) {
        return -1;
    }
    for (uint32_t k = 0u; k < 6u; ++k) {
        s_mesh_ids[k][0] = K_SCENE_INVALID_MESH;
        s_mesh_ids[k][1] = K_SCENE_INVALID_MESH;
    }
    s_vk          = vk;
    s_solid_ready = false;
    return 0;
}

void solid_submit_scene(uint32_t kind, bool curved, float yaw_deg,
                        float origin_x, float origin_y, float origin_z,
                        float squash, bool selected) {
    if (kind >= 6u || !solid_ensure_ready()) {
        return;
    }
    uint32_t cage = curved ? 1u : 0u;
    uint32_t mid  = s_mesh_ids[kind][cage];
    if (mid == K_SCENE_INVALID_MESH) {
        return;
    }
    const float k_deg_to_rad = 0.017453292519943295f;
    scene_submit_instance(mid,
                          origin_x, origin_y, origin_z,
                          84.0f,
                          28.0f * k_deg_to_rad,
                          yaw_deg * k_deg_to_rad,
                          squash,
                          0.30f,
                          selected ? 1.0f : 0.0f,
                          0.0f,
                          k_mat_solid0 + kind,
                          0u);
}

uint32_t solid_vertex_count_for(uint32_t kind, bool curved) {
    if (kind >= 6u) {
        return 0u;
    }
    return k_count[kind][curved ? 1u : 0u];
}
