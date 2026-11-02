// Scene and bar commits, detach, and teardown.
#include "wayland/internal.h"
#include <errno.h>
#include <stdio.h>


bool wl_commit_explicit(WaylandContext *w, uint32_t surface, uint32_t sync_id,
                        uint32_t buffer, uint32_t width, uint32_t height,
                        uint32_t acquire_timeline, uint64_t acquire_point,
                        uint32_t release_timeline, uint64_t release_point,
                        uint32_t *opaque_id, uint32_t *opaque_w, uint32_t *opaque_h,
                        uint32_t *fresh) {
    if (fresh != nullptr) {
        *fresh = 0;
    }
    uint32_t mine = 0;
    if (!wl_opaque_prepare(w, surface, opaque_id, opaque_w, opaque_h, width, height, &mine)) {
        return false;
    }
    uint32_t acquire_payload[3];
    uint32_t release_payload[3];
    wl_put_timeline_point(acquire_payload, acquire_timeline, acquire_point);
    wl_put_timeline_point(release_payload, release_timeline, release_point);
    uint32_t attach_payload[3] = { buffer, 0u, 0u };
    uint16_t damage_opcode = (w->compositor_bound_version >= 4u) ? 9u : 2u;
    int32_t damage_payload[4] = { 0, 0, (int32_t)width, (int32_t)height };
    bool ok = wayland_put_raw(w, sync_id, 1u, acquire_payload, sizeof acquire_payload) &&
              wayland_put_raw(w, sync_id, 2u, release_payload, sizeof release_payload) &&
              wayland_put_raw(w, surface, 1u, attach_payload, sizeof attach_payload) &&
              wayland_put_raw(w, surface, damage_opcode, damage_payload, sizeof damage_payload) &&
              wayland_put_raw(w, surface, 6u, nullptr, 0u);
    if (!ok) {
        wl_release_unsent(w, mine);
        return false;
    }
    if (fresh != nullptr) {
        *fresh = mine;
    }
    return true;
}


