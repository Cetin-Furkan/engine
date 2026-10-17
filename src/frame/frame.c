#include "frame/frame.h"
#include "ring/ring.h"
#include "wayland/wayland.h"
#include "vulkan/core.h"
#include "vulkan/memory.h"

#include <drm/drm.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <unistd.h>

static constexpr uint64_t k_acquire_point     = 1;
static constexpr uint64_t k_release_point     = 1;
static constexpr uint64_t k_ud_signal_cancel  = UINT64_MAX;

typedef struct EngineFrameState {
    VulkanExportableImage image;
    VkImageView           image_view;
    VkCommandBuffer       cmd_buf;
    VkSemaphore           acquire_semaphore;
    // Optimal color target used only when the exported image cannot be rendered
    // to. The clear is copied into the linear DMA-BUF before the queue release.
    VkImage               copy_src;
    VkDeviceMemory        copy_src_memory;
    int                   drm_fd;
    uint32_t              release_handle;
    int                   release_eventfd;
    uint32_t              wl_buffer_id;
    bool                  release_pending;
    bool                  is_active;
} EngineFrameState;

static EngineFrameState s_frame = {
    .image            = { .dmabuf_fd = -1 },
    .drm_fd           = -1,
    .release_eventfd  = -1,
};

static int record_and_submit_clear(VulkanContext *vk, EngineFrameState *frame, VkSemaphore acquire) {
    const bool copy_out = frame->copy_src != VK_NULL_HANDLE;
    const VkImage render_image = copy_out ? frame->copy_src : frame->image.image;

    VkImageViewCreateInfo view_info = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext    = nullptr,
        .flags    = 0,
        .image    = render_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = frame->image.format,
        .components = {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };

    VkResult res = vkCreateImageView(vk->device, &view_info, nullptr, &frame->image_view);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Engine Error] vkCreateImageView failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }

    VkCommandBufferAllocateInfo alloc_info = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext              = nullptr,
        .commandPool        = vk->cmd_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };

    res = vkAllocateCommandBuffers(vk->device, &alloc_info, &frame->cmd_buf);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Engine Error] vkAllocateCommandBuffers failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }

    VkCommandBufferBeginInfo begin_info = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext            = nullptr,
        .flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };

    res = vkBeginCommandBuffer(frame->cmd_buf, &begin_info);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Engine Error] vkBeginCommandBuffer failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }

    // Barrier 1: UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL
    VkImageMemoryBarrier2 to_attachment = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext               = nullptr,
        .srcStageMask        = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask       = VK_ACCESS_2_NONE,
        .dstStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = render_image,
        .subresourceRange    = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };
    VkDependencyInfo dep_to_attachment = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext                   = nullptr,
        .dependencyFlags         = 0,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &to_attachment,
    };
    vkCmdPipelineBarrier2(frame->cmd_buf, &dep_to_attachment);

    // Dynamic Rendering Clear Pass
    VkRenderingAttachmentInfo color_attachment = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext       = nullptr,
        .imageView   = frame->image_view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue  = {
            .color = { .float32 = { 0.08f, 0.24f, 0.48f, 1.0f } },
        },
    };
    VkRenderingInfo rendering_info = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .pNext                = nullptr,
        .flags                = 0,
        .renderArea           = {
            .offset = { .x = 0, .y = 0 },
            .extent = { .width = frame->image.width, .height = frame->image.height },
        },
        .layerCount           = 1,
        .viewMask             = 0,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &color_attachment,
        .pDepthAttachment     = nullptr,
        .pStencilAttachment   = nullptr,
    };

    vkCmdBeginRendering(frame->cmd_buf, &rendering_info);
    vkCmdEndRendering(frame->cmd_buf);

    VkImageSubresourceRange color_range = {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };

    if (!copy_out) {
        // COLOR_ATTACHMENT_OPTIMAL -> GENERAL, and release to the compositor.
        VkImageMemoryBarrier2 to_general = {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .pNext               = nullptr,
            .srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
            .dstAccessMask       = VK_ACCESS_2_NONE,
            .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = vk->queues.graphics_family_index,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
            .image               = frame->image.image,
            .subresourceRange    = color_range,
        };
        VkDependencyInfo dep_to_general = {
            .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .pNext                   = nullptr,
            .dependencyFlags         = 0,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers    = &to_general,
        };
        vkCmdPipelineBarrier2(frame->cmd_buf, &dep_to_general);
    } else {
        VkImageMemoryBarrier2 pre_copy[2] = {
            {
                .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                .dstStageMask        = VK_PIPELINE_STAGE_2_COPY_BIT,
                .dstAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT,
                .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image               = frame->copy_src,
                .subresourceRange    = color_range,
            },
            {
                .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask        = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                .srcAccessMask       = VK_ACCESS_2_NONE,
                .dstStageMask        = VK_PIPELINE_STAGE_2_COPY_BIT,
                .dstAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image               = frame->image.image,
                .subresourceRange    = color_range,
            },
        };
        VkDependencyInfo dep_pre_copy = {
            .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 2,
            .pImageMemoryBarriers    = pre_copy,
        };
        vkCmdPipelineBarrier2(frame->cmd_buf, &dep_pre_copy);

        VkImageCopy region = {
            .srcSubresource = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel       = 0,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
            .srcOffset = {0, 0, 0},
            .dstSubresource = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel       = 0,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
            .dstOffset = {0, 0, 0},
            .extent = {
                .width  = frame->image.width,
                .height = frame->image.height,
                .depth  = 1,
            },
        };
        vkCmdCopyImage(frame->cmd_buf,
                       frame->copy_src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       frame->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &region);

        VkImageMemoryBarrier2 to_general = {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask        = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
            .dstAccessMask       = VK_ACCESS_2_NONE,
            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = vk->queues.graphics_family_index,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
            .image               = frame->image.image,
            .subresourceRange    = color_range,
        };
        VkDependencyInfo dep_to_general = {
            .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers    = &to_general,
        };
        vkCmdPipelineBarrier2(frame->cmd_buf, &dep_to_general);
    }

    res = vkEndCommandBuffer(frame->cmd_buf);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Engine Error] vkEndCommandBuffer failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }

    VkCommandBufferSubmitInfo cmd_submit = {
        .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .pNext         = nullptr,
        .commandBuffer = frame->cmd_buf,
        .deviceMask    = 0,
    };
    // The GPU writes the acquire point when this submission finishes.
    // The CPU does not wait here. The compositor's GPU waits on that point.
    VkSemaphoreSubmitInfo signal_info = {
        .sType       = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .pNext       = nullptr,
        .semaphore   = acquire,
        .value       = k_acquire_point,
        .stageMask   = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .deviceIndex = 0,
    };
    VkSubmitInfo2 submit_info = {
        .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .pNext                    = nullptr,
        .flags                    = 0,
        .waitSemaphoreInfoCount   = 0,
        .pWaitSemaphoreInfos      = nullptr,
        .commandBufferInfoCount   = 1,
        .pCommandBufferInfos      = &cmd_submit,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos    = &signal_info,
    };

    res = vkQueueSubmit2(vk->queues.graphics_queue, 1, &submit_info, VK_NULL_HANDLE);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Engine Error] vkQueueSubmit2 failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }

    return 0;
}

