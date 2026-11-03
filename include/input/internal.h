#ifndef ENGINE_INPUT_INTERNAL_H
#define ENGINE_INPUT_INTERNAL_H

// Seat, pointer, keyboard, and the cursor request. The public names stay
// in pointer.h. The device page is internal until the frame records it.

#include "input/pointer.h"
#include <stdbool.h>
#include <stdint.h>

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

enum : uint32_t {
    k_spawn_cap  = 8u,
    k_key_cap    = 16u,
    k_held_cap   = 32u,
    k_utf8_cap   = 32u,
    k_utf8_bytes = 16u,
};

// focus, x, y, buttons, and sample_ms are atomic words. The io_uring thread
// is the only CPU writer; the GPU reads the device page. A publish copies
// each word with one atomic load so a 32-bit sample cannot tear.
typedef struct InputCursor {
    uint32_t         enter_serial;
    bool             have_enter;
    _Atomic uint32_t focus;
    _Atomic float    x;
    _Atomic float    y;
    _Atomic uint32_t buttons;
    _Atomic uint32_t sample_ms;
    uint32_t wanted_shape;
    uint32_t last_shape;
    bool     have_last;
    bool     cursor_dirty;
    bool     frame_pending_retarget;
    bool     frame_entering;
    uint32_t edge;
    bool     close_hit;
    bool     resize_pending;
    uint32_t resize_serial;
    uint32_t resize_edge;
    bool     title_hit;
    bool     move_pending;
    uint32_t move_serial;
    bool     cursor_inflight;
    uint32_t inflight_shape;
    uint32_t inflight_serial;
    uint32_t cursor_surface;
    uint32_t cursor_pool;
    uint32_t cursor_buffer;
    int      cursor_fd;
    void    *cursor_map;
    uint32_t cursor_bytes;
    // True after F_SEAL_WRITE sticks. The compositor then maps an immutable arrow.
    bool     cursor_sealed;
} InputCursor;

typedef struct InputSpawn {
    int32_t x;
    int32_t y;
} InputSpawn;

typedef struct InputUtf8 {
    char    bytes[k_utf8_bytes];
    uint8_t n;
} InputUtf8;

extern InputCursor          s_ptr;
extern InputSpawn           s_spawn[k_spawn_cap];
extern uint32_t             s_spawn_n;
// Same seat snapshot as InputCursor: one writer, tear-free loads in publish.
extern _Atomic float        s_drag_dx;
extern _Atomic float        s_drag_dy;
extern _Atomic uint32_t     s_drag_buttons;
extern uint32_t             s_key_q[k_key_cap];
extern uint32_t             s_key_n;
extern _Atomic uint32_t     s_held[k_held_cap];
extern _Atomic uint32_t     s_held_n;
extern _Atomic bool         s_key_focus;
extern _Atomic double       s_scroll;

static_assert(__atomic_always_lock_free(4, (const void *)0), "seat sample is one word");
static_assert(__atomic_always_lock_free(8, (const void *)0), "scroll sample is one word");
extern InputUtf8            s_utf8[k_utf8_cap];
extern uint32_t             s_utf8_n;
extern struct xkb_context  *s_xkb_ctx;
extern struct xkb_keymap   *s_xkb_map;
extern struct xkb_state    *s_xkb_state;
extern int32_t              s_repeat_rate;
extern int32_t              s_repeat_delay_ms;
extern bool                 s_repeat_armed;
extern uint32_t             s_repeat_key;
extern char                 s_repeat_bytes[k_utf8_bytes];
extern uint8_t              s_repeat_n;
extern uint64_t             s_repeat_next;
extern bool                 s_xkb_warned;

[[nodiscard]] bool input_take_u32(const unsigned char **p, const unsigned char *end, uint32_t *o);
[[nodiscard]] bool input_take_i32(const unsigned char **p, const unsigned char *end, int32_t *o);

[[nodiscard]] bool input_on_pointer(WaylandContext *w, uint16_t opcode,
                                    const unsigned char *payload, uint16_t len);
[[nodiscard]] bool input_on_keyboard(WaylandContext *w, uint16_t opcode,
                                     const unsigned char *payload, uint16_t len);

[[nodiscard]] int cursor_make_fallback(WaylandContext *w);
void cursor_release_memory(void);

// Even sequence: the first line is stable. Odd: a publish is mid-write.
void input_page_publish(void);

#endif /* ENGINE_INPUT_INTERNAL_H */
