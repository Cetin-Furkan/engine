#include "frame/frame.h"
#include "frame/cube.h"
#include "frame/hud.h"
#include "frame/shape.h"
#include "frame/solid.h"
#include "frame/world.h"
#include "chrome/gpu.h"
#include "chrome/layer.h"
#include "chrome/spec.h"
#include "input/pointer.h"
#include "ring/ring.h"
#include "wayland/wayland.h"
#include "vulkan/core.h"
#include "vulkan/memory.h"
#include <drm/drm.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <unistd.h>

enum : uint64_t {
    k_ud_signal_cancel  = UINT64_MAX,
    k_ud_release_cancel = UINT64_MAX - 1u,
};

enum : uint32_t {
    k_orphan_cap = 8u,
};

enum : int {
    k_turn_arm  = 0,
    k_turn_run  = 1,
    k_turn_done = 2,
};

// One presented image. The DRM node and the acquire timeline are not in here.
typedef struct FrameSlot {
    VulkanExportableImage image;
    VkImageView           image_view;
    VkFormat              view_format;
    CubeDepth             depth;
    CubeCoverage          coverage;
    VkCommandBuffer       cmd_buf;
    VkSemaphore           release_semaphore;
    uint32_t              release_handle;
    uint32_t              release_timeline_id;
    int                   release_eventfd;
    uint64_t              release_point;
    uint64_t              acquire_point;
    uint32_t              wl_buffer_id;
    bool                  release_pending;
} FrameSlot;

// Process-owned present state. Both slots signal this one acquire timeline.
static int         s_drm_fd = -1;
static VkSemaphore s_acquire = VK_NULL_HANDLE;
static bool        s_presented = false;

// s_slot[s_front] is on screen. The other slot is the spare while the turn runs.
static FrameSlot s_slot[2] = {
    [0] = { .image = { .dmabuf_fd = -1 }, .release_eventfd = -1 },
    [1] = { .image = { .dmabuf_fd = -1 }, .release_eventfd = -1 },
};
static int  s_front = 0;
static bool s_spare_live = false;

static FrameSlot *shown(void) {
    return &s_slot[s_front];
}

static FrameSlot *spare(void) {
    return &s_slot[s_front ^ 1];
}

static int      s_turn_phase = k_turn_arm;
static bool     s_turn_swap_pending = false;
static uint64_t s_pending_release = 0;

// A resize commit is queued like a turn commit. The old front is retired when
// ENG_UD_WL_SEND accepts the bytes, not when a wl_callback.done arrives.
static bool      s_resize_pending = false;
static FrameSlot s_resize_fresh = { .image = { .dmabuf_fd = -1 }, .release_eventfd = -1 };
static uint64_t  s_resize_release = 0;
static uint32_t  s_resize_serial = 0;
static uint32_t  s_resize_old_buffer = 0;
static uint32_t  s_resize_old_w = 0;
static uint32_t  s_resize_old_h = 0;
static char      s_resize_tranche[16];

// Last acquire point signaled on the shared acquire timeline.
// Each slot keeps one release syncobj. The point starts at 1. A resize
// reallocates the images and bumps that same point. It does not import again.
static uint64_t s_sync_point = 0;

// Deferred retirement ring for buffers whose compositor release point has not
// yet signaled at the moment of resize replacement.
static FrameSlot s_orphans[k_orphan_cap];
static uint32_t         s_orphan_count = 0;
static int              s_stuck_release_fd = -1;

static void wait_acquire_point(VulkanContext *vk, uint64_t point) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE ||
        s_acquire == VK_NULL_HANDLE || point == 0) {
        return;
    }
    VkSemaphoreWaitInfo wait_info = {
        .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .pNext          = nullptr,
        .flags          = 0,
        .semaphoreCount = 1,
        .pSemaphores    = &s_acquire,
        .pValues        = &point,
    };
    (void)vkWaitSemaphores(vk->device, &wait_info, 3000000000ull);
}

static bool acquire_point_retired(VulkanContext *vk, uint64_t point) {
    if (point == 0 || s_acquire == VK_NULL_HANDLE ||
        vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return true;
    }
    // Timeout 0 does not block. It retires the submit that signaled this
    // point so that image's command buffer can be recorded again.
    VkSemaphoreWaitInfo wait_info = {
        .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores    = &s_acquire,
        .pValues        = &point,
    };
    return vkWaitSemaphores(vk->device, &wait_info, 0) == VK_SUCCESS;
}

static bool poll_release_semaphore(VulkanContext *vk, VkSemaphore semaphore, uint64_t point) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE ||
        semaphore == VK_NULL_HANDLE || point == 0) {
        return false;
    }
    uint64_t counter = 0;
    if (vkGetSemaphoreCounterValue(vk->device, semaphore, &counter) == VK_SUCCESS) {
        return counter >= point;
    }
    return false;
}

static int ensure_frame_targets(VulkanContext *vk, FrameSlot *frame) {
    if (frame->image_view != VK_NULL_HANDLE) {
        return 0;
    }
    VkFormat view_format = (frame->image.format == VK_FORMAT_B8G8R8A8_UNORM)
        ? VK_FORMAT_B8G8R8A8_SRGB
        : frame->image.format;
    VkImageViewCreateInfo view_info = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext    = nullptr,
        .flags    = 0,
        .image    = frame->image.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = view_format,
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
    if (res != VK_SUCCESS && view_format != frame->image.format) {
        fprintf(stderr,
                "[Engine Error] DMA-BUF format %d rejected an sRGB view: %s (%d). The cube and quads are drawn only into that view.\n",
                (int)frame->image.format, vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Engine Error] vkCreateImageView failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }
    frame->view_format = view_info.format;
    if (view_info.format == VK_FORMAT_B8G8R8A8_SRGB &&
        (cube_depth_create(vk, &frame->depth, frame->image.width, frame->image.height) != 0 ||
         cube_coverage_create(vk, &frame->coverage, frame->image.width, frame->image.height) != 0)) {
        return -1;
    }
    return 0;
}

