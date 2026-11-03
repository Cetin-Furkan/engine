#include "frame/frame_int.h"

// The two scene images and the wl_buffer imported for each. Orphaned
// releases live in orphan.c. The first 64 bytes of a slot are the gate
// the turn polls; the DMA-BUF image starts on the next line.

int ensure_frame_targets(VulkanContext *vk, FrameSlot *frame) {
    if (frame->image_view != VK_NULL_HANDLE) {
        return 0;
    }
    VkFormat view_format = (frame->image.format == VK_FORMAT_B8G8R8A8_UNORM)
        ? VK_FORMAT_B8G8R8A8_SRGB
        : frame->image.format;
    VkImageViewCreateInfo view_info = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext    = nullptr,
        .flags    = 0,
        .image    = frame->image.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = view_format,
        .components = {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
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
        fprintf(stderr,
                "[Engine Error] DMA-BUF format %d rejected an sRGB view: %s (%d). The cube and faces are drawn only into that view.\n",
                (int)frame->image.format, vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Engine Error] vkCreateImageView failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }
    frame->view_format = view_info.format;
    if (view_info.format == VK_FORMAT_B8G8R8A8_SRGB &&
        engine_app()->create_targets(vk, &frame->targets,
                                     frame->image.width, frame->image.height) != 0) {
        return -1;
    }
    return 0;
}

void destroy_frame_gpu(VulkanContext *vk, FrameSlot *frame) {
    if (frame == nullptr) {
        return;
    }
    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        if (!wait_acquire_point(vk, frame->acquire_point)) {
            fprintf(stderr,
                    "[Engine Error] Image left alive. Acquire point %llu is still pending.\n",
                    (unsigned long long)frame->acquire_point);
            return;
        }
        if (frame->cmd_buf != VK_NULL_HANDLE && vk->cmd_pool != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &frame->cmd_buf);
        }
        if (frame->image_view != VK_NULL_HANDLE) {
            vkDestroyImageView(vk->device, frame->image_view, nullptr);
        }
        engine_app()->destroy_targets(vk, &frame->targets);
        vulkan_destroy_exportable_image(vk, &frame->image);
    } else if (frame->image.dmabuf_fd >= 0) {
        close(frame->image.dmabuf_fd);
        frame->image.dmabuf_fd = -1;
    }
    frame->cmd_buf = VK_NULL_HANDLE;
    frame->image_view = VK_NULL_HANDLE;
    frame->view_format = VK_FORMAT_UNDEFINED;
    frame->acquire_point = 0;
    frame->ts_armed = false;
}


int create_release_timeline(VulkanContext *vk, WaylandContext *wl, FrameSlot *frame) {
    struct drm_syncobj_create release_create = {0};
    if (frame_state.drm_fd < 0 || frame == nullptr) {
        return -EINVAL;
    }
    if (ioctl(frame_state.drm_fd, DRM_IOCTL_SYNCOBJ_CREATE, &release_create) != 0) {
        fprintf(stderr, "[Engine Error] DRM_IOCTL_SYNCOBJ_CREATE: %s\n", strerror(errno));
        return -1;
    }
    frame->release_handle = release_create.handle;
    int release_fd = drm_syncobj_to_fd(frame_state.drm_fd, frame->release_handle);
    if (release_fd < 0) {
        drm_syncobj_destroy(frame_state.drm_fd, &frame->release_handle);
        return -1;
    }
    if (import_release_semaphore_fd(vk, release_fd, &frame->release_semaphore) < 0) {
        close(release_fd);
        drm_syncobj_destroy(frame_state.drm_fd, &frame->release_handle);
        return -1;
    }
    uint32_t timeline_id = 0;
    int imported = wayland_import_syncobj_timeline(wl, release_fd, &timeline_id);
    close(release_fd);
    if (imported < 0) {
        if (timeline_id != 0 && wl != nullptr && wl->is_initialized && !wl->failed) {
            if (wayland_put_raw(wl, timeline_id, 0u, nullptr, 0u)) {
                (void)wayland_send_batch(wl);
            }
        }
        if (frame->release_semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(vk->device, frame->release_semaphore, nullptr);
            frame->release_semaphore = VK_NULL_HANDLE;
        }
        drm_syncobj_destroy(frame_state.drm_fd, &frame->release_handle);
        return -1;
    }
    frame->release_timeline_id = timeline_id;
    frame->release_point = 1;
    return 0;
}

