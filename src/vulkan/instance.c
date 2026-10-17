#include "vulkan/instance.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(ENGINE_DEBUG)
// Two installs of VkLayer_khronos_validation.json make the loader emit a
// warning through the debug messenger. VK_LAYER_PATH replaces the explicit
// manifest search. Implicit layers stay on their own path.
static void prefer_one_validation_manifest(void) {
    if (getenv("VK_LAYER_PATH") != nullptr) {
        return;
    }

    const char *local_dir = "/usr/local/share/vulkan/explicit_layer.d";
    const char *local_json = "/usr/local/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json";
    const char *distro_json = "/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json";
    if (access(local_json, R_OK) != 0 || access(distro_json, R_OK) != 0) {
        return;
    }

    char path[512];
    size_t used = 0;
    path[0] = '\0';

    const char *home = getenv("HOME");
    char home_dir[256];
    if (home != nullptr && home[0] != '\0') {
        int n = snprintf(home_dir, sizeof home_dir, "%s/.config/vulkan/explicit_layer.d", home);
        if (n > 0 && (size_t)n < sizeof home_dir && access(home_dir, R_OK) == 0) {
            int wrote = snprintf(path + used, sizeof path - used, "%s%s",
                                 used == 0 ? "" : ":", home_dir);
            if (wrote > 0 && (size_t)wrote < sizeof path - used) {
                used += (size_t)wrote;
            }
        }
    }

    const char *keep[] = {
        "/etc/vulkan/explicit_layer.d",
        local_dir,
    };
    for (size_t i = 0; i < sizeof keep / sizeof keep[0]; ++i) {
        if (access(keep[i], R_OK) != 0) {
            continue;
        }
        int wrote = snprintf(path + used, sizeof path - used, "%s%s",
                             used == 0 ? "" : ":", keep[i]);
        if (wrote < 0 || (size_t)wrote >= sizeof path - used) {
            return;
        }
        used += (size_t)wrote;
    }

    if (used > 0) {
        setenv("VK_LAYER_PATH", path, 0);
    }
}
#endif

#if defined(ENGINE_DEBUG)
/**
 * @brief Checks if a specific Vulkan instance layer is installed on the host.
 */
static bool is_layer_supported(const char *layer_name) {
    if (layer_name == nullptr) {
        return false;
    }

    uint32_t layer_count = 0;
    if (vkEnumerateInstanceLayerProperties(&layer_count, nullptr) != VK_SUCCESS || layer_count == 0) {
        return false;
    }

    VkLayerProperties *available_layers = malloc(sizeof(VkLayerProperties) * layer_count);
    if (available_layers == nullptr) {
        fprintf(stderr, "[Vulkan Error] Out of host memory while probing instance layers\n");
        return false;
    }

    if (vkEnumerateInstanceLayerProperties(&layer_count, available_layers) != VK_SUCCESS) {
        free(available_layers);
        return false;
    }

    bool found = false;
    for (uint32_t i = 0; i < layer_count; ++i) {
        if (strncmp(available_layers[i].layerName, layer_name, VK_MAX_EXTENSION_NAME_SIZE) == 0) {
            found = true;
            break;
        }
    }

    free(available_layers);
    return found;
}

/**
 * @brief Checks if a specific Vulkan instance extension is supported by the host loader.
 */
static bool is_extension_supported(const char *extension_name) {
    if (extension_name == nullptr) {
        return false;
    }

    uint32_t extension_count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr) != VK_SUCCESS || extension_count == 0) {
        return false;
    }

    VkExtensionProperties *available_exts = malloc(sizeof(VkExtensionProperties) * extension_count);
    if (available_exts == nullptr) {
        fprintf(stderr, "[Vulkan Error] Out of host memory while probing instance extensions\n");
        return false;
    }

    if (vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, available_exts) != VK_SUCCESS) {
        free(available_exts);
        return false;
    }

    bool found = false;
    for (uint32_t i = 0; i < extension_count; ++i) {
        if (strncmp(available_exts[i].extensionName, extension_name, VK_MAX_EXTENSION_NAME_SIZE) == 0) {
            found = true;
            break;
        }
    }

    free(available_exts);
    return found;
}

