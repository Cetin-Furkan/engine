#ifndef VULKAN_TYPES_H
#define VULKAN_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief High-granularity status and error codes for the Vulkan subsystem.
 */
typedef enum VulkanResult {
    VULKAN_SUCCESS = 0,
    VULKAN_ERROR_INVALID_ARGUMENT,
    VULKAN_ERROR_OUT_OF_HOST_MEMORY,
    VULKAN_ERROR_OUT_OF_DEVICE_MEMORY,
    VULKAN_ERROR_INSTANCE_CREATION_FAILED,
    VULKAN_ERROR_LAYER_NOT_SUPPORTED,
    VULKAN_ERROR_DEBUG_MESSENGER_CREATION_FAILED,
    VULKAN_ERROR_NO_SUITABLE_GPU,
    VULKAN_ERROR_REQUIRED_EXTENSION_MISSING,
    VULKAN_ERROR_REQUIRED_FEATURE_MISSING,
    VULKAN_ERROR_DEVICE_CREATION_FAILED,
    VULKAN_ERROR_QUEUE_NOT_FOUND,
    VULKAN_ERROR_MEMORY_TYPE_NOT_FOUND,
    VULKAN_ERROR_FUNCTION_POINTER_MISSING,
    VULKAN_ERROR_INITIALIZATION_FAILED,
    VULKAN_ERROR_UNKNOWN
} VulkanResult;

/**
 * @brief Converts a VulkanResult enum into a human-readable string.
 * @param res The status or error code.
 * @return A statically allocated string describing the result.
 */
[[nodiscard]] const char *vulkan_result_to_str(VulkanResult res);

/**
 * @brief Table of dynamically resolved non-static Vulkan extension function pointers.
 *
 * These function pointers are loaded via vkGetInstanceProcAddr or vkGetDeviceProcAddr
 * to ensure bulletproof portability and prevent unresolved symbol errors or
 * null-pointer crashes across different Vulkan loader implementations.
 */
typedef struct VulkanDispatchTable {
    // --- Instance-Level Extensions: Debug Utils (VK_EXT_debug_utils) ---
    PFN_vkCreateDebugUtilsMessengerEXT  vkCreateDebugUtilsMessengerEXT;
    PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT;

    // --- Device-Level Extensions: Linux DMA-BUF (VK_KHR_external_memory_fd) ---
    PFN_vkGetMemoryFdKHR                vkGetMemoryFdKHR;
    PFN_vkGetMemoryFdPropertiesKHR      vkGetMemoryFdPropertiesKHR;

    // --- Device-Level Extensions: DRM Format Modifiers (VK_EXT_image_drm_format_modifier) ---
    PFN_vkGetImageDrmFormatModifierPropertiesEXT vkGetImageDrmFormatModifierPropertiesEXT;

    // --- Device-Level Extensions: Linux DRM Syncobj (VK_KHR_external_semaphore_fd) ---
    PFN_vkGetSemaphoreFdKHR             vkGetSemaphoreFdKHR;
    PFN_vkImportSemaphoreFdKHR          vkImportSemaphoreFdKHR;
} VulkanDispatchTable;

/**
 * @brief Queue handles and queue family indices discovered on the hardware.
 */
typedef struct VulkanQueueInfo {
    VkQueue  graphics_queue;
    uint32_t graphics_family_index;

    VkQueue  compute_queue;
    uint32_t compute_family_index;

    VkQueue  transfer_queue;
    uint32_t transfer_family_index;

    bool     has_dedicated_compute;
    bool     has_dedicated_transfer;
} VulkanQueueInfo;

/**
 * @brief Hardware identity and verified capability flags for the selected physical device.
 */
typedef struct VulkanDeviceInfo {
    char                   name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    VkPhysicalDeviceType   type;
    uint32_t               vendor_id;
    uint32_t               device_id;
    uint32_t               driver_version;
    uint32_t               api_version;

    VkPhysicalDeviceMemoryProperties memory_properties;

    // Validated Hardware Feature Capabilities
    bool supports_dma_buf;
    bool supports_drm_format_modifiers;
    bool supports_external_syncobj;
    bool supports_buffer_device_address;
    bool supports_dynamic_rendering;
    bool supports_synchronization2;
    bool supports_maintenance5;
    bool supports_maintenance6;
    bool supports_push_descriptors;
} VulkanDeviceInfo;

/**
 * @brief Master Vulkan subsystem state context.
 *
 * Encapsulates all Vulkan handles, queue dispatches, device properties,
 * and dynamic function pointers required for the engine.
 */
typedef struct VulkanContext {
    VkInstance               instance;
    VkDebugUtilsMessengerEXT debug_messenger;
    VkPhysicalDevice         physical_device;
    VkDevice                 device;

    VulkanQueueInfo          queues;
    VulkanDeviceInfo         device_info;
    VulkanDispatchTable      dispatch;

    bool                     validation_enabled;
    bool                     is_initialized;
} VulkanContext;

#ifdef __cplusplus
}
#endif

#endif /* VULKAN_TYPES_H */
