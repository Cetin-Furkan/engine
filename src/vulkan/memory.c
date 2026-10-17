#include "vulkan/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

VulkanResult vulkan_find_memory_type(
    const VulkanContext   *ctx,
    uint32_t               type_filter,
    VkMemoryPropertyFlags  required_flags,
    uint32_t              *out_type_index
) {
    if (ctx == nullptr || out_type_index == nullptr) {
        return VULKAN_ERROR_INVALID_ARGUMENT;
    }

    const VkPhysicalDeviceMemoryProperties *mem_props = &ctx->device_info.memory_properties;
    int32_t best_index = -1;
    int best_rank = -1;
    VkDeviceSize best_heap_size = 0;
    const bool want_device_local = (required_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;

    for (uint32_t i = 0; i < mem_props->memoryTypeCount && i < 32; ++i) {
        if ((type_filter & (1u << i)) == 0) {
            continue;
        }

        VkMemoryPropertyFlags flags = mem_props->memoryTypes[i].propertyFlags;
        if ((flags & required_flags) != required_flags) {
            continue;
        }
        if ((flags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) != 0 &&
            (required_flags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) == 0) {
            continue;
        }
        if ((flags & VK_MEMORY_PROPERTY_PROTECTED_BIT) != 0 &&
            (required_flags & VK_MEMORY_PROPERTY_PROTECTED_BIT) == 0) {
            continue;
        }

        uint32_t heap_idx = mem_props->memoryTypes[i].heapIndex;
        if (heap_idx >= mem_props->memoryHeapCount) {
            continue;
        }

        // Rank 2: real device-local VRAM. Rank 1: host-visible ReBAR or UMA memory.
        const bool host_visible = (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
        int rank = (want_device_local && !host_visible) ? 2 : 1;
        VkDeviceSize heap_size = mem_props->memoryHeaps[heap_idx].size;

        if (best_index < 0 || rank > best_rank || (rank == best_rank && heap_size > best_heap_size)) {
            best_index = (int32_t)i;
            best_rank = rank;
            best_heap_size = heap_size;
        }
    }

    if (best_index >= 0) {
        *out_type_index = (uint32_t)best_index;
        return VULKAN_SUCCESS;
    }

    return VULKAN_ERROR_MEMORY_TYPE_NOT_FOUND;
}

static bool result_is_memory_error(VulkanResult result) {
    return result == VULKAN_ERROR_OUT_OF_HOST_MEMORY ||
           result == VULKAN_ERROR_OUT_OF_DEVICE_MEMORY;
}

static VulkanResult map_image_vkresult(VkResult result) {
    if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) {
        return VULKAN_ERROR_FORMAT_NOT_SUPPORTED;
    }
    VulkanResult mapped = vulkan_result_from_vkresult(result);
    return mapped == VULKAN_ERROR_UNKNOWN ? VULKAN_ERROR_INITIALIZATION_FAILED : mapped;
}

static bool usage_to_required_features(VkImageUsageFlags usage, VkFormatFeatureFlags *out_features) {
    if (usage == 0 || out_features == nullptr) {
        return false;
    }

    VkImageUsageFlags known = 0;
    VkFormatFeatureFlags features = 0;

    if ((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0) {
        known |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        features |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    }
    if ((usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0) {
        known |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        features |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    }
    if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0) {
        known |= VK_IMAGE_USAGE_SAMPLED_BIT;
        features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    }
    if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0) {
        known |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    }
    if ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
        known |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0) {
        known |= VK_IMAGE_USAGE_STORAGE_BIT;
        features |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    }

    if ((usage & ~known) != 0) {
        return false;
    }

    *out_features = features;
    return true;
}

static bool features_cover(VkFormatFeatureFlags have, VkFormatFeatureFlags required) {
    return (have & required) == required;
}

static bool modifier_list_contains(const uint64_t *list, uint32_t count, uint64_t modifier) {
    if (list == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (list[i] == modifier) {
            return true;
        }
    }
    return false;
}