int wayland_present_single_frame_and_wait(
    WaylandContext *w,
    uint32_t        buffer_id,
    uint32_t        width,
    uint32_t        height,
    uint32_t        configure_serial,
    uint32_t        acquire_timeline,
    uint64_t        acquire_point,
    uint32_t        release_timeline,
    uint64_t        release_point
) {
    if (w == nullptr || !w->is_initialized || buffer_id == 0 || w->surface_id == 0 ||
        w->explicit_sync_id == 0 || acquire_timeline == 0 || release_timeline == 0 ||
        acquire_timeline == release_timeline) {
        return -EINVAL;
    }
    // Flush a batch already queued ahead of this present when the buffer is close to full.
    if (w->send_len + 256u > sizeof(w->send_bytes)) {
        if (wl_send_batch(w) < 0) {
            return -EIO;
        }
    }

    w->frame_done = false;
    uint32_t batch       = w->send_len;
    uint32_t bar_fresh   = 0;
    uint32_t scene_fresh = 0;
    uint32_t bar_w       = 0;
    uint32_t bar_h       = 0;
    bool     sent_bar    = false;

    if (w->lock_toplevel_size) {
        int32_t sz[2] = { w->lock_width, w->lock_height };
        // set_max_size is request 7, set_min_size is request 8. Applied on commit.
        if (!wayland_put_raw(w, w->xdg_toplevel_id, 7u, sz, sizeof sz) ||
            !wayland_put_raw(w, w->xdg_toplevel_id, 8u, sz, sizeof sz)) {
            wayland_rewind_send(w, batch);
            fprintf(stderr, "[Wayland Error] Toplevel size lock encode failed: %s\n", w->err);
            return -EIO;
        }
    } else if (w->min_width > 0 && w->min_height > 0 &&
               !wl_put_min_size(w, w->min_width, w->min_height)) {
        wayland_rewind_send(w, batch);
        fprintf(stderr, "[Wayland Error] Toplevel minimum encode failed: %s\n", w->err);
        return -EIO;
    }

    // Ack only a serial we have not already accepted. Repeating one is invalid_serial.
    // acked_configure_serial starts at 0, so the flag distinguishes "never acked".
    bool did_ack = false;
    if (!w->configure_ack_valid || configure_serial != w->acked_configure_serial) {
        if (!wayland_put_raw(w, w->xdg_surface_id, 4u, &configure_serial, sizeof configure_serial)) {
            wayland_rewind_send(w, batch);
            fprintf(stderr, "[Wayland Error] ack_configure encode failed: %s\n", w->err);
            return -EIO;
        }
        did_ack = true;
    }

    // 2. xdg_surface.set_window_geometry (request 3). The bar sits outside the
    // scene buffer, so the geometry is the window, not the scene image.
    int32_t geom_payload[4];
    wl_geom_box(w, width, height, geom_payload);
    if (!wayland_put_raw(w, w->xdg_surface_id, 3u, geom_payload, sizeof(geom_payload))) {
        wayland_rewind_send(w, batch);
        fprintf(stderr, "[Wayland Error] set_window_geometry encode failed: %s\n", w->err);
        return -EIO;
    }

    if (w->bar_attach_buffer != 0 && w->bar_surface_id != 0 && w->bar_sync_id != 0) {
        bar_w = w->bar_attach_w;
        bar_h = w->bar_attach_h;
        if (!wl_commit_explicit(w, w->bar_surface_id, w->bar_sync_id,
                                w->bar_attach_buffer, bar_w, bar_h,
                                w->bar_attach_acquire_tl, w->bar_attach_acquire_pt,
                                w->bar_attach_release_tl, w->bar_attach_release_pt,
                                &w->bar_opaque_id, &w->bar_opaque_w, &w->bar_opaque_h,
                                &bar_fresh)) {
            wayland_rewind_send(w, batch);
            fprintf(stderr, "[Wayland Error] Bar present encode failed: %s\n", w->err);
            return -EIO;
        }
        sent_bar = true;
    }

    // The scene buffer is opaque. The compositor can scan it out directly.
    if (!wl_opaque_prepare(w, w->surface_id, &w->scene_opaque_id, &w->scene_opaque_w,
                           &w->scene_opaque_h, width, height, &scene_fresh)) {
        wayland_rewind_send(w, batch);
        wl_release_unsent(w, bar_fresh);
        fprintf(stderr, "[Wayland Error] Scene opaque region encode failed: %s\n", w->err);
        return -EIO;
    }

    // 3. set_acquire_point (request 1) and set_release_point (request 2) on this commit
    uint32_t acquire_payload[3];
    uint32_t release_payload[3];
    wl_put_timeline_point(acquire_payload, acquire_timeline, acquire_point);
    wl_put_timeline_point(release_payload, release_timeline, release_point);
    uint32_t attach_payload[3] = { buffer_id, 0u, 0u };
    uint16_t damage_opcode = (w->compositor_bound_version >= 4u) ? 9u : 2u;
    int32_t damage_payload[4] = { 0, 0, (int32_t)width, (int32_t)height };

    bool ok = wayland_put_raw(w, w->explicit_sync_id, 1u, acquire_payload, sizeof acquire_payload) &&
              wayland_put_raw(w, w->explicit_sync_id, 2u, release_payload, sizeof release_payload) &&
              wayland_put_raw(w, w->surface_id, 1u, attach_payload, sizeof(attach_payload)) &&
              wayland_put_raw(w, w->surface_id, damage_opcode, damage_payload, sizeof(damage_payload));

    if (ok && w->viewport_id != 0) {
        int32_t drop_dest[2] = { -1, -1 };
        // wp_viewport.set_destination is request 2. -1,-1 clears the scale.
        ok = wayland_put_raw(w, w->viewport_id, 2u, drop_dest, sizeof drop_dest);
    }

    // 5. wl_surface.commit (request 6)
    if (ok) {
        ok = wayland_put_raw(w, w->surface_id, 6u, nullptr, 0u);
    }

    // 6. wl_display.sync guarantees the compositor processed commit even if occluded.
    // No standing wl_surface.frame callback is armed.
    uint32_t sync_id = 0;
    if (ok) {
        sync_id = wayland_alloc_id(w);
        if (sync_id == 0 || !wl_append_display_sync(w, sync_id)) {
            ok = false;
        }
    }

    if (!ok) {
        wayland_rewind_send(w, batch);
        wl_release_unsent(w, sync_id);
        wl_release_unsent(w, scene_fresh);
        wl_release_unsent(w, bar_fresh);
        fprintf(stderr, "[Wayland Error] Present commit encode failed: %s\n", w->err);
        return -EIO;
    }

    w->synced  = false;
    w->sync_id = sync_id;

    if (wl_send_batch(w) < 0) {
        if (w->send_off == 0) {
            wayland_rewind_send(w, batch);
            wl_release_unsent(w, sync_id);
            wl_release_unsent(w, scene_fresh);
            wl_release_unsent(w, bar_fresh);
        }
        fprintf(stderr, "[Wayland Error] Present commit batch failed: %s\n", w->err);
        return -EIO;
    }

    w->buffers_detached = false;
    wl_opaque_keep(&w->bar_opaque_id, &w->bar_opaque_w, &w->bar_opaque_h, bar_fresh, bar_w, bar_h);
    wl_opaque_keep(&w->scene_opaque_id, &w->scene_opaque_w, &w->scene_opaque_h,
                   scene_fresh, width, height);
    if (sent_bar) {
        w->bar_attach_buffer = 0;
    }
    if (did_ack) {
        w->acked_configure_serial = configure_serial;
        w->configure_ack_valid = true;
    }

    struct io_uring_cqe cqe = {0};
    while (!w->synced && !w->failed) {
        if (wl_wait_tag(w, ENG_UD_WL_RECV, &cqe) < 0) {
            break;
        }
        wayland_on_cqe(w, &cqe);
    }
    if (w->failed || !w->synced) {
        fprintf(stderr, "[Wayland Error] Present sync wait failed: %s\n", w->err);
        return -EIO;
    }
    w->frame_done = true;

    if (w->ring->sq_pending > 0) {
        unsigned n = ring_submit(w->ring);
        (void)ring_enter(w->ring, n, 0, -1);
    }

    return 0;
}


