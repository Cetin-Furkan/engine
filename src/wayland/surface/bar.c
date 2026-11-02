// Desynced top-bar subsurface.
#include "wayland/internal.h"
#include <errno.h>
#include <stdio.h>


void wayland_arm_bar(WaylandContext *w, uint32_t buffer, uint32_t width, uint32_t height,
                     uint32_t acquire_timeline, uint64_t acquire_point,
                     uint32_t release_timeline, uint64_t release_point) {
    if (w == nullptr) {
        return;
    }
    w->bar_attach_buffer = buffer;
    w->bar_attach_w = width;
    w->bar_attach_h = height;
    w->bar_attach_acquire_tl = acquire_timeline;
    w->bar_attach_acquire_pt = acquire_point;
    w->bar_attach_release_tl = release_timeline;
    w->bar_attach_release_pt = release_point;
}


int wayland_create_bar(WaylandContext *w, int32_t x, int32_t y) {
    if (w == nullptr || !w->is_initialized || w->surface_id == 0 || w->compositor_id == 0) {
        return -EINVAL;
    }
    if (w->bar_surface_id != 0) {
        return 0;
    }
    if (w->subcompositor_id == 0 || w->drm_syncobj_id == 0) {
        wl_fail(w, "wl_subcompositor or explicit sync is missing; the bar needs its own surface");
        return -ENOTSUP;
    }
    if (wl_flush_pending(w) < 0) {
        return -EIO;
    }
    uint32_t surface = wayland_alloc_id(w);
    uint32_t sub = wayland_alloc_id(w);
    uint32_t sync = wayland_alloc_id(w);
    if (surface == 0 || sub == 0 || sync == 0) {
        wl_release_unsent(w, sync);
        wl_release_unsent(w, sub);
        wl_release_unsent(w, surface);
        return -EIO;
    }
    uint32_t mark = w->send_len;
    uint32_t sub_payload[3] = { sub, surface, w->surface_id };
    uint32_t sync_payload[2] = { sync, surface };
    int32_t pos[2] = { x, y };
    // wl_compositor.create_surface, wl_subcompositor.get_subsurface,
    // wl_subsurface.set_position (1) and set_desync (5),
    // wp_linux_drm_syncobj_manager_v1.get_surface (1), then commit the desync.
    bool ok = wayland_put_raw(w, w->compositor_id, 0u, &surface, sizeof surface) &&
              wayland_put_raw(w, w->subcompositor_id, 1u, sub_payload, sizeof sub_payload) &&
              wayland_put_raw(w, sub, 1u, pos, sizeof pos) &&
              wayland_put_raw(w, sub, 5u, nullptr, 0u) &&
              wayland_put_raw(w, w->drm_syncobj_id, 1u, sync_payload, sizeof sync_payload) &&
              wayland_put_raw(w, surface, 6u, nullptr, 0u);
    if (!ok) {
        wayland_rewind_send(w, mark);
        wl_release_unsent(w, sync);
        wl_release_unsent(w, sub);
        wl_release_unsent(w, surface);
        fprintf(stderr, "[Wayland Error] Bar subsurface encode failed: %s\n", w->err);
        return -EIO;
    }
    w->bar_surface_id = surface;
    w->bar_subsurface_id = sub;
    w->bar_sync_id = sync;
    if (wl_finish_roundtrip(w, -1) < 0) {
        fprintf(stderr, "[Wayland Error] Bar subsurface failed: %s\n", w->err);
        return -EIO;
    }
    return 0;
}


int wayland_queue_bar_commit(WaylandContext *w) {
    if (w == nullptr || !w->is_initialized || w->failed || w->bar_surface_id == 0) {
        return -EINVAL;
    }
    if (w->bar_attach_buffer == 0) {
        return -EAGAIN;
    }
    if (w->send_len != 0) {
        return -EBUSY;
    }
    uint32_t mark = w->send_len;
    uint32_t fresh = 0;
    uint32_t bw = w->bar_attach_w;
    uint32_t bh = w->bar_attach_h;
    if (!wl_commit_explicit(w, w->bar_surface_id, w->bar_sync_id,
                            w->bar_attach_buffer, bw, bh,
                            w->bar_attach_acquire_tl, w->bar_attach_acquire_pt,
                            w->bar_attach_release_tl, w->bar_attach_release_pt,
                            &w->bar_opaque_id, &w->bar_opaque_w, &w->bar_opaque_h,
                            &fresh)) {
        wayland_rewind_send(w, mark);
        fprintf(stderr, "[Wayland Error] Bar commit encode failed: %s\n", w->err);
        return -EIO;
    }
    int queued = wayland_queue_send(w, false);
    if (queued != 0) {
        wayland_rewind_send(w, mark);
        wl_release_unsent(w, fresh);
        return queued;
    }
    wl_opaque_keep(&w->bar_opaque_id, &w->bar_opaque_w, &w->bar_opaque_h, fresh, bw, bh);
    w->bar_attach_buffer = 0;
    return 0;
}

