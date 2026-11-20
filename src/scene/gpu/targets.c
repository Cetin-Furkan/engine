#include "scene_gpu_int.h"
#include <stdio.h>

static void destroy_target_image(VulkanContext *vk, SceneTargetImage *img) {
    if (vk != nullptr && vk->device != VK_NULL_HANDLE && img != nullptr) {
        if (img->view != VK_NULL_HANDLE) {
            vkDestroyImageView(vk->device, img->view, nullptr);
        }
        if (img->image != VK_NULL_HANDLE) {
            vkDestroyImage(vk->device, img->image, nullptr);
        }
        if (img->memory != VK_NULL_HANDLE) {
            vkFreeMemory(vk->device, img->memory, nullptr);
        }
    }
    if (img != nullptr) {
        *img = (SceneTargetImage){0};
    }
}

static int create_target_image(VulkanContext *vk, uint32_t width, uint32_t height,
                               VkFormat format, VkImageUsageFlags usage,
                               VkImageAspectFlags aspect, SceneTargetImage *out) {
    destroy_target_image(vk, out);

    VkImageCreateInfo img_info = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = format,
        .extent        = { .width = width, .height = height, .depth = 1u },
        .mipLevels     = 1u,
        .arrayLayers   = 1u,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = usage,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(vk->device, &img_info, nullptr, &out->image) != VK_SUCCESS) {
        destroy_target_image(vk, out);
        return -1;
    }

    VkImageMemoryRequirementsInfo2 req_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = out->image,
    };
    VkMemoryRequirements2 reqs2 = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
    };
    vkGetImageMemoryRequirements2(vk->device, &req_info, &reqs2);
    uint32_t mem_type = 0u;
    if (vulkan_find_memory_type(vk, reqs2.memoryRequirements.memoryTypeBits,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mem_type) != VULKAN_SUCCESS) {
        destroy_target_image(vk, out);
        return -1;
    }

    VkMemoryPriorityAllocateInfoEXT prio_info = {0};
    VkMemoryAllocateInfo alloc_info = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = vulkan_chain_memory_priority(vk, &prio_info, 1.0f, nullptr),
        .allocationSize  = reqs2.memoryRequirements.size,
        .memoryTypeIndex = mem_type,
    };
    if (vkAllocateMemory(vk->device, &alloc_info, nullptr, &out->memory) != VK_SUCCESS) {
        destroy_target_image(vk, out);
        return -1;
    }
    VkBindImageMemoryInfo bind_info = {
        .sType        = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
        .image        = out->image,
        .memory       = out->memory,
        .memoryOffset = 0u,
    };
    if (vkBindImageMemory2(vk->device, 1u, &bind_info) != VK_SUCCESS) {
        destroy_target_image(vk, out);
        return -1;
    }

    VkImageViewCreateInfo view_info = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = out->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = format,
        .subresourceRange = {
            .aspectMask     = aspect,
            .baseMipLevel   = 0u,
            .levelCount     = 1u,
            .baseArrayLayer = 0u,
            .layerCount     = 1u,
        },
    };
    if (vkCreateImageView(vk->device, &view_info, nullptr, &out->view) != VK_SUCCESS) {
        destroy_target_image(vk, out);
        return -1;
    }

    out->width  = width;
    out->height = height;
    return 0;
}

void scene_targets_reap(VulkanContext *vk, bool force_all) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return;
    }
    for (uint32_t i = 0u; i < K_SCENE_RETIRE_CAP; ++i) {
        if (!g_scene.retired_res[i].active) {
            continue;
        }
        if (force_all || scene_point_retired(g_scene.retired_res[i].retire_point)) {
            destroy_target_image(vk, &g_scene.retired_res[i].img);
            da_buffer_destroy(vk, &g_scene.retired_res[i].buf);
            g_scene.retired_res[i] = (SceneRetiredRes){0};
        }
    }
}

void scene_targets_destroy(VulkanContext *vk) {
    scene_targets_reap(vk, true);
    destroy_target_image(vk, &g_scene.vis_depth);
    destroy_target_image(vk, &g_scene.vis_color);
    da_buffer_destroy(vk, &g_scene.hiz_buf);
}

