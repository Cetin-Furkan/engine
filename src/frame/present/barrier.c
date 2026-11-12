#include "frame/internal.h"
#include "vulkan/device.h"

// Sync2 barriers for a DMA-BUF that is cleared and then handed to the
// compositor. oldLayout UNDEFINED has nothing to make available, so the
// source stage is NONE. The foreign-queue release is published by the
// timeline signal at the end of the command buffer, so the destination
// stage is NONE as well.

static VkImageSubresourceRange one_level(VkImageAspectFlags aspect) {
    return (VkImageSubresourceRange){
        .aspectMask     = aspect,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };
}

void frame_fill_discard_color(VkImageMemoryBarrier2 *out, VkImage image, bool unified) {
    *out = (VkImageMemoryBarrier2){
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask       = VK_ACCESS_2_NONE,
        .dstStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = unified ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = image,
        .subresourceRange    = one_level(VK_IMAGE_ASPECT_COLOR_BIT),
    };
}

void frame_fill_discard_depth(VkImageMemoryBarrier2 *out, VkImage image, bool unified) {
    *out = (VkImageMemoryBarrier2){
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask       = VK_ACCESS_2_NONE,
        .dstStageMask        = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
        .dstAccessMask       = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = unified ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = image,
        .subresourceRange    = one_level(VK_IMAGE_ASPECT_DEPTH_BIT),
    };
}

void frame_fill_release_foreign(VkImageMemoryBarrier2 *out, const VulkanContext *vk, VkImage image) {
    uint32_t dst_queue = vk->device_info.supports_queue_family_foreign
        ? VK_QUEUE_FAMILY_FOREIGN_EXT
        : VK_QUEUE_FAMILY_EXTERNAL;
    *out = (VkImageMemoryBarrier2){
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask        = VK_PIPELINE_STAGE_2_NONE,
        .dstAccessMask       = VK_ACCESS_2_NONE,
        .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = vk->queues.graphics_family_index,
        .dstQueueFamilyIndex = dst_queue,
        .image               = image,
        .subresourceRange    = one_level(VK_IMAGE_ASPECT_COLOR_BIT),
    };
}

void frame_cmd_barriers(VkCommandBuffer cmd, const VkImageMemoryBarrier2 *barriers, uint32_t count) {
    VkDependencyInfo dep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = count,
        .pImageMemoryBarriers    = barriers,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

static uint32_t foreign_queue_family(const VulkanContext *vk) {
    return vk->device_info.supports_queue_family_foreign
        ? VK_QUEUE_FAMILY_FOREIGN_EXT
        : VK_QUEUE_FAMILY_EXTERNAL;
}

static FrameRect clamp_slot_copy_rect(const FrameSlot *frame) {
    uint32_t fw = frame->image.width;
    uint32_t fh = frame->image.height;
    FrameRect r = frame->copy_damage;
    if (r.width == 0u || r.height == 0u || !frame->foreign_owned) {
        return (FrameRect){ .x = 0, .y = 0, .width = fw, .height = fh };
    }
    int64_t x0 = r.x < 0 ? 0 : r.x;
    int64_t y0 = r.y < 0 ? 0 : r.y;
    int64_t x1 = (int64_t)r.x + (int64_t)r.width;
    int64_t y1 = (int64_t)r.y + (int64_t)r.height;
    if (x1 > (int64_t)fw) {
        x1 = (int64_t)fw;
    }
    if (y1 > (int64_t)fh) {
        y1 = (int64_t)fh;
    }
    if (x0 >= x1 || y0 >= y1) {
        return (FrameRect){ .x = 0, .y = 0, .width = fw, .height = fh };
    }
    return (FrameRect){
        .x      = (int32_t)x0,
        .y      = (int32_t)y0,
        .width  = (uint32_t)(x1 - x0),
        .height = (uint32_t)(y1 - y0),
    };
}

static void fill_color_to_copy(VkImageMemoryBarrier2 *out, VkImage image, bool unified) {
    *out = (VkImageMemoryBarrier2){
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask        = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT,
        .oldLayout           = unified ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout           = unified ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = image,
        .subresourceRange    = one_level(VK_IMAGE_ASPECT_COLOR_BIT),
    };
}

static void fill_export_as_copy_dst(VkImageMemoryBarrier2 *out, const VulkanContext *vk,
                                    VkImage image, uint32_t dst_family, bool partial_preserve) {
    *out = (VkImageMemoryBarrier2){
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask       = VK_ACCESS_2_NONE,
        .dstStageMask        = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout           = partial_preserve ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = partial_preserve ? foreign_queue_family(vk) : VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = partial_preserve ? dst_family : VK_QUEUE_FAMILY_IGNORED,
        .image               = image,
        .subresourceRange    = one_level(VK_IMAGE_ASPECT_COLOR_BIT),
    };
}

static void fill_release_from_copy(VkImageMemoryBarrier2 *out, const VulkanContext *vk,
                                   VkImage image, uint32_t src_family) {
    *out = (VkImageMemoryBarrier2){
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask        = VK_PIPELINE_STAGE_2_NONE,
        .dstAccessMask       = VK_ACCESS_2_NONE,
        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = src_family,
        .dstQueueFamilyIndex = foreign_queue_family(vk),
        .image               = image,
        .subresourceRange    = one_level(VK_IMAGE_ASPECT_COLOR_BIT),
    };
}

static void cmd_prime_copy_region(VkCommandBuffer cmd, const FrameSlot *frame,
                                  FrameRect rect, bool unified_src) {
    VkImageCopy2 region = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
        .srcSubresource = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel       = 0,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
        .srcOffset = { .x = rect.x, .y = rect.y, .z = 0 },
        .dstSubresource = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel       = 0,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
        .dstOffset = { .x = rect.x, .y = rect.y, .z = 0 },
        .extent = {
            .width  = rect.width,
            .height = rect.height,
            .depth  = 1,
        },
    };
    VkCopyImageInfo2 copy = {
        .sType          = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
        .srcImage       = frame->prime_image,
        .srcImageLayout = unified_src ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .dstImage       = frame->image.image,
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount    = 1,
        .pRegions       = &region,
    };
    vkCmdCopyImage2(cmd, &copy);
}

