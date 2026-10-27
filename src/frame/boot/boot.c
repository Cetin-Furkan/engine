#include "frame/frame_int.h"

// Device pipelines, the world file, the first present, and the reverse shutdown.

static void forward_init_cqe(const struct io_uring_cqe *cqe, void *user);

static void forward_init_cqe(const struct io_uring_cqe *cqe, void *user) {
    if (user != nullptr) {
        wayland_on_cqe(user, cqe);
    }
}

int frame_boot(void) {
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
    s_same_repaint = false;
    s_chrome_dirty = false;
    s_release_fd = -1;
    s_release_fixed = false;
    s_release_counter = 0;
    s_inotify_fd = -1;
    s_inotify_wd = -1;
    s_inotify_fixed = false;
    s_inotify_stop = false;
    blank_frame(&s_resize_fresh);
    s_resize_release = 0;
    s_resize_serial = 0;
    s_resize_old_buffer = 0;
    s_resize_old_w = 0;
    s_resize_old_h = 0;
    s_resize_tranche[0] = '\0';
    engine_app()->reset();
    frame_clock_reset(&s_clock);
    s_timer_armed = false;
    s_last_stamp_mono_ns = 0;
    s_stall_reported = false;
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
        (void)frame_stop();
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
        (void)frame_stop();
        return 1;
    }

    const char *tranche_kind = "general";
    if (alloc_content_image(vk, wl, width, height, &shown()->image, &tranche_kind) != 0) {
        (void)frame_stop();
        return 1;
    }

    if (create_acquire_semaphore(vk, &s_acquire) < 0) {
        (void)frame_stop();
        return 1;
    }
    // The layout has to exist before the app binds the scene sample count.
    if (chrome_gpu_init(vk) != 0 ||
        engine_app()->init(vk, forward_init_cqe, wl) != 0) {
        (void)frame_stop();
        return 1;
    }
    if (engine_app()->load() != 0) {
        (void)frame_stop();
        return 1;
    }
    if (record_and_submit_clear(vk, shown(), s_acquire, 1u, false, 0u) < 0) {
        (void)frame_stop();
        return 1;
    }
    s_sync_point = 1u;

    char render_path[64];
    int path_n = snprintf(render_path, sizeof render_path, "/dev/dri/renderD%lld",
                          (long long)vk->device_info.drm_render_minor);
    if (path_n < 0 || (size_t)path_n >= sizeof render_path) {
        fprintf(stderr, "[Engine Error] DRM render node path is too long\n");
        (void)frame_stop();
        return 1;
    }
    s_drm_fd = open(render_path, O_RDWR | O_CLOEXEC);
    if (s_drm_fd < 0) {
        fprintf(stderr, "[Engine Error] open %s: %s\n", render_path, strerror(errno));
        (void)frame_stop();
        return 1;
    }

    int acquire_vk_fd = export_semaphore_fd(vk, s_acquire);
    if (acquire_vk_fd < 0) {
        (void)frame_stop();
        return 1;
    }
    uint32_t acquire_handle = 0;
    int imported = drm_syncobj_from_fd(s_drm_fd, acquire_vk_fd, &acquire_handle);
    close(acquire_vk_fd);
    if (imported < 0) {
        (void)frame_stop();
        return 1;
    }
    int acquire_fd = drm_syncobj_to_fd(s_drm_fd, acquire_handle);
    drm_syncobj_destroy(s_drm_fd, &acquire_handle);
    if (acquire_fd < 0) {
        (void)frame_stop();
        return 1;
    }

    if (create_release_timeline(vk, wl, shown()) != 0) {
        close(acquire_fd);
        (void)frame_stop();
        return 1;
    }
    wl->release_timeline_id = shown()->release_timeline_id;

    int acquire_import = wayland_import_syncobj_timeline(wl, acquire_fd, &wl->acquire_timeline_id);
    close(acquire_fd);
    if (acquire_import < 0 || wayland_bind_explicit_sync(wl) < 0) {
        (void)frame_stop();
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
        (void)frame_stop();
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
        (void)frame_stop();
        return 1;
    }

    if (pointer_flush(wl) < 0) {
        fprintf(stderr, "[Engine Error] Cursor was not set: %s\n", wl->err);
        (void)frame_stop();
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

int frame_stop(void) {
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
            printf("[Engine] GPU release point signalled (io_uring poll)\n");
        } else {
            release_rc = 1;
            if (!armed && s_release_fd < 0) {
                fprintf(stderr, "[Engine Error] GPU release eventfd is closed\n");
            }
            if (armed) {
                bool late = false;
                if (cancel_release_poll(ring, wl, &late) != 0) {
                    fprintf(stderr, "[Engine Error] GPU release poll is still armed\n");
                    s_stuck_release_fd = s_release_fd;
                    s_release_fd = -1;
                    s_release_fixed = false;
                } else if (late || poll_release_semaphore(vk, shown()->release_semaphore, shown()->release_point)) {
                    printf("[Engine] GPU release point signalled (io_uring poll)\n");
                    release_rc = 0;
                }
            }
        }
    }

    release_persistent_fd(ring);

    sweep_ready_orphans(wl, vk, true);

    bool release_held = shown()->release_pending &&
        !poll_release_semaphore(vk, shown()->release_semaphore, shown()->release_point);
    bool acquire_held = false;
    if (vk != nullptr && vk->device != VK_NULL_HANDLE &&
        !wait_acquire_point(vk, shown()->acquire_point)) {
        fprintf(stderr,
                "[Engine Error] Image left alive. Acquire point %llu is still pending.\n",
                (unsigned long long)shown()->acquire_point);
        acquire_held = true;
    }
    uint32_t left_alive = s_orphan_count + (release_held ? 1u : 0u);
    if (left_alive > 0u) {
        fprintf(stderr, "[Engine Error] %u compositor-held image(s) were left alive\n", left_alive);
    }

    if (wl != nullptr && wl->is_initialized) {
        wayland_destroy_frame_objects(wl);
        shown()->wl_buffer_id = 0;
    }

    if (!release_held) {
        drm_syncobj_destroy(s_drm_fd, &shown()->release_handle);
    }
    if (s_drm_fd >= 0) {
        close(s_drm_fd);
        s_drm_fd = -1;
    }

    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        bool destroy_shown = !release_held && !acquire_held;
        if (destroy_shown && shown()->release_semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(vk->device, shown()->release_semaphore, nullptr);
            shown()->release_semaphore = VK_NULL_HANDLE;
        }
        if (!acquire_held && s_acquire != VK_NULL_HANDLE) {
            vkDestroySemaphore(vk->device, s_acquire, nullptr);
            s_acquire = VK_NULL_HANDLE;
        }
        if (destroy_shown) {
            if (shown()->cmd_buf != VK_NULL_HANDLE && vk->cmd_pool != VK_NULL_HANDLE) {
                vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &shown()->cmd_buf);
                shown()->cmd_buf = VK_NULL_HANDLE;
            }
            if (shown()->image_view != VK_NULL_HANDLE) {
                vkDestroyImageView(vk->device, shown()->image_view, nullptr);
                shown()->image_view = VK_NULL_HANDLE;
            }
            vulkan_destroy_exportable_image(vk, &shown()->image);
            engine_app()->destroy_targets(vk, &shown()->targets);
        }
        chrome_gpu_shutdown(vk);
        engine_app()->shutdown(vk);
    }

    s_presented = false;
    shown()->release_pending = false;
    printf("goodbye from engine\n");
    return release_rc;
}
