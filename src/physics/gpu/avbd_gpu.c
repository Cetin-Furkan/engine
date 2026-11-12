#include "physics/physics.h"
#include "vulkan/bda.h"
#include "vulkan/core.h"
#include "vulkan/pipe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ============================================================================
// Vulkan 1.4 BDA Fused Workgroup SLM AVBD Physics Runtime (avbd_gpu.c)
// - Lazily allocates 4 x 64 KiB = 256 KiB host-mapped BDA ring slab on first step
//   (0.00 MiB overhead on Pure Engine World 0 and Terrain World 8)
// - Dual-Architecture Profile Split:
//     * UMA_IGPU:        4 primal-dual iterations, zero-copy LLC BDA
//     * DISCRETE_DIRECT: 8 primal-dual iterations, ring-buffered BDA compute
// ============================================================================

alignas(uint32_t) static const unsigned char k_physics_avbd_spv[] = {
#embed "physics_avbd.comp.spv"
};

static_assert(sizeof k_physics_avbd_spv % 4u == 0u, "physics_avbd.comp.spv align");

enum : uint32_t {
    k_avbd_ring_slots   = 4u,
    k_max_frame_submits = 8u,
};

typedef struct PhysicsComputePush {
    uint64_t hdr_bda;
    uint32_t flags;
    uint32_t pad0;
} PhysicsComputePush;

static_assert(sizeof(PhysicsComputePush) == 16u, "PhysicsComputePush must be 16 bytes");

static VulkanContext   *s_vk                 = nullptr;
static VkPipelineLayout s_comp_layout        = VK_NULL_HANDLE;
static VkPipeline       s_comp_pipe          = VK_NULL_HANDLE;
static DaBuffer         s_ring_buf           = {0};
static bool             s_slab_ready         = false;

static bool             s_gpu_user_on        = true;
static uint32_t         s_exec_mode          = AVBD_EXEC_UMA_SLM_4;
static uint32_t         s_rec_iterations     = 4u;

static uint32_t         s_ring_slot          = 0u;
static uint32_t         s_slot_cursor        = 0u;
static uint64_t         s_pending_bda[k_max_frame_submits];
static uint32_t         s_pending_count      = 0u;
static uint32_t         s_pending_verts      = 0u;
static uint32_t         s_pending_cons       = 0u;
static uint32_t         s_pending_colors     = 0u;

static uint32_t         s_last_dispatches    = 0u;
static uint32_t         s_last_verts         = 0u;
static uint32_t         s_last_cons          = 0u;
static uint32_t         s_last_colors        = 0u;

static bool             s_logged_init        = false;
static bool             s_logged_slab        = false;

static uint32_t align_up_u32(uint32_t v, uint32_t a) {
    return (v + (a - 1u)) & ~(a - 1u);
}

static void parse_physics_env(void) {
    const char *env = getenv("ENGINE_PHYSICS");
    if (env == nullptr || env[0] == '\0') {
        s_gpu_user_on = true;
        return;
    }
    if (strcasecmp(env, "cpu") == 0 || strcasecmp(env, "0") == 0 ||
        strcasecmp(env, "off") == 0 || strcasecmp(env, "false") == 0) {
        s_gpu_user_on = false;
    } else {
        s_gpu_user_on = true;
    }
}

static void select_profile_params(const VulkanContext *vk) {
    if (vk != nullptr && vk->device_info.execution_profile != VULKAN_PROFILE_UMA_IGPU) {
        s_exec_mode      = AVBD_EXEC_DGPU_SLM_8;
        s_rec_iterations = 8u;
    } else {
        s_exec_mode      = AVBD_EXEC_UMA_SLM_4;
        s_rec_iterations = 4u;
    }
}