static int export_semaphore_fd(VulkanContext *vk, VkSemaphore semaphore) {
    VkSemaphoreGetFdInfoKHR info = {
        .sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .pNext      = nullptr,
        .semaphore  = semaphore,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    int fd = -1;
    VkResult res = vk->dispatch.vkGetSemaphoreFdKHR(vk->device, &info, &fd);
    if (res != VK_SUCCESS || fd < 0) {
        fprintf(stderr, "[Engine Error] vkGetSemaphoreFdKHR failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }
    return fd;
}

static int drm_syncobj_from_fd(int drm_fd, int syncobj_fd, uint32_t *out_handle) {
    struct drm_syncobj_handle imp = {
        .handle = 0,
        .flags  = 0,
        .fd     = syncobj_fd,
        .pad    = 0,
        .point  = 0,
    };
    if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &imp) != 0) {
        fprintf(stderr, "[Engine Error] DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE: %s\n", strerror(errno));
        return -1;
    }
    *out_handle = imp.handle;
    return 0;
}

static int drm_syncobj_to_fd(int drm_fd, uint32_t handle) {
    struct drm_syncobj_handle exp = {
        .handle = handle,
        .flags  = 0,
        .fd     = -1,
        .pad    = 0,
        .point  = 0,
    };
    if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &exp) != 0 || exp.fd < 0) {
        fprintf(stderr, "[Engine Error] DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD: %s\n", strerror(errno));
        return -1;
    }
    return exp.fd;
}

