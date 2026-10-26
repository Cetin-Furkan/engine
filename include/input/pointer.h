#ifndef ENGINE_INPUT_POINTER_H
#define ENGINE_INPUT_POINTER_H

#include "wayland/wayland.h"
#include <stdint.h>

/**
 * @brief Called from the Wayland parser for seat, pointer, and shm events.
 * 
 * Returns true when the object belongs to this module.
 */
[[nodiscard]] bool pointer_dispatch(
    WaylandContext       *w,
    uint32_t              object,
    uint16_t              opcode,
    const unsigned char  *payload,
    uint16_t              len
);

/**
 * @brief After the bind roundtrip: get_pointer, and a cursor-shape device when
 * the compositor offers one. Otherwise a client cursor surface.
 */
[[nodiscard]] int pointer_open_devices(WaylandContext *w);

/**
 * @brief Encodes a pending cursor or resize request into the Wayland send buffer.
 * 
 * Returns 1 when bytes were written, 0 when there is nothing to send, and
 * a negative errno when the buffer is busy or the encode failed.
 */
[[nodiscard]] int pointer_encode(WaylandContext *w);

/**
 * @brief Blocking send of anything pointer_encode would queue. Used before idle.
 */
[[nodiscard]] int pointer_flush(WaylandContext *w);

/**
 * @brief Rejects a spec cursor the bound wp_cursor_shape_device_v1 cannot express.
 */
[[nodiscard]] int pointer_check_spec(const WaylandContext *w);

void pointer_on_send_done(bool ok);
void pointer_shutdown(WaylandContext *w);

/**
 * @brief Takes one left click on empty client space, in surface pixels.
 * Empty means the click was not the close control, the title bar, or a
 * resize edge. Returns false when none are waiting.
 */
bool pointer_take_spawn(int32_t *x, int32_t *y);

// One key press while this surface has keyboard focus. The code is an
// evdev keycode. Camera keys, shift, and shift+4 are not queued here.
// F, M, L, Home, and numpad 1 are taps. Release and repeat are not
// reported. Returns false when none are waiting.
bool pointer_take_key(uint32_t *key);

// True while this surface has focus and the key is down. Used for held
// camera motion. A leave or a new enter clears every held key.
bool pointer_key_down(uint32_t key);

#endif /* ENGINE_INPUT_POINTER_H */