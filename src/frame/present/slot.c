#include "frame/frame_int.h"

// The two images, the orphan ring, and the wl_buffer on each.

static void destroy_frame_gpu(VulkanContext *vk, FrameSlot *frame);
static void destroy_wl_buffer_now(WaylandContext *wl, uint32_t buffer_id);
static bool slot_has_pixels(const FrameSlot *frame);
static bool same_release(const FrameSlot *a, const FrameSlot *b);
static bool release_held_elsewhere(const FrameSlot *frame);
static int stash_orphan(WaylandContext *wl, VulkanContext *vk, FrameSlot *frame);

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

static void destroy_frame_gpu(VulkanContext *vk, FrameSlot *frame) {
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
}

static void destroy_wl_buffer_now(WaylandContext *wl, uint32_t buffer_id) {
    if (wl == nullptr || !wl->is_initialized || wl->failed || buffer_id == 0) {
        return;
    }
    if (wl->send_len > 0) {
        (void)wayland_send_batch(wl);
    }
    if (!wl->failed && wayland_put_raw(wl, buffer_id, 0u, nullptr, 0u)) {
        (void)wayland_send_batch(wl);
    }
    if (wl->buffer_id == buffer_id) {
        wl->buffer_id = 0;
    }
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
}

static bool slot_has_pixels(const FrameSlot *frame) {
    return frame != nullptr &&
           (frame->image.image != VK_NULL_HANDLE ||
            frame->image_view != VK_NULL_HANDLE ||
            frame->cmd_buf != VK_NULL_HANDLE ||
            frame->wl_buffer_id != 0 ||
            frame->targets.depth_image != VK_NULL_HANDLE ||
            frame->targets.coverage_image != VK_NULL_HANDLE ||
            frame->image.dmabuf_fd >= 0);
}

static bool same_release(const FrameSlot *a, const FrameSlot *b) {
    if (a->release_semaphore != VK_NULL_HANDLE &&
        a->release_semaphore == b->release_semaphore) {
        return true;
    }
    if (a->release_timeline_id != 0 && a->release_timeline_id == b->release_timeline_id) {
        return true;
    }
    if (a->release_handle != 0 && a->release_handle == b->release_handle) {
        return true;
    }
    return false;
}

static bool release_held_elsewhere(const FrameSlot *frame) {
    for (int i = 0; i < 2; ++i) {
        if (&frame_state.slot[i] != frame && same_release(frame, &frame_state.slot[i])) {
            return true;
        }
    }
    uint32_t mask = frame_state.orphan_mask;
    while (mask != 0u) {
        uint32_t i = (uint32_t)__builtin_ctz(mask);
        mask &= mask - 1u;
        if (&frame_state.orphans[i] != frame && same_release(frame, &frame_state.orphans[i])) {
            return true;
        }
    }
    return false;
}

void reap_frame_resources(WaylandContext *wl, VulkanContext *vk, FrameSlot *frame) {
    if (frame == nullptr) {
        return;
    }
    if (frame->release_eventfd >= 0) {
        close(frame->release_eventfd);
        frame->release_eventfd = -1;
    }
    if (frame->wl_buffer_id != 0) {
        destroy_wl_buffer_now(wl, frame->wl_buffer_id);
        frame->wl_buffer_id = 0;
    }
    // Several copies poll one semaphore: the slot, and each resized image
    // still on screen. The last copy destroys the Wayland timeline, the DRM
    // syncobj, and the Vulkan semaphore.
    bool last_release = (frame->release_semaphore != VK_NULL_HANDLE ||
                         frame->release_timeline_id != 0 ||
                         frame->release_handle != 0) &&
                        !release_held_elsewhere(frame);
    if (last_release) {
        if (frame->release_timeline_id != 0 && wl != nullptr && wl->is_initialized && !wl->failed) {
            uint32_t id = frame->release_timeline_id;
            if (wl->send_len > 0) {
                (void)wayland_send_batch(wl);
            }
            if (!wl->failed && wayland_put_raw(wl, id, 0u, nullptr, 0u)) {
                (void)wayland_send_batch(wl);
            }
            if (wl->release_timeline_id == id) {
                wl->release_timeline_id = 0;
            }
        }
        drm_syncobj_destroy(frame_state.drm_fd, &frame->release_handle);
        if (vk != nullptr && vk->device != VK_NULL_HANDLE &&
            frame->release_semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(vk->device, frame->release_semaphore, nullptr);
        }
    }
    frame->release_timeline_id = 0;
    frame->release_handle = 0;
    frame->release_semaphore = VK_NULL_HANDLE;
    destroy_frame_gpu(vk, frame);
    frame->release_point = 0;
    frame->release_pending = false;
}

static void reap_orphan_slot(WaylandContext *wl, VulkanContext *vk, uint32_t idx) {
    FrameSlot *orb = &frame_state.orphans[idx];
    reap_frame_resources(wl, vk, orb);
    *orb = (FrameSlot){
        .image           = { .dmabuf_fd = -1 },
        .generation      = 0,
        .release_eventfd = -1,
    };
    frame_state.orphan_mask &= ~(1u << idx);
    frame_state.orphan_count = (uint32_t)__builtin_popcount(frame_state.orphan_mask);
}