static const char *image_tiling_name(VkImageTiling tiling) {
    switch (tiling) {
        case VK_IMAGE_TILING_LINEAR: return "linear";
        case VK_IMAGE_TILING_OPTIMAL: return "optimal";
        case VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT: return "drm-modifier";
        default: return "other";
    }
}

static void destroy_unexported_image(VkDevice device, VkImage image, VkDeviceMemory memory) {
    if (image != VK_NULL_HANDLE) {
        vkDestroyImage(device, image, nullptr);
    }
    if (memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, memory, nullptr);
    }
}

static VulkanResult image_is_exportable(
    const VulkanContext *ctx,
    VkFormat             format,
    VkImageUsageFlags    usage,
    VkImageTiling        tiling,
    bool                 with_modifier,
    uint64_t             modifier
) {
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info = {
        .sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .pNext             = nullptr,
        .drmFormatModifier = modifier,
        .sharingMode       = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkPhysicalDeviceExternalImageFormatInfo external_format = {
        .sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext      = with_modifier ? &modifier_info : nullptr,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkPhysicalDeviceImageFormatInfo2 format_info = {
        .sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext  = &external_format,
        .format = format,
        .type   = VK_IMAGE_TYPE_2D,
        .tiling = tiling,
        .usage  = usage,
        .flags  = 0,
    };
    VkExternalImageFormatProperties external_props = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 format_props2 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &external_props,
    };

    VkResult format_res = vkGetPhysicalDeviceImageFormatProperties2(
        ctx->physical_device, &format_info, &format_props2);
    if (format_res != VK_SUCCESS) {
        return map_image_vkresult(format_res);
    }

    const VkExternalMemoryProperties *export_props = &external_props.externalMemoryProperties;
    if ((export_props->externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0 ||
        (export_props->compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) == 0) {
        return VULKAN_ERROR_FORMAT_NOT_SUPPORTED;
    }
    return VULKAN_SUCCESS;
}

// DRM_FORMAT_MODIFIER_LINEAR is 0. Any other single-plane modifier is treated as tiled.
// When filter_modifiers != nullptr, only modifiers present in filter_modifiers are collected.
static VulkanResult collect_export_modifiers_filtered(
    const VulkanContext  *ctx,
    VkFormat              format,
    VkFormatFeatureFlags  required_features,
    VkImageUsageFlags     usage,
    const uint64_t       *filter_modifiers,
    uint32_t              filter_count,
    bool                  prefer_linear,
    uint64_t            **out_modifiers,
    uint32_t             *out_count
) {
    *out_modifiers = nullptr;
    *out_count = 0;

    VkDrmFormatModifierPropertiesListEXT list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 props2 = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &list,
    };
    vkGetPhysicalDeviceFormatProperties2(ctx->physical_device, format, &props2);

    uint32_t available = list.drmFormatModifierCount;
    if (available == 0) {
        return VULKAN_SUCCESS;
    }

    VkDrmFormatModifierPropertiesEXT *props = calloc(available, sizeof(*props));
    if (props == nullptr) {
        fprintf(stderr, "[Vulkan Error] Out of host memory while listing DRM format modifiers\n");
        return VULKAN_ERROR_OUT_OF_HOST_MEMORY;
    }

    list.drmFormatModifierCount = available;
    list.pDrmFormatModifierProperties = props;
    vkGetPhysicalDeviceFormatProperties2(ctx->physical_device, format, &props2);

    uint32_t written = list.drmFormatModifierCount;
    if (written > available) {
        written = available;
    }

    uint64_t *tiled = calloc(available, sizeof(*tiled));
    uint64_t *linear = calloc(available, sizeof(*linear));
    if (tiled == nullptr || linear == nullptr) {
        free(tiled);
        free(linear);
        free(props);
        fprintf(stderr, "[Vulkan Error] Out of host memory while selecting DRM format modifiers\n");
        return VULKAN_ERROR_OUT_OF_HOST_MEMORY;
    }

    uint32_t tiled_count = 0;
    uint32_t linear_count = 0;
    VulkanResult fatal = VULKAN_SUCCESS;
    for (uint32_t i = 0; i < written; ++i) {
        if (props[i].drmFormatModifierPlaneCount != 1) {
            continue;
        }
        if (filter_modifiers != nullptr &&
            !modifier_list_contains(filter_modifiers, filter_count, props[i].drmFormatModifier)) {
            continue;
        }
        if (!features_cover(props[i].drmFormatModifierTilingFeatures, required_features)) {
            continue;
        }

        VulkanResult exportable = image_is_exportable(
            ctx, format, usage, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            true, props[i].drmFormatModifier);
        if (result_is_memory_error(exportable)) {
            fatal = exportable;
            break;
        }
        if (exportable != VULKAN_SUCCESS) {
            continue;
        }

        if (props[i].drmFormatModifier == 0) {
            linear[linear_count++] = props[i].drmFormatModifier;
        } else {
            tiled[tiled_count++] = props[i].drmFormatModifier;
        }
    }

    if (fatal != VULKAN_SUCCESS) {
        free(tiled);
        free(linear);
        free(props);
        return fatal;
    }

    // List order is not a preference. On a cross-GPU handoff the other device
    // cannot read this GPU's tiled layout, so linear is the only entry offered.
    if (prefer_linear && linear_count > 0) {
        *out_modifiers = linear;
        *out_count = linear_count;
        free(tiled);
    } else if (tiled_count > 0) {
        *out_modifiers = tiled;
        *out_count = tiled_count;
        free(linear);
    } else if (linear_count > 0) {
        *out_modifiers = linear;
        *out_count = linear_count;
        free(tiled);
    } else {
        free(tiled);
        free(linear);
    }
    free(props);
    return VULKAN_SUCCESS;
}

static VulkanResult create_bound_export_image(
    VulkanContext         *ctx,
    uint32_t               width,
    uint32_t               height,
    VkFormat               format,
    VkImageUsageFlags      usage,
    VkImageTiling          tiling,
    const uint64_t        *modifiers,
    uint32_t               modifier_count,
    bool                   allow_shared_memory,
    VulkanExportableImage *out_image
) {
    const bool modifier_tiling = tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    if (modifier_tiling && (modifiers == nullptr || modifier_count == 0)) {
        return VULKAN_ERROR_INVALID_ARGUMENT;
    }

    VkImageDrmFormatModifierListCreateInfoEXT modifier_list = {
        .sType                  = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .pNext                  = nullptr,
        .drmFormatModifierCount = modifier_count,
        .pDrmFormatModifiers    = modifiers,
    };
    VkExternalMemoryImageCreateInfo ext_image_info = {
        .sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext       = modifier_tiling ? &modifier_list : nullptr,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo image_info = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext         = &ext_image_info,
        .flags         = 0,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = format,
        .extent        = {.width = width, .height = height, .depth = 1},
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = tiling,
        .usage         = usage,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkImage image = VK_NULL_HANDLE;
    VkResult img_res = vkCreateImage(ctx->device, &image_info, nullptr, &image);
    if (img_res != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan Error] vkCreateImage (%s) for exportable DMA-BUF failed: %s (%d)\n",
                image_tiling_name(tiling), vulkan_vkresult_to_str(img_res), (int)img_res);
        return map_image_vkresult(img_res);
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(ctx->device, image, &mem_reqs);

    uint32_t memory_type_index = 0;
    VulkanResult mem_type_res = VULKAN_ERROR_MEMORY_TYPE_NOT_FOUND;
    // A linear buffer read by another GPU is often placed in host-visible
    // memory. Same-GPU images stay on a device-local type only.
    if (allow_shared_memory) {
        mem_type_res = vulkan_find_memory_type(
            ctx, mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &memory_type_index);
    }
    if (mem_type_res != VULKAN_SUCCESS) {
        mem_type_res = vulkan_find_memory_type(
            ctx, mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_type_index);
    }
    if (allow_shared_memory && mem_type_res != VULKAN_SUCCESS) {
        mem_type_res = vulkan_find_memory_type(
            ctx, mem_reqs.memoryTypeBits, 0, &memory_type_index);
    }
    if (mem_type_res != VULKAN_SUCCESS) {
        fprintf(stderr, "[Vulkan Error] Could not find %s memory type for DMA-BUF image\n",
                allow_shared_memory ? "compatible" : "device-local");
        vkDestroyImage(ctx->device, image, nullptr);
        return mem_type_res;
    }

    // Dedicated allocation keeps the exported DMA-BUF at offset 0.
    VkExportMemoryAllocateInfo export_info = {
        .sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType  = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext  = &export_info,
        .image  = image,
        .buffer = VK_NULL_HANDLE,
    };
    VkMemoryAllocateInfo alloc_info = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = &dedicated_info,
        .allocationSize  = mem_reqs.size,
        .memoryTypeIndex = memory_type_index,
    };

    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult alloc_res = vkAllocateMemory(ctx->device, &alloc_info, nullptr, &memory);
    if (alloc_res != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan Error] vkAllocateMemory for DMA-BUF image failed: %s (%d)\n",
                vulkan_vkresult_to_str(alloc_res), (int)alloc_res);
        vkDestroyImage(ctx->device, image, nullptr);
        return map_image_vkresult(alloc_res);
    }

    VkResult bind_res = vkBindImageMemory(ctx->device, image, memory, 0);
    if (bind_res != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan Error] vkBindImageMemory failed: %s (%d)\n",
                vulkan_vkresult_to_str(bind_res), (int)bind_res);
        destroy_unexported_image(ctx->device, image, memory);
        return map_image_vkresult(bind_res);
    }

    bool has_modifier = false;
    uint64_t chosen_modifier = 0;
    if (modifier_tiling) {
        if (ctx->dispatch.vkGetImageDrmFormatModifierPropertiesEXT == nullptr) {
            fprintf(stderr, "[Vulkan Error] vkGetImageDrmFormatModifierPropertiesEXT is null\n");
            destroy_unexported_image(ctx->device, image, memory);
            return VULKAN_ERROR_FUNCTION_POINTER_MISSING;
        }

        VkImageDrmFormatModifierPropertiesEXT modifier_props = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
        };
        VkResult modifier_res = ctx->dispatch.vkGetImageDrmFormatModifierPropertiesEXT(
            ctx->device, image, &modifier_props);
        if (modifier_res != VK_SUCCESS) {
            fprintf(stderr, "[Vulkan Error] vkGetImageDrmFormatModifierPropertiesEXT failed: %s (%d)\n",
                    vulkan_vkresult_to_str(modifier_res), (int)modifier_res);
            destroy_unexported_image(ctx->device, image, memory);
            return map_image_vkresult(modifier_res);
        }
        if (!modifier_list_contains(modifiers, modifier_count, modifier_props.drmFormatModifier)) {
            fprintf(stderr,
                    "[Vulkan Error] Driver picked DRM modifier 0x%llx which was not in the requested list\n",
                    (unsigned long long)modifier_props.drmFormatModifier);
            destroy_unexported_image(ctx->device, image, memory);
            return VULKAN_ERROR_FORMAT_NOT_SUPPORTED;
        }
        has_modifier = true;
        chosen_modifier = modifier_props.drmFormatModifier;
    }

    VkDeviceSize plane_offset = 0;
    VkDeviceSize plane_row_pitch = 0;
    if (modifier_tiling || tiling == VK_IMAGE_TILING_LINEAR) {
        VkImageSubresource subres = {
            .aspectMask = modifier_tiling
                ? VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT
                : VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel   = 0,
            .arrayLayer = 0,
        };
        VkSubresourceLayout layout = {0};
        vkGetImageSubresourceLayout(ctx->device, image, &subres, &layout);
        plane_offset = layout.offset;
        plane_row_pitch = layout.rowPitch;
    }

    if (ctx->dispatch.vkGetMemoryFdKHR == nullptr) {
        fprintf(stderr, "[Vulkan Error] vkGetMemoryFdKHR dispatch pointer is null\n");
        destroy_unexported_image(ctx->device, image, memory);
        return VULKAN_ERROR_FUNCTION_POINTER_MISSING;
    }

    int dmabuf_fd = -1;
    VkMemoryGetFdInfoKHR get_fd_info = {
        .sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory     = memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkResult fd_res = ctx->dispatch.vkGetMemoryFdKHR(ctx->device, &get_fd_info, &dmabuf_fd);
    if (fd_res != VK_SUCCESS || dmabuf_fd < 0) {
        fprintf(stderr, "[Vulkan Error] vkGetMemoryFdKHR failed: %s (%d)\n",
                vulkan_vkresult_to_str(fd_res), (int)fd_res);
        if (dmabuf_fd >= 0) {
            close(dmabuf_fd);
        }
        destroy_unexported_image(ctx->device, image, memory);
        if (fd_res != VK_SUCCESS) {
            return map_image_vkresult(fd_res);
        }
        return VULKAN_ERROR_INITIALIZATION_FAILED;
    }

    out_image->image = image;
    out_image->memory = memory;
    out_image->dmabuf_fd = dmabuf_fd;
    out_image->width = width;
    out_image->height = height;
    out_image->format = format;
    out_image->usage = usage;
    out_image->tiling = tiling;
    out_image->drm_format_modifier = chosen_modifier;
    out_image->has_drm_format_modifier = has_modifier;
    out_image->allocation_size = mem_reqs.size;
    out_image->offset = plane_offset;
    out_image->row_pitch = plane_row_pitch;
    return VULKAN_SUCCESS;
}