/**
 * @brief Colorized terminal callback for Vulkan validation layer diagnostics.
 */
static VKAPI_ATTR VkBool32 VKAPI_CALL vulkan_debug_messenger_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT           message_severity,
    VkDebugUtilsMessageTypeFlagsEXT                  message_type,
    const VkDebugUtilsMessengerCallbackDataEXT*     callback_data,
    void*                                            user_data
) {
    (void)user_data;

    const char *prefix_color = "\033[0m";
    const char *prefix_str   = "[Vulkan]";

    if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        prefix_color = "\033[1;31m"; // Bold Red
        prefix_str   = "[Vulkan Error]";
    } else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        prefix_color = "\033[1;33m"; // Bold Yellow
        prefix_str   = "[Vulkan Warn]";
    } else if (message_type & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) {
        prefix_color = "\033[1;35m"; // Bold Magenta
        prefix_str   = "[Vulkan Perf]";
    } else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        prefix_color = "\033[0;36m"; // Cyan
        prefix_str   = "[Vulkan Info]";
    }

    const char *id_name = (callback_data && callback_data->pMessageIdName)
                              ? callback_data->pMessageIdName
                              : "General";
    const char *msg     = (callback_data && callback_data->pMessage)
                              ? callback_data->pMessage
                              : "(No message)";

    fprintf(stderr, "%s%s\033[0m (%s): %s\n", prefix_color, prefix_str, id_name, msg);

    // Vulkan specification states applications must return VK_FALSE
    return VK_FALSE;
}

/**
 * @brief Configures standard debug utils messenger parameters.
 */
static void setup_debug_messenger_create_info(VkDebugUtilsMessengerCreateInfoEXT *info) {
    *info = (VkDebugUtilsMessengerCreateInfoEXT){
        .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext           = nullptr,
        .flags           = 0,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = vulkan_debug_messenger_callback,
        .pUserData       = nullptr,
    };
}
#endif /* ENGINE_DEBUG */

VulkanResult vulkan_instance_create(VulkanContext *ctx) {
    if (ctx == nullptr) {
        return VULKAN_ERROR_INVALID_ARGUMENT;
    }

#if defined(ENGINE_DEBUG)
    prefer_one_validation_manifest();
#endif

    // 1. Verify host loader supports Vulkan 1.4+
    uint32_t loader_version = 0;
    VkResult version_res = vkEnumerateInstanceVersion(&loader_version);
    if (version_res != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan Error] vkEnumerateInstanceVersion failed: %s (%d)\n",
                vulkan_vkresult_to_str(version_res), (int)version_res);
        return VULKAN_ERROR_INSTANCE_CREATION_FAILED;
    }
    if (loader_version < VK_API_VERSION_1_4) {
        fprintf(stderr,
                "[Vulkan Error] Host loader reports version %u.%u.%u, but engine requires Vulkan 1.4+\n",
                VK_API_VERSION_MAJOR(loader_version),
                VK_API_VERSION_MINOR(loader_version),
                VK_API_VERSION_PATCH(loader_version));
        return VULKAN_ERROR_INSTANCE_CREATION_FAILED;
    }

    // 2. Configure application descriptor
    VkApplicationInfo app_info = {
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext              = nullptr,
        .pApplicationName   = "Antigravity Engine",
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .pEngineName        = "Antigravity",
        .engineVersion      = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .apiVersion         = VK_API_VERSION_1_4,
    };

    // 3. Configure layer selection with safe probing
    const char *enabled_layers[4] = {0};
    uint32_t    enabled_layer_count = 0;
    bool        enable_validation   = false;

#if defined(ENGINE_DEBUG)
    const char *validation_layer = "VK_LAYER_KHRONOS_validation";
    if (is_layer_supported(validation_layer)) {
        enabled_layers[enabled_layer_count++] = validation_layer;
        enable_validation = true;
    } else {
        fprintf(stderr,
                "[Vulkan Notice] Validation layer '%s' not found on host. Running without validation.\n",
                validation_layer);
    }