int wayland_queue_turn_commit(
    WaylandContext *w,
    uint32_t        buffer_id,
    uint32_t        width,
    uint32_t        height,
    uint32_t        acquire_timeline,
    uint64_t        acquire_point,
    uint32_t        release_timeline,
    uint64_t        release_point,
    bool            arm_frame,
    bool            with_configure,
    uint32_t        configure_serial
) {
    if (w == nullptr || !w->is_initialized || w->failed || w->surface_id == 0) {
        return -EINVAL;
    }
    if (w->send_len != 0) {
        return -EBUSY;
    }
    if (buffer_id == 0 && !arm_frame) {
        return -EINVAL;
    }
    if (with_configure && buffer_id == 0) {
        return -EINVAL;
    }
    if (buffer_id != 0 &&
        (width == 0 || height == 0 || w->explicit_sync_id == 0 ||
         acquire_timeline == 0 || release_timeline == 0 ||
         acquire_timeline == release_timeline ||
         acquire_point == 0 || release_point == 0)) {
        return -EINVAL;
    }
    if (arm_frame && w->frame_cb_id != 0) {
        return -EALREADY;
    }

    uint32_t mark = w->send_len;
    bool did_ack = false;
    bool ok = true;
    if (with_configure) {
        if (w->xdg_surface_id == 0 || w->xdg_toplevel_id == 0) {
            ok = false;
        } else if (w->lock_toplevel_size) {
            int32_t sz[2] = { w->lock_width, w->lock_height };
            // set_max_size is request 7, set_min_size is request 8. Applied on commit.
            ok = wayland_put_raw(w, w->xdg_toplevel_id, 7u, sz, sizeof sz) &&
                 wayland_put_raw(w, w->xdg_toplevel_id, 8u, sz, sizeof sz);
        } else if (w->min_width > 0 && w->min_height > 0) {
            ok = wl_put_min_size(w, w->min_width, w->min_height);
        }
        // Repeating an acked serial is invalid_serial. A new one has to ride this commit.
        if (ok && (!w->configure_ack_valid || configure_serial != w->acked_configure_serial)) {
            ok = wayland_put_raw(w, w->xdg_surface_id, 4u, &configure_serial, sizeof configure_serial);
            did_ack = ok;
        }
        if (ok) {
            int32_t geom[4];
            wl_geom_box(w, width, height, geom);
            ok = wayland_put_raw(w, w->xdg_surface_id, 3u, geom, sizeof geom);
        }
        if (ok && w->viewport_id != 0) {
            int32_t drop_dest[2] = { -1, -1 };
            ok = wayland_put_raw(w, w->viewport_id, 2u, drop_dest, sizeof drop_dest);
        }
    }
    uint32_t bar_fresh = 0;
    uint32_t scene_fresh = 0;
    uint32_t bar_w = w->bar_attach_w;
    uint32_t bar_h = w->bar_attach_h;
    if (ok && w->bar_attach_buffer != 0 && w->bar_surface_id != 0 && w->bar_sync_id != 0) {
        ok = wl_commit_explicit(w, w->bar_surface_id, w->bar_sync_id,
                                w->bar_attach_buffer, bar_w, bar_h,
                                w->bar_attach_acquire_tl, w->bar_attach_acquire_pt,
                                w->bar_attach_release_tl, w->bar_attach_release_pt,
                                &w->bar_opaque_id, &w->bar_opaque_w, &w->bar_opaque_h,
                                &bar_fresh);
    }
    if (ok && buffer_id != 0) {
        uint32_t acquire_payload[3];
        uint32_t release_payload[3];
        wl_put_timeline_point(acquire_payload, acquire_timeline, acquire_point);
        wl_put_timeline_point(release_payload, release_timeline, release_point);
        uint32_t attach_payload[3] = { buffer_id, 0u, 0u };
        uint16_t damage_opcode = (w->compositor_bound_version >= 4u) ? 9u : 2u;
        int32_t damage_payload[4] = { 0, 0, (int32_t)width, (int32_t)height };
        ok = wl_opaque_prepare(w, w->surface_id, &w->scene_opaque_id, &w->scene_opaque_w,
                               &w->scene_opaque_h, width, height, &scene_fresh) &&
             wayland_put_raw(w, w->explicit_sync_id, 1u, acquire_payload, sizeof acquire_payload) &&
             wayland_put_raw(w, w->explicit_sync_id, 2u, release_payload, sizeof release_payload) &&
             wayland_put_raw(w, w->surface_id, 1u, attach_payload, sizeof attach_payload) &&
             wayland_put_raw(w, w->surface_id, damage_opcode, damage_payload, sizeof damage_payload);
    }
    // Allocated after the regions. A fresh callback id encoded before a higher
    // create_region id makes Mutter reject the region (invalid new object id).
    uint32_t cb = 0;
    if (ok && arm_frame) {
        cb = wayland_alloc_id(w);
        if (cb == 0) {
            ok = false;
        } else {
            // wl_surface.frame is request 3. It applies to the commit below.
            ok = wayland_put_raw(w, w->surface_id, 3u, &cb, sizeof cb);
        }
    }
    if (ok) {
        ok = wayland_put_raw(w, w->surface_id, 6u, nullptr, 0u);
    }
    if (!ok) {
        wayland_rewind_send(w, mark);
        wl_release_unsent(w, cb);
        wl_release_unsent(w, scene_fresh);
        wl_release_unsent(w, bar_fresh);
        fprintf(stderr, "[Wayland Error] Queued commit encode failed: %s\n", w->err);
        return -EIO;
    }
    int queued = wayland_queue_send(w, did_ack);
    if (queued != 0) {
        wayland_rewind_send(w, mark);
        wl_release_unsent(w, cb);
        wl_release_unsent(w, scene_fresh);
        wl_release_unsent(w, bar_fresh);
        return queued;
    }
    wl_opaque_keep(&w->bar_opaque_id, &w->bar_opaque_w, &w->bar_opaque_h, bar_fresh, bar_w, bar_h);
    if (buffer_id != 0) {
        wl_opaque_keep(&w->scene_opaque_id, &w->scene_opaque_w, &w->scene_opaque_h,
                       scene_fresh, width, height);
        w->buffers_detached = false;
    }
    if (bar_fresh != 0 || w->bar_attach_buffer != 0) {
        w->bar_attach_buffer = 0;
    }
    if (cb != 0) {
        w->frame_cb_id = cb;
    }
    return 0;
}