static void retire_screen_res(VulkanContext *vk, SceneTargetImage *img, DaBuffer *buf, uint64_t pt) {
    if (vk == nullptr) {
        return;
    }
    const bool has_img = (img != nullptr && img->image != VK_NULL_HANDLE);
    const bool has_buf = (buf != nullptr && buf->buffer != VK_NULL_HANDLE);
    if (!has_img && !has_buf) {
        return;
    }
    scene_targets_reap(vk, false);

    uint32_t slot = K_SCENE_RETIRE_CAP;
    for (uint32_t i = 0u; i < K_SCENE_RETIRE_CAP; ++i) {
        if (!g_scene.retired_res[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == K_SCENE_RETIRE_CAP) {
        slot = 0u;
        if (g_scene.upload_sem != VK_NULL_HANDLE && g_scene.retired_res[0].retire_point > 0u) {
            VkSemaphoreWaitInfo wi = {
                .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                .semaphoreCount = 1u,
                .pSemaphores    = &g_scene.upload_sem,
                .pValues        = &g_scene.retired_res[0].retire_point,
            };
            (void)vkWaitSemaphores(vk->device, &wi, 3000000000ull);
        }
        destroy_target_image(vk, &g_scene.retired_res[0].img);
        da_buffer_destroy(vk, &g_scene.retired_res[0].buf);
    }

    SceneRetiredRes entry = { .retire_point = pt, .active = true };
    if (has_img) {
        entry.img = *img;
        *img = (SceneTargetImage){0};
    }
    if (has_buf) {
        entry.buf = *buf;
        *buf = (DaBuffer){0};
    }
    g_scene.retired_res[slot] = entry;
}

int scene_targets_ensure(uint32_t width, uint32_t height,
                         bool need_vis_color, bool need_hiz_buf) {
    if (g_scene.vk == nullptr || width == 0u || height == 0u) {
        return -1;
    }
    const bool depth_ok = g_scene.vis_depth.image != VK_NULL_HANDLE &&
                          g_scene.vis_depth.width == width &&
                          g_scene.vis_depth.height == height;
    const bool color_ok = !need_vis_color ||
                          (g_scene.vis_color.image != VK_NULL_HANDLE &&
                           g_scene.vis_color.width == width &&
                           g_scene.vis_color.height == height);
    const bool hiz_ok   = !need_hiz_buf ||
                          (g_scene.hiz_buf.buffer != VK_NULL_HANDLE &&
                           g_scene.hiz_hdr_cpu.full_width == width &&
                           g_scene.hiz_hdr_cpu.full_height == height);
    if (depth_ok && color_ok && hiz_ok) {
        return 0;
    }

    if (!depth_ok) {
        retire_screen_res(g_scene.vk, &g_scene.vis_depth, nullptr, g_scene.screen_last_pt);
        if (create_target_image(g_scene.vk, width, height, VK_FORMAT_D32_SFLOAT,
                                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT,
                                VK_IMAGE_ASPECT_DEPTH_BIT, &g_scene.vis_depth) != 0) {
            return -1;
        }
    }

    if (need_vis_color && !color_ok) {
        retire_screen_res(g_scene.vk, &g_scene.vis_color, nullptr, g_scene.screen_last_pt);
        if (create_target_image(g_scene.vk, width, height, VK_FORMAT_R32_UINT,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, &g_scene.vis_color) != 0) {
            return -1;
        }
    }

    if (!need_hiz_buf) {
        return 0;
    }

    SceneHizHeader hdr = {
        .full_width  = width,
        .full_height = height,
        .valid       = 0u,
    };
    uint32_t mw = (width + 1u) >> 1u;
    uint32_t mh = (height + 1u) >> 1u;
    uint32_t float_off = 0u;
    uint32_t mips = 0u;
    while (mips < K_SCENE_HIZ_MAX_MIPS) {
        hdr.mips[mips] = (SceneHizMipInfo){
            .offset_floats = float_off,
            .width         = mw,
            .height        = mh,
            .pad           = 0u,
        };
        float_off += mw * mh;
        mips++;
        if (mw == 1u && mh == 1u) {
            break;
        }
        mw = (mw + 1u) >> 1u;
        mh = (mh + 1u) >> 1u;
    }
    hdr.mip_count            = mips;
    g_scene.hiz_hdr_cpu      = hdr;
    g_scene.hiz_header_dirty = true;

    VkDeviceSize needed_bytes = 256ull + (VkDeviceSize)float_off * sizeof(float);
    needed_bytes = (needed_bytes + 255ull) & ~255ull;
    if (g_scene.hiz_buf.buffer == VK_NULL_HANDLE || g_scene.hiz_buf.bytes < needed_bytes) {
        retire_screen_res(g_scene.vk, nullptr, &g_scene.hiz_buf, g_scene.screen_last_pt);
        if (da_buffer_replace(g_scene.vk, &g_scene.hiz_buf, needed_bytes,
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              false) != 0) {
            return -1;
        }
    }
    return 0;
}

void scene_invalidate_hiz(void) {
    g_scene.hiz_hdr_cpu.valid = 0u;
    g_scene.hiz_header_dirty  = true;
}

bool scene_visbuf_active(void) {
    return g_scene.visbuf_enabled;
}

bool scene_toggle_visbuf(void) {
    if (!g_scene.ready || !g_scene.can_hiz_push) {
        return false;
    }
    g_scene.visbuf_enabled = !g_scene.visbuf_enabled;
    printf("[Scene] Visibility Buffer deferred shading: %s\n",
           g_scene.visbuf_enabled ? "ON (1x R32_UINT + Analytical Barycentrics + Specular AA)"
                                  : "OFF (Hardware MSAA Indirect Meshlets)");
    fflush(stdout);
    return true;
}