void clear_pixels(FrameSlot *frame) {
    frame->image         = (VulkanExportableImage){ .dmabuf_fd = -1 };
    frame->image_view    = VK_NULL_HANDLE;
    frame->view_format   = VK_FORMAT_UNDEFINED;
    frame->targets       = (EngineTargets){0};
    frame->cmd_buf       = VK_NULL_HANDLE;
    frame->acquire_point = 0;
    frame->wl_buffer_id  = 0;
    frame->foreign_owned = false;
    frame->ts_armed      = false;
}

bool slot_has_pixels(const FrameSlot *frame) {
    return frame != nullptr &&
           (frame->image.image != VK_NULL_HANDLE ||
            frame->image_view != VK_NULL_HANDLE ||
            frame->cmd_buf != VK_NULL_HANDLE ||
            frame->wl_buffer_id != 0 ||
            frame->targets.depth_image != VK_NULL_HANDLE ||
            frame->targets.coverage_image != VK_NULL_HANDLE ||
            frame->image.dmabuf_fd >= 0);
}


int alloc_content_image(
    VulkanContext         *vk,
    WaylandContext        *wl,
    uint32_t               width,
    uint32_t               height,
    VulkanExportableImage *out,
    const char           **tranche_kind
) {
    *out = (VulkanExportableImage){ .dmabuf_fd = -1 };
    *tranche_kind = "general";
    VulkanResult img_res = VULKAN_ERROR_FORMAT_NOT_SUPPORTED;
    if (wl->scanout_modifier_count > 0) {
        img_res = vulkan_create_exportable_image_with_modifiers(
            vk,
            width,
            height,
            VK_FORMAT_B8G8R8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            wl->scanout_modifiers,
            wl->scanout_modifier_count,
            out);
        if (img_res != VULKAN_SUCCESS) {
            img_res = vulkan_create_exportable_image_with_modifiers(
                vk,
                width,
                height,
                VK_FORMAT_B8G8R8A8_SRGB,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                wl->scanout_modifiers,
                wl->scanout_modifier_count,
                out);
        }
        if (img_res == VULKAN_SUCCESS) {
            *tranche_kind = "scanout";
        }
    }

    if (img_res != VULKAN_SUCCESS) {
        img_res = vulkan_create_exportable_image_with_modifiers(
            vk,
            width,
            height,
            VK_FORMAT_B8G8R8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            wl->all_modifiers,
            wl->all_modifier_count,
            out);
        if (img_res != VULKAN_SUCCESS) {
            img_res = vulkan_create_exportable_image_with_modifiers(
                vk,
                width,
                height,
                VK_FORMAT_B8G8R8A8_SRGB,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                wl->all_modifiers,
                wl->all_modifier_count,
                out);
        }
        *tranche_kind = "general";
    }

    if (img_res != VULKAN_SUCCESS || !out->has_drm_format_modifier) {
        fprintf(stderr, "[Engine Error] Failed to allocate modifier-backed DMA-BUF image: %s\n",
                vulkan_result_to_str(img_res));
        return -1;
    }

    const uint64_t min_stride = (uint64_t)out->width * 4ull;
    if (out->row_pitch < min_stride ||
        out->row_pitch > 0xffffffffu ||
        out->offset > 0xffffffffu) {
        fprintf(stderr,
                "[Engine Error] DMA-BUF layout is not importable (stride=%llu offset=%llu width=%u)\n",
                (unsigned long long)out->row_pitch,
                (unsigned long long)out->offset,
                out->width);
        vulkan_destroy_exportable_image(vk, out);
        return -1;
    }
    return 0;
}