static bool ensure_physics_gpu_resources(void) {
    if (s_vk == nullptr || s_vk->device == VK_NULL_HANDLE) {
        return false;
    }
    if (!s_slab_ready) {
        const VkDeviceSize total_bytes = (VkDeviceSize)k_avbd_ring_slots * K_AVBD_SLICE_BYTES;
        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        if (da_buffer_create(s_vk, total_bytes, usage, true, &s_ring_buf) != 0 ||
            s_ring_buf.map == nullptr || s_ring_buf.address == 0ull) {
            da_buffer_destroy(s_vk, &s_ring_buf);
            return false;
        }
        memset(s_ring_buf.map, 0, (size_t)total_bytes);
        s_slab_ready = true;
    }

    if (s_comp_layout == VK_NULL_HANDLE) {
        VkPushConstantRange range = {
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset     = 0u,
            .size       = sizeof(PhysicsComputePush),
        };
        VkPipelineLayoutCreateInfo info = {
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1u,
            .pPushConstantRanges    = &range,
        };
        if (vkCreatePipelineLayout(s_vk->device, &info, nullptr, &s_comp_layout) != VK_SUCCESS) {
            s_comp_layout = VK_NULL_HANDLE;
            return false;
        }
    }

    if (s_comp_pipe == VK_NULL_HANDLE) {
        s_comp_pipe = vulkan_compute_pipeline(s_vk, k_physics_avbd_spv,
                                              sizeof k_physics_avbd_spv, s_comp_layout);
        if (s_comp_pipe == VK_NULL_HANDLE) {
            return false;
        }
    }

    if (!s_logged_slab) {
        printf("[Physics] 2026 AVBD Fused SLM GPU solver active (%u iters/step, %u KiB BDA ring, BDA=0x%llx)\n",
               s_rec_iterations,
               (unsigned)((k_avbd_ring_slots * K_AVBD_SLICE_BYTES) / 1024u),
               (unsigned long long)s_ring_buf.address);
        fflush(stdout);
        s_logged_slab = true;
    }
    return true;
}

int physics_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return -1;
    }
    if (!physics_self_check()) {
        fprintf(stderr, "[Physics Error] 2026 AVBD mathematical & LDL^T self-check failed!\n");
        return -1;
    }
    s_vk = vk;
    parse_physics_env();
    select_profile_params(vk);
    physics_reset();

    if (!s_logged_init) {
        printf("[Physics] Core AVBD kernel armed (LDL^T 3x3 + Graph Color SLM, GPU=%s, profile_iters=%u)\n",
               s_gpu_user_on ? "on" : "cpu-only",
               s_rec_iterations);
        fflush(stdout);
        s_logged_init = true;
    }
    return 0;
}

void physics_shutdown(VulkanContext *vk) {
    VulkanContext *ctx = vk != nullptr ? vk : s_vk;
    if (ctx != nullptr && ctx->device != VK_NULL_HANDLE) {
        if (s_comp_pipe != VK_NULL_HANDLE) {
            vulkan_pipe_release(ctx, s_comp_pipe);
        }
        if (s_comp_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(ctx->device, s_comp_layout, nullptr);
        }
        da_buffer_destroy(ctx, &s_ring_buf);
    }
    s_comp_pipe   = VK_NULL_HANDLE;
    s_comp_layout = VK_NULL_HANDLE;
    s_ring_buf    = (DaBuffer){0};
    s_slab_ready  = false;
    s_vk          = nullptr;
    physics_reset();
}

void physics_reset(void) {
    s_slot_cursor     = 0u;
    s_pending_count   = 0u;
    s_pending_verts   = 0u;
    s_pending_cons    = 0u;
    s_pending_colors  = 0u;
    s_last_dispatches = 0u;
    s_last_verts      = 0u;
    s_last_cons       = 0u;
    s_last_colors     = 0u;
}

uint32_t physics_recommended_iterations(void) {
    return s_rec_iterations > 0u ? s_rec_iterations : 4u;
}

