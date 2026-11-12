#include "frame/frame_int.h"
#include <limits.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

// Acquire timeline, release syncobj, and the shutdown release poll.

bool wait_acquire_point(VulkanContext *vk, uint64_t point) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE ||
        frame_state.acquire == VK_NULL_HANDLE || point == 0) {
        return true;
    }
    VkSemaphoreWaitInfo wait_info = {
        .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .pNext          = nullptr,
        .flags          = 0,
        .semaphoreCount = 1,
        .pSemaphores    = &frame_state.acquire,
        .pValues        = &point,
    };
    return vkWaitSemaphores(vk->device, &wait_info, 3000000000ull) == VK_SUCCESS;
}

bool acquire_point_retired(VulkanContext *vk, uint64_t point) {
    if (point == 0 || frame_state.acquire == VK_NULL_HANDLE ||
        vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return true;
    }
    // Timeout 0 does not block. It retires the submit that signaled this
    // point so that image's command buffer can be recorded again.
    VkSemaphoreWaitInfo wait_info = {
        .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores    = &frame_state.acquire,
        .pValues        = &point,
    };
    return vkWaitSemaphores(vk->device, &wait_info, 0) == VK_SUCCESS;
}

static bool dmabuf_fences_idle(int dmabuf_fd, int timeout_ms) {
    if (dmabuf_fd < 0) {
        return true;
    }
    struct dma_buf_export_sync_file exp = {
        .flags = DMA_BUF_SYNC_RW,
        .fd    = -1,
    };
    if (ioctl(dmabuf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exp) == 0) {
        if (exp.fd < 0) {
            return true;
        }
        struct pollfd pfd = {
            .fd      = exp.fd,
            .events  = POLLIN,
            .revents = 0,
        };
        int rc;
        do {
            rc = poll(&pfd, 1, timeout_ms);
        } while (rc < 0 && errno == EINTR && timeout_ms != 0);
        close(exp.fd);
        return rc > 0 && (pfd.revents & (POLLIN | POLLERR | POLLNVAL)) == POLLIN;
    }
    struct pollfd fallback_pfd = {
        .fd      = dmabuf_fd,
        .events  = POLLOUT,
        .revents = 0,
    };
    int rc;
    do {
        rc = poll(&fallback_pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR && timeout_ms != 0);
    return rc > 0 && (fallback_pfd.revents & POLLOUT) != 0;
}

bool poll_slot_released(const WaylandContext *wl, const FrameSlot *frame) {
    if (frame == nullptr) {
        return true;
    }
    if (frame_state.implicit_dmabuf_sync) {
        if (frame->wl_buffer_id != 0u && !wayland_buffer_is_released(wl, frame->wl_buffer_id)) {
            return false;
        }
        return dmabuf_fences_idle(frame->image.dmabuf_fd, 0);
    }
    if (frame_state.drm_fd < 0 || frame->release_handle == 0 || frame->release_point == 0) {
        return false;
    }
    uint32_t handle = frame->release_handle;
    uint64_t point  = frame->release_point;
    struct drm_syncobj_timeline_wait wait_arg = {
        .handles       = (uintptr_t)&handle,
        .points        = (uintptr_t)&point,
        .timeout_nsec  = 0,
        .count_handles = 1u,
        .flags         = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
    };
    return ioctl(frame_state.drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait_arg) == 0;
}

int open_drm_render_node(const VulkanContext *vk) {
    if (vk == nullptr || !vk->device_info.has_drm_render_node) {
        return -1;
    }
    char render_path[64];
    int path_n = snprintf(render_path, sizeof render_path, "/dev/dri/renderD%lld",
                          (long long)vk->device_info.drm_render_minor);
    if (path_n < 0 || (size_t)path_n >= sizeof render_path) {
        fprintf(stderr, "[Engine Error] DRM render node path is too long\n");
        return -1;
    }
    int fd = open(render_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "[Engine Error] open %s: %s\n", render_path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode) ||
        (int64_t)major(st.st_rdev) != vk->device_info.drm_render_major ||
        (int64_t)minor(st.st_rdev) != vk->device_info.drm_render_minor) {
        fprintf(stderr, "[Engine Error] %s does not match DRM render node %lld:%lld\n",
                render_path,
                (long long)vk->device_info.drm_render_major,
                (long long)vk->device_info.drm_render_minor);
        close(fd);
        return -1;
    }
    struct drm_syncobj_create probe = {0};
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &probe) != 0) {
        fprintf(stderr, "[Engine Error] DRM_IOCTL_SYNCOBJ_CREATE on %s: %s\n",
                render_path, strerror(errno));
        close(fd);
        return -1;
    }
    uint64_t probe_point = 1u;
    struct drm_syncobj_timeline_array probe_sig = {
        .handles       = (uintptr_t)&probe.handle,
        .points        = (uintptr_t)&probe_point,
        .count_handles = 1u,
        .flags         = 0,
    };
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &probe_sig) != 0) {
        fprintf(stderr, "[Engine Error] DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL on %s: %s\n",
                render_path, strerror(errno));
        drm_syncobj_destroy(fd, &probe.handle);
        close(fd);
        return -1;
    }
    drm_syncobj_destroy(fd, &probe.handle);
    if (frame_state.drm_fd >= 0) {
        close(frame_state.drm_fd);
        frame_state.drm_fd = -1;
    }
    frame_state.drm_fd = fd;
    frame_state.timeline_fd_to_handle_unsupported = false;
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

