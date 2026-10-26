#include "frame/frame_int.h"

// Acquire timeline, release syncobj, and the shutdown release poll.

bool wait_acquire_point(VulkanContext *vk, uint64_t point) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE ||
        s_acquire == VK_NULL_HANDLE || point == 0) {
        return true;
    }
    VkSemaphoreWaitInfo wait_info = {
        .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .pNext          = nullptr,
        .flags          = 0,
        .semaphoreCount = 1,
        .pSemaphores    = &s_acquire,
        .pValues        = &point,
    };
    return vkWaitSemaphores(vk->device, &wait_info, 3000000000ull) == VK_SUCCESS;
}

bool acquire_point_retired(VulkanContext *vk, uint64_t point) {
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

bool poll_release_semaphore(VulkanContext *vk, VkSemaphore semaphore, uint64_t point) {
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

int export_semaphore_fd(VulkanContext *vk, VkSemaphore semaphore) {
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

int import_release_semaphore_fd(VulkanContext *vk, int syncobj_fd, VkSemaphore *out) {
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

int drm_syncobj_from_fd(int drm_fd, int syncobj_fd, uint32_t *out_handle) {
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

int drm_syncobj_to_fd(int drm_fd, uint32_t handle) {
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

void drm_syncobj_destroy(int drm_fd, uint32_t *handle) {
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

int create_acquire_semaphore(VulkanContext *vk, VkSemaphore *out) {
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

int arm_slot_read(RingContext *ring, uint32_t slot, bool fixed, int raw_fd,
                         void *buf, uint32_t len, uint64_t user_data) {
    if (ring == nullptr || !ring->is_initialized || buf == nullptr || len == 0 ||
        (!fixed && raw_fd < 0)) {
        return -EINVAL;
    }
    struct io_uring_sqe *sqe = ring_sqe(ring);
    if (sqe == nullptr) {
        return -ENOSPC;
    }
    sqe->opcode = IORING_OP_READ;
    sqe->flags  = IOSQE_ASYNC;
    sqe->fd     = fixed ? (int)slot : raw_fd;
    if (fixed) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }
    sqe->addr      = (uint64_t)(uintptr_t)buf;
    sqe->len       = len;
    sqe->off       = 0;
    sqe->user_data = user_data;
    return 0;
}

int prep_cancel_fd(RingContext *ring, int fd, uint32_t slot, bool fixed, uint64_t user_data) {
    if (ring == nullptr || !ring->is_initialized || (!fixed && fd < 0)) {
        return -EINVAL;
    }
    struct io_uring_sqe *sqe = ring_sqe(ring);
    if (sqe == nullptr) {
        return -ENOSPC;
    }
    sqe->opcode       = IORING_OP_ASYNC_CANCEL;
    sqe->fd           = fixed ? (int)slot : fd;
    sqe->cancel_flags = IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_FD;
    if (fixed) {
        sqe->cancel_flags |= IORING_ASYNC_CANCEL_FD_FIXED;
    }
    sqe->user_data = user_data;
    return 0;
}

int ensure_release_fd(RingContext *ring) {
    if (s_release_fd >= 0) {
        return 0;
    }
    int fd = eventfd(0, EFD_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "[Engine Error] eventfd: %s\n", strerror(errno));
        return -1;
    }
    s_release_fd = fd;
    s_release_fixed = false;
    if (ring != nullptr && ring->is_initialized &&
        ring_update_file(ring, ENG_RELEASE_FD_SLOT, fd) == 0) {
        s_release_fixed = true;
    }
    return 0;
}

int attach_release_eventfd(const FrameSlot *frame) {
    if (frame == nullptr || frame->release_handle == 0 || frame->release_point == 0 ||
        s_drm_fd < 0 || s_release_fd < 0) {
        return -EINVAL;
    }
    struct drm_syncobj_eventfd release_event = {
        .handle = frame->release_handle,
        .flags  = 0,
        .point  = frame->release_point,
        .fd     = s_release_fd,
        .pad    = 0,
    };
    if (ioctl(s_drm_fd, DRM_IOCTL_SYNCOBJ_EVENTFD, &release_event) != 0) {
        fprintf(stderr, "[Engine Error] DRM_IOCTL_SYNCOBJ_EVENTFD: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int arm_release_read(RingContext *ring) {
    return arm_slot_read(ring, ENG_RELEASE_FD_SLOT, s_release_fixed, s_release_fd,
                         &s_release_counter, (uint32_t)sizeof s_release_counter,
                         ENG_UD_GPU_RELEASE);
}

int wait_gpu_release(RingContext *ring, WaylandContext *wl) {
    uint64_t deadline = ring_mono_ns() + 3000000000ull;
    for (;;) {
        struct io_uring_cqe cqe;
        while (ring_peek(ring, &cqe)) {
            if (cqe.user_data == ENG_UD_GPU_RELEASE) {
                if (cqe.res != (int)sizeof s_release_counter) {
                    if (cqe.res < 0) {
                        fprintf(stderr, "[Engine Error] io_uring read on GPU release: %s\n",
                                strerror(-cqe.res));
                    } else {
                        fprintf(stderr, "[Engine Error] io_uring read on GPU release was short (%d)\n",
                                cqe.res);
                    }
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

VkResult wait_own_release(VulkanContext *vk, FrameSlot *frame, uint64_t timeout_ns) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || frame == nullptr ||
        frame->release_semaphore == VK_NULL_HANDLE || frame->release_point == 0) {
        return VK_SUCCESS;
    }
    VkSemaphoreWaitInfo wait_info = {
        .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .pNext          = nullptr,
        .flags          = 0,
        .semaphoreCount = 1,
        .pSemaphores    = &frame->release_semaphore,
        .pValues        = &frame->release_point,
    };
    return vkWaitSemaphores(vk->device, &wait_info, timeout_ns);
}

int cancel_release_poll(RingContext *ring, WaylandContext *wl, bool *released) {
    *released = false;
    if (prep_cancel_fd(ring, s_release_fd, ENG_RELEASE_FD_SLOT, s_release_fixed,
                       k_ud_release_cancel) != 0) {
        return -ENOSPC;
    }
    bool saw_read = false;
    bool saw_cancel = false;
    uint64_t deadline = ring_mono_ns() + 3000000000ull;
    for (;;) {
        struct io_uring_cqe cqe;
        while (ring_peek(ring, &cqe)) {
            if (cqe.user_data == ENG_UD_GPU_RELEASE) {
                saw_read = true;
                if (cqe.res == (int)sizeof s_release_counter) {
                    *released = true;
                }
            } else if (cqe.user_data == k_ud_release_cancel) {
                saw_cancel = true;
            } else {
                wayland_on_cqe(wl, &cqe);
            }
        }
        if (saw_cancel && saw_read) {
            return 0;
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

void release_persistent_fd(RingContext *ring) {
    if (s_stuck_release_fd >= 0 || s_release_fd < 0) {
        return;
    }
    if (s_release_fixed && ring != nullptr && ring->is_initialized) {
        (void)ring_update_file(ring, ENG_RELEASE_FD_SLOT, -1);
        s_release_fixed = false;
    }
    close(s_release_fd);
    s_release_fd = -1;
}
