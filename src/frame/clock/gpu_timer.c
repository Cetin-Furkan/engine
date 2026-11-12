#include "frame/internal.h"

// Hardware intervals for the two scene slots. vkCmdWriteTimestamp2 needs a
// single pipeline stage bit: STAGE_NONE is zero and is not a timestamp point.
// TOP_OF_PIPE is the start. ALL_COMMANDS matches the acquire signal, so the
// total covers compute, the color pass, and the foreign-queue release.
// Queries 0..7 belong to slot[0] and slot[1] graphics/compute; queries 8..11
// measure dedicated SDMA async PRIME copies when HYBRID_MUXLESS is active.

static int scene_slot_index(const FrameSlot *frame) {
    if (frame == &frame_state.slot[0]) {
        return 0;
    }
    if (frame == &frame_state.slot[1]) {
        return 1;
    }
    return -1;
}

static uint64_t delta_ticks(uint64_t later, uint64_t earlier, uint32_t valid_bits) {
    if (valid_bits == 0u) {
        return 0u;
    }
    if (valid_bits >= 64u) {
        return later >= earlier ? (later - earlier) : 0u;
    }
    uint64_t mask = (1ull << valid_bits) - 1ull;
    uint64_t diff = (later - earlier) & mask;
    if (diff > (mask >> 1u)) {
        return 0u;
    }
    return diff;
}

static uint64_t delta_to_ns(uint64_t ticks_diff) {
    if (ticks_diff == 0u || frame_state.ts_period_ns <= 0.0f) {
        return 0u;
    }
    double ns = (double)ticks_diff * (double)frame_state.ts_period_ns;
    if (ns >= 18446744073709551615.0) {
        return UINT64_MAX;
    }
    return (uint64_t)ns;
}

