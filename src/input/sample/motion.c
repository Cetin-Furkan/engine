#include "input/internal.h"
#include "chrome/layer.h"
#include "engine/app.h"
#include <linux/input-event-codes.h>

static float fixed_to_px(int32_t v) {
    return (float)v / 256.0f;
}

static int32_t px_floor_i32(float v) {
    int32_t i = (int32_t)v;
    return (v < (float)i) ? (i - 1) : i;
}

static uint32_t button_bit(uint32_t button) {
    if (button == BTN_LEFT) {
        return ENG_BUTTON_LEFT;
    }
    if (button == BTN_RIGHT) {
        return ENG_BUTTON_RIGHT;
    }
    if (button == BTN_MIDDLE) {
        return ENG_BUTTON_MIDDLE;
    }
    return 0;
}

static void retarget(WaylandContext *w, bool entering) {
    ChromeHit hit;
    chrome_hit_test(w, s_ptr.focus, px_floor_i32(s_ptr.x), px_floor_i32(s_ptr.y), &hit);
    s_ptr.wanted_shape = hit.cursor_shape;
    s_ptr.edge = hit.resize_edge;
    s_ptr.close_hit = hit.close;
    s_ptr.title_hit = hit.title;
    bool same_inflight = s_ptr.cursor_inflight &&
                         s_ptr.inflight_shape == hit.cursor_shape &&
                         s_ptr.inflight_serial == s_ptr.enter_serial;
    if (entering || !s_ptr.have_last || (hit.cursor_shape != s_ptr.last_shape && !same_inflight)) {
        s_ptr.cursor_dirty = true;
    }
}

bool input_on_pointer(WaylandContext *w, uint16_t opcode,
                      const unsigned char *payload, uint16_t len) {
    const unsigned char *p = payload;
    const unsigned char *end = payload + len;
    if (opcode == 0u) {
        // wl_pointer.enter
        uint32_t serial = 0, surface = 0;
        int32_t fx = 0, fy = 0;
        if (!input_take_u32(&p, end, &serial) || !input_take_u32(&p, end, &surface) ||
            !input_take_i32(&p, end, &fx) || !input_take_i32(&p, end, &fy)) {
            wayland_fail(w, "truncated wl_pointer.enter");
            return true;
        }
        s_ptr.enter_serial = serial;
        s_ptr.have_enter = true;
        s_ptr.focus = surface;
        s_ptr.x = fixed_to_px(fx);
        s_ptr.y = fixed_to_px(fy);
        s_ptr.frame_pending_retarget = true;
        s_ptr.frame_entering = true;
        retarget(w, true);
        return true;
    }
    if (opcode == 1u) {
        // wl_pointer.leave
        s_ptr.focus = 0;
        s_ptr.buttons = 0;
        s_ptr.cursor_dirty = false;
        s_ptr.frame_pending_retarget = false;
        s_ptr.frame_entering = false;
        s_ptr.close_hit = false;
        s_ptr.title_hit = false;
        s_ptr.edge = 0;
        return true;
    }
    if (opcode == 2u) {
        // wl_pointer.motion
        uint32_t time_ms = 0;
        int32_t fx = 0, fy = 0;
        if (!input_take_u32(&p, end, &time_ms) || !input_take_i32(&p, end, &fx) ||
            !input_take_i32(&p, end, &fy)) {
            wayland_fail(w, "truncated wl_pointer.motion");
            return true;
        }
        s_ptr.sample_ms = time_ms;
        if (s_ptr.focus == 0) {
            return true;
        }
        float nx = fixed_to_px(fx);
        float ny = fixed_to_px(fy);
        float dx = nx - s_ptr.x;
        float dy = ny - s_ptr.y;
        uint32_t drag = s_ptr.buttons & (ENG_BUTTON_RIGHT | ENG_BUTTON_MIDDLE);
        if (s_ptr.focus == w->surface_id && drag != 0 && (dx != 0.0f || dy != 0.0f)) {
            s_drag_dx += dx;
            s_drag_dy += dy;
            s_drag_buttons |= drag;
        }
        s_ptr.x = nx;
        s_ptr.y = ny;
        s_ptr.frame_pending_retarget = true;
        retarget(w, false);
        return true;
    }
    if (opcode == 3u) {
        // wl_pointer.button
        uint32_t serial = 0, time_ms = 0, button = 0, state = 0;
        if (!input_take_u32(&p, end, &serial) || !input_take_u32(&p, end, &time_ms) ||
            !input_take_u32(&p, end, &button) || !input_take_u32(&p, end, &state)) {
            wayland_fail(w, "truncated wl_pointer.button");
            return true;
        }
        s_ptr.sample_ms = time_ms;
        uint32_t bit = button_bit(button);
        if (bit != 0) {
            if (state == 1u) {
                s_ptr.buttons |= bit;
            } else {
                s_ptr.buttons &= ~bit;
            }
        }
        if (state == 1u && button == BTN_LEFT) {
            if (s_ptr.frame_pending_retarget && s_ptr.focus != 0) {
                retarget(w, s_ptr.frame_entering);
                s_ptr.frame_pending_retarget = false;
                s_ptr.frame_entering = false;
            }
            if (s_ptr.close_hit) {
                // The idle loop treats this as a clean exit.
                s_ptr.resize_pending = false;
                s_ptr.move_pending = false;
                w->toplevel_closed = true;
            } else if (s_ptr.edge != 0 && w->xdg_toplevel_id != 0) {
                s_ptr.move_pending = false;
                s_ptr.resize_pending = true;
                s_ptr.resize_serial = serial;
                s_ptr.resize_edge = s_ptr.edge;
            } else if (s_ptr.title_hit && w->xdg_toplevel_id != 0) {
                s_ptr.resize_pending = false;
                s_ptr.move_pending = true;
                s_ptr.move_serial = serial;
            } else if (s_ptr.focus == w->surface_id && s_spawn_n < k_spawn_cap) {
                s_spawn[s_spawn_n].x = px_floor_i32(s_ptr.x);
                s_spawn[s_spawn_n].y = px_floor_i32(s_ptr.y);
                s_spawn_n++;
            }
        }
        return true;
    }
    if (opcode == 4u) {
        // wl_pointer.axis (since v5): time, axis, wl_fixed value.
        // Axis 0 is vertical. Opcode 9 (axis_value120) is the same
        // gesture in detent units and is left unread so it is not applied twice.
        uint32_t time_ms = 0;
        uint32_t axis = 0;
        int32_t value = 0;
        if (!input_take_u32(&p, end, &time_ms) || !input_take_u32(&p, end, &axis) ||
            !input_take_i32(&p, end, &value)) {
            wayland_fail(w, "truncated wl_pointer.axis");
            return true;
        }
        s_ptr.sample_ms = time_ms;
        // The bar is its own surface. A wheel there must not zoom the scene.
        if (axis == 0u && s_ptr.focus == w->surface_id) {
            s_scroll += (double)value / 256.0;
        }
        return true;
    }
    if (opcode == 5u) {
        // wl_pointer.frame (since wl_seat v5): atomic end of a pointer event group.
        if (s_ptr.frame_pending_retarget && s_ptr.focus != 0) {
            retarget(w, s_ptr.frame_entering);
        }
        s_ptr.frame_pending_retarget = false;
        s_ptr.frame_entering = false;
        return true;
    }
    return true;
}

