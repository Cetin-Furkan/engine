#include "frame/internal.h"
#include "input/page.h"

// Reverse of frame_boot. Unmap, then the release point, then the GPU.

int frame_stop(void) {
    if (!frame_state.presented) {
        return 0;
    }
    WaylandContext *wl   = wayland_get_context();
    VulkanContext  *vk   = vulkan_get_context();
    RingContext    *ring = ring_get_context();
    int             release_rc = 0;

    if (wl != nullptr) {
        pointer_shutdown(wl);
    }
    if (frame_state.resize_pending) {
        drop_resize_attempt(wl, vk);
    }
    chrome_set_buffer_size(0, 0);

    // Unmap first. The GPU idle and the release wait can take the rest
    // of the shutdown, and the window should already be gone.
    if (wl != nullptr && wl->is_initialized && !wl->failed &&
        wayland_detach_buffer(wl) < 0) {
        fprintf(stderr, "[Engine Error] Buffer detach failed before GPU release wait\n");
    }

    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        (void)vkDeviceWaitIdle(vk->device);
        retire_spare(wl, vk);
    }

    if (frame_state.stuck_release_fd >= 0) {
        fprintf(stderr, "[Engine Error] GPU release read could not be removed\n");
        release_rc = 1;
    } else if (shown()->release_pending && frame_state.implicit_dmabuf_sync) {
        bool released_now = poll_slot_released(wl, shown());
        if (!released_now) {
            (void)wait_own_release(shown(), 3000000000ull);
            released_now = poll_slot_released(wl, shown());
        }
        if (released_now) {
            printf("[Engine] GPU release point signalled (dma-buf implicit sync + wl_buffer.release)\n");
        } else {
            release_rc = 1;
        }
    } else if (shown()->release_pending) {
        bool released_now = poll_slot_released(wl, shown());
        bool armed = false;
        if (!released_now &&
            ensure_release_fd(ring) == 0 &&
            attach_release_eventfd(shown()) == 0 &&
            ring != nullptr && ring->is_initialized &&
            wl != nullptr && wl->is_initialized &&
            arm_release_read(ring) == 0) {
            armed = true;
        }
        if (released_now || (armed && wait_gpu_release(ring, wl) == 0)) {
            printf("[Engine] GPU release point signalled (io_uring fixed read)\n");
        } else {
            release_rc = 1;
            if (!armed && frame_state.release_fd < 0) {
                fprintf(stderr, "[Engine Error] GPU release eventfd is closed\n");
            }
            if (armed) {
                bool late = false;
                if (cancel_release_poll(ring, wl, &late) != 0) {
                    fprintf(stderr, "[Engine Error] GPU release read is still armed\n");
                    frame_state.stuck_release_fd = frame_state.release_fd;
                    frame_state.release_fd = -1;
                    frame_state.release_fixed = false;
                } else if (late || poll_slot_released(wl, shown())) {
                    printf("[Engine] GPU release point signalled (io_uring fixed read)\n");
                    release_rc = 0;
                }
            }
        }
    }

    release_persistent_fd(ring);

    sweep_ready_orphans(wl, vk, true);

    bool release_held = shown()->release_pending &&
        !poll_slot_released(wl, shown());
    bool acquire_held = false;
    if (vk != nullptr && vk->device != VK_NULL_HANDLE &&
        !wait_acquire_point(vk, shown()->acquire_point)) {
        fprintf(stderr,
                "[Engine Error] Image left alive. Acquire point %llu is still pending.\n",
                (unsigned long long)shown()->acquire_point);
        acquire_held = true;
    }
    uint32_t left_alive = frame_state.orphan_count + (release_held ? 1u : 0u);
    if (left_alive > 0u) {
        fprintf(stderr, "[Engine Error] %u compositor-held image(s) timed out on shutdown; reclaiming GPU handles\n",
                left_alive);
    }
    if (frame_state.orphan_mask != 0ull) {
        uint64_t rem = frame_state.orphan_mask;
        while (rem != 0ull) {
            uint32_t idx = (uint32_t)__builtin_ctzll(rem);
            rem &= rem - 1ull;
            FrameSlot *orb = &frame_state.orphans[idx];
            if (orb->release_eventfd >= 0) {
                close(orb->release_eventfd);
                orb->release_eventfd = -1;
            }
            (void)wait_acquire_point(vk, orb->acquire_point);
            destroy_frame_gpu(vk, orb);
            *orb = (FrameSlot){ .image = { .dmabuf_fd = -1 }, .release_eventfd = -1 };
        }
        frame_state.orphan_mask  = 0ull;
        frame_state.orphan_count = 0u;
    }
    if (frame_state.orphans != nullptr &&
        frame_state.orphans != frame_state.orphans_inline) {
        free(frame_state.orphans);
    }
    frame_state.orphans    = frame_state.orphans_inline;
    frame_state.orphan_cap = k_orphan_init_cap;

    bar_shutdown(wl, vk);
    if (wl != nullptr && wl->is_initialized) {
        wayland_destroy_frame_objects(wl);
        shown()->wl_buffer_id = 0;
    }

    if (!release_held) {
        drm_syncobj_destroy(frame_state.drm_fd, &shown()->release_handle);
    }
    if (!acquire_held) {
        drm_syncobj_destroy(frame_state.drm_fd, &frame_state.acquire_handle);
    }
    if (frame_state.drm_fd >= 0) {
        close(frame_state.drm_fd);
        frame_state.drm_fd = -1;
    }

    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        if (acquire_held) {
            (void)vkDeviceWaitIdle(vk->device);
        }
        input_page_harvest();
        uint32_t seen = input_page_seen();
        if (seen != 0u) {
            printf("[Pointer] GPU read input page seq %u\n", seen);
        }
        gpu_timer_shutdown(vk);
        input_gpu_shutdown(vk);
        if (frame_state.prime_acquire_sync != VK_NULL_HANDLE) {
            vkDestroySemaphore(vk->device, frame_state.prime_acquire_sync, nullptr);
            frame_state.prime_acquire_sync = VK_NULL_HANDLE;
        }
        if (frame_state.prime_ready_sem != VK_NULL_HANDLE) {
            vkDestroySemaphore(vk->device, frame_state.prime_ready_sem, nullptr);
            frame_state.prime_ready_sem = VK_NULL_HANDLE;
        }
        if (frame_state.acquire_sync != VK_NULL_HANDLE) {
            vkDestroySemaphore(vk->device, frame_state.acquire_sync, nullptr);
            frame_state.acquire_sync = VK_NULL_HANDLE;
        }
        if (frame_state.acquire != VK_NULL_HANDLE) {
            vkDestroySemaphore(vk->device, frame_state.acquire, nullptr);
            frame_state.acquire = VK_NULL_HANDLE;
        }
        destroy_prime_color(vk, shown());
        if (shown()->cmd_buf != VK_NULL_HANDLE && vk->cmd_pool != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &shown()->cmd_buf);
            shown()->cmd_buf = VK_NULL_HANDLE;
        }
        if (shown()->transfer_cmd_buf != VK_NULL_HANDLE && vk->transfer_cmd_pool != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(vk->device, vk->transfer_cmd_pool, 1, &shown()->transfer_cmd_buf);
            shown()->transfer_cmd_buf = VK_NULL_HANDLE;
        }
        if (shown()->image_view != VK_NULL_HANDLE) {
            vkDestroyImageView(vk->device, shown()->image_view, nullptr);
            shown()->image_view = VK_NULL_HANDLE;
        }
        vulkan_destroy_exportable_image(vk, &shown()->image);
        if (engine_app() != nullptr && engine_app()->destroy_targets != nullptr) {
            engine_app()->destroy_targets(vk, &shown()->targets);
        } else {
            shown()->targets = (EngineTargets){0};
        }
        chrome_gpu_shutdown(vk);
        if (engine_app() != nullptr && engine_app()->shutdown != nullptr) {
            engine_app()->shutdown(vk);
        }
    }

    if (ring != nullptr) {
        ring_set_dispatcher(ring, nullptr, nullptr);
    }
    frame_state.presented = false;
    shown()->release_pending = false;
    printf("goodbye from engine\n");
    return release_rc;
}