// heat_dt is the callback interval. Zero draws the field as it stands.
static int record_and_submit_clear(VulkanContext *vk, FrameSlot *frame, VkSemaphore acquire,
                                   uint64_t signal_point, bool draw_live, uint32_t heat_dt) {
    frame->acquire_point = signal_point;
    if (ensure_frame_targets(vk, frame) != 0) {
        return -1;
    }
    const bool srgb_view = frame->view_format == VK_FORMAT_B8G8R8A8_SRGB;
    VkResult res = VK_SUCCESS;

    if (frame->cmd_buf == VK_NULL_HANDLE) {
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
    } else {
        res = vkResetCommandBuffer(frame->cmd_buf, 0);
        if (res != VK_SUCCESS) {
            fprintf(stderr, "[Engine Error] vkResetCommandBuffer failed: %s (%d)\n",
                    vulkan_vkresult_to_str(res), (int)res);
            return -1;
        }
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
    if (heat_dt > 0u || cube_heat_poke_pending()) {
        cube_heat_record(frame->cmd_buf, heat_dt, world_cube_yaw_deg(draw_live));
    }

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
        .image               = frame->image.image,
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
    if (frame->coverage.image != VK_NULL_HANDLE) {
        VkImageMemoryBarrier2 to_coverage = to_attachment;
        to_coverage.image = frame->coverage.image;
        VkDependencyInfo dep_coverage = {
            .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers    = &to_coverage,
        };
        vkCmdPipelineBarrier2(frame->cmd_buf, &dep_coverage);
    }
    if (srgb_view && frame->depth.image != VK_NULL_HANDLE) {
        cube_prepare_draw(frame->cmd_buf, frame->depth.image);
    }

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
    VkRenderingAttachmentInfo depth_attachment = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = frame->depth.view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp     = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue  = { .depthStencil = { .depth = 1.0f, .stencil = 0 } },
    };
    if (srgb_view && frame->depth.view != VK_NULL_HANDLE) {
        rendering_info.pDepthAttachment = &depth_attachment;
    }
    if (frame->coverage.view != VK_NULL_HANDLE) {
        color_attachment.imageView = frame->coverage.view;
        color_attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color_attachment.resolveImageView = frame->image_view;
        color_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }

    vkCmdBeginRendering(frame->cmd_buf, &rendering_info);
    if (rendering_info.pDepthAttachment != nullptr) {
        world_draw(frame->cmd_buf, frame->image.width, frame->image.height, draw_live);
    }
    vkCmdEndRendering(frame->cmd_buf);

    VkImageSubresourceRange color_range = {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };

    VkImageMemoryBarrier2 chrome_vis = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = frame->image.image,
        .subresourceRange    = color_range,
    };
    VkDependencyInfo chrome_dep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &chrome_vis,
    };
    vkCmdPipelineBarrier2(frame->cmd_buf, &chrome_dep);

    VkRenderingAttachmentInfo chrome_color = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = frame->image_view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
    };
    VkRenderingInfo chrome_rendering = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = rendering_info.renderArea,
        .layerCount           = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &chrome_color,
    };
    vkCmdBeginRendering(frame->cmd_buf, &chrome_rendering);
    chrome_gpu_draw(vk, frame->cmd_buf, frame->image.width, frame->image.height, frame->view_format);
    {
        WaylandContext *wl_hud = wayland_get_context();
        bool have_stamp = wl_hud != nullptr && wl_hud->frame_stamp_valid;
        hud_prepare(have_stamp ? wl_hud->frame_time_ms : 0u, have_stamp,
                    frame->image.drm_format_modifier);
        hud_draw(frame->cmd_buf, frame->image.width, frame->image.height, frame->view_format);
    }
    vkCmdEndRendering(frame->cmd_buf);

    uint32_t dst_queue = vk->device_info.supports_queue_family_foreign
        ? VK_QUEUE_FAMILY_FOREIGN_EXT
        : VK_QUEUE_FAMILY_EXTERNAL;

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
        .dstQueueFamilyIndex = dst_queue,
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

    res = vkEndCommandBuffer(frame->cmd_buf);
    if (res != VK_SUCCESS) {
        cube_heat_record_abort();
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
    // The field the shaders read was written by an earlier submit. That
    // submit signaled this acquire value at ALL_COMMANDS. Waiting it here
    // is what makes the write visible. The signal stage is the whole
    // command buffer, so a step waits at compute and a later draw waits
    // at vertex. The bake's seed stays at 0 because that queue was idled.
    uint64_t heat_point = cube_heat_published();
    bool wait_heat = heat_point != 0 && heat_point < signal_point;
    VkSemaphoreSubmitInfo wait_info = {
        .sType       = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .pNext       = nullptr,
        .semaphore   = acquire,
        .value       = heat_point,
        .stageMask   = cube_heat_step_open() ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                             : VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
        .deviceIndex = 0,
    };
    VkSemaphoreSubmitInfo signal_info = {
        .sType       = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .pNext       = nullptr,
        .semaphore   = acquire,
        .value       = signal_point,
        .stageMask   = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .deviceIndex = 0,
    };
    VkSubmitInfo2 submit_info = {
        .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .pNext                    = nullptr,
        .flags                    = 0,
        .waitSemaphoreInfoCount   = wait_heat ? 1u : 0u,
        .pWaitSemaphoreInfos      = wait_heat ? &wait_info : nullptr,
        .commandBufferInfoCount   = 1,
        .pCommandBufferInfos      = &cmd_submit,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos    = &signal_info,
    };

    res = vkQueueSubmit2(vk->queues.graphics_queue, 1, &submit_info, VK_NULL_HANDLE);
    if (res != VK_SUCCESS) {
        cube_heat_record_abort();
        fprintf(stderr, "[Engine Error] vkQueueSubmit2 failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }
    cube_heat_record_ok(signal_point);
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

static int import_release_semaphore_fd(VulkanContext *vk, int syncobj_fd, VkSemaphore *out) {
    VkSemaphoreTypeCreateInfo type_info = {
        .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext         = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue  = 0,
    };
    VkSemaphoreCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &type_info,
        .flags = 0,
    };
    VkSemaphore sem = VK_NULL_HANDLE;
    VkResult res = vkCreateSemaphore(vk->device, &create_info, nullptr, &sem);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Engine Error] vkCreateSemaphore (release) failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }
    int dup_fd = dup(syncobj_fd);
    if (dup_fd < 0) {
        fprintf(stderr, "[Engine Error] dup release syncobj fd: %s\n", strerror(errno));
        vkDestroySemaphore(vk->device, sem, nullptr);
        return -1;
    }

    VkImportSemaphoreFdInfoKHR import_info = {
        .sType      = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .pNext      = nullptr,
        .semaphore  = sem,
        .flags      = 0,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
        .fd         = dup_fd,
    };
    res = vk->dispatch.vkImportSemaphoreFdKHR(vk->device, &import_info);
    if (res != VK_SUCCESS) {
        close(dup_fd);
        vkDestroySemaphore(vk->device, sem, nullptr);
        fprintf(stderr, "[Engine Error] vkImportSemaphoreFdKHR failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }

    *out = sem;
    return 0;
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

static int arm_release_poll_fixed(RingContext *ring, int release_fd) {
    if (ring == nullptr || !ring->is_initialized || release_fd < 0) {
        return -EINVAL;
    }
    if (ring_update_file(ring, ENG_RELEASE_FD_SLOT, release_fd) == 0) {
        return ring_arm_poll_fixed(ring, ENG_RELEASE_FD_SLOT, POLLIN, ENG_UD_GPU_RELEASE);
    }
    return ring_arm_poll(ring, release_fd, POLLIN, ENG_UD_GPU_RELEASE);
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

static void destroy_frame_gpu(VulkanContext *vk, FrameSlot *frame) {
    if (frame == nullptr) {
        return;
    }
    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        wait_acquire_point(vk, frame->acquire_point);
        if (frame->cmd_buf != VK_NULL_HANDLE && vk->cmd_pool != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &frame->cmd_buf);
        }
        if (frame->image_view != VK_NULL_HANDLE) {
            vkDestroyImageView(vk->device, frame->image_view, nullptr);
        }
        cube_depth_destroy(vk, &frame->depth);
        cube_coverage_destroy(vk, &frame->coverage);
        vulkan_destroy_exportable_image(vk, &frame->image);
    } else if (frame->image.dmabuf_fd >= 0) {
        close(frame->image.dmabuf_fd);
        frame->image.dmabuf_fd = -1;
    }
    frame->cmd_buf = VK_NULL_HANDLE;
    frame->image_view = VK_NULL_HANDLE;
    frame->view_format = VK_FORMAT_UNDEFINED;
    frame->acquire_point = 0;
}

static void destroy_wl_buffer_now(WaylandContext *wl, uint32_t buffer_id) {
    if (wl == nullptr || !wl->is_initialized || wl->failed || buffer_id == 0) {
        return;
    }
    if (wl->send_len > 0) {
        (void)wayland_send_batch(wl);
    }
    if (!wl->failed && wayland_put_raw(wl, buffer_id, 0u, nullptr, 0u)) {
        (void)wayland_send_batch(wl);
    }
    if (wl->buffer_id == buffer_id) {
        wl->buffer_id = 0;
    }
}

static int create_release_timeline(VulkanContext *vk, WaylandContext *wl, FrameSlot *frame) {
    struct drm_syncobj_create release_create = {0};
    if (s_drm_fd < 0 || frame == nullptr) {
        return -EINVAL;
    }
    if (ioctl(s_drm_fd, DRM_IOCTL_SYNCOBJ_CREATE, &release_create) != 0) {
        fprintf(stderr, "[Engine Error] DRM_IOCTL_SYNCOBJ_CREATE: %s\n", strerror(errno));
        return -1;
    }
    frame->release_handle = release_create.handle;
    int release_fd = drm_syncobj_to_fd(s_drm_fd, frame->release_handle);
    if (release_fd < 0) {
        drm_syncobj_destroy(s_drm_fd, &frame->release_handle);
        return -1;
    }
    if (import_release_semaphore_fd(vk, release_fd, &frame->release_semaphore) < 0) {
        close(release_fd);
        drm_syncobj_destroy(s_drm_fd, &frame->release_handle);
        return -1;
    }
    uint32_t timeline_id = 0;
    int imported = wayland_import_syncobj_timeline(wl, release_fd, &timeline_id);
    close(release_fd);
    if (imported < 0) {
        if (timeline_id != 0 && wl != nullptr && wl->is_initialized && !wl->failed) {
            if (wayland_put_raw(wl, timeline_id, 0u, nullptr, 0u)) {
                (void)wayland_send_batch(wl);
            }
        }
        if (frame->release_semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(vk->device, frame->release_semaphore, nullptr);
            frame->release_semaphore = VK_NULL_HANDLE;
        }
        drm_syncobj_destroy(s_drm_fd, &frame->release_handle);
        return -1;
    }
    frame->release_timeline_id = timeline_id;
    frame->release_point = 1;
    return 0;
}

static void wait_own_release(VulkanContext *vk, FrameSlot *frame, uint64_t timeout_ns) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || frame == nullptr ||
        frame->release_semaphore == VK_NULL_HANDLE || frame->release_point == 0) {
        return;
    }
    VkSemaphoreWaitInfo wait_info = {
        .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .pNext          = nullptr,
        .flags          = 0,
        .semaphoreCount = 1,
        .pSemaphores    = &frame->release_semaphore,
        .pValues        = &frame->release_point,
    };
    (void)vkWaitSemaphores(vk->device, &wait_info, timeout_ns);
}

