#define _GNU_SOURCE
#include "input/internal.h"
#include "wayland/internal.h"
#include "engine/app.h"
#include "ring/ring.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

static bool shift_key(uint32_t key) {
    return key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT;
}

static bool chord_mod_active(void) {
    if (s_xkb_state == nullptr) {
        return false;
    }
    return xkb_state_mod_name_is_active(s_xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0 ||
           xkb_state_mod_name_is_active(s_xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0 ||
           xkb_state_mod_name_is_active(s_xkb_state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0;
}

static bool ctrl_only_active(void) {
    if (s_xkb_state == nullptr) {
        return false;
    }
    return xkb_state_mod_name_is_active(s_xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0 &&
           xkb_state_mod_name_is_active(s_xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) == 0 &&
           xkb_state_mod_name_is_active(s_xkb_state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) == 0;
}

static bool repeatable_tap_key(uint32_t key) {
    return key == KEY_BACKSPACE || key == KEY_DELETE ||
           key == KEY_ENTER || key == KEY_KPENTER ||
           key == KEY_LEFT || key == KEY_RIGHT ||
           key == KEY_UP || key == KEY_DOWN ||
           key == KEY_HOME || key == KEY_END;
}

static bool shift_held(void) {
    for (uint32_t i = 0; i < s_held_n; ++i) {
        if (shift_key(s_held[i])) {
            return true;
        }
    }
    return false;
}

static bool motion_key(uint32_t key) {
    return key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN ||
           key == KEY_Q || key == KEY_E || key == KEY_C || key == KEY_V ||
           key == KEY_MINUS || key == KEY_EQUAL || key == KEY_KPMINUS || key == KEY_KPPLUS;
}

static bool tap_key(uint32_t key) {
    return key == KEY_ESC || key == KEY_SPACE || key == KEY_ENTER || key == KEY_KPENTER ||
           key == KEY_BACKSPACE || key == KEY_DELETE || key == KEY_TAB ||
           key == KEY_HOME || key == KEY_END ||
           key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN ||
           (key >= KEY_1 && key <= KEY_0) ||
           (key >= KEY_Q && key <= KEY_P) ||
           (key >= KEY_A && key <= KEY_L) ||
           (key >= KEY_Z && key <= KEY_M) ||
           (key >= KEY_F1 && key <= KEY_F10) ||
           key == KEY_F11 || key == KEY_F12 ||
           key == KEY_KP1;
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

static void arm_repeat(uint32_t key, const char *bytes, uint32_t n, bool tap_repeat) {
    if (s_repeat_rate <= 0 || (!tap_repeat && (n == 0u || n >= k_utf8_bytes))) {
        s_repeat_armed = false;
        s_repeat_tap = false;
        return;
    }
    s_repeat_armed = true;
    s_repeat_tap = tap_repeat;
    s_repeat_key = key;
    if (n > 0u && n < k_utf8_bytes && bytes != nullptr) {
        memcpy(s_repeat_bytes, bytes, n);
        s_repeat_bytes[n] = '\0';
        s_repeat_n = (uint8_t)n;
    } else {
        s_repeat_bytes[0] = '\0';
        s_repeat_n = 0u;
    }
    // Same CLOCK_MONOTONIC the ring registered with IORING_REGISTER_CLOCK.
    s_repeat_next = ring_mono_ns() + (uint64_t)s_repeat_delay_ms * 1000000ull;
}

static void queue_key_text(uint32_t key, bool allow_tap_repeat) {
    bool want_tap_rep = allow_tap_repeat && repeatable_tap_key(key);
    if (s_xkb_state == nullptr) {
        if (want_tap_rep) {
            arm_repeat(key, nullptr, 0u, true);
        }
        return;
    }
    xkb_keycode_t code = (xkb_keycode_t)(key + 8u);
    char buf[k_utf8_bytes];
    int  n        = 0;
    bool composed = false;
    if (s_xkb_compose_state != nullptr) {
        xkb_keysym_t sym = xkb_state_key_get_one_sym(s_xkb_state, code);
        if (sym != XKB_KEY_NoSymbol &&
            xkb_compose_state_feed(s_xkb_compose_state, sym) == XKB_COMPOSE_FEED_ACCEPTED) {
            enum xkb_compose_status st = xkb_compose_state_get_status(s_xkb_compose_state);
            if (st == XKB_COMPOSE_COMPOSING) {
                return;
            }
            if (st == XKB_COMPOSE_COMPOSED) {
                n = xkb_compose_state_get_utf8(s_xkb_compose_state, buf, sizeof buf);
                xkb_compose_state_reset(s_xkb_compose_state);
                composed = true;
            } else if (st == XKB_COMPOSE_CANCELLED) {
                xkb_compose_state_reset(s_xkb_compose_state);
                return;
            }
        }
    }
    if (!composed) {
        n = xkb_state_key_get_utf8(s_xkb_state, code, buf, sizeof buf);
    }
    bool printable = (n > 0 && n < (int)sizeof buf) &&
                     !(n == 1 && ((unsigned char)buf[0] < 32u || (unsigned char)buf[0] == 0x7fu));
    if (printable) {
        enqueue_utf8(buf, (uint32_t)n);
        arm_repeat(key, buf, (uint32_t)n, want_tap_rep);
    } else if (want_tap_rep) {
        arm_repeat(key, nullptr, 0u, true);
    }
}

static void reset_xkb_state(bool clear_utf8) {
    if (s_xkb_state != nullptr) {
        xkb_state_unref(s_xkb_state);
        s_xkb_state = nullptr;
    }
    if (s_xkb_map != nullptr) {
        s_xkb_state = xkb_state_new(s_xkb_map);
    }
    if (s_xkb_compose_state != nullptr) {
        xkb_compose_state_reset(s_xkb_compose_state);
    }
    s_repeat_armed = false;
    s_repeat_tap = false;
    if (clear_utf8) {
        s_utf8_n = 0;
    }
}

static const char *resolve_compose_locale(void) {
    const char *loc = getenv("LC_ALL");
    if (loc == nullptr || loc[0] == '\0') {
        loc = getenv("LC_CTYPE");
    }
    if (loc == nullptr || loc[0] == '\0') {
        loc = getenv("LANG");
    }
    if (loc == nullptr || loc[0] == '\0') {
        loc = "C";
    }
    return loc;
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
    size_t text_len = strnlen((const char *)mem, size);
    struct xkb_keymap *map = xkb_keymap_new_from_buffer(
        s_xkb_ctx, mem, text_len, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
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
    if (s_xkb_compose_table == nullptr) {
        s_xkb_compose_table = xkb_compose_table_new_from_locale(
            s_xkb_ctx, resolve_compose_locale(), XKB_COMPOSE_COMPILE_NO_FLAGS);
    }
    if (s_xkb_compose_state != nullptr) {
        xkb_compose_state_unref(s_xkb_compose_state);
        s_xkb_compose_state = nullptr;
    }
    if (s_xkb_compose_table != nullptr) {
        s_xkb_compose_state = xkb_compose_state_new(s_xkb_compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
    }
    s_repeat_armed = false;
    s_repeat_tap = false;
}

static void push_dead_offer(uint32_t id) {
    if (id == 0u) {
        return;
    }
    for (uint32_t i = 0; i < s_dead_offers_n; ++i) {
        if (s_dead_offers[i] == id) {
            return;
        }
    }
    if (s_dead_offers_n < k_dead_cap) {
        s_dead_offers[s_dead_offers_n++] = id;
    }
}

static void push_dead_source(uint32_t id) {
    if (id == 0u) {
        return;
    }
    s_retired_sources[s_retired_sources_cursor & (k_dead_cap - 1u)] = id;
    s_retired_sources_cursor++;
    for (uint32_t i = 0; i < s_dead_sources_n; ++i) {
        if (s_dead_sources[i] == id) {
            return;
        }
    }
    if (s_dead_sources_n < k_dead_cap) {
        s_dead_sources[s_dead_sources_n++] = id;
    }
}

static bool arm_clip_pipe_read(RingContext *r) {
    struct io_uring_sqe *sqe = ring_sqe(r);
    if (sqe == nullptr) {
        return false;
    }
    sqe->opcode    = IORING_OP_READ;
    sqe->flags     = IOSQE_FIXED_FILE;
    sqe->fd        = ENG_CLIPBOARD_FD_SLOT;
    sqe->addr      = (uint64_t)(uintptr_t)s_clip_rx_buf;
    sqe->len       = sizeof(s_clip_rx_buf);
    sqe->off       = (uint64_t)-1;
    sqe->user_data = ENG_UD_CLIP_READ;
    return true;
}

static bool arm_clip_pipe_write(RingContext *r) {
    struct io_uring_sqe *sqe = ring_sqe(r);
    if (sqe == nullptr) {
        return false;
    }
    sqe->opcode    = IORING_OP_WRITE;
    sqe->flags     = IOSQE_FIXED_FILE;
    sqe->fd        = ENG_CLIPBOARD_TX_FD_SLOT;
    sqe->addr      = (uint64_t)(uintptr_t)(s_clip_write_buf + s_clip_write_off);
    sqe->len       = s_clip_write_len - s_clip_write_off;
    sqe->off       = (uint64_t)-1;
    sqe->user_data = ENG_UD_CLIP_WRITE;
    return true;
}

static void close_clip_fixed_slot(RingContext *r, uint32_t slot, bool *active_flag) {
    if (!*active_flag) {
        return;
    }
    *active_flag = false;
    if (r == nullptr || !r->is_initialized) {
        return;
    }
    struct io_uring_sqe *sqe = ring_sqe(r);
    if (sqe != nullptr) {
        sqe->opcode     = IORING_OP_CLOSE;
        sqe->fd         = 0;
        sqe->file_index = slot + 1u;
        sqe->user_data  = ENG_UD_CLIP_CLOSE;
        return;
    }
    (void)ring_update_file(r, slot, -1);
}

static void start_clipboard_paste(WaylandContext *w) {
    if (w == nullptr || w->ring == nullptr || !w->ring->is_initialized || w->failed) {
        return;
    }
    if (s_clip_rx_active || s_selection_offer_id == 0u || s_selection_prio == 0u ||
        s_selection_mime[0] == '\0') {
        return;
    }
    int fds[2] = { -1, -1 };
    if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
        return;
    }
    if (ring_update_file(w->ring, ENG_CLIPBOARD_FD_SLOT, fds[0]) != 0) {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    close(fds[0]);
    if (wl_queue_data_receive(w, s_selection_offer_id, s_selection_mime, fds[1]) != 0) {
        close(fds[1]);
        (void)ring_update_file(w->ring, ENG_CLIPBOARD_FD_SLOT, -1);
        return;
    }
    s_clip_rx_len     = 0u;
    s_clip_rx_carry_n = 0u;
    s_clip_rx_active  = true;
    if (!arm_clip_pipe_read(w->ring)) {
        close_clip_fixed_slot(w->ring, ENG_CLIPBOARD_FD_SLOT, &s_clip_rx_active);
    }
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
        uint32_t keys_len = 0;
        if (!input_take_u32(&p, end, &serial) || !input_take_u32(&p, end, &surface) ||
            !input_take_u32(&p, end, &keys_len)) {
            wayland_fail(w, "truncated wl_keyboard.enter");
            return true;
        }
        uint32_t pad = (4u - (keys_len & 3u)) & 3u;
        if ((size_t)(end - p) < (size_t)keys_len + pad) {
            wayland_fail(w, "wl_keyboard.enter keys array out of bounds");
            return true;
        }
        (void)serial;
        bool had_focus = s_key_focus;
        // Accept keyboard focus on both the scene surface and the bar subsurface.
        s_key_focus = surface != 0 &&
                      (surface == w->surface_id ||
                       (w->bar_surface_id != 0 && surface == w->bar_surface_id));
        held_clear();
        // Preserve queued UTF-8 text when focus shifts internally between scene and bar.
        reset_xkb_state(!had_focus || !s_key_focus);
        if (s_key_focus) {
            const EngineApp *app = engine_app();
            uint32_t left = keys_len;
            const unsigned char *kp = p;
            while (left >= 4u) {
                uint32_t k = 0;
                memcpy(&k, kp, 4);
                if (s_xkb_state != nullptr) {
                    xkb_state_update_key(s_xkb_state, (xkb_keycode_t)(k + 8u), XKB_KEY_DOWN);
                }
                uint8_t route = ENGINE_KEY_IGNORE;
                if (shift_key(k)) {
                    held_add(k);
                } else if (app != nullptr && app->route_key != nullptr) {
                    route = app->route_key(k);
                } else if (app != nullptr && app->camera_active != nullptr && motion_key(k)) {
                    route = ENGINE_KEY_HOLD;
                }
                if (route == ENGINE_KEY_HOLD) {
                    held_add(k);
                }
                kp += 4;
                left -= 4u;
            }
        }
        return true;
    }
    if (opcode == 2u) {
        s_key_focus = false;
        held_clear();
        reset_xkb_state(true);
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
        s_last_key_serial = serial;
        if (s_xkb_state != nullptr) {
            xkb_state_update_key(s_xkb_state, code, XKB_KEY_DOWN);
        }
        if (ctrl_only_active()) {
            if (key == KEY_V) {
                start_clipboard_paste(w);
                return true;
            }
            if (key == KEY_C) {
                if (s_clip_tx_len > 0u) {
                    s_clip_copy_pending = true;
                }
                return true;
            }
        }
        if (shift_key(key)) {
            held_add(key);
            return true;
        }
        const EngineApp *app = engine_app();
        bool chord = chord_mod_active();
        uint8_t route = ENGINE_KEY_IGNORE;
        if (app != nullptr && app->route_key != nullptr) {
            route = app->route_key(key);
        } else if (!chord && app != nullptr && app->camera_active != nullptr &&
                   (motion_key(key) || (key == KEY_4 && shift_held()))) {
            route = ENGINE_KEY_HOLD;
        } else if (tap_key(key)) {
            route = ENGINE_KEY_TAP;
        }
        if (chord) {
            bool fn_or_esc = (key == KEY_ESC) ||
                             (key >= KEY_F1 && key <= KEY_F10) ||
                             key == KEY_F11 || key == KEY_F12;
            if (!fn_or_esc) {
                route = ENGINE_KEY_IGNORE;
            }
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
            queue_key_text(key, true);
            return true;
        }
        if (!chord) {
            held_add(key);
            queue_key_text(key, false);
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

uint64_t pointer_next_repeat_deadline_ns(void) {
    if (!s_repeat_armed || s_repeat_rate <= 0 || !pointer_key_down(s_repeat_key)) {
        return 0u;
    }
    return s_repeat_next;
}

void pointer_pump_text(void) {
    if (!s_repeat_armed) {
        return;
    }
    if (!pointer_key_down(s_repeat_key)) {
        s_repeat_armed = false;
        s_repeat_tap = false;
        return;
    }
    if (s_repeat_rate <= 0) {
        s_repeat_armed = false;
        s_repeat_tap = false;
        return;
    }
    uint64_t now = ring_mono_ns();
    if (now < s_repeat_next) {
        return;
    }
    if (s_repeat_n > 0u) {
        enqueue_utf8(s_repeat_bytes, s_repeat_n);
    }
    if (s_repeat_tap && s_key_n < k_key_cap) {
        s_key_q[s_key_n++] = s_repeat_key;
    }
    uint64_t step = 1000000000ull / (uint64_t)s_repeat_rate;
    if (step == 0u) {
        step = 1000000ull;
    }
    s_repeat_next += step;
    if (s_repeat_next <= now) {
        s_repeat_next = now + step;
    }
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

static uint8_t mime_priority(const char *mime) {
    if (strcmp(mime, "text/plain;charset=utf-8") == 0) {
        return 3u;
    }
    if (strcmp(mime, "UTF8_STRING") == 0) {
        return 2u;
    }
    if (strcmp(mime, "text/plain") == 0) {
        return 1u;
    }
    return 0u;
}

bool input_on_data_device(WaylandContext *w, uint16_t opcode,
                          const unsigned char *payload, uint16_t len) {
    const unsigned char *p   = payload;
    const unsigned char *end = payload + len;
    if (opcode == 0u) {
        // wl_data_device.data_offer(new_id)
        uint32_t offer_id = 0u;
        if (!input_take_u32(&p, end, &offer_id)) {
            wayland_fail(w, "truncated wl_data_device.data_offer");
            return true;
        }
        if (s_pending_offer_id != 0u && s_pending_offer_id != s_selection_offer_id) {
            push_dead_offer(s_pending_offer_id);
        }
        s_pending_offer_id      = offer_id;
        s_pending_offer_prio    = 0u;
        s_pending_offer_mime[0] = '\0';
        return true;
    }
    if (opcode == 1u) {
        // wl_data_device.enter(serial, surface, x, y, id) — DnD offer is retired unused.
        if (len >= 20u) {
            uint32_t dnd_id = 0u;
            memcpy(&dnd_id, payload + 16u, sizeof dnd_id);
            if (dnd_id != 0u) {
                if (dnd_id == s_pending_offer_id) {
                    s_pending_offer_id      = 0u;
                    s_pending_offer_prio    = 0u;
                    s_pending_offer_mime[0] = '\0';
                }
                if (dnd_id != s_selection_offer_id) {
                    push_dead_offer(dnd_id);
                }
            }
        }
        return true;
    }
    if (opcode == 5u) {
        // wl_data_device.selection(id)
        uint32_t sel_id = 0u;
        if (!input_take_u32(&p, end, &sel_id)) {
            wayland_fail(w, "truncated wl_data_device.selection");
            return true;
        }
        if (s_selection_offer_id != 0u && s_selection_offer_id != sel_id) {
            push_dead_offer(s_selection_offer_id);
        }
        if (sel_id == 0u) {
            s_selection_offer_id = 0u;
            s_selection_prio     = 0u;
            s_selection_mime[0]  = '\0';
        } else if (sel_id == s_pending_offer_id) {
            s_selection_offer_id = sel_id;
            s_selection_prio     = s_pending_offer_prio;
            memcpy(s_selection_mime, s_pending_offer_mime, sizeof s_selection_mime);
            s_pending_offer_id      = 0u;
            s_pending_offer_prio    = 0u;
            s_pending_offer_mime[0] = '\0';
        } else {
            s_selection_offer_id = sel_id;
            s_selection_prio     = 0u;
            s_selection_mime[0]  = '\0';
        }
        return true;
    }
    return true;
}

bool input_on_data_offer(WaylandContext *w, uint32_t id, uint16_t opcode,
                         const unsigned char *payload, uint16_t len) {
    if (opcode != 0u) {
        return true;
    }
    // wl_data_offer.offer(mime_type)
    const unsigned char *p   = payload;
    const unsigned char *end = payload + len;
    uint32_t slen = 0u;
    if (!input_take_u32(&p, end, &slen) || slen == 0u || p + slen > end || p[slen - 1u] != 0u) {
        wayland_fail(w, "invalid wl_data_offer.offer string");
        return true;
    }
    const char *mime = (const char *)p;
    uint8_t prio = mime_priority(mime);
    if (prio == 0u || slen > sizeof(s_pending_offer_mime)) {
        return true;
    }
    if (id == s_pending_offer_id && prio > s_pending_offer_prio) {
        s_pending_offer_prio = prio;
        memcpy(s_pending_offer_mime, mime, slen);
    } else if (id == s_selection_offer_id && prio > s_selection_prio) {
        s_selection_prio = prio;
        memcpy(s_selection_mime, mime, slen);
    }
    return true;
}

bool input_on_data_source(WaylandContext *w, uint32_t id, uint16_t opcode,
                          const unsigned char *payload, uint16_t len) {
    (void)payload;
    (void)len;
    if (opcode == 1u) {
        // wl_data_source.send(mime_type, fd) — ALWAYS pop parked fd to keep FIFO aligned!
        int fd = -1;
        if (!wayland_take_parked_fd(w, &fd)) {
            wayland_fail(w, "wl_data_source.send missing SCM_RIGHTS fd");
            return true;
        }
        if (id != s_data_source_id || s_clip_tx_len == 0u || s_clip_write_active ||
            w->ring == nullptr || !w->ring->is_initialized) {
            close(fd);
            return true;
        }
        if (ring_update_file(w->ring, ENG_CLIPBOARD_TX_FD_SLOT, fd) != 0) {
            close(fd);
            return true;
        }
        close(fd);
        memcpy(s_clip_write_buf, s_clip_tx_buf, s_clip_tx_len);
        s_clip_write_len    = s_clip_tx_len;
        s_clip_write_off    = 0u;
        s_clip_write_active = true;
        if (!arm_clip_pipe_write(w->ring)) {
            close_clip_fixed_slot(w->ring, ENG_CLIPBOARD_TX_FD_SLOT, &s_clip_write_active);
        }
        return true;
    }
    if (opcode == 2u) {
        // wl_data_source.cancelled() — destroy retired selection source (op 1).
        if (id == s_data_source_id) {
            s_data_source_id = 0u;
            push_dead_source(id);
        }
        return true;
    }
    return true;
}

void pointer_set_clipboard_text(const char *utf8, uint32_t len) {
    if (utf8 == nullptr || len == 0u) {
        s_clip_tx_len    = 0u;
        s_clip_tx_buf[0] = '\0';
        return;
    }
    if (len >= k_clip_cap) {
        len = k_clip_cap - 1u;
    }
    memcpy(s_clip_tx_buf, utf8, len);
    s_clip_tx_buf[len] = '\0';
    s_clip_tx_len      = len;
}

static uint32_t utf8_expected_len(unsigned char lead) {
    if ((lead & 0x80u) == 0u) {
        return 1u;
    }
    if ((lead & 0xE0u) == 0xC0u) {
        return 2u;
    }
    if ((lead & 0xF0u) == 0xE0u) {
        return 3u;
    }
    if ((lead & 0xF8u) == 0xF0u) {
        return 4u;
    }
    return 1u;
}

static bool drain_paste_span(const char *src, uint32_t len, bool is_eof) {
    char combined[k_clip_cap + 4u];
    uint32_t total = 0u;
    if (s_clip_rx_carry_n > 0u) {
        memcpy(combined, s_clip_rx_carry, s_clip_rx_carry_n);
        total += s_clip_rx_carry_n;
        s_clip_rx_carry_n = 0u;
    }
    if (len > 0u) {
        if (len > k_clip_cap) {
            len = k_clip_cap;
        }
        memcpy(combined + total, src, len);
        total += len;
    }
    if (total == 0u) {
        return false;
    }

    // Hold back a trailing incomplete multi-byte UTF-8 sequence until the next read.
    uint32_t safe_end = total;
    if (!is_eof) {
        uint32_t back = (total < 3u) ? total : 3u;
        for (uint32_t d = 1u; d <= back; ++d) {
            uint32_t idx = total - d;
            unsigned char b = (unsigned char)combined[idx];
            if ((b & 0xC0u) != 0x80u) {
                uint32_t need = utf8_expected_len(b);
                if (need > d) {
                    safe_end = idx;
                    s_clip_rx_carry_n = (uint8_t)d;
                    memcpy(s_clip_rx_carry, combined + idx, d);
                }
                break;
            }
        }
    }

    bool any_pushed = false;
    char chunk[k_utf8_bytes];
    uint32_t chunk_len = 0u;
    uint32_t i = 0u;
    while (i < safe_end && s_utf8_n < k_utf8_cap) {
        unsigned char b = (unsigned char)combined[i];
        uint32_t cp_len = utf8_expected_len(b);
        if (i + cp_len > safe_end) {
            cp_len = 1u;
        }
        if (cp_len == 1u) {
            if (b == '\r') {
                i++;
                continue;
            }
            char c = (b == '\t') ? ' ' : (char)b;
            if (c != '\n' && ((unsigned char)c < 32u || (unsigned char)c == 0x7fu)) {
                i++;
                continue;
            }
            if (chunk_len + 1u >= k_utf8_bytes) {
                enqueue_utf8(chunk, chunk_len);
                any_pushed = true;
                chunk_len = 0u;
                if (s_utf8_n >= k_utf8_cap) {
                    break;
                }
            }
            chunk[chunk_len++] = c;
            i++;
        } else {
            if (chunk_len + cp_len >= k_utf8_bytes) {
                if (chunk_len > 0u) {
                    enqueue_utf8(chunk, chunk_len);
                    any_pushed = true;
                    chunk_len = 0u;
                    if (s_utf8_n >= k_utf8_cap) {
                        break;
                    }
                }
            }
            memcpy(chunk + chunk_len, combined + i, cp_len);
            chunk_len += cp_len;
            i += cp_len;
        }
    }
    if (chunk_len > 0u && s_utf8_n < k_utf8_cap) {
        enqueue_utf8(chunk, chunk_len);
        any_pushed = true;
    }
    return any_pushed;
}

void pointer_on_clip_cqe(WaylandContext *w, const struct io_uring_cqe *cqe) {
    if (w == nullptr || cqe == nullptr) {
        return;
    }
    uint16_t tag = ring_ud_tag(cqe->user_data);
    if (tag == ENG_UD_CLIP_READ) {
        if (!s_clip_rx_active) {
            return;
        }
        if (cqe->res > 0) {
            uint32_t n = (uint32_t)cqe->res;
            if (n > sizeof(s_clip_rx_buf)) {
                n = (uint32_t)sizeof(s_clip_rx_buf);
            }
            if (drain_paste_span(s_clip_rx_buf, n, false)) {
                w->clipboard_updated = true;
            }
            s_clip_rx_len += n;
            if (s_utf8_n < k_utf8_cap && s_clip_rx_len < 4096u && arm_clip_pipe_read(w->ring)) {
                return;
            }
            if (drain_paste_span(nullptr, 0u, true)) {
                w->clipboard_updated = true;
            }
            close_clip_fixed_slot(w->ring, ENG_CLIPBOARD_FD_SLOT, &s_clip_rx_active);
            return;
        }
        if (cqe->res == -EAGAIN || cqe->res == -EINTR) {
            if (arm_clip_pipe_read(w->ring)) {
                return;
            }
        }
        if (drain_paste_span(nullptr, 0u, true)) {
            w->clipboard_updated = true;
        }
        close_clip_fixed_slot(w->ring, ENG_CLIPBOARD_FD_SLOT, &s_clip_rx_active);
        return;
    }
    if (tag == ENG_UD_CLIP_WRITE) {
        if (!s_clip_write_active) {
            return;
        }
        if (cqe->res > 0) {
            uint32_t n = (uint32_t)cqe->res;
            if (n < s_clip_write_len - s_clip_write_off) {
                s_clip_write_off += n;
                if (arm_clip_pipe_write(w->ring)) {
                    return;
                }
            }
        } else if (cqe->res == -EAGAIN || cqe->res == -EINTR) {
            if (arm_clip_pipe_write(w->ring)) {
                return;
            }
        }
        close_clip_fixed_slot(w->ring, ENG_CLIPBOARD_TX_FD_SLOT, &s_clip_write_active);
        return;
    }
}
