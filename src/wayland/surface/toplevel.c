// xdg_toplevel, window geometry, and configure acks.
#include "wayland/internal.h"
#include <errno.h>
#include <stdio.h>


bool wl_put_min_size(WaylandContext *w, int32_t width, int32_t height) {
    if (w->xdg_toplevel_id == 0 || width <= 0 || height <= 0) {
        return false;
    }
    int32_t sz[2] = { width, height };
    // xdg_toplevel.set_min_size is request 8. A zero argument means no bound.
    return wayland_put_raw(w, w->xdg_toplevel_id, 8u, sz, sizeof sz);
}


int wayland_create_toplevel_and_configure(
    WaylandContext *w,
    const char     *title,
    const char     *app_id,
    uint32_t       *out_width,
    uint32_t       *out_height,
    uint32_t       *out_serial
) {
    if (w == nullptr || !w->is_initialized || out_width == nullptr ||
        out_height == nullptr || out_serial == nullptr) {
        return -EINVAL;
    }
    if (wl_flush_pending(w) < 0) {
        return -EIO;
    }
    uint32_t mark       = w->send_len;
    uint32_t surface_id = wayland_alloc_id(w);
    uint32_t xdg_surf   = wayland_alloc_id(w);
    uint32_t xdg_top    = wayland_alloc_id(w);
    if (surface_id == 0 || xdg_surf == 0 || xdg_top == 0) {
        wl_release_unsent(w, xdg_top);
        wl_release_unsent(w, xdg_surf);
        wl_release_unsent(w, surface_id);
        return -EIO;
    }
    w->surface_id             = surface_id;
    w->xdg_surface_id         = xdg_surf;
    w->xdg_toplevel_id        = xdg_top;
    w->toplevel_configured    = false;
    w->xdg_surface_configured = false;
    w->has_staged_toplevel    = false;
    w->configure_width        = 0;
    w->configure_height       = 0;
    w->configure_serial       = 0;
    w->configure_bounds_w     = 0;
    w->configure_bounds_h     = 0;
    w->scene_vp_w             = -1;
    w->scene_vp_h             = -1;

    // 1-4. create_surface, get_xdg_surface, get_toplevel, set_title, set_app_id
    uint32_t get_xdg_payload[2] = { w->xdg_surface_id, w->surface_id };
    bool ok = wayland_put_raw(w, w->compositor_id, 0u, &w->surface_id, sizeof(w->surface_id)) &&
              wayland_put_raw(w, w->xdg_wm_base_id, 2u, get_xdg_payload, sizeof(get_xdg_payload)) &&
              wayland_put_raw(w, w->xdg_surface_id, 1u, &w->xdg_toplevel_id, sizeof(w->xdg_toplevel_id)) &&
              wl_append_string(w, w->xdg_toplevel_id, 2u, title != nullptr ? title : "Engine") &&
              wl_append_string(w, w->xdg_toplevel_id, 3u, app_id != nullptr ? app_id : "engine");
    if (!ok) {
        wayland_rewind_send(w, mark);
        wl_release_unsent(w, xdg_top);
        wl_release_unsent(w, xdg_surf);
        wl_release_unsent(w, surface_id);
        w->xdg_toplevel_id = 0;
        w->xdg_surface_id  = 0;
        w->surface_id      = 0;
        fprintf(stderr, "[Wayland Error] Toplevel creation encode failed: %s\n", w->err);
        return -EIO;
    }

    if (w->viewporter_id != 0) {
        uint32_t viewport = wayland_alloc_id(w);
        uint32_t viewport_payload[2] = { viewport, w->surface_id };
        // wp_viewporter.get_viewport is request 1.
        if (viewport == 0 ||
            !wayland_put_raw(w, w->viewporter_id, 1u, viewport_payload, sizeof viewport_payload)) {
            wl_release_unsent(w, viewport);
            w->viewport_id = 0;
        } else {
            w->viewport_id = viewport;
        }
    }

    if (w->fractional_scale_manager_id != 0) {
        uint32_t fs_id = wayland_alloc_id(w);
        uint32_t fs_payload[2] = { fs_id, w->surface_id };
        // wp_fractional_scale_manager_v1.get_fractional_scale is request 1.
        if (fs_id == 0 ||
            !wayland_put_raw(w, w->fractional_scale_manager_id, 1u, fs_payload, sizeof fs_payload)) {
            wl_release_unsent(w, fs_id);
            w->fractional_scale_id = 0;
        } else {
            w->fractional_scale_id = fs_id;
        }
    }

    if (w->fifo_manager_id != 0) {
        uint32_t fifo_id = wayland_alloc_id(w);
        uint32_t fifo_payload[2] = { fifo_id, w->surface_id };
        // wp_fifo_manager_v1.get_fifo is request 1.
        if (fifo_id == 0 ||
            !wayland_put_raw(w, w->fifo_manager_id, 1u, fifo_payload, sizeof fifo_payload)) {
            wl_release_unsent(w, fifo_id);
            w->fifo_id = 0;
        } else {
            w->fifo_id = fifo_id;
        }
    }

    if (w->commit_timing_manager_id != 0) {
        uint32_t timer_id = wayland_alloc_id(w);
        uint32_t timer_payload[2] = { timer_id, w->surface_id };
        // wp_commit_timing_manager_v1.get_timer is request 1.
        if (timer_id == 0 ||
            !wayland_put_raw(w, w->commit_timing_manager_id, 1u, timer_payload, sizeof timer_payload)) {
            wl_release_unsent(w, timer_id);
            w->commit_timer_id = 0;
        } else {
            w->commit_timer_id = timer_id;
        }
    }

    if (w->min_width > 0 && w->min_height > 0 &&
        !wl_put_min_size(w, w->min_width, w->min_height)) {
        fprintf(stderr, "[Wayland Error] Toplevel minimum encode failed: %s\n", w->err);
        return -EIO;
    }

    // Decoration goes in this batch, before the first commit, so a v1 compositor
    // sees the client-side request before any buffer.
    if (!wl_request_client_decoration(w)) {
        fprintf(stderr, "[Wayland Error] Client-side decoration encode failed: %s\n", w->err);
        return -EIO;
    }

    // 5. wl_surface.commit (request 6) with empty body
    if (!wayland_put_raw(w, w->surface_id, 6u, nullptr, 0u)) {
        fprintf(stderr, "[Wayland Error] Initial toplevel commit encode failed: %s\n", w->err);
        return -EIO;
    }

    if (wl_send_batch(w) < 0) {
        fprintf(stderr, "[Wayland Error] Failed to send toplevel creation batch: %s\n", w->err);
        return -EIO;
    }

    struct io_uring_cqe cqe = {0};
    bool want_decor = w->decoration_id != 0;
    while ((!w->toplevel_configured || !w->xdg_surface_configured ||
            (want_decor && !w->decoration_configured)) && !w->failed) {
        if (wl_wait_tag(w, ENG_UD_WL_RECV, &cqe) < 0) {
            break;
        }
        wayland_on_cqe(w, &cqe);
    }
    if (w->failed || !w->toplevel_configured || !w->xdg_surface_configured ||
        (want_decor && !w->decoration_configured)) {
        fprintf(stderr, "[Wayland Error] Toplevel configure wait failed: %s\n", w->err);
        return -EIO;
    }
    if (w->decoration_manager_id == 0) {
        printf("[Wayland] Decoration manager is absent. Server chrome is not negotiated.\n");
    } else if (w->decoration_mode == ENG_DECOR_CLIENT) {
        printf("[Wayland] Decoration mode client-side (object %u). Only the engine frame is used.\n",
               w->decoration_id);
    } else if (w->decoration_mode == ENG_DECOR_SERVER) {
        printf("[Wayland] Decoration mode server-side (object %u). The compositor refused client-side.\n",
               w->decoration_id);
    } else {
        printf("[Wayland] Decoration object %u mode %u.\n", w->decoration_id, w->decoration_mode);
    }

    if (w->ring->sq_pending > 0) {
        unsigned n = ring_submit(w->ring);
        (void)ring_enter(w->ring, n, 0, -1);
    }

    uint32_t def_w = 1280u;
    uint32_t def_h = 720u;
    if (w->configure_bounds_w > 0u && def_w > w->configure_bounds_w) {
        def_w = w->configure_bounds_w;
    }
    if (w->configure_bounds_h > 0u && def_h > w->configure_bounds_h) {
        def_h = w->configure_bounds_h;
    }
    w->floating_width  = def_w;
    w->floating_height = def_h;

    uint32_t chosen_w = (w->configure_width > 0u && w->configure_height > 0u) ? w->configure_width  : def_w;
    uint32_t chosen_h = (w->configure_width > 0u && w->configure_height > 0u) ? w->configure_height : def_h;
    constexpr uint32_t k_constrained =
        ENG_TOP_MAXIMIZED | ENG_TOP_FULLSCREEN |
        ENG_TOP_TILED_LEFT | ENG_TOP_TILED_RIGHT |
        ENG_TOP_TILED_TOP | ENG_TOP_TILED_BOTTOM;
    if ((w->toplevel_states & k_constrained) == 0u) {
        w->floating_width  = chosen_w;
        w->floating_height = chosen_h;
    }
    *out_width  = chosen_w;
    *out_height = chosen_h;
    *out_serial = w->configure_serial;
    return 0;
}