void physics_submit_step(const AvbdVertex     *verts,
                         uint32_t              vert_count,
                         const AvbdConstraint *cons,
                         uint32_t              con_count,
                         float                 dt,
                         const float           gravity[3],
                         float                 damp_keep) {
    if (verts == nullptr || vert_count == 0u || vert_count > K_AVBD_MAX_VERTS || !(dt > 1.0e-6f)) {
        return;
    }
    const uint32_t safe_cons = (cons != nullptr)
        ? (con_count > K_AVBD_MAX_CONS ? K_AVBD_MAX_CONS : con_count)
        : 0u;

    uint32_t csr_row[K_AVBD_MAX_VERTS + 1u];
    uint32_t csr_col[K_AVBD_MAX_CSR_EDGES];
    uint32_t edge_cnt = avbd_build_csr(vert_count, cons, safe_cons,
                                       csr_row, csr_col, K_AVBD_MAX_CSR_EDGES);

    uint32_t color_verts[K_AVBD_MAX_VERTS];
    uint32_t color_start[K_AVBD_MAX_COLORS];
    uint32_t color_count[K_AVBD_MAX_COLORS];
    uint32_t num_colors = avbd_color_graph(verts, vert_count, cons, safe_cons,
                                           csr_row, csr_col,
                                           color_verts, color_start, color_count);

    // Record telemetry even when running in CPU-only mode
    s_pending_verts += vert_count;
    s_pending_cons  += safe_cons;
    if (num_colors > s_pending_colors) {
        s_pending_colors = num_colors;
    }
    s_last_verts  = s_pending_verts;
    s_last_cons   = s_pending_cons;
    s_last_colors = s_pending_colors;

    if (!s_gpu_user_on || s_pending_count >= k_max_frame_submits || !ensure_physics_gpu_resources()) {
        return;
    }

    const uint32_t off_hdr   = align_up_u32(s_slot_cursor, 64u);
    const uint32_t off_verts = align_up_u32(off_hdr + (uint32_t)sizeof(AvbdStepHeader), 64u);
    const uint32_t off_cons  = align_up_u32(off_verts + vert_count * (uint32_t)sizeof(AvbdVertex), 64u);
    const uint32_t off_row   = align_up_u32(off_cons + (safe_cons > 0u ? safe_cons : 1u) * (uint32_t)sizeof(AvbdConstraint), 64u);
    const uint32_t off_col   = align_up_u32(off_row + (vert_count + 1u) * (uint32_t)sizeof(uint32_t), 64u);
    const uint32_t off_cvert = align_up_u32(off_col + (edge_cnt > 0u ? edge_cnt : 1u) * (uint32_t)sizeof(uint32_t), 64u);
    const uint32_t end_bytes = align_up_u32(off_cvert + vert_count * (uint32_t)sizeof(uint32_t), 64u);

    if (end_bytes > K_AVBD_SLICE_BYTES) {
        return;
    }

    const size_t   slot_base_off = (size_t)s_ring_slot * K_AVBD_SLICE_BYTES;
    uint8_t       *slot_cpu      = (uint8_t *)s_ring_buf.map + slot_base_off;
    const uint64_t slot_bda      = s_ring_buf.address + (uint64_t)slot_base_off;

    memcpy(slot_cpu + off_verts, verts, (size_t)vert_count * sizeof(AvbdVertex));
    if (safe_cons > 0u) {
        memcpy(slot_cpu + off_cons, cons, (size_t)safe_cons * sizeof(AvbdConstraint));
    } else {
        memset(slot_cpu + off_cons, 0, sizeof(AvbdConstraint));
    }
    memcpy(slot_cpu + off_row, csr_row, (size_t)(vert_count + 1u) * sizeof(uint32_t));
    if (edge_cnt > 0u) {
        memcpy(slot_cpu + off_col, csr_col, (size_t)edge_cnt * sizeof(uint32_t));
    } else {
        memset(slot_cpu + off_col, 0, sizeof(uint32_t));
    }
    memcpy(slot_cpu + off_cvert, color_verts, (size_t)vert_count * sizeof(uint32_t));

    AvbdStepHeader *hdr = (AvbdStepHeader *)(slot_cpu + off_hdr);
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic               = K_AVBD_MAGIC;
    hdr->vert_count          = vert_count;
    hdr->con_count           = safe_cons;
    hdr->num_colors          = num_colors;
    hdr->iterations          = s_rec_iterations;
    hdr->csr_edge_count      = edge_cnt;
    hdr->dt                  = dt;
    hdr->warm_decay          = 0.85f;
    hdr->gravity[0]          = gravity != nullptr ? gravity[0] : 0.0f;
    hdr->gravity[1]          = gravity != nullptr ? gravity[1] : 0.0f;
    hdr->gravity[2]          = gravity != nullptr ? gravity[2] : 0.0f;
    hdr->damp_keep           = damp_keep;
    hdr->verts_bda           = slot_bda + off_verts;
    hdr->cons_bda            = slot_bda + off_cons;
    hdr->csr_row_bda         = slot_bda + off_row;
    hdr->csr_col_bda         = slot_bda + off_col;
    hdr->color_verts_bda     = slot_bda + off_cvert;
    hdr->stats_bda           = slot_bda + off_hdr + offsetof(AvbdStepHeader, gpu_completed_iters);
    memcpy(hdr->color_start, color_start, sizeof color_start);
    memcpy(hdr->color_count, color_count, sizeof color_count);

    s_pending_bda[s_pending_count++] = slot_bda + off_hdr;
    s_slot_cursor = end_bytes;
}

