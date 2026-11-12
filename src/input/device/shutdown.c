#include "input/internal.h"
#include "wayland/internal.h"
#include <string.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

void pointer_shutdown(WaylandContext *w) {
    if (w != nullptr && w->is_initialized && !w->failed) {
        (void)wl_flush_pending(w);
        if (!w->failed) {
            if (s_ptr.cursor_surface != 0) {
                (void)wayland_put_raw(w, s_ptr.cursor_surface, 0u, nullptr, 0u);
                s_ptr.cursor_surface = 0;
            }
            if (s_ptr.cursor_buffer != 0) {
                (void)wayland_put_raw(w, s_ptr.cursor_buffer, 0u, nullptr, 0u);
                s_ptr.cursor_buffer = 0;
            }
            if (s_ptr.cursor_pool != 0) {
                (void)wayland_put_raw(w, s_ptr.cursor_pool, 1u, nullptr, 0u);
                s_ptr.cursor_pool = 0;
            }
            for (uint32_t i = 0; i < s_dead_offers_n; ++i) {
                (void)wayland_put_raw(w, s_dead_offers[i], 2u, nullptr, 0u);
            }
            if (s_pending_offer_id != 0 && s_pending_offer_id != s_selection_offer_id) {
                (void)wayland_put_raw(w, s_pending_offer_id, 2u, nullptr, 0u);
            }
            if (s_selection_offer_id != 0) {
                (void)wayland_put_raw(w, s_selection_offer_id, 2u, nullptr, 0u);
            }
            for (uint32_t i = 0; i < s_dead_sources_n; ++i) {
                (void)wayland_put_raw(w, s_dead_sources[i], 1u, nullptr, 0u);
            }
            if (s_data_source_id != 0) {
                (void)wayland_put_raw(w, s_data_source_id, 1u, nullptr, 0u);
            }
            if (w->data_device_id != 0) {
                if (w->data_device_manager_bound_version >= 2u) {
                    (void)wayland_put_raw(w, w->data_device_id, 2u, nullptr, 0u);
                }
                w->data_device_id = 0;
            }
            if (w->locked_pointer_id != 0) {
                (void)wayland_put_raw(w, w->locked_pointer_id, 0u, nullptr, 0u);
                w->locked_pointer_id = 0;
            }
            if (w->relative_pointer_id != 0) {
                (void)wayland_put_raw(w, w->relative_pointer_id, 0u, nullptr, 0u);
                w->relative_pointer_id = 0;
            }
            if (w->cursor_shape_device_id != 0) {
                (void)wayland_put_raw(w, w->cursor_shape_device_id, 0u, nullptr, 0u);
                w->cursor_shape_device_id = 0;
            }
            if (w->pointer_id != 0 && w->seat_bound_version >= 3u) {
                (void)wayland_put_raw(w, w->pointer_id, 1u, nullptr, 0u);
                w->pointer_id = 0;
            }
            if (w->keyboard_id != 0 && w->seat_bound_version >= 3u) {
                // wl_keyboard.release is request 0.
                (void)wayland_put_raw(w, w->keyboard_id, 0u, nullptr, 0u);
                w->keyboard_id = 0;
            }
            if (w->send_len > 0 && !w->failed) {
                (void)wayland_send_batch(w);
            }
        }
    }
    if (w != nullptr && w->ring != nullptr && w->ring->is_initialized) {
        if (s_clip_rx_active) {
            (void)ring_update_file(w->ring, ENG_CLIPBOARD_FD_SLOT, -1);
        }
        if (s_clip_write_active) {
            (void)ring_update_file(w->ring, ENG_CLIPBOARD_TX_FD_SLOT, -1);
        }
    }
    // The device page stays mapped until the GPU idle in frame_stop.
    // Clearing it here would race the dispatch still in the last submit.
    cursor_release_memory();
    s_ptr.cursor_surface = 0;
    s_ptr.cursor_pool = 0;
    s_ptr.cursor_buffer = 0;
    s_ptr.cursor_dirty = false;
    s_ptr.close_hit = false;
    s_ptr.close_pressed = false;
    s_ptr.frame_pending_retarget = false;
    s_ptr.frame_entering = false;
    s_ptr.resize_pending = false;
    s_ptr.move_pending = false;
    s_ptr.have_enter = false;
    s_ptr.focus = 0;
    s_ptr.buttons = 0;
    s_ptr.sample_ms = 0;
    s_ptr.rel_frame_seen = false;
    s_ptr.lock_wanted = false;
    s_drag_dx = 0.0f;
    s_drag_dy = 0.0f;
    s_drag_buttons = 0;
    s_spawn_n = 0;
    s_key_n = 0;
    s_held_n = 0;
    s_key_focus = false;
    s_scroll = 0.0;
    s_seat_caps_dirty = false;
    s_utf8_n = 0;
    s_repeat_armed = false;
    s_repeat_tap = false;
    s_last_key_serial = 0;
    s_pending_offer_id = 0;
    s_pending_offer_prio = 0;
    s_pending_offer_mime[0] = '\0';
    s_selection_offer_id = 0;
    s_selection_prio = 0;
    s_selection_mime[0] = '\0';
    s_data_source_id = 0;
    s_dead_offers_n = 0;
    s_dead_sources_n = 0;
    memset(s_retired_sources, 0, sizeof s_retired_sources);
    s_retired_sources_cursor = 0;
    s_clip_copy_pending = false;
    s_clip_tx_len = 0;
    s_clip_write_len = 0;
    s_clip_write_off = 0;
    s_clip_write_active = false;
    s_clip_rx_len = 0;
    s_clip_rx_carry_n = 0;
    s_clip_rx_active = false;
    if (s_xkb_compose_state != nullptr) {
        xkb_compose_state_unref(s_xkb_compose_state);
        s_xkb_compose_state = nullptr;
    }
    if (s_xkb_compose_table != nullptr) {
        xkb_compose_table_unref(s_xkb_compose_table);
        s_xkb_compose_table = nullptr;
    }
    if (s_xkb_state != nullptr) {
        xkb_state_unref(s_xkb_state);
        s_xkb_state = nullptr;
    }
    if (s_xkb_map != nullptr) {
        xkb_keymap_unref(s_xkb_map);
        s_xkb_map = nullptr;
    }
    if (s_xkb_ctx != nullptr) {
        xkb_context_unref(s_xkb_ctx);
        s_xkb_ctx = nullptr;
    }
}
