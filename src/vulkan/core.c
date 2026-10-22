#include "vulkan/core.h"
#include "vulkan/instance.h"
#include "vulkan/device.h"
#include "vulkan/memory.h"
#include <stdio.h>

static const char *queue_place(uint32_t graphics_family, uint32_t graphics_index,
                               uint32_t family, uint32_t index, bool dedicated) {
    if (family != graphics_family) {
        return dedicated ? "dedicated family" : "separate family";
    }
    if (index != graphics_index) {
        return "distinct queue";
    }
    return "aliased";
}

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

    char driver_version[64];
    vulkan_format_driver_version(ctx->device_info.vendor_id, ctx->device_info.driver_version,
                                 driver_version, sizeof driver_version);
    printf("[Vulkan] Hardware: %s (Driver v%s, API v%u.%u.%u)\n",
           ctx->device_info.name,
           driver_version,
           VK_API_VERSION_MAJOR(ctx->device_info.api_version),
           VK_API_VERSION_MINOR(ctx->device_info.api_version),
           VK_API_VERSION_PATCH(ctx->device_info.api_version));

    printf("[Vulkan] Queues: Graphics=family %u index %u, Compute=family %u index %u (%s), Transfer=family %u index %u (%s)\n",
           ctx->queues.graphics_family_index,
           ctx->queues.graphics_queue_index,
           ctx->queues.compute_family_index,
           ctx->queues.compute_queue_index,
           queue_place(ctx->queues.graphics_family_index, ctx->queues.graphics_queue_index,
                       ctx->queues.compute_family_index, ctx->queues.compute_queue_index,
                       ctx->queues.has_dedicated_compute),
           ctx->queues.transfer_family_index,
           ctx->queues.transfer_queue_index,
           queue_place(ctx->queues.graphics_family_index, ctx->queues.graphics_queue_index,
                       ctx->queues.transfer_family_index, ctx->queues.transfer_queue_index,
                       ctx->queues.has_dedicated_transfer));

    if (ctx->queues.submission_aliased) {
        fflush(stdout);
        fprintf(stderr,
                "[Vulkan Warn] Two roles share one VkQueue. Concurrent submits on those roles must be externally synchronized.\n");
    }

    printf("[Vulkan] Capabilities: DMA-BUF=%s, DRM-Modifiers=%s, ForeignQueue=%s, Syncobj=%s, BDA=%s, ScalarLayout=%s, Anisotropy=%s, DynRender=%s, LocalRead=%s, Sync2=%s, Timeline=%s, Maint5=%s, Maint6=%s, PushDesc=%s, HostImageCopy=%s, Int16=%s, Int64=%s, Storage16=%s\n",
           ctx->device_info.supports_dma_buf ? "yes" : "no",
           ctx->device_info.supports_drm_format_modifiers ? "yes" : "no",
           ctx->device_info.supports_queue_family_foreign ? "yes" : "no",
           ctx->device_info.supports_external_syncobj ? "yes" : "no",
           ctx->device_info.supports_buffer_device_address ? "yes" : "no",
           ctx->device_info.supports_scalar_block_layout ? "yes" : "no",
           ctx->device_info.supports_sampler_anisotropy ? "yes" : "no",
           ctx->device_info.supports_dynamic_rendering ? "yes" : "no",
           ctx->device_info.supports_dynamic_rendering_local_read ? "yes" : "no",
           ctx->device_info.supports_synchronization2 ? "yes" : "no",
           ctx->device_info.supports_timeline_semaphore ? "yes" : "no",
           ctx->device_info.supports_maintenance5 ? "yes" : "no",
           ctx->device_info.supports_maintenance6 ? "yes" : "no",
           ctx->device_info.supports_push_descriptors ? "yes" : "no",
           ctx->device_info.supports_host_image_copy ? "yes" : "no",
           ctx->device_info.supports_shader_int16 ? "yes" : "no",
           ctx->device_info.supports_shader_int64 ? "yes" : "no",
           ctx->device_info.supports_storage_buffer_16bit ? "yes" : "no");

    if (ctx->device_info.has_drm_render_node || ctx->device_info.has_drm_primary_node) {
        printf("[Vulkan] DRM Device Nodes: Primary=%s (%lld:%lld), Render=%s (%lld:%lld)\n",
               ctx->device_info.has_drm_primary_node ? "yes" : "no",
               (long long)ctx->device_info.drm_primary_major,
               (long long)ctx->device_info.drm_primary_minor,
               ctx->device_info.has_drm_render_node ? "yes" : "no",
               (long long)ctx->device_info.drm_render_major,
               (long long)ctx->device_info.drm_render_minor);
    }

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