static VulkanResult try_fixed_tiling(
    VulkanContext         *ctx,
    uint32_t               width,
    uint32_t               height,
    VkFormat               format,
    VkImageUsageFlags      usage,
    VkFormatFeatureFlags   format_features,
    VkFormatFeatureFlags   required_features,
    VkImageTiling          tiling,
    VulkanExportableImage *out_image,
    bool                  *tried
) {
    *tried = false;
    if (!features_cover(format_features, required_features)) {
        return VULKAN_ERROR_FORMAT_NOT_SUPPORTED;
    }

    VulkanResult exportable = image_is_exportable(ctx, format, usage, tiling, false, 0);
    if (exportable != VULKAN_SUCCESS) {
        return exportable;
    }

    *tried = true;
    return create_bound_export_image(ctx, width, height, format, usage, tiling,
                                     nullptr, 0, false, out_image);
}

VulkanResult vulkan_create_exportable_image_with_modifiers(
    VulkanContext         *ctx,
    uint32_t               width,
    uint32_t               height,
    VkFormat               format,
    VkImageUsageFlags      usage,
    const uint64_t        *allowed_modifiers,
    uint32_t               allowed_modifier_count,
    bool                   prefer_linear,
    VulkanExportableImage *out_image
) {
    if (ctx == nullptr || out_image == nullptr || width == 0 || height == 0 ||
        ctx->device == VK_NULL_HANDLE || allowed_modifiers == nullptr || allowed_modifier_count == 0) {
        return VULKAN_ERROR_INVALID_ARGUMENT;
    }
    if (!ctx->device_info.supports_drm_format_modifiers ||
        ctx->dispatch.vkGetImageDrmFormatModifierPropertiesEXT == nullptr) {
        return VULKAN_ERROR_FORMAT_NOT_SUPPORTED;
    }

    VkFormatFeatureFlags required_features = 0;
    if (!usage_to_required_features(usage, &required_features)) {
        return VULKAN_ERROR_INVALID_ARGUMENT;
    }

    *out_image = (VulkanExportableImage){ .dmabuf_fd = -1 };

    uint64_t *modifiers = nullptr;
    uint32_t modifier_count = 0;
    VulkanResult collected = collect_export_modifiers_filtered(
        ctx, format, required_features, usage,
        allowed_modifiers, allowed_modifier_count,
        prefer_linear,
        &modifiers, &modifier_count);
    if (collected != VULKAN_SUCCESS) {
        return collected;
    }
    if (modifier_count == 0) {
        free(modifiers);
        return VULKAN_ERROR_FORMAT_NOT_SUPPORTED;
    }

    VulkanResult created = create_bound_export_image(
        ctx, width, height, format, usage, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        modifiers, modifier_count, prefer_linear, out_image);
    free(modifiers);
    if (created != VULKAN_SUCCESS) {
        return created;
    }

    if (!out_image->has_drm_format_modifier ||
        !modifier_list_contains(allowed_modifiers, allowed_modifier_count, out_image->drm_format_modifier)) {
        vulkan_destroy_exportable_image(ctx, out_image);
        return VULKAN_ERROR_FORMAT_NOT_SUPPORTED;
    }

    return VULKAN_SUCCESS;
}

