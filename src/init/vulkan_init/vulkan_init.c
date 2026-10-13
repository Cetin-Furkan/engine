#include "init/vulkan_init/vulkan_init.h"
#include <stdio.h>

VulkanResult vulkan_context_init(VulkanContext *ctx) {
    if (ctx == nullptr) {
        return VULKAN_ERROR_INVALID_ARGUMENT;
    }
    *ctx = (VulkanContext){0};
    printf("hello init from vulkan\n");
    ctx->is_initialized = true;
    return VULKAN_SUCCESS;
}

void vulkan_context_shutdown(VulkanContext *ctx) {
    if (ctx != nullptr && ctx->is_initialized) {
        printf("goodbye from vulkan\n");
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