static uint32_t ns_to_us_u32(uint64_t ns) {
    uint64_t us = (ns + 500ull) / 1000ull;
    return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

void gpu_timer_init(VulkanContext *vk) {
    frame_state.ts_pool            = VK_NULL_HANDLE;
    frame_state.ts_period_ns       = 0.0f;
    frame_state.ts_valid_bits      = 0u;
    frame_state.ts_xfer_valid_bits = 0u;
    frame_state.ts_reset_mask      = 0u;
    frame_state.ts_host_reset      = false;
    frame_state.last_gpu_comp_ns    = 0u;
    frame_state.last_gpu_rend_ns    = 0u;
    frame_state.last_gpu_xfer_ns    = 0u;
    frame_state.last_gpu_tot_ns     = 0u;
    frame_state.gpu_samples_retired = 0u;
    for (int i = 0; i < 2; ++i) {
        frame_state.slot[i].ts_armed      = false;
        frame_state.slot[i].ts_xfer_armed = false;
        frame_state.slot[i].ts_base       = (uint8_t)(i * 4);
        frame_state.slot[i].ts_xfer_base  = (uint8_t)(8 + i * 2);
    }
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->physical_device == VK_NULL_HANDLE) {
        return;
    }
    VkPhysicalDeviceProperties props = {0};
    vkGetPhysicalDeviceProperties(vk->physical_device, &props);
    if (props.limits.timestampPeriod <= 0.0f) {
        fprintf(stderr, "[Engine] GPU timestamp queries are unavailable on this device.\n");
        return;
    }

    uint32_t qf_count = 0u;
    vkGetPhysicalDeviceQueueFamilyProperties(vk->physical_device, &qf_count, nullptr);
    if (qf_count == 0u) {
        return;
    }
    VkQueueFamilyProperties stack_qf[16];
    VkQueueFamilyProperties *qfs = qf_count <= 16u
        ? stack_qf
        : malloc(sizeof(*qfs) * qf_count);
    if (qfs == nullptr) {
        return;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(vk->physical_device, &qf_count, qfs);
    uint32_t gfx_bits = 0u;
    uint32_t xfer_bits = 0u;
    if (vk->queues.graphics_family_index < qf_count) {
        gfx_bits = qfs[vk->queues.graphics_family_index].timestampValidBits;
    }
    if (vk->queues.transfer_family_index < qf_count) {
        xfer_bits = qfs[vk->queues.transfer_family_index].timestampValidBits;
    }
    if (qfs != stack_qf) {
        free(qfs);
    }
    if (gfx_bits == 0u) {
        fprintf(stderr, "[Engine] Graphics queue family lacks hardware timestamps.\n");
        return;
    }

    VkQueryPoolCreateInfo info = {
        .sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType  = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = k_gpu_timer_slots,
    };
    if (vkCreateQueryPool(vk->device, &info, nullptr, &frame_state.ts_pool) != VK_SUCCESS) {
        frame_state.ts_pool = VK_NULL_HANDLE;
        fprintf(stderr, "[Engine] GPU timestamp query pool was not created.\n");
        return;
    }
    vulkan_name_object(vk, VK_OBJECT_TYPE_QUERY_POOL, (uint64_t)frame_state.ts_pool,
                       "Frame:TimestampPool");
    frame_state.ts_period_ns       = props.limits.timestampPeriod;
    frame_state.ts_valid_bits      = gfx_bits;
    frame_state.ts_xfer_valid_bits = xfer_bits;
    frame_state.ts_host_reset      = vk->device_info.supports_host_query_reset;
    if (frame_state.ts_host_reset) {
        vkResetQueryPool(vk->device, frame_state.ts_pool, 0u, k_gpu_timer_slots);
        frame_state.ts_reset_mask = (uint16_t)((1u << k_gpu_timer_slots) - 1u);
    }
}

void gpu_timer_shutdown(VulkanContext *vk) {
    if (vk != nullptr && vk->device != VK_NULL_HANDLE && frame_state.ts_pool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(vk->device, frame_state.ts_pool, nullptr);
    }
    frame_state.ts_pool            = VK_NULL_HANDLE;
    frame_state.ts_period_ns       = 0.0f;
    frame_state.ts_valid_bits      = 0u;
    frame_state.ts_xfer_valid_bits = 0u;
    frame_state.ts_reset_mask      = 0u;
    frame_state.ts_host_reset      = false;
    frame_state.last_gpu_comp_ns    = 0u;
    frame_state.last_gpu_rend_ns    = 0u;
    frame_state.last_gpu_xfer_ns    = 0u;
    frame_state.last_gpu_tot_ns     = 0u;
    frame_state.gpu_samples_retired = 0u;
    for (int i = 0; i < 2; ++i) {
        frame_state.slot[i].ts_armed      = false;
        frame_state.slot[i].ts_xfer_armed = false;
    }
}

void gpu_timer_begin_frame(VulkanContext *vk, VkCommandBuffer cmd, FrameSlot *frame) {
    if (frame == nullptr) {
        return;
    }
    int index = scene_slot_index(frame);
    frame->ts_armed      = false;
    frame->ts_xfer_armed = false;
    if (index < 0) {
        return;
    }
    frame->ts_base      = (uint8_t)(index * 4);
    frame->ts_xfer_base = (uint8_t)(8 + index * 2);
    if (cmd == VK_NULL_HANDLE || frame_state.ts_pool == VK_NULL_HANDLE) {
        return;
    }
    uint16_t mask4 = (uint16_t)(0xfu << frame->ts_base);
    if ((frame_state.ts_reset_mask & mask4) != mask4) {
        if (frame_state.ts_host_reset && vk != nullptr && vk->device != VK_NULL_HANDLE) {
            vkResetQueryPool(vk->device, frame_state.ts_pool, frame->ts_base, 4u);
        } else {
            vkCmdResetQueryPool(cmd, frame_state.ts_pool, frame->ts_base, 4u);
        }
    }
    frame_state.ts_reset_mask &= (uint16_t)~mask4;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                         frame_state.ts_pool, (uint32_t)frame->ts_base);
}

void gpu_timer_mark_compute_end(VkCommandBuffer cmd, const FrameSlot *frame) {
    if (cmd == VK_NULL_HANDLE || frame == nullptr || scene_slot_index(frame) < 0 ||
        frame_state.ts_pool == VK_NULL_HANDLE) {
        return;
    }
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         frame_state.ts_pool, (uint32_t)frame->ts_base + 1u);
}

void gpu_timer_mark_render_begin(VkCommandBuffer cmd, const FrameSlot *frame) {
    if (cmd == VK_NULL_HANDLE || frame == nullptr || scene_slot_index(frame) < 0 ||
        frame_state.ts_pool == VK_NULL_HANDLE) {
        return;
    }
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                         frame_state.ts_pool, (uint32_t)frame->ts_base + 2u);
}

void gpu_timer_end_frame(VkCommandBuffer cmd, FrameSlot *frame) {
    if (cmd == VK_NULL_HANDLE || frame == nullptr || scene_slot_index(frame) < 0 ||
        frame_state.ts_pool == VK_NULL_HANDLE) {
        return;
    }
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         frame_state.ts_pool, (uint32_t)frame->ts_base + 3u);
    frame->ts_armed = true;
}