static int create_export_binary_semaphore(VulkanContext *vk, VkSemaphore *out_sync) {
    VkSemaphoreTypeCreateInfo binary_type = {
        .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext         = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_BINARY,
        .initialValue  = 0,
    };
    VkExportSemaphoreCreateInfo export_info = {
        .sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .pNext       = &binary_type,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkSemaphoreCreateInfo sync_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &export_info,
        .flags = 0,
    };
    VkResult res = vkCreateSemaphore(vk->device, &sync_info, nullptr, out_sync);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Engine Error] vkCreateSemaphore (acquire sync_file) failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        *out_sync = VK_NULL_HANDLE;
        return -1;
    }
    return 0;
}

int create_acquire_semaphore(VulkanContext *vk, VkSemaphore *out_timeline, VkSemaphore *out_sync) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE ||
        out_timeline == nullptr || out_sync == nullptr) {
        return -1;
    }
    *out_timeline = VK_NULL_HANDLE;
    *out_sync     = VK_NULL_HANDLE;

    VkSemaphoreTypeCreateInfo timeline_type = {
        .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext         = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue  = 0,
    };
    VkSemaphoreCreateInfo timeline_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &timeline_type,
        .flags = 0,
    };
    VkResult res = vkCreateSemaphore(vk->device, &timeline_info, nullptr, out_timeline);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Engine Error] vkCreateSemaphore (acquire timeline) failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }

    if (create_export_binary_semaphore(vk, out_sync) != 0) {
        vkDestroySemaphore(vk->device, *out_timeline, nullptr);
        *out_timeline = VK_NULL_HANDLE;
        return -1;
    }
    return 0;
}

int create_acquire_timeline(WaylandContext *wl) {
    if (wl == nullptr || frame_state.drm_fd < 0) {
        return -1;
    }
    struct drm_syncobj_create create_arg = {0};
    if (ioctl(frame_state.drm_fd, DRM_IOCTL_SYNCOBJ_CREATE, &create_arg) != 0) {
        fprintf(stderr, "[Engine Error] DRM_IOCTL_SYNCOBJ_CREATE (acquire): %s\n", strerror(errno));
        return -1;
    }
    frame_state.acquire_handle = create_arg.handle;

    int acquire_fd = drm_syncobj_to_fd(frame_state.drm_fd, frame_state.acquire_handle);
    if (acquire_fd < 0) {
        drm_syncobj_destroy(frame_state.drm_fd, &frame_state.acquire_handle);
        return -1;
    }

    int imported = wayland_import_syncobj_timeline(wl, acquire_fd, &wl->acquire_timeline_id);
    close(acquire_fd);
    if (imported < 0 || wayland_bind_explicit_sync(wl) < 0) {
        drm_syncobj_destroy(frame_state.drm_fd, &frame_state.acquire_handle);
        return -1;
    }
    return 0;
}

