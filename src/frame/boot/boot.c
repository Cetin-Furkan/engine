#include "frame/frame_int.h"
#include "input/page.h"

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

    frame_state.front = 0;
    frame_state.drm_fd = -1;
    frame_state.acquire = VK_NULL_HANDLE;
    blank_frame(shown());
    blank_frame(spare());
    frame_state.spare_live = false;
    frame_state.turn_phase = k_turn_arm;
    frame_state.turn_swap_pending = false;
    frame_state.pending_release = 0;
    frame_state.resize_pending = false;
    frame_state.same_repaint = false;
    frame_state.chrome_dirty = false;
    frame_state.release_fd = -1;
    frame_state.release_fixed = false;
    frame_state.inotify_fd = -1;
    frame_state.inotify_wd = -1;
    frame_state.inotify_fixed = false;
    frame_state.inotify_stop = false;
    blank_frame(&frame_state.bar_slot[0]);
    blank_frame(&frame_state.bar_slot[1]);
    frame_state.bar_front = 0;
    frame_state.bar_live = false;
    frame_state.bar_dirty = false;
    frame_state.bar_swap_on_send = false;
    frame_state.scene_dirty = false;
    frame_state.sim_latched = false;
    frame_state.sim_hot = false;
    frame_state.sim_timer_on = false;
    frame_state.sim_mshot_armed = false;
    frame_state.sim_fixed = false;
    frame_state.sim_fd = -1;
    frame_state.mono0_ns = 0;
    frame_state.pending_heat = 0;
    frame_state.pending_sim_stamp = 0;
    blank_frame(&frame_state.resize_fresh);
    frame_state.resize_release = 0;
    frame_state.resize_serial = 0;
    frame_state.resize_old_buffer = 0;
    frame_state.resize_old_w = 0;
    frame_state.resize_old_h = 0;
    frame_state.resize_tranche[0] = '\0';
    engine_app()->reset();
    bda_transient_reset();
    frame_clock_reset(&frame_state.clock);
    frame_state.timer_armed = false;
    frame_state.last_stamp_mono_ns = 0;
    frame_state.stall_reported = false;
    frame_state.sync_point = 0;
    frame_state.orphan_mask = 0;
    frame_state.orphan_count = 0;
    for (uint32_t i = 0; i < k_orphan_cap; ++i) {
        frame_state.orphans[i] = (FrameSlot){ .image = { .dmabuf_fd = -1 }, .release_eventfd = -1 };
    }
    frame_state.stuck_release_fd = -1;
    frame_state.presented = true;

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

    uint32_t content_w = 0;
    uint32_t content_h = 0;
    frame_apply_layout(wl, width, height, &content_w, &content_h);
    if (bar_open_surface(wl) != 0) {
        (void)frame_stop();
        return 1;
    }

    const char *tranche_kind = "general";
    if (alloc_content_image(vk, wl, content_w, content_h, &shown()->image, &tranche_kind) != 0) {
        (void)frame_stop();
        return 1;
    }

    if (create_acquire_semaphore(vk, &frame_state.acquire) < 0) {
        (void)frame_stop();
        return 1;
    }
    gpu_timer_init(vk);
    if (input_gpu_init(vk) != 0) {
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
    if (record_and_submit_clear(vk, shown(), frame_state.acquire, 1u, false, 0u) < 0) {
        (void)frame_stop();
        return 1;
    }
    frame_state.sync_point = 1u;

    char render_path[64];
    int path_n = snprintf(render_path, sizeof render_path, "/dev/dri/renderD%lld",
                          (long long)vk->device_info.drm_render_minor);
    if (path_n < 0 || (size_t)path_n >= sizeof render_path) {
        fprintf(stderr, "[Engine Error] DRM render node path is too long\n");
        (void)frame_stop();
        return 1;
    }
    frame_state.drm_fd = open(render_path, O_RDWR | O_CLOEXEC);
    if (frame_state.drm_fd < 0) {
        fprintf(stderr, "[Engine Error] open %s: %s\n", render_path, strerror(errno));
        (void)frame_stop();
        return 1;
    }

    int acquire_vk_fd = export_semaphore_fd(vk, frame_state.acquire);
    if (acquire_vk_fd < 0) {
        (void)frame_stop();
        return 1;
    }
    uint32_t acquire_handle = 0;
    int imported = drm_syncobj_from_fd(frame_state.drm_fd, acquire_vk_fd, &acquire_handle);
    close(acquire_vk_fd);
    if (imported < 0) {
        (void)frame_stop();
        return 1;
    }
    int acquire_fd = drm_syncobj_to_fd(frame_state.drm_fd, acquire_handle);
    drm_syncobj_destroy(frame_state.drm_fd, &acquire_handle);
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

    if (!spec->resize) {
        wl->lock_toplevel_size = true;
        wl->lock_width = (int32_t)frame_state.window_w;
        wl->lock_height = (int32_t)frame_state.window_h;
    }

    if (bar_record_first(wl, vk) != 0) {
        (void)frame_stop();
        return 1;
    }

    uint32_t present_serial = (wl->configure_serial != 0u) ? wl->configure_serial : serial;
    if (wayland_present_single_frame_and_wait(
            wl,
            shown()->wl_buffer_id,
            shown()->image.width,
            shown()->image.height,
            present_serial,
            wl->acquire_timeline_id,
            shown()->acquire_point,
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
    bar_note_presented();
    printf("[Engine] Presented 1 visible frame: window=%ux%u scene=%ux%u (serial=%u), fourcc=0x%08x (ARGB8888), modifier=0x%llx (%s tranche), stride=%zu, offset=%zu, fd=%d, explicit-sync acquire=%llu release=%llu timeline=%u\n",
           frame_state.window_w,
           frame_state.window_h,
           shown()->image.width,
           shown()->image.height,
           present_serial,
           ENG_DRM_FORMAT_ARGB8888,
           (unsigned long long)shown()->image.drm_format_modifier,
           tranche_kind,
           (size_t)shown()->image.row_pitch,
           (size_t)shown()->image.offset,
           shown()->image.dmabuf_fd,
           (unsigned long long)shown()->acquire_point,
           (unsigned long long)shown()->release_point,
           shown()->release_timeline_id);
    printf("hello init from engine\n");
    return 0;
}

