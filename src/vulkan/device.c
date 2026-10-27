#include "vulkan/device.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *k_required_device_extensions[] = {
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME,
};

typedef struct DeviceExtensionList {
    VkExtensionProperties *items;
    uint32_t count;
} DeviceExtensionList;

typedef struct GpuCandidate {
    VkPhysicalDevice                 device;
    VkPhysicalDeviceProperties       properties;
    VkPhysicalDeviceMemoryProperties memory_properties;
    uint32_t                         score;
    bool                             is_disqualified;
    char                             disqualification_reason[256];
    bool                             queues_probed;
    bool                             features_probed;
    bool                             extensions_probed;

    uint32_t                         graphics_family;
    uint32_t                         compute_family;
    uint32_t                         transfer_family;
    uint32_t                         graphics_queue_count;
    uint32_t                         compute_queue_count;
    uint32_t                         transfer_queue_count;
    bool                             has_graphics;
    bool                             has_dedicated_compute;
    bool                             has_dedicated_transfer;

    bool                             supports_external_memory_fd;
    bool                             supports_dma_buf;
    bool                             supports_drm_modifiers;
    bool                             supports_syncobj;
    bool                             supports_queue_family_foreign;
    bool                             supports_physical_device_drm;
    bool                             supports_bda;
    bool                             supports_scalar_layout;
    bool                             supports_anisotropy;
    bool                             supports_dyn_render;
    bool                             supports_sync2;
    bool                             supports_timeline;
    bool                             supports_maintenance5;
    bool                             supports_maintenance6;
    bool                             supports_push_descriptors;
    bool                             supports_host_image_copy;
    bool                             supports_dynamic_rendering_local_read;
    bool                             supports_shader_int16;
    bool                             supports_shader_int64;
    bool                             supports_storage_buffer_16bit;

    bool                             has_drm_primary;
    bool                             has_drm_render;
    int64_t                          drm_primary_major;
    int64_t                          drm_primary_minor;
    int64_t                          drm_render_major;
    int64_t                          drm_render_minor;

    uint64_t                         device_local_bytes;
    uint64_t                         dedicated_vram_bytes;
} GpuCandidate;

static void device_extension_list_free(DeviceExtensionList *list) {
    free(list->items);
    list->items = nullptr;
    list->count = 0;
}

static VulkanResult device_extension_list_load(VkPhysicalDevice device, DeviceExtensionList *list) {
    list->items = nullptr;
    list->count = 0;
    uint32_t count = 0;
    VkResult res = vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan Error] vkEnumerateDeviceExtensionProperties failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return vulkan_result_from_vkresult(res);
    }
    if (count == 0) {
        return VULKAN_SUCCESS;
    }

    VkExtensionProperties *items = malloc(sizeof(*items) * count);
    if (items == nullptr) {
        fprintf(stderr, "[Vulkan Error] Out of host memory while probing device extensions\n");
        return VULKAN_ERROR_OUT_OF_HOST_MEMORY;
    }

    res = vkEnumerateDeviceExtensionProperties(device, nullptr, &count, items);
    if (res != VK_SUCCESS) {
        free(items);
        fprintf(stderr, "[Vulkan Error] vkEnumerateDeviceExtensionProperties failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return vulkan_result_from_vkresult(res);
    }

    list->items = items;
    list->count = count;
    return VULKAN_SUCCESS;
}

static bool device_extension_list_has(const DeviceExtensionList *list, const char *name) {
    for (uint32_t i = 0; i < list->count; ++i) {
        if (strncmp(list->items[i].extensionName, name, VK_MAX_EXTENSION_NAME_SIZE) == 0) {
            return true;
        }
    }
    return false;
}

static const char *get_vendor_name(uint32_t vendor_id) {
    switch (vendor_id) {
    case 0x10DE: return "NVIDIA";
    case 0x8086: return "Intel";
    case 0x1002: return "AMD";
    case 0x13B5: return "ARM";
    case 0x5143: return "Qualcomm";
    case 0x1010: return "ImgTec";
    case 0x10005: return "Mesa";
    default:     return "Unknown";
    }
}

static const char *get_device_type_str(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "Discrete GPU";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "Integrated GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "Virtual GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU (Software)";
    default:                                     return "Other";
    }
}

static char ascii_lower(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return (char)c;
}

// ASCII-only. Vulkan device, vendor, and type names used here are ASCII.
static bool ascii_contains_ci(const char *haystack, const char *needle) {
    if (haystack == nullptr || needle == nullptr) {
        return false;
    }
    if (*needle == '\0') {
        return true;
    }
    for (const char *start = haystack; *start != '\0'; ++start) {
        const char *left = start;
        const char *right = needle;
        while (*left != '\0' && *right != '\0' &&
               ascii_lower((unsigned char)*left) == ascii_lower((unsigned char)*right)) {
            ++left;
            ++right;
        }
        if (*right == '\0') {
            return true;
        }
    }
    return false;
}

static bool parse_gpu_index(const char *text, uint32_t *out_index) {
    if (text == nullptr || *text == '\0' || out_index == nullptr) {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 0 || (unsigned long)value > UINT32_MAX) {
        return false;
    }
    *out_index = (uint32_t)value;
    return true;
}

static void disqualify(GpuCandidate *candidate, const char *reason) {
    candidate->is_disqualified = true;
    candidate->score = 0;
    snprintf(candidate->disqualification_reason, sizeof candidate->disqualification_reason,
             "%s", reason);
}

