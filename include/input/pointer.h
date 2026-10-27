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
// evdev keycode. route_key on the hosted program says whether the press
// is ignored, held for polling, or queued as one tap. Shift is always
// held and never queued. Release and repeat are not reported. When
// route_key is null, the built-in camera set is held and the built-in
// tap set is queued. Returns false when none are waiting.
bool pointer_take_key(uint32_t *key);

// Vertical wheel or touchpad scroll since the last take, in notches.
// One notch is about ten wl_fixed surface units. Positive steps zoom in
// (the finger moved up). Horizontal motion is ignored. The discrete
// value120 event is not added; the continuous axis already carries it.
// Returns false when nothing moved.
bool pointer_take_scroll(float *steps);

// True while this surface has focus and the key is down. Used for held
// camera motion. A leave or a new enter clears every held key.
bool pointer_key_down(uint32_t key);

// Last pointer position in surface pixels, top-left origin. False before
// the pointer has entered this surface.
bool pointer_client_xy(int32_t *x, int32_t *y);

// Queues a repeated UTF-8 string when the compositor delay has elapsed.
// Repeat does not queue the evdev key again. The clock is CLOCK_MONOTONIC.
void pointer_pump_text(void);

// One UTF-8 string from a tap or a repeat. `n` is the byte count, not
// including the NUL that is written when cap allows it. False when empty.
bool pointer_take_text(char *utf8, uint32_t cap, uint32_t *n);

#endif /* ENGINE_INPUT_POINTER_H */