static void drm_syncobj_destroy(int drm_fd, uint32_t *handle) {
    if (drm_fd < 0 || handle == nullptr || *handle == 0) {
        return;
    }
    struct drm_syncobj_destroy destroy = {
        .handle = *handle,
        .pad    = 0,
    };
    if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy) != 0) {
        fprintf(stderr, "[Engine Error] DRM_IOCTL_SYNCOBJ_DESTROY: %s\n", strerror(errno));
    }
    *handle = 0;
}

static int create_acquire_semaphore(VulkanContext *vk, VkSemaphore *out) {
    VkSemaphoreTypeCreateInfo type_info = {
        .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext         = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue  = 0,
    };
    VkExportSemaphoreCreateInfo export_info = {
        .sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .pNext       = &type_info,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkSemaphoreCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &export_info,
        .flags = 0,
    };
    VkResult res = vkCreateSemaphore(vk->device, &create_info, nullptr, out);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Engine Error] vkCreateSemaphore failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }
    return 0;
}

static int wait_gpu_release(RingContext *ring, WaylandContext *wl) {
    uint64_t deadline = ring_mono_ns() + 3000000000ull;
    for (;;) {
        struct io_uring_cqe cqe;
        while (ring_peek(ring, &cqe)) {
            if (cqe.user_data == ENG_UD_GPU_RELEASE) {
                if (cqe.res < 0) {
                    fprintf(stderr, "[Engine Error] io_uring poll on GPU release: %s\n",
                            strerror(-cqe.res));
                    return -1;
                }
                return 0;
            }
            wayland_on_cqe(wl, &cqe);
            if (wl->failed) {
                fprintf(stderr, "[Engine Error] Wayland failed while waiting for GPU release: %s\n",
                        wl->err);
                return -1;
            }
        }
        if (ring_mono_ns() > deadline) {
            fprintf(stderr, "[Engine Error] GPU release timeline point was not signalled\n");
            return -1;
        }
        unsigned n = ring_submit(ring);
        int rc = ring_enter(ring, n, 1, 200);
        if (rc < 0 && rc != -ETIME && rc != -EINTR) {
            fprintf(stderr, "[Engine Error] io_uring_enter while waiting for GPU release: %s\n",
                    strerror(-rc));
            return -1;
        }
    }
}

static void destroy_copy_source(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        s_frame.copy_src = VK_NULL_HANDLE;
        s_frame.copy_src_memory = VK_NULL_HANDLE;
        return;
    }
    if (s_frame.copy_src != VK_NULL_HANDLE) {
        vkDestroyImage(vk->device, s_frame.copy_src, nullptr);
        s_frame.copy_src = VK_NULL_HANDLE;
    }
    if (s_frame.copy_src_memory != VK_NULL_HANDLE) {
        vkFreeMemory(vk->device, s_frame.copy_src_memory, nullptr);
        s_frame.copy_src_memory = VK_NULL_HANDLE;
    }
}

static int create_copy_source(VulkanContext *vk, uint32_t width, uint32_t height, VkFormat format) {
    destroy_copy_source(vk);

    VkImageCreateInfo image_info = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = format,
        .extent        = { .width = width, .height = height, .depth = 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkImage image = VK_NULL_HANDLE;
    VkResult img_res = vkCreateImage(vk->device, &image_info, nullptr, &image);
    if (img_res != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan Error] vkCreateImage for copy source failed: %s (%d)\n",
                vulkan_vkresult_to_str(img_res), (int)img_res);
        return -1;
    }

    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(vk->device, image, &reqs);
    uint32_t memory_type = 0;
    if (vulkan_find_memory_type(vk, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                &memory_type) != VULKAN_SUCCESS) {
        vkDestroyImage(vk->device, image, nullptr);
        return -1;
    }

    VkMemoryAllocateInfo alloc_info = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = reqs.size,
        .memoryTypeIndex = memory_type,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult alloc_res = vkAllocateMemory(vk->device, &alloc_info, nullptr, &memory);
    if (alloc_res != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan Error] vkAllocateMemory for copy source failed: %s (%d)\n",
                vulkan_vkresult_to_str(alloc_res), (int)alloc_res);
        vkDestroyImage(vk->device, image, nullptr);
        return -1;
    }
    if (vkBindImageMemory(vk->device, image, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(vk->device, memory, nullptr);
        vkDestroyImage(vk->device, image, nullptr);
        return -1;
    }

    s_frame.copy_src = image;
    s_frame.copy_src_memory = memory;
    return 0;
}

