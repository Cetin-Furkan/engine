#include "vulkan/core.h"
#include "vulkan/instance.h"
#include "vulkan/device.h"
#include "vulkan/memory.h"
#include "vulkan/bda.h"
#include <stdio.h>

VulkanResult vulkan_context_init(VulkanContext *ctx) {
    if (ctx == nullptr) {
        return VULKAN_ERROR_INVALID_ARGUMENT;
    }
    if (ctx->is_initialized) {
        vulkan_context_shutdown(ctx);
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

    // Step 4: Command pool creation
    res = vulkan_command_pool_create(ctx);
    if (res != VULKAN_SUCCESS) {
        fprintf(stderr, "[Vulkan Error] Command pool creation failed: %s\n",
                vulkan_result_to_str(res));
        vulkan_device_destroy(ctx);
        vulkan_instance_destroy(ctx);
        return res;
    }

    vulkan_device_log(ctx);

    printf("hello init from vulkan (device & command pool active, validation: %s)\n",
           ctx->validation_enabled ? "enabled" : "disabled");

    ctx->is_initialized = true;
    return VULKAN_SUCCESS;
}

void vulkan_context_shutdown(VulkanContext *ctx) {
    if (ctx != nullptr && ctx->is_initialized) {
        // GPU work must finish before command pools or the device are destroyed.
        if (ctx->device != VK_NULL_HANDLE) {
            VkResult wait = vkDeviceWaitIdle(ctx->device);
            if (wait != VK_SUCCESS) {
                fprintf(stderr, "[Vulkan Error] vkDeviceWaitIdle during shutdown: %s (%d)\n",
                        vulkan_vkresult_to_str(wait), (int)wait);
            }
        }
        bda_system_shutdown(ctx);
        vulkan_command_pool_destroy(ctx);
        vulkan_device_destroy(ctx);
        vulkan_instance_destroy(ctx);

        printf("goodbye from vulkan (command pool, device & instance destroyed)\n");
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

VulkanContext *vulkan_get_context(void) {
    return &s_global_ctx;
}