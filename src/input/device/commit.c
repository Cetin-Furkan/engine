#include "input/internal.h"
#include <errno.h>

int pointer_encode(WaylandContext *w) {
    if (w == nullptr) {
        return -EINVAL;
    }
    if (w->send_len != 0) {
        return -EBUSY;
    }
    uint32_t send_mark = w->send_len;
    bool any = false;
    bool sent_resize = false;
    bool sent_move   = false;
    if (s_ptr.cursor_dirty && s_ptr.have_enter && s_ptr.focus != 0) {
        bool wrote = false;
        if (w->cursor_shape_device_id != 0) {
            uint32_t payload[2] = { s_ptr.enter_serial, s_ptr.wanted_shape };
            // wp_cursor_shape_device_v1.set_shape is request 1.
            wrote = wayland_put_raw(w, w->cursor_shape_device_id, 1u, payload, sizeof payload);
        } else if (s_ptr.cursor_surface != 0 && w->pointer_id != 0) {
            uint32_t payload[4] = { s_ptr.enter_serial, s_ptr.cursor_surface, 0u, 0u };
            // wl_pointer.set_cursor is request 0. Hotspot is the arrow tip.
            wrote = wayland_put_raw(w, w->pointer_id, 0u, payload, sizeof payload);
        }
        if (!wrote) {
            wayland_rewind_send(w, send_mark);
            s_ptr.cursor_inflight = false;
            return -EIO;
        }
        s_ptr.cursor_inflight = true;
        s_ptr.inflight_shape  = s_ptr.wanted_shape;
        s_ptr.inflight_serial = s_ptr.enter_serial;
        any = true;
    }
    if (s_ptr.resize_pending && w->xdg_toplevel_id != 0 && w->seat_id != 0) {
        uint32_t payload[3] = { w->seat_id, s_ptr.resize_serial, s_ptr.resize_edge };
        // xdg_toplevel.resize is request 6.
        if (!wayland_put_raw(w, w->xdg_toplevel_id, 6u, payload, sizeof payload)) {
            wayland_rewind_send(w, send_mark);
            s_ptr.cursor_inflight = false;
            return -EIO;
        }
        sent_resize = true;
        any = true;
    }
    if (s_ptr.move_pending && w->xdg_toplevel_id != 0 && w->seat_id != 0) {
        uint32_t payload[2] = { w->seat_id, s_ptr.move_serial };
        // xdg_toplevel.move is request 5.
        if (!wayland_put_raw(w, w->xdg_toplevel_id, 5u, payload, sizeof payload)) {
            wayland_rewind_send(w, send_mark);
            s_ptr.cursor_inflight = false;
            return -EIO;
        }
        sent_move = true;
        any = true;
    }
    if (sent_resize) {
        s_ptr.resize_pending = false;
    }
    if (sent_move) {
        s_ptr.move_pending = false;
    }
    return any ? 1 : 0;
}

void pointer_on_send_done(bool ok) {
    // Success means the cursor shape, resize, or move reached the wire.
    // Encode already dropped the pending bits, so the page stays at the
    // button sample until this completion. A failed batch that was holding
    // a cursor still publishes, or the inflight bit would stick.
    bool publish = ok;
    if (s_ptr.cursor_inflight) {
        if (ok) {
            s_ptr.last_shape = s_ptr.inflight_shape;
            s_ptr.have_last = true;
            if (s_ptr.wanted_shape == s_ptr.last_shape && s_ptr.enter_serial == s_ptr.inflight_serial) {
                s_ptr.cursor_dirty = false;
            }
        }
        s_ptr.cursor_inflight = false;
        publish = true;
    }
    if (publish) {
        input_page_publish();
    }
}

int pointer_flush(WaylandContext *w) {
    int enc = pointer_encode(w);
    if (enc <= 0) {
        return enc;
    }
    if (wayland_send_batch(w) < 0) {
        pointer_on_send_done(false);
        return -1;
    }
    pointer_on_send_done(true);
    return 0;
}