static bool present_image_layout_ok(const VulkanExportableImage *image) {
    const uint64_t min_stride = (uint64_t)image->width * 4ull;
    return image->has_drm_format_modifier &&
           image->row_pitch >= min_stride &&
           image->row_pitch <= 0xffffffffu &&
           image->offset <= 0xffffffffu;
}

static bool keep_present_image(VulkanResult result) {
    if (result == VULKAN_SUCCESS && present_image_layout_ok(&s_frame.image)) {
        return true;
    }
    if (result == VULKAN_SUCCESS) {
        vulkan_destroy_exportable_image(vulkan_get_context(), &s_frame.image);
    }
    return false;
}

// B8G8R8A8_SRGB and B8G8R8A8_UNORM are the same DRM_FORMAT_ARGB8888 byte order.
// SRGB is tried first. UNORM is the same pixels when a driver cannot export SRGB.
static const VkFormat k_argb_formats[] = {
    VK_FORMAT_B8G8R8A8_SRGB,
    VK_FORMAT_B8G8R8A8_UNORM,
};

static int allocate_present_image(VulkanContext *vk, WaylandContext *wl,
                                  uint32_t width, uint32_t height,
                                  const char **tranche_kind) {
    VulkanResult img_res = VULKAN_ERROR_FORMAT_NOT_SUPPORTED;
    *tranche_kind = wl->feedback_prime ? "prime" : "general";

    if (!wl->feedback_prime && wl->scanout_modifier_count > 0) {
        img_res = vulkan_create_exportable_image_with_modifiers(
            vk, width, height, VK_FORMAT_B8G8R8A8_SRGB,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            wl->scanout_modifiers, wl->scanout_modifier_count,
            false, &s_frame.image);
        if (keep_present_image(img_res)) {
            *tranche_kind = "scanout";
            return 0;
        }
    }

    img_res = vulkan_create_exportable_image_with_modifiers(
        vk, width, height, VK_FORMAT_B8G8R8A8_SRGB,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        wl->all_modifiers, wl->all_modifier_count,
        wl->feedback_prime, &s_frame.image);
    if (keep_present_image(img_res)) {
        return 0;
    }

    if (!wl->feedback_prime) {
        fprintf(stderr, "[Engine Error] Failed to allocate modifier-backed DMA-BUF image: %s\n",
                vulkan_result_to_str(img_res));
        return -1;
    }

    // The compositor's modifiers belong to its own GPU. This GPU stays selected.
    // A linear buffer is the layout the other device can import.
    printf("[Engine] Selected GPU cannot export the compositor modifiers. "
           "Allocating DRM_FORMAT_MOD_LINEAR on that GPU.\n");

    const uint64_t linear_modifier = 0;
    for (size_t i = 0; i < sizeof k_argb_formats / sizeof k_argb_formats[0]; ++i) {
        const VkFormat format = k_argb_formats[i];

        img_res = vulkan_create_exportable_image_with_modifiers(
            vk, width, height, format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            &linear_modifier, 1, true, &s_frame.image);
        if (keep_present_image(img_res)) {
            *tranche_kind = "prime-linear";
            return 0;
        }

        img_res = vulkan_create_exportable_linear_dmabuf(
            vk, width, height, format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &s_frame.image);
        if (keep_present_image(img_res)) {
            *tranche_kind = "prime-linear";
            return 0;
        }

        img_res = vulkan_create_exportable_linear_dmabuf(
            vk, width, height, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT, &s_frame.image);
        if (keep_present_image(img_res)) {
            if (create_copy_source(vk, width, height, format) == 0) {
                *tranche_kind = "prime-copy";
                return 0;
            }
            vulkan_destroy_exportable_image(vk, &s_frame.image);
        }
    }

    fprintf(stderr,
            "[Engine Error] Selected GPU cannot export a DMA-BUF the compositor can import: %s\n",
            vulkan_result_to_str(img_res));
    return -1;
}