static void clear_pixels(FrameSlot *frame) {
    frame->image         = (VulkanExportableImage){ .dmabuf_fd = -1 };
    frame->image_view    = VK_NULL_HANDLE;
    frame->view_format   = VK_FORMAT_UNDEFINED;
    frame->depth         = (CubeDepth){0};
    frame->coverage      = (CubeCoverage){0};
    frame->cmd_buf       = VK_NULL_HANDLE;
    frame->acquire_point = 0;
    frame->wl_buffer_id  = 0;
}

static bool slot_has_pixels(const FrameSlot *frame) {
    return frame != nullptr &&
           (frame->image.image != VK_NULL_HANDLE ||
            frame->image_view != VK_NULL_HANDLE ||
            frame->cmd_buf != VK_NULL_HANDLE ||
            frame->wl_buffer_id != 0 ||
            frame->depth.image != VK_NULL_HANDLE ||
            frame->coverage.image != VK_NULL_HANDLE ||
            frame->image.dmabuf_fd >= 0);
}

static bool same_release(const FrameSlot *a, const FrameSlot *b) {
    if (a->release_semaphore != VK_NULL_HANDLE &&
        a->release_semaphore == b->release_semaphore) {
        return true;
    }
    if (a->release_timeline_id != 0 && a->release_timeline_id == b->release_timeline_id) {
        return true;
    }
    if (a->release_handle != 0 && a->release_handle == b->release_handle) {
        return true;
    }
    return false;
}

static bool release_held_elsewhere(const FrameSlot *frame) {
    for (int i = 0; i < 2; ++i) {
        if (&s_slot[i] != frame && same_release(frame, &s_slot[i])) {
            return true;
        }
    }
    for (uint32_t i = 0; i < s_orphan_count; ++i) {
        if (&s_orphans[i] != frame && same_release(frame, &s_orphans[i])) {
            return true;
        }
    }
    return false;
}

static void reap_frame_resources(WaylandContext *wl, VulkanContext *vk, FrameSlot *frame) {
    if (frame == nullptr) {
        return;
    }
    if (frame->release_eventfd >= 0) {
        close(frame->release_eventfd);
        frame->release_eventfd = -1;
    }
    if (frame->wl_buffer_id != 0) {
        destroy_wl_buffer_now(wl, frame->wl_buffer_id);
        frame->wl_buffer_id = 0;
    }
    // Several copies poll one semaphore: the slot, and each resized image
    // still on screen. The last copy destroys the Wayland timeline, the DRM
    // syncobj, and the Vulkan semaphore.
    bool last_release = (frame->release_semaphore != VK_NULL_HANDLE ||
                         frame->release_timeline_id != 0 ||
                         frame->release_handle != 0) &&
                        !release_held_elsewhere(frame);
    if (last_release) {
        if (frame->release_timeline_id != 0 && wl != nullptr && wl->is_initialized && !wl->failed) {
            uint32_t id = frame->release_timeline_id;
            if (wl->send_len > 0) {
                (void)wayland_send_batch(wl);
            }
            if (!wl->failed && wayland_put_raw(wl, id, 0u, nullptr, 0u)) {
                (void)wayland_send_batch(wl);
            }
            if (wl->release_timeline_id == id) {
                wl->release_timeline_id = 0;
            }
        }
        drm_syncobj_destroy(s_drm_fd, &frame->release_handle);
        if (vk != nullptr && vk->device != VK_NULL_HANDLE &&
            frame->release_semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(vk->device, frame->release_semaphore, nullptr);
        }
    }
    frame->release_timeline_id = 0;
    frame->release_handle = 0;
    frame->release_semaphore = VK_NULL_HANDLE;
    destroy_frame_gpu(vk, frame);
    frame->release_point = 0;
    frame->release_pending = false;
}

static void sweep_ready_orphans(WaylandContext *wl, VulkanContext *vk, bool force_all) {
    uint32_t write_idx = 0;
    for (uint32_t i = 0; i < s_orphan_count; ++i) {
        FrameSlot *orb = &s_orphans[i];
        bool ready = poll_release_semaphore(vk, orb->release_semaphore, orb->release_point);
        if (!ready && force_all) {
            wait_own_release(vk, orb, 3000000000ull);
            if (!poll_release_semaphore(vk, orb->release_semaphore, orb->release_point)) {
                fprintf(stderr,
                        "[Engine Error] Release point %llu was not signalled before shutdown\n",
                        (unsigned long long)orb->release_point);
            }
            ready = true;
        }
        if (ready) {
            reap_frame_resources(wl, vk, orb);
        } else {
            if (write_idx != i) {
                s_orphans[write_idx] = *orb;
            }
            write_idx++;
        }
    }
    s_orphan_count = write_idx;
}

static void make_orphan_room(WaylandContext *wl, VulkanContext *vk) {
    sweep_ready_orphans(wl, vk, false);
    if (s_orphan_count < k_orphan_cap) {
        return;
    }
    FrameSlot *oldest = &s_orphans[0];
    wait_own_release(vk, oldest, 500000000ull);
    reap_frame_resources(wl, vk, oldest);
    for (uint32_t i = 1; i < s_orphan_count; ++i) {
        s_orphans[i - 1] = s_orphans[i];
    }
    s_orphan_count--;
}

static void stash_orphan(WaylandContext *wl, VulkanContext *vk, FrameSlot *frame) {
    if (frame == nullptr) {
        return;
    }
    make_orphan_room(wl, vk);
    // The copy keeps the release syncobj. The source must not destroy it.
    s_orphans[s_orphan_count++] = *frame;
    frame->release_handle = 0;
    frame->release_semaphore = VK_NULL_HANDLE;
    frame->release_timeline_id = 0;
    frame->release_point = 0;
    frame->release_eventfd = -1;
    frame->release_pending = false;
    clear_pixels(frame);
}