static void measure_memory(GpuCandidate *candidate) {
    const VkPhysicalDeviceMemoryProperties *mem = &candidate->memory_properties;
    uint64_t device_local = 0;
    uint64_t dedicated = 0;
    for (uint32_t heap = 0; heap < mem->memoryHeapCount; ++heap) {
        if ((mem->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) {
            continue;
        }
        device_local += mem->memoryHeaps[heap].size;

        bool pure_vram = false;
        for (uint32_t type = 0; type < mem->memoryTypeCount; ++type) {
            VkMemoryPropertyFlags flags = mem->memoryTypes[type].propertyFlags;
            if (mem->memoryTypes[type].heapIndex == heap &&
                (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
                (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
                pure_vram = true;
                break;
            }
        }
        // Integrated and virtual devices expose system RAM as device-local. Only a
        // discrete GPU's non-host-visible heap is dedicated VRAM.
        if (pure_vram && candidate->properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            dedicated += mem->memoryHeaps[heap].size;
        }
    }

    candidate->device_local_bytes = device_local;
    candidate->dedicated_vram_bytes = dedicated;
}

static VulkanResult discover_queue_families(GpuCandidate *candidate) {
    candidate->queues_probed = true;
    candidate->has_graphics = false;
    candidate->has_dedicated_compute = false;
    candidate->has_dedicated_transfer = false;
    candidate->graphics_family = UINT32_MAX;
    candidate->compute_family = UINT32_MAX;
    candidate->transfer_family = UINT32_MAX;
    candidate->graphics_queue_count = 0;
    candidate->compute_queue_count = 0;
    candidate->transfer_queue_count = 0;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(candidate->device, &count, nullptr);
    if (count == 0) {
        return VULKAN_SUCCESS;
    }

    VkQueueFamilyProperties *families = malloc(sizeof(*families) * count);
    if (families == nullptr) {
        fprintf(stderr, "[Vulkan Error] Out of host memory while probing queue families\n");
        return VULKAN_ERROR_OUT_OF_HOST_MEMORY;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(candidate->device, &count, families);

    for (uint32_t i = 0; i < count; ++i) {
        VkQueueFlags flags = families[i].queueFlags;
        uint32_t queue_count = families[i].queueCount;
        if (queue_count == 0) {
            continue;
        }

        if (!candidate->has_graphics && (flags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            candidate->graphics_family = i;
            candidate->graphics_queue_count = queue_count;
            candidate->has_graphics = true;
        }
        if (!candidate->has_dedicated_compute &&
            (flags & VK_QUEUE_COMPUTE_BIT) != 0 &&
            (flags & VK_QUEUE_GRAPHICS_BIT) == 0) {
            candidate->compute_family = i;
            candidate->compute_queue_count = queue_count;
            candidate->has_dedicated_compute = true;
        }
        if (!candidate->has_dedicated_transfer &&
            (flags & VK_QUEUE_TRANSFER_BIT) != 0 &&
            (flags & VK_QUEUE_GRAPHICS_BIT) == 0 &&
            (flags & VK_QUEUE_COMPUTE_BIT) == 0) {
            candidate->transfer_family = i;
            candidate->transfer_queue_count = queue_count;
            candidate->has_dedicated_transfer = true;
        }
    }

    if (candidate->has_graphics && !candidate->has_dedicated_compute) {
        uint32_t graphics = candidate->graphics_family;
        if ((families[graphics].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
            candidate->compute_family = graphics;
            candidate->compute_queue_count = families[graphics].queueCount;
        } else {
            for (uint32_t i = 0; i < count; ++i) {
                if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0 && families[i].queueCount > 0) {
                    candidate->compute_family = i;
                    candidate->compute_queue_count = families[i].queueCount;
                    break;
                }
            }
        }
    }

    // A pure transfer family was already recorded. Otherwise keep transfer off the
    // graphics queue when some other non-graphics family can do the copies.
    if (candidate->has_graphics && !candidate->has_dedicated_transfer) {
        uint32_t separate_family = UINT32_MAX;
        uint32_t separate_count = 0;
        uint32_t compute_family_transfer = UINT32_MAX;
        uint32_t compute_family_transfer_count = 0;

        for (uint32_t i = 0; i < count; ++i) {
            VkQueueFlags flags = families[i].queueFlags;
            uint32_t queue_count = families[i].queueCount;
            if (queue_count == 0 || (flags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                continue;
            }

            const bool can_transfer = (flags & VK_QUEUE_TRANSFER_BIT) != 0 ||
                                      (flags & VK_QUEUE_COMPUTE_BIT) != 0;
            if (!can_transfer) {
                continue;
            }

            if (candidate->compute_family != UINT32_MAX && i == candidate->compute_family) {
                if (compute_family_transfer == UINT32_MAX) {
                    compute_family_transfer = i;
                    compute_family_transfer_count = queue_count;
                }
                continue;
            }

            separate_family = i;
            separate_count = queue_count;
            break;
        }

        if (separate_family != UINT32_MAX) {
            candidate->transfer_family = separate_family;
            candidate->transfer_queue_count = separate_count;
        } else if (compute_family_transfer != UINT32_MAX) {
            candidate->transfer_family = compute_family_transfer;
            candidate->transfer_queue_count = compute_family_transfer_count;
        } else {
            candidate->transfer_family = candidate->graphics_family;
            candidate->transfer_queue_count = candidate->graphics_queue_count;
        }
    }

    free(families);
    return VULKAN_SUCCESS;
}

static uint32_t score_candidate(const GpuCandidate *candidate) {
    uint32_t score = 1000;
    switch (candidate->properties.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   score += 100000; break;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score += 40000; break;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    score += 10000; break;
    default:                                     score += 1000; break;
    }
    if (candidate->has_dedicated_compute)                  score += 1000;
    if (candidate->has_dedicated_transfer)                 score += 500;
    if (candidate->supports_drm_modifiers)                 score += 1000;
    if (candidate->supports_syncobj)                       score += 1000;
    if (candidate->supports_maintenance5)                  score += 500;
    if (candidate->supports_host_image_copy)               score += 500;
    if (candidate->supports_dynamic_rendering_local_read)  score += 500;

    // Dedicated VRAM can break ties inside a class. It cannot outrank a discrete GPU
    // with system memory that an integrated GPU reports as device-local.
    uint32_t gib = (uint32_t)(candidate->dedicated_vram_bytes / (1024ULL * 1024ULL * 1024ULL));
    if (gib > 200) {
        gib = 200;
    }
    score += gib * 100;
    return score;
}

static VulkanResult evaluate_device(GpuCandidate *candidate) {
    vkGetPhysicalDeviceProperties(candidate->device, &candidate->properties);
    vkGetPhysicalDeviceMemoryProperties(candidate->device, &candidate->memory_properties);
    measure_memory(candidate);
    if (candidate->properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
        disqualify(candidate, "Software rasterizer (CPU device) is not used");
        return VULKAN_SUCCESS;
    }

    if (candidate->properties.apiVersion < VK_API_VERSION_1_4) {
        char reason[256];
        snprintf(reason, sizeof reason,
                 "Driver API v%u.%u.%u is below required Vulkan 1.4",
                 VK_API_VERSION_MAJOR(candidate->properties.apiVersion),
                 VK_API_VERSION_MINOR(candidate->properties.apiVersion),
                 VK_API_VERSION_PATCH(candidate->properties.apiVersion));
        disqualify(candidate, reason);
        return VULKAN_SUCCESS;
    }

    VulkanResult queues = discover_queue_families(candidate);
    if (queues != VULKAN_SUCCESS) {
        return queues;
    }
    if (!candidate->has_graphics || candidate->graphics_queue_count < 1) {
        disqualify(candidate, "No queue family supports VK_QUEUE_GRAPHICS_BIT");
        return VULKAN_SUCCESS;
    }
    if (candidate->compute_family == UINT32_MAX || candidate->compute_queue_count < 1) {
        disqualify(candidate, "No queue family supports VK_QUEUE_COMPUTE_BIT");
        return VULKAN_SUCCESS;
    }
    if (candidate->transfer_queue_count < 1) {
        disqualify(candidate, "No queue available for transfer work");
        return VULKAN_SUCCESS;
    }

    VkPhysicalDeviceVulkan14Features feat14 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
    };
    VkPhysicalDeviceVulkan13Features feat13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &feat14,
    };
    VkPhysicalDeviceVulkan12Features feat12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &feat13,
    };
    VkPhysicalDeviceVulkan11Features feat11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = &feat12,
    };
    VkPhysicalDeviceFeatures2 features2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &feat11,
    };
    vkGetPhysicalDeviceFeatures2(candidate->device, &features2);

    candidate->supports_dyn_render                   = (feat13.dynamicRendering == VK_TRUE);
    candidate->supports_sync2                        = (feat13.synchronization2 == VK_TRUE);
    candidate->supports_bda                          = (feat12.bufferDeviceAddress == VK_TRUE);
    candidate->supports_scalar_layout                = (feat12.scalarBlockLayout == VK_TRUE);
    candidate->supports_timeline                     = (feat12.timelineSemaphore == VK_TRUE);
    candidate->supports_anisotropy                   = (features2.features.samplerAnisotropy == VK_TRUE);
    candidate->supports_maintenance5                 = (feat14.maintenance5 == VK_TRUE);
    candidate->supports_maintenance6                 = (feat14.maintenance6 == VK_TRUE);
    candidate->supports_push_descriptors             = (feat14.pushDescriptor == VK_TRUE);
    candidate->supports_host_image_copy              = (feat14.hostImageCopy == VK_TRUE);
    candidate->supports_dynamic_rendering_local_read = (feat14.dynamicRenderingLocalRead == VK_TRUE);
    candidate->supports_shader_int16                 = (features2.features.shaderInt16 == VK_TRUE);
    candidate->supports_shader_int64                 = (features2.features.shaderInt64 == VK_TRUE);
    candidate->supports_storage_buffer_16bit         = (feat11.storageBuffer16BitAccess == VK_TRUE);
    candidate->features_probed                       = true;

    if (!candidate->supports_dyn_render || !candidate->supports_sync2 ||
        !candidate->supports_bda || !candidate->supports_timeline ||
        !candidate->supports_scalar_layout || !candidate->supports_shader_int16 ||
        !candidate->supports_shader_int64 || !candidate->supports_storage_buffer_16bit ||
        !candidate->supports_maintenance5 || !candidate->supports_maintenance6 ||
        !candidate->supports_push_descriptors || !candidate->supports_host_image_copy ||
        !candidate->supports_dynamic_rendering_local_read) {
        char reason[320];
        snprintf(reason, sizeof reason,
                 "Missing Vulkan 1.4 feature (DynRender=%d, Sync2=%d, BDA=%d, Timeline=%d, Scalar=%d, Int16=%d, Int64=%d, Storage16=%d, Maint5=%d, Maint6=%d, PushDesc=%d, HostCopy=%d, LocalRead=%d)",
                 candidate->supports_dyn_render, candidate->supports_sync2,
                 candidate->supports_bda, candidate->supports_timeline,
                 candidate->supports_scalar_layout, candidate->supports_shader_int16,
                 candidate->supports_shader_int64, candidate->supports_storage_buffer_16bit,
                 candidate->supports_maintenance5, candidate->supports_maintenance6,
                 candidate->supports_push_descriptors, candidate->supports_host_image_copy,
                 candidate->supports_dynamic_rendering_local_read);
        disqualify(candidate, reason);
        return VULKAN_SUCCESS;
    }

    DeviceExtensionList extensions = {0};
    VulkanResult ext_res = device_extension_list_load(candidate->device, &extensions);
    if (ext_res != VULKAN_SUCCESS) {
        return ext_res;
    }

    candidate->supports_external_memory_fd = device_extension_list_has(
        &extensions, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    candidate->supports_dma_buf = device_extension_list_has(
        &extensions, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    candidate->supports_drm_modifiers = device_extension_list_has(
        &extensions, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    candidate->supports_syncobj = device_extension_list_has(
        &extensions, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
    candidate->supports_queue_family_foreign = device_extension_list_has(
        &extensions, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
    candidate->supports_physical_device_drm = device_extension_list_has(
        &extensions, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
    candidate->extensions_probed = true;

    if (candidate->supports_physical_device_drm) {
        VkPhysicalDeviceDrmPropertiesEXT drm_props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
        };
        VkPhysicalDeviceProperties2 props2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &drm_props,
        };
        vkGetPhysicalDeviceProperties2(candidate->device, &props2);
        candidate->has_drm_primary   = (drm_props.hasPrimary == VK_TRUE);
        candidate->has_drm_render    = (drm_props.hasRender == VK_TRUE);
        candidate->drm_primary_major = drm_props.primaryMajor;
        candidate->drm_primary_minor = drm_props.primaryMinor;
        candidate->drm_render_major  = drm_props.renderMajor;
        candidate->drm_render_minor  = drm_props.renderMinor;
    }

    char missing[256];
    size_t used = 0;
    missing[0] = '\0';
    for (size_t i = 0; i < sizeof k_required_device_extensions / sizeof k_required_device_extensions[0]; ++i) {
        if (device_extension_list_has(&extensions, k_required_device_extensions[i])) {
            continue;
        }
        int wrote = snprintf(missing + used, sizeof missing - used, "%s%s",
                             used == 0 ? "" : ", ", k_required_device_extensions[i]);
        if (wrote < 0 || (size_t)wrote >= sizeof missing - used) {
            break;
        }
        used += (size_t)wrote;
    }
    device_extension_list_free(&extensions);

    if (used > 0) {
        char reason[256];
        snprintf(reason, sizeof reason, "Missing required device extension: %s", missing);
        disqualify(candidate, reason);
        return VULKAN_SUCCESS;
    }

    if (!candidate->has_drm_render) {
        disqualify(candidate, "Device lacks Linux DRM render node");
        return VULKAN_SUCCESS;
    }

    // Probe timeline semaphore export AND import with VkSemaphoreTypeCreateInfo in pNext.
    VkSemaphoreTypeCreateInfo timeline_type_info = {
        .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext         = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue  = 0,
    };
    VkPhysicalDeviceExternalSemaphoreInfo ext_info = {
        .sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .pNext      = &timeline_type_info,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkExternalSemaphoreProperties ext_props = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
        .pNext = nullptr,
    };
    vkGetPhysicalDeviceExternalSemaphoreProperties(candidate->device, &ext_info, &ext_props);
    const VkExternalSemaphoreFeatureFlags req_sem_flags =
        VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
        VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
    if ((ext_props.externalSemaphoreFeatures & req_sem_flags) != req_sem_flags) {
        disqualify(candidate, "Timeline semaphore opaque-fd export/import is not supported");
        return VULKAN_SUCCESS;
    }

    candidate->is_disqualified = false;
    candidate->score = score_candidate(candidate);
    return VULKAN_SUCCESS;
}

static void print_candidate(uint32_t index, const GpuCandidate *candidate) {
    char driver_version[64];
    vulkan_format_driver_version(candidate->properties.vendorID,
                                 candidate->properties.driverVersion,
                                 driver_version, sizeof driver_version);
    printf("  [%u] %s (%s | Vendor: %s [0x%04x] | DeviceID: 0x%04x)\n",
           index,
           candidate->properties.deviceName,
           get_device_type_str(candidate->properties.deviceType),
           get_vendor_name(candidate->properties.vendorID),
           candidate->properties.vendorID,
           candidate->properties.deviceID);
    printf("      Driver: v%s | Vulkan API: v%u.%u.%u | Device-local heap: %.2f GiB | Dedicated VRAM: %.2f GiB\n",
           driver_version,
           VK_API_VERSION_MAJOR(candidate->properties.apiVersion),
           VK_API_VERSION_MINOR(candidate->properties.apiVersion),
           VK_API_VERSION_PATCH(candidate->properties.apiVersion),
           (double)candidate->device_local_bytes / (1024.0 * 1024.0 * 1024.0),
           (double)candidate->dedicated_vram_bytes / (1024.0 * 1024.0 * 1024.0));

    if (!candidate->queues_probed) {
        printf("      Queues: not probed\n");
    } else if (!candidate->has_graphics) {
        printf("      Queues: Graphics=no\n");
    } else {
        printf("      Queues: Graphics=family %u x%u",
               candidate->graphics_family, candidate->graphics_queue_count);
        if (candidate->compute_family == UINT32_MAX) {
            printf(", Compute=no");
        } else {
            const char *compute_place = candidate->has_dedicated_compute ? "dedicated"
                : (candidate->compute_family != candidate->graphics_family ? "separate" : "shared");
            printf(", Compute=%s family %u x%u",
                   compute_place,
                   candidate->compute_family, candidate->compute_queue_count);
        }
        if (candidate->transfer_family == UINT32_MAX) {
            printf(", Transfer=no\n");
        } else {
            const char *transfer_place = candidate->has_dedicated_transfer ? "dedicated"
                : (candidate->transfer_family != candidate->graphics_family ? "separate" : "shared");
            printf(", Transfer=%s family %u x%u\n",
                   transfer_place,
                   candidate->transfer_family, candidate->transfer_queue_count);
        }
    }

    if (!candidate->features_probed) {
        printf("      Features: not probed\n");
    } else {
        printf("      Features: DynRender=%s, LocalRead=%s, Sync2=%s, BDA=%s, ScalarLayout=%s, Timeline=%s, Anisotropy=%s, Maint5=%s, Maint6=%s, PushDesc=%s, HostImageCopy=%s, Int16=%s, Int64=%s, Storage16=%s\n",
               candidate->supports_dyn_render ? "yes" : "no",
               candidate->supports_dynamic_rendering_local_read ? "yes" : "no",
               candidate->supports_sync2 ? "yes" : "no",
               candidate->supports_bda ? "yes" : "no",
               candidate->supports_scalar_layout ? "yes" : "no",
               candidate->supports_timeline ? "yes" : "no",
               candidate->supports_anisotropy ? "yes" : "no",
               candidate->supports_maintenance5 ? "yes" : "no",
               candidate->supports_maintenance6 ? "yes" : "no",
               candidate->supports_push_descriptors ? "yes" : "no",
               candidate->supports_host_image_copy ? "yes" : "no",
               candidate->supports_shader_int16 ? "yes" : "no",
               candidate->supports_shader_int64 ? "yes" : "no",
               candidate->supports_storage_buffer_16bit ? "yes" : "no");
    }

    if (!candidate->extensions_probed) {
        printf("      Extensions: not probed\n");
    } else {
        char drm_node_info[64];
        if (candidate->has_drm_render) {
            snprintf(drm_node_info, sizeof drm_node_info, "render %lld:%lld",
                     (long long)candidate->drm_render_major,
                     (long long)candidate->drm_render_minor);
        } else if (candidate->has_drm_primary) {
            snprintf(drm_node_info, sizeof drm_node_info, "primary %lld:%lld",
                     (long long)candidate->drm_primary_major,
                     (long long)candidate->drm_primary_minor);
        } else {
            snprintf(drm_node_info, sizeof drm_node_info, "no");
        }

        printf("      Extensions: external-memory-fd=%s, DMA-BUF=%s, DRM-Modifiers=%s, Syncobj=%s, ForeignQueue=%s, DRM-Node=%s\n",
               candidate->supports_external_memory_fd ? "yes" : "no",
               candidate->supports_dma_buf ? "yes" : "no",
               candidate->supports_drm_modifiers ? "yes" : "no",
               candidate->supports_syncobj ? "yes" : "no",
               candidate->supports_queue_family_foreign ? "yes" : "no",
               drm_node_info);
    }

    if (candidate->is_disqualified) {
        printf("      Evaluation: \033[1;31mDISQUALIFIED\033[0m (%s) | Score: 0\n",
               candidate->disqualification_reason);
    } else {
        printf("      Evaluation: \033[1;32mELIGIBLE\033[0m | Score: %u\n", candidate->score);
    }
}

typedef struct FamilyPlan {
    uint32_t family;
    uint32_t available;
    uint32_t request;
    uint32_t next_index;
} FamilyPlan;

static int family_plan_find(const FamilyPlan *plans, uint32_t count, uint32_t family) {
    for (uint32_t i = 0; i < count; ++i) {
        if (plans[i].family == family) {
            return (int)i;
        }
    }
    return -1;
}

static bool family_plan_note(FamilyPlan *plans, uint32_t *count, uint32_t capacity,
                             uint32_t family, uint32_t available) {
    int found = family_plan_find(plans, *count, family);
    if (found >= 0) {
        if (available > plans[found].available) {
            plans[found].available = available;
        }
        return true;
    }
    if (*count >= capacity) {
        return false;
    }
    plans[*count] = (FamilyPlan){
        .family = family,
        .available = available,
    };
    *count += 1;
    return true;
}

static uint32_t family_plan_take(FamilyPlan *plans, uint32_t count, uint32_t family) {
    int found = family_plan_find(plans, count, family);
    if (found < 0) {
        return 0;
    }
    FamilyPlan *plan = &plans[found];
    if (plan->next_index < plan->request) {
        return plan->next_index++;
    }
    return 0;
}

static bool append_extension(const char **names, uint32_t *count, uint32_t capacity, const char *name) {
    for (uint32_t i = 0; i < *count; ++i) {
        if (strcmp(names[i], name) == 0) {
            return true;
        }
    }
    if (*count >= capacity) {
        return false;
    }
    names[(*count)++] = name;
    return true;
}

VulkanResult vulkan_device_create(VulkanContext *ctx) {
    if (ctx == nullptr || ctx->instance == VK_NULL_HANDLE) {
        return VULKAN_ERROR_INVALID_ARGUMENT;
    }
    uint32_t device_count = 0;
    VkResult enum_res = vkEnumeratePhysicalDevices(ctx->instance, &device_count, nullptr);
    if (enum_res != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan Error] vkEnumeratePhysicalDevices failed: %s (%d)\n",
                vulkan_vkresult_to_str(enum_res), (int)enum_res);
        return vulkan_result_from_vkresult(enum_res);
    }
    if (device_count == 0) {
        fprintf(stderr, "[Vulkan Error] No physical devices found on host system.\n");
        return VULKAN_ERROR_NO_SUITABLE_GPU;
    }

    VkPhysicalDevice *devices = malloc(sizeof(*devices) * device_count);
    if (devices == nullptr) {
        fprintf(stderr, "[Vulkan Error] Out of host memory while enumerating physical devices\n");
        return VULKAN_ERROR_OUT_OF_HOST_MEMORY;
    }
    enum_res = vkEnumeratePhysicalDevices(ctx->instance, &device_count, devices);
    if (enum_res != VK_SUCCESS) {
        free(devices);
        fprintf(stderr, "[Vulkan Error] vkEnumeratePhysicalDevices failed: %s (%d)\n",
                vulkan_vkresult_to_str(enum_res), (int)enum_res);
        return vulkan_result_from_vkresult(enum_res);
    }

    GpuCandidate *candidates = calloc(device_count, sizeof(*candidates));
    if (candidates == nullptr) {
        free(devices);
        fprintf(stderr, "[Vulkan Error] Out of host memory while scoring physical devices\n");
        return VULKAN_ERROR_OUT_OF_HOST_MEMORY;
    }

    printf("[Vulkan Probe] Enumerated %u physical device(s) on host:\n", device_count);
    for (uint32_t i = 0; i < device_count; ++i) {
        candidates[i].device = devices[i];
        VulkanResult eval = evaluate_device(&candidates[i]);
        if (eval != VULKAN_SUCCESS) {
            free(candidates);
            free(devices);
            return eval;
        }
        print_candidate(i, &candidates[i]);
    }

    const char *env_gpu_index = getenv("ENGINE_GPU_INDEX");
    const char *env_gpu_match = getenv("ENGINE_GPU");
    int32_t selected_index = -1;
    uint32_t highest_score = 0;
    const char *selection_override_note = "";

    if (env_gpu_index != nullptr) {
        uint32_t idx = 0;
        if (!parse_gpu_index(env_gpu_index, &idx)) {
            fprintf(stderr,
                    "[Vulkan Warn] ENGINE_GPU_INDEX='%s' is not a valid index. Falling back.\n",
                    env_gpu_index);
        } else if (idx >= device_count) {
            fprintf(stderr,
                    "[Vulkan Warn] ENGINE_GPU_INDEX=%u is out of range (0..%u). Falling back.\n",
                    idx, device_count - 1);
        } else if (candidates[idx].is_disqualified) {
            fprintf(stderr,
                    "[Vulkan Warn] ENGINE_GPU_INDEX=%u requested, but device is disqualified (%s). Falling back.\n",
                    idx, candidates[idx].disqualification_reason);
        } else {
            selected_index = (int32_t)idx;
            selection_override_note = " [Forced by ENGINE_GPU_INDEX]";
        }
    }

    if (selected_index == -1 && env_gpu_match != nullptr) {
        for (uint32_t i = 0; i < device_count; ++i) {
            if (candidates[i].is_disqualified) {
                continue;
            }
            const char *vendor = get_vendor_name(candidates[i].properties.vendorID);
            const char *type = get_device_type_str(candidates[i].properties.deviceType);
            const char *name = candidates[i].properties.deviceName;
            if (ascii_contains_ci(vendor, env_gpu_match) ||
                ascii_contains_ci(type, env_gpu_match) ||
                ascii_contains_ci(name, env_gpu_match)) {
                if (candidates[i].score > highest_score) {
                    highest_score = candidates[i].score;
                    selected_index = (int32_t)i;
                    selection_override_note = " [Filtered by ENGINE_GPU]";
                }
            }
        }
    }

    if (selected_index == -1) {
        for (uint32_t i = 0; i < device_count; ++i) {
            if (!candidates[i].is_disqualified && candidates[i].score > highest_score) {
                highest_score = candidates[i].score;
                selected_index = (int32_t)i;
            }
        }
    }

    if (selected_index == -1) {
        fprintf(stderr, "[Vulkan Error] None of the %u available GPUs satisfy engine requirements.\n",
                device_count);
        free(candidates);
        free(devices);
        return VULKAN_ERROR_NO_SUITABLE_GPU;
    }

    GpuCandidate best = candidates[selected_index];
    printf("[Vulkan Device] Selected GPU [%d]: %s (%s, Score: %u)%s\n",
           selected_index,
           best.properties.deviceName,
           get_device_type_str(best.properties.deviceType),
           best.score,
           selection_override_note);
    free(candidates);
    free(devices);

    ctx->physical_device = best.device;
    ctx->queues.graphics_family_index = best.graphics_family;
    ctx->queues.compute_family_index = best.compute_family;
    ctx->queues.transfer_family_index = best.transfer_family;
    ctx->queues.has_dedicated_compute = best.has_dedicated_compute;
    ctx->queues.has_dedicated_transfer = best.has_dedicated_transfer;

    strncpy(ctx->device_info.name, best.properties.deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
    ctx->device_info.name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1] = '\0';
    ctx->device_info.type = best.properties.deviceType;
    ctx->device_info.vendor_id = best.properties.vendorID;
    ctx->device_info.device_id = best.properties.deviceID;
    ctx->device_info.driver_version = best.properties.driverVersion;
    ctx->device_info.api_version = best.properties.apiVersion;
    ctx->device_info.memory_properties = best.memory_properties;
    ctx->device_info.supports_dma_buf = best.supports_dma_buf;
    ctx->device_info.supports_drm_format_modifiers = best.supports_drm_modifiers;
    ctx->device_info.supports_external_syncobj = best.supports_syncobj;
    ctx->device_info.supports_queue_family_foreign = best.supports_queue_family_foreign;
    ctx->device_info.supports_buffer_device_address = best.supports_bda;
    ctx->device_info.supports_scalar_block_layout = best.supports_scalar_layout;
    ctx->device_info.supports_sampler_anisotropy = best.supports_anisotropy;
    ctx->device_info.supports_dynamic_rendering = best.supports_dyn_render;
    ctx->device_info.supports_synchronization2 = best.supports_sync2;
    ctx->device_info.supports_timeline_semaphore = best.supports_timeline;
    ctx->device_info.supports_maintenance5 = best.supports_maintenance5;
    ctx->device_info.supports_maintenance6 = best.supports_maintenance6;
    ctx->device_info.supports_push_descriptors = best.supports_push_descriptors;
    ctx->device_info.supports_host_image_copy = best.supports_host_image_copy;
    ctx->device_info.supports_dynamic_rendering_local_read = best.supports_dynamic_rendering_local_read;
    ctx->device_info.supports_shader_int16 = best.supports_shader_int16;
    ctx->device_info.supports_shader_int64 = best.supports_shader_int64;
    ctx->device_info.supports_storage_buffer_16bit = best.supports_storage_buffer_16bit;
    ctx->device_info.has_drm_primary_node = best.has_drm_primary;
    ctx->device_info.has_drm_render_node = best.has_drm_render;
    ctx->device_info.drm_primary_major = best.drm_primary_major;
    ctx->device_info.drm_primary_minor = best.drm_primary_minor;
    ctx->device_info.drm_render_major = best.drm_render_major;
    ctx->device_info.drm_render_minor = best.drm_render_minor;

    FamilyPlan plans[3] = {0};
    uint32_t plan_count = 0;
    if (!family_plan_note(plans, &plan_count, 3, best.graphics_family, best.graphics_queue_count) ||
        !family_plan_note(plans, &plan_count, 3, best.compute_family, best.compute_queue_count) ||
        !family_plan_note(plans, &plan_count, 3, best.transfer_family, best.transfer_queue_count)) {
        return VULKAN_ERROR_QUEUE_NOT_FOUND;
    }

    const uint32_t role_families[3] = {
        best.graphics_family,
        best.compute_family,
        best.transfer_family,
    };
    for (uint32_t i = 0; i < 3; ++i) {
        int slot = family_plan_find(plans, plan_count, role_families[i]);
        if (slot >= 0) {
            plans[slot].request += 1;
        }
    }
    for (uint32_t i = 0; i < plan_count; ++i) {
        if (plans[i].available == 0 || plans[i].request == 0) {
            return VULKAN_ERROR_QUEUE_NOT_FOUND;
        }
        if (plans[i].request > plans[i].available) {
            plans[i].request = plans[i].available;
        }
    }

    // One priority per role. A family is asked for at most three queues.
    const float queue_priorities[3] = {1.0f, 1.0f, 1.0f};
    VkDeviceQueueCreateInfo queue_create_infos[3];
    for (uint32_t i = 0; i < plan_count; ++i) {
        queue_create_infos[i] = (VkDeviceQueueCreateInfo){
            .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext            = nullptr,
            .flags            = 0,
            .queueFamilyIndex = plans[i].family,
            .queueCount       = plans[i].request,
            .pQueuePriorities = queue_priorities,
        };
    }

    const char *device_extensions[16] = {0};
    uint32_t device_extension_count = 0;
    for (size_t i = 0; i < sizeof k_required_device_extensions / sizeof k_required_device_extensions[0]; ++i) {
        if (!append_extension(device_extensions, &device_extension_count, 16, k_required_device_extensions[i])) {
            return VULKAN_ERROR_INITIALIZATION_FAILED;
        }
    }

    VkPhysicalDeviceVulkan14Features feat14 = {
        .sType                     = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
        .maintenance5              = VK_TRUE,
        .maintenance6              = VK_TRUE,
        .pushDescriptor            = VK_TRUE,
        .hostImageCopy             = VK_TRUE,
        .dynamicRenderingLocalRead = VK_TRUE,
    };
    VkPhysicalDeviceVulkan13Features feat13 = {
        .sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext            = &feat14,
        .dynamicRendering = VK_TRUE,
        .synchronization2 = VK_TRUE,
    };
    VkPhysicalDeviceVulkan12Features feat12 = {
        .sType               = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext               = &feat13,
        .scalarBlockLayout   = VK_TRUE,
        .bufferDeviceAddress = VK_TRUE,
        .timelineSemaphore   = VK_TRUE,
    };
    VkPhysicalDeviceVulkan11Features feat11 = {
        .sType                     = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext                     = &feat12,
        .storageBuffer16BitAccess  = VK_TRUE,
    };
    VkPhysicalDeviceFeatures2 features2 = {
        .sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext    = &feat11,
        .features = {
            .samplerAnisotropy = best.supports_anisotropy ? VK_TRUE : VK_FALSE,
            .shaderInt16       = VK_TRUE,
            .shaderInt64       = VK_TRUE,
        },
    };

    VkDeviceCreateInfo device_create_info = {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = &features2,
        .flags                   = 0,
        .queueCreateInfoCount    = plan_count,
        .pQueueCreateInfos       = queue_create_infos,
        .enabledLayerCount       = 0,
        .ppEnabledLayerNames     = nullptr,
        .enabledExtensionCount   = device_extension_count,
        .ppEnabledExtensionNames = device_extensions,
        .pEnabledFeatures        = nullptr,
    };

    VkResult dev_res = vkCreateDevice(ctx->physical_device, &device_create_info, nullptr, &ctx->device);
    if (dev_res != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan Error] vkCreateDevice failed for '%s': %s (%d)\n",
                ctx->device_info.name, vulkan_vkresult_to_str(dev_res), (int)dev_res);
        ctx->device = VK_NULL_HANDLE;
        if (dev_res == VK_ERROR_FEATURE_NOT_PRESENT) {
            return VULKAN_ERROR_REQUIRED_FEATURE_MISSING;
        }
        if (dev_res == VK_ERROR_EXTENSION_NOT_PRESENT) {
            return VULKAN_ERROR_REQUIRED_EXTENSION_MISSING;
        }
        VulkanResult mapped = vulkan_result_from_vkresult(dev_res);
        return mapped == VULKAN_ERROR_UNKNOWN ? VULKAN_ERROR_DEVICE_CREATION_FAILED : mapped;
    }

    ctx->queues.graphics_queue_index = family_plan_take(plans, plan_count, best.graphics_family);
    ctx->queues.compute_queue_index = family_plan_take(plans, plan_count, best.compute_family);
    ctx->queues.transfer_queue_index = family_plan_take(plans, plan_count, best.transfer_family);
    ctx->queues.submission_aliased =
        (ctx->queues.graphics_family_index == ctx->queues.compute_family_index &&
         ctx->queues.graphics_queue_index == ctx->queues.compute_queue_index) ||
        (ctx->queues.graphics_family_index == ctx->queues.transfer_family_index &&
         ctx->queues.graphics_queue_index == ctx->queues.transfer_queue_index) ||
        (ctx->queues.compute_family_index == ctx->queues.transfer_family_index &&
         ctx->queues.compute_queue_index == ctx->queues.transfer_queue_index);

    vkGetDeviceQueue(ctx->device, ctx->queues.graphics_family_index,
                     ctx->queues.graphics_queue_index, &ctx->queues.graphics_queue);
    vkGetDeviceQueue(ctx->device, ctx->queues.compute_family_index,
                     ctx->queues.compute_queue_index, &ctx->queues.compute_queue);
    vkGetDeviceQueue(ctx->device, ctx->queues.transfer_family_index,
                     ctx->queues.transfer_queue_index, &ctx->queues.transfer_queue);

    ctx->dispatch.vkGetMemoryFdKHR =
        (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(ctx->device, "vkGetMemoryFdKHR");
    ctx->dispatch.vkGetMemoryFdPropertiesKHR =
        (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(ctx->device, "vkGetMemoryFdPropertiesKHR");
    ctx->dispatch.vkGetImageDrmFormatModifierPropertiesEXT =
        (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(
            ctx->device, "vkGetImageDrmFormatModifierPropertiesEXT");
    ctx->dispatch.vkGetSemaphoreFdKHR =
        (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(ctx->device, "vkGetSemaphoreFdKHR");
    ctx->dispatch.vkImportSemaphoreFdKHR =
        (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(ctx->device, "vkImportSemaphoreFdKHR");

    if (ctx->dispatch.vkGetMemoryFdKHR == nullptr ||
        ctx->dispatch.vkGetImageDrmFormatModifierPropertiesEXT == nullptr ||
        ctx->dispatch.vkGetSemaphoreFdKHR == nullptr ||
        ctx->dispatch.vkImportSemaphoreFdKHR == nullptr) {
        fprintf(stderr,
                "[Vulkan Error] Required device extension dispatch pointers resolved to null\n");
        vulkan_device_destroy(ctx);
        return VULKAN_ERROR_FUNCTION_POINTER_MISSING;
    }

    return VULKAN_SUCCESS;
}

void vulkan_device_destroy(VulkanContext *ctx) {
    if (ctx == nullptr || ctx->device == VK_NULL_HANDLE) {
        return;
    }
    vkDeviceWaitIdle(ctx->device);
    vkDestroyDevice(ctx->device, nullptr);
    ctx->device = VK_NULL_HANDLE;
    ctx->queues.graphics_queue = VK_NULL_HANDLE;
    ctx->queues.compute_queue = VK_NULL_HANDLE;
    ctx->queues.transfer_queue = VK_NULL_HANDLE;
    ctx->queues.graphics_queue_index = 0;
    ctx->queues.compute_queue_index = 0;
    ctx->queues.transfer_queue_index = 0;
    ctx->queues.submission_aliased = false;

    ctx->dispatch.vkGetMemoryFdKHR = nullptr;
    ctx->dispatch.vkGetMemoryFdPropertiesKHR = nullptr;
    ctx->dispatch.vkGetImageDrmFormatModifierPropertiesEXT = nullptr;
    ctx->dispatch.vkGetSemaphoreFdKHR = nullptr;
    ctx->dispatch.vkImportSemaphoreFdKHR = nullptr;
}