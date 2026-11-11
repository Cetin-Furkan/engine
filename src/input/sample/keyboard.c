#include "input/internal.h"
#include "engine/app.h"
#include "ring/ring.h"
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

static bool shift_key(uint32_t key) {
    return key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT;
}

static bool shift_held(void) {
    for (uint32_t i = 0; i < s_held_n; ++i) {
        if (shift_key(s_held[i])) {
            return true;
        }
    }
    return false;
}

// KEY_4 is the cylinder tap unless shift is already down. On a Turkish Q
// keyboard that chord is plus. Numpad plus and minus are their own keys.
static bool motion_key(uint32_t key) {
    return key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN ||
           key == KEY_Q || key == KEY_E || key == KEY_C || key == KEY_V ||
           key == KEY_MINUS || key == KEY_EQUAL || key == KEY_KPMINUS || key == KEY_KPPLUS;
}

static bool tap_key(uint32_t key) {
    return key == KEY_ESC || key == KEY_BACKSPACE || key == KEY_F5 ||
           key == KEY_R || key == KEY_S || key == KEY_G ||
           key == KEY_F || key == KEY_M || key == KEY_L ||
           key == KEY_HOME || key == KEY_KP1 ||
           (key >= KEY_1 && key <= KEY_6);
}

static void held_add(uint32_t key) {
    for (uint32_t i = 0; i < s_held_n; ++i) {
        if (s_held[i] == key) {
            return;
        }
    }
    if (s_held_n < k_held_cap) {
        s_held[s_held_n++] = key;
    }
}

static void held_clear(void) {
    s_held_n = 0;
}

static void enqueue_utf8(const char *bytes, uint32_t n) {
    if (bytes == nullptr || n == 0u || n >= k_utf8_bytes || s_utf8_n >= k_utf8_cap) {
        return;
    }
    memcpy(s_utf8[s_utf8_n].bytes, bytes, n);
    s_utf8[s_utf8_n].bytes[n] = '\0';
    s_utf8[s_utf8_n].n = (uint8_t)n;
    s_utf8_n++;
}

static void arm_repeat(uint32_t key, const char *bytes, uint32_t n) {
    if (s_repeat_rate <= 0 || n == 0u || n >= k_utf8_bytes) {
        s_repeat_armed = false;
        return;
    }
    s_repeat_armed = true;
    s_repeat_key = key;
    memcpy(s_repeat_bytes, bytes, n);
    s_repeat_bytes[n] = '\0';
    s_repeat_n = (uint8_t)n;
    // Same CLOCK_MONOTONIC the ring registered with IORING_REGISTER_CLOCK.
    s_repeat_next = ring_mono_ns() + (uint64_t)s_repeat_delay_ms * 1000000ull;
}

static void queue_key_text(uint32_t key) {
    if (s_xkb_state == nullptr) {
        return;
    }
    char buf[k_utf8_bytes];
    int n = xkb_state_key_get_utf8(s_xkb_state, (xkb_keycode_t)(key + 8u), buf, sizeof buf);
    if (n <= 0 || n >= (int)sizeof buf) {
        return;
    }
    if (n == 1 && (unsigned char)buf[0] < 32u) {
        return;
    }
    enqueue_utf8(buf, (uint32_t)n);
    arm_repeat(key, buf, (uint32_t)n);
}

static void reset_xkb_state(void) {
    if (s_xkb_state != nullptr) {
        xkb_state_unref(s_xkb_state);
        s_xkb_state = nullptr;
    }
    if (s_xkb_map != nullptr) {
        s_xkb_state = xkb_state_new(s_xkb_map);
    }
    s_repeat_armed = false;
    s_utf8_n = 0;
}

