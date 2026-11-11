#include "frame/frame_int.h"

// A new size is drawn beside the front image and installed when the send lands.

static int note_idle_send(WaylandContext *wl, const struct io_uring_cqe *cqe, uint32_t pending_serial);
static int install_resize(WaylandContext *wl, VulkanContext *vk);
static int queue_replacement(WaylandContext *wl, VulkanContext *vk, uint32_t width,
                             uint32_t height, bool with_configure, uint32_t serial,
                             bool same_repaint);

static int note_idle_send(WaylandContext *wl, const struct io_uring_cqe *cqe, uint32_t pending_serial) {
    bool tracked = wl->send_tracks_ack;
    int noted = wayland_note_send_cqe(wl, cqe, pending_serial);
    if (!tracked && noted != 1) {
        pointer_on_send_done(noted == 0);
    }
    return noted;
}

bool configure_needs_ack(const WaylandContext *wl) {
    return wl != nullptr && wl->xdg_surface_configured &&
           (!wl->configure_ack_valid || wl->configure_serial != wl->acked_configure_serial);
}

bool consume_noted_signal(WaylandContext *wl, int *signo) {
    if (wl == nullptr || !wl->noted_signal) {
        return false;
    }
    wl->noted_signal = false;
    if (signo != nullptr) {
        int got = frame_signal_number(wl->noted_signal_res);
        if (got != 0) {
            *signo = got;
        }
    }
    wl->noted_signal_res = 0;
    return true;
}

bool pending_content_size(const WaylandContext *wl, uint32_t *width, uint32_t *height) {
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
    return w != frame_state.window_w || h != frame_state.window_h;
}

bool content_size_pending(const ChromeSpec *spec, const WaylandContext *wl) {
    return spec != nullptr && spec->resize && pending_content_size(wl, nullptr, nullptr);
}

void drop_resize_attempt(WaylandContext *wl, VulkanContext *vk) {
    if (!frame_state.resize_pending) {
        return;
    }
    if (frame_state.resize_old_w != 0 && frame_state.resize_old_h != 0) {
        frame_apply_layout(wl, frame_state.resize_old_w, frame_state.resize_old_h, nullptr, nullptr);
    }
    if (wl != nullptr) {
        wl->buffer_id = frame_state.resize_old_buffer;
    }
    reap_frame_resources(wl, vk, &frame_state.resize_fresh);
    blank_frame(&frame_state.resize_fresh);
    frame_state.resize_pending = false;
    frame_state.same_repaint = false;
    bar_drop_unsent(wl, vk);
}

static int install_resize(WaylandContext *wl, VulkanContext *vk) {
    if (!frame_state.resize_pending) {
        return 0;
    }
    FrameSlot fresh = frame_state.resize_fresh;
    uint64_t rel = frame_state.resize_release;
    uint32_t serial = frame_state.resize_serial;
    char tranche[16];
    snprintf(tranche, sizeof tranche, "%s", frame_state.resize_tranche);
    blank_frame(&frame_state.resize_fresh);
    frame_state.resize_pending = false;
    // This completion installed the resize, not a turn swap. Leaving the
    // flag set would swap the new front away on the next idle send.
    frame_state.turn_swap_pending = false;

    if (retire_pixels(wl, vk, shown()) != 0) {
        fprintf(stderr,
                "[Engine Error] Resize installed over a held image. Old Vulkan handles were left alive.\n");
        clear_pixels(shown());
    }

    shown()->image           = fresh.image;
    shown()->image_view      = fresh.image_view;
    shown()->prime_image     = fresh.prime_image;
    shown()->prime_memory    = fresh.prime_memory;
    shown()->prime_view      = fresh.prime_view;
    shown()->view_format      = fresh.view_format;
    shown()->targets          = fresh.targets;
    shown()->cmd_buf          = fresh.cmd_buf;
    shown()->transfer_cmd_buf = fresh.transfer_cmd_buf;
    shown()->surface_damage   = fresh.surface_damage;
    shown()->copy_damage      = fresh.copy_damage;
    shown()->acquire_point    = fresh.acquire_point;
    shown()->wl_buffer_id    = fresh.wl_buffer_id;
    shown()->foreign_owned   = fresh.foreign_owned;
    shown()->release_pending = true;
    shown()->release_point   = rel;
    if (wl != nullptr) {
        wl->release_timeline_id = shown()->release_timeline_id;
        wl->buffer_id           = fresh.wl_buffer_id;
    }

    if (frame_state.same_repaint) {
        frame_state.same_repaint = false;
        printf("[Chrome] Repainted %ux%u\n", shown()->image.width, shown()->image.height);
    } else {
        printf("[Engine] Resized buffer to %ux%u (serial=%u), modifier=0x%llx (%s tranche), stride=%zu, explicit-sync acquire=%llu release=%llu timeline=%u\n",
               shown()->image.width,
               shown()->image.height,
               serial,
               (unsigned long long)shown()->image.drm_format_modifier,
               tranche[0] != '\0' ? tranche : "general",
               (size_t)shown()->image.row_pitch,
               (unsigned long long)frame_state.sync_point,
               (unsigned long long)shown()->release_point,
               shown()->release_timeline_id);
    }

    if (frame_state.turn_phase != k_turn_done) {
        if (alloc_spare(wl, vk) != 0) {
            return -1;
        }
    }
    return 0;
}

