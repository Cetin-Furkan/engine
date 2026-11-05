#include "backend/internal.h"
#include <stddef.h>
#include <stdio.h>

// ============================================================================
// 256-Byte PULSE Active GPU Window & Slang 2026 Compute Filter (directory.c)
// ============================================================================

alignas(uint32_t) static const unsigned char k_terrain_filter_spv[] = {
#embed "terrain_filter.comp.spv"
};

typedef struct FilterPush {
    uint64_t dir_bda;
    uint64_t rel_tile_ptr;
    float    sample_spacing_m;
    uint32_t slot_index;
    uint32_t pad0;
    uint32_t pad1;
} FilterPush;

static_assert(sizeof(FilterPush) == 32u, "FilterPush must be 32 bytes");
static_assert(sizeof k_terrain_filter_spv % 4u == 0u, "terrain_filter.comp.spv alignment");

int backend_gpu_directory_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return -1;
    }
    VkPushConstantRange range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0u,
        .size       = sizeof(FilterPush),
    };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1u,
        .pPushConstantRanges    = &range,
    };
    if (vkCreatePipelineLayout(vk->device, &layout_info, nullptr, &g_backend.comp_layout) != VK_SUCCESS) {
        g_backend.comp_layout = VK_NULL_HANDLE;
        return -1;
    }
    g_backend.comp_pipe = vulkan_compute_pipeline(vk, k_terrain_filter_spv,
                                                  sizeof k_terrain_filter_spv,
                                                  g_backend.comp_layout);
    if (g_backend.comp_pipe == VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, g_backend.comp_layout, nullptr);
        g_backend.comp_layout = VK_NULL_HANDLE;
        return -1;
    }
    return 0;
}

void backend_gpu_directory_shutdown(VulkanContext *vk) {
    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        if (g_backend.comp_pipe != VK_NULL_HANDLE) {
            vulkan_pipe_release(vk, g_backend.comp_pipe);
        }
        if (g_backend.comp_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(vk->device, g_backend.comp_layout, nullptr);
        }
    }
    g_backend.comp_pipe   = VK_NULL_HANDLE;
    g_backend.comp_layout = VK_NULL_HANDLE;
}

void backend_gpu_directory_sync(void) {
    if (g_backend.gpu_hdr == nullptr) {
        return;
    }
    g_backend.gpu_hdr->dir.root_node_rel = relptr2d_pack(0, 0, 0, 0u, 1u);
    g_backend.gpu_hdr->dir.tile_pool_rel = relptr2d_pack((int32_t)k_gmpack_header_blocks, 0, 0, 0u, 1u);
}

void backend_gpu_record_filter(VkCommandBuffer cmd) {
    if (cmd == VK_NULL_HANDLE || g_backend.comp_pipe == VK_NULL_HANDLE ||
        g_backend.gpu_hdr == nullptr || g_backend.bda_buf.address == 0ull ||
        g_backend.dirty_slots_mask == 0ull) {
        return;
    }

    uint64_t mask = g_backend.dirty_slots_mask;
    g_backend.dirty_slots_mask = 0ull;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_backend.comp_pipe);

    for (uint32_t slot = 0u; slot < k_gmpack_gpu_slot_cap; ++slot) {
        if ((mask & (1ull << slot)) == 0ull) {
            continue;
        }
        uint32_t node_idx = g_backend.slot_to_node[slot];
        if (node_idx >= k_gmpack_node_count) {
            continue;
        }
        GpuMathQuadNode *node = &g_backend.gpu_hdr->nodes[node_idx];
        uint32_t grid_n = 1u << node->lod;
        float spacing_m = 24000.0f / (float)(grid_n * (k_gmpack_tile_dim - 1u));

        FilterPush push = {
            .dir_bda          = g_backend.bda_buf.address,
            .rel_tile_ptr     = node->rel_gpu_payload,
            .sample_spacing_m = spacing_m,
            .slot_index       = slot,
            .pad0             = 0u,
            .pad1             = 0u,
        };
        vulkan_cmd_push(cmd, g_backend.comp_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof push, &push);
        vkCmdDispatch(cmd, k_gmpack_tile_dim / 8u, k_gmpack_tile_dim / 8u, 1u);
    }

    VkMemoryBarrier2 barrier = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    };
    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1u,
        .pMemoryBarriers    = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}