static int signal_timeline_point(int drm_fd, uint32_t handle, uint64_t point) {
    struct drm_syncobj_timeline_array sig = {
        .handles       = (uintptr_t)&handle,
        .points        = (uintptr_t)&point,
        .count_handles = 1u,
        .flags         = 0,
    };
    if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &sig) != 0) {
        fprintf(stderr, "[Engine Error] DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL (point %llu): %s\n",
                (unsigned long long)point, strerror(errno));
        return -1;
    }
    return 0;
}

static int import_sync_file_into_timeline(int drm_fd, uint32_t handle, uint64_t point, int sync_fd) {
    if (!frame_state.timeline_fd_to_handle_unsupported) {
        struct drm_syncobj_handle imp = {
            .handle = handle,
            .flags  = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE |
                      DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE,
            .fd     = sync_fd,
            .pad    = 0,
            .point  = point,
        };
        if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &imp) == 0) {
            return 0;
        }
        if (errno != EINVAL && errno != EOPNOTSUPP) {
            fprintf(stderr, "[Engine Error] DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE (timeline sync_file): %s\n",
                    strerror(errno));
            return -1;
        }
        frame_state.timeline_fd_to_handle_unsupported = true;
    }

    // Two-step fallback for kernels without DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE:
    // import the sync_file into a separate binary syncobj (point 0) so we never
    // overwrite point 0 of the timeline's dma_fence_chain, then transfer to dst_point.
    struct drm_syncobj_create tmp_create = {0};
    if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_CREATE, &tmp_create) != 0) {
        fprintf(stderr, "[Engine Error] DRM_IOCTL_SYNCOBJ_CREATE (transfer temp): %s\n",
                strerror(errno));
        return -1;
    }
    struct drm_syncobj_handle tmp_imp = {
        .handle = tmp_create.handle,
        .flags  = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE,
        .fd     = sync_fd,
        .pad    = 0,
        .point  = 0,
    };
    if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &tmp_imp) != 0) {
        fprintf(stderr, "[Engine Error] DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE (sync_file): %s\n",
                strerror(errno));
        drm_syncobj_destroy(drm_fd, &tmp_create.handle);
        return -1;
    }
    struct drm_syncobj_transfer xfer = {
        .src_handle = tmp_create.handle,
        .dst_handle = handle,
        .src_point  = 0,
        .dst_point  = point,
        .flags      = 0,
        .pad        = 0,
    };
    int rc = 0;
    if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &xfer) != 0) {
        fprintf(stderr, "[Engine Error] DRM_IOCTL_SYNCOBJ_TRANSFER (point %llu): %s\n",
                (unsigned long long)point, strerror(errno));
        rc = -1;
    }
    drm_syncobj_destroy(drm_fd, &tmp_create.handle);
    return rc;
}

