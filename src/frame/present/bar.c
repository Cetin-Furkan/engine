#include "frame/frame_int.h"

// The top bar is a desynced wl_subsurface with its own DMA-BUF. A scene
// present does not record it, and a bar present does not record the scene.

static FrameSlot *bar_shown(void) {
    return &frame_state.bar_slot[frame_state.bar_front & 1];
}

static FrameSlot *bar_spare(void) {
    return &frame_state.bar_slot[(frame_state.bar_front ^ 1) & 1];
}

void frame_apply_layout(WaylandContext *wl, uint32_t window_w, uint32_t window_h,
                        uint32_t *content_w, uint32_t *content_h) {
    uint32_t top = 0;
    uint32_t bottom = 0;
    if (wl == nullptr || wl->decoration_mode != ENG_DECOR_SERVER) {
        chrome_band_thickness(&top, &bottom);
    }
    if (bottom > 0) {
        static bool noted = false;
        if (!noted) {
            noted = true;
            fprintf(stderr, "[Chrome] A bottom layer has no subsurface yet, so it is not drawn.\n");
        }
    }
    if (window_w == 0) {
        window_w = 1;
    }
    if (window_h <= top) {
        window_h = top + 1u;
    }
    frame_state.window_w = window_w;
    frame_state.window_h = window_h;
    frame_state.bar_px = top;
    uint32_t s120        = wayland_effective_scale_120(wl);
    uint32_t scene_log_w = window_w;
    uint32_t scene_log_h = window_h - top;
    uint32_t phys_win_w  = wayland_phys_px(scene_log_w, s120);
    uint32_t phys_scn_h  = wayland_phys_px(scene_log_h, s120);
    uint32_t phys_bar_h  = (top > 0u) ? wayland_phys_px(top, s120) : 0u;
    if (content_w != nullptr) {
        *content_w = phys_win_w;
    }
    if (content_h != nullptr) {
        *content_h = phys_scn_h;
    }
    int32_t gy = top > 0 ? -(int32_t)top : 0;
    if (wl != nullptr) {
        wl->scene_logical_w = scene_log_w;
        wl->scene_logical_h = scene_log_h;
        wl->scene_phys_w    = phys_win_w;
        wl->scene_phys_h    = phys_scn_h;
        wl->bar_logical_w   = (top > 0u) ? window_w : 0u;
        wl->bar_logical_h   = top;
        wl->bar_phys_w      = (top > 0u) ? phys_win_w : 0u;
        wl->bar_phys_h      = phys_bar_h;
        wayland_set_window_geom(wl, 0, gy, window_w, window_h);
    }
    chrome_set_buffer_size(window_w, window_h);
}

int bar_open_surface(WaylandContext *wl) {
    if (wl == nullptr || frame_state.bar_px == 0) {
        return 0;
    }
    return wayland_create_bar(wl, 0, -(int32_t)frame_state.bar_px);
}

static int make_bar_view(VulkanContext *vk, FrameSlot *frame) {
    if (frame->image_view != VK_NULL_HANDLE) {
        return 0;
    }
    if (frame->image.prime_copy) {
        if (ensure_prime_color(vk, frame) != 0) {
            return -1;
        }
        frame->image_view = frame->prime_view;
        frame->view_format = VK_FORMAT_B8G8R8A8_SRGB;
        return 0;
    }
    VkFormat view_format = (frame->image.format == VK_FORMAT_B8G8R8A8_UNORM)
        ? VK_FORMAT_B8G8R8A8_SRGB
        : frame->image.format;
    VkImageViewCreateInfo view_info = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = frame->image.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = view_format,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };
    VkResult res = vkCreateImageView(vk->device, &view_info, nullptr, &frame->image_view);
    if (res != VK_SUCCESS && view_format != frame->image.format) {
        view_info.format = frame->image.format;
        res = vkCreateImageView(vk->device, &view_info, nullptr, &frame->image_view);
    }
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Chrome Error] Bar image view failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }
    frame->view_format = view_info.format;
    return 0;
}