int wayland_detach_buffer(WaylandContext *w) {
    if (w == nullptr || !w->is_initialized || w->surface_id == 0 || w->failed) {
        return -EINVAL;
    }
    if (w->buffers_detached) {
        return 0;
    }
    if (wl_flush_pending(w) < 0) {
        return -EIO;
    }
    // A null wl_buffer unmaps the surface. Acquire and release points are
    // required only on a commit that attaches a non-null buffer.
    uint32_t attach_payload[3] = { 0u, 0u, 0u };
    bool detached = true;
    if (w->bar_surface_id != 0) {
        detached = wayland_put_raw(w, w->bar_surface_id, 1u, attach_payload, sizeof attach_payload) &&
                   wayland_put_raw(w, w->bar_surface_id, 6u, nullptr, 0u);
    }
    if (!detached ||
        !wayland_put_raw(w, w->surface_id, 1u, attach_payload, sizeof attach_payload) ||
        !wayland_put_raw(w, w->surface_id, 6u, nullptr, 0u)) {
        fprintf(stderr, "[Wayland Error] buffer detach encode failed: %s\n", w->err);
        return -EIO;
    }
    if (wl_finish_roundtrip(w, -1) < 0) {
        fprintf(stderr, "[Wayland Error] buffer detach failed: %s\n", w->err);
        return -EIO;
    }
    w->buffers_detached = true;
    return 0;
}


