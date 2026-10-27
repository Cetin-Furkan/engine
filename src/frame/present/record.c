#include "frame/frame_int.h"

// One command buffer: world, clear, chrome, HUD, then the acquire signal.

// heat_dt is the callback interval. Zero draws the field as it stands.
int record_and_submit_clear(VulkanContext *vk, FrameSlot *frame, VkSemaphore acquire,
                                   uint64_t signal_point, bool draw_live, uint32_t heat_dt) {
    frame->acquire_point = signal_point;
    if (ensure_frame_targets(vk, frame) != 0) {
        return -1;
    }
    const bool srgb_view = frame->view_format == VK_FORMAT_B8G8R8A8_SRGB;
    VkResult res = VK_SUCCESS;

    if (frame->cmd_buf == VK_NULL_HANDLE) {
        VkCommandBufferAllocateInfo alloc_info = {
            .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext              = nullptr,
            .commandPool        = vk->cmd_pool,
            .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };

        res = vkAllocateCommandBuffers(vk->device, &alloc_info, &frame->cmd_buf);
        if (res != VK_SUCCESS) {
            fprintf(stderr, "[Engine Error] vkAllocateCommandBuffers failed: %s (%d)\n",
                    vulkan_vkresult_to_str(res), (int)res);
            return -1;
        }
    } else {
        res = vkResetCommandBuffer(frame->cmd_buf, 0);
        if (res != VK_SUCCESS) {
            fprintf(stderr, "[Engine Error] vkResetCommandBuffer failed: %s (%d)\n",
                    vulkan_vkresult_to_str(res), (int)res);
            return -1;
        }
    }

    VkCommandBufferBeginInfo begin_info = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext            = nullptr,
        .flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };

    res = vkBeginCommandBuffer(frame->cmd_buf, &begin_info);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Engine Error] vkBeginCommandBuffer failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }
    engine_app()->record_compute(frame->cmd_buf, heat_dt, draw_live);
    engine_app()->claim_frame(acquire, signal_point);

    // The image is cleared. oldLayout UNDEFINED discards the previous
    // contents, so this is not a queue-family acquire. The release at the
    // end of the buffer is the ownership handoff to the compositor, and
    // the release syncobj is what lets this record start.
    const bool depth_pass = srgb_view && frame->targets.depth_image != VK_NULL_HANDLE;
    VkImageMemoryBarrier2 barriers[3] = {0};
    uint32_t nbar = 0;
    barriers[nbar++] = (VkImageMemoryBarrier2){
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask       = VK_ACCESS_2_NONE,
        .dstStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = frame->image.image,
        .subresourceRange    = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };
    if (frame->targets.coverage_image != VK_NULL_HANDLE) {
        barriers[nbar] = barriers[0];
        barriers[nbar].image = frame->targets.coverage_image;
        nbar++;
    }
    if (depth_pass) {
        barriers[nbar++] = (VkImageMemoryBarrier2){
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask        = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            .srcAccessMask       = VK_ACCESS_2_NONE,
            .dstStageMask        = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
            .dstAccessMask       = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout           = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = frame->targets.depth_image,
            .subresourceRange    = {
                .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        };
    }
    VkDependencyInfo dep_to_attachment = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = nbar,
        .pImageMemoryBarriers    = barriers,
    };
    vkCmdPipelineBarrier2(frame->cmd_buf, &dep_to_attachment);
    if (depth_pass) {
        engine_app()->prepare_draw(frame->cmd_buf, frame->targets.depth_image);
    }

    VkRenderingAttachmentInfo color_attachment = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext       = nullptr,
        .imageView   = frame->image_view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue  = {
            .color = { .float32 = { 0.08f, 0.24f, 0.48f, 1.0f } },
        },
    };
    VkRenderingInfo rendering_info = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .pNext                = nullptr,
        .flags                = 0,
        .renderArea           = {
            .offset = { .x = 0, .y = 0 },
            .extent = { .width = frame->image.width, .height = frame->image.height },
        },
        .layerCount           = 1,
        .viewMask             = 0,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &color_attachment,
        .pDepthAttachment     = nullptr,
        .pStencilAttachment   = nullptr,
    };
    VkRenderingAttachmentInfo depth_attachment = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = frame->targets.depth_view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp     = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue  = { .depthStencil = { .depth = 1.0f, .stencil = 0 } },
    };
    if (depth_pass && frame->targets.depth_view != VK_NULL_HANDLE) {
        rendering_info.pDepthAttachment = &depth_attachment;
    }
    if (frame->targets.coverage_view != VK_NULL_HANDLE) {
        color_attachment.imageView = frame->targets.coverage_view;
        color_attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color_attachment.resolveImageView = frame->image_view;
        color_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }

    vkCmdBeginRendering(frame->cmd_buf, &rendering_info);
    VkViewport viewport = {
        .width    = (float)frame->image.width,
        .height   = (float)frame->image.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor = {
        .extent = { .width = frame->image.width, .height = frame->image.height },
    };
    vkCmdSetViewport(frame->cmd_buf, 0, 1, &viewport);
    vkCmdSetScissor(frame->cmd_buf, 0, 1, &scissor);
    if (rendering_info.pDepthAttachment != nullptr) {
        engine_app()->draw(frame->cmd_buf, frame->image.width, frame->image.height, draw_live);
    }
    // Chrome, the readout, and text are drawn in this pass. Sample shading
    // is off, so one fragment covers every sample and the resolve at
    // vkCmdEndRendering is the only color store.
    VkFormat overlay_depth = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits overlay_samples = VK_SAMPLE_COUNT_1_BIT;
    if (rendering_info.pDepthAttachment != nullptr) {
        overlay_depth = frame->targets.depth_format;
        overlay_samples = frame->targets.samples;
    }
    chrome_gpu_draw(vk, frame->cmd_buf, frame->image.width, frame->image.height,
                    frame->view_format, overlay_depth, overlay_samples);
    {
        WaylandContext *wl_hud = wayland_get_context();
        bool have_stamp = wl_hud != nullptr && wl_hud->frame_stamp_valid;
        engine_app()->draw_overlay(frame->cmd_buf, frame->image.width, frame->image.height,
                                   frame->view_format, overlay_depth, overlay_samples,
                                   have_stamp ? wl_hud->frame_time_ms : 0u, have_stamp,
                                   frame->image.drm_format_modifier);
    }
    vkCmdEndRendering(frame->cmd_buf);

    VkImageSubresourceRange color_range = {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };

    uint32_t dst_queue = vk->device_info.supports_queue_family_foreign
        ? VK_QUEUE_FAMILY_FOREIGN_EXT
        : VK_QUEUE_FAMILY_EXTERNAL;

    VkImageMemoryBarrier2 to_general = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext               = nullptr,
        .srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask        = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
        .dstAccessMask       = VK_ACCESS_2_NONE,
        .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = vk->queues.graphics_family_index,
        .dstQueueFamilyIndex = dst_queue,
        .image               = frame->image.image,
        .subresourceRange    = color_range,
    };
    VkDependencyInfo dep_to_general = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext                   = nullptr,
        .dependencyFlags         = 0,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &to_general,
    };
    vkCmdPipelineBarrier2(frame->cmd_buf, &dep_to_general);

    res = vkEndCommandBuffer(frame->cmd_buf);
    if (res != VK_SUCCESS) {
        engine_app()->release_frame(signal_point);
        engine_app()->record_abort();
        fprintf(stderr, "[Engine Error] vkEndCommandBuffer failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }

    VkCommandBufferSubmitInfo cmd_submit = {
        .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .pNext         = nullptr,
        .commandBuffer = frame->cmd_buf,
        .deviceMask    = 0,
    };
    // The field the shaders read was written by an earlier submit. That
    // submit signaled this acquire value at ALL_COMMANDS. Waiting it here
    // is what makes the write visible. The signal stage is the whole
    // command buffer, so a step waits at compute and a later draw waits
    // at vertex. The bake's seed stays at 0 because that queue was idled.
    uint64_t heat_point = engine_app()->record_published();
    bool wait_heat = heat_point != 0 && heat_point < signal_point;
    VkSemaphoreSubmitInfo wait_info = {
        .sType       = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .pNext       = nullptr,
        .semaphore   = acquire,
        .value       = heat_point,
        .stageMask   = engine_app()->record_step_open() ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                                : VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
        .deviceIndex = 0,
    };
    VkSemaphoreSubmitInfo signal_info = {
        .sType       = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .pNext       = nullptr,
        .semaphore   = acquire,
        .value       = signal_point,
        .stageMask   = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .deviceIndex = 0,
    };
    VkSubmitInfo2 submit_info = {
        .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .pNext                    = nullptr,
        .flags                    = 0,
        .waitSemaphoreInfoCount   = wait_heat ? 1u : 0u,
        .pWaitSemaphoreInfos      = wait_heat ? &wait_info : nullptr,
        .commandBufferInfoCount   = 1,
        .pCommandBufferInfos      = &cmd_submit,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos    = &signal_info,
    };

    res = vkQueueSubmit2(vk->queues.graphics_queue, 1, &submit_info, VK_NULL_HANDLE);
    if (res != VK_SUCCESS) {
        engine_app()->release_frame(signal_point);
        engine_app()->record_abort();
        fprintf(stderr, "[Engine Error] vkQueueSubmit2 failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }
    frame->foreign_owned = true;
    engine_app()->record_ok(signal_point);
    return 0;
}
