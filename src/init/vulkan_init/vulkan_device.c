#include "init/vulkan_init/vulkan_device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/**
 * @brief Checks if a specific device extension is supported by a physical device.
 */
static bool is_device_extension_supported(VkPhysicalDevice device, const char *extension_name) {
    if (device == VK_NULL_HANDLE || extension_name == nullptr) {
        return false;
    }

    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS || count == 0) {
        return false;
    }

    VkExtensionProperties *extensions = malloc(sizeof(VkExtensionProperties) * count);
    if (extensions == nullptr) {
        return false;
    }

    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions) != VK_SUCCESS) {
        free(extensions);
        return false;
    }

    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (strncmp(extensions[i].extensionName, extension_name, VK_MAX_EXTENSION_NAME_SIZE) == 0) {
            found = true;
            break;
        }
    }

    free(extensions);
    return found;
}

/**
 * @brief Translates PCI vendor ID into human-readable brand name.
 */
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

/**
 * @brief Translates VkPhysicalDeviceType into readable description string.
 */
static const char *get_device_type_str(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "Discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "Integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "Virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU (Software)";
        default:                                     return "Other";
    }
}

/**
 * @brief Candidate evaluation record for GPU selection.
 */
typedef struct GpuCandidate {
    VkPhysicalDevice                 device;
    VkPhysicalDeviceProperties       properties;
    VkPhysicalDeviceMemoryProperties memory_properties;
    uint32_t                         score;
    bool                             is_disqualified;
    char                             disqualification_reason[256];

    uint32_t                         graphics_family;
    uint32_t                         compute_family;
    uint32_t                         transfer_family;
    bool                             has_graphics;
    bool                             has_dedicated_compute;
    bool                             has_dedicated_transfer;

    bool                             supports_dma_buf;
    bool                             supports_drm_modifiers;
    bool                             supports_syncobj;
    bool                             supports_bda;
    bool                             supports_dyn_render;
    bool                             supports_sync2;
    bool                             supports_maintenance5;

    uint64_t                         dedicated_vram_bytes;
} GpuCandidate;

/**
 * @brief Discovers and categorizes queue families for a given physical device.
 */
static void discover_queue_families(GpuCandidate *candidate) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(candidate->device, &count, nullptr);
    if (count == 0) {
        return;
    }

    VkQueueFamilyProperties *families = malloc(sizeof(VkQueueFamilyProperties) * count);
    if (families == nullptr) {
        return;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(candidate->device, &count, families);

    candidate->has_graphics = false;
    candidate->has_dedicated_compute = false;
    candidate->has_dedicated_transfer = false;

    // 1. Find Graphics Family (primary)
    for (uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            candidate->graphics_family = i;
            candidate->has_graphics = true;
            break;
        }
    }

    // Default compute and transfer to graphics family initially
    candidate->compute_family = candidate->graphics_family;
    candidate->transfer_family = candidate->graphics_family;

    // 2. Look for dedicated Compute (compute without graphics)
    for (uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            candidate->compute_family = i;
            candidate->has_dedicated_compute = true;
            break;
        }
    }

    // 3. Look for dedicated Transfer (transfer without graphics and compute)
    for (uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
            !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            !(families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            candidate->transfer_family = i;
            candidate->has_dedicated_transfer = true;
            break;
        }
    }

    free(families);
}

/**
 * @brief Evaluates and scores a physical device candidate.
 */