void wayland_destroy_frame_objects(WaylandContext *w) {
    if (w == nullptr || !w->is_initialized) {
        return;
    }
    // An in-flight send still owns send_bytes. The socket close drops the
    // objects. Writing the buffer now would race that send.
    if (w->send_queued) {
        return;
    }
    if (w->noted_send) {
        w->noted_send = false;
        int prog = wl_note_send_progress(w, w->noted_send_res);
        if (prog < 0) {
            w->send_len = 0;
            w->send_off = 0;
            w->send_tracks_ack = false;
        }
    }
    if (!w->failed && w->send_len > 0) {
        (void)wl_send_batch(w);
    }
    w->send_len = 0;
    w->send_off = 0;
    if (w->feedback_id != 0) {
        (void)wayland_put_raw(w, w->feedback_id, 0u, nullptr, 0u);
        w->feedback_id = 0;
    }
    // The decoration object has to die before xdg_toplevel.
    if (w->decoration_id != 0) {
        (void)wayland_put_raw(w, w->decoration_id, 0u, nullptr, 0u);
        w->decoration_id = 0;
    }
    if (w->scene_opaque_id != 0) {
        (void)wayland_put_raw(w, w->scene_opaque_id, 0u, nullptr, 0u);
        w->scene_opaque_id = 0;
    }
    if (w->bar_opaque_id != 0) {
        (void)wayland_put_raw(w, w->bar_opaque_id, 0u, nullptr, 0u);
        w->bar_opaque_id = 0;
    }
    if (w->viewport_id != 0) {
        (void)wayland_put_raw(w, w->viewport_id, 0u, nullptr, 0u);
        w->viewport_id = 0;
    }
    if (w->bar_sync_id != 0) {
        (void)wayland_put_raw(w, w->bar_sync_id, 0u, nullptr, 0u);
        w->bar_sync_id = 0;
    }
    if (w->bar_subsurface_id != 0) {
        (void)wayland_put_raw(w, w->bar_subsurface_id, 0u, nullptr, 0u);
        w->bar_subsurface_id = 0;
    }
    if (w->bar_surface_id != 0) {
        (void)wayland_put_raw(w, w->bar_surface_id, 0u, nullptr, 0u);
        w->bar_surface_id = 0;
    }
    if (w->explicit_sync_id != 0) {
        (void)wayland_put_raw(w, w->explicit_sync_id, 0u, nullptr, 0u);
        w->explicit_sync_id = 0;
    }
    if (w->acquire_timeline_id != 0) {
        (void)wayland_put_raw(w, w->acquire_timeline_id, 0u, nullptr, 0u);
        w->acquire_timeline_id = 0;
    }
    if (w->release_timeline_id != 0) {
        (void)wayland_put_raw(w, w->release_timeline_id, 0u, nullptr, 0u);
        w->release_timeline_id = 0;
    }
    if (w->xdg_toplevel_id != 0) {
        (void)wayland_put_raw(w, w->xdg_toplevel_id, 0u, nullptr, 0u);
        w->xdg_toplevel_id = 0;
    }
    if (w->xdg_surface_id != 0) {
        (void)wayland_put_raw(w, w->xdg_surface_id, 0u, nullptr, 0u);
        w->xdg_surface_id = 0;
    }
    if (w->surface_id != 0) {
        (void)wayland_put_raw(w, w->surface_id, 0u, nullptr, 0u);
        w->surface_id = 0;
    }
    if (w->buffer_id != 0) {
        (void)wayland_put_raw(w, w->buffer_id, 0u, nullptr, 0u);
        w->buffer_id = 0;
    }

    for (uint32_t i = 0; i < w->destroy_later_n && !w->failed; ++i) {
        (void)wayland_put_raw(w, w->destroy_later[i], 0u, nullptr, 0u);
    }
    w->destroy_later_n = 0;
    if (w->send_len > 0 && !w->failed) {
        w->synced  = false;
        w->sync_id = wayland_alloc_id(w);
        (void)wl_append_display_sync(w, w->sync_id);
        if (wl_send_batch(w) == 0) {
            struct io_uring_cqe cqe = {0};
            while (!w->synced && !w->failed) {
                if (wl_wait_tag(w, ENG_UD_WL_RECV, &cqe) < 0) {
                    break;
                }
                wayland_on_cqe(w, &cqe);
            }
        }
    }
    wl_close_feedback_table(w);
}