#include "init/vulkan_init/vulkan_device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
 * @brief Candidate evaluation record for GPU selection.
 */
typedef struct GpuCandidate {
    VkPhysicalDevice         device;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory_properties;
    uint32_t                 score;

    uint32_t                 graphics_family;
    uint32_t                 compute_family;
    uint32_t                 transfer_family;
    bool                     has_graphics;
    bool                     has_dedicated_compute;
    bool                     has_dedicated_transfer;

    bool                     supports_dma_buf;
    bool                     supports_drm_modifiers;
    bool                     supports_syncobj;
    bool                     supports_bda;
    bool                     supports_dyn_render;
    bool                     supports_sync2;
    bool                     supports_maintenance5;
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

    // Rule 1: Must support Vulkan 1.4 baseline
    if (candidate->properties.apiVersion < VK_API_VERSION_1_4) {
        candidate->score = 0;
        return;
    }

    // Rule 2: Must support a Graphics Queue
    discover_queue_families(candidate);
    if (!candidate->has_graphics) {
        candidate->score = 0;
        return;
    }

    // Rule 3: Query Vulkan 1.2, 1.3, and 1.4 core features
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
    VkPhysicalDeviceFeatures2 features2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &feat12,
    };

    vkGetPhysicalDeviceFeatures2(candidate->device, &features2);

    // Enforce modern non-negotiable features
    if (!feat13.dynamicRendering || !feat13.synchronization2 || !feat12.bufferDeviceAddress) {
        candidate->score = 0;
        return;
    }

    candidate->supports_dyn_render    = (feat13.dynamicRendering == VK_TRUE);
    candidate->supports_sync2         = (feat13.synchronization2 == VK_TRUE);
    candidate->supports_bda           = (feat12.bufferDeviceAddress == VK_TRUE);
    candidate->supports_maintenance5  = (feat14.maintenance5 == VK_TRUE);

    // Rule 4: Probe Linux DMA-BUF, DRM format modifiers, and syncobj extensions
    candidate->supports_dma_buf = is_device_extension_supported(
        candidate->device, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    candidate->supports_drm_modifiers = is_device_extension_supported(
        candidate->device, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    candidate->supports_syncobj = is_device_extension_supported(
        candidate->device, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);

    // Base score by GPU type
    uint32_t score = 1000;
    switch (candidate->properties.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            score += 10000;
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

    // Bonus for dedicated async compute and transfer engines
    if (candidate->has_dedicated_compute)  score += 500;
    if (candidate->has_dedicated_transfer) score += 300;

    // Bonus for direct DMA-BUF and syncobj presentation support
    if (candidate->supports_dma_buf)       score += 2000;
    if (candidate->supports_drm_modifiers) score += 1000;
    if (candidate->supports_syncobj)       score += 1000;

    // VRAM capacity tie-breaker
    uint64_t total_device_local_vram = 0;
    for (uint32_t i = 0; i < candidate->memory_properties.memoryHeapCount; ++i) {
        if (candidate->memory_properties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            total_device_local_vram += candidate->memory_properties.memoryHeaps[i].size;
        }
    }
    score += (uint32_t)(total_device_local_vram / (1024ULL * 1024ULL * 1024ULL)) * 100;

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

    // 2. Score each candidate GPU
    GpuCandidate best_candidate = {0};
    uint32_t     highest_score  = 0;

    for (uint32_t i = 0; i < device_count; ++i) {
        GpuCandidate candidate = {.device = devices[i]};
        score_physical_device(&candidate);

        if (candidate.score > highest_score) {
            highest_score  = candidate.score;
            best_candidate = candidate;
        }
    }

    free(devices);

    if (highest_score == 0 || best_candidate.device == VK_NULL_HANDLE) {
        fprintf(stderr, "[Vulkan Error] None of the %u available GPUs satisfy Vulkan 1.4 requirements.\n", device_count);
        return VULKAN_ERROR_NO_SUITABLE_GPU;
    }

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

    // 3. Prepare Queue Create Info (with de-duplication)
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

    // 4. Configure enabled device extensions
    const char *device_extensions[8] = {0};
    uint32_t    device_extension_count = 0;

    // External memory FD is always enabled for buffer/image exporting
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

    // 5. Build modern Vulkan 1.4 feature chain via pNext
    VkPhysicalDeviceVulkan14Features feat14 = {
        .sType        = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
        .pNext        = nullptr,
        .maintenance5 = best_candidate.supports_maintenance5 ? VK_TRUE : VK_FALSE,
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
        .enabledLayerCount       = 0,       // Deprecated and ignored in modern Vulkan
        .ppEnabledLayerNames     = nullptr,
        .enabledExtensionCount   = device_extension_count,
        .ppEnabledExtensionNames = device_extensions,
        .pEnabledFeatures        = nullptr, // Fully managed via features2.pNext
    };

    // 6. Create logical device
    VkResult dev_res = vkCreateDevice(ctx->physical_device, &device_create_info, nullptr, &ctx->device);
    if (dev_res != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan Error] vkCreateDevice failed with code %d\n", dev_res);
        return VULKAN_ERROR_DEVICE_CREATION_FAILED;
    }

    // 7. Retrieve queue handles
    vkGetDeviceQueue(ctx->device, ctx->queues.graphics_family_index, 0, &ctx->queues.graphics_queue);
    vkGetDeviceQueue(ctx->device, ctx->queues.compute_family_index,  0, &ctx->queues.compute_queue);
    vkGetDeviceQueue(ctx->device, ctx->queues.transfer_family_index, 0, &ctx->queues.transfer_queue);

    // 8. Dynamically resolve device-level extension function pointers
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