void blank_frame(FrameSlot *frame) {
    uint32_t generation = slot_next_generation(frame->generation);
    *frame = (FrameSlot){
        .image           = { .dmabuf_fd = -1 },
        .generation      = generation,
        .release_eventfd = -1,
    };
}

SlotRef slot_ref(const FrameSlot *slot) {
    if (slot == nullptr || slot->generation == 0) {
        return 0;
    }
    for (int i = 0; i < 2; ++i) {
        if (&frame_state.slot[i] == slot) {
            return slot_ref_pack(slot->generation, (uint32_t)(i + 1));
        }
    }
    uint32_t mask = frame_state.orphan_mask;
    while (mask != 0u) {
        uint32_t i = (uint32_t)__builtin_ctz(mask);
        mask &= mask - 1u;
        if (&frame_state.orphans[i] == slot) {
            return slot_ref_pack(slot->generation, i + 3u);
        }
    }
    return 0;
}

FrameSlot *slot_from_ref(SlotRef ref) {
    return slot_resolve(ref, frame_state.slot, frame_state.orphans, k_orphan_cap);
}


int alloc_spare(WaylandContext *wl, VulkanContext *vk) {
    if (shown()->image.width == 0 || shown()->image.height == 0) {
        return -EINVAL;
    }
    // The first spare imports one timeline. A later call at a new size keeps
    // that timeline and replaces the images.
    if (frame_state.spare_live &&
        spare()->image.image != VK_NULL_HANDLE &&
        spare()->image.width == shown()->image.width &&
        spare()->image.height == shown()->image.height) {
        return 0;
    }

    const bool keep_timeline = spare()->release_timeline_id != 0;
    if (!keep_timeline) {
        blank_frame(spare());
    } else if (slot_has_pixels(spare())) {
        if (retire_pixels(wl, vk, spare()) != 0) {
            return -1;
        }
    }

    const char *tranche_kind = "general";
    if (alloc_content_image(vk, wl, shown()->image.width, shown()->image.height,
                            &spare()->image, &tranche_kind) != 0) {
        if (!keep_timeline) {
            blank_frame(spare());
            frame_state.spare_live = false;
        }
        return -1;
    }
    (void)tranche_kind;
    if (!keep_timeline && create_release_timeline(vk, wl, spare()) != 0) {
        spare()->acquire_point = 0;
        destroy_frame_gpu(vk, spare());
        blank_frame(spare());
        frame_state.spare_live = false;
        return -1;
    }
    uint32_t front_buffer = wl->buffer_id;
    uint32_t spare_buffer = 0;
    int imported = wayland_import_dmabuf_immed(
        wl,
        spare()->image.dmabuf_fd,
        spare()->image.width,
        spare()->image.height,
        (uint32_t)spare()->image.row_pitch,
        (uint32_t)spare()->image.offset,
        ENG_DRM_FORMAT_ARGB8888,
        spare()->image.drm_format_modifier,
        &spare_buffer);
    wl->buffer_id = front_buffer;
    if (imported < 0) {
        if (keep_timeline) {
            FrameSlot junk = *spare();
            junk.release_eventfd = -1;
            junk.release_handle = 0;
            junk.release_semaphore = VK_NULL_HANDLE;
            junk.release_timeline_id = 0;
            junk.release_point = 0;
            clear_pixels(spare());
            reap_frame_resources(wl, vk, &junk);
        } else {
            reap_frame_resources(wl, vk, spare());
            blank_frame(spare());
            frame_state.spare_live = false;
        }
        return -1;
    }
    spare()->wl_buffer_id = spare_buffer;
    spare()->release_pending = false;
    frame_state.spare_live = true;
    return 0;
}

void swap_front_spare(void) {
    frame_state.front ^= 1;
    frame_state.spare_live = true;
}
