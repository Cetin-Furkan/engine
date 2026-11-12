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
// - Single-owner GPU pose architecture (1-frame timeline latency, timeout = 0)
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

typedef struct AvbdStepEntry {
    uint32_t channel_id;
    uint32_t epoch;
    uint32_t off_hdr;
    uint32_t off_verts;
    uint32_t off_cons;
    uint32_t vert_count;
    uint32_t con_count;
    float    dt;
} AvbdStepEntry;

typedef struct AvbdRingSlot {
    uint64_t      signal_point;
    uint32_t      entry_count;
    uint32_t      read_cursor;
    uint32_t      byte_cursor;
    uint32_t      total_verts;
    uint32_t      total_cons;
    uint32_t      max_colors;
    bool          recorded;
    bool          submitted;
    bool          abandoned;
    AvbdStepEntry entries[k_max_frame_submits];
} AvbdRingSlot;

static VulkanContext   *s_vk                 = nullptr;
static VkPipelineLayout s_comp_layout        = VK_NULL_HANDLE;
static VkPipeline       s_comp_pipe          = VK_NULL_HANDLE;
static VkSemaphore      s_acquire_sem        = VK_NULL_HANDLE;
static DaBuffer         s_ring_buf           = {0};
static bool             s_slab_ready         = false;

static bool             s_gpu_user_on        = true;
static uint32_t         s_exec_mode          = AVBD_EXEC_UMA_SLM_4;
static uint32_t         s_rec_iterations     = 4u;

