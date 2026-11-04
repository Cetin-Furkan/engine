#include "frame/frame_int.h"

// The two scene images and the wl_buffer imported for each. Orphaned
// releases live in orphan.c. The first 64 bytes of a slot are the gate
// the turn polls; the DMA-BUF image starts on the next line.

int ensure_prime_color(VulkanContext *vk, FrameSlot *frame) {
    if (frame->prime_view != VK_NULL_HANDLE) {
        return 0;
    }
    VkImageCreateInfo image_info = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_B8G8R8A8_SRGB,
        .extent        = { .width = frame->image.width, .height = frame->image.height, .depth = 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image = VK_NULL_HANDLE;
    VkResult res = vkCreateImage(vk->device, &image_info, nullptr, &image);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Engine Error] Cross-device color target failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }
    VkImageMemoryRequirementsInfo2 req_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = image,
    };
    VkMemoryRequirements2 reqs2 = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
    };
    vkGetImageMemoryRequirements2(vk->device, &req_info, &reqs2);
    uint32_t type_index = 0;
    if (vulkan_find_memory_type(vk, reqs2.memoryRequirements.memoryTypeBits,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_index) != VULKAN_SUCCESS) {
        fprintf(stderr, "[Engine Error] Cross-device color target has no device-local memory\n");
        vkDestroyImage(vk->device, image, nullptr);
        return -1;
    }
    VkMemoryAllocateInfo alloc = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = reqs2.memoryRequirements.size,
        .memoryTypeIndex = type_index,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    res = vkAllocateMemory(vk->device, &alloc, nullptr, &memory);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Engine Error] Cross-device color memory failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        vkDestroyImage(vk->device, image, nullptr);
        return -1;
    }
    VkBindImageMemoryInfo bind = {
        .sType  = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
        .image  = image,
        .memory = memory,
    };
    res = vkBindImageMemory2(vk->device, 1, &bind);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Engine Error] Cross-device color bind failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        vkFreeMemory(vk->device, memory, nullptr);
        vkDestroyImage(vk->device, image, nullptr);
        return -1;
    }
    VkImageViewCreateInfo view_info = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = VK_FORMAT_B8G8R8A8_SRGB,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };
    VkImageView view = VK_NULL_HANDLE;
    res = vkCreateImageView(vk->device, &view_info, nullptr, &view);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Engine Error] Cross-device color view failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        vkFreeMemory(vk->device, memory, nullptr);
        vkDestroyImage(vk->device, image, nullptr);
        return -1;
    }
    frame->prime_image = image;
    frame->prime_memory = memory;
    frame->prime_view = view;
    return 0;
}

void destroy_prime_color(VulkanContext *vk, FrameSlot *frame) {
    if (frame == nullptr) {
        return;
    }
    if (frame->image_view != VK_NULL_HANDLE && frame->image_view == frame->prime_view) {
        frame->image_view = VK_NULL_HANDLE;
    }
    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        if (frame->prime_view != VK_NULL_HANDLE) {
            vkDestroyImageView(vk->device, frame->prime_view, nullptr);
        }
        if (frame->prime_image != VK_NULL_HANDLE) {
            vkDestroyImage(vk->device, frame->prime_image, nullptr);
        }
        if (frame->prime_memory != VK_NULL_HANDLE) {
            vkFreeMemory(vk->device, frame->prime_memory, nullptr);
        }
    }
    frame->prime_view = VK_NULL_HANDLE;
    frame->prime_image = VK_NULL_HANDLE;
    frame->prime_memory = VK_NULL_HANDLE;
}

int ensure_frame_targets(VulkanContext *vk, FrameSlot *frame) {
    if (frame->image_view != VK_NULL_HANDLE) {
        return 0;
    }
    if (frame->image.prime_copy) {
        if (ensure_prime_color(vk, frame) != 0) {
            return -1;
        }
        frame->image_view = frame->prime_view;
        frame->view_format = VK_FORMAT_B8G8R8A8_SRGB;
        if (engine_app()->create_targets(vk, &frame->targets,
                                         frame->image.width, frame->image.height) != 0) {
            return -1;
        }
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

    if (view_format == VK_FORMAT_B8G8R8A8_SRGB &&
        frame->image.format == VK_FORMAT_B8G8R8A8_UNORM &&
        !frame->image.mutable_bgra8) {
        fprintf(stderr,
                "[Engine Error] DMA-BUF image is UNORM without a mutable sRGB view. Allocation should have rejected it.\n");
        return -1;
    }
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
        destroy_prime_color(vk, frame);
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
    frame->prime_image = VK_NULL_HANDLE;
    frame->prime_memory = VK_NULL_HANDLE;
    frame->prime_view = VK_NULL_HANDLE;
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
    frame->prime_image   = VK_NULL_HANDLE;
    frame->prime_memory  = VK_NULL_HANDLE;
    frame->prime_view    = VK_NULL_HANDLE;
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
            frame->prime_image != VK_NULL_HANDLE ||
            frame->image.dmabuf_fd >= 0);
}


