#include "init/vulkan_init/vulkan_init.h"
#include "init/vulkan_init/vulkan_instance.h"
#include "init/vulkan_init/vulkan_device.h"
#include <stdio.h>

VulkanResult vulkan_context_init(VulkanContext *ctx) {
    if (ctx == nullptr) {
        return VULKAN_ERROR_INVALID_ARGUMENT;
    }

    *ctx = (VulkanContext){0};

    // Step 2: Safe instance creation with layer probing & debug messenger
    VulkanResult res = vulkan_instance_create(ctx);
    if (res != VULKAN_SUCCESS) {
        fprintf(stderr, "[Vulkan Error] Instance creation failed: %s\n",
                vulkan_result_to_str(res));
        return res;
    }

    // Step 3: Physical device scoring, queue discovery, and logical device creation
    res = vulkan_device_create(ctx);
    if (res != VULKAN_SUCCESS) {
        fprintf(stderr, "[Vulkan Error] Device creation failed: %s\n",
                vulkan_result_to_str(res));
        vulkan_instance_destroy(ctx);
        return res;
    }

    printf("[Vulkan] Hardware: %s (Driver v%u.%u.%u, API v%u.%u.%u)\n",
           ctx->device_info.name,
           VK_API_VERSION_MAJOR(ctx->device_info.driver_version),
           VK_API_VERSION_MINOR(ctx->device_info.driver_version),
           VK_API_VERSION_PATCH(ctx->device_info.driver_version),
           VK_API_VERSION_MAJOR(ctx->device_info.api_version),
           VK_API_VERSION_MINOR(ctx->device_info.api_version),
           VK_API_VERSION_PATCH(ctx->device_info.api_version));

    printf("[Vulkan] Queues: Graphics=%u, Compute=%u (dedicated: %s), Transfer=%u (dedicated: %s)\n",
           ctx->queues.graphics_family_index,
           ctx->queues.compute_family_index,
           ctx->queues.has_dedicated_compute ? "yes" : "no",
           ctx->queues.transfer_family_index,
           ctx->queues.has_dedicated_transfer ? "yes" : "no");

    printf("[Vulkan] Capabilities: DMA-BUF=%s, DRM-Modifiers=%s, Syncobj=%s, BDA=%s, DynRender=%s, Sync2=%s\n",
           ctx->device_info.supports_dma_buf ? "yes" : "no",
           ctx->device_info.supports_drm_format_modifiers ? "yes" : "no",
           ctx->device_info.supports_external_syncobj ? "yes" : "no",
           ctx->device_info.supports_buffer_device_address ? "yes" : "no",
           ctx->device_info.supports_dynamic_rendering ? "yes" : "no",
           ctx->device_info.supports_synchronization2 ? "yes" : "no");

    printf("hello init from vulkan (device active, validation: %s)\n",
           ctx->validation_enabled ? "enabled" : "disabled");

    ctx->is_initialized = true;
    return VULKAN_SUCCESS;
}

void vulkan_context_shutdown(VulkanContext *ctx) {
    if (ctx != nullptr && ctx->is_initialized) {
        // Step 3 symmetric destruction (device wait idle + destroy):
        vulkan_device_destroy(ctx);

        // Step 2 symmetric destruction (debug messenger + instance):
        vulkan_instance_destroy(ctx);

        printf("goodbye from vulkan (device & instance destroyed)\n");
        ctx->is_initialized = false;
    }
}

static VulkanContext s_global_ctx = {0};

int vulkan_init(void) {
    return (vulkan_context_init(&s_global_ctx) == VULKAN_SUCCESS) ? 0 : 1;
}

void vulkan_shutdown(void) {
    vulkan_context_shutdown(&s_global_ctx);
}