VulkanResult vulkan_create_exportable_image(
    VulkanContext         *ctx,
    uint32_t               width,
    uint32_t               height,
    VkFormat               format,
    VkImageUsageFlags      usage,
    VulkanExportTiling     tiling_policy,
    VulkanExportableImage *out_image
) {
    if (ctx == nullptr || out_image == nullptr || width == 0 || height == 0 ||
        ctx->device == VK_NULL_HANDLE) {
        return VULKAN_ERROR_INVALID_ARGUMENT;
    }
    if (tiling_policy != VULKAN_EXPORT_TILING_AUTO &&
        tiling_policy != VULKAN_EXPORT_TILING_DRM_MODIFIER &&
        tiling_policy != VULKAN_EXPORT_TILING_LINEAR &&
        tiling_policy != VULKAN_EXPORT_TILING_OPTIMAL) {
        return VULKAN_ERROR_INVALID_ARGUMENT;
    }

    VkFormatFeatureFlags required_features = 0;
    if (!usage_to_required_features(usage, &required_features)) {
        return VULKAN_ERROR_INVALID_ARGUMENT;
    }

    *out_image = (VulkanExportableImage){.dmabuf_fd = -1};

    VkFormatProperties format_props;
    vkGetPhysicalDeviceFormatProperties(ctx->physical_device, format, &format_props);

    const bool want_modifier = tiling_policy == VULKAN_EXPORT_TILING_AUTO ||
                               tiling_policy == VULKAN_EXPORT_TILING_DRM_MODIFIER;
    const bool want_linear = tiling_policy == VULKAN_EXPORT_TILING_AUTO ||
                             tiling_policy == VULKAN_EXPORT_TILING_LINEAR;
    const bool want_optimal = tiling_policy == VULKAN_EXPORT_TILING_AUTO ||
                              tiling_policy == VULKAN_EXPORT_TILING_OPTIMAL;

    bool tried_modifier = false;
    bool tried_linear = false;
    bool tried_optimal = false;

    if (want_modifier && ctx->device_info.supports_drm_format_modifiers &&
        ctx->dispatch.vkGetImageDrmFormatModifierPropertiesEXT != nullptr) {
        uint64_t *modifiers = nullptr;
        uint32_t modifier_count = 0;
        VulkanResult collected = collect_export_modifiers_filtered(
            ctx, format, required_features, usage, nullptr, 0, false,
            &modifiers, &modifier_count);
        if (collected != VULKAN_SUCCESS) {
            return collected;
        }
        if (modifier_count > 0) {
            tried_modifier = true;
            VulkanResult created = create_bound_export_image(
                ctx, width, height, format, usage, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                modifiers, modifier_count, false, out_image);
            free(modifiers);
            if (created == VULKAN_SUCCESS || result_is_memory_error(created) ||
                created == VULKAN_ERROR_FUNCTION_POINTER_MISSING) {
                return created;
            }
            fprintf(stderr,
                    "[Vulkan Warn] DRM-modifier DMA-BUF export failed (%s). %s\n",
                    vulkan_result_to_str(created),
                    want_linear || want_optimal ? "Trying another tiling." : "No fallback tiling was requested.");
        } else {
            free(modifiers);
        }
    }

    if (out_image->image == VK_NULL_HANDLE && want_linear) {
        VulkanResult created = try_fixed_tiling(
            ctx, width, height, format, usage, format_props.linearTilingFeatures,
            required_features, VK_IMAGE_TILING_LINEAR, out_image, &tried_linear);
        if (created == VULKAN_SUCCESS || result_is_memory_error(created) ||
            created == VULKAN_ERROR_FUNCTION_POINTER_MISSING) {
            return created;
        }
    }

    if (out_image->image == VK_NULL_HANDLE && want_optimal) {
        VulkanResult created = try_fixed_tiling(
            ctx, width, height, format, usage, format_props.optimalTilingFeatures,
            required_features, VK_IMAGE_TILING_OPTIMAL, out_image, &tried_optimal);
        if (created == VULKAN_SUCCESS || result_is_memory_error(created) ||
            created == VULKAN_ERROR_FUNCTION_POINTER_MISSING) {
            return created;
        }
    }

    fprintf(stderr,
            "[Vulkan Error] No DMA-BUF tiling supports format %d usage 0x%x "
            "(drm-modifier %s, linear %s, optimal %s)\n",
            (int)format,
            usage,
            tried_modifier ? "tried" : "unavailable",
            tried_linear ? "tried" : "unavailable",
            tried_optimal ? "tried" : "unavailable");
    return VULKAN_ERROR_FORMAT_NOT_SUPPORTED;
}