int finish_idle_send(WaylandContext *wl, VulkanContext *vk,
                            const struct io_uring_cqe *cqe, uint32_t pending_serial) {
    uint32_t serial = frame_state.resize_pending ? frame_state.resize_serial : pending_serial;
    int noted = note_idle_send(wl, cqe, serial);
    if (noted < 0) {
        drop_resize_attempt(wl, vk);
        bar_drop_unsent(wl, vk);
        frame_state.turn_swap_pending = false;
        return noted;
    }
    if (noted == 1) {
        return 1;
    }
    if (frame_state.resize_pending) {
        int installed = install_resize(wl, vk);
        bar_install();
        return installed;
    }
    bar_install();
    commit_turn_swap(wl);
    return 0;
}

// Records a replacement image and queues the commit. The shown slot stays the
// front until the send completion. A same-size repaint acks nothing and arms
// no frame callback. No wl_display.sync.
static int queue_replacement(WaylandContext *wl, VulkanContext *vk, uint32_t width,
                             uint32_t height, bool with_configure, uint32_t serial,
                             bool same_repaint) {
    if (frame_state.resize_pending || width == 0 || height == 0 ||
        frame_state.acquire == VK_NULL_HANDLE ||
        frame_state.acquire_sync == VK_NULL_HANDLE ||
        frame_state.acquire_handle == 0 ||
        shown()->release_timeline_id == 0 ||
        shown()->release_handle == 0) {
        return -EINVAL;
    }
    if (make_orphan_room(wl, vk) != 0) {
        fprintf(stderr, "[Engine Error] Resize refused. Eight release points are still held.\n");
        return -1;
    }

    uint32_t old_w = frame_state.window_w;
    uint32_t old_h = frame_state.window_h;
    uint32_t content_w = 0;
    uint32_t content_h = 0;
    frame_apply_layout(wl, width, height, &content_w, &content_h);

    FrameSlot fresh = {
        .image           = { .dmabuf_fd = -1 },
        .release_eventfd = -1,
    };
    const char *tranche_kind = "general";
    if (alloc_content_image(vk, wl, content_w, content_h, &fresh.image, &tranche_kind) != 0) {
        frame_apply_layout(wl, old_w, old_h, nullptr, nullptr);
        return -1;
    }

    uint64_t next_point = frame_state.sync_point + 1u;
    if (record_and_submit_clear(vk, &fresh, frame_state.acquire, next_point, false, 0u) != 0) {
        reap_frame_resources(wl, vk, &fresh);
        frame_apply_layout(wl, old_w, old_h, nullptr, nullptr);
        return -1;
    }

    int prepared = bar_prepare(wl, vk, true);
    if (prepared < 0 && prepared != -EAGAIN) {
        reap_frame_resources(wl, vk, &fresh);
        frame_apply_layout(wl, old_w, old_h, nullptr, nullptr);
        return -1;
    }

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
        bar_drop_unsent(wl, vk);
        reap_frame_resources(wl, vk, &fresh);
        frame_apply_layout(wl, old_w, old_h, nullptr, nullptr);
        return -1;
    }
    fresh.wl_buffer_id = new_buffer;

    uint64_t rel = shown()->release_point;
    if (shown()->release_pending) {
        rel += 1u;
    }

    int queued = wayland_queue_turn_commit(
        wl,
        new_buffer,
        fresh.image.width,
        fresh.image.height,
        0,
        0,
        fresh.image.width,
        fresh.image.height,
        wl->acquire_timeline_id,
        fresh.acquire_point,
        shown()->release_timeline_id,
        rel,
        false,
        with_configure,
        serial);
    if (queued != 0) {
        bar_drop_unsent(wl, vk);
        wl->buffer_id = previous_buffer;
        reap_frame_resources(wl, vk, &fresh);
        frame_apply_layout(wl, old_w, old_h, nullptr, nullptr);
        return -1;
    }

    frame_state.resize_fresh = fresh;
    frame_state.resize_release = rel;
    frame_state.resize_serial = serial;
    frame_state.resize_old_buffer = previous_buffer;
    frame_state.resize_old_w = old_w;
    frame_state.resize_old_h = old_h;
    snprintf(frame_state.resize_tranche, sizeof frame_state.resize_tranche, "%s", tranche_kind != nullptr ? tranche_kind : "general");
    frame_state.same_repaint = same_repaint;
    frame_state.resize_pending = true;
    return 0;
}

int queue_resize_frame(WaylandContext *wl, VulkanContext *vk) {
    uint32_t width  = wl->configure_width;
    uint32_t height = wl->configure_height;
    chrome_clamp_size(&width, &height);
    return queue_replacement(wl, vk, width, height, true, wl->configure_serial, false);
}

int queue_chrome_repaint(WaylandContext *wl, VulkanContext *vk) {
    uint32_t top = 0;
    uint32_t bottom = 0;
    chrome_band_thickness(&top, &bottom);
    (void)bottom;
    uint32_t content_h = frame_state.window_h > top ? frame_state.window_h - top : 1u;
    if (shown()->image.width != frame_state.window_w || shown()->image.height != content_h ||
        top != frame_state.bar_px) {
        return queue_replacement(wl, vk, frame_state.window_w, frame_state.window_h, false, 0, true);
    }
    int prepared = bar_prepare(wl, vk, true);
    if (prepared == 1) {
        return wayland_queue_bar_commit(wl);
    }
    if (prepared == 0) {
        return -EAGAIN;
    }
    return prepared;
}