static int record_bar(VulkanContext *vk, FrameSlot *frame, uint64_t signal_point) {
    if (make_bar_view(vk, frame) != 0) {
        return -1;
    }
    frame->surface_damage = (FrameRect){ 0, 0, frame->image.width, frame->image.height };
    frame->copy_damage    = frame->surface_damage;
    VkResult res = VK_SUCCESS;
    if (frame->cmd_buf == VK_NULL_HANDLE) {
        VkCommandBufferAllocateInfo alloc_info = {
            .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool        = vk->cmd_pool,
            .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        res = vkAllocateCommandBuffers(vk->device, &alloc_info, &frame->cmd_buf);
        if (res != VK_SUCCESS) {
            fprintf(stderr, "[Chrome Error] Bar command buffer failed: %s (%d)\n",
                    vulkan_vkresult_to_str(res), (int)res);
            return -1;
        }
    } else {
        res = vkResetCommandBuffer(frame->cmd_buf, 0);
        if (res != VK_SUCCESS) {
            fprintf(stderr, "[Chrome Error] Bar command reset failed: %s (%d)\n",
                    vulkan_vkresult_to_str(res), (int)res);
            return -1;
        }
    }
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    res = vkBeginCommandBuffer(frame->cmd_buf, &begin_info);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Chrome Error] Bar command begin failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }

    const bool unified_color = vk->device_info.supports_unified_image_layouts &&
                               (frame->prime_image != VK_NULL_HANDLE);
    VkImage color_image = frame->prime_image != VK_NULL_HANDLE ? frame->prime_image : frame->image.image;
    VkImageMemoryBarrier2 to_color;
    frame_fill_discard_color(&to_color, color_image, unified_color);
    frame_cmd_barriers(frame->cmd_buf, &to_color, 1);

    const bool srgb_view = (frame->view_format == VK_FORMAT_B8G8R8A8_SRGB);
    VkRenderingAttachmentInfo color = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = frame->image_view,
        .imageLayout = unified_color ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue  = {
            .color = {
                .float32 = {
                    srgb_view ? 0.0116f : 0.110f,
                    srgb_view ? 0.0130f : 0.118f,
                    srgb_view ? 0.0160f : 0.133f,
                    1.0f,
                },
            },
        },
    };
    VkRenderingInfo rendering = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = {
            .extent = { .width = frame->image.width, .height = frame->image.height },
        },
        .layerCount           = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &color,
    };
    vkCmdBeginRendering(frame->cmd_buf, &rendering);
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
    chrome_gpu_draw(vk, frame->cmd_buf,
                    frame_state.window_w, frame_state.window_h,
                    0, 0, frame->image.width, frame->image.height,
                    frame->view_format, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT);
    vkCmdEndRendering(frame->cmd_buf);

    frame_cmd_finish_present(frame->cmd_buf, vk, frame);
    res = vkEndCommandBuffer(frame->cmd_buf);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Chrome Error] Bar command end failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }

    // The bar signals the shared acquire timeline at its own point and does
    // not wait on the previous point on the graphics queue. When async PRIME
    // transfer is active, frame_submit_slot_commands preserves strict timeline
    // signal order across queues.
    bool async_prime = frame_use_async_prime_transfer(vk, frame);
    if (frame_submit_slot_commands(vk, frame, frame_state.acquire, signal_point,
                                   0u, VK_PIPELINE_STAGE_2_NONE) != 0) {
        return -1;
    }
    frame->acquire_point = signal_point;
    frame_state.sync_point = signal_point;
    frame->foreign_owned = true;
    VkSemaphore *sync_sem = async_prime ? &frame_state.prime_acquire_sync : &frame_state.acquire_sync;
    if (publish_acquire_point_from(vk, sync_sem, signal_point, frame->image.dmabuf_fd) < 0) {
        (void)wait_acquire_point(vk, signal_point);
        return -1;
    }
    return 0;
}

static int import_bar_buffer(WaylandContext *wl, FrameSlot *frame) {
    uint32_t saved  = wl->buffer_id;
    uint32_t buffer = 0;
    uint32_t fourcc = wayland_drm_fourcc_for_modifier(wl, frame->image.drm_format_modifier, false);
    int imported = wayland_import_dmabuf_immed(
        wl,
        frame->image.dmabuf_fd,
        frame->image.width,
        frame->image.height,
        (uint32_t)frame->image.row_pitch,
        (uint32_t)frame->image.offset,
        fourcc,
        frame->image.drm_format_modifier,
        &buffer);
    wl->buffer_id = saved;
    if (imported < 0) {
        return -1;
    }
    frame->wl_buffer_id = buffer;
    return 0;
}

static void arm_slot(WaylandContext *wl, const FrameSlot *frame) {
    wayland_arm_bar(wl, frame->wl_buffer_id, frame->image.width, frame->image.height,
                    wl->acquire_timeline_id, frame->acquire_point,
                    frame->release_timeline_id, frame->release_point);
}

int bar_record_first(WaylandContext *wl, VulkanContext *vk) {
    if (wl == nullptr || vk == nullptr || frame_state.bar_px == 0 || wl->bar_surface_id == 0) {
        return 0;
    }
    FrameSlot *bar = bar_shown();
    if (create_release_timeline(wl, bar) != 0) {
        return -1;
    }
    uint32_t bw = (wl->bar_phys_w != 0u) ? wl->bar_phys_w : frame_state.window_w;
    uint32_t bh = (wl->bar_phys_h != 0u) ? wl->bar_phys_h : frame_state.bar_px;
    const char *tranche = "general";
    if (alloc_content_image(vk, wl, bw, bh,
                            &bar->image, &tranche) != 0) {
        return -1;
    }
    uint64_t next = frame_state.sync_point + 1u;
    if (record_bar(vk, bar, next) != 0) {
        (void)wait_acquire_point(vk, bar->acquire_point);
        reap_frame_resources(wl, vk, bar);
        return -1;
    }
    if (import_bar_buffer(wl, bar) != 0) {
        (void)wait_acquire_point(vk, bar->acquire_point);
        reap_frame_resources(wl, vk, bar);
        return -1;
    }
    arm_slot(wl, bar);
    frame_state.bar_live = true;
    printf("[Chrome] Bar surface %ux%u, scene buffer excludes it.\n",
           bar->image.width, bar->image.height);
    fflush(stdout);
    return 0;
}

