#include "input/internal.h"
#include <string.h>

bool pointer_dispatch(
    WaylandContext      *w,
    uint32_t             object,
    uint16_t             opcode,
    const unsigned char *payload,
    uint16_t             len
) {
    if (w == nullptr) {
        return false;
    }
    if (w->shm_id != 0 && object == w->shm_id) {
        if (opcode == 0u && len >= 4u) {
            uint32_t format = 0;
            memcpy(&format, payload, 4);
            if (format == 0u) {
                w->shm_argb8888 = true;
            }
        }
        return true;
    }
    if (w->seat_id != 0 && object == w->seat_id) {
        if (opcode == 0u && len >= 4u) {
            uint32_t caps = 0;
            memcpy(&caps, payload, 4);
            if (!w->is_initialized) {
                w->seat_capabilities = caps;
                s_seat_caps_pending  = caps;
            } else {
                s_seat_caps_pending = caps;
                s_seat_caps_dirty   = (caps != w->seat_capabilities);
            }
        }
        return true;
    }
    if (w->pointer_id != 0 && object == w->pointer_id) {
        bool handled = input_on_pointer(w, opcode, payload, len);
        // wl_pointer.frame (opcode 5, seat v5) closes the group. A seat
        // older than 5 has no frame event, so each event is the publish.
        if (opcode == 5u || w->seat_bound_version < 5u) {
            input_page_publish();
        }
        return handled;
    }
    if (w->keyboard_id != 0 && object == w->keyboard_id) {
        bool handled = input_on_keyboard(w, opcode, payload, len);
        input_page_publish();
        return handled;
    }
    if (w->relative_pointer_id != 0 && object == w->relative_pointer_id) {
        bool handled = input_on_relative_pointer(w, opcode, payload, len);
        if (w->seat_bound_version < 5u) {
            input_page_publish();
        }
        return handled;
    }
    if (w->locked_pointer_id != 0 && object == w->locked_pointer_id) {
        // Oneshot zwp_locked_pointer_v1 becomes permanently inert on unlocked (op 1).
        if (opcode == 1u) {
            (void)wayland_defer_destroy(w, w->locked_pointer_id);
            w->locked_pointer_id = 0;
        }
        return true;
    }
    if (w->data_device_id != 0 && object == w->data_device_id) {
        return input_on_data_device(w, opcode, payload, len);
    }
    if (object != 0u) {
        if (object == s_pending_offer_id || object == s_selection_offer_id) {
            return input_on_data_offer(w, object, opcode, payload, len);
        }
        for (uint32_t i = 0; i < s_dead_offers_n; ++i) {
            if (s_dead_offers[i] == object) {
                return true;
            }
        }
        if (object == s_data_source_id) {
            return input_on_data_source(w, object, opcode, payload, len);
        }
        for (uint32_t i = 0; i < s_dead_sources_n; ++i) {
            if (s_dead_sources[i] == object) {
                return input_on_data_source(w, object, opcode, payload, len);
            }
        }
        for (uint32_t i = 0; i < k_dead_cap; ++i) {
            if (s_retired_sources[i] == object) {
                return input_on_data_source(w, object, opcode, payload, len);
            }
        }
    }
    return false;
}
