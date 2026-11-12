#include "frame/frame_int.h"
#include "input/page.h"
#include "system/system.h"
#include "vulkan/device.h"
#include "vulkan/memory.h"

// Device pipelines, the world file, the first present, and the reverse shutdown.

static void forward_init_cqe(const struct io_uring_cqe *cqe, void *user);

static void forward_init_cqe(const struct io_uring_cqe *cqe, void *user) {
    if (user != nullptr) {
        wayland_on_cqe(user, cqe);
    }
}

int frame_boot(void) {
    const SystemCapabilities *sys_caps = system_get_capabilities();
    RingContext    *ring = ring_get_context();
    WaylandContext *wl   = wayland_get_context();
    VulkanContext  *vk   = vulkan_get_context();
    if (sys_caps == nullptr || !sys_caps->is_initialized ||
        ring == nullptr || !ring->is_initialized ||
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
        vk->dispatch.vkGetSemaphoreFdKHR == nullptr) {
        fprintf(stderr,
                "[Engine Error] DMA-BUF synchronization needs VK_KHR_external_semaphore_fd (SYNC_FD)\n");
        return 1;
    }

    if (chrome_load() != 0) {
        return 1;
    }
    const ChromeSpec *spec = chrome_spec();
    if (spec == nullptr || pointer_check_spec(wl) != 0) {
        return 1;
    }

    ring_set_dispatcher(ring, forward_init_cqe, wl);

    frame_state.front = 0;
    frame_state.drm_fd = -1;
    frame_state.acquire_handle = 0;
    frame_state.acquire = VK_NULL_HANDLE;
    frame_state.acquire_sync = VK_NULL_HANDLE;
    frame_state.prime_ready_sem = VK_NULL_HANDLE;
    frame_state.prime_acquire_sync = VK_NULL_HANDLE;
    frame_state.prime_ready_point = 0;
    frame_state.last_acquire_signal_point = 0;
    frame_state.last_acquire_signal_queue = VK_NULL_HANDLE;
    frame_state.timeline_fd_to_handle_unsupported = false;
    frame_state.implicit_dmabuf_sync = (wl->drm_syncobj_id == 0u || !vk->device_info.has_drm_render_node);
    frame_state.pending_damage_set = true;
    frame_state.pending_damage_full = true;
    frame_state.pending_damage = (FrameRect){0};
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
    frame_state.world_wd = -1;
    frame_state.world_dirty = false;
    frame_state.chrome_watch_base[0] = '\0';
    frame_state.world_watch_path[0] = '\0';
    frame_state.world_watch_dir[0] = '\0';
    frame_state.world_watch_base[0] = '\0';
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
    frame_state.sim_cancel_sent = false;
    frame_state.sim_gen = 0u;
    frame_state.sim_inflight_count = 0u;
    frame_state.sim_cancel_inflight = 0u;
    frame_state.sim_active_ud = 0ull;
    frame_state.timer_deadline_ns = 0ull;
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
    frame_state.loop_was_busy = false;
    frame_state.gpu_submits = 0ull;
    frame_state.turn_frames = 0u;
    if (engine_app() != nullptr && engine_app()->reset != nullptr) {
        engine_app()->reset();
    }
    bda_transient_reset();
    frame_clock_reset(&frame_state.clock);
    frame_state.timer_armed = false;
    frame_state.last_stamp_mono_ns = 0;
    frame_state.stall_reported = false;
    frame_state.sync_point = 0;
    if (frame_state.orphans != nullptr &&
        frame_state.orphans != frame_state.orphans_inline) {
        free(frame_state.orphans);
    }
    frame_state.orphans = frame_state.orphans_inline;
    frame_state.orphan_cap = k_orphan_init_cap;
    frame_state.orphan_mask = 0;
    frame_state.orphan_count = 0;
    memset(frame_state.orphan_gen, 0, sizeof(frame_state.orphan_gen));
    for (uint32_t i = 0; i < k_orphan_init_cap; ++i) {
        frame_state.orphans[i] = (FrameSlot){ .image = { .dmabuf_fd = -1 }, .release_eventfd = -1 };
    }
    frame_state.stuck_release_fd = -1;
    frame_state.presented = true;
    wl->uncapped_commits = frame_state.bench_uncapped;

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
    if ((wl->toplevel_states & (ENG_TOP_MAXIMIZED | ENG_TOP_FULLSCREEN | ENG_TOP_TILED_MASK)) == 0u) {
        wl->floating_width  = width;
        wl->floating_height = height;
    }

    uint32_t content_w = 0;
    uint32_t content_h = 0;
    frame_apply_layout(wl, width, height, &content_w, &content_h);

    const char *tranche_kind = "general";
    for (;;) {
        const bool has_render  = vk->device_info.has_drm_render_node;
        const bool has_primary = vk->device_info.has_drm_primary_node;
        const int64_t target_major = has_render
            ? vk->device_info.drm_render_major
            : vk->device_info.drm_primary_major;
        const int64_t target_minor = has_render
            ? vk->device_info.drm_render_minor
            : vk->device_info.drm_primary_minor;
        const bool has_alt = has_render && has_primary;
        frame_state.implicit_dmabuf_sync =
            (wl->drm_syncobj_id == 0u || !vk->device_info.has_drm_render_node);
        int feedback = wayland_query_surface_dmabuf_feedback(
            wl,
            target_major,
            target_minor,
            has_alt ? vk->device_info.drm_primary_major : 0,
            has_alt ? vk->device_info.drm_primary_minor : 0,
            has_alt);
        if (feedback == 0 &&
            (frame_state.implicit_dmabuf_sync || open_drm_render_node(vk) == 0) &&
            alloc_content_image(vk, wl, content_w, content_h, &shown()->image, &tranche_kind) == 0 &&
            create_acquire_semaphore(vk, &frame_state.acquire, &frame_state.acquire_sync) == 0) {
            if (vk->queues.has_dedicated_transfer &&
                vk->queues.transfer_family_index != vk->queues.graphics_family_index &&
                !vk->queues.submission_aliased &&
                vk->transfer_cmd_pool != VK_NULL_HANDLE) {
                (void)create_acquire_semaphore(vk, &frame_state.prime_ready_sem,
                                               &frame_state.prime_acquire_sync);
            }
            if (wl->feedback_cross_device &&
                !vk->device_info.profile_locked &&
                vk->device_info.execution_profile == VULKAN_PROFILE_DISCRETE_DIRECT) {
                vk->device_info.execution_profile         = VULKAN_PROFILE_HYBRID_MUXLESS;
                vk->device_info.recommended_terrain_slots = 256u;
                printf("[Vulkan] Promoted execution profile -> %s (cross-device Wayland scanout)\n",
                       vulkan_profile_to_str(vk->device_info.execution_profile));
                fflush(stdout);
            }
            break;
        }
        // ENOENT is "this GPU was not named by the compositor". An allocation
        // failure is "it was named, but this device cannot export that buffer".
        // Another eligible GPU may be the display device itself.
        if (feedback != 0 && feedback != -ENOENT) {
            (void)frame_stop();
            return 1;
        }
        if (feedback == 0) {
            fprintf(stderr,
                    "[Vulkan] This GPU cannot export a buffer or timeline the compositor can import.\n");
        }
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
        vulkan_destroy_exportable_image(vk, &shown()->image);
        if (frame_state.drm_fd >= 0) {
            close(frame_state.drm_fd);
            frame_state.drm_fd = -1;
        }
        vulkan_command_pool_destroy(vk);
        if (vulkan_device_use_next(vk) != VULKAN_SUCCESS ||
            vulkan_command_pool_create(vk) != VULKAN_SUCCESS) {
            fprintf(stderr, "[Vulkan Error] No remaining GPU can present to this compositor.\n");
            (void)frame_stop();
            return 1;
        }
    }

    if (vk->device_info.is_true_uma) {
        ring_release_pager_fixed_buffers(ring);
    }

    if (bar_open_surface(wl) != 0) {
        (void)frame_stop();
        return 1;
    }
    if (!frame_state.implicit_dmabuf_sync) {
        if (create_acquire_timeline(wl) < 0 ||
            create_release_timeline(wl, shown()) != 0) {
            (void)frame_stop();
            return 1;
        }
    } else {
        shown()->release_point = 1u;
        printf("[Engine] Explicit sync unavailable (drm_syncobj=%u); active fallback: DMA_BUF_IOCTL_IMPORT/EXPORT_SYNC_FILE + wl_buffer.release\n",
               wl->drm_syncobj_id);
        fflush(stdout);
    }
    wl->release_timeline_id = shown()->release_timeline_id;

    gpu_timer_init(vk);
    if (input_gpu_init(vk) != 0) {
        (void)frame_stop();
        return 1;
    }
    // The layout has to exist before the app binds the scene sample count.
    if (chrome_gpu_init(vk) != 0 ||
        (engine_app() != nullptr && engine_app()->init != nullptr &&
         engine_app()->init(vk, forward_init_cqe, wl) != 0)) {
        (void)frame_stop();
        return 1;
    }
    if (engine_app() != nullptr && engine_app()->load != nullptr &&
        engine_app()->load() != 0) {
        (void)frame_stop();
        return 1;
    }
    if (record_and_submit_clear(vk, shown(), frame_state.acquire, 1u, false, 0u) < 0) {
        (void)frame_stop();
        return 1;
    }

    uint32_t scene_fourcc = wayland_drm_fourcc_for_modifier(
        wl, shown()->image.drm_format_modifier, true);
    if (wayland_import_dmabuf_immed(
            wl,
            shown()->image.dmabuf_fd,
            shown()->image.width,
            shown()->image.height,
            (uint32_t)shown()->image.row_pitch,
            (uint32_t)shown()->image.offset,
            scene_fourcc,
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
    frame_state.turn_frames   = 1u;
    frame_state.loop_was_busy = true;
    printf("[Engine] Presented 1 visible frame: window=%ux%u scene=%ux%u scale=%.2fx (serial=%u), fourcc=0x%08x (%s), modifier=0x%llx (%s tranche), stride=%zu, offset=%zu, fd=%d, %s acquire=%llu release=%llu timeline=%u\n",
           frame_state.window_w,
           frame_state.window_h,
           shown()->image.width,
           shown()->image.height,
           (double)wayland_scale_factor(wl),
           present_serial,
           scene_fourcc,
           scene_fourcc == ENG_DRM_FORMAT_XRGB8888 ? "XRGB8888" : "ARGB8888",
           (unsigned long long)shown()->image.drm_format_modifier,
           tranche_kind,
           (size_t)shown()->image.row_pitch,
           (size_t)shown()->image.offset,
           shown()->image.dmabuf_fd,
           frame_state.implicit_dmabuf_sync ? "implicit-dmabuf-sync" : "explicit-sync",
           (unsigned long long)shown()->acquire_point,
           (unsigned long long)shown()->release_point,
           shown()->release_timeline_id);
    printf("hello init from engine\n");
    return 0;
}