void frame_cmd_finish_present(VkCommandBuffer cmd, const VulkanContext *vk, const FrameSlot *frame) {
    const bool unified_prime = vk->device_info.supports_unified_image_layouts;
    if (frame_use_async_prime_transfer(vk, frame)) {
        // Transition CONCURRENT device-local color target to TRANSFER_SRC_OPTIMAL
        // (or keep GENERAL when VK_KHR_unified_image_layouts is active);
        // the prime_ready_sem signal publishes visibility to the DMA transfer queue.
        VkImageMemoryBarrier2 to_copy_src = {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_NONE,
            .dstAccessMask       = VK_ACCESS_2_NONE,
            .oldLayout           = unified_prime ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout           = unified_prime ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = frame->prime_image,
            .subresourceRange    = one_level(VK_IMAGE_ASPECT_COLOR_BIT),
        };
        frame_cmd_barriers(cmd, &to_copy_src, 1);
        return;
    }

    if (frame->image.prime_copy && frame->prime_image != VK_NULL_HANDLE) {
        FrameRect rect = clamp_slot_copy_rect(frame);
        bool partial = frame->foreign_owned &&
            (rect.x != 0 || rect.y != 0 ||
             rect.width != frame->image.width || rect.height != frame->image.height);
        VkImageMemoryBarrier2 prepare[2];
        fill_color_to_copy(&prepare[0], frame->prime_image, unified_prime);
        fill_export_as_copy_dst(&prepare[1], vk, frame->image.image,
                                vk->queues.graphics_family_index, partial);
        frame_cmd_barriers(cmd, prepare, 2);

        cmd_prime_copy_region(cmd, frame, rect, unified_prime);

        VkImageMemoryBarrier2 release;
        fill_release_from_copy(&release, vk, frame->image.image, vk->queues.graphics_family_index);
        frame_cmd_barriers(cmd, &release, 1);
        return;
    }

    VkImageMemoryBarrier2 to_general;
    frame_fill_release_foreign(&to_general, vk, frame->image.image);
    frame_cmd_barriers(cmd, &to_general, 1);
}