int engine_subsystem_init(void) {
    RingContext    *ring = ring_get_context();
    WaylandContext *wl   = wayland_get_context();
    VulkanContext  *vk   = vulkan_get_context();

    if (ring == nullptr || !ring->is_initialized ||
        wl == nullptr || !wl->is_initialized ||
        vk == nullptr || !vk->is_initialized) {
        fprintf(stderr, "[Engine Error] Required subsystems are not active\n");
        return 1;
    }

    if (!vk->device_info.has_drm_render_node && !vk->device_info.has_drm_primary_node) {
        fprintf(stderr, "[Engine Error] Selected GPU does not expose a Linux DRM node\n");
        return 1;
    }
    if (!vk->device_info.supports_external_syncobj || vk->dispatch.vkGetSemaphoreFdKHR == nullptr ||
        wl->drm_syncobj_id == 0 || !vk->device_info.has_drm_render_node) {
        fprintf(stderr,
                "[Engine Error] Explicit sync needs a DRM render node, VK_KHR_external_semaphore_fd, and wp_linux_drm_syncobj_manager_v1\n");
        return 1;
    }
    VkPhysicalDeviceExternalSemaphoreInfo ext_info = {
        .sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .pNext      = nullptr,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkExternalSemaphoreProperties ext_props = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
        .pNext = nullptr,
    };
    vkGetPhysicalDeviceExternalSemaphoreProperties(vk->physical_device, &ext_info, &ext_props);
    if ((ext_props.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) == 0) {
        fprintf(stderr, "[Engine Error] Timeline semaphore opaque-fd export is not available\n");
        return 1;
    }

    s_frame = (EngineFrameState){
        .image           = { .dmabuf_fd = -1 },
        .drm_fd          = -1,
        .release_eventfd = -1,
    };
    s_frame.is_active = true;

    // 1. Surface and configure
    uint32_t width = 0, height = 0, serial = 0;
    if (wayland_create_toplevel_and_configure(wl, "Antigravity", "antigravity",
                                              &width, &height, &serial) < 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }

    // 2. DMA-BUF feedback for that surface matched against VulkanDeviceInfo
    const bool has_render  = vk->device_info.has_drm_render_node;
    const bool has_primary = vk->device_info.has_drm_primary_node;
    const int64_t target_major = has_render
        ? vk->device_info.drm_render_major
        : vk->device_info.drm_primary_major;
    const int64_t target_minor = has_render
        ? vk->device_info.drm_render_minor
        : vk->device_info.drm_primary_minor;
    const bool has_alt = has_render && has_primary;

    if (wayland_query_surface_dmabuf_feedback(
            wl,
            target_major,
            target_minor,
            has_alt ? vk->device_info.drm_primary_major : 0,
            has_alt ? vk->device_info.drm_primary_minor : 0,
            has_alt) < 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }

    // 3. Image on the already selected GPU. Same-GPU uses the compositor's
    //    tiled modifiers. A different DRM node gets a linear buffer that node
    //    can import. The GPU is not switched.
    const char *tranche_kind = "general";
    if (allocate_present_image(vk, wl, width, height, &tranche_kind) < 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }

    if (create_acquire_semaphore(vk, &s_frame.acquire_semaphore) < 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }
    if (record_and_submit_clear(vk, &s_frame, s_frame.acquire_semaphore) < 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }

    char render_path[64];
    int path_n = snprintf(render_path, sizeof render_path, "/dev/dri/renderD%lld",
                          (long long)vk->device_info.drm_render_minor);
    if (path_n < 0 || (size_t)path_n >= sizeof render_path) {
        fprintf(stderr, "[Engine Error] DRM render node path is too long\n");
        (void)engine_subsystem_shutdown();
        return 1;
    }
    s_frame.drm_fd = open(render_path, O_RDWR | O_CLOEXEC);
    if (s_frame.drm_fd < 0) {
        fprintf(stderr, "[Engine Error] open %s: %s\n", render_path, strerror(errno));
        (void)engine_subsystem_shutdown();
        return 1;
    }

    int acquire_vk_fd = export_semaphore_fd(vk, s_frame.acquire_semaphore);
    if (acquire_vk_fd < 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }
    uint32_t acquire_handle = 0;
    int imported = drm_syncobj_from_fd(s_frame.drm_fd, acquire_vk_fd, &acquire_handle);
    close(acquire_vk_fd);
    if (imported < 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }
    int acquire_fd = drm_syncobj_to_fd(s_frame.drm_fd, acquire_handle);
    drm_syncobj_destroy(s_frame.drm_fd, &acquire_handle);
    if (acquire_fd < 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }

    struct drm_syncobj_create release_create = {0};
    if (ioctl(s_frame.drm_fd, DRM_IOCTL_SYNCOBJ_CREATE, &release_create) != 0) {
        fprintf(stderr, "[Engine Error] DRM_IOCTL_SYNCOBJ_CREATE: %s\n", strerror(errno));
        close(acquire_fd);
        (void)engine_subsystem_shutdown();
        return 1;
    }
    s_frame.release_handle = release_create.handle;
    int release_fd = drm_syncobj_to_fd(s_frame.drm_fd, s_frame.release_handle);
    if (release_fd < 0) {
        close(acquire_fd);
        (void)engine_subsystem_shutdown();
        return 1;
    }

    int acquire_import = wayland_import_syncobj_timeline(wl, acquire_fd, &wl->acquire_timeline_id);
    close(acquire_fd);
    if (acquire_import < 0) {
        close(release_fd);
        (void)engine_subsystem_shutdown();
        return 1;
    }
    int release_import = wayland_import_syncobj_timeline(wl, release_fd, &wl->release_timeline_id);
    close(release_fd);
    if (release_import < 0 || wayland_bind_explicit_sync(wl) < 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }

    s_frame.release_eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (s_frame.release_eventfd < 0) {
        fprintf(stderr, "[Engine Error] eventfd: %s\n", strerror(errno));
        (void)engine_subsystem_shutdown();
        return 1;
    }
    struct drm_syncobj_eventfd release_event = {
        .handle = s_frame.release_handle,
        .flags  = 0,
        .point  = k_release_point,
        .fd     = s_frame.release_eventfd,
        .pad    = 0,
    };
    if (ioctl(s_frame.drm_fd, DRM_IOCTL_SYNCOBJ_EVENTFD, &release_event) != 0) {
        fprintf(stderr, "[Engine Error] DRM_IOCTL_SYNCOBJ_EVENTFD: %s\n", strerror(errno));
        (void)engine_subsystem_shutdown();
        return 1;
    }

    // 4. Import into Wayland and present one visible frame
    if (wayland_import_dmabuf_immed(
            wl,
            s_frame.image.dmabuf_fd,
            s_frame.image.width,
            s_frame.image.height,
            (uint32_t)s_frame.image.row_pitch,
            (uint32_t)s_frame.image.offset,
            ENG_DRM_FORMAT_ARGB8888,
            s_frame.image.drm_format_modifier,
            &s_frame.wl_buffer_id) < 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }

    if (wayland_present_single_frame_and_wait(
            wl,
            s_frame.wl_buffer_id,
            s_frame.image.width,
            s_frame.image.height,
            serial,
            wl->acquire_timeline_id,
            k_acquire_point,
            wl->release_timeline_id,
            k_release_point) < 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }

    s_frame.release_pending = true;
    printf("[Engine] Presented 1 visible frame: size=%ux%u (serial=%u), fourcc=0x%08x (ARGB8888), modifier=0x%llx (%s tranche), stride=%zu, offset=%zu, fd=%d, explicit-sync acquire=%llu release=%llu\n",
           s_frame.image.width,
           s_frame.image.height,
           serial,
           ENG_DRM_FORMAT_ARGB8888,
           (unsigned long long)s_frame.image.drm_format_modifier,
           tranche_kind,
           (size_t)s_frame.image.row_pitch,
           (size_t)s_frame.image.offset,
           s_frame.image.dmabuf_fd,
           (unsigned long long)k_acquire_point,
           (unsigned long long)k_release_point);
    printf("hello init from engine\n");
    return 0;
}