static void score_physical_device(GpuCandidate *candidate) {
    vkGetPhysicalDeviceProperties(candidate->device, &candidate->properties);
    vkGetPhysicalDeviceMemoryProperties(candidate->device, &candidate->memory_properties);

    // Sum all dedicated VRAM heaps (VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
    uint64_t total_vram = 0;
    for (uint32_t i = 0; i < candidate->memory_properties.memoryHeapCount; ++i) {
        if (candidate->memory_properties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            total_vram += candidate->memory_properties.memoryHeaps[i].size;
        }
    }
    candidate->dedicated_vram_bytes = total_vram;

    // Baseline Requirement: Vulkan 1.3
    // Vulkan 1.3 is the architectural minimum providing Dynamic Rendering, Synchronization2,
    // Timeline Semaphores, Descriptor Indexing, and Vulkan 1.2 Buffer Device Address.
    if (candidate->properties.apiVersion < VK_API_VERSION_1_3) {
        candidate->is_disqualified = true;
        snprintf(candidate->disqualification_reason, sizeof(candidate->disqualification_reason),
                 "Driver API v%u.%u.%u is below required Vulkan 1.3 baseline",
                 VK_API_VERSION_MAJOR(candidate->properties.apiVersion),
                 VK_API_VERSION_MINOR(candidate->properties.apiVersion),
                 VK_API_VERSION_PATCH(candidate->properties.apiVersion));
        candidate->score = 0;
        return;
    }

    // Requirement: Graphics Queue
    discover_queue_families(candidate);
    if (!candidate->has_graphics) {
        candidate->is_disqualified = true;
        snprintf(candidate->disqualification_reason, sizeof(candidate->disqualification_reason),
                 "No queue family supports VK_QUEUE_GRAPHICS_BIT");
        candidate->score = 0;
        return;
    }

    // Dynamic Feature Query: Chained safely according to physical device API level
    VkPhysicalDeviceVulkan14Features feat14 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
    };
    VkPhysicalDeviceVulkan13Features feat13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = (candidate->properties.apiVersion >= VK_API_VERSION_1_4) ? &feat14 : nullptr,
    };
    VkPhysicalDeviceVulkan12Features feat12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &feat13,
    };
    VkPhysicalDeviceFeatures2 features2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &feat12,
    };

    vkGetPhysicalDeviceFeatures2(candidate->device, &features2);

    // Enforce modern non-negotiable features
    if (!feat13.dynamicRendering || !feat13.synchronization2 || !feat12.bufferDeviceAddress) {
        candidate->is_disqualified = true;
        snprintf(candidate->disqualification_reason, sizeof(candidate->disqualification_reason),
                 "Missing non-negotiable feature (DynRender=%d, Sync2=%d, BDA=%d)",
                 feat13.dynamicRendering, feat13.synchronization2, feat12.bufferDeviceAddress);
        candidate->score = 0;
        return;
    }

    candidate->supports_dyn_render   = (feat13.dynamicRendering == VK_TRUE);
    candidate->supports_sync2        = (feat13.synchronization2 == VK_TRUE);
    candidate->supports_bda          = (feat12.bufferDeviceAddress == VK_TRUE);
    candidate->supports_maintenance5 = (candidate->properties.apiVersion >= VK_API_VERSION_1_4 &&
                                        feat14.maintenance5 == VK_TRUE);

    // Extension Probing: Linux DMA-BUF, DRM Modifiers, External Syncobj
    candidate->supports_dma_buf = is_device_extension_supported(
        candidate->device, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    candidate->supports_drm_modifiers = is_device_extension_supported(
        candidate->device, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    candidate->supports_syncobj = is_device_extension_supported(
        candidate->device, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);

    // Base scoring by GPU architecture type
    uint32_t score = 1000;
    switch (candidate->properties.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            score += 15000; // Prioritize discrete GPU over integrated GPU
            break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            score += 5000;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            score += 2000;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
        case VK_PHYSICAL_DEVICE_TYPE_OTHER:
        default:
            score += 100;
            break;
    }

    // Engine hardware capabilities bonus
    if (candidate->has_dedicated_compute)  score += 1000;
    if (candidate->has_dedicated_transfer) score += 500;
    if (candidate->supports_dma_buf)       score += 2000;
    if (candidate->supports_drm_modifiers) score += 1000;
    if (candidate->supports_syncobj)       score += 1000;
    if (candidate->supports_maintenance5)  score += 500;

    // VRAM capacity scoring: 500 points per GiB of dedicated local memory
    score += (uint32_t)(total_vram / (1024ULL * 1024ULL * 1024ULL)) * 500;

    candidate->is_disqualified = false;
    candidate->score = score;
}

VulkanResult vulkan_device_create(VulkanContext *ctx) {
    if (ctx == nullptr || ctx->instance == VK_NULL_HANDLE) {
        return VULKAN_ERROR_INVALID_ARGUMENT;
    }

    // 1. Enumerate all connected physical devices
    uint32_t device_count = 0;
    if (vkEnumeratePhysicalDevices(ctx->instance, &device_count, nullptr) != VK_SUCCESS || device_count == 0) {
        fprintf(stderr, "[Vulkan Error] No physical devices found on host system.\n");
        return VULKAN_ERROR_NO_SUITABLE_GPU;
    }

    VkPhysicalDevice *devices = malloc(sizeof(VkPhysicalDevice) * device_count);
    if (devices == nullptr) {
        return VULKAN_ERROR_OUT_OF_HOST_MEMORY;
    }
    vkEnumeratePhysicalDevices(ctx->instance, &device_count, devices);

    GpuCandidate *candidates = calloc(device_count, sizeof(GpuCandidate));
    if (candidates == nullptr) {
        free(devices);
        return VULKAN_ERROR_OUT_OF_HOST_MEMORY;
    }

    printf("[Vulkan Probe] Enumerated %u physical device(s) on host:\n", device_count);

    // 2. Score and log each candidate GPU with full hardware transparency
    for (uint32_t i = 0; i < device_count; ++i) {
        candidates[i].device = devices[i];
        score_physical_device(&candidates[i]);

        GpuCandidate *c = &candidates[i];
        printf("  [%u] %s (%s | Vendor: %s [0x%04x] | DeviceID: 0x%04x)\n",
               i,
               c->properties.deviceName,
               get_device_type_str(c->properties.deviceType),
               get_vendor_name(c->properties.vendorID),
               c->properties.vendorID,
               c->properties.deviceID);

        printf("      Driver: v%u.%u.%u | Vulkan API: v%u.%u.%u | Dedicated VRAM: %.2f GiB\n",
               VK_API_VERSION_MAJOR(c->properties.driverVersion),
               VK_API_VERSION_MINOR(c->properties.driverVersion),
               VK_API_VERSION_PATCH(c->properties.driverVersion),
               VK_API_VERSION_MAJOR(c->properties.apiVersion),
               VK_API_VERSION_MINOR(c->properties.apiVersion),
               VK_API_VERSION_PATCH(c->properties.apiVersion),
               (double)c->dedicated_vram_bytes / (1024.0 * 1024.0 * 1024.0));

        printf("      Queues: Graphics=%s, Compute=%s, Transfer=%s\n",
               c->has_graphics ? "yes" : "no",
               c->has_dedicated_compute ? "dedicated" : "shared",
               c->has_dedicated_transfer ? "dedicated" : "shared");

        printf("      Features: DynRender=%s, Sync2=%s, BDA=%s, Maint5=%s\n",
               c->supports_dyn_render ? "yes" : "no",
               c->supports_sync2 ? "yes" : "no",
               c->supports_bda ? "yes" : "no",
               c->supports_maintenance5 ? "yes" : "no");

        printf("      Extensions: DMA-BUF=%s, DRM-Modifiers=%s, Syncobj=%s\n",
               c->supports_dma_buf ? "yes" : "no",
               c->supports_drm_modifiers ? "yes" : "no",
               c->supports_syncobj ? "yes" : "no");

        if (c->is_disqualified) {
            printf("      Evaluation: \033[1;31mDISQUALIFIED\033[0m (%s) | Score: 0\n",
                   c->disqualification_reason);
        } else {
            printf("      Evaluation: \033[1;32mELIGIBLE\033[0m | Score: %u\n", c->score);
        }
    }

    // 3. Selection evaluation with support for manual environment overrides
    const char *env_gpu_index = getenv("ENGINE_GPU_INDEX");
    const char *env_gpu_match = getenv("ENGINE_GPU");

    int32_t  selected_index = -1;
    uint32_t highest_score  = 0;
    const char *selection_override_note = "";

    // Method A: Explicit index override (ENGINE_GPU_INDEX=0/1/...)
    if (env_gpu_index != nullptr) {
        int idx = atoi(env_gpu_index);
        if (idx >= 0 && (uint32_t)idx < device_count) {
            if (!candidates[idx].is_disqualified) {
                selected_index = idx;
                selection_override_note = " [Forced by ENGINE_GPU_INDEX]";
            } else {
                fprintf(stderr,
                        "[Vulkan Warn] ENGINE_GPU_INDEX=%d requested, but device is disqualified (%s). Falling back.\n",
                        idx, candidates[idx].disqualification_reason);
            }
        } else {
            fprintf(stderr,
                    "[Vulkan Warn] ENGINE_GPU_INDEX=%s is out of range (0..%u). Falling back.\n",
                    env_gpu_index, device_count - 1);
        }
    }

    // Method B: Name or vendor filter override (ENGINE_GPU=nvidia/intel/discrete/integrated)
    if (selected_index == -1 && env_gpu_match != nullptr) {
        for (uint32_t i = 0; i < device_count; ++i) {
            if (candidates[i].is_disqualified) continue;

            const char *vendor = get_vendor_name(candidates[i].properties.vendorID);
            const char *type   = get_device_type_str(candidates[i].properties.deviceType);
            const char *name   = candidates[i].properties.deviceName;

            if (strcasestr(vendor, env_gpu_match) != nullptr ||
                strcasestr(type,   env_gpu_match) != nullptr ||
                strcasestr(name,   env_gpu_match) != nullptr) {
                if (candidates[i].score > highest_score) {
                    highest_score  = candidates[i].score;
                    selected_index = (int32_t)i;
                    selection_override_note = " [Filtered by ENGINE_GPU]";
                }
            }
        }
    }

    // Method C: Automatic scoring selection (highest eligible score)
    if (selected_index == -1) {
        for (uint32_t i = 0; i < device_count; ++i) {
            if (!candidates[i].is_disqualified && candidates[i].score > highest_score) {
                highest_score  = candidates[i].score;
                selected_index = (int32_t)i;
            }
        }
    }

    if (selected_index == -1) {
        fprintf(stderr, "[Vulkan Error] None of the %u available GPUs satisfy engine requirements.\n", device_count);
        free(candidates);
        free(devices);
        return VULKAN_ERROR_NO_SUITABLE_GPU;
    }

    GpuCandidate best_candidate = candidates[selected_index];
    printf("[Vulkan Device] Selected GPU [%d]: %s (%s, Score: %u)%s\n",
           selected_index,
           best_candidate.properties.deviceName,
           get_device_type_str(best_candidate.properties.deviceType),
           best_candidate.score,
           selection_override_note);

    free(candidates);
    free(devices);

    // Store physical device and hardware info in context
    ctx->physical_device = best_candidate.device;
    ctx->queues.graphics_family_index  = best_candidate.graphics_family;
    ctx->queues.compute_family_index   = best_candidate.compute_family;
    ctx->queues.transfer_family_index  = best_candidate.transfer_family;
    ctx->queues.has_dedicated_compute  = best_candidate.has_dedicated_compute;
    ctx->queues.has_dedicated_transfer = best_candidate.has_dedicated_transfer;

    strncpy(ctx->device_info.name, best_candidate.properties.deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
    ctx->device_info.name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1] = '\0';
    ctx->device_info.type                       = best_candidate.properties.deviceType;
    ctx->device_info.vendor_id                  = best_candidate.properties.vendorID;
    ctx->device_info.device_id                  = best_candidate.properties.deviceID;
    ctx->device_info.driver_version             = best_candidate.properties.driverVersion;
    ctx->device_info.api_version                = best_candidate.properties.apiVersion;
    ctx->device_info.memory_properties          = best_candidate.memory_properties;
    ctx->device_info.supports_dma_buf           = best_candidate.supports_dma_buf;
    ctx->device_info.supports_drm_format_modifiers = best_candidate.supports_drm_modifiers;
    ctx->device_info.supports_external_syncobj  = best_candidate.supports_syncobj;
    ctx->device_info.supports_buffer_device_address = best_candidate.supports_bda;
    ctx->device_info.supports_dynamic_rendering = best_candidate.supports_dyn_render;
    ctx->device_info.supports_synchronization2  = best_candidate.supports_sync2;
    ctx->device_info.supports_maintenance5      = best_candidate.supports_maintenance5;

    // 4. Prepare Queue Create Info (with de-duplication)
    uint32_t unique_families[3] = {0};
    uint32_t unique_family_count = 0;

    uint32_t requested_families[3] = {
        ctx->queues.graphics_family_index,
        ctx->queues.compute_family_index,
        ctx->queues.transfer_family_index
    };

    for (uint32_t i = 0; i < 3; ++i) {
        bool already_added = false;
        for (uint32_t j = 0; j < unique_family_count; ++j) {
            if (unique_families[j] == requested_families[i]) {
                already_added = true;
                break;
            }
        }
        if (!already_added) {
            unique_families[unique_family_count++] = requested_families[i];
        }
    }

    const float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_create_infos[3];
    for (uint32_t i = 0; i < unique_family_count; ++i) {
        queue_create_infos[i] = (VkDeviceQueueCreateInfo){
            .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext            = nullptr,
            .flags            = 0,
            .queueFamilyIndex = unique_families[i],
            .queueCount       = 1,
            .pQueuePriorities = &queue_priority,
        };
    }

    // 5. Configure enabled device extensions
    const char *device_extensions[8] = {0};
    uint32_t    device_extension_count = 0;

    // External memory FD is enabled for buffer/image exporting
    if (is_device_extension_supported(ctx->physical_device, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
        device_extensions[device_extension_count++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
    }
    if (best_candidate.supports_dma_buf) {
        device_extensions[device_extension_count++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
    }
    if (best_candidate.supports_drm_modifiers) {
        device_extensions[device_extension_count++] = VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME;
    }
    if (best_candidate.supports_syncobj) {
        device_extensions[device_extension_count++] = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
    }

    // 6. Dynamically build feature chain: Only attach Vulkan 1.4 structs if device is >= 1.4
    VkPhysicalDeviceVulkan14Features feat14 = {
        .sType        = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
        .pNext        = nullptr,
        .maintenance5 = best_candidate.supports_maintenance5 ? VK_TRUE : VK_FALSE,
    };

    VkPhysicalDeviceVulkan13Features feat13 = {
        .sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext            = (best_candidate.properties.apiVersion >= VK_API_VERSION_1_4) ? &feat14 : nullptr,
        .dynamicRendering = VK_TRUE,
        .synchronization2 = VK_TRUE,
    };

    VkPhysicalDeviceVulkan12Features feat12 = {
        .sType               = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext               = &feat13,
        .bufferDeviceAddress = VK_TRUE,
        .timelineSemaphore   = VK_TRUE,
        .descriptorIndexing  = VK_TRUE,
    };

    VkPhysicalDeviceFeatures2 features2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &feat12,
    };

    VkDeviceCreateInfo device_create_info = {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = &features2,
        .flags                   = 0,
        .queueCreateInfoCount    = unique_family_count,
        .pQueueCreateInfos       = queue_create_infos,
        .enabledLayerCount       = 0,
        .ppEnabledLayerNames     = nullptr,
        .enabledExtensionCount   = device_extension_count,
        .ppEnabledExtensionNames = device_extensions,
        .pEnabledFeatures        = nullptr,
    };

    // 7. Create logical device
    VkResult dev_res = vkCreateDevice(ctx->physical_device, &device_create_info, nullptr, &ctx->device);
    if (dev_res != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan Error] vkCreateDevice failed with code %d\n", dev_res);
        return VULKAN_ERROR_DEVICE_CREATION_FAILED;
    }

    // 8. Retrieve queue handles
    vkGetDeviceQueue(ctx->device, ctx->queues.graphics_family_index, 0, &ctx->queues.graphics_queue);
    vkGetDeviceQueue(ctx->device, ctx->queues.compute_family_index,  0, &ctx->queues.compute_queue);
    vkGetDeviceQueue(ctx->device, ctx->queues.transfer_family_index, 0, &ctx->queues.transfer_queue);

    // 9. Dynamically resolve device-level extension function pointers
    ctx->dispatch.vkGetMemoryFdKHR =
        (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(ctx->device, "vkGetMemoryFdKHR");
    ctx->dispatch.vkGetMemoryFdPropertiesKHR =
        (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(ctx->device, "vkGetMemoryFdPropertiesKHR");
    ctx->dispatch.vkGetImageDrmFormatModifierPropertiesEXT =
        (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(ctx->device, "vkGetImageDrmFormatModifierPropertiesEXT");
    ctx->dispatch.vkGetSemaphoreFdKHR =
        (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(ctx->device, "vkGetSemaphoreFdKHR");
    ctx->dispatch.vkImportSemaphoreFdKHR =
        (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(ctx->device, "vkImportSemaphoreFdKHR");

    return VULKAN_SUCCESS;
}

void vulkan_device_destroy(VulkanContext *ctx) {
    if (ctx == nullptr || ctx->device == VK_NULL_HANDLE) {
        return;
    }

    // Commandment #10: Guard against in-flight GPU execution
    vkDeviceWaitIdle(ctx->device);

    vkDestroyDevice(ctx->device, nullptr);
    ctx->device                 = VK_NULL_HANDLE;
    ctx->queues.graphics_queue  = VK_NULL_HANDLE;
    ctx->queues.compute_queue   = VK_NULL_HANDLE;
    ctx->queues.transfer_queue  = VK_NULL_HANDLE;

    // Reset device dispatch entry points
    ctx->dispatch.vkGetMemoryFdKHR                         = nullptr;
    ctx->dispatch.vkGetMemoryFdPropertiesKHR               = nullptr;
    ctx->dispatch.vkGetImageDrmFormatModifierPropertiesEXT = nullptr;
    ctx->dispatch.vkGetSemaphoreFdKHR                      = nullptr;
    ctx->dispatch.vkImportSemaphoreFdKHR                   = nullptr;
}