static int record_prime_transfer_cmd(VulkanContext *vk, FrameSlot *frame) {
    VkResult res = VK_SUCCESS;
    if (frame->transfer_cmd_buf == VK_NULL_HANDLE) {
        VkCommandBufferAllocateInfo alloc_info = {
            .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool        = vk->transfer_cmd_pool,
            .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        res = vkAllocateCommandBuffers(vk->device, &alloc_info, &frame->transfer_cmd_buf);
        if (res != VK_SUCCESS) {
            fprintf(stderr, "[Engine Error] vkAllocateCommandBuffers (transfer) failed: %s (%d)\n",
                    vulkan_vkresult_to_str(res), (int)res);
            return -1;
        }
    } else {
        res = vkResetCommandBuffer(frame->transfer_cmd_buf, 0);
        if (res != VK_SUCCESS) {
            fprintf(stderr, "[Engine Error] vkResetCommandBuffer (transfer) failed: %s (%d)\n",
                    vulkan_vkresult_to_str(res), (int)res);
            return -1;
        }
    }

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    res = vkBeginCommandBuffer(frame->transfer_cmd_buf, &begin_info);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Engine Error] vkBeginCommandBuffer (transfer) failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }

    gpu_timer_begin_transfer(vk, frame->transfer_cmd_buf, frame);

    FrameRect rect = clamp_slot_copy_rect(frame);
    bool partial = frame->foreign_owned &&
        (rect.x != 0 || rect.y != 0 ||
         rect.width != frame->image.width || rect.height != frame->image.height);

    // prime_image uses VK_SHARING_MODE_CONCURRENT across graphics + transfer and
    // was transitioned to TRANSFER_SRC_OPTIMAL before prime_ready_sem signalled.
    VkImageMemoryBarrier2 prepare_dst;
    fill_export_as_copy_dst(&prepare_dst, vk, frame->image.image,
                            vk->queues.transfer_family_index, partial);
    frame_cmd_barriers(frame->transfer_cmd_buf, &prepare_dst, 1);

    cmd_prime_copy_region(frame->transfer_cmd_buf, frame, rect,
                          vk->device_info.supports_unified_image_layouts);

    VkImageMemoryBarrier2 release;
    fill_release_from_copy(&release, vk, frame->image.image, vk->queues.transfer_family_index);
    frame_cmd_barriers(frame->transfer_cmd_buf, &release, 1);

    gpu_timer_end_transfer(frame->transfer_cmd_buf, frame);

    res = vkEndCommandBuffer(frame->transfer_cmd_buf);
    if (res != VK_SUCCESS) {
        frame->ts_xfer_armed = false;
        fprintf(stderr, "[Engine Error] vkEndCommandBuffer (transfer) failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }
    return 0;
}

int frame_submit_slot_commands(VulkanContext *vk, FrameSlot *frame, VkSemaphore acquire,
                               uint64_t signal_point, uint64_t wait_point,
                               VkPipelineStageFlags2 wait_stage) {
    bool has_wait = (wait_point != 0u && wait_point < signal_point);
    if (frame_use_async_prime_transfer(vk, frame)) {
        if (record_prime_transfer_cmd(vk, frame) != 0) {
            return -1;
        }
        uint64_t prime_pt = frame_state.prime_ready_point + 1u;
        VkCommandBufferSubmitInfo gfx_cmd = {
            .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = frame->cmd_buf,
        };
        VkSemaphoreSubmitInfo gfx_wait = {
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = acquire,
            .value     = wait_point,
            .stageMask = wait_stage,
        };
        VkSemaphoreSubmitInfo gfx_sig = {
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = frame_state.prime_ready_sem,
            .value     = prime_pt,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
        VkSubmitInfo2 gfx_submit = {
            .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .waitSemaphoreInfoCount   = has_wait ? 1u : 0u,
            .pWaitSemaphoreInfos      = has_wait ? &gfx_wait : nullptr,
            .commandBufferInfoCount   = 1u,
            .pCommandBufferInfos      = &gfx_cmd,
            .signalSemaphoreInfoCount = 1u,
            .pSignalSemaphoreInfos    = &gfx_sig,
        };
        VkResult res = vkQueueSubmit2(vk->queues.graphics_queue, 1u, &gfx_submit, VK_NULL_HANDLE);
        if (res != VK_SUCCESS) {
            frame->ts_xfer_armed = false;
            if (res == VK_ERROR_DEVICE_LOST) {
                vulkan_report_device_fault(vk, "vkQueueSubmit2 (graphics->prime)");
            }
            fprintf(stderr, "[Engine Error] vkQueueSubmit2 (graphics->prime) failed: %s (%d)\n",
                    vulkan_vkresult_to_str(res), (int)res);
            return -1;
        }
        frame_state.prime_ready_point = prime_pt;

        VkCommandBufferSubmitInfo xfer_cmd = {
            .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = frame->transfer_cmd_buf,
        };
        VkSemaphoreSubmitInfo xfer_waits[2];
        uint32_t xfer_wait_count = 0u;
        xfer_waits[xfer_wait_count++] = (VkSemaphoreSubmitInfo){
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = frame_state.prime_ready_sem,
            .value     = prime_pt,
            .stageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        };
        if (frame_state.last_acquire_signal_point != 0u &&
            frame_state.last_acquire_signal_point < signal_point &&
            frame_state.last_acquire_signal_queue != vk->queues.transfer_queue) {
            xfer_waits[xfer_wait_count++] = (VkSemaphoreSubmitInfo){
                .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .semaphore = acquire,
                .value     = frame_state.last_acquire_signal_point,
                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            };
        }
        VkSemaphoreSubmitInfo xfer_sigs[2] = {
            {
                .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .semaphore = acquire,
                .value     = signal_point,
                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            },
            {
                .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .semaphore = frame_state.prime_acquire_sync,
                .value     = 0,
                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            },
        };
        VkSubmitInfo2 xfer_submit = {
            .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .waitSemaphoreInfoCount   = xfer_wait_count,
            .pWaitSemaphoreInfos      = xfer_waits,
            .commandBufferInfoCount   = 1u,
            .pCommandBufferInfos      = &xfer_cmd,
            .signalSemaphoreInfoCount = 2u,
            .pSignalSemaphoreInfos    = xfer_sigs,
        };
        res = vkQueueSubmit2(vk->queues.transfer_queue, 1u, &xfer_submit, VK_NULL_HANDLE);
        if (res != VK_SUCCESS) {
            frame->ts_xfer_armed = false;
            if (res == VK_ERROR_DEVICE_LOST) {
                vulkan_report_device_fault(vk, "vkQueueSubmit2 (transfer DMA)");
            }
            VkSemaphoreWaitInfo drain = {
                .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                .semaphoreCount = 1u,
                .pSemaphores    = &frame_state.prime_ready_sem,
                .pValues        = &prime_pt,
            };
            (void)vkWaitSemaphores(vk->device, &drain, 3000000000ull);
            fprintf(stderr, "[Engine Error] vkQueueSubmit2 (transfer DMA) failed: %s (%d)\n",
                    vulkan_vkresult_to_str(res), (int)res);
            return -1;
        }
        frame_state.last_acquire_signal_point = signal_point;
        frame_state.last_acquire_signal_queue = vk->queues.transfer_queue;
        frame_state.gpu_submits++;
        return 0;
    }

    VkCommandBufferSubmitInfo cmd_submit = {
        .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = frame->cmd_buf,
    };
    VkSemaphoreSubmitInfo waits[2];
    uint32_t wait_count = 0u;
    if (has_wait) {
        waits[wait_count++] = (VkSemaphoreSubmitInfo){
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = acquire,
            .value     = wait_point,
            .stageMask = wait_stage,
        };
    }
    if (frame_state.last_acquire_signal_point != 0u &&
        frame_state.last_acquire_signal_point < signal_point &&
        frame_state.last_acquire_signal_queue != vk->queues.graphics_queue &&
        (!has_wait || frame_state.last_acquire_signal_point > wait_point)) {
        waits[wait_count++] = (VkSemaphoreSubmitInfo){
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = acquire,
            .value     = frame_state.last_acquire_signal_point,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
    }
    VkSemaphoreSubmitInfo signal_infos[2] = {
        {
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = acquire,
            .value     = signal_point,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        },
        {
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = frame_state.acquire_sync,
            .value     = 0,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        },
    };
    VkSubmitInfo2 submit_info = {
        .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount   = wait_count,
        .pWaitSemaphoreInfos      = wait_count > 0u ? waits : nullptr,
        .commandBufferInfoCount   = 1u,
        .pCommandBufferInfos      = &cmd_submit,
        .signalSemaphoreInfoCount = 2u,
        .pSignalSemaphoreInfos    = signal_infos,
    };
    VkResult res = vkQueueSubmit2(vk->queues.graphics_queue, 1u, &submit_info, VK_NULL_HANDLE);
    if (res != VK_SUCCESS) {
        if (res == VK_ERROR_DEVICE_LOST) {
            vulkan_report_device_fault(vk, "vkQueueSubmit2 (graphics)");
        }
        fprintf(stderr, "[Engine Error] vkQueueSubmit2 failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }
    frame_state.last_acquire_signal_point = signal_point;
    frame_state.last_acquire_signal_queue = vk->queues.graphics_queue;
    frame_state.gpu_submits++;
    return 0;
}
