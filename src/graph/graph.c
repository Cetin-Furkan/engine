#include "graph/graph.h"
#include "vulkan/core.h"
#include <stdio.h>
#include <string.h>

// Double-buffered hardware timestamp query slots: 2 slots x 24 passes x 2 stamps = 96 queries.
enum : uint32_t {
    k_rg_frame_slots   = 2u,
    k_rg_stamps_slot   = RG_MAX_PASSES * 2u,
    k_rg_total_queries = k_rg_frame_slots * k_rg_stamps_slot,
};

typedef struct RgSlotRecord {
    char       names[RG_MAX_PASSES][24];
    RgPassType types[RG_MAX_PASSES];
    uint32_t   pass_count;
    bool       armed;
} RgSlotRecord;

static VulkanContext               *s_vk             = nullptr;
static VkQueryPool                  s_query_pool     = VK_NULL_HANDLE;
static float                        s_ts_period_ns   = 0.0f;
static uint32_t                     s_ts_valid_bits  = 0u;
static bool                         s_ts_host_reset  = false;
static uint32_t                     s_slot_reset_msk = 0u;
static RgSlotRecord                 s_slot_rec[k_rg_frame_slots];
static RgProfileSnapshot            s_profile        = {0};
static PFN_vkCmdBeginDebugUtilsLabelEXT s_cmd_begin_label = nullptr;
static PFN_vkCmdEndDebugUtilsLabelEXT   s_cmd_end_label   = nullptr;

typedef struct UsageInfo {
    VkPipelineStageFlags2 stage;
    VkAccessFlags2        access;
    VkImageLayout         layout;
    bool                  is_write;
    bool                  discard_contents;
} UsageInfo;

