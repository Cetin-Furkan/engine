#include "input/internal.h"
#include "wayland/internal.h"
#include <errno.h>

static int encode_seat_hotplug(WaylandContext *w, bool *any_out) {
    if (!s_seat_caps_dirty || w->seat_id == 0) {
        s_seat_caps_dirty = false;
        return 0;
    }
    uint32_t caps = s_seat_caps_pending;
    uint32_t mark = w->send_len;
    uint32_t new_ptr = 0;
    uint32_t new_shape = 0;
    uint32_t new_rel = 0;
    uint32_t new_kbd = 0;
    bool wrote = false;

    if ((caps & 1u) == 0u && w->pointer_id != 0) {
        if (w->locked_pointer_id != 0) {
            if (!wayland_put_raw(w, w->locked_pointer_id, 0u, nullptr, 0u)) {
                wayland_rewind_send(w, mark);
                return -EIO;
            }
            wrote = true;
        }
        if (w->relative_pointer_id != 0) {
            if (!wayland_put_raw(w, w->relative_pointer_id, 0u, nullptr, 0u)) {
                wayland_rewind_send(w, mark);
                return -EIO;
            }
            wrote = true;
        }
        if (w->cursor_shape_device_id != 0) {
            if (!wayland_put_raw(w, w->cursor_shape_device_id, 0u, nullptr, 0u)) {
                wayland_rewind_send(w, mark);
                return -EIO;
            }
            wrote = true;
        }
        if (w->seat_bound_version >= 3u) {
            if (!wayland_put_raw(w, w->pointer_id, 1u, nullptr, 0u)) {
                wayland_rewind_send(w, mark);
                return -EIO;
            }
            wrote = true;
        }
    } else if ((caps & 1u) != 0u && w->pointer_id == 0) {
        new_ptr = wayland_alloc_id(w);
        if (new_ptr == 0 || !wayland_put_raw(w, w->seat_id, 0u, &new_ptr, sizeof new_ptr)) {
            wayland_rewind_send(w, mark);
            wl_release_unsent(w, new_ptr);
            return -EIO;
        }
        wrote = true;
        if (w->cursor_shape_manager_id != 0) {
            new_shape = wayland_alloc_id(w);
            uint32_t payload[2] = { new_shape, new_ptr };
            if (new_shape == 0 ||
                !wayland_put_raw(w, w->cursor_shape_manager_id, 1u, payload, sizeof payload)) {
                wayland_rewind_send(w, mark);
                wl_release_unsent(w, new_shape);
                wl_release_unsent(w, new_ptr);
                return -EIO;
            }
        }
        if (w->relative_pointer_manager_id != 0) {
            new_rel = wayland_alloc_id(w);
            uint32_t payload[2] = { new_rel, new_ptr };
            if (new_rel == 0 ||
                !wayland_put_raw(w, w->relative_pointer_manager_id, 1u, payload, sizeof payload)) {
                wayland_rewind_send(w, mark);
                wl_release_unsent(w, new_rel);
                wl_release_unsent(w, new_shape);
                wl_release_unsent(w, new_ptr);
                return -EIO;
            }
        }
    }

    if ((caps & 2u) == 0u && w->keyboard_id != 0) {
        if (w->seat_bound_version >= 3u) {
            if (!wayland_put_raw(w, w->keyboard_id, 0u, nullptr, 0u)) {
                wayland_rewind_send(w, mark);
                wl_release_unsent(w, new_rel);
                wl_release_unsent(w, new_shape);
                wl_release_unsent(w, new_ptr);
                return -EIO;
            }
            wrote = true;
        }
    } else if ((caps & 2u) != 0u && w->keyboard_id == 0) {
        new_kbd = wayland_alloc_id(w);
        if (new_kbd == 0 || !wayland_put_raw(w, w->seat_id, 1u, &new_kbd, sizeof new_kbd)) {
            wayland_rewind_send(w, mark);
            wl_release_unsent(w, new_kbd);
            wl_release_unsent(w, new_rel);
            wl_release_unsent(w, new_shape);
            wl_release_unsent(w, new_ptr);
            return -EIO;
        }
        wrote = true;
    }

    if ((caps & 1u) == 0u && w->pointer_id != 0) {
        w->locked_pointer_id = 0;
        w->relative_pointer_id = 0;
        w->cursor_shape_device_id = 0;
        w->pointer_id = 0;
        s_ptr.focus = 0;
        s_ptr.buttons = 0;
        s_ptr.have_enter = false;
        s_ptr.cursor_dirty = false;
        s_ptr.close_pressed = false;
        s_ptr.lock_wanted = false;
        s_ptr.rel_frame_seen = false;
    } else if (new_ptr != 0) {
        w->pointer_id = new_ptr;
        w->cursor_shape_device_id = new_shape;
        w->relative_pointer_id = new_rel;
    }

    if ((caps & 2u) == 0u && w->keyboard_id != 0) {
        w->keyboard_id = 0;
        s_key_focus = false;
        s_held_n = 0;
        s_repeat_armed = false;
        s_repeat_tap = false;
    } else if (new_kbd != 0) {
        w->keyboard_id = new_kbd;
    }

    w->seat_capabilities = caps;
    s_seat_caps_dirty = false;
    if (wrote) {
        *any_out = true;
    }
    return 0;
}

