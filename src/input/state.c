#include "input/internal.h"

InputCursor s_ptr = {
    .cursor_fd = -1,
};

InputSpawn s_spawn[k_spawn_cap];
uint32_t   s_spawn_n = 0;
float      s_drag_dx = 0.0f;
float      s_drag_dy = 0.0f;
uint32_t   s_drag_buttons = 0;

uint32_t s_key_q[k_key_cap];
uint32_t s_key_n = 0;
uint32_t s_held[k_held_cap];
uint32_t s_held_n = 0;
bool     s_key_focus = false;
double   s_scroll = 0.0;
bool     s_seat_caps_dirty = false;
uint32_t s_seat_caps_pending = 0;

InputUtf8 s_utf8[k_utf8_cap];
uint32_t  s_utf8_n = 0;

struct xkb_context       *s_xkb_ctx = nullptr;
struct xkb_keymap        *s_xkb_map = nullptr;
struct xkb_state         *s_xkb_state = nullptr;
struct xkb_compose_table *s_xkb_compose_table = nullptr;
struct xkb_compose_state *s_xkb_compose_state = nullptr;

int32_t  s_repeat_rate = 40;
int32_t  s_repeat_delay_ms = 400;
bool     s_repeat_armed = false;
bool     s_repeat_tap = false;
uint32_t s_repeat_key = 0;
char     s_repeat_bytes[k_utf8_bytes];
uint8_t  s_repeat_n = 0;
uint64_t s_repeat_next = 0;
bool     s_xkb_warned = false;
uint32_t s_last_key_serial = 0;

uint32_t s_pending_offer_id = 0;
uint8_t  s_pending_offer_prio = 0;
char     s_pending_offer_mime[32];
uint32_t s_selection_offer_id = 0;
uint8_t  s_selection_prio = 0;
char     s_selection_mime[32];
uint32_t s_data_source_id = 0;
uint32_t s_dead_offers[k_dead_cap];
uint32_t s_dead_offers_n = 0;
uint32_t s_dead_sources[k_dead_cap];
uint32_t s_dead_sources_n = 0;
uint32_t s_retired_sources[k_dead_cap];
uint32_t s_retired_sources_cursor = 0;
bool     s_clip_copy_pending = false;
char     s_clip_tx_buf[k_clip_cap];
uint32_t s_clip_tx_len = 0;
char     s_clip_write_buf[k_clip_cap];
uint32_t s_clip_write_len = 0;
uint32_t s_clip_write_off = 0;
bool     s_clip_write_active = false;
char     s_clip_rx_buf[k_clip_cap];
uint32_t s_clip_rx_len = 0;
char     s_clip_rx_carry[4];
uint8_t  s_clip_rx_carry_n = 0;
bool     s_clip_rx_active = false;
