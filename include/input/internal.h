#ifndef ENGINE_INPUT_INTERNAL_H
#define ENGINE_INPUT_INTERNAL_H

// Seat, pointer, keyboard, and the cursor request. The public names stay
// in pointer.h. The device page is internal until the frame records it.

#include "input/pointer.h"
#include <stdint.h>

struct xkb_context;
struct xkb_keymap;
struct xkb_state;
struct xkb_compose_table;
struct xkb_compose_state;

enum : uint32_t {
    k_spawn_cap  = 8u,
    k_key_cap    = 16u,
    k_held_cap   = 32u,
    k_utf8_cap   = 64u,
    k_utf8_bytes = 16u,
    k_clip_cap   = 1024u,
    k_dead_cap   = 8u,
};

// Single-threaded CPU seat state owned by the main io_uring event loop.
// input_page_publish snapshots these fields into the 4-slot GPU ring.
typedef struct InputCursor {
    uint32_t enter_serial;
    bool     have_enter;
    uint32_t focus;
    float    x;
    float    y;
    uint32_t buttons;
    uint32_t sample_ms;
    uint32_t wanted_shape;
    uint32_t last_shape;
    bool     have_last;
    bool     cursor_dirty;
    bool     frame_pending_retarget;
    bool     frame_entering;
    uint32_t edge;
    bool     close_hit;
    bool     close_pressed;
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
    bool     left_client_down;
    bool     left_dragged;
    float    left_down_x;
    float    left_down_y;
    bool     rel_frame_seen;
    bool     lock_wanted;
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
extern float                s_drag_dx;
extern float                s_drag_dy;
extern uint32_t             s_drag_buttons;
extern uint32_t             s_key_q[k_key_cap];
extern uint32_t             s_key_n;
extern uint32_t             s_held[k_held_cap];
extern uint32_t             s_held_n;
extern bool                 s_key_focus;
extern double               s_scroll;
extern bool                 s_seat_caps_dirty;
extern uint32_t             s_seat_caps_pending;

extern InputUtf8            s_utf8[k_utf8_cap];
extern uint32_t             s_utf8_n;
extern struct xkb_context       *s_xkb_ctx;
extern struct xkb_keymap        *s_xkb_map;
extern struct xkb_state         *s_xkb_state;
extern struct xkb_compose_table *s_xkb_compose_table;
extern struct xkb_compose_state *s_xkb_compose_state;
extern int32_t              s_repeat_rate;
extern int32_t              s_repeat_delay_ms;
extern bool                 s_repeat_armed;
extern bool                 s_repeat_tap;
extern uint32_t             s_repeat_key;
extern char                 s_repeat_bytes[k_utf8_bytes];
extern uint8_t              s_repeat_n;
extern uint64_t             s_repeat_next;
extern bool                 s_xkb_warned;
extern uint32_t             s_last_key_serial;

// Wayland clipboard (wl_data_device / wl_data_offer / wl_data_source) + async io_uring pipes
extern uint32_t             s_pending_offer_id;
extern uint8_t              s_pending_offer_prio;
extern char                 s_pending_offer_mime[32];
extern uint32_t             s_selection_offer_id;
extern uint8_t              s_selection_prio;
extern char                 s_selection_mime[32];
extern uint32_t             s_data_source_id;
extern uint32_t             s_dead_offers[k_dead_cap];
extern uint32_t             s_dead_offers_n;
extern uint32_t             s_dead_sources[k_dead_cap];
extern uint32_t             s_dead_sources_n;
extern uint32_t             s_retired_sources[k_dead_cap];
extern uint32_t             s_retired_sources_cursor;
extern bool                 s_clip_copy_pending;
extern char                 s_clip_tx_buf[k_clip_cap];
extern uint32_t             s_clip_tx_len;
extern char                 s_clip_write_buf[k_clip_cap];
extern uint32_t             s_clip_write_len;
extern uint32_t             s_clip_write_off;
extern bool                 s_clip_write_active;
extern char                 s_clip_rx_buf[k_clip_cap];
extern uint32_t             s_clip_rx_len;
extern char                 s_clip_rx_carry[4];
extern uint8_t              s_clip_rx_carry_n;
extern bool                 s_clip_rx_active;

[[nodiscard]] bool input_take_u32(const unsigned char **p, const unsigned char *end, uint32_t *o);
[[nodiscard]] bool input_take_i32(const unsigned char **p, const unsigned char *end, int32_t *o);

[[nodiscard]] bool input_on_pointer(WaylandContext *w, uint16_t opcode,
                                    const unsigned char *payload, uint16_t len);
[[nodiscard]] bool input_on_relative_pointer(WaylandContext *w, uint16_t opcode,
                                             const unsigned char *payload, uint16_t len);
[[nodiscard]] bool input_on_keyboard(WaylandContext *w, uint16_t opcode,
                                     const unsigned char *payload, uint16_t len);
[[nodiscard]] bool input_on_data_device(WaylandContext *w, uint16_t opcode,
                                        const unsigned char *payload, uint16_t len);
[[nodiscard]] bool input_on_data_offer(WaylandContext *w, uint32_t id, uint16_t opcode,
                                       const unsigned char *payload, uint16_t len);
[[nodiscard]] bool input_on_data_source(WaylandContext *w, uint32_t id, uint16_t opcode,
                                        const unsigned char *payload, uint16_t len);

[[nodiscard]] int cursor_make_fallback(WaylandContext *w);
void cursor_release_memory(void);

// Even sequence: the first line is stable. Odd: a publish is mid-write.
void input_page_publish(void);

#endif /* ENGINE_INPUT_INTERNAL_H */