int publish_acquire_point_from(VulkanContext *vk, VkSemaphore *io_sync_sem,
                               uint64_t point, int dmabuf_fd) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE ||
        io_sync_sem == nullptr || *io_sync_sem == VK_NULL_HANDLE || point == 0) {
        return -1;
    }
    if (!frame_state.implicit_dmabuf_sync &&
        (frame_state.drm_fd < 0 || frame_state.acquire_handle == 0)) {
        return -1;
    }
    if (frame_state.implicit_dmabuf_sync && dmabuf_fd < 0) {
        return -1;
    }
    VkSemaphoreGetFdInfoKHR info = {
        .sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .pNext      = nullptr,
        .semaphore  = *io_sync_sem,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    int sync_fd = -1;
    VkResult res = vk->dispatch.vkGetSemaphoreFdKHR(vk->device, &info, &sync_fd);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Engine Error] vkGetSemaphoreFdKHR (SYNC_FD) failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        // The binary semaphore was left signaled because SYNC_FD export failed.
        // Wait for the timeline submit to finish, recreate a clean unsignaled
        // binary semaphore for future frames, and signal the syncobj point directly.
        bool retired = wait_acquire_point(vk, point);
        VkSemaphore fresh = VK_NULL_HANDLE;
        if (retired && create_export_binary_semaphore(vk, &fresh) == 0) {
            vkDestroySemaphore(vk->device, *io_sync_sem, nullptr);
            *io_sync_sem = fresh;
            if (frame_state.implicit_dmabuf_sync) {
                return 0;
            }
            return signal_timeline_point(frame_state.drm_fd, frame_state.acquire_handle, point);
        }
        return -1;
    }
    if (frame_state.implicit_dmabuf_sync) {
        if (sync_fd < 0) {
            return 0;
        }
        struct dma_buf_import_sync_file imp = {
            .flags = DMA_BUF_SYNC_WRITE,
            .fd    = sync_fd,
        };
        int imp_rc = 0;
        if (ioctl(dmabuf_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &imp) != 0) {
            fprintf(stderr, "[Engine Error] DMA_BUF_IOCTL_IMPORT_SYNC_FILE: %s\n", strerror(errno));
            imp_rc = -1;
        }
        close(sync_fd);
        return imp_rc;
    }
    // Vulkan allows SYNC_FD export to return VK_SUCCESS with fd == -1 when the
    // semaphore is already signaled at export time (while still resetting the
    // binary semaphore payload). Materialize the completed timeline point directly.
    if (sync_fd < 0) {
        return signal_timeline_point(frame_state.drm_fd, frame_state.acquire_handle, point);
    }
    int rc = import_sync_file_into_timeline(
        frame_state.drm_fd, frame_state.acquire_handle, point, sync_fd);
    close(sync_fd);
    return rc;
}

int arm_slot_read(RingContext *ring, uint32_t slot, bool fixed, int raw_fd,
                         uint32_t buf_index, uint64_t user_data) {
    return ring_arm_read_fixed(ring, slot, fixed, raw_fd, buf_index, user_data);
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
    if (frame_state.release_fd >= 0) {
        return 0;
    }
    int fd = eventfd(0, EFD_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "[Engine Error] eventfd: %s\n", strerror(errno));
        return -1;
    }
    frame_state.release_fd = fd;
    frame_state.release_fixed = false;
    if (ring != nullptr && ring->is_initialized &&
        ring_update_file(ring, ENG_RELEASE_FD_SLOT, fd) == 0) {
        frame_state.release_fixed = true;
    }
    return 0;
}