static void install_keymap(int fd, uint32_t size) {
    void *mem = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mem == MAP_FAILED) {
        fprintf(stderr, "[Keyboard] Keymap was not mapped\n");
        return;
    }
    if (s_xkb_ctx == nullptr) {
        s_xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    }
    if (s_xkb_ctx == nullptr) {
        if (!s_xkb_warned) {
            fprintf(stderr, "[Keyboard] xkb context was not created\n");
            s_xkb_warned = true;
        }
        munmap(mem, size);
        return;
    }
    struct xkb_keymap *map = xkb_keymap_new_from_buffer(
        s_xkb_ctx, mem, size, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(mem, size);
    if (map == nullptr) {
        fprintf(stderr, "[Keyboard] Keymap was not compiled\n");
        return;
    }
    if (s_xkb_state != nullptr) {
        xkb_state_unref(s_xkb_state);
        s_xkb_state = nullptr;
    }
    if (s_xkb_map != nullptr) {
        xkb_keymap_unref(s_xkb_map);
    }
    s_xkb_map = map;
    s_xkb_state = xkb_state_new(s_xkb_map);
    s_repeat_armed = false;
}

bool input_on_keyboard(WaylandContext *w, uint16_t opcode,
                       const unsigned char *payload, uint16_t len) {
    const unsigned char *p = payload;
    const unsigned char *end = payload + len;
    if (opcode == 0u) {
        // wl_keyboard.keymap. format and size are in the payload. The fd
        // shares parked_fds with the DMA-BUF format table, so it is taken
        // here even when the keymap is not xkb v1.
        uint32_t format = 0;
        uint32_t size = 0;
        bool fields = input_take_u32(&p, end, &format) && input_take_u32(&p, end, &size);
        int fd = -1;
        if (!wayland_take_parked_fd(w, &fd)) {
            wayland_fail(w, "wl_keyboard.keymap missing fd");
            return true;
        }
        if (!fields || format != 1u || size == 0u) {
            close(fd);
            return true;
        }
        install_keymap(fd, size);
        return true;
    }
    if (opcode == 1u) {
        uint32_t serial = 0;
        uint32_t surface = 0;
        if (!input_take_u32(&p, end, &serial) || !input_take_u32(&p, end, &surface)) {
            wayland_fail(w, "truncated wl_keyboard.enter");
            return true;
        }
        (void)serial;
        // Accept keyboard focus on both the scene surface and the bar subsurface.
        s_key_focus = surface != 0 &&
                      (surface == w->surface_id ||
                       (w->bar_surface_id != 0 && surface == w->bar_surface_id));
        held_clear();
        reset_xkb_state();
        return true;
    }
    if (opcode == 2u) {
        s_key_focus = false;
        held_clear();
        reset_xkb_state();
        return true;
    }
    if (opcode == 3u) {
        uint32_t serial = 0;
        uint32_t time_ms = 0;
        uint32_t key = 0;
        uint32_t state = 0;
        if (!input_take_u32(&p, end, &serial) || !input_take_u32(&p, end, &time_ms) ||
            !input_take_u32(&p, end, &key) || !input_take_u32(&p, end, &state)) {
            wayland_fail(w, "truncated wl_keyboard.key");
            return true;
        }
        (void)serial;
        s_ptr.sample_ms = time_ms;
        if (!s_key_focus) {
            return true;
        }
        xkb_keycode_t code = (xkb_keycode_t)(key + 8u);
        if (state == 0u) {
            if (s_xkb_state != nullptr) {
                xkb_state_update_key(s_xkb_state, code, XKB_KEY_UP);
            }
            if (s_repeat_armed && s_repeat_key == key) {
                s_repeat_armed = false;
            }
            for (uint32_t i = 0; i < s_held_n; ++i) {
                if (s_held[i] == key) {
                    s_held[i] = s_held[s_held_n - 1u];
                    s_held_n--;
                    break;
                }
            }
            return true;
        }
        if (state != 1u) {
            return true;
        }
        if (s_xkb_state != nullptr) {
            xkb_state_update_key(s_xkb_state, code, XKB_KEY_DOWN);
        }
        if (shift_key(key)) {
            held_add(key);
            return true;
        }
        const EngineApp *app = engine_app();
        uint8_t route = ENGINE_KEY_IGNORE;
        if (app != nullptr && app->route_key != nullptr) {
            route = app->route_key(key);
        } else if (motion_key(key) || (key == KEY_4 && shift_held())) {
            route = ENGINE_KEY_HOLD;
        } else if (tap_key(key)) {
            route = ENGINE_KEY_TAP;
        }
        if (route == ENGINE_KEY_HOLD) {
            held_add(key);
            return true;
        }
        if (route == ENGINE_KEY_TAP) {
            held_add(key);
            if (s_key_n < k_key_cap) {
                s_key_q[s_key_n++] = key;
            }
            queue_key_text(key);
        }
        return true;
    }
    if (opcode == 4u) {
        uint32_t serial = 0;
        uint32_t depressed = 0;
        uint32_t latched = 0;
        uint32_t locked = 0;
        uint32_t group = 0;
        if (!input_take_u32(&p, end, &serial) || !input_take_u32(&p, end, &depressed) ||
            !input_take_u32(&p, end, &latched) || !input_take_u32(&p, end, &locked) ||
            !input_take_u32(&p, end, &group)) {
            wayland_fail(w, "truncated wl_keyboard.modifiers");
            return true;
        }
        (void)serial;
        if (s_xkb_state != nullptr) {
            xkb_state_update_mask(s_xkb_state, depressed, latched, locked, 0, 0, group);
        }
        return true;
    }
    if (opcode == 5u) {
        int32_t rate = 0;
        int32_t delay = 0;
        if (!input_take_i32(&p, end, &rate) || !input_take_i32(&p, end, &delay)) {
            wayland_fail(w, "truncated wl_keyboard.repeat_info");
            return true;
        }
        if (rate < 0 || delay < 0) {
            return true;
        }
        s_repeat_rate = rate;
        s_repeat_delay_ms = delay;
        if (rate == 0) {
            s_repeat_armed = false;
        }
        return true;
    }
    return true;
}