void physics_record_compute(VkCommandBuffer cmd) {
    if (s_pending_count == 0u) {
        return;
    }
    if (cmd == VK_NULL_HANDLE || !s_gpu_user_on || s_comp_pipe == VK_NULL_HANDLE ||
        s_comp_layout == VK_NULL_HANDLE) {
        s_pending_count  = 0u;
        s_pending_verts  = 0u;
        s_pending_cons   = 0u;
        s_pending_colors = 0u;
        s_slot_cursor    = 0u;
        return;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_comp_pipe);
    for (uint32_t i = 0u; i < s_pending_count; ++i) {
        PhysicsComputePush push = {
            .hdr_bda = s_pending_bda[i],
            .flags   = s_exec_mode,
            .pad0    = 0u,
        };
        vulkan_cmd_push(cmd, s_comp_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof push, &push);
        vkCmdDispatch(cmd, 1u, 1u, 1u);
    }

    VkMemoryBarrier2 bar = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    };
    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1u,
        .pMemoryBarriers    = &bar,
    };
    vkCmdPipelineBarrier2(cmd, &dep);

    s_last_dispatches = s_pending_count;
    s_last_verts      = s_pending_verts;
    s_last_cons       = s_pending_cons;
    s_last_colors     = s_pending_colors;

    s_ring_slot      = (s_ring_slot + 1u) % k_avbd_ring_slots;
    s_slot_cursor    = 0u;
    s_pending_count  = 0u;
    s_pending_verts  = 0u;
    s_pending_cons   = 0u;
    s_pending_colors = 0u;
}

bool physics_gpu_active(void) {
    return s_gpu_user_on;
}

bool physics_toggle_gpu(void) {
    s_gpu_user_on = !s_gpu_user_on;
    printf("[Physics] 2026 AVBD Fused Workgroup SLM Compute: %s (iters=%u)\n",
           s_gpu_user_on ? "GPU+CPU" : "CPU-ONLY",
           s_rec_iterations);
    fflush(stdout);
    return true;
}

void physics_get_stats(PhysicsBenchStats *out_stats) {
    if (out_stats == nullptr) {
        return;
    }
    *out_stats = (PhysicsBenchStats){
        .gpu_enabled        = s_gpu_user_on,
        .exec_mode          = s_gpu_user_on ? s_exec_mode : AVBD_EXEC_CPU_ONLY,
        .iterations         = s_rec_iterations,
        .active_verts       = s_last_verts,
        .active_constraints = s_last_cons,
        .num_colors         = s_last_colors,
        .dispatches_frame   = s_last_dispatches,
        .bda_bytes          = s_slab_ready ? (uint64_t)s_ring_buf.bytes : 0ull,
    };
}