bool pointer_take_drag(float *dx, float *dy, uint32_t *buttons) {
    if (dx == nullptr || dy == nullptr || buttons == nullptr) {
        return false;
    }
    if (s_drag_dx == 0.0f && s_drag_dy == 0.0f) {
        return false;
    }
    *dx = s_drag_dx;
    *dy = s_drag_dy;
    *buttons = s_drag_buttons;
    s_drag_dx = 0.0f;
    s_drag_dy = 0.0f;
    s_drag_buttons = 0;
    input_page_publish();
    return true;
}

bool pointer_take_spawn(int32_t *x, int32_t *y) {
    if (s_spawn_n == 0 || x == nullptr || y == nullptr) {
        return false;
    }
    *x = s_spawn[0].x;
    *y = s_spawn[0].y;
    for (uint32_t i = 1; i < s_spawn_n; ++i) {
        s_spawn[i - 1] = s_spawn[i];
    }
    s_spawn_n--;
    return true;
}

bool pointer_take_scroll(float *steps) {
    if (steps == nullptr || s_scroll == 0.0) {
        return false;
    }
    *steps = (float)(-s_scroll / 10.0);
    s_scroll = 0.0;
    input_page_publish();
    return *steps != 0.0f;
}

bool pointer_client_xy(int32_t *x, int32_t *y) {
    if (x == nullptr || y == nullptr || s_ptr.focus == 0) {
        return false;
    }
    const WaylandContext *w = wayland_get_context();
    int32_t px = px_floor_i32(s_ptr.x);
    int32_t py = px_floor_i32(s_ptr.y);
    if (w != nullptr && w->bar_surface_id != 0 && s_ptr.focus == w->bar_surface_id) {
        py -= (int32_t)w->bar_px;
    }
    *x = px;
    *y = py;
    return true;
}