void wl_geom_box(const WaylandContext *w, uint32_t width, uint32_t height, int32_t out[4]) {
    if (w->geom_w != 0 && w->geom_h != 0) {
        out[0] = w->geom_x;
        out[1] = w->geom_y;
        out[2] = (int32_t)w->geom_w;
        out[3] = (int32_t)w->geom_h;
        return;
    }
    out[0] = 0;
    out[1] = 0;
    out[2] = (int32_t)width;
    out[3] = (int32_t)height;
}


void wayland_set_window_geom(WaylandContext *w, int32_t x, int32_t y,
                             uint32_t width, uint32_t height) {
    if (w == nullptr) {
        return;
    }
    w->geom_x = x;
    w->geom_y = y;
    w->geom_w = width;
    w->geom_h = height;
    w->bar_px = y < 0 ? (uint32_t)(-y) : 0u;
}


int wayland_queue_configure_ack(
    WaylandContext *w,
    uint32_t        serial,
    uint32_t        width,
    uint32_t        height,
    int32_t         viewport_mode
) {
    if (w == nullptr || !w->is_initialized || w->failed ||
        w->surface_id == 0 || w->xdg_surface_id == 0 || width == 0 || height == 0) {
        return -EINVAL;
    }
    if (w->send_len != 0 || wl_send_blocked(w)) {
        return -EBUSY;
    }
    uint32_t mark               = w->send_len;
    uint32_t prepended_destroys = wl_encode_deferred_destroys(w);
    int32_t geom[4];
    wl_geom_box(w, width, height, geom);
    if (!wayland_put_raw(w, w->xdg_surface_id, 4u, &serial, sizeof serial) ||
        !wayland_put_raw(w, w->xdg_surface_id, 3u, geom, sizeof geom)) {
        wayland_rewind_send(w, mark);
        return -EIO;
    }
    if (w->viewport_id != 0 && viewport_mode == ENG_VIEWPORT_UNSET) {
        int32_t dest[2] = { -1, -1 };
        if (!wayland_put_raw(w, w->viewport_id, 2u, dest, sizeof dest)) {
            wayland_rewind_send(w, mark);
            return -EIO;
        }
    }
    if (!wayland_put_raw(w, w->surface_id, 6u, nullptr, 0u)) {
        wayland_rewind_send(w, mark);
        return -EIO;
    }
    int queued = wayland_queue_send(w, true);
    if (queued == 0) {
        wl_consume_deferred_destroys(w, prepended_destroys);
        if (w->viewport_id != 0 && viewport_mode == ENG_VIEWPORT_UNSET) {
            w->scene_vp_w = -1;
            w->scene_vp_h = -1;
        }
    } else {
        wayland_rewind_send(w, mark);
    }
    return queued;
}