static UsageInfo usage_info(RgUsage u, VkImageAspectFlags aspect, bool unified_general) {
    const bool is_depth = (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0u;
    switch (u) {
    case RG_USE_COMPUTE_READ:
        return (UsageInfo){
            .stage    = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access   = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .layout   = VK_IMAGE_LAYOUT_GENERAL,
            .is_write = false,
        };
    case RG_USE_COMPUTE_WRITE:
        return (UsageInfo){
            .stage    = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access   = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .layout   = VK_IMAGE_LAYOUT_GENERAL,
            .is_write = true,
        };
    case RG_USE_COMPUTE_RW:
        return (UsageInfo){
            .stage    = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access   = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .layout   = VK_IMAGE_LAYOUT_GENERAL,
            .is_write = true,
        };
    case RG_USE_COMPUTE_SAMPLED:
        return (UsageInfo){
            .stage    = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access   = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout   = unified_general ? VK_IMAGE_LAYOUT_GENERAL
                                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .is_write = false,
        };
    case RG_USE_INDIRECT_READ:
        return (UsageInfo){
            .stage    = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            .access   = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
            .layout   = VK_IMAGE_LAYOUT_UNDEFINED,
            .is_write = false,
        };
    case RG_USE_COLOR_WRITE:
        return (UsageInfo){
            .stage            = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .access           = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .layout           = unified_general ? VK_IMAGE_LAYOUT_GENERAL
                                                : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .is_write         = true,
            .discard_contents = true,
        };
    case RG_USE_COLOR_RW:
        return (UsageInfo){
            .stage    = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .access   = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .layout   = unified_general ? VK_IMAGE_LAYOUT_GENERAL
                                        : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .is_write = true,
        };
    case RG_USE_DEPTH_WRITE:
        return (UsageInfo){
            .stage            = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            .access           = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .layout           = unified_general ? VK_IMAGE_LAYOUT_GENERAL
                                                : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .is_write         = true,
            .discard_contents = true,
        };
    case RG_USE_DEPTH_READ:
        return (UsageInfo){
            .stage    = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            .access   = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
            .layout   = unified_general ? VK_IMAGE_LAYOUT_GENERAL
                                        : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
            .is_write = false,
        };
    case RG_USE_FRAG_SAMPLED:
        return (UsageInfo){
            .stage    = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .access   = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout   = unified_general
                            ? VK_IMAGE_LAYOUT_GENERAL
                            : (is_depth ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            .is_write = false,
        };
    case RG_USE_GRAPHICS_READ:
        return (UsageInfo){
            .stage    = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .access   = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .layout   = VK_IMAGE_LAYOUT_UNDEFINED,
            .is_write = false,
        };
    case RG_USE_PRESENT:
        return (UsageInfo){
            .stage    = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .access   = VK_ACCESS_2_NONE,
            .layout   = VK_IMAGE_LAYOUT_GENERAL,
            .is_write = false,
        };
    case RG_USE_NONE:
    default:
        return (UsageInfo){
            .stage  = VK_PIPELINE_STAGE_2_NONE,
            .access = VK_ACCESS_2_NONE,
            .layout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
    }
}

static uint64_t rg_delta_ticks(uint64_t later, uint64_t earlier, uint32_t valid_bits) {
    if (valid_bits == 0u) {
        return 0u;
    }
    if (valid_bits >= 64u) {
        return later >= earlier ? (later - earlier) : 0u;
    }
    uint64_t mask = (1ull << valid_bits) - 1ull;
    uint64_t diff = (later - earlier) & mask;
    return (diff > (mask >> 1u)) ? 0u : diff;
}

static void rg_harvest_slot(uint32_t slot) {
    if (slot >= k_rg_frame_slots || !s_slot_rec[slot].armed ||
        s_vk == nullptr || s_vk->device == VK_NULL_HANDLE ||
        s_query_pool == VK_NULL_HANDLE || s_ts_period_ns <= 0.0f) {
        return;
    }
    const uint32_t count = s_slot_rec[slot].pass_count;
    if (count == 0u || count > RG_MAX_PASSES) {
        s_slot_rec[slot].armed = false;
        return;
    }
    const uint32_t first_q = slot * k_rg_stamps_slot;
    const uint32_t q_count = count * 2u;
    uint64_t raw[k_rg_stamps_slot * 2u] = {0};
    VkResult res = vkGetQueryPoolResults(
        s_vk->device, s_query_pool, first_q, q_count,
        q_count * 2u * sizeof(uint64_t), raw, 2u * sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (res == VK_NOT_READY) {
        return;
    }
    for (uint32_t i = 0u; i < q_count; ++i) {
        if (raw[i * 2u + 1u] == 0u) {
            return;
        }
    }
    s_slot_rec[slot].armed = false;
    if (s_ts_host_reset) {
        vkResetQueryPool(s_vk->device, s_query_pool, first_q, k_rg_stamps_slot);
        s_slot_reset_msk |= (1u << slot);
    }
    if (res != VK_SUCCESS) {
        return;
    }

    RgProfileSnapshot snap = {0};
    snap.pass_count = count;
    snap.samples    = s_profile.samples + 1u;
    uint64_t total_ns = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        uint64_t t0 = raw[i * 4u + 0u];
        uint64_t t1 = raw[i * 4u + 2u];
        uint64_t dt = rg_delta_ticks(t1, t0, s_ts_valid_bits);
        uint64_t ns = (uint64_t)((double)dt * (double)s_ts_period_ns);
        total_ns += ns;
        snap.passes[i].type   = s_slot_rec[slot].types[i];
        snap.passes[i].gpu_us = (uint32_t)((ns + 500ull) / 1000ull);
        memcpy(snap.passes[i].name, s_slot_rec[slot].names[i], sizeof(snap.passes[i].name));
    }
    snap.total_us = (uint32_t)((total_ns + 500ull) / 1000ull);
    s_profile = snap;
}

void rg_init(VulkanContext *vk) {
    rg_shutdown(vk);
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return;
    }
    s_vk = vk;
    if (vk->instance != VK_NULL_HANDLE) {
        s_cmd_begin_label = (PFN_vkCmdBeginDebugUtilsLabelEXT)vkGetInstanceProcAddr(
            vk->instance, "vkCmdBeginDebugUtilsLabelEXT");
        s_cmd_end_label = (PFN_vkCmdEndDebugUtilsLabelEXT)vkGetInstanceProcAddr(
            vk->instance, "vkCmdEndDebugUtilsLabelEXT");
    }
    const float period = vk->device_info.limits.timestampPeriod;
    const uint32_t bits = vk->device_info.graphics_ts_valid_bits;
    if (period <= 0.0f || bits == 0u) {
        return;
    }
    VkQueryPoolCreateInfo info = {
        .sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType  = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = k_rg_total_queries,
    };
    if (vkCreateQueryPool(vk->device, &info, nullptr, &s_query_pool) != VK_SUCCESS) {
        s_query_pool = VK_NULL_HANDLE;
        return;
    }
    vulkan_name_object(vk, VK_OBJECT_TYPE_QUERY_POOL, (uint64_t)s_query_pool, "RenderGraph:Timestamps");
    s_ts_period_ns  = period;
    s_ts_valid_bits = bits;
    s_ts_host_reset = vk->device_info.supports_host_query_reset;
    if (s_ts_host_reset) {
        vkResetQueryPool(vk->device, s_query_pool, 0u, k_rg_total_queries);
        s_slot_reset_msk = (1u << k_rg_frame_slots) - 1u;
    }
}

void rg_shutdown(VulkanContext *vk) {
    if (vk != nullptr && vk->device != VK_NULL_HANDLE && s_query_pool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(vk->device, s_query_pool, nullptr);
    }
    s_query_pool      = VK_NULL_HANDLE;
    s_vk              = nullptr;
    s_ts_period_ns    = 0.0f;
    s_ts_valid_bits   = 0u;
    s_ts_host_reset   = false;
    s_slot_reset_msk  = 0u;
    s_cmd_begin_label = nullptr;
    s_cmd_end_label   = nullptr;
    memset(s_slot_rec, 0, sizeof s_slot_rec);
    s_profile = (RgProfileSnapshot){0};
}

void rg_begin_frame(RgGraph *g, uint32_t frame_slot) {
    if (g == nullptr) {
        return;
    }
    g->res_count       = 0u;
    g->pass_count      = 0u;
    g->executed_count  = 0u;
    g->recorded_ts     = 0u;
    g->frame_slot      = frame_slot % k_rg_frame_slots;
    g->slot_reset_done = false;
    rg_harvest_slot(g->frame_slot);
}

RgResHandle rg_import_image(RgGraph *g,
                            const char *name,
                            VkImage image,
                            VkImageAspectFlags aspect,
                            uint32_t mip_base,
                            uint32_t mip_count,
                            VkImageLayout initial_layout,
                            VkImageLayout *persistent_layout,
                            bool unified_general) {
    if (g == nullptr || image == VK_NULL_HANDLE || g->res_count >= RG_MAX_RESOURCES) {
        return RG_INVALID_RES;
    }
    RgResHandle h = g->res_count++;
    g->resources[h] = (RgResourceState){
        .kind            = RG_RES_IMAGE,
        .name            = name != nullptr ? name : "Image",
        .image           = image,
        .aspect          = aspect != 0u ? aspect : VK_IMAGE_ASPECT_COLOR_BIT,
        .mip_base        = mip_base,
        .mip_count       = mip_count != 0u ? mip_count : 1u,
        .layout          = persistent_layout != nullptr ? *persistent_layout : initial_layout,
        .external_layout = persistent_layout,
        .unified_general = unified_general,
    };
    return h;
}

RgResHandle rg_import_buffer(RgGraph *g,
                             const char *name,
                             VkPipelineStageFlags2 initial_write_stage,
                             VkAccessFlags2 initial_write_access) {
    if (g == nullptr || g->res_count >= RG_MAX_RESOURCES) {
        return RG_INVALID_RES;
    }
    RgResHandle h = g->res_count++;
    g->resources[h] = (RgResourceState){
        .kind           = RG_RES_BUFFER,
        .name           = name != nullptr ? name : "Buffer",
        .write_stage    = initial_write_stage,
        .write_access   = initial_write_access,
        .visible_stages = VK_PIPELINE_STAGE_2_NONE,
        .dirty_write    = (initial_write_stage != VK_PIPELINE_STAGE_2_NONE),
    };
    return h;
}

void rg_set_prior_access(RgGraph *g,
                         RgResHandle res,
                         VkPipelineStageFlags2 write_stage,
                         VkAccessFlags2 write_access,
                         VkPipelineStageFlags2 read_stages) {
    if (g == nullptr || res >= g->res_count) {
        return;
    }
    RgResourceState *r = &g->resources[res];
    r->write_stage    = write_stage;
    r->write_access   = write_access;
    r->dirty_write    = (write_stage != VK_PIPELINE_STAGE_2_NONE);
    r->read_stages    = read_stages;
    r->has_readers    = (read_stages != VK_PIPELINE_STAGE_2_NONE);
    r->visible_stages = VK_PIPELINE_STAGE_2_NONE;
}

void rg_add_pass(RgGraph *g,
                 const char *name,
                 RgPassType type,
                 const RgAccess *accesses,
                 uint32_t access_count,
                 RgPassFn execute,
                 void *user) {
    if (g == nullptr || execute == nullptr || g->pass_count >= RG_MAX_PASSES) {
        return;
    }
    uint32_t n = access_count <= RG_MAX_PASS_ACCESS ? access_count : RG_MAX_PASS_ACCESS;
    RgPassDesc *p = &g->passes[g->pass_count++];
    *p = (RgPassDesc){
        .name         = name != nullptr ? name : "Pass",
        .type         = type,
        .execute      = execute,
        .user         = user,
        .access_count = n,
        .enabled      = true,
    };
    if (accesses != nullptr && n > 0u) {
        memcpy(p->accesses, accesses, (size_t)n * sizeof(RgAccess));
    }
}

void rg_plan_pass_barriers(RgGraph *g, uint32_t pass_index, RgBarrierBatch *out_batch) {
    if (out_batch == nullptr) {
        return;
    }
    *out_batch = (RgBarrierBatch){0};
    out_batch->mem_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;

    if (g == nullptr || pass_index >= g->pass_count) {
        return;
    }
    const RgPassDesc *pass = &g->passes[pass_index];
    if (!pass->enabled) {
        return;
    }

    for (uint32_t i = 0u; i < pass->access_count; ++i) {
        RgResHandle h = pass->accesses[i].res;
        RgUsage     u = pass->accesses[i].usage;
        if (h >= g->res_count || u == RG_USE_NONE) {
            continue;
        }
        RgResourceState *r = &g->resources[h];
        UsageInfo req = usage_info(u, r->aspect, r->unified_general);

        if (r->kind == RG_RES_IMAGE) {
            const bool layout_change = (r->layout != req.layout);
            const bool need_vis      = (!req.is_write) && (r->write_stage != VK_PIPELINE_STAGE_2_NONE) &&
                                       ((r->visible_stages & req.stage) != req.stage);
            const bool need_raw      = (r->dirty_write || need_vis) && !req.is_write;
            const bool need_waw      = r->dirty_write && req.is_write;
            const bool need_war      = r->has_readers && req.is_write;

            VkPipelineStageFlags2 dst_stage  = req.stage;
            VkAccessFlags2        dst_access = req.access;
            if (!req.is_write && (layout_change || need_raw)) {
                // Look-ahead RAR visibility merge: union all consecutive compatible reader stages
                // until the next write or layout change so downstream read-only passes emit 0 barriers.
                for (uint32_t k = pass_index + 1u; k < g->pass_count; ++k) {
                    const RgPassDesc *fp = &g->passes[k];
                    if (!fp->enabled) {
                        continue;
                    }
                    bool stop_scan = false;
                    for (uint32_t a = 0u; a < fp->access_count; ++a) {
                        if (fp->accesses[a].res != h || fp->accesses[a].usage == RG_USE_NONE) {
                            continue;
                        }
                        UsageInfo freq = usage_info(fp->accesses[a].usage, r->aspect, r->unified_general);
                        if (freq.is_write || freq.layout != req.layout) {
                            stop_scan = true;
                            break;
                        }
                        dst_stage  |= freq.stage;
                        dst_access |= freq.access;
                    }
                    if (stop_scan) {
                        break;
                    }
                }
            }

            if (layout_change || need_raw || need_waw || need_war) {
                VkPipelineStageFlags2 src_stage  = VK_PIPELINE_STAGE_2_NONE;
                VkAccessFlags2        src_access = VK_ACCESS_2_NONE;
                if (req.is_write) {
                    if (r->dirty_write) {
                        src_stage  |= r->write_stage;
                        src_access |= r->write_access;
                    }
                    if (r->has_readers) {
                        src_stage  |= r->read_stages; // WAR needs execution barrier only
                    }
                } else {
                    if (r->dirty_write || need_vis) {
                        src_stage  = r->write_stage;
                        src_access = r->write_access;
                    } else if (r->has_readers) {
                        src_stage  = r->read_stages;
                        src_access = VK_ACCESS_2_NONE;
                    }
                }

                if (out_batch->image_count < RG_MAX_BARRIERS) {
                    out_batch->image_barriers[out_batch->image_count++] = (VkImageMemoryBarrier2){
                        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                        .srcStageMask        = src_stage,
                        .srcAccessMask       = src_access,
                        .dstStageMask        = dst_stage,
                        .dstAccessMask       = dst_access,
                        .oldLayout           = req.discard_contents ? VK_IMAGE_LAYOUT_UNDEFINED : r->layout,
                        .newLayout           = req.layout,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image               = r->image,
                        .subresourceRange    = {
                            .aspectMask     = r->aspect,
                            .baseMipLevel   = r->mip_base,
                            .levelCount     = r->mip_count,
                            .baseArrayLayer = 0u,
                            .layerCount     = 1u,
                        },
                    };
                }
            }

            r->layout = req.layout;
            if (req.is_write) {
                r->write_stage    = req.stage;
                r->write_access   = req.access;
                r->dirty_write    = true;
                r->has_readers    = false;
                r->read_stages    = VK_PIPELINE_STAGE_2_NONE;
                r->visible_stages = VK_PIPELINE_STAGE_2_NONE;
            } else {
                r->dirty_write     = false;
                r->visible_stages |= dst_stage;
                r->has_readers     = true;
                r->read_stages    |= req.stage;
            }
        } else {
            // BDA / buffer hazard tracking (coalesced into a single global VkMemoryBarrier2 per pass)
            const bool need_vis = (!req.is_write) && (r->write_stage != VK_PIPELINE_STAGE_2_NONE) &&
                                  ((r->visible_stages & req.stage) != req.stage);
            const bool need_raw = (r->dirty_write || need_vis) && !req.is_write;
            const bool need_waw = r->dirty_write && req.is_write;
            const bool need_war = r->has_readers && req.is_write;

            VkPipelineStageFlags2 dst_stage  = req.stage;
            VkAccessFlags2        dst_access = req.access;
            if (!req.is_write && need_raw) {
                for (uint32_t k = pass_index + 1u; k < g->pass_count; ++k) {
                    const RgPassDesc *fp = &g->passes[k];
                    if (!fp->enabled) {
                        continue;
                    }
                    bool stop_scan = false;
                    for (uint32_t a = 0u; a < fp->access_count; ++a) {
                        if (fp->accesses[a].res != h || fp->accesses[a].usage == RG_USE_NONE) {
                            continue;
                        }
                        UsageInfo freq = usage_info(fp->accesses[a].usage, r->aspect, r->unified_general);
                        if (freq.is_write) {
                            stop_scan = true;
                            break;
                        }
                        dst_stage  |= freq.stage;
                        dst_access |= freq.access;
                    }
                    if (stop_scan) {
                        break;
                    }
                }
            }

            if (need_raw || need_waw || need_war) {
                VkPipelineStageFlags2 src_stage  = VK_PIPELINE_STAGE_2_NONE;
                VkAccessFlags2        src_access = VK_ACCESS_2_NONE;
                if (req.is_write) {
                    if (r->dirty_write) {
                        src_stage  |= r->write_stage;
                        src_access |= r->write_access;
                    }
                    if (r->has_readers) {
                        src_stage  |= r->read_stages;
                    }
                } else {
                    src_stage  = r->write_stage;
                    src_access = r->write_access;
                }
                out_batch->mem_barrier.srcStageMask  |= src_stage;
                out_batch->mem_barrier.srcAccessMask |= src_access;
                out_batch->mem_barrier.dstStageMask  |= dst_stage;
                out_batch->mem_barrier.dstAccessMask |= dst_access;
                out_batch->has_mem_barrier            = true;
            }

            if (req.is_write) {
                r->write_stage    = req.stage;
                r->write_access   = req.access;
                r->dirty_write    = true;
                r->has_readers    = false;
                r->read_stages    = VK_PIPELINE_STAGE_2_NONE;
                r->visible_stages = VK_PIPELINE_STAGE_2_NONE;
            } else {
                r->dirty_write     = false;
                r->visible_stages |= dst_stage;
                r->has_readers     = true;
                r->read_stages    |= req.stage;
            }
        }
    }
}

static void pass_label_color(RgPassType t, float out_rgba[4]) {
    switch (t) {
    case RG_PASS_COMPUTE: out_rgba[0] = 0.20f; out_rgba[1] = 0.78f; out_rgba[2] = 0.92f; out_rgba[3] = 1.0f; break;
    case RG_PASS_SHADOW:  out_rgba[0] = 0.95f; out_rgba[1] = 0.76f; out_rgba[2] = 0.22f; out_rgba[3] = 1.0f; break;
    case RG_PASS_GI:      out_rgba[0] = 0.68f; out_rgba[1] = 0.42f; out_rgba[2] = 0.96f; out_rgba[3] = 1.0f; break;
    case RG_PASS_SCENE:   out_rgba[0] = 0.24f; out_rgba[1] = 0.85f; out_rgba[2] = 0.48f; out_rgba[3] = 1.0f; break;
    case RG_PASS_POST:    out_rgba[0] = 0.96f; out_rgba[1] = 0.36f; out_rgba[2] = 0.72f; out_rgba[3] = 1.0f; break;
    case RG_PASS_OVERLAY:
    default:              out_rgba[0] = 0.88f; out_rgba[1] = 0.90f; out_rgba[2] = 0.94f; out_rgba[3] = 1.0f; break;
    }
}

static bool rg_prepare_slot_ts(RgGraph *g, VkCommandBuffer cmd) {
    const bool use_ts = (s_query_pool != VK_NULL_HANDLE && s_vk != nullptr && s_ts_period_ns > 0.0f);
    if (!use_ts || g->slot_reset_done) {
        return use_ts;
    }
    const uint32_t slot  = g->frame_slot % k_rg_frame_slots;
    const uint32_t qbase = slot * k_rg_stamps_slot;
    if ((s_slot_reset_msk & (1u << slot)) == 0u) {
        if (s_ts_host_reset && s_vk->device != VK_NULL_HANDLE) {
            vkResetQueryPool(s_vk->device, s_query_pool, qbase, k_rg_stamps_slot);
        } else if (cmd != VK_NULL_HANDLE) {
            vkCmdResetQueryPool(cmd, s_query_pool, qbase, k_rg_stamps_slot);
        }
    }
    s_slot_reset_msk  &= ~(1u << slot);
    g->slot_reset_done = true;
    return true;
}

void rg_flush(RgGraph *g, VkCommandBuffer cmd) {
    if (g == nullptr || cmd == VK_NULL_HANDLE) {
        return;
    }
    const uint32_t slot  = g->frame_slot % k_rg_frame_slots;
    const uint32_t qbase = slot * k_rg_stamps_slot;
    const bool use_ts = rg_prepare_slot_ts(g, cmd);

    while (g->executed_count < g->pass_count) {
        uint32_t i = g->executed_count++;
        const RgPassDesc *pass = &g->passes[i];
        if (!pass->enabled || pass->execute == nullptr) {
            continue;
        }

        RgBarrierBatch batch = {0};
        rg_plan_pass_barriers(g, i, &batch);
        if (batch.image_count > 0u || batch.has_mem_barrier) {
            VkDependencyInfo dep = {
                .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount      = batch.has_mem_barrier ? 1u : 0u,
                .pMemoryBarriers         = batch.has_mem_barrier ? &batch.mem_barrier : nullptr,
                .imageMemoryBarrierCount = batch.image_count,
                .pImageMemoryBarriers    = batch.image_count > 0u ? batch.image_barriers : nullptr,
            };
            vkCmdPipelineBarrier2(cmd, &dep);
        }

        if (s_cmd_begin_label != nullptr) {
            VkDebugUtilsLabelEXT lbl = {
                .sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
                .pLabelName = pass->name,
            };
            pass_label_color(pass->type, lbl.color);
            s_cmd_begin_label(cmd, &lbl);
        }

        if (use_ts && g->recorded_ts < RG_MAX_PASSES) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                 s_query_pool, qbase + g->recorded_ts * 2u);
        }

        pass->execute(cmd, pass->user);

        if (use_ts && g->recorded_ts < RG_MAX_PASSES) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                 s_query_pool, qbase + g->recorded_ts * 2u + 1u);
            s_slot_rec[slot].types[g->recorded_ts] = pass->type;
            snprintf(s_slot_rec[slot].names[g->recorded_ts],
                     sizeof(s_slot_rec[slot].names[g->recorded_ts]),
                     "%s", pass->name != nullptr ? pass->name : "Pass");
            g->recorded_ts++;
        }

        if (s_cmd_end_label != nullptr) {
            s_cmd_end_label(cmd);
        }
    }
}