int engine_subsystem_run(void) {
    if (!s_frame.is_active || !s_frame.release_pending) {
        fprintf(stderr, "[Engine Error] Idle run requires a presented frame\n");
        return 1;
    }

    RingContext    *ring = ring_get_context();
    WaylandContext *wl   = wayland_get_context();
    if (ring == nullptr || !ring->is_initialized || wl == nullptr || !wl->is_initialized) {
        fprintf(stderr, "[Engine Error] Idle run requires the ring and Wayland\n");
        return 1;
    }

    sigset_t mask;
    sigset_t old_mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &mask, &old_mask) != 0) {
        fprintf(stderr, "[Engine Error] sigprocmask: %s\n", strerror(errno));
        return 1;
    }

    int signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signal_fd < 0) {
        fprintf(stderr, "[Engine Error] signalfd: %s\n", strerror(errno));
        (void)sigprocmask(SIG_SETMASK, &old_mask, nullptr);
        return 1;
    }
    if (ring_arm_poll(ring, signal_fd, POLLIN, ENG_UD_SIGNAL) != 0) {
        fprintf(stderr, "[Engine Error] Failed to arm io_uring poll on signalfd\n");
        close(signal_fd);
        (void)sigprocmask(SIG_SETMASK, &old_mask, nullptr);
        return 1;
    }

    printf("[Engine] Idle. Buffer %ux%u stays on screen. CPU is blocked in io_uring.\n",
           s_frame.image.width, s_frame.image.height);
    fflush(stdout);

    bool     signal_poll_armed = true;
    bool     cancel_in_flight  = false;
    bool     send_in_flight    = false;
    uint32_t pending_serial    = 0;
    bool     got_signal        = false;
    int      signo             = 0;
    int      run_rc            = 0;
    const char *why            = "idle wait ended";

    for (;;) {
        struct io_uring_cqe cqe;
        while (ring_peek(ring, &cqe)) {
            if (cqe.user_data == ENG_UD_SIGNAL) {
                signal_poll_armed = false;
                if (cqe.res >= 0) {
                    struct signalfd_siginfo info;
                    ssize_t nread = read(signal_fd, &info, sizeof info);
                    if (nread == (ssize_t)sizeof info) {
                        signo = (int)info.ssi_signo;
                    }
                    got_signal = true;
                }
            } else if (cqe.user_data == ENG_UD_WL_SEND) {
                send_in_flight = false;
                if (wayland_note_send_cqe(wl, &cqe, pending_serial) < 0) {
                    run_rc = 1;
                }
            } else {
                wayland_on_cqe(wl, &cqe);
            }
        }

        if (got_signal) {
            why = "signal";
            break;
        }
        if (wl->toplevel_closed) {
            why = "toplevel close";
            break;
        }
        if (wl->failed) {
            why    = wl->err;
            run_rc = 1;
            break;
        }

        if (!send_in_flight && wl->configure_serial != wl->acked_configure_serial) {
            // Keep geometry matched to the single allocated buffer while idle.
            const uint32_t width  = s_frame.image.width;
            const uint32_t height = s_frame.image.height;
            pending_serial = wl->configure_serial;
            int queued = wayland_queue_configure_ack(wl, pending_serial, width, height);
            if (queued == 0) {
                send_in_flight = true;
            } else if (queued != -EBUSY) {
                run_rc = 1;
                why    = wl->err[0] != '\0' ? wl->err : "configure ack failed";
                break;
            }
        }

        if (!wl->recv_armed && wayland_ensure_recv(wl) < 0) {
            run_rc = 1;
            why    = wl->err;
            break;
        }

        unsigned submitted = ring_submit(ring);
        int rc = ring_enter(ring, submitted, 1, -1);
        if (rc < 0) {
            fprintf(stderr, "[Engine Error] io_uring_enter while idle: %s\n", strerror(-rc));
            run_rc = 1;
            why    = "io_uring_enter failed";
            break;
        }
    }

    // If we exited via toplevel close rather than a signal, remove the armed
    // poll on signal_fd before closing it.
    if (signal_poll_armed) {
        struct io_uring_sqe *cancel_sqe = ring_sqe(ring);
        if (cancel_sqe != nullptr) {
            cancel_sqe->opcode    = IORING_OP_POLL_REMOVE;
            cancel_sqe->addr      = ENG_UD_SIGNAL;
            cancel_sqe->user_data = k_ud_signal_cancel;
            cancel_in_flight      = true;
        }
    }

    // Drain any in-flight configure ack send, xdg_wm_base pong send, or signal poll cancel
    // before closing signal_fd or reusing the Wayland send buffer during shutdown.
    uint64_t drain_deadline = ring_mono_ns() + 3000000000ull;
    while ((send_in_flight || wl->pong_in_flight || signal_poll_armed || cancel_in_flight) &&
           !wl->failed) {
        if (ring_mono_ns() > drain_deadline) {
            fprintf(stderr, "[Engine Error] Idle teardown drain timed out\n");
            run_rc = 1;
            break;
        }
        struct io_uring_cqe cqe;
        if (!ring_peek(ring, &cqe)) {
            unsigned submitted = ring_submit(ring);
            int rc = ring_enter(ring, submitted, 1, 200);
            if (rc < 0 && rc != -ETIME && rc != -EINTR) {
                run_rc = 1;
                break;
            }
            continue;
        }
        if (cqe.user_data == ENG_UD_WL_SEND) {
            send_in_flight = false;
            if (wayland_note_send_cqe(wl, &cqe, pending_serial) < 0) {
                run_rc = 1;
            }
        } else if (cqe.user_data == ENG_UD_SIGNAL) {
            signal_poll_armed = false;
        } else if (cqe.user_data == k_ud_signal_cancel) {
            cancel_in_flight = false;
        } else {
            wayland_on_cqe(wl, &cqe);
        }
    }

    close(signal_fd);
    (void)sigprocmask(SIG_SETMASK, &old_mask, nullptr);

    if (got_signal && signo != 0) {
        printf("[Engine] Idle ended (signal %d)\n", signo);
    } else {
        printf("[Engine] Idle ended (%s)\n", why);
    }
    fflush(stdout);
    return run_rc;
}