static int insert_orphan(const FrameSlot *src) {
    uint32_t free_mask = (~frame_state.orphan_mask) & ((1u << k_orphan_cap) - 1u);
    if (free_mask == 0u) {
        return -1;
    }
    uint32_t idx = (uint32_t)__builtin_ctz(free_mask);
    uint32_t gen = slot_next_generation(frame_state.orphan_gen[idx]);
    if (src->generation >= gen) {
        gen = slot_next_generation(src->generation);
    }
    frame_state.orphan_gen[idx] = gen;
    frame_state.orphans[idx] = *src;
    frame_state.orphans[idx].generation = gen;
    frame_state.orphan_mask |= (1u << idx);
    frame_state.orphan_count = (uint32_t)__builtin_popcount(frame_state.orphan_mask);
    return (int)idx;
}

void sweep_ready_orphans(WaylandContext *wl, VulkanContext *vk, bool force_all) {
    uint32_t mask = frame_state.orphan_mask;
    while (mask != 0u) {
        uint32_t i = (uint32_t)__builtin_ctz(mask);
        mask &= mask - 1u;
        // Resolve through the generation-tagged handle: if this orphan
        // was replaced since the sweep started, the ref is stale and
        // the entry must not be reaped under its new image.
        FrameSlot *orb = slot_from_ref(slot_ref(&frame_state.orphans[i]));
        if (orb == nullptr) {
            continue;
        }
        bool ready = poll_release_semaphore(vk, orb->release_semaphore, orb->release_point);
        if (!ready && force_all) {
            (void)wait_own_release(vk, orb, 3000000000ull);
            ready = poll_release_semaphore(vk, orb->release_semaphore, orb->release_point);
            if (!ready) {
                fprintf(stderr,
                        "[Engine Error] Release point %llu was not signalled before shutdown\n",
                        (unsigned long long)orb->release_point);
            }
        }
        if (ready) {
            reap_orphan_slot(wl, vk, i);
        }
    }
}

int make_orphan_room(WaylandContext *wl, VulkanContext *vk) {
    sweep_ready_orphans(wl, vk, false);
    if (frame_state.orphan_count < k_orphan_cap) {
        return 0;
    }
    uint32_t mask = frame_state.orphan_mask;
    if (mask == 0u) {
        return 0;
    }
    uint32_t oldest_idx = (uint32_t)__builtin_ctz(mask);
    FrameSlot *oldest = &frame_state.orphans[oldest_idx];
    mask &= mask - 1u;
    while (mask != 0u) {
        uint32_t i = (uint32_t)__builtin_ctz(mask);
        mask &= mask - 1u;
        if (frame_state.orphans[i].acquire_point < oldest->acquire_point ||
            (frame_state.orphans[i].acquire_point == oldest->acquire_point &&
             frame_state.orphans[i].release_point < oldest->release_point)) {
            oldest_idx = i;
            oldest = &frame_state.orphans[i];
        }
    }
    (void)wait_own_release(vk, oldest, 500000000ull);
    if (!poll_release_semaphore(vk, oldest->release_semaphore, oldest->release_point)) {
        return -1;
    }
    reap_orphan_slot(wl, vk, oldest_idx);
    return 0;
}

static int stash_orphan(WaylandContext *wl, VulkanContext *vk, FrameSlot *frame) {
    if (frame == nullptr) {
        return 0;
    }
    if (make_orphan_room(wl, vk) != 0) {
        return -1;
    }
    // The copy keeps the release syncobj. The source must not destroy it.
    if (insert_orphan(frame) < 0) {
        return -1;
    }
    frame->release_handle = 0;
    frame->release_semaphore = VK_NULL_HANDLE;
    frame->release_timeline_id = 0;
    frame->release_point = 0;
    frame->release_eventfd = -1;
    frame->release_pending = false;
    clear_pixels(frame);
    return 0;
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

// Drop the images and the wl_buffer. The release syncobj stays on the slot.
// A buffer the compositor still holds is orphaned at the old point and polls
// the same semaphore. It must not destroy that semaphore. A pending release
// moves this slot's point forward by one.
int retire_pixels(WaylandContext *wl, VulkanContext *vk, FrameSlot *frame) {
    if (frame == nullptr) {
        return 0;
    }
    bool pending = frame->release_pending;
    uint64_t old_point = frame->release_point;
    if (!slot_has_pixels(frame)) {
        if (pending && old_point != 0) {
            frame->release_point = old_point + 1u;
        }
        frame->release_pending = false;
        return 0;
    }

    bool held = pending &&
                !poll_release_semaphore(vk, frame->release_semaphore, old_point);
    if (held && make_orphan_room(wl, vk) != 0) {
        return -1;
    }

    FrameSlot pixels = *frame;
    pixels.release_eventfd = -1;
    pixels.release_pending = false;

    clear_pixels(frame);
    frame->release_pending = false;
    if (pending && old_point != 0) {
        frame->release_point = old_point + 1u;
    }

    if (!held) {
        pixels.release_handle = 0;
        pixels.release_semaphore = VK_NULL_HANDLE;
        pixels.release_timeline_id = 0;
        pixels.release_point = 0;
        reap_frame_resources(wl, vk, &pixels);
        return 0;
    }

    pixels.release_point = old_point;
    if (insert_orphan(&pixels) < 0) {
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

void retire_spare(WaylandContext *wl, VulkanContext *vk) {
    if (!frame_state.spare_live) {
        return;
    }
    bool held = spare()->release_pending &&
                !poll_release_semaphore(vk, spare()->release_semaphore, spare()->release_point);
    if (held) {
        if (stash_orphan(wl, vk, spare()) != 0) {
            fprintf(stderr,
                    "[Engine Error] Spare image left alive. Eight release points are still held.\n");
            return;
        }
    } else {
        reap_frame_resources(wl, vk, spare());
    }
    blank_frame(spare());
    frame_state.spare_live = false;
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