static bool export_view_is_legal(const VulkanExportableImage *image, VkImageUsageFlags usage) {
    if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
        return true;
    }
    if (image->format == VK_FORMAT_B8G8R8A8_SRGB) {
        return true;
    }
    return image->format == VK_FORMAT_B8G8R8A8_UNORM && image->mutable_bgra8;
}

static int try_export(
    VulkanContext         *vk,
    uint32_t               width,
    uint32_t               height,
    VkFormat               format,
    VkImageUsageFlags      usage,
    const uint64_t        *mods,
    uint32_t               mod_count,
    bool                   foreign_read,
    VulkanExportableImage *out
) {
    if (mods == nullptr || mod_count == 0) {
        return -1;
    }
    VulkanResult img_res = vulkan_create_exportable_image_with_modifiers(
        vk, width, height, format, usage, mods, mod_count, foreign_read, out);
    if (img_res != VULKAN_SUCCESS || !out->has_drm_format_modifier) {
        if (out->image != VK_NULL_HANDLE || out->dmabuf_fd >= 0) {
            vulkan_destroy_exportable_image(vk, out);
        }
        return -1;
    }
    if (!export_view_is_legal(out, usage)) {
        vulkan_destroy_exportable_image(vk, out);
        return -1;
    }
    return 0;
}

static void keep_better_export(VulkanContext *vk, VulkanExportableImage *best, bool *have,
                               VulkanExportableImage *got) {
    const bool got_tiled = got->drm_format_modifier != 0;
    const bool best_linear = *have && best->drm_format_modifier == 0;
    if (!*have || (best_linear && got_tiled)) {
        if (*have) {
            vulkan_destroy_exportable_image(vk, best);
        }
        *best = *got;
        *have = true;
        *got = (VulkanExportableImage){ .dmabuf_fd = -1 };
        return;
    }
    vulkan_destroy_exportable_image(vk, got);
}

static int alloc_modifier_list(
    VulkanContext         *vk,
    uint32_t               width,
    uint32_t               height,
    VkImageUsageFlags      usage,
    const uint64_t        *mods,
    uint32_t               mod_count,
    bool                   prime_copy,
    bool                   foreign_read,
    VulkanExportableImage *out
) {
    const VkFormat formats[2] = {
        VK_FORMAT_B8G8R8A8_SRGB,
        VK_FORMAT_B8G8R8A8_UNORM,
    };
    VulkanExportableImage best = { .dmabuf_fd = -1 };
    bool have = false;
    for (uint32_t i = 0; i < 2; ++i) {
        VulkanExportableImage got = { .dmabuf_fd = -1 };
        if (try_export(vk, width, height, formats[i], usage, mods, mod_count, foreign_read, &got) != 0) {
            continue;
        }
        got.prime_copy = prime_copy;
        keep_better_export(vk, &best, &have, &got);
        if (best.drm_format_modifier != 0) {
            break;
        }
    }
    if (!have) {
        return -1;
    }
    *out = best;
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

    if (wl->feedback_cross_device) {
        // Draw in device-local memory and copy into a buffer the display GPU
        // can import. A color attachment on that buffer is the fallback when
        // the driver will not export a transfer-only image.
        if (alloc_modifier_list(vk, width, height, VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                wl->all_modifiers, wl->all_modifier_count,
                                true, true, out) == 0) {
            *tranche_kind = "cross-device";
        } else if (alloc_modifier_list(vk, width, height, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                       wl->all_modifiers, wl->all_modifier_count,
                                       false, true, out) == 0) {
            *tranche_kind = "cross-device";
        }
    } else if (wl->scanout_modifier_count > 0 &&
               alloc_modifier_list(vk, width, height, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                   wl->scanout_modifiers, wl->scanout_modifier_count,
                                   false, false, out) == 0) {
        *tranche_kind = "scanout";
        if (out->drm_format_modifier == 0) {
            VulkanExportableImage tiled = { .dmabuf_fd = -1 };
            if (alloc_modifier_list(vk, width, height, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                    wl->all_modifiers, wl->all_modifier_count,
                                    false, false, &tiled) == 0 &&
                tiled.drm_format_modifier != 0) {
                vulkan_destroy_exportable_image(vk, out);
                *out = tiled;
                *tranche_kind = "general";
            } else if (tiled.image != VK_NULL_HANDLE || tiled.dmabuf_fd >= 0) {
                vulkan_destroy_exportable_image(vk, &tiled);
            }
        }
    } else if (alloc_modifier_list(vk, width, height, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                   wl->all_modifiers, wl->all_modifier_count,
                                   false, false, out) == 0) {
        *tranche_kind = "general";
    }

    if (out->image == VK_NULL_HANDLE || !out->has_drm_format_modifier) {
        fprintf(stderr, "[Engine Error] Failed to allocate modifier-backed DMA-BUF image\n");
        if (out->dmabuf_fd >= 0 || out->image != VK_NULL_HANDLE) {
            vulkan_destroy_exportable_image(vk, out);
        }
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