int engine_subsystem_shutdown(void) {
    if (!s_frame.is_active) {
        return 0;
    }

    WaylandContext *wl   = wayland_get_context();
    VulkanContext  *vk   = vulkan_get_context();
    RingContext    *ring = ring_get_context();
    int             release_rc = 0;

    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        (void)vkDeviceWaitIdle(vk->device);

        if (s_frame.cmd_buf != VK_NULL_HANDLE && vk->cmd_pool != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &s_frame.cmd_buf);
            s_frame.cmd_buf = VK_NULL_HANDLE;
        }
        if (s_frame.image_view != VK_NULL_HANDLE) {
            vkDestroyImageView(vk->device, s_frame.image_view, nullptr);
            s_frame.image_view = VK_NULL_HANDLE;
        }
        destroy_copy_source(vk);
    }

    // The compositor signals the release point when it drops the buffer.
    // A null attach asks for that drop while the release timeline still exists.
    // The eventfd was armed before present, so a signal during this roundtrip
    // stays readable for the poll below.
    if (s_frame.release_pending) {
        if (wl == nullptr || !wl->is_initialized || wayland_detach_buffer(wl) < 0) {
            fprintf(stderr, "[Engine Error] Buffer detach failed before GPU release wait\n");
        }
        if (s_frame.release_eventfd >= 0 && ring != nullptr && ring->is_initialized &&
            wl != nullptr && wl->is_initialized &&
            ring_arm_poll(ring, s_frame.release_eventfd, POLLIN, ENG_UD_GPU_RELEASE) == 0 &&
            wait_gpu_release(ring, wl) == 0) {
            printf("[Engine] GPU release point signalled (io_uring poll)\n");
        } else {
            if (s_frame.release_eventfd < 0) {
                fprintf(stderr, "[Engine Error] GPU release eventfd is closed\n");
            }
            release_rc = 1;
        }
    }

    if (s_frame.release_eventfd >= 0) {
        close(s_frame.release_eventfd);
        s_frame.release_eventfd = -1;
    }

    if (wl != nullptr && wl->is_initialized) {
        wayland_destroy_frame_objects(wl);
        s_frame.wl_buffer_id = 0;
    }

    drm_syncobj_destroy(s_frame.drm_fd, &s_frame.release_handle);
    if (s_frame.drm_fd >= 0) {
        close(s_frame.drm_fd);
        s_frame.drm_fd = -1;
    }

    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        if (s_frame.acquire_semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(vk->device, s_frame.acquire_semaphore, nullptr);
            s_frame.acquire_semaphore = VK_NULL_HANDLE;
        }
        vulkan_destroy_exportable_image(vk, &s_frame.image);
    }

    s_frame.is_active = false;
    s_frame.release_pending = false;
    printf("goodbye from engine\n");
    return release_rc;
}