void vulkan_destroy_exportable_image(
    VulkanContext         *ctx,
    VulkanExportableImage *image
) {
    if (ctx == nullptr || image == nullptr) {
        return;
    }

    if (image->dmabuf_fd >= 0) {
        close(image->dmabuf_fd);
        image->dmabuf_fd = -1;
    }

    if (ctx->device != VK_NULL_HANDLE) {
        if (image->image != VK_NULL_HANDLE) {
            vkDestroyImage(ctx->device, image->image, nullptr);
            image->image = VK_NULL_HANDLE;
        }

        if (image->memory != VK_NULL_HANDLE) {
            vkFreeMemory(ctx->device, image->memory, nullptr);
            image->memory = VK_NULL_HANDLE;
        }
    }

    image->usage = 0;
    image->tiling = VK_IMAGE_TILING_OPTIMAL;
    image->drm_format_modifier = 0;
    image->has_drm_format_modifier = false;
    image->width = 0;
    image->height = 0;
    image->allocation_size = 0;
    image->offset = 0;
    image->row_pitch = 0;
}

VulkanResult vulkan_command_pool_create(VulkanContext *ctx) {
    if (ctx == nullptr || ctx->device == VK_NULL_HANDLE) {
        return VULKAN_ERROR_INVALID_ARGUMENT;
    }

    VkCommandPoolCreateInfo pool_info = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext            = nullptr,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = ctx->queues.graphics_family_index,
    };

    VkResult res = vkCreateCommandPool(ctx->device, &pool_info, nullptr, &ctx->cmd_pool);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan Error] vkCreateCommandPool failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        VulkanResult mapped = vulkan_result_from_vkresult(res);
        return mapped == VULKAN_ERROR_UNKNOWN ? VULKAN_ERROR_INITIALIZATION_FAILED : mapped;
    }

    return VULKAN_SUCCESS;
}

void vulkan_command_pool_destroy(VulkanContext *ctx) {
    if (ctx == nullptr || ctx->device == VK_NULL_HANDLE || ctx->cmd_pool == VK_NULL_HANDLE) {
        return;
    }

    vkDestroyCommandPool(ctx->device, ctx->cmd_pool, nullptr);
    ctx->cmd_pool = VK_NULL_HANDLE;
}