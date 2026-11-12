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
    uint32_t log_w = (surface == w->bar_surface_id && w->bar_logical_w != 0u) ? w->bar_logical_w : width;
    uint32_t log_h = (surface == w->bar_surface_id && w->bar_logical_h != 0u) ? w->bar_logical_h : height;
    uint32_t mine = 0;
    if (!wl_opaque_prepare(w, surface, opaque_id, opaque_w, opaque_h, log_w, log_h, &mine)) {
        return false;
    }
    bool ok = true;
    if (sync_id != 0u) {
        uint32_t acquire_payload[3];
        uint32_t release_payload[3];
        wl_put_timeline_point(acquire_payload, acquire_timeline, acquire_point);
        wl_put_timeline_point(release_payload, release_timeline, release_point);
        ok = wayland_put_raw(w, sync_id, 1u, acquire_payload, sizeof acquire_payload) &&
             wayland_put_raw(w, sync_id, 2u, release_payload, sizeof release_payload);
    }
    uint32_t attach_payload[3] = { buffer, 0u, 0u };
    bool buf_dmg = (w->compositor_bound_version >= 4u);
    uint16_t damage_opcode = buf_dmg ? 9u : 2u;
    int32_t damage_payload[4] = {
        0,
        0,
        buf_dmg ? (int32_t)width  : (int32_t)log_w,
        buf_dmg ? (int32_t)height : (int32_t)log_h,
    };
    if (ok) {
        ok = wayland_put_raw(w, surface, 1u, attach_payload, sizeof attach_payload) &&
             wayland_put_raw(w, surface, damage_opcode, damage_payload, sizeof damage_payload);
    }
    int32_t next_bvw = w->bar_vp_w;
    int32_t next_bvh = w->bar_vp_h;
    if (ok && surface == w->bar_surface_id && w->bar_viewport_id != 0u) {
        int32_t want_vw = (width != log_w || height != log_h) ? (int32_t)log_w : -1;
        int32_t want_vh = (width != log_w || height != log_h) ? (int32_t)log_h : -1;
        if (want_vw != w->bar_vp_w || want_vh != w->bar_vp_h) {
            int32_t dest[2] = { want_vw, want_vh };
            ok = wayland_put_raw(w, w->bar_viewport_id, 2u, dest, sizeof dest);
            if (ok) {
                next_bvw = want_vw;
                next_bvh = want_vh;
            }
        }
    }
    if (ok) {
        ok = wayland_put_raw(w, surface, 6u, nullptr, 0u);
    }
    if (!ok) {
        wl_release_unsent(w, mine);
        return false;
    }
    if (surface == w->bar_surface_id) {
        w->bar_vp_w = next_bvw;
        w->bar_vp_h = next_bvh;
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
    if (w == nullptr || !w->is_initialized || buffer_id == 0 || w->surface_id == 0) {
        return -EINVAL;
    }
    const bool use_explicit = (w->drm_syncobj_id != 0u || w->explicit_sync_id != 0u);
    if (use_explicit &&
        (w->explicit_sync_id == 0 || acquire_timeline == 0 || release_timeline == 0 ||
         acquire_timeline == release_timeline)) {
        return -EINVAL;
    }
    // Flush a batch already queued ahead of this present when the buffer is close to full.
    if (w->send_len + 256u > sizeof(w->send_bytes)) {
        if (wl_send_batch(w) < 0) {
            return -EIO;
        }
    }

    w->frame_done = false;
    uint32_t batch              = w->send_len;
    uint32_t prepended_destroys = wl_encode_deferred_destroys(w);
    uint32_t bar_fresh          = 0;
    uint32_t scene_fresh        = 0;
    uint32_t bar_w              = 0;
    uint32_t bar_h              = 0;
    bool     sent_bar           = false;
    int32_t  old_bx             = w->bar_pos_x;
    int32_t  old_by             = w->bar_pos_y;
    bool     old_bsync          = w->bar_is_synced;
    int32_t  old_bvw            = w->bar_vp_w;
    int32_t  old_bvh            = w->bar_vp_h;
    uint32_t log_w              = (w->scene_logical_w != 0u) ? w->scene_logical_w : width;
    uint32_t log_h              = (w->scene_logical_h != 0u) ? w->scene_logical_h : height;
    int32_t  want_vw            = (width != log_w || height != log_h) ? (int32_t)log_w : -1;
    int32_t  want_vh            = (width != log_w || height != log_h) ? (int32_t)log_h : -1;

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
    wl_geom_box(w, log_w, log_h, geom_payload);
    if (!wayland_put_raw(w, w->xdg_surface_id, 3u, geom_payload, sizeof(geom_payload))) {
        wayland_rewind_send(w, batch);
        fprintf(stderr, "[Wayland Error] set_window_geometry encode failed: %s\n", w->err);
        return -EIO;
    }

    if (w->bar_attach_buffer != 0 && w->bar_surface_id != 0 &&
        (!use_explicit || w->bar_sync_id != 0)) {
        bar_w = w->bar_attach_w;
        bar_h = w->bar_attach_h;
        if (!wayland_encode_bar_sync(w, w->geom_x, w->geom_y, true) ||
            !wl_commit_explicit(w, w->bar_surface_id, use_explicit ? w->bar_sync_id : 0u,
                                w->bar_attach_buffer, bar_w, bar_h,
                                w->bar_attach_acquire_tl, w->bar_attach_acquire_pt,
                                w->bar_attach_release_tl, w->bar_attach_release_pt,
                                &w->bar_opaque_id, &w->bar_opaque_w, &w->bar_opaque_h,
                                &bar_fresh)) {
            w->bar_pos_x     = old_bx;
            w->bar_pos_y     = old_by;
            w->bar_is_synced = old_bsync;
            w->bar_vp_w      = old_bvw;
            w->bar_vp_h      = old_bvh;
            wayland_rewind_send(w, batch);
            fprintf(stderr, "[Wayland Error] Bar present encode failed: %s\n", w->err);
            return -EIO;
        }
        sent_bar = true;
    }

    // The scene buffer is opaque. The compositor can scan it out directly.
    if (!wl_opaque_prepare(w, w->surface_id, &w->scene_opaque_id, &w->scene_opaque_w,
                           &w->scene_opaque_h, log_w, log_h, &scene_fresh)) {
        w->bar_pos_x     = old_bx;
        w->bar_pos_y     = old_by;
        w->bar_is_synced = old_bsync;
        w->bar_vp_w      = old_bvw;
        w->bar_vp_h      = old_bvh;
        wayland_rewind_send(w, batch);
        wl_release_unsent(w, bar_fresh);
        fprintf(stderr, "[Wayland Error] Scene opaque region encode failed: %s\n", w->err);
        return -EIO;
    }

    // 3. set_acquire_point (request 1) and set_release_point (request 2) on this commit
    bool ok = true;
    if (use_explicit) {
        uint32_t acquire_payload[3];
        uint32_t release_payload[3];
        wl_put_timeline_point(acquire_payload, acquire_timeline, acquire_point);
        wl_put_timeline_point(release_payload, release_timeline, release_point);
        ok = wayland_put_raw(w, w->explicit_sync_id, 1u, acquire_payload, sizeof acquire_payload) &&
             wayland_put_raw(w, w->explicit_sync_id, 2u, release_payload, sizeof release_payload);
    }
    uint32_t attach_payload[3] = { buffer_id, 0u, 0u };
    bool buf_dmg = (w->compositor_bound_version >= 4u);
    uint16_t damage_opcode = buf_dmg ? 9u : 2u;
    int32_t damage_payload[4] = {
        0,
        0,
        buf_dmg ? (int32_t)width  : (int32_t)log_w,
        buf_dmg ? (int32_t)height : (int32_t)log_h,
    };
    if (ok) {
        ok = wayland_put_raw(w, w->surface_id, 1u, attach_payload, sizeof(attach_payload)) &&
             wayland_put_raw(w, w->surface_id, damage_opcode, damage_payload, sizeof(damage_payload));
    }

    if (ok && w->viewport_id != 0) {
        int32_t dest[2] = { want_vw, want_vh };
        // wp_viewport.set_destination is request 2. -1,-1 clears the scale.
        ok = wayland_put_raw(w, w->viewport_id, 2u, dest, sizeof dest);
    }

    // 5. wl_surface.commit (request 6), then restore bar desync so standalone bar repaints stay zero-stall
    if (ok) {
        ok = wayland_put_raw(w, w->surface_id, 6u, nullptr, 0u);
    }
    if (ok && sent_bar) {
        ok = wayland_encode_bar_sync(w, w->geom_x, w->geom_y, false);
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
        w->bar_pos_x     = old_bx;
        w->bar_pos_y     = old_by;
        w->bar_is_synced = old_bsync;
        w->bar_vp_w      = old_bvw;
        w->bar_vp_h      = old_bvh;
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
        w->bar_vp_w = old_bvw;
        w->bar_vp_h = old_bvh;
        fprintf(stderr, "[Wayland Error] Present commit batch failed: %s\n", w->err);
        return -EIO;
    }

    wl_consume_deferred_destroys(w, prepended_destroys);

    wayland_mark_buffer_busy(w, buffer_id);
    if (sent_bar && w->bar_attach_buffer != 0u) {
        wayland_mark_buffer_busy(w, w->bar_attach_buffer);
    }

    w->buffers_detached = false;
    if (w->viewport_id != 0) {
        w->scene_vp_w = want_vw;
        w->scene_vp_h = want_vh;
    }
    uint32_t bar_log_w = (w->bar_logical_w != 0u) ? w->bar_logical_w : bar_w;
    uint32_t bar_log_h = (w->bar_logical_h != 0u) ? w->bar_logical_h : bar_h;
    wl_opaque_keep(&w->bar_opaque_id, &w->bar_opaque_w, &w->bar_opaque_h, bar_fresh, bar_log_w, bar_log_h);
    wl_opaque_keep(&w->scene_opaque_id, &w->scene_opaque_w, &w->scene_opaque_h,
                   scene_fresh, log_w, log_h);
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
    int32_t         dmg_x,
    int32_t         dmg_y,
    uint32_t        dmg_w,
    uint32_t        dmg_h,
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
    if (w->send_len != 0 || wl_send_blocked(w)) {
        return -EBUSY;
    }
    if (buffer_id == 0 && !arm_frame) {
        return -EINVAL;
    }
    if (with_configure && buffer_id == 0) {
        return -EINVAL;
    }
    const bool use_explicit = (w->drm_syncobj_id != 0u || w->explicit_sync_id != 0u);
    if (buffer_id != 0 &&
        (width == 0 || height == 0 ||
         (use_explicit &&
          (w->explicit_sync_id == 0 ||
           acquire_timeline == 0 || release_timeline == 0 ||
           acquire_timeline == release_timeline ||
           acquire_point == 0 || release_point == 0)))) {
        return -EINVAL;
    }
    if (arm_frame && w->frame_cb_id != 0) {
        return -EALREADY;
    }

    uint32_t mark               = w->send_len;
    uint32_t prepended_destroys = wl_encode_deferred_destroys(w);
    int32_t  old_bx             = w->bar_pos_x;
    int32_t  old_by             = w->bar_pos_y;
    bool     old_bsync          = w->bar_is_synced;
    int32_t  old_bvw            = w->bar_vp_w;
    int32_t  old_bvh            = w->bar_vp_h;
    bool     synced_bar_resize  = false;
    bool     did_ack            = false;
    bool     wrote_vp           = false;
    bool     ok                 = true;
    uint32_t log_w              = (w->scene_logical_w != 0u) ? w->scene_logical_w : width;
    uint32_t log_h              = (w->scene_logical_h != 0u) ? w->scene_logical_h : height;
    int32_t  want_vw            = (buffer_id != 0 && (width != log_w || height != log_h)) ? (int32_t)log_w : -1;
    int32_t  want_vh            = (buffer_id != 0 && (width != log_w || height != log_h)) ? (int32_t)log_h : -1;

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
            wl_geom_box(w, log_w, log_h, geom);
            ok = wayland_put_raw(w, w->xdg_surface_id, 3u, geom, sizeof geom);
        }
    }
    uint32_t bar_fresh = 0;
    uint32_t scene_fresh = 0;
    uint32_t bar_w = w->bar_attach_w;
    uint32_t bar_h = w->bar_attach_h;
    uint32_t sent_bar_buf = 0u;
    if (ok && w->bar_attach_buffer != 0 && w->bar_surface_id != 0 &&
        (!use_explicit || w->bar_sync_id != 0)) {
        if (with_configure) {
            ok = wayland_encode_bar_sync(w, w->geom_x, w->geom_y, true);
            synced_bar_resize = ok;
        } else {
            ok = wayland_encode_bar_sync(w, w->geom_x, w->geom_y, false);
        }
        if (ok) {
            ok = wl_commit_explicit(w, w->bar_surface_id, use_explicit ? w->bar_sync_id : 0u,
                                    w->bar_attach_buffer, bar_w, bar_h,
                                    w->bar_attach_acquire_tl, w->bar_attach_acquire_pt,
                                    w->bar_attach_release_tl, w->bar_attach_release_pt,
                                    &w->bar_opaque_id, &w->bar_opaque_w, &w->bar_opaque_h,
                                    &bar_fresh);
            if (ok) {
                sent_bar_buf = w->bar_attach_buffer;
            }
        }
    }
    if (ok && buffer_id != 0) {
        uint32_t attach_payload[3] = { buffer_id, 0u, 0u };
        bool buf_dmg = (w->compositor_bound_version >= 4u);
        uint16_t damage_opcode = buf_dmg ? 9u : 2u;
        bool sub_dmg = (dmg_w > 0u && dmg_h > 0u);
        int32_t damage_payload[4];
        if (buf_dmg) {
            damage_payload[0] = sub_dmg ? dmg_x : 0;
            damage_payload[1] = sub_dmg ? dmg_y : 0;
            damage_payload[2] = sub_dmg ? (int32_t)dmg_w : (int32_t)width;
            damage_payload[3] = sub_dmg ? (int32_t)dmg_h : (int32_t)height;
        } else {
            damage_payload[0] = 0;
            damage_payload[1] = 0;
            damage_payload[2] = (int32_t)log_w;
            damage_payload[3] = (int32_t)log_h;
        }
        ok = wl_opaque_prepare(w, w->surface_id, &w->scene_opaque_id, &w->scene_opaque_w,
                               &w->scene_opaque_h, log_w, log_h, &scene_fresh);
        if (ok && use_explicit) {
            uint32_t acquire_payload[3];
            uint32_t release_payload[3];
            wl_put_timeline_point(acquire_payload, acquire_timeline, acquire_point);
            wl_put_timeline_point(release_payload, release_timeline, release_point);
            ok = wayland_put_raw(w, w->explicit_sync_id, 1u, acquire_payload, sizeof acquire_payload) &&
                 wayland_put_raw(w, w->explicit_sync_id, 2u, release_payload, sizeof release_payload);
        }
        if (ok) {
            ok = wayland_put_raw(w, w->surface_id, 1u, attach_payload, sizeof attach_payload) &&
                 wayland_put_raw(w, w->surface_id, damage_opcode, damage_payload, sizeof damage_payload);
        }
        if (ok && w->viewport_id != 0 &&
            (with_configure || want_vw != w->scene_vp_w || want_vh != w->scene_vp_h)) {
            int32_t dest[2] = { want_vw, want_vh };
            ok = wayland_put_raw(w, w->viewport_id, 2u, dest, sizeof dest);
            wrote_vp = ok;
        }
        // wp_fifo_v1: wait on previous barrier (op 1) and arm next barrier (op 0) for paced commits.
        if (ok && !with_configure && !w->uncapped_commits && w->fifo_id != 0u) {
            ok = wayland_put_raw(w, w->fifo_id, 1u, nullptr, 0u) &&
                 wayland_put_raw(w, w->fifo_id, 0u, nullptr, 0u);
        }
        // wp_commit_timer_v1: constrain latch to CLOCK_MONOTONIC presentation deadline (op 0).
        if (ok && !with_configure && !w->uncapped_commits &&
            w->commit_timer_id != 0u &&
            w->has_presentation_clk && w->presentation_clk_id == 1u &&
            w->target_present_ns != 0ull) {
            uint64_t sec  = w->target_present_ns / 1000000000ull;
            uint32_t nsec = (uint32_t)(w->target_present_ns % 1000000000ull);
            uint32_t ts_payload[3] = {
                (uint32_t)(sec >> 32),
                (uint32_t)(sec & 0xffffffffu),
                nsec,
            };
            ok = wayland_put_raw(w, w->commit_timer_id, 0u, ts_payload, sizeof ts_payload);
        }
    }
    // Allocated after the regions in strict ascending new-id order.
    uint32_t pfb = 0;
    if (ok && buffer_id != 0 && w->presentation_id != 0u && w->presentation_fb_id == 0u) {
        pfb = wayland_alloc_id(w);
        if (pfb == 0) {
            ok = false;
        } else {
            uint32_t pfb_payload[2] = { w->surface_id, pfb };
            // wp_presentation.feedback is request 1.
            ok = wayland_put_raw(w, w->presentation_id, 1u, pfb_payload, sizeof pfb_payload);
        }
    }
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
    if (ok && synced_bar_resize) {
        ok = wayland_encode_bar_sync(w, w->geom_x, w->geom_y, false);
    }
    if (!ok) {
        w->bar_pos_x     = old_bx;
        w->bar_pos_y     = old_by;
        w->bar_is_synced = old_bsync;
        w->bar_vp_w      = old_bvw;
        w->bar_vp_h      = old_bvh;
        wayland_rewind_send(w, mark);
        wl_release_unsent(w, cb);
        wl_release_unsent(w, pfb);
        wl_release_unsent(w, scene_fresh);
        wl_release_unsent(w, bar_fresh);
        fprintf(stderr, "[Wayland Error] Queued commit encode failed: %s\n", w->err);
        return -EIO;
    }
    int queued = wayland_queue_send(w, did_ack);
    if (queued != 0) {
        w->bar_pos_x     = old_bx;
        w->bar_pos_y     = old_by;
        w->bar_is_synced = old_bsync;
        w->bar_vp_w      = old_bvw;
        w->bar_vp_h      = old_bvh;
        wayland_rewind_send(w, mark);
        wl_release_unsent(w, cb);
        wl_release_unsent(w, pfb);
        wl_release_unsent(w, scene_fresh);
        wl_release_unsent(w, bar_fresh);
        return queued;
    }
    wl_consume_deferred_destroys(w, prepended_destroys);
    if (buffer_id != 0u) {
        wayland_mark_buffer_busy(w, buffer_id);
    }
    if (sent_bar_buf != 0u) {
        wayland_mark_buffer_busy(w, sent_bar_buf);
    }
    uint32_t bar_log_w = (w->bar_logical_w != 0u) ? w->bar_logical_w : bar_w;
    uint32_t bar_log_h = (w->bar_logical_h != 0u) ? w->bar_logical_h : bar_h;
    wl_opaque_keep(&w->bar_opaque_id, &w->bar_opaque_w, &w->bar_opaque_h, bar_fresh, bar_log_w, bar_log_h);
    if (buffer_id != 0) {
        wl_opaque_keep(&w->scene_opaque_id, &w->scene_opaque_w, &w->scene_opaque_h,
                       scene_fresh, log_w, log_h);
        w->buffers_detached  = false;
        w->target_present_ns = 0ull;
        if (wrote_vp) {
            w->scene_vp_w = want_vw;
            w->scene_vp_h = want_vh;
        }
    }
    if (bar_fresh != 0 || w->bar_attach_buffer != 0) {
        w->bar_attach_buffer = 0;
    }
    if (pfb != 0) {
        w->presentation_fb_id = pfb;
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
    if (!w->failed) {
        (void)wl_flush_pending(w);
    }
    w->send_len = 0;
    w->send_off = 0;
    w->send_tracks_ack = false;

    if (!w->failed) {
        if (w->feedback_id != 0) {
            (void)wayland_put_raw(w, w->feedback_id, 0u, nullptr, 0u);
            w->retired_feedback_id = w->feedback_id;
            w->feedback_id         = 0;
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
        if (w->bar_viewport_id != 0) {
            (void)wayland_put_raw(w, w->bar_viewport_id, 0u, nullptr, 0u);
            w->bar_viewport_id = 0;
        }
        if (w->fractional_scale_id != 0) {
            (void)wayland_put_raw(w, w->fractional_scale_id, 0u, nullptr, 0u);
            w->fractional_scale_id = 0;
        }
        // wp_fifo_v1.destroy is opcode 2 (not 0!).
        if (w->fifo_id != 0) {
            (void)wayland_put_raw(w, w->fifo_id, 2u, nullptr, 0u);
            w->fifo_id = 0;
        }
        // wp_commit_timer_v1.destroy is opcode 1 (not 0!).
        if (w->commit_timer_id != 0) {
            (void)wayland_put_raw(w, w->commit_timer_id, 1u, nullptr, 0u);
            w->commit_timer_id = 0;
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
            wayland_untrack_buffer(w, w->buffer_id);
            (void)wayland_put_raw(w, w->buffer_id, 0u, nullptr, 0u);
            w->buffer_id = 0;
        }
        if (w->destroy_ids != nullptr) {
            for (uint32_t i = 0; i < w->destroy_later_n && !w->failed; ++i) {
                if (w->send_len + 256u > sizeof(w->send_bytes)) {
                    (void)wl_send_batch(w);
                }
                (void)wayland_put_raw(w, w->destroy_ids[i], 0u, nullptr, 0u);
            }
        }
        w->destroy_later_n = 0;
        if (w->send_len > 0 && !w->failed) {
            w->synced  = false;
            w->sync_id = wayland_alloc_id(w);
            if (w->sync_id != 0 && wl_append_display_sync(w, w->sync_id) &&
                wl_send_batch(w) == 0) {
                struct io_uring_cqe cqe = {0};
                while (!w->synced && !w->failed) {
                    if (wl_wait_tag(w, ENG_UD_WL_RECV, &cqe) < 0) {
                        break;
                    }
                    wayland_on_cqe(w, &cqe);
                }
            }
        }
        w->presentation_fb_id = 0;
    } else {
        w->feedback_id         = 0;
        w->decoration_id       = 0;
        w->scene_opaque_id     = 0;
        w->bar_opaque_id       = 0;
        w->viewport_id         = 0;
        w->bar_viewport_id     = 0;
        w->fractional_scale_id = 0;
        w->fifo_id             = 0;
        w->commit_timer_id     = 0;
        w->presentation_fb_id  = 0;
        w->bar_sync_id         = 0;
        w->bar_subsurface_id   = 0;
        w->bar_surface_id      = 0;
        w->explicit_sync_id    = 0;
        w->acquire_timeline_id = 0;
        w->release_timeline_id = 0;
        w->xdg_toplevel_id     = 0;
        w->xdg_surface_id      = 0;
        w->surface_id          = 0;
        w->buffer_id           = 0;
        w->destroy_later_n     = 0;
    }
    for (uint32_t i = 0u; i < ENG_WL_TRACKED_BUFS; ++i) {
        w->tracked_buf_ids[i]      = 0u;
        w->tracked_buf_released[i] = 0u;
    }
    wl_close_feedback_table(w);
}