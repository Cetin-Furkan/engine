#include "init/vulkan_init/vulkan_types.h"

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
        case VULKAN_ERROR_UNKNOWN:
        default:
            return "Unknown Vulkan error";
    }
}