int pointer_encode(WaylandContext *w) {
    if (w == nullptr) {
        return -EINVAL;
    }
    if (w->send_len != 0 || wl_send_blocked(w)) {
        return -EBUSY;
    }
    uint32_t send_mark = w->send_len;
    bool any = false;
    bool sent_resize = false;
    bool sent_move   = false;
    if (s_seat_caps_dirty) {
        int hrc = encode_seat_hotplug(w, &any);
        if (hrc != 0) {
            return hrc;
        }
        if (any) {
            return 1;
        }
    }

    uint32_t new_lock       = 0;
    uint32_t new_src        = 0;
    uint32_t retired_src    = 0;
    bool     unlocked_ptr   = false;
    bool     consumed_copy  = false;

    for (uint32_t i = 0; i < s_dead_offers_n; ++i) {
        // wl_data_offer.destroy is opcode 2 (not 0!).
        if (!wayland_put_raw(w, s_dead_offers[i], 2u, nullptr, 0u)) {
            wayland_rewind_send(w, send_mark);
            return -EIO;
        }
        any = true;
    }
    for (uint32_t i = 0; i < s_dead_sources_n; ++i) {
        // wl_data_source.destroy is opcode 1 (not 0!).
        if (!wayland_put_raw(w, s_dead_sources[i], 1u, nullptr, 0u)) {
            wayland_rewind_send(w, send_mark);
            return -EIO;
        }
        any = true;
    }

    if (!s_ptr.lock_wanted && w->locked_pointer_id != 0) {
        // zwp_locked_pointer_v1.destroy is opcode 0.
        if (!wayland_put_raw(w, w->locked_pointer_id, 0u, nullptr, 0u)) {
            wayland_rewind_send(w, send_mark);
            return -EIO;
        }
        unlocked_ptr = true;
        any = true;
    } else if (s_ptr.lock_wanted && w->locked_pointer_id == 0 &&
               w->pointer_constraints_id != 0 && w->pointer_id != 0 &&
               w->surface_id != 0 && s_ptr.focus == w->surface_id) {
        new_lock = wayland_alloc_id(w);
        // zwp_pointer_constraints_v1.lock_pointer is opcode 1 (lifetime = 1 oneshot).
        uint32_t lock_payload[5] = { new_lock, w->surface_id, w->pointer_id, 0u, 1u };
        if (new_lock == 0 ||
            !wayland_put_raw(w, w->pointer_constraints_id, 1u, lock_payload, sizeof lock_payload)) {
            wayland_rewind_send(w, send_mark);
            wl_release_unsent(w, new_lock);
            return -EIO;
        }
        any = true;
    }

    if (s_clip_copy_pending) {
        if (w->data_device_manager_id != 0 && w->data_device_id != 0 &&
            s_clip_tx_len > 0u && s_last_key_serial != 0u) {
            if (s_data_source_id != 0u) {
                if (!wayland_put_raw(w, s_data_source_id, 1u, nullptr, 0u)) {
                    wayland_rewind_send(w, send_mark);
                    wl_release_unsent(w, new_lock);
                    return -EIO;
                }
                retired_src = s_data_source_id;
            }
            new_src = wayland_alloc_id(w);
            uint32_t sel_payload[2] = { new_src, s_last_key_serial };
            // wl_data_device_manager.create_data_source (0), wl_data_source.offer (0),
            // and wl_data_device.set_selection (1).
            if (new_src == 0 ||
                !wayland_put_raw(w, w->data_device_manager_id, 0u, &new_src, sizeof new_src) ||
                !wl_append_string(w, new_src, 0u, "text/plain;charset=utf-8") ||
                !wl_append_string(w, new_src, 0u, "text/plain") ||
                !wayland_put_raw(w, w->data_device_id, 1u, sel_payload, sizeof sel_payload)) {
                wayland_rewind_send(w, send_mark);
                wl_release_unsent(w, new_src);
                wl_release_unsent(w, new_lock);
                return -EIO;
            }
            any = true;
        }
        consumed_copy = true;
    }

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
            wl_release_unsent(w, new_src);
            wl_release_unsent(w, new_lock);
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
            wl_release_unsent(w, new_src);
            wl_release_unsent(w, new_lock);
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
            wl_release_unsent(w, new_src);
            wl_release_unsent(w, new_lock);
            s_ptr.cursor_inflight = false;
            return -EIO;
        }
        sent_move = true;
        any = true;
    }

    s_dead_offers_n  = 0;
    s_dead_sources_n = 0;
    if (unlocked_ptr) {
        w->locked_pointer_id = 0;
    } else if (new_lock != 0) {
        w->locked_pointer_id = new_lock;
    }
    if (retired_src != 0) {
        s_retired_sources[s_retired_sources_cursor & (k_dead_cap - 1u)] = retired_src;
        s_retired_sources_cursor++;
    }
    if (new_src != 0) {
        s_data_source_id = new_src;
    }
    if (consumed_copy) {
        s_clip_copy_pending = false;
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