static AvbdRingSlot     s_slots[k_avbd_ring_slots];
static uint32_t         s_write_slot         = 0u;
static uint32_t         s_retire_head        = 0u;
static int              s_open_record_slot   = -1;

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
    s_acquire_sem = VK_NULL_HANDLE;
    parse_physics_env();
    select_profile_params(vk);
    physics_reset();

    if (!s_logged_init) {
        printf("[Physics] Core AVBD kernel armed (LDL^T 3x3 + Graph Color SLM, owner=%s, profile_iters=%u)\n",
               s_gpu_user_on ? "gpu" : "cpu",
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
    s_acquire_sem = VK_NULL_HANDLE;
    s_ring_buf    = (DaBuffer){0};
    s_slab_ready  = false;
    s_vk          = nullptr;
    physics_reset();
}

void physics_reset(void) {
    if (s_vk == nullptr || s_vk->device == VK_NULL_HANDLE) {
        memset(s_slots, 0, sizeof s_slots);
        s_write_slot  = 0u;
        s_retire_head = 0u;
    } else {
        for (uint32_t i = 0u; i < k_avbd_ring_slots; ++i) {
            if (s_slots[i].submitted || s_slots[i].recorded) {
                s_slots[i].abandoned = true;
            } else {
                s_slots[i] = (AvbdRingSlot){0};
            }
        }
    }
    s_open_record_slot = -1;
    s_last_dispatches  = 0u;
    s_last_verts       = 0u;
    s_last_cons        = 0u;
    s_last_colors      = 0u;
}

uint32_t physics_recommended_iterations(void) {
    return s_rec_iterations > 0u ? s_rec_iterations : 4u;
}

bool physics_gpu_pose_owner(void) {
    return s_gpu_user_on && s_vk != nullptr && s_vk->device != VK_NULL_HANDLE &&
           ensure_physics_gpu_resources();
}

static int find_staging_slot(void) {
    for (uint32_t n = 0u; n < k_avbd_ring_slots; ++n) {
        uint32_t idx = (s_write_slot + n) % k_avbd_ring_slots;
        const AvbdRingSlot *sl = &s_slots[idx];
        if (!sl->recorded && !sl->submitted) {
            s_write_slot = idx;
            return (int)idx;
        }
    }
    return -1;
}

bool physics_submit_step(uint32_t              channel_id,
                         uint32_t              epoch,
                         const AvbdVertex     *verts,
                         uint32_t              vert_count,
                         const AvbdConstraint *cons,
                         uint32_t              con_count,
                         float                 dt,
                         const float           gravity[3],
                         float                 damp_keep) {
    if (verts == nullptr || vert_count == 0u || vert_count > K_AVBD_MAX_VERTS || !(dt > 1.0e-6f)) {
        return false;
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

    s_last_verts  = vert_count;
    s_last_cons   = safe_cons;
    s_last_colors = num_colors;

    if (!physics_gpu_pose_owner()) {
        return false;
    }

    int slot_idx = find_staging_slot();
    if (slot_idx < 0) {
        return false;
    }
    AvbdRingSlot *slot = &s_slots[slot_idx];
    if (slot->entry_count >= k_max_frame_submits) {
        return false;
    }

    const uint32_t off_hdr   = align_up_u32(slot->byte_cursor, 64u);
    const uint32_t off_verts = align_up_u32(off_hdr + (uint32_t)sizeof(AvbdStepHeader), 64u);
    const uint32_t off_cons  = align_up_u32(off_verts + vert_count * (uint32_t)sizeof(AvbdVertex), 64u);
    const uint32_t off_row   = align_up_u32(off_cons + (safe_cons > 0u ? safe_cons : 1u) * (uint32_t)sizeof(AvbdConstraint), 64u);
    const uint32_t off_col   = align_up_u32(off_row + (vert_count + 1u) * (uint32_t)sizeof(uint32_t), 64u);
    const uint32_t off_cvert = align_up_u32(off_col + (edge_cnt > 0u ? edge_cnt : 1u) * (uint32_t)sizeof(uint32_t), 64u);
    const uint32_t end_bytes = align_up_u32(off_cvert + vert_count * (uint32_t)sizeof(uint32_t), 64u);

    if (end_bytes > K_AVBD_SLICE_BYTES) {
        return false;
    }

    const size_t   slot_base_off = (size_t)slot_idx * K_AVBD_SLICE_BYTES;
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
    memcpy(slot_cpu + off_cvert, color_verts, (size_t)vert_count * (uint32_t)sizeof(uint32_t));

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

    slot->entries[slot->entry_count++] = (AvbdStepEntry){
        .channel_id = channel_id,
        .epoch      = epoch,
        .off_hdr    = off_hdr,
        .off_verts  = off_verts,
        .off_cons   = off_cons,
        .vert_count = vert_count,
        .con_count  = safe_cons,
        .dt         = dt,
    };
    slot->byte_cursor  = end_bytes;
    slot->total_verts += vert_count;
    slot->total_cons  += safe_cons;
    if (num_colors > slot->max_colors) {
        slot->max_colors = num_colors;
    }
    return true;
}

void physics_record_compute(VkCommandBuffer cmd) {
    int target = -1;
    for (uint32_t n = 0u; n < k_avbd_ring_slots; ++n) {
        uint32_t idx = (s_retire_head + n) % k_avbd_ring_slots;
        if (s_slots[idx].entry_count > 0u && !s_slots[idx].recorded && !s_slots[idx].submitted) {
            target = (int)idx;
            break;
        }
    }
    if (target < 0) {
        return;
    }

    AvbdRingSlot *slot = &s_slots[target];
    if (cmd == VK_NULL_HANDLE || !s_gpu_user_on || s_comp_pipe == VK_NULL_HANDLE ||
        s_comp_layout == VK_NULL_HANDLE) {
        *slot = (AvbdRingSlot){0};
        return;
    }

    const uint64_t slot_bda = s_ring_buf.address + (uint64_t)target * K_AVBD_SLICE_BYTES;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_comp_pipe);
    for (uint32_t i = 0u; i < slot->entry_count; ++i) {
        PhysicsComputePush push = {
            .hdr_bda = slot_bda + slot->entries[i].off_hdr,
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
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                         VK_ACCESS_2_HOST_READ_BIT,
    };
    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1u,
        .pMemoryBarriers    = &bar,
    };
    vkCmdPipelineBarrier2(cmd, &dep);

    s_last_dispatches  = slot->entry_count;
    s_last_verts       = slot->total_verts;
    s_last_cons        = slot->total_cons;
    s_last_colors      = slot->max_colors;

    slot->recorded     = true;
    slot->read_cursor  = 0u;
    s_open_record_slot = target;
    s_write_slot       = ((uint32_t)target + 1u) % k_avbd_ring_slots;
}

void physics_claim_frame(VkSemaphore acquire, uint64_t signal_point) {
    if (acquire != VK_NULL_HANDLE) {
        s_acquire_sem = acquire;
    }
    if (s_open_record_slot >= 0 && (uint32_t)s_open_record_slot < k_avbd_ring_slots) {
        AvbdRingSlot *slot = &s_slots[s_open_record_slot];
        slot->signal_point = signal_point;
        slot->submitted    = true;
        s_open_record_slot = -1;
    }
}

void physics_abort_frame(uint64_t signal_point) {
    if (s_open_record_slot >= 0 && (uint32_t)s_open_record_slot < k_avbd_ring_slots) {
        AvbdRingSlot *slot = &s_slots[s_open_record_slot];
        slot->recorded     = false;
        slot->submitted    = false;
        slot->signal_point = 0u;
        s_open_record_slot = -1;
    }
    if (signal_point == 0u) {
        return;
    }
    for (uint32_t i = 0u; i < k_avbd_ring_slots; ++i) {
        if (s_slots[i].submitted && s_slots[i].signal_point == signal_point) {
            s_slots[i].recorded     = false;
            s_slots[i].submitted    = false;
            s_slots[i].signal_point = 0u;
        }
    }
}

bool physics_poll_solved_step(PhysicsSolvedStep *out_step) {
    if (out_step == nullptr || !s_slab_ready || s_ring_buf.map == nullptr ||
        s_vk == nullptr || s_vk->device == VK_NULL_HANDLE || s_acquire_sem == VK_NULL_HANDLE) {
        return false;
    }

    uint64_t done = 0u;
    if (vkGetSemaphoreCounterValue(s_vk->device, s_acquire_sem, &done) != VK_SUCCESS) {
        return false;
    }

    for (uint32_t scanned = 0u; scanned < k_avbd_ring_slots; ++scanned) {
        uint32_t idx = (s_retire_head + scanned) % k_avbd_ring_slots;
        AvbdRingSlot *slot = &s_slots[idx];
        if (slot->entry_count == 0u) {
            continue;
        }
        if (!slot->submitted || slot->signal_point == 0u || done < slot->signal_point) {
            // Preserve strict chronological retirement order across ring slots
            return false;
        }
        if (slot->abandoned) {
            *slot = (AvbdRingSlot){0};
            s_retire_head = (idx + 1u) % k_avbd_ring_slots;
            continue;
        }
        if (slot->read_cursor < slot->entry_count) {
            const AvbdStepEntry *e = &slot->entries[slot->read_cursor++];
            const uint8_t *slot_cpu = (const uint8_t *)s_ring_buf.map + (size_t)idx * K_AVBD_SLICE_BYTES;
            *out_step = (PhysicsSolvedStep){
                .channel_id = e->channel_id,
                .epoch      = e->epoch,
                .vert_count = e->vert_count,
                .con_count  = e->con_count,
                .dt         = e->dt,
                .verts      = (const AvbdVertex *)(slot_cpu + e->off_verts),
                .cons       = e->con_count > 0u ? (const AvbdConstraint *)(slot_cpu + e->off_cons) : nullptr,
            };
            if (slot->read_cursor >= slot->entry_count) {
                *slot = (AvbdRingSlot){0};
                s_retire_head = (idx + 1u) % k_avbd_ring_slots;
            }
            return true;
        }
        *slot = (AvbdRingSlot){0};
        s_retire_head = (idx + 1u) % k_avbd_ring_slots;
    }
    return false;
}

bool physics_channel_busy(uint32_t channel_id, uint32_t epoch) {
    for (uint32_t s = 0u; s < k_avbd_ring_slots; ++s) {
        const AvbdRingSlot *slot = &s_slots[s];
        if (slot->abandoned) {
            continue;
        }
        for (uint32_t i = slot->read_cursor; i < slot->entry_count; ++i) {
            if (slot->entries[i].channel_id == channel_id &&
                slot->entries[i].epoch == epoch) {
                return true;
            }
        }
    }
    return false;
}

bool physics_has_unrecorded_steps(void) {
    for (uint32_t s = 0u; s < k_avbd_ring_slots; ++s) {
        if (!s_slots[s].abandoned && s_slots[s].entry_count > 0u &&
            !s_slots[s].recorded && !s_slots[s].submitted) {
            return true;
        }
    }
    return false;
}

bool physics_any_inflight(void) {
    for (uint32_t s = 0u; s < k_avbd_ring_slots; ++s) {
        if (!s_slots[s].abandoned && s_slots[s].entry_count > s_slots[s].read_cursor) {
            return true;
        }
    }
    return false;
}

bool physics_gpu_active(void) {
    return s_gpu_user_on;
}

bool physics_toggle_gpu(void) {
    s_gpu_user_on = !s_gpu_user_on;
    printf("[Physics] 2026 AVBD Single Pose Owner: %s (iters=%u)\n",
           s_gpu_user_on ? "GPU BDA SLM" : "CPU-ONLY",
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