void rg_begin_external_pass(RgGraph *g, VkCommandBuffer cmd, const char *name, RgPassType type) {
    if (g == nullptr || cmd == VK_NULL_HANDLE) {
        return;
    }
    const uint32_t slot  = g->frame_slot % k_rg_frame_slots;
    const uint32_t qbase = slot * k_rg_stamps_slot;
    const bool use_ts = rg_prepare_slot_ts(g, cmd);
    if (s_cmd_begin_label != nullptr) {
        VkDebugUtilsLabelEXT lbl = {
            .sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
            .pLabelName = name != nullptr ? name : "External",
        };
        pass_label_color(type, lbl.color);
        s_cmd_begin_label(cmd, &lbl);
    }
    if (use_ts && g->recorded_ts < RG_MAX_PASSES) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                             s_query_pool, qbase + g->recorded_ts * 2u);
        s_slot_rec[slot].types[g->recorded_ts] = type;
        snprintf(s_slot_rec[slot].names[g->recorded_ts],
                 sizeof(s_slot_rec[slot].names[g->recorded_ts]),
                 "%s", name != nullptr ? name : "External");
    }
}

void rg_end_external_pass(RgGraph *g, VkCommandBuffer cmd) {
    if (g == nullptr || cmd == VK_NULL_HANDLE) {
        return;
    }
    const uint32_t slot  = g->frame_slot % k_rg_frame_slots;
    const uint32_t qbase = slot * k_rg_stamps_slot;
    if (s_query_pool != VK_NULL_HANDLE && g->slot_reset_done && g->recorded_ts < RG_MAX_PASSES) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                             s_query_pool, qbase + g->recorded_ts * 2u + 1u);
        g->recorded_ts++;
    }
    if (s_cmd_end_label != nullptr) {
        s_cmd_end_label(cmd);
    }
}

void rg_end_frame(RgGraph *g) {
    if (g == nullptr) {
        return;
    }
    const uint32_t slot = g->frame_slot % k_rg_frame_slots;
    if (s_query_pool != VK_NULL_HANDLE && g->slot_reset_done) {
        s_slot_rec[slot].pass_count = g->recorded_ts;
        s_slot_rec[slot].armed      = (g->recorded_ts > 0u);
    }
    for (uint32_t r = 0u; r < g->res_count; ++r) {
        if (g->resources[r].kind == RG_RES_IMAGE && g->resources[r].external_layout != nullptr) {
            *g->resources[r].external_layout = g->resources[r].layout;
        }
    }
}

void rg_get_profile(RgProfileSnapshot *out) {
    if (out == nullptr) {
        return;
    }
    *out = s_profile;
}