void gpu_timer_begin_transfer(VulkanContext *vk, VkCommandBuffer cmd, FrameSlot *frame) {
    if (cmd == VK_NULL_HANDLE || frame == nullptr || scene_slot_index(frame) < 0 ||
        frame_state.ts_pool == VK_NULL_HANDLE || frame_state.ts_xfer_valid_bits == 0u) {
        return;
    }
    uint16_t mask2 = (uint16_t)(0x3u << frame->ts_xfer_base);
    if ((frame_state.ts_reset_mask & mask2) != mask2) {
        if (frame_state.ts_host_reset && vk != nullptr && vk->device != VK_NULL_HANDLE) {
            vkResetQueryPool(vk->device, frame_state.ts_pool, frame->ts_xfer_base, 2u);
        } else {
            vkCmdResetQueryPool(cmd, frame_state.ts_pool, frame->ts_xfer_base, 2u);
        }
    }
    frame_state.ts_reset_mask &= (uint16_t)~mask2;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                         frame_state.ts_pool, (uint32_t)frame->ts_xfer_base);
}

void gpu_timer_end_transfer(VkCommandBuffer cmd, FrameSlot *frame) {
    if (cmd == VK_NULL_HANDLE || frame == nullptr || scene_slot_index(frame) < 0 ||
        frame_state.ts_pool == VK_NULL_HANDLE || frame_state.ts_xfer_valid_bits == 0u) {
        return;
    }
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         frame_state.ts_pool, (uint32_t)frame->ts_xfer_base + 1u);
    frame->ts_xfer_armed = true;
}

void gpu_timer_harvest(VulkanContext *vk, FrameSlot *frame) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || frame == nullptr ||
        !frame->ts_armed || scene_slot_index(frame) < 0 ||
        frame_state.ts_pool == VK_NULL_HANDLE || frame_state.ts_period_ns <= 0.0f) {
        return;
    }
    uint64_t raw[8] = {0};
    VkResult res = vkGetQueryPoolResults(
        vk->device, frame_state.ts_pool, frame->ts_base, 4u,
        sizeof raw, raw, 2u * sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (res == VK_NOT_READY || raw[1] == 0u || raw[3] == 0u || raw[5] == 0u || raw[7] == 0u) {
        return;
    }
    frame->ts_armed = false;
    if (frame_state.ts_host_reset) {
        vkResetQueryPool(vk->device, frame_state.ts_pool, frame->ts_base, 4u);
        frame_state.ts_reset_mask |= (uint16_t)(0xfu << frame->ts_base);
    }
    if (res != VK_SUCCESS) {
        frame->ts_xfer_armed = false;
        return;
    }

    uint64_t xfer_ns = 0u;
    if (frame->ts_xfer_armed && frame_state.ts_xfer_valid_bits > 0u) {
        uint64_t xraw[4] = {0};
        VkResult xres = vkGetQueryPoolResults(
            vk->device, frame_state.ts_pool, frame->ts_xfer_base, 2u,
            sizeof xraw, xraw, 2u * sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        frame->ts_xfer_armed = false;
        if (frame_state.ts_host_reset) {
            vkResetQueryPool(vk->device, frame_state.ts_pool, frame->ts_xfer_base, 2u);
            frame_state.ts_reset_mask |= (uint16_t)(0x3u << frame->ts_xfer_base);
        }
        if (xres == VK_SUCCESS && xraw[1] != 0u && xraw[3] != 0u) {
            xfer_ns = delta_to_ns(delta_ticks(xraw[2], xraw[0], frame_state.ts_xfer_valid_bits));
        }
    }

    const uint32_t bits = frame_state.ts_valid_bits;
    const uint64_t comp_ns = delta_to_ns(delta_ticks(raw[2], raw[0], bits));
    const uint64_t rend_ns = delta_to_ns(delta_ticks(raw[6], raw[4], bits));
    const uint64_t gfx_ns  = delta_to_ns(delta_ticks(raw[6], raw[0], bits));
    const uint64_t tot_ns  = gfx_ns + xfer_ns;

    frame_state.last_gpu_comp_ns = comp_ns;
    frame_state.last_gpu_rend_ns = rend_ns;
    frame_state.last_gpu_xfer_ns = xfer_ns;
    frame_state.last_gpu_tot_ns  = tot_ns;
    frame_state.gpu_samples_retired++;
    frame_clock_note_gpu(&frame_state.clock,
                         ns_to_us_u32(comp_ns),
                         ns_to_us_u32(rend_ns + xfer_ns),
                         ns_to_us_u32(tot_ns));
}
