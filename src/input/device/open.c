#include "input/internal.h"
#include "chrome/spec.h"
#include <errno.h>
#include <stdio.h>

int pointer_open_devices(WaylandContext *w) {
    if (w == nullptr) {
        return -EINVAL;
    }
    if (w->seat_id == 0) {
        fprintf(stderr, "[Pointer Error] wl_seat is required so the cursor can be set\n");
        return -1;
    }
    if ((w->seat_capabilities & 1u) == 0u) {
        fprintf(stderr, "[Pointer Error] wl_seat has no pointer capability (mask=0x%x)\n",
                w->seat_capabilities);
        return -1;
    }
    w->pointer_id = wayland_alloc_id(w);
    if (w->pointer_id == 0 || !wayland_put_raw(w, w->seat_id, 0u, &w->pointer_id, sizeof w->pointer_id)) {
        return -1;
    }
    if ((w->seat_capabilities & 2u) != 0u) {
        w->keyboard_id = wayland_alloc_id(w);
        // wl_seat.get_keyboard is request 1.
        if (w->keyboard_id == 0 ||
            !wayland_put_raw(w, w->seat_id, 1u, &w->keyboard_id, sizeof w->keyboard_id)) {
            return -1;
        }
    } else {
        fprintf(stderr, "[Keyboard] wl_seat has no keyboard capability\n");
    }
    if (w->cursor_shape_manager_id != 0) {
        w->cursor_shape_device_id = wayland_alloc_id(w);
        uint32_t payload[2] = { w->cursor_shape_device_id, w->pointer_id };
        // wp_cursor_shape_manager_v1.get_pointer is request 1.
        if (w->cursor_shape_device_id == 0 ||
            !wayland_put_raw(w, w->cursor_shape_manager_id, 1u, payload, sizeof payload)) {
            return -1;
        }
    }
    if (wayland_roundtrip_batch(w, -1) < 0) {
        fprintf(stderr, "[Pointer Error] Pointer bind failed: %s\n", w->err);
        return -1;
    }
    if (w->cursor_shape_device_id == 0 && cursor_make_fallback(w) != 0) {
        return -1;
    }
    printf("[Pointer] seat=%u pointer=%u keyboard=%u cursor-shape=%u shm_argb=%s\n",
           w->seat_id,
           w->pointer_id,
           w->keyboard_id,
           w->cursor_shape_device_id,
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