bool pointer_key_down(uint32_t key) {
    if (!s_key_focus) {
        return false;
    }
    for (uint32_t i = 0; i < s_held_n; ++i) {
        if (s_held[i] == key) {
            return true;
        }
    }
    return false;
}

void pointer_pump_text(void) {
    if (!s_repeat_armed) {
        return;
    }
    if (!pointer_key_down(s_repeat_key)) {
        s_repeat_armed = false;
        return;
    }
    if (s_repeat_rate <= 0) {
        return;
    }
    uint64_t now = ring_mono_ns();
    if (now < s_repeat_next) {
        return;
    }
    enqueue_utf8(s_repeat_bytes, s_repeat_n);
    uint64_t step = 1000000000ull / (uint64_t)s_repeat_rate;
    if (step == 0u) {
        step = 1000000ull;
    }
    s_repeat_next = now + step;
}

bool pointer_take_text(char *utf8, uint32_t cap, uint32_t *n) {
    if (utf8 == nullptr || n == nullptr || cap == 0u || s_utf8_n == 0u) {
        return false;
    }
    uint32_t len = s_utf8[0].n;
    if (len >= cap) {
        len = cap - 1u;
    }
    memcpy(utf8, s_utf8[0].bytes, len);
    utf8[len] = '\0';
    *n = len;
    for (uint32_t i = 1; i < s_utf8_n; ++i) {
        s_utf8[i - 1u] = s_utf8[i];
    }
    s_utf8_n--;
    return true;
}

bool pointer_take_key(uint32_t *key) {
    if (s_key_n == 0 || key == nullptr) {
        return false;
    }
    *key = s_key_q[0];
    for (uint32_t i = 1; i < s_key_n; ++i) {
        s_key_q[i - 1] = s_key_q[i];
    }
    s_key_n--;
    return true;
}