static int alloc_content_image(
    VulkanContext         *vk,
    WaylandContext        *wl,
    uint32_t               width,
    uint32_t               height,
    VulkanExportableImage *out,
    const char           **tranche_kind
) {
    *out = (VulkanExportableImage){ .dmabuf_fd = -1 };
    *tranche_kind = "general";
    VulkanResult img_res = VULKAN_ERROR_FORMAT_NOT_SUPPORTED;
    if (wl->scanout_modifier_count > 0) {
        img_res = vulkan_create_exportable_image_with_modifiers(
            vk,
            width,
            height,
            VK_FORMAT_B8G8R8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            wl->scanout_modifiers,
            wl->scanout_modifier_count,
            out);
        if (img_res != VULKAN_SUCCESS) {
            img_res = vulkan_create_exportable_image_with_modifiers(
                vk,
                width,
                height,
                VK_FORMAT_B8G8R8A8_SRGB,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                wl->scanout_modifiers,
                wl->scanout_modifier_count,
                out);
        }
        if (img_res == VULKAN_SUCCESS) {
            *tranche_kind = "scanout";
        }
    }

    if (img_res != VULKAN_SUCCESS) {
        img_res = vulkan_create_exportable_image_with_modifiers(
            vk,
            width,
            height,
            VK_FORMAT_B8G8R8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            wl->all_modifiers,
            wl->all_modifier_count,
            out);
        if (img_res != VULKAN_SUCCESS) {
            img_res = vulkan_create_exportable_image_with_modifiers(
                vk,
                width,
                height,
                VK_FORMAT_B8G8R8A8_SRGB,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                wl->all_modifiers,
                wl->all_modifier_count,
                out);
        }
        *tranche_kind = "general";
    }

    if (img_res != VULKAN_SUCCESS || !out->has_drm_format_modifier) {
        fprintf(stderr, "[Engine Error] Failed to allocate modifier-backed DMA-BUF image: %s\n",
                vulkan_result_to_str(img_res));
        return -1;
    }

    const uint64_t min_stride = (uint64_t)out->width * 4ull;
    if (out->row_pitch < min_stride ||
        out->row_pitch > 0xffffffffu ||
        out->offset > 0xffffffffu) {
        fprintf(stderr,
                "[Engine Error] DMA-BUF layout is not importable (stride=%llu offset=%llu width=%u)\n",
                (unsigned long long)out->row_pitch,
                (unsigned long long)out->offset,
                out->width);
        vulkan_destroy_exportable_image(vk, out);
        return -1;
    }
    return 0;
}

static int note_idle_send(WaylandContext *wl, const struct io_uring_cqe *cqe, uint32_t pending_serial) {
    bool tracked = wl->send_tracks_ack;
    int noted = wayland_note_send_cqe(wl, cqe, pending_serial);
    if (!tracked && noted != 1) {
        pointer_on_send_done(noted == 0);
    }
    return noted;
}

static bool configure_needs_ack(const WaylandContext *wl) {
    return wl != nullptr && wl->xdg_surface_configured &&
           (!wl->configure_ack_valid || wl->configure_serial != wl->acked_configure_serial);
}

static bool consume_noted_signal(WaylandContext *wl, int signal_fd, int *signo) {
    if (wl == nullptr || !wl->noted_signal) {
        return false;
    }
    wl->noted_signal = false;
    struct signalfd_siginfo info;
    ssize_t nread = read(signal_fd, &info, sizeof info);
    if (nread == (ssize_t)sizeof info && signo != nullptr) {
        *signo = (int)info.ssi_signo;
    }
    return true;
}

static bool pending_content_size(const WaylandContext *wl, uint32_t *width, uint32_t *height) {
    if (wl->configure_width == 0 || wl->configure_height == 0) {
        return false;
    }
    uint32_t w = wl->configure_width;
    uint32_t h = wl->configure_height;
    chrome_clamp_size(&w, &h);
    if (width != nullptr) {
        *width = w;
    }
    if (height != nullptr) {
        *height = h;
    }
    return w != shown()->image.width || h != shown()->image.height;
}

static bool content_size_pending(const ChromeSpec *spec, const WaylandContext *wl) {
    return spec != nullptr && spec->resize && pending_content_size(wl, nullptr, nullptr);
}

static int cancel_release_poll(RingContext *ring, WaylandContext *wl, bool *released) {
    *released = false;
    struct io_uring_sqe *sqe = ring_sqe(ring);
    if (sqe == nullptr) {
        return -ENOSPC;
    }
    sqe->opcode    = IORING_OP_POLL_REMOVE;
    sqe->addr      = ENG_UD_GPU_RELEASE;
    sqe->user_data = k_ud_release_cancel;
    uint64_t deadline = ring_mono_ns() + 3000000000ull;
    for (;;) {
        struct io_uring_cqe cqe;
        while (ring_peek(ring, &cqe)) {
            if (cqe.user_data == ENG_UD_GPU_RELEASE) {
                *released = cqe.res >= 0;
                return 0;
            }
            if (cqe.user_data == k_ud_release_cancel) {
                return 0;
            }
            wayland_on_cqe(wl, &cqe);
        }
        if (ring_mono_ns() > deadline) {
            return -ETIMEDOUT;
        }
        unsigned n = ring_submit(ring);
        int rc = ring_enter(ring, n, 1, 200);
        if (rc < 0 && rc != -ETIME && rc != -EINTR) {
            return rc;
        }
    }
}

