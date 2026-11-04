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
            memcpy(&w->seat_capabilities, payload, 4);
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
    return false;
}
