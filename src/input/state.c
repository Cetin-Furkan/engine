#include "input/internal.h"

InputCursor s_ptr = {
    .cursor_fd = -1,
};

InputSpawn s_spawn[k_spawn_cap];
uint32_t   s_spawn_n = 0;
_Atomic float    s_drag_dx = 0.0f;
_Atomic float    s_drag_dy = 0.0f;
_Atomic uint32_t s_drag_buttons = 0;

uint32_t s_key_q[k_key_cap];
uint32_t s_key_n = 0;
_Atomic uint32_t s_held[k_held_cap];
_Atomic uint32_t s_held_n = 0;
_Atomic bool     s_key_focus = false;
_Atomic double   s_scroll = 0.0;

InputUtf8 s_utf8[k_utf8_cap];
uint32_t  s_utf8_n = 0;

struct xkb_context *s_xkb_ctx = nullptr;
struct xkb_keymap  *s_xkb_map = nullptr;
struct xkb_state   *s_xkb_state = nullptr;

int32_t  s_repeat_rate = 40;
int32_t  s_repeat_delay_ms = 400;
bool     s_repeat_armed = false;
uint32_t s_repeat_key = 0;
char     s_repeat_bytes[k_utf8_bytes];
uint8_t  s_repeat_n = 0;
uint64_t s_repeat_next = 0;
bool     s_xkb_warned = false;
