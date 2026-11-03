#include "frame/internal.h"

// Hardware intervals for the two scene slots. vkCmdWriteTimestamp2 needs a
// real pipeline stage: STAGE_NONE is zero and is not a timestamp point.
// TOP_OF_PIPE is the start. ALL_COMMANDS matches the acquire signal, so the
// total covers compute, the color pass, and the foreign-queue release.
// A resize image and a bar image are not scene slots. They must not reset
// queries 0..7, which belong to slot[0] and slot[1].

static int scene_slot_index(const FrameSlot *frame) {
    if (frame == &frame_state.slot[0]) {
        return 0;
    }
    if (frame == &frame_state.slot[1]) {
        return 1;
    }
    return -1;
}

static uint32_t ticks_to_us(uint64_t later, uint64_t earlier) {
    if (later <= earlier || frame_state.ts_period_ns <= 0.0f) {
        return 0;
    }
    double us = (double)(later - earlier) * (double)frame_state.ts_period_ns / 1000.0;
    if (us >= 4294967295.0) {
        return UINT32_MAX;
    }
    return (uint32_t)us;
}

void gpu_timer_init(VulkanContext *vk) {
    frame_state.ts_pool      = VK_NULL_HANDLE;
    frame_state.ts_period_ns = 0.0f;
    frame_state.slot[0].ts_armed = false;
    frame_state.slot[1].ts_armed = false;
    frame_state.slot[0].ts_base  = 0;
    frame_state.slot[1].ts_base  = 4;
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->physical_device == VK_NULL_HANDLE) {
        return;
    }
    VkPhysicalDeviceProperties props = {0};
    vkGetPhysicalDeviceProperties(vk->physical_device, &props);
    if (props.limits.timestampPeriod <= 0.0f || props.limits.timestampComputeAndGraphics != VK_TRUE) {
        fprintf(stderr, "[Engine] GPU timestamp queries are unavailable on this device.\n");
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
    frame_state.ts_period_ns = props.limits.timestampPeriod;
}

void gpu_timer_shutdown(VulkanContext *vk) {
    if (vk != nullptr && vk->device != VK_NULL_HANDLE && frame_state.ts_pool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(vk->device, frame_state.ts_pool, nullptr);
    }
    frame_state.ts_pool      = VK_NULL_HANDLE;
    frame_state.ts_period_ns = 0.0f;
    frame_state.slot[0].ts_armed = false;
    frame_state.slot[1].ts_armed = false;
}

void gpu_timer_begin_frame(VkCommandBuffer cmd, FrameSlot *frame) {
    if (frame == nullptr) {
        return;
    }
    int index = scene_slot_index(frame);
    frame->ts_armed = false;
    if (index < 0) {
        return;
    }
    frame->ts_base = (uint8_t)(index * 4);
    if (cmd == VK_NULL_HANDLE || frame_state.ts_pool == VK_NULL_HANDLE) {
        return;
    }
    vkCmdResetQueryPool(cmd, frame_state.ts_pool, frame->ts_base, 4u);
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
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
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

void gpu_timer_harvest(VulkanContext *vk, FrameSlot *frame) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || frame == nullptr ||
        !frame->ts_armed || scene_slot_index(frame) < 0 ||
        frame_state.ts_pool == VK_NULL_HANDLE || frame_state.ts_period_ns <= 0.0f) {
        return;
    }
    uint64_t ticks[4] = {0};
    VkResult res = vkGetQueryPoolResults(
        vk->device, frame_state.ts_pool, frame->ts_base, 4u,
        sizeof ticks, ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (res == VK_NOT_READY) {
        return;
    }
    frame->ts_armed = false;
    if (res != VK_SUCCESS) {
        return;
    }
    frame_clock_note_gpu(&frame_state.clock,
                         ticks_to_us(ticks[1], ticks[0]),
                         ticks_to_us(ticks[3], ticks[2]),
                         ticks_to_us(ticks[3], ticks[0]));
}