int attach_release_eventfd(const FrameSlot *frame) {
    if (frame == nullptr || frame->release_handle == 0 || frame->release_point == 0 ||
        frame_state.drm_fd < 0 || frame_state.release_fd < 0) {
        return -EINVAL;
    }
    struct drm_syncobj_eventfd release_event = {
        .handle = frame->release_handle,
        .flags  = 0,
        .point  = frame->release_point,
        .fd     = frame_state.release_fd,
        .pad    = 0,
    };
    if (ioctl(frame_state.drm_fd, DRM_IOCTL_SYNCOBJ_EVENTFD, &release_event) != 0) {
        fprintf(stderr, "[Engine Error] DRM_IOCTL_SYNCOBJ_EVENTFD: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int arm_release_read(RingContext *ring) {
    return arm_slot_read(ring, ENG_RELEASE_FD_SLOT, frame_state.release_fixed, frame_state.release_fd,
                         ENG_FBUF_RELEASE, ENG_UD_GPU_RELEASE);
}

int wait_gpu_release(RingContext *ring, WaylandContext *wl) {
    uint64_t deadline = ring_mono_ns() + 3000000000ull;
    for (;;) {
        struct io_uring_cqe cqe;
        while (ring_peek(ring, &cqe)) {
            if (cqe.user_data == ENG_UD_GPU_RELEASE) {
                if (cqe.res != (int)ENG_FBUF_RELEASE_LEN) {
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

int wait_own_release(const FrameSlot *frame, uint64_t timeout_ns) {
    if (frame == nullptr) {
        return 0;
    }
    if (frame_state.implicit_dmabuf_sync) {
        uint64_t deadline = ring_mono_ns() + timeout_ns;
        WaylandContext *wl = wayland_get_context();
        RingContext *ring  = ring_get_context();
        while (frame->wl_buffer_id != 0u &&
               wl != nullptr && wl->is_initialized && !wl->failed &&
               !wayland_buffer_is_released(wl, frame->wl_buffer_id)) {
            struct io_uring_cqe cqe;
            bool progressed = false;
            while (ring != nullptr && ring->is_initialized && ring_peek(ring, &cqe)) {
                progressed = true;
                wayland_on_cqe(wl, &cqe);
                if (wl->failed) {
                    break;
                }
            }
            if (wayland_buffer_is_released(wl, frame->wl_buffer_id)) {
                break;
            }
            uint64_t now = ring_mono_ns();
            if (now >= deadline || ring == nullptr || !ring->is_initialized || wl->failed) {
                return -ETIMEDOUT;
            }
            if (!progressed) {
                (void)wayland_ensure_recv(wl);
                uint64_t rem_ms = (deadline - now + 999999ull) / 1000000ull;
                int wait_ms = rem_ms > 100ull ? 100 : (int)rem_ms;
                unsigned n = ring_submit(ring);
                int rc = ring_enter(ring, n, 1u, wait_ms);
                if (rc < 0 && rc != -ETIME && rc != -EINTR) {
                    return rc;
                }
            }
        }
        uint64_t now = ring_mono_ns();
        int rem_ms = 0;
        if (now < deadline) {
            uint64_t ms = (deadline - now + 999999ull) / 1000000ull;
            rem_ms = (ms > (uint64_t)INT_MAX) ? INT_MAX : (int)ms;
        }
        return dmabuf_fences_idle(frame->image.dmabuf_fd, rem_ms) ? 0 : -ETIMEDOUT;
    }
    if (frame->release_handle == 0 ||
        frame->release_point == 0 || frame_state.drm_fd < 0) {
        return 0;
    }
    uint64_t now_ns = ring_mono_ns();
    uint64_t abs_ns = (timeout_ns > (uint64_t)INT64_MAX - now_ns)
        ? (uint64_t)INT64_MAX
        : (now_ns + timeout_ns);
    for (;;) {
        uint32_t handle = frame->release_handle;
        uint64_t point  = frame->release_point;
        struct drm_syncobj_timeline_wait wait_arg = {
            .handles       = (uintptr_t)&handle,
            .points        = (uintptr_t)&point,
            .timeout_nsec  = (int64_t)abs_ns,
            .count_handles = 1u,
            .flags         = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
        };
        if (ioctl(frame_state.drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait_arg) == 0) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        return -errno;
    }
}

int cancel_release_poll(RingContext *ring, WaylandContext *wl, bool *released) {
    *released = false;
    if (prep_cancel_fd(ring, frame_state.release_fd, ENG_RELEASE_FD_SLOT, frame_state.release_fixed,
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
                if (cqe.res == (int)ENG_FBUF_RELEASE_LEN) {
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
    if (frame_state.stuck_release_fd >= 0 || frame_state.release_fd < 0) {
        return;
    }
    if (frame_state.release_fixed && ring != nullptr && ring->is_initialized) {
        (void)ring_update_file(ring, ENG_RELEASE_FD_SLOT, -1);
        frame_state.release_fixed = false;
    }
    close(frame_state.release_fd);
    frame_state.release_fd = -1;
}
