#include "vulkan/types.h"
#include <stdio.h>

const char *vulkan_result_to_str(VulkanResult res) {
    switch (res) {
        case VULKAN_SUCCESS:
            return "Success";
        case VULKAN_ERROR_INVALID_ARGUMENT:
            return "Invalid argument passed to Vulkan subsystem";
        case VULKAN_ERROR_OUT_OF_HOST_MEMORY:
            return "Host CPU out of memory";
        case VULKAN_ERROR_OUT_OF_DEVICE_MEMORY:
            return "Device GPU VRAM out of memory";
        case VULKAN_ERROR_INSTANCE_CREATION_FAILED:
            return "Failed to create Vulkan 1.4 instance";
        case VULKAN_ERROR_LAYER_NOT_SUPPORTED:
            return "Requested validation layer is not available on host system";
        case VULKAN_ERROR_DEBUG_MESSENGER_CREATION_FAILED:
            return "Failed to initialize debug utils messenger";
        case VULKAN_ERROR_NO_SUITABLE_GPU:
            return "No suitable Vulkan 1.4 physical device found on host system";
        case VULKAN_ERROR_REQUIRED_EXTENSION_MISSING:
            return "GPU driver is missing a required hardware extension";
        case VULKAN_ERROR_REQUIRED_FEATURE_MISSING:
            return "GPU driver is missing a required core Vulkan feature";
        case VULKAN_ERROR_DEVICE_CREATION_FAILED:
            return "Failed to create Vulkan logical device";
        case VULKAN_ERROR_QUEUE_NOT_FOUND:
            return "Required queue family (Graphics/Compute) not found on GPU";
        case VULKAN_ERROR_MEMORY_TYPE_NOT_FOUND:
            return "No matching VRAM memory type index found for allocation";
        case VULKAN_ERROR_FUNCTION_POINTER_MISSING:
            return "Failed to dynamically resolve extension function pointer";
        case VULKAN_ERROR_INITIALIZATION_FAILED:
            return "General initialization failure in Vulkan subsystem";
        case VULKAN_ERROR_FORMAT_NOT_SUPPORTED:
            return "Image format, tiling, or usage is not supported for this allocation";
        case VULKAN_ERROR_UNKNOWN:
        default:
            return "Unknown Vulkan error";
    }
}

const char *vulkan_vkresult_to_str(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
        case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
        case VK_ERROR_FRAGMENTATION: return "VK_ERROR_FRAGMENTATION";
        case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS: return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
        default: return "unrecognized VkResult";
    }
}

VulkanResult vulkan_result_from_vkresult(VkResult result) {
    switch (result) {
        case VK_SUCCESS:
            return VULKAN_SUCCESS;
        case VK_ERROR_OUT_OF_HOST_MEMORY:
            return VULKAN_ERROR_OUT_OF_HOST_MEMORY;
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            return VULKAN_ERROR_OUT_OF_DEVICE_MEMORY;
        case VK_ERROR_LAYER_NOT_PRESENT:
            return VULKAN_ERROR_LAYER_NOT_SUPPORTED;
        case VK_ERROR_EXTENSION_NOT_PRESENT:
            return VULKAN_ERROR_REQUIRED_EXTENSION_MISSING;
        case VK_ERROR_FEATURE_NOT_PRESENT:
            return VULKAN_ERROR_REQUIRED_FEATURE_MISSING;
        case VK_ERROR_FORMAT_NOT_SUPPORTED:
            return VULKAN_ERROR_FORMAT_NOT_SUPPORTED;
        case VK_ERROR_INITIALIZATION_FAILED:
        case VK_ERROR_DEVICE_LOST:
        case VK_ERROR_INCOMPATIBLE_DRIVER:
            return VULKAN_ERROR_INITIALIZATION_FAILED;
        default:
            return VULKAN_ERROR_UNKNOWN;
    }
}

void vulkan_format_driver_version(uint32_t vendor_id, uint32_t driver_version,
                                  char *buf, size_t buf_size) {
    if (buf == nullptr || buf_size == 0) {
        return;
    }

    // NVIDIA stores major in 10 bits, then 8, 8, and 6. VK_API_VERSION_MAJOR keeps 7 bits.
    if (vendor_id == 0x10DEu) {
        snprintf(buf, buf_size, "%u.%u.%u.%u",
                 (driver_version >> 22) & 0x3FFu,
                 (driver_version >> 14) & 0xFFu,
                 (driver_version >> 6) & 0xFFu,
                 driver_version & 0x3Fu);
        return;
    }

    snprintf(buf, buf_size, "%u.%u.%u",
             VK_API_VERSION_MAJOR(driver_version),
             VK_API_VERSION_MINOR(driver_version),
             VK_API_VERSION_PATCH(driver_version));
}
