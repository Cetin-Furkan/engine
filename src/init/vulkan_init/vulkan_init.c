#include "init/vulkan_init/vulkan_init.h"
#include "init/vulkan_init/vulkan_instance.h"
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

    printf("hello init from vulkan (instance created, validation: %s)\n",
           ctx->validation_enabled ? "enabled" : "disabled");

    ctx->is_initialized = true;
    return VULKAN_SUCCESS;
}

void vulkan_context_shutdown(VulkanContext *ctx) {
    if (ctx != nullptr && ctx->is_initialized) {
        // Step 2 symmetric destruction:
        vulkan_instance_destroy(ctx);
        printf("goodbye from vulkan (instance destroyed)\n");
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
