#ifndef ENGINE_VULKAN_TYPES_H
#define ENGINE_VULKAN_TYPES_H

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
    VULKAN_ERROR_FORMAT_NOT_SUPPORTED,
    VULKAN_ERROR_UNKNOWN
} VulkanResult;

/**
 * @brief Converts a VulkanResult enum into a human-readable string.
 * @param res The status or error code.
 * @return A statically allocated string describing the result.
 */
[[nodiscard]] const char *vulkan_result_to_str(VulkanResult res);

/**
 * @brief Stable name for a VkResult. Unknown codes return "unrecognized VkResult".
 */
[[nodiscard]] const char *vulkan_vkresult_to_str(VkResult result);

/**
 * @brief Maps a VkResult onto the engine's VulkanResult. VK_SUCCESS maps to VULKAN_SUCCESS.
 */
[[nodiscard]] VulkanResult vulkan_result_from_vkresult(VkResult result);

/**
 * @brief Writes a driver version into buf.
 *
 * NVIDIA packs driverVersion differently from VK_API_VERSION_*. Other vendors
 * use the Khronos packing.
 */
void vulkan_format_driver_version(uint32_t vendor_id, uint32_t driver_version,
                                  char *buf, size_t buf_size);

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
 *
 * A role keeps its own queue index when its family has one queue per role.
 * submission_aliased is set when two roles still received the same VkQueue.
 * Those handles are externally synchronized: do not submit them at once.
 */
typedef struct VulkanQueueInfo {
    VkQueue  graphics_queue;
    uint32_t graphics_family_index;
    uint32_t graphics_queue_index;

    VkQueue  compute_queue;
    uint32_t compute_family_index;
    uint32_t compute_queue_index;

    VkQueue  transfer_queue;
    uint32_t transfer_family_index;
    uint32_t transfer_queue_index;

    bool     has_dedicated_compute;
    bool     has_dedicated_transfer;
    bool     submission_aliased;
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

    // Features and extensions enabled on the logical device. A true flag was
    // reported by the physical device and turned on at vkCreateDevice.
    bool supports_dma_buf;
    bool supports_drm_format_modifiers;
    bool supports_external_syncobj;
    bool supports_buffer_device_address;
    bool supports_scalar_block_layout;
    bool supports_sampler_anisotropy;
    bool supports_dynamic_rendering;
    bool supports_synchronization2;
    bool supports_timeline_semaphore;
    bool supports_maintenance5;
    bool supports_maintenance6;
    bool supports_push_descriptors;

    // Linux DRM device node identity (VK_EXT_physical_device_drm) for Wayland matching.
    bool    has_drm_primary_node;
    bool    has_drm_render_node;
    int64_t drm_primary_major;
    int64_t drm_primary_minor;
    int64_t drm_render_major;
    int64_t drm_render_minor;
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
    VkCommandPool            cmd_pool;

    VulkanQueueInfo          queues;
    VulkanDeviceInfo         device_info;
    VulkanDispatchTable      dispatch;

    bool                     validation_enabled;
    bool                     is_initialized;
} VulkanContext;

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_VULKAN_TYPES_H */