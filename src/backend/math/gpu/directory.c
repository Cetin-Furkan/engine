#include "backend/internal.h"
#include <stddef.h>
#include <stdio.h>

// ============================================================================
// 256-Byte PULSE Active GPU Window & Slang 2026 Compute Filter (directory.c)
// ============================================================================

alignas(uint32_t) static const unsigned char k_terrain_filter_spv[] = {
#embed "terrain_filter.comp.spv"
};
alignas(uint32_t) static const unsigned char k_terrain_cull_spv[] = {
#embed "terrain_cull.comp.spv"
};

typedef struct FilterPush {
    uint64_t dir_bda;
    uint64_t rel_tile_ptr;
    float    sample_spacing_m;
    uint32_t slot_index;
    uint32_t pad0;
    uint32_t pad1;
} FilterPush;

typedef struct CullPush {
    uint64_t dir_bda;
    uint64_t indirect_bda;
    float    qx;
    float    qy;
    float    qz;
    float    qw;
    float    cam_x;
    float    cam_y;
    float    cam_z;
    float    zoom;
    float    elev_base_m;
    float    vertical_scale;
    uint32_t width;
    uint32_t height;
    uint32_t leaf_count;
    uint32_t tile_verts;
} CullPush;

static_assert(sizeof(FilterPush) == 32u, "FilterPush must be 32 bytes");
static_assert(sizeof(CullPush) == 72u, "CullPush must be 72 bytes");
static_assert(sizeof k_terrain_filter_spv % 4u == 0u, "terrain_filter.comp.spv alignment");
static_assert(sizeof k_terrain_cull_spv % 4u == 0u, "terrain_cull.comp.spv alignment");

int backend_gpu_directory_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return -1;
    }
    VkPushConstantRange filter_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0u,
        .size       = sizeof(FilterPush),
    };
    VkPipelineLayoutCreateInfo filter_layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1u,
        .pPushConstantRanges    = &filter_range,
    };
    if (vkCreatePipelineLayout(vk->device, &filter_layout_info, nullptr, &g_backend.comp_layout) != VK_SUCCESS) {
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

    VkPushConstantRange cull_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0u,
        .size       = sizeof(CullPush),
    };
    VkPipelineLayoutCreateInfo cull_layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1u,
        .pPushConstantRanges    = &cull_range,
    };
    if (vkCreatePipelineLayout(vk->device, &cull_layout_info, nullptr, &g_backend.cull_layout) != VK_SUCCESS) {
        g_backend.cull_layout = VK_NULL_HANDLE;
        vulkan_pipe_release(vk, g_backend.comp_pipe);
        vkDestroyPipelineLayout(vk->device, g_backend.comp_layout, nullptr);
        g_backend.comp_pipe   = VK_NULL_HANDLE;
        g_backend.comp_layout = VK_NULL_HANDLE;
        return -1;
    }
    g_backend.cull_pipe = vulkan_compute_pipeline(vk, k_terrain_cull_spv,
                                                  sizeof k_terrain_cull_spv,
                                                  g_backend.cull_layout);
    if (g_backend.cull_pipe == VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, g_backend.cull_layout, nullptr);
        vulkan_pipe_release(vk, g_backend.comp_pipe);
        vkDestroyPipelineLayout(vk->device, g_backend.comp_layout, nullptr);
        g_backend.cull_layout = VK_NULL_HANDLE;
        g_backend.comp_pipe   = VK_NULL_HANDLE;
        g_backend.comp_layout = VK_NULL_HANDLE;
        return -1;
    }
    return 0;
}

void backend_gpu_directory_shutdown(VulkanContext *vk) {
    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        if (g_backend.cull_pipe != VK_NULL_HANDLE) {
            vulkan_pipe_release(vk, g_backend.cull_pipe);
        }
        if (g_backend.cull_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(vk->device, g_backend.cull_layout, nullptr);
        }
        if (g_backend.comp_pipe != VK_NULL_HANDLE) {
            vulkan_pipe_release(vk, g_backend.comp_pipe);
        }
        if (g_backend.comp_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(vk->device, g_backend.comp_layout, nullptr);
        }
    }
    g_backend.cull_pipe   = VK_NULL_HANDLE;
    g_backend.cull_layout = VK_NULL_HANDLE;
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
        (g_backend.dirty_slot_count == 0u && g_backend.dirty_slots_mask == 0ull)) {
        return;
    }

    uint64_t mask = g_backend.dirty_slots_mask;
    g_backend.dirty_slots_mask = 0ull;
    g_backend.dirty_slot_count = 0u;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_backend.comp_pipe);

    for (uint32_t slot = 0u; slot < k_gmpack_gpu_slot_cap; ++slot) {
        bool is_dirty = (g_backend.slot_dirty[slot] != 0u) ||
                        (slot < 64u && (mask & (1ull << slot)) != 0ull);
        if (!is_dirty) {
            continue;
        }
        g_backend.slot_dirty[slot] = 0u;
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

void backend_gpu_record_cull(VkCommandBuffer cmd, const TerrainCullParams *cull) {
    if (g_backend.gpu_hdr == nullptr || g_backend.indirect_hdr == nullptr || cull == nullptr) {
        return;
    }

    // Mirror exact frustum + elevation AABB culling into host-mapped indirect_hdr
    // so CPU vertex telemetry and fallback paths stay immediately synchronized.
    (void)backend_stream_cull_frustum(g_backend.gpu_hdr, cull, g_backend.indirect_hdr);

    if (cmd == VK_NULL_HANDLE || g_backend.cull_pipe == VK_NULL_HANDLE ||
        g_backend.bda_buf.address == 0ull || g_backend.indirect_buf.address == 0ull) {
        return;
    }

    CullPush push = {
        .dir_bda        = g_backend.bda_buf.address,
        .indirect_bda   = g_backend.indirect_buf.address,
        .qx             = cull->qx,
        .qy             = cull->qy,
        .qz             = cull->qz,
        .qw             = cull->qw,
        .cam_x          = cull->cam_x,
        .cam_y          = cull->cam_y,
        .cam_z          = cull->cam_z,
        .zoom           = cull->zoom,
        .elev_base_m    = cull->elev_base_m,
        .vertical_scale = cull->vertical_scale,
        .width          = cull->width,
        .height         = cull->height,
        .leaf_count     = g_backend.gpu_hdr->dir.active_leaf_count,
        .tile_verts     = cull->verts_per_tile,
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_backend.cull_pipe);
    vulkan_cmd_push(cmd, g_backend.cull_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof push, &push);
    vkCmdDispatch(cmd, 1u, 1u, 1u);

    VkMemoryBarrier2 barrier = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    };
    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1u,
        .pMemoryBarriers    = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}