static int register_release_eventfd(FrameSlot *frame) {
    if (frame == nullptr || frame->release_handle == 0 || frame->release_point == 0 ||
        s_drm_fd < 0) {
        return -EINVAL;
    }
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[Engine Error] eventfd: %s\n", strerror(errno));
        return -1;
    }
    struct drm_syncobj_eventfd release_event = {
        .handle = frame->release_handle,
        .flags  = 0,
        .point  = frame->release_point,
        .fd     = fd,
        .pad    = 0,
    };
    if (ioctl(s_drm_fd, DRM_IOCTL_SYNCOBJ_EVENTFD, &release_event) != 0) {
        fprintf(stderr, "[Engine Error] DRM_IOCTL_SYNCOBJ_EVENTFD: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    frame->release_eventfd = fd;
    return 0;
}

// Drop the images and the wl_buffer. The release syncobj stays on the slot.
// A buffer the compositor still holds is orphaned at the old point and polls
// the same semaphore. It must not destroy that semaphore. A pending release
// moves this slot's point forward by one.
static void retire_pixels(WaylandContext *wl, VulkanContext *vk, FrameSlot *frame) {
    if (frame == nullptr) {
        return;
    }
    bool pending = frame->release_pending;
    uint64_t old_point = frame->release_point;
    if (!slot_has_pixels(frame)) {
        if (pending && old_point != 0) {
            frame->release_point = old_point + 1u;
        }
        frame->release_pending = false;
        return;
    }

    bool held = pending &&
                !poll_release_semaphore(vk, frame->release_semaphore, old_point);
    FrameSlot pixels = *frame;
    pixels.release_eventfd = -1;
    pixels.release_pending = false;

    clear_pixels(frame);
    frame->release_pending = false;
    if (pending && old_point != 0) {
        frame->release_point = old_point + 1u;
    }

    if (!held) {
        pixels.release_handle = 0;
        pixels.release_semaphore = VK_NULL_HANDLE;
        pixels.release_timeline_id = 0;
        pixels.release_point = 0;
        reap_frame_resources(wl, vk, &pixels);
        return;
    }

    pixels.release_point = old_point;
    make_orphan_room(wl, vk);
    s_orphans[s_orphan_count++] = pixels;
}

static void blank_frame(FrameSlot *frame) {
    *frame = (FrameSlot){
        .image           = { .dmabuf_fd = -1 },
        .release_eventfd = -1,
    };
}

static void retire_spare(WaylandContext *wl, VulkanContext *vk) {
    if (!s_spare_live) {
        return;
    }
    bool held = spare()->release_pending &&
                !poll_release_semaphore(vk, spare()->release_semaphore, spare()->release_point);
    if (held) {
        stash_orphan(wl, vk, spare());
    } else {
        reap_frame_resources(wl, vk, spare());
    }
    blank_frame(spare());
    s_spare_live = false;
}

static int alloc_spare(WaylandContext *wl, VulkanContext *vk) {
    if (shown()->image.width == 0 || shown()->image.height == 0) {
        return -EINVAL;
    }
    // The first spare imports one timeline. A later call at a new size keeps
    // that timeline and replaces the images.
    if (s_spare_live &&
        spare()->image.image != VK_NULL_HANDLE &&
        spare()->image.width == shown()->image.width &&
        spare()->image.height == shown()->image.height) {
        return 0;
    }

    const bool keep_timeline = spare()->release_timeline_id != 0;
    if (!keep_timeline) {
        blank_frame(spare());
    } else if (slot_has_pixels(spare())) {
        retire_pixels(wl, vk, spare());
    }

    const char *tranche_kind = "general";
    if (alloc_content_image(vk, wl, shown()->image.width, shown()->image.height,
                            &spare()->image, &tranche_kind) != 0) {
        if (!keep_timeline) {
            blank_frame(spare());
            s_spare_live = false;
        }
        return -1;
    }
    (void)tranche_kind;
    if (!keep_timeline && create_release_timeline(vk, wl, spare()) != 0) {
        spare()->acquire_point = 0;
        destroy_frame_gpu(vk, spare());
        blank_frame(spare());
        s_spare_live = false;
        return -1;
    }
    uint32_t front_buffer = wl->buffer_id;
    uint32_t spare_buffer = 0;
    int imported = wayland_import_dmabuf_immed(
        wl,
        spare()->image.dmabuf_fd,
        spare()->image.width,
        spare()->image.height,
        (uint32_t)spare()->image.row_pitch,
        (uint32_t)spare()->image.offset,
        ENG_DRM_FORMAT_ARGB8888,
        spare()->image.drm_format_modifier,
        &spare_buffer);
    wl->buffer_id = front_buffer;
    if (imported < 0) {
        if (keep_timeline) {
            FrameSlot junk = *spare();
            junk.release_eventfd = -1;
            junk.release_handle = 0;
            junk.release_semaphore = VK_NULL_HANDLE;
            junk.release_timeline_id = 0;
            junk.release_point = 0;
            clear_pixels(spare());
            reap_frame_resources(wl, vk, &junk);
        } else {
            reap_frame_resources(wl, vk, spare());
            blank_frame(spare());
            s_spare_live = false;
        }
        return -1;
    }
    spare()->wl_buffer_id = spare_buffer;
    spare()->release_pending = false;
    s_spare_live = true;
    return 0;
}

static void swap_front_spare(void) {
    s_front ^= 1;
    s_spare_live = true;
}

static void commit_turn_swap(WaylandContext *wl) {
    if (!s_turn_swap_pending) {
        return;
    }
    spare()->release_point = s_pending_release;
    spare()->release_pending = true;
    swap_front_spare();
    world_commit();
    if (wl != nullptr) {
        wl->buffer_id = shown()->wl_buffer_id;
        wl->release_timeline_id = shown()->release_timeline_id;
    }
    s_turn_swap_pending = false;
}

static int queue_frame_only(WaylandContext *wl, bool *queued) {
    int q = wayland_queue_turn_commit(wl, 0, 0, 0, 0, 0, 0, 0, true, false, 0);
    if (q == 0) {
        *queued = true;
    }
    return q;
}

static bool turn_wants_step(const WaylandContext *wl) {
    if (s_turn_phase == k_turn_done) {
        return false;
    }
    if (s_turn_phase == k_turn_arm) {
        return true;
    }
    return wl != nullptr && wl->frame_stamp_valid;
}

static int turn_step(WaylandContext *wl, VulkanContext *vk, bool *queued) {
    *queued = false;
    if (wl == nullptr || vk == nullptr) {
        return -EINVAL;
    }
    if (s_turn_phase == k_turn_done) {
        wl->frame_stamp_valid = false;
        return 0;
    }
    if (!s_spare_live && alloc_spare(wl, vk) != 0) {
        return -1;
    }
    if (s_turn_phase == k_turn_arm) {
        int q = queue_frame_only(wl, queued);
        if (q == 0 || q == -EALREADY) {
            s_turn_phase = k_turn_run;
            return 0;
        }
        return q;
    }
    if (!wl->frame_stamp_valid) {
        return 0;
    }
    uint32_t stamp = wl->frame_time_ms;
    bool pulse_ended = false;
    bool pulse = cube_heat_wants_frame(stamp, &pulse_ended);
    if (pulse_ended) {
        cube_heat_hold();
    }
    uint32_t heat_dt = cube_heat_interval(stamp);
    bool changed = world_advance(stamp);
    // The pose can stay still. An open pulse still presents, because the
    // spot and the diffusion change the picture. The cube's yaw does not move.
    bool paint = changed || (pulse && (heat_dt > 0u || cube_heat_poke_pending()));
    if (!paint) {
        if (!world_busy() && !pulse) {
            wl->frame_stamp_valid = false;
            s_turn_phase = k_turn_done;
            retire_spare(wl, vk);
            uint32_t frames = 0;
            uint32_t missed = 0;
            bool spun = world_consume_spin();
            hud_turn_report(&frames, &missed);
            if (spun) {
                printf("[Engine] Turn idle. Pose repeated. Frame callback dropped.\n");
                printf("[Engine] %u frames, %u missed.\n", frames, missed);
                fflush(stdout);
                cube_heat_hold();
            }
            return 0;
        }
        int q = queue_frame_only(wl, queued);
        if (q == 0 || q == -EALREADY) {
            wl->frame_stamp_valid = false;
            return 0;
        }
        return q;
    }

    bool spare_free = acquire_point_retired(vk, spare()->acquire_point) &&
        (!spare()->release_pending ||
         poll_release_semaphore(vk, spare()->release_semaphore, spare()->release_point));
    if (!spare_free) {
        int q = queue_frame_only(wl, queued);
        if (q == 0 || q == -EALREADY) {
            wl->frame_stamp_valid = false;
            return 0;
        }
        return q;
    }

    // A resize stores the point it presented and leaves release_pending false,
    // so this bump runs once per present.
    uint64_t rel = spare()->release_point;
    if (spare()->release_pending) {
        rel += 1u;
    }
    uint64_t next = s_sync_point + 1u;
    if (record_and_submit_clear(vk, spare(), s_acquire, next, true, heat_dt) != 0) {
        spare()->acquire_point = 0;
        return -1;
    }
    if (heat_dt > 0u) {
        cube_heat_commit_stamp(stamp);
    }
    s_sync_point = next;
    int q = wayland_queue_turn_commit(
        wl,
        spare()->wl_buffer_id,
        spare()->image.width,
        spare()->image.height,
        wl->acquire_timeline_id,
        next,
        spare()->release_timeline_id,
        rel,
        true,
        false,
        0);
    if (q != 0) {
        return -1;
    }
    wl->frame_stamp_valid = false;
    s_pending_release = rel;
    s_turn_swap_pending = true;
    *queued = true;
    return 0;
}

static void drop_resize_attempt(WaylandContext *wl, VulkanContext *vk) {
    if (!s_resize_pending) {
        return;
    }
    chrome_set_buffer_size(s_resize_old_w, s_resize_old_h);
    if (wl != nullptr) {
        wl->buffer_id = s_resize_old_buffer;
    }
    reap_frame_resources(wl, vk, &s_resize_fresh);
    blank_frame(&s_resize_fresh);
    s_resize_pending = false;
}

static int install_resize(WaylandContext *wl, VulkanContext *vk) {
    if (!s_resize_pending) {
        return 0;
    }
    FrameSlot fresh = s_resize_fresh;
    uint64_t rel = s_resize_release;
    uint32_t serial = s_resize_serial;
    char tranche[16];
    snprintf(tranche, sizeof tranche, "%s", s_resize_tranche);
    blank_frame(&s_resize_fresh);
    s_resize_pending = false;

    retire_pixels(wl, vk, shown());

    shown()->image           = fresh.image;
    shown()->image_view      = fresh.image_view;
    shown()->view_format     = fresh.view_format;
    shown()->depth           = fresh.depth;
    shown()->coverage        = fresh.coverage;
    shown()->cmd_buf         = fresh.cmd_buf;
    shown()->acquire_point   = fresh.acquire_point;
    shown()->wl_buffer_id    = fresh.wl_buffer_id;
    shown()->release_pending = true;
    shown()->release_point   = rel;
    if (wl != nullptr) {
        wl->release_timeline_id = shown()->release_timeline_id;
        wl->buffer_id           = fresh.wl_buffer_id;
    }

    if (shown()->release_eventfd >= 0) {
        close(shown()->release_eventfd);
        shown()->release_eventfd = -1;
    }
    if (register_release_eventfd(shown()) != 0) {
        fprintf(stderr,
                "[Engine] Resize kept timeline %u at release point %llu. The eventfd was not re-armed.\n",
                shown()->release_timeline_id,
                (unsigned long long)shown()->release_point);
    }

    printf("[Engine] Resized buffer to %ux%u (serial=%u), modifier=0x%llx (%s tranche), stride=%zu, explicit-sync acquire=%llu release=%llu timeline=%u\n",
           shown()->image.width,
           shown()->image.height,
           serial,
           (unsigned long long)shown()->image.drm_format_modifier,
           tranche[0] != '\0' ? tranche : "general",
           (size_t)shown()->image.row_pitch,
           (unsigned long long)s_sync_point,
           (unsigned long long)shown()->release_point,
           shown()->release_timeline_id);

    if (s_turn_phase != k_turn_done) {
        if (alloc_spare(wl, vk) != 0) {
            return -1;
        }
    }
    return 0;
}

static int finish_idle_send(WaylandContext *wl, VulkanContext *vk,
                            const struct io_uring_cqe *cqe, uint32_t pending_serial) {
    uint32_t serial = s_resize_pending ? s_resize_serial : pending_serial;
    int noted = note_idle_send(wl, cqe, serial);
    if (noted < 0) {
        drop_resize_attempt(wl, vk);
        s_turn_swap_pending = false;
        return noted;
    }
    if (noted == 1) {
        return 1;
    }
    if (s_resize_pending) {
        return install_resize(wl, vk);
    }
    commit_turn_swap(wl);
    return 0;
}

// Records the new size into a side slot and queues the commit. The shown slot
// stays the front until the send completion. No wl_display.sync and no frame callback.
static int queue_resize_frame(WaylandContext *wl, VulkanContext *vk) {
    uint32_t width  = wl->configure_width;
    uint32_t height = wl->configure_height;
    chrome_clamp_size(&width, &height);
    const uint32_t serial = wl->configure_serial;
    if (s_resize_pending || width == 0 || height == 0 || s_acquire == VK_NULL_HANDLE ||
        shown()->release_timeline_id == 0 ||
        shown()->release_semaphore == VK_NULL_HANDLE) {
        return -EINVAL;
    }
    sweep_ready_orphans(wl, vk, false);

    FrameSlot fresh = {
        .image           = { .dmabuf_fd = -1 },
        .release_eventfd = -1,
    };
    const char *tranche_kind = "general";
    if (alloc_content_image(vk, wl, width, height, &fresh.image, &tranche_kind) != 0) {
        return -1;
    }

    uint64_t next_point = s_sync_point + 1u;
    if (record_and_submit_clear(vk, &fresh, s_acquire, next_point, false, 0u) != 0) {
        fresh.acquire_point = 0;
        reap_frame_resources(wl, vk, &fresh);
        return -1;
    }
    s_sync_point = next_point;

    const uint32_t previous_buffer = shown()->wl_buffer_id;
    uint32_t new_buffer = 0;
    int imported = wayland_import_dmabuf_immed(
        wl,
        fresh.image.dmabuf_fd,
        fresh.image.width,
        fresh.image.height,
        (uint32_t)fresh.image.row_pitch,
        (uint32_t)fresh.image.offset,
        ENG_DRM_FORMAT_ARGB8888,
        fresh.image.drm_format_modifier,
        &new_buffer);
    wl->buffer_id = previous_buffer;
    if (imported < 0) {
        reap_frame_resources(wl, vk, &fresh);
        return -1;
    }
    fresh.wl_buffer_id = new_buffer;

    uint64_t rel = shown()->release_point;
    if (shown()->release_pending) {
        rel += 1u;
    }

    uint32_t old_w = shown()->image.width;
    uint32_t old_h = shown()->image.height;
    chrome_set_buffer_size(fresh.image.width, fresh.image.height);
    int queued = wayland_queue_turn_commit(
        wl,
        new_buffer,
        fresh.image.width,
        fresh.image.height,
        wl->acquire_timeline_id,
        s_sync_point,
        shown()->release_timeline_id,
        rel,
        false,
        true,
        serial);
    if (queued != 0) {
        chrome_set_buffer_size(old_w, old_h);
        wl->buffer_id = previous_buffer;
        reap_frame_resources(wl, vk, &fresh);
        return -1;
    }

    s_resize_fresh = fresh;
    s_resize_release = rel;
    s_resize_serial = serial;
    s_resize_old_buffer = previous_buffer;
    s_resize_old_w = old_w;
    s_resize_old_h = old_h;
    snprintf(s_resize_tranche, sizeof s_resize_tranche, "%s", tranche_kind != nullptr ? tranche_kind : "general");
    s_resize_pending = true;
    return 0;
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
    if (!vk->device_info.supports_external_syncobj ||
        vk->dispatch.vkGetSemaphoreFdKHR == nullptr ||
        vk->dispatch.vkImportSemaphoreFdKHR == nullptr ||
        wl->drm_syncobj_id == 0 || !vk->device_info.has_drm_render_node) {
        fprintf(stderr,
                "[Engine Error] Explicit sync needs a DRM render node, VK_KHR_external_semaphore_fd, and wp_linux_drm_syncobj_manager_v1\n");
        return 1;
    }

    if (chrome_load() != 0) {
        return 1;
    }
    const ChromeSpec *spec = chrome_spec();
    if (spec == nullptr || pointer_check_spec(wl) != 0) {
        return 1;
    }

    s_front = 0;
    s_drm_fd = -1;
    s_acquire = VK_NULL_HANDLE;
    blank_frame(shown());
    blank_frame(spare());
    s_spare_live = false;
    s_turn_phase = k_turn_arm;
    s_turn_swap_pending = false;
    s_pending_release = 0;
    s_resize_pending = false;
    blank_frame(&s_resize_fresh);
    s_resize_release = 0;
    s_resize_serial = 0;
    s_resize_old_buffer = 0;
    s_resize_old_w = 0;
    s_resize_old_h = 0;
    s_resize_tranche[0] = '\0';
    world_reset();
    s_sync_point = 0;
    s_orphan_count = 0;
    s_stuck_release_fd = -1;
    s_presented = true;

    uint32_t width = 0, height = 0, serial = 0;
    if (spec->resize) {
        uint32_t min_w = 1;
        uint32_t min_h = 1;
        chrome_min_size(&min_w, &min_h);
        if (min_w > 0x7fffffffu) {
            min_w = 0x7fffffffu;
        }
        if (min_h > 0x7fffffffu) {
            min_h = 0x7fffffffu;
        }
        wl->min_width = (int32_t)min_w;
        wl->min_height = (int32_t)min_h;
        printf("[Engine] Toplevel minimum %ux%u\n", min_w, min_h);
        fflush(stdout);
    }
    if (wayland_create_toplevel_and_configure(wl, spec->title, spec->app_id,
                                              &width, &height, &serial) < 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }
    chrome_clamp_size(&width, &height);

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

    const char *tranche_kind = "general";
    if (alloc_content_image(vk, wl, width, height, &shown()->image, &tranche_kind) != 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }

    if (create_acquire_semaphore(vk, &s_acquire) < 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }
    if (cube_init(vk) != 0 || solid_init(vk) != 0 || shape_init(vk) != 0 ||
        chrome_gpu_init(vk) != 0 || hud_init(vk) != 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }
    if (record_and_submit_clear(vk, shown(), s_acquire, 1u, false, 0u) < 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }
    s_sync_point = 1u;

    char render_path[64];
    int path_n = snprintf(render_path, sizeof render_path, "/dev/dri/renderD%lld",
                          (long long)vk->device_info.drm_render_minor);
    if (path_n < 0 || (size_t)path_n >= sizeof render_path) {
        fprintf(stderr, "[Engine Error] DRM render node path is too long\n");
        (void)engine_subsystem_shutdown();
        return 1;
    }
    s_drm_fd = open(render_path, O_RDWR | O_CLOEXEC);
    if (s_drm_fd < 0) {
        fprintf(stderr, "[Engine Error] open %s: %s\n", render_path, strerror(errno));
        (void)engine_subsystem_shutdown();
        return 1;
    }

    int acquire_vk_fd = export_semaphore_fd(vk, s_acquire);
    if (acquire_vk_fd < 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }
    uint32_t acquire_handle = 0;
    int imported = drm_syncobj_from_fd(s_drm_fd, acquire_vk_fd, &acquire_handle);
    close(acquire_vk_fd);
    if (imported < 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }
    int acquire_fd = drm_syncobj_to_fd(s_drm_fd, acquire_handle);
    drm_syncobj_destroy(s_drm_fd, &acquire_handle);
    if (acquire_fd < 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }

    if (create_release_timeline(vk, wl, shown()) != 0) {
        close(acquire_fd);
        (void)engine_subsystem_shutdown();
        return 1;
    }
    wl->release_timeline_id = shown()->release_timeline_id;

    int acquire_import = wayland_import_syncobj_timeline(wl, acquire_fd, &wl->acquire_timeline_id);
    close(acquire_fd);
    if (acquire_import < 0 || wayland_bind_explicit_sync(wl) < 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }

    if (register_release_eventfd(shown()) != 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }

    if (wayland_import_dmabuf_immed(
            wl,
            shown()->image.dmabuf_fd,
            shown()->image.width,
            shown()->image.height,
            (uint32_t)shown()->image.row_pitch,
            (uint32_t)shown()->image.offset,
            ENG_DRM_FORMAT_ARGB8888,
            shown()->image.drm_format_modifier,
            &shown()->wl_buffer_id) < 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }

    chrome_set_buffer_size(shown()->image.width, shown()->image.height);
    if (!spec->resize) {
        wl->lock_toplevel_size = true;
        wl->lock_width = (int32_t)shown()->image.width;
        wl->lock_height = (int32_t)shown()->image.height;
    }

    uint32_t present_serial = (wl->configure_serial != 0u) ? wl->configure_serial : serial;
    if (wayland_present_single_frame_and_wait(
            wl,
            shown()->wl_buffer_id,
            shown()->image.width,
            shown()->image.height,
            present_serial,
            wl->acquire_timeline_id,
            s_sync_point,
            shown()->release_timeline_id,
            shown()->release_point) < 0) {
        (void)engine_subsystem_shutdown();
        return 1;
    }

    if (pointer_flush(wl) < 0) {
        fprintf(stderr, "[Engine Error] Cursor was not set: %s\n", wl->err);
        (void)engine_subsystem_shutdown();
        return 1;
    }

    shown()->release_pending = true;
    printf("[Engine] Presented 1 visible frame: size=%ux%u (serial=%u), fourcc=0x%08x (ARGB8888), modifier=0x%llx (%s tranche), stride=%zu, offset=%zu, fd=%d, explicit-sync acquire=%llu release=%llu timeline=%u\n",
           shown()->image.width,
           shown()->image.height,
           present_serial,
           ENG_DRM_FORMAT_ARGB8888,
           (unsigned long long)shown()->image.drm_format_modifier,
           tranche_kind,
           (size_t)shown()->image.row_pitch,
           (size_t)shown()->image.offset,
           shown()->image.dmabuf_fd,
           (unsigned long long)s_sync_point,
           (unsigned long long)shown()->release_point,
           shown()->release_timeline_id);
    printf("hello init from engine\n");
    return 0;
}

int engine_subsystem_run(void) {
    if (!s_presented || !shown()->release_pending) {
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

    bool signal_fixed = (ring_update_file(ring, ENG_SIGNAL_FD_SLOT, signal_fd) == 0);
    int poll_rc = signal_fixed
        ? ring_arm_poll_fixed(ring, ENG_SIGNAL_FD_SLOT, POLLIN, ENG_UD_SIGNAL)
        : ring_arm_poll(ring, signal_fd, POLLIN, ENG_UD_SIGNAL);
    if (poll_rc != 0) {
        fprintf(stderr, "[Engine Error] Failed to arm io_uring poll on signalfd\n");
        if (signal_fixed) {
            (void)ring_update_file(ring, ENG_SIGNAL_FD_SLOT, -1);
        }
        close(signal_fd);
        (void)sigprocmask(SIG_SETMASK, &old_mask, nullptr);
        return 1;
    }

    printf("[Engine] Window %ux%u is up. One turn, then io_uring idle. Four clicks on empty space close a rectangle. A click on the cube plants heat for five seconds. Its colors sweep on that clock, then hold. Keys 1-6 raise a tetrahedron, octahedron, icosahedron, cylinder, cone, and torus. R spins until R again. S smooths edges. Esc clears, Backspace removes, F5 turns the selection again.\n",
           shown()->image.width, shown()->image.height);
    fflush(stdout);

    const ChromeSpec *spec = chrome_spec();
    VulkanContext *vk = vulkan_get_context();
    bool     signal_poll_armed = true;
    bool     cancel_in_flight  = false;
    bool     send_in_flight    = false;
    bool     resend_wire       = false;
    uint32_t pending_serial    = 0;
    bool     got_signal        = false;
    int      signo             = 0;
    int      run_rc            = 0;
    const char *why            = "idle wait ended";

    for (;;) {
        struct io_uring_cqe cqe;
        while (ring_peek(ring, &cqe)) {
            hud_note_cqe();
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
                int noted = finish_idle_send(wl, vk, &cqe, pending_serial);
                if (noted < 0) {
                    send_in_flight = false;
                    run_rc = 1;
                } else if (noted == 1) {
                    send_in_flight = false;
                    resend_wire = true;
                } else {
                    send_in_flight = false;
                }
            } else {
                wayland_on_cqe(wl, &cqe);
            }
        }

        if (consume_noted_signal(wl, signal_fd, &signo)) {
            signal_poll_armed = false;
            got_signal = true;
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

        sweep_ready_orphans(wl, vk, false);

        if (!send_in_flight && shown()->image.width != 0 && shown()->image.height != 0) {
            int32_t sx = 0;
            int32_t sy = 0;
            while (pointer_take_spawn(&sx, &sy)) {
                float ox = (float)sx - (float)shown()->image.width * 0.5f;
                float oy = (float)shown()->image.height * 0.5f - (float)sy;
                if (world_hits_cube(ox, oy)) {
                    if (cube_heat_poke(ox, oy) && s_turn_phase == k_turn_done) {
                        s_turn_phase = k_turn_arm;
                    }
                } else if (world_hits_solid(ox, oy)) {
                    if (world_select_solid_at(ox, oy) && s_turn_phase == k_turn_done) {
                        s_turn_phase = k_turn_arm;
                    }
                } else if (world_select_at(ox, oy)) {
                    if (s_turn_phase == k_turn_done) {
                        s_turn_phase = k_turn_arm;
                    }
                } else if (world_spawn(ox, oy) && s_turn_phase == k_turn_done) {
                    s_turn_phase = k_turn_arm;
                }
            }
            uint32_t key = 0;
            while (pointer_take_key(&key)) {
                bool changed = false;
                if (key == KEY_ESC) {
                    changed = world_cancel();
                } else if (key == KEY_BACKSPACE) {
                    changed = world_delete();
                } else if (key == KEY_F5) {
                    changed = world_respin_selected();
                } else if (key == KEY_R) {
                    changed = world_toggle_spin();
                } else if (key == KEY_S) {
                    changed = world_toggle_smooth();
                } else if (key >= KEY_1 && key <= KEY_6) {
                    changed = world_raise_solid(key - KEY_1);
                }
                if (changed && s_turn_phase == k_turn_done) {
                    s_turn_phase = k_turn_arm;
                }
            }
        }

        if (!send_in_flight && resend_wire) {
            resend_wire = false;
            int queued = wayland_queue_send(wl, wl->send_tracks_ack);
            if (queued == 0) {
                send_in_flight = true;
            } else {
                run_rc = 1;
                why = wl->err[0] != '\0' ? wl->err : "wayland send retry failed";
                break;
            }
        } else if (!send_in_flight && content_size_pending(spec, wl)) {
            uint32_t next_w = 0;
            uint32_t next_h = 0;
            (void)pending_content_size(wl, &next_w, &next_h);
            printf("[Engine] Resize buffer %ux%u -> %ux%u\n",
                   shown()->image.width, shown()->image.height, next_w, next_h);
            fflush(stdout);
            if (vk == nullptr || queue_resize_frame(wl, vk) != 0) {
                run_rc = 1;
                why = wl->err[0] != '\0' ? wl->err : "buffer replace failed";
                if (consume_noted_signal(wl, signal_fd, &signo)) {
                    signal_poll_armed = false;
                    got_signal = true;
                    why = "signal";
                    run_rc = 0;
                }
                break;
            }
            pending_serial = s_resize_serial;
            send_in_flight = true;
            if (consume_noted_signal(wl, signal_fd, &signo)) {
                signal_poll_armed = false;
                got_signal = true;
                why = "signal";
                break;
            }
        }

        if (!send_in_flight && configure_needs_ack(wl)) {
            uint32_t width  = shown()->image.width;
            uint32_t height = shown()->image.height;
            // The buffer is already the clamped configure size, bar included.
            // Scaling it would stretch the bar. A leftover destination is cleared.
            int32_t viewport_mode = ENG_VIEWPORT_LEAVE;
            pending_serial = wl->configure_serial;
            int queued = wayland_queue_configure_ack(wl, pending_serial, width, height, viewport_mode);
            if (queued == 0) {
                send_in_flight = true;
            } else if (queued != -EBUSY) {
                run_rc = 1;
                why    = wl->err[0] != '\0' ? wl->err : "configure ack failed";
                break;
            }
        } else if (!send_in_flight && turn_wants_step(wl)) {
            bool queued = false;
            int stepped = turn_step(wl, vk, &queued);
            if (stepped != 0 && stepped != -EBUSY) {
                run_rc = 1;
                why = wl->err[0] != '\0' ? wl->err : "turn step failed";
                break;
            }
            if (queued) {
                send_in_flight = true;
            }
        } else if (!send_in_flight) {
            int enc = pointer_encode(wl);
            if (enc < 0) {
                pointer_on_send_done(false);
                run_rc = 1;
                why = "pointer encode failed";
                break;
            }
            if (enc == 1) {
                int queued = wayland_queue_send(wl, false);
                if (queued == 0) {
                    send_in_flight = true;
                } else {
                    pointer_on_send_done(false);
                    run_rc = 1;
                    why = wl->err[0] != '\0' ? wl->err : "pointer send failed";
                    break;
                }
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

    if (consume_noted_signal(wl, signal_fd, &signo)) {
        signal_poll_armed = false;
        got_signal = true;
        why = "signal";
    }

    if (signal_poll_armed) {
        struct io_uring_sqe *cancel_sqe = ring_sqe(ring);
        if (cancel_sqe != nullptr) {
            cancel_sqe->opcode    = IORING_OP_POLL_REMOVE;
            cancel_sqe->addr      = ENG_UD_SIGNAL;
            cancel_sqe->user_data = k_ud_signal_cancel;
            cancel_in_flight      = true;
        }
    }

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
        hud_note_cqe();
        if (cqe.user_data == ENG_UD_WL_SEND) {
            int noted = finish_idle_send(wl, vk, &cqe, pending_serial);
            if (noted < 0) {
                send_in_flight = false;
                run_rc = 1;
            } else if (noted == 1) {
                if (wayland_queue_send(wl, wl->send_tracks_ack) == 0) {
                    send_in_flight = true;
                } else {
                    send_in_flight = false;
                    run_rc = 1;
                }
            } else {
                send_in_flight = false;
            }
        } else if (cqe.user_data == ENG_UD_SIGNAL) {
            signal_poll_armed = false;
        } else if (cqe.user_data == k_ud_signal_cancel) {
            cancel_in_flight = false;
        } else {
            wayland_on_cqe(wl, &cqe);
        }
    }

    if (signal_fixed) {
        (void)ring_update_file(ring, ENG_SIGNAL_FD_SLOT, -1);
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
    if (!s_presented) {
        return 0;
    }
    WaylandContext *wl   = wayland_get_context();
    VulkanContext  *vk   = vulkan_get_context();
    RingContext    *ring = ring_get_context();
    int             release_rc = 0;

    if (wl != nullptr) {
        pointer_shutdown(wl);
    }
    if (s_resize_pending) {
        drop_resize_attempt(wl, vk);
    }
    chrome_set_buffer_size(0, 0);

    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        (void)vkDeviceWaitIdle(vk->device);

        if (shown()->cmd_buf != VK_NULL_HANDLE && vk->cmd_pool != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &shown()->cmd_buf);
            shown()->cmd_buf = VK_NULL_HANDLE;
        }
        if (shown()->image_view != VK_NULL_HANDLE) {
            vkDestroyImageView(vk->device, shown()->image_view, nullptr);
            shown()->image_view = VK_NULL_HANDLE;
        }
        retire_spare(wl, vk);
    }

    if (s_stuck_release_fd >= 0) {
        fprintf(stderr, "[Engine Error] GPU release poll could not be removed\n");
        release_rc = 1;
    } else if (shown()->release_pending) {
        if (wl == nullptr || !wl->is_initialized || wayland_detach_buffer(wl) < 0) {
            fprintf(stderr, "[Engine Error] Buffer detach failed before GPU release wait\n");
        }
        bool released_now = poll_release_semaphore(vk, shown()->release_semaphore, shown()->release_point);
        if (!released_now) {
            // The eventfd was armed for the first release point. A reused image
            // waits on its own later point.
            if (shown()->release_eventfd >= 0) {
                close(shown()->release_eventfd);
                shown()->release_eventfd = -1;
            }
            (void)register_release_eventfd(shown());
        }
        bool armed = !released_now && shown()->release_eventfd >= 0 &&
                     ring != nullptr && ring->is_initialized &&
                     wl != nullptr && wl->is_initialized &&
                     arm_release_poll_fixed(ring, shown()->release_eventfd) == 0;
        if (released_now || (armed && wait_gpu_release(ring, wl) == 0)) {
            printf("[Engine] GPU release point signalled (io_uring poll)\n");
        } else {
            release_rc = 1;
            if (!armed && shown()->release_eventfd < 0) {
                fprintf(stderr, "[Engine Error] GPU release eventfd is closed\n");
            }
            if (armed) {
                bool late = false;
                if (cancel_release_poll(ring, wl, &late) != 0) {
                    fprintf(stderr, "[Engine Error] GPU release poll is still armed\n");
                    s_stuck_release_fd = shown()->release_eventfd;
                    shown()->release_eventfd = -1;
                } else if (late || poll_release_semaphore(vk, shown()->release_semaphore, shown()->release_point)) {
                    printf("[Engine] GPU release point signalled (io_uring poll)\n");
                    release_rc = 0;
                }
            }
        }
        if (armed && s_stuck_release_fd < 0) {
            (void)ring_update_file(ring, ENG_RELEASE_FD_SLOT, -1);
        }
    }

    if (shown()->release_eventfd >= 0) {
        close(shown()->release_eventfd);
        shown()->release_eventfd = -1;
    }

    sweep_ready_orphans(wl, vk, true);

    if (wl != nullptr && wl->is_initialized) {
        wayland_destroy_frame_objects(wl);
        shown()->wl_buffer_id = 0;
    }

    drm_syncobj_destroy(s_drm_fd, &shown()->release_handle);
    if (s_drm_fd >= 0) {
        close(s_drm_fd);
        s_drm_fd = -1;
    }

    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        if (shown()->release_semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(vk->device, shown()->release_semaphore, nullptr);
            shown()->release_semaphore = VK_NULL_HANDLE;
        }
        if (s_acquire != VK_NULL_HANDLE) {
            vkDestroySemaphore(vk->device, s_acquire, nullptr);
            s_acquire = VK_NULL_HANDLE;
        }
        vulkan_destroy_exportable_image(vk, &shown()->image);
        cube_depth_destroy(vk, &shown()->depth);
        cube_coverage_destroy(vk, &shown()->coverage);
        chrome_gpu_shutdown(vk);
        hud_shutdown(vk);
        shape_shutdown(vk);
        solid_shutdown(vk);
        cube_shutdown(vk);
    }

    s_presented = false;
    shown()->release_pending = false;
    printf("goodbye from engine\n");
    return release_rc;
}