#endif

    // 4. Configure extension selection with safe probing (strictly ZERO WSI extensions)
    const char *enabled_extensions[4] = {0};
    uint32_t    enabled_extension_count = 0;

#if defined(ENGINE_DEBUG)
    bool enable_debug_utils = false;
    const char *debug_utils_ext = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    if (is_extension_supported(debug_utils_ext)) {
        enabled_extensions[enabled_extension_count++] = debug_utils_ext;
        enable_debug_utils = true;
    }
#endif

    // 5. Setup instance create info with optional pre-instance debug hook
    VkInstanceCreateInfo instance_info = {
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext                   = nullptr,
        .flags                   = 0,
        .pApplicationInfo        = &app_info,
        .enabledLayerCount       = enabled_layer_count,
        .ppEnabledLayerNames     = enabled_layers,
        .enabledExtensionCount   = enabled_extension_count,
        .ppEnabledExtensionNames = enabled_extensions,
    };

#if defined(ENGINE_DEBUG)
    VkDebugUtilsMessengerCreateInfoEXT debug_create_info = {0};
    if (enable_debug_utils) {
        setup_debug_messenger_create_info(&debug_create_info);
        instance_info.pNext = &debug_create_info;
    }
#endif

    // 6. Create the instance
    VkResult result = vkCreateInstance(&instance_info, nullptr, &ctx->instance);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan Error] vkCreateInstance failed: %s (%d)\n",
                vulkan_vkresult_to_str(result), (int)result);
        if (result == VK_ERROR_LAYER_NOT_PRESENT) {
            return VULKAN_ERROR_LAYER_NOT_SUPPORTED;
        }
        if (result == VK_ERROR_EXTENSION_NOT_PRESENT) {
            return VULKAN_ERROR_REQUIRED_EXTENSION_MISSING;
        }
        VulkanResult mapped = vulkan_result_from_vkresult(result);
        return mapped == VULKAN_ERROR_UNKNOWN ? VULKAN_ERROR_INSTANCE_CREATION_FAILED : mapped;
    }

    ctx->validation_enabled = enable_validation;

    // 7. Dynamically resolve debug utils functions and install standalone messenger
#if defined(ENGINE_DEBUG)
    if (enable_debug_utils) {
        ctx->dispatch.vkCreateDebugUtilsMessengerEXT =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                ctx->instance, "vkCreateDebugUtilsMessengerEXT");
        ctx->dispatch.vkDestroyDebugUtilsMessengerEXT =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                ctx->instance, "vkDestroyDebugUtilsMessengerEXT");

        if (ctx->dispatch.vkCreateDebugUtilsMessengerEXT != nullptr) {
            VkResult msg_res = ctx->dispatch.vkCreateDebugUtilsMessengerEXT(
                ctx->instance, &debug_create_info, nullptr, &ctx->debug_messenger);
            if (msg_res != VK_SUCCESS) {
                fprintf(stderr,
                        "[Vulkan Warn] Failed to register persistent debug messenger (code %d)\n",
                        msg_res);
                ctx->debug_messenger = VK_NULL_HANDLE;
            }
        }
    }
#endif

    return VULKAN_SUCCESS;
}

void vulkan_instance_destroy(VulkanContext *ctx) {
    if (ctx == nullptr || ctx->instance == VK_NULL_HANDLE) {
        return;
    }

    // Symmetric LIFO destruction:
    // 1. Destroy debug messenger before the instance
    if (ctx->debug_messenger != VK_NULL_HANDLE) {
        if (ctx->dispatch.vkDestroyDebugUtilsMessengerEXT != nullptr) {
            ctx->dispatch.vkDestroyDebugUtilsMessengerEXT(ctx->instance, ctx->debug_messenger, nullptr);
        }
        ctx->debug_messenger = VK_NULL_HANDLE;
    }

    // 2. Destroy the instance
    vkDestroyInstance(ctx->instance, nullptr);
    ctx->instance = VK_NULL_HANDLE;

    // Reset dispatch entry points
    ctx->dispatch.vkCreateDebugUtilsMessengerEXT  = nullptr;
    ctx->dispatch.vkDestroyDebugUtilsMessengerEXT = nullptr;
    ctx->validation_enabled                       = false;
}
