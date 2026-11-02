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
    w->surface_id             = wayland_alloc_id(w);
    w->xdg_surface_id         = wayland_alloc_id(w);
    w->xdg_toplevel_id        = wayland_alloc_id(w);
    w->toplevel_configured    = false;
    w->xdg_surface_configured = false;
    w->configure_width        = 0;
    w->configure_height       = 0;
    w->configure_serial       = 0;

    // 1. wl_compositor.create_surface (request 0)
    (void)wayland_put_raw(w, w->compositor_id, 0u, &w->surface_id, sizeof(w->surface_id));

    // 2. xdg_wm_base.get_xdg_surface (request 2)
    uint32_t get_xdg_payload[2] = { w->xdg_surface_id, w->surface_id };
    (void)wayland_put_raw(w, w->xdg_wm_base_id, 2u, get_xdg_payload, sizeof(get_xdg_payload));

    // 3. xdg_surface.get_toplevel (request 1)
    (void)wayland_put_raw(w, w->xdg_surface_id, 1u, &w->xdg_toplevel_id, sizeof(w->xdg_toplevel_id));

    // 4. xdg_toplevel.set_title (request 2) and set_app_id (request 3)
    (void)wl_append_string(w, w->xdg_toplevel_id, 2u, title != nullptr ? title : "Antigravity");
    (void)wl_append_string(w, w->xdg_toplevel_id, 3u, app_id != nullptr ? app_id : "antigravity");

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
    (void)wayland_put_raw(w, w->surface_id, 6u, nullptr, 0u);

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

    *out_width  = (w->configure_width > 0 && w->configure_height > 0) ? w->configure_width  : 1280u;
    *out_height = (w->configure_width > 0 && w->configure_height > 0) ? w->configure_height : 720u;
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
    if (w->send_len != 0) {
        return -EBUSY;
    }
    uint32_t mark = w->send_len;
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
    return wayland_queue_send(w, true);
}

