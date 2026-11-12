#include "input/internal.h"
#include "wayland/internal.h"
#include "chrome/spec.h"
#include <errno.h>
#include <stdio.h>

int pointer_open_devices(WaylandContext *w) {
    if (w == nullptr) {
        return -EINVAL;
    }
    s_seat_caps_dirty   = false;
    s_seat_caps_pending = w->seat_capabilities;
    if (w->seat_id == 0) {
        fprintf(stderr, "[Pointer] No wl_seat offered by compositor; continuing without seat devices\n");
        return 0;
    }
    uint32_t mark     = w->send_len;
    uint32_t ptr_id   = 0;
    uint32_t kbd_id   = 0;
    uint32_t shape_id = 0;
    uint32_t rel_id   = 0;
    uint32_t dd_id    = 0;

    if ((w->seat_capabilities & 1u) != 0u) {
        ptr_id = wayland_alloc_id(w);
        if (ptr_id == 0 || !wayland_put_raw(w, w->seat_id, 0u, &ptr_id, sizeof ptr_id)) {
            wayland_rewind_send(w, mark);
            wl_release_unsent(w, ptr_id);
            return -1;
        }
        if (w->cursor_shape_manager_id != 0) {
            shape_id = wayland_alloc_id(w);
            uint32_t payload[2] = { shape_id, ptr_id };
            // wp_cursor_shape_manager_v1.get_pointer is request 1.
            if (shape_id == 0 ||
                !wayland_put_raw(w, w->cursor_shape_manager_id, 1u, payload, sizeof payload)) {
                wayland_rewind_send(w, mark);
                wl_release_unsent(w, shape_id);
                wl_release_unsent(w, ptr_id);
                return -1;
            }
        }
        if (w->relative_pointer_manager_id != 0) {
            rel_id = wayland_alloc_id(w);
            uint32_t payload[2] = { rel_id, ptr_id };
            // zwp_relative_pointer_manager_v1.get_relative_pointer is request 1.
            if (rel_id == 0 ||
                !wayland_put_raw(w, w->relative_pointer_manager_id, 1u, payload, sizeof payload)) {
                wayland_rewind_send(w, mark);
                wl_release_unsent(w, rel_id);
                wl_release_unsent(w, shape_id);
                wl_release_unsent(w, ptr_id);
                return -1;
            }
        }
    } else {
        fprintf(stderr, "[Pointer] wl_seat currently has no pointer capability (mask=0x%x); hotplug ready\n",
                w->seat_capabilities);
    }

    if ((w->seat_capabilities & 2u) != 0u) {
        kbd_id = wayland_alloc_id(w);
        // wl_seat.get_keyboard is request 1.
        if (kbd_id == 0 || !wayland_put_raw(w, w->seat_id, 1u, &kbd_id, sizeof kbd_id)) {
            wayland_rewind_send(w, mark);
            wl_release_unsent(w, kbd_id);
            wl_release_unsent(w, rel_id);
            wl_release_unsent(w, shape_id);
            wl_release_unsent(w, ptr_id);
            return -1;
        }
    } else {
        fprintf(stderr, "[Keyboard] wl_seat currently has no keyboard capability; hotplug ready\n");
    }

    if (w->data_device_manager_id != 0) {
        dd_id = wayland_alloc_id(w);
        uint32_t payload[2] = { dd_id, w->seat_id };
        // wl_data_device_manager.get_data_device is request 1.
        if (dd_id == 0 ||
            !wayland_put_raw(w, w->data_device_manager_id, 1u, payload, sizeof payload)) {
            wayland_rewind_send(w, mark);
            wl_release_unsent(w, dd_id);
            wl_release_unsent(w, kbd_id);
            wl_release_unsent(w, rel_id);
            wl_release_unsent(w, shape_id);
            wl_release_unsent(w, ptr_id);
            return -1;
        }
    }

    w->pointer_id             = ptr_id;
    w->keyboard_id            = kbd_id;
    w->cursor_shape_device_id = shape_id;
    w->relative_pointer_id    = rel_id;
    w->data_device_id         = dd_id;

    if (w->send_len > mark && wayland_roundtrip_batch(w, -1) < 0) {
        fprintf(stderr, "[Pointer Error] Seat device bind failed: %s\n", w->err);
        return -1;
    }
    if (w->pointer_id != 0 && w->cursor_shape_device_id == 0 && cursor_make_fallback(w) != 0) {
        return -1;
    }
    printf("[Pointer] seat=%u pointer=%u keyboard=%u cursor-shape=%u rel-ptr=%u ptr-lock=%s data-dev=%u shm_argb=%s\n",
           w->seat_id,
           w->pointer_id,
           w->keyboard_id,
           w->cursor_shape_device_id,
           w->relative_pointer_id,
           w->pointer_constraints_id != 0u ? "yes" : "no",
           w->data_device_id,
           w->shm_argb8888 ? "yes" : "no");
    return 0;
}

int pointer_check_spec(const WaylandContext *w) {
    const ChromeSpec *spec = chrome_spec();
    if (w == nullptr || spec == nullptr) {
        return -EINVAL;
    }
    if (w->cursor_shape_device_id == 0) {
        return 0;
    }
    uint32_t max_shape = (w->cursor_shape_bound_version >= 2u) ? 36u : 34u;
    if (spec->cursor_shape == 0 || spec->cursor_shape > max_shape) {
        fprintf(stderr, "[Pointer Error] cursor.shape %u is not in cursor-shape v%u\n",
                spec->cursor_shape, w->cursor_shape_bound_version);
        return -1;
    }
    for (uint32_t i = 0; i < spec->ncontrols; ++i) {
        if (spec->controls[i].has_cursor &&
            (spec->controls[i].cursor_shape == 0 || spec->controls[i].cursor_shape > max_shape)) {
            fprintf(stderr, "[Pointer Error] Control '%s' cursor %u is not in cursor-shape v%u\n",
                    spec->controls[i].name, spec->controls[i].cursor_shape,
                    w->cursor_shape_bound_version);
            return -1;
        }
    }
    return 0;
}
