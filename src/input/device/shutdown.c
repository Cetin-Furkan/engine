#include "input/internal.h"
#include <xkbcommon/xkbcommon.h>

void pointer_shutdown(WaylandContext *w) {
    if (w != nullptr && w->is_initialized && !w->failed && !w->send_queued && !w->noted_send) {
        if (w->send_len > 0) {
            (void)wayland_send_batch(w);
        }
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
    // The device page stays mapped until the GPU idle in frame_stop.
    // Clearing it here would race the dispatch still in the last submit.
    cursor_release_memory();
    s_ptr.cursor_surface = 0;
    s_ptr.cursor_pool = 0;
    s_ptr.cursor_buffer = 0;
    s_ptr.cursor_dirty = false;
    s_ptr.frame_pending_retarget = false;
    s_ptr.frame_entering = false;
    s_ptr.resize_pending = false;
    s_ptr.move_pending = false;
    s_ptr.have_enter = false;
    s_ptr.focus = 0;
    s_ptr.buttons = 0;
    s_ptr.sample_ms = 0;
    s_drag_dx = 0.0f;
    s_drag_dy = 0.0f;
    s_drag_buttons = 0;
    s_spawn_n = 0;
    s_key_n = 0;
    s_key_focus = false;
    s_utf8_n = 0;
    s_repeat_armed = false;
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