void bar_note_presented(void) {
    if (!frame_state.bar_live) {
        return;
    }
    bar_shown()->release_pending = true;
}

int bar_prepare(WaylandContext *wl, VulkanContext *vk, bool force) {
    if (wl == nullptr || vk == nullptr || frame_state.bar_px == 0) {
        return 0;
    }
    if (wl->bar_surface_id == 0 && bar_open_surface(wl) != 0) {
        return -1;
    }
    uint32_t bw = (wl->bar_phys_w != 0u) ? wl->bar_phys_w : frame_state.window_w;
    uint32_t bh = (wl->bar_phys_h != 0u) ? wl->bar_phys_h : frame_state.bar_px;
    FrameSlot *shown = bar_shown();
    bool size_changed = !frame_state.bar_live ||
                        shown->image.width != bw ||
                        shown->image.height != bh;
    if (!force && !size_changed && !frame_state.bar_dirty) {
        return 0;
    }
    if (frame_state.bar_swap_on_send) {
        return -EBUSY;
    }
    FrameSlot *dst = bar_spare();
    // Resetting the command buffer while its submit is pending is invalid.
    // On a resize, retire_pixels can orphan a compositor-held bar buffer as
    // soon as the GPU acquire point has retired.
    if (!acquire_point_retired(vk, dst->acquire_point)) {
        frame_state.bar_dirty = true;
        return -EAGAIN;
    }
    bool released = !dst->release_pending ||
                    poll_slot_released(wl, dst);
    if (!released && !size_changed) {
        frame_state.bar_dirty = true;
        return -EAGAIN;
    }
    if (slot_has_pixels(dst)) {
        if (retire_pixels(wl, vk, dst) != 0) {
            return -1;
        }
    }
    if (!frame_state.implicit_dmabuf_sync &&
        dst->release_handle == 0 && create_release_timeline(wl, dst) != 0) {
        return -1;
    }
    if (frame_state.implicit_dmabuf_sync && dst->release_point == 0u) {
        dst->release_point = 1u;
    }
    const char *tranche = "general";
    if (alloc_content_image(vk, wl, bw, bh,
                            &dst->image, &tranche) != 0) {
        return -1;
    }
    uint64_t next = frame_state.sync_point + 1u;
    if (record_bar(vk, dst, next) != 0) {
        (void)retire_pixels(wl, vk, dst);
        return -1;
    }
    if (import_bar_buffer(wl, dst) != 0) {
        (void)retire_pixels(wl, vk, dst);
        return -1;
    }
    arm_slot(wl, dst);
    frame_state.bar_swap_on_send = true;
    frame_state.bar_dirty = false;
    return 1;
}

void bar_drop_unsent(WaylandContext *wl, VulkanContext *vk) {
    if (wl != nullptr) {
        wayland_arm_bar(wl, 0, 0, 0, 0, 0, 0, 0);
    }
    if (!frame_state.bar_swap_on_send) {
        return;
    }
    frame_state.bar_swap_on_send = false;
    frame_state.bar_dirty = true;
    FrameSlot *dst = bar_spare();
    dst->release_pending = false;
    // Drop the unsent image and keep this slot's release timeline. The next
    // prepare reuses it. Reaping here destroyed that syncobj while the other
    // bar slot, or an orphan of this one, still named it.
    (void)retire_pixels(wl, vk, dst);
}

void bar_install(void) {
    if (!frame_state.bar_swap_on_send) {
        return;
    }
    frame_state.bar_swap_on_send = false;
    FrameSlot *now = bar_spare();
    now->release_pending = true;
    frame_state.bar_front ^= 1;
    frame_state.bar_live = true;
}

void bar_shutdown(WaylandContext *wl, VulkanContext *vk) {
    for (int i = 0; i < 2; ++i) {
        FrameSlot *slot = &frame_state.bar_slot[i];
        bool held = slot->release_pending &&
                    !poll_slot_released(wl, slot);
        if (held) {
            (void)wait_own_release(slot, 1000000000ull);
            held = !poll_slot_released(wl, slot);
        }
        if (held) {
            fprintf(stderr,
                    "[Chrome] Bar release point %llu timed out on shutdown; reclaiming GPU handles.\n",
                    (unsigned long long)slot->release_point);
            if (slot->release_eventfd >= 0) {
                close(slot->release_eventfd);
                slot->release_eventfd = -1;
            }
            (void)wait_acquire_point(vk, slot->acquire_point);
            destroy_frame_gpu(vk, slot);
            slot->release_pending = false;
            continue;
        }
        (void)wait_acquire_point(vk, slot->acquire_point);
        reap_frame_resources(wl, vk, slot);
    }
}
