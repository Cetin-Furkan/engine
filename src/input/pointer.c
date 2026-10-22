#include "input/pointer.h"
#include "chrome/layer.h"
#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 0x0008U
#endif

enum : uint32_t {
    k_cursor_side = 16u,
};

static struct {
    uint32_t enter_serial;
    bool     have_enter;
    uint32_t focus;
    int32_t  x;
    int32_t  y;
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
} s_ptr = { .cursor_fd = -1 };

enum { k_spawn_cap = 8u };
static struct {
    int32_t x;
    int32_t y;
} s_spawn[k_spawn_cap];
static uint32_t s_spawn_n = 0;

static int32_t fixed_to_int(int32_t v) {
    return v >> 8;
}

static bool take_u32(const unsigned char **p, const unsigned char *end, uint32_t *o) {
    if ((size_t)(end - *p) < 4u) {
        return false;
    }
    memcpy(o, *p, 4);
    *p += 4;
    return true;
}

static bool take_i32(const unsigned char **p, const unsigned char *end, int32_t *o) {
    uint32_t u = 0;
    if (!take_u32(p, end, &u)) {
        return false;
    }
    memcpy(o, &u, 4);
    return true;
}

static void retarget(WaylandContext *w, bool entering) {
    ChromeHit hit;
    chrome_hit_test(w, s_ptr.focus, s_ptr.x, s_ptr.y, &hit);
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

static void release_cursor_memory(void) {
    if (s_ptr.cursor_map != nullptr && s_ptr.cursor_bytes > 0) {
        munmap(s_ptr.cursor_map, s_ptr.cursor_bytes);
    }
    s_ptr.cursor_map = nullptr;
    s_ptr.cursor_bytes = 0;
    if (s_ptr.cursor_fd >= 0) {
        close(s_ptr.cursor_fd);
        s_ptr.cursor_fd = -1;
    }
}

static void paint_arrow(uint32_t *px) {
    const uint32_t fg = 0xfff4f1eau;
    const uint32_t ol = 0xff16181cu;
    memset(px, 0, k_cursor_side * k_cursor_side * sizeof(uint32_t));
    for (uint32_t y = 0; y < 11u; ++y) {
        for (uint32_t x = 0; x <= y && x < 11u; ++x) {
            bool edge = (x == 0u || x == y || y == 10u);
            px[y * k_cursor_side + x] = edge ? ol : fg;
        }
    }
    for (uint32_t y = 11u; y < 16u; ++y) {
        for (uint32_t x = 3u; x <= 5u; ++x) {
            bool edge = (x == 3u || x == 5u || y == 15u);
            px[y * k_cursor_side + x] = edge ? ol : fg;
        }
    }
}

static int make_fallback_cursor(WaylandContext *w) {
    if (w->shm_id == 0 || !w->shm_argb8888 || w->compositor_id == 0) {
        fprintf(stderr, "[Pointer Error] No cursor-shape global and no wl_shm ARGB8888 for a cursor\n");
        return -1;
    }
    const uint32_t bytes = k_cursor_side * k_cursor_side * 4u;
    int fd = memfd_create("engine-cursor", MFD_CLOEXEC | MFD_NOEXEC_SEAL);
    if (fd < 0) {
        fd = memfd_create("engine-cursor", MFD_CLOEXEC);
    }
    if (fd < 0) {
        fprintf(stderr, "[Pointer Error] memfd_create cursor: %s\n", strerror(errno));
        return -1;
    }
    if (ftruncate(fd, (off_t)bytes) != 0) {
        close(fd);
        fprintf(stderr, "[Pointer Error] ftruncate cursor: %s\n", strerror(errno));
        return -1;
    }
    void *map = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        fprintf(stderr, "[Pointer Error] mmap cursor: %s\n", strerror(errno));
        return -1;
    }
    paint_arrow((uint32_t *)map);
    s_ptr.cursor_fd = fd;
    s_ptr.cursor_map = map;
    s_ptr.cursor_bytes = bytes;
    s_ptr.cursor_pool = wayland_alloc_id(w);
    s_ptr.cursor_buffer = wayland_alloc_id(w);
    s_ptr.cursor_surface = wayland_alloc_id(w);
    if (s_ptr.cursor_pool == 0 || s_ptr.cursor_buffer == 0 || s_ptr.cursor_surface == 0) {
        release_cursor_memory();
        return -1;
    }
    int32_t pool_payload[2] = { (int32_t)s_ptr.cursor_pool, (int32_t)bytes };
    int32_t buf_payload[6] = {
        (int32_t)s_ptr.cursor_buffer, 0, (int32_t)k_cursor_side, (int32_t)k_cursor_side,
        (int32_t)(k_cursor_side * 4u), 0,
    };
    uint32_t attach[3] = { s_ptr.cursor_buffer, 0u, 0u };
    int32_t damage[4] = { 0, 0, (int32_t)k_cursor_side, (int32_t)k_cursor_side };
    uint16_t damage_op = (w->compositor_bound_version >= 4u) ? 9u : 2u;
    uint32_t send_mark = w->send_len;
    if (!wayland_put_raw(w, w->shm_id, 0u, pool_payload, sizeof pool_payload) ||
        !wayland_put_raw(w, s_ptr.cursor_pool, 0u, buf_payload, sizeof buf_payload) ||
        !wayland_put_raw(w, w->compositor_id, 0u, &s_ptr.cursor_surface, sizeof s_ptr.cursor_surface) ||
        !wayland_put_raw(w, s_ptr.cursor_surface, 1u, attach, sizeof attach) ||
        !wayland_put_raw(w, s_ptr.cursor_surface, damage_op, damage, sizeof damage) ||
        !wayland_put_raw(w, s_ptr.cursor_surface, 6u, nullptr, 0u)) {
        wayland_rewind_send(w, send_mark);
        release_cursor_memory();
        return -1;
    }
    if (wayland_roundtrip_batch(w, fd) < 0) {
        fprintf(stderr, "[Pointer Error] Cursor surface failed: %s\n", w->err);
        release_cursor_memory();
        return -1;
    }
    printf("[Pointer] cursor-shape unavailable; using a client cursor surface\n");
    return 0;
}

static bool on_pointer(WaylandContext *w, uint16_t opcode,
                       const unsigned char *payload, uint16_t len) {
    const unsigned char *p = payload;
    const unsigned char *end = payload + len;
    if (opcode == 0u) {
        // wl_pointer.enter
        uint32_t serial = 0, surface = 0;
        int32_t fx = 0, fy = 0;
        if (!take_u32(&p, end, &serial) || !take_u32(&p, end, &surface) ||
            !take_i32(&p, end, &fx) || !take_i32(&p, end, &fy)) {
            wayland_fail(w, "truncated wl_pointer.enter");
            return true;
        }
        s_ptr.enter_serial = serial;
        s_ptr.have_enter = true;
        s_ptr.focus = surface;
        s_ptr.x = fixed_to_int(fx);
        s_ptr.y = fixed_to_int(fy);
        s_ptr.frame_pending_retarget = true;
        s_ptr.frame_entering = true;
        retarget(w, true);
        return true;
    }
    if (opcode == 1u) {
        // wl_pointer.leave
        s_ptr.focus = 0;
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
        if (!take_u32(&p, end, &time_ms) || !take_i32(&p, end, &fx) || !take_i32(&p, end, &fy)) {
            wayland_fail(w, "truncated wl_pointer.motion");
            return true;
        }
        (void)time_ms;
        if (s_ptr.focus == 0) {
            return true;
        }
        s_ptr.x = fixed_to_int(fx);
        s_ptr.y = fixed_to_int(fy);
        s_ptr.frame_pending_retarget = true;
        retarget(w, false);
        return true;
    }
    if (opcode == 3u) {
        // wl_pointer.button
        uint32_t serial = 0, time_ms = 0, button = 0, state = 0;
        if (!take_u32(&p, end, &serial) || !take_u32(&p, end, &time_ms) ||
            !take_u32(&p, end, &button) || !take_u32(&p, end, &state)) {
            wayland_fail(w, "truncated wl_pointer.button");
            return true;
        }
        (void)time_ms;
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
                s_spawn[s_spawn_n].x = s_ptr.x;
                s_spawn[s_spawn_n].y = s_ptr.y;
                s_spawn_n++;
            }
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
        return on_pointer(w, opcode, payload, len);
    }
    return false;
}

int pointer_open_devices(WaylandContext *w) {
    if (w == nullptr) {
        return -EINVAL;
    }
    if (w->seat_id == 0) {
        fprintf(stderr, "[Pointer Error] wl_seat is required so the cursor can be set\n");
        return -1;
    }
    if ((w->seat_capabilities & 1u) == 0u) {
        fprintf(stderr, "[Pointer Error] wl_seat has no pointer capability (mask=0x%x)\n",
                w->seat_capabilities);
        return -1;
    }
    w->pointer_id = wayland_alloc_id(w);
    if (w->pointer_id == 0 || !wayland_put_raw(w, w->seat_id, 0u, &w->pointer_id, sizeof w->pointer_id)) {
        return -1;
    }
    if (w->cursor_shape_manager_id != 0) {
        w->cursor_shape_device_id = wayland_alloc_id(w);
        uint32_t payload[2] = { w->cursor_shape_device_id, w->pointer_id };
        // wp_cursor_shape_manager_v1.get_pointer is request 1.
        if (w->cursor_shape_device_id == 0 ||
            !wayland_put_raw(w, w->cursor_shape_manager_id, 1u, payload, sizeof payload)) {
            return -1;
        }
    }
    if (wayland_roundtrip_batch(w, -1) < 0) {
        fprintf(stderr, "[Pointer Error] Pointer bind failed: %s\n", w->err);
        return -1;
    }
    if (w->cursor_shape_device_id == 0 && make_fallback_cursor(w) != 0) {
        return -1;
    }
    printf("[Pointer] seat=%u pointer=%u cursor-shape=%u shm_argb=%s\n",
           w->seat_id,
           w->pointer_id,
           w->cursor_shape_device_id,
           w->shm_argb8888 ? "yes" : "no");
    return 0;
}

int pointer_check_spec(const WaylandContext *w) {
    const ChromeSpec *spec = chrome_spec();
    if (w == nullptr || spec == nullptr) {
        return -EINVAL;
    }
    if (w->cursor_shape_device_id == 0) {
        return 0;
    }
    uint32_t max_shape = (w->cursor_shape_bound_version >= 2u) ? 36u : 34u;
    if (spec->cursor_shape == 0 || spec->cursor_shape > max_shape) {
        fprintf(stderr, "[Pointer Error] cursor.shape %u is not in cursor-shape v%u\n",
                spec->cursor_shape, w->cursor_shape_bound_version);
        return -1;
    }
    for (uint32_t i = 0; i < spec->ncontrols; ++i) {
        if (spec->controls[i].has_cursor &&
            (spec->controls[i].cursor_shape == 0 || spec->controls[i].cursor_shape > max_shape)) {
            fprintf(stderr, "[Pointer Error] Control '%s' cursor %u is not in cursor-shape v%u\n",
                    spec->controls[i].name, spec->controls[i].cursor_shape,
                    w->cursor_shape_bound_version);
            return -1;
        }
    }
    return 0;
}

int pointer_encode(WaylandContext *w) {
    if (w == nullptr) {
        return -EINVAL;
    }
    if (w->send_len != 0) {
        return -EBUSY;
    }
    uint32_t send_mark = w->send_len;
    bool any = false;
    if (s_ptr.cursor_dirty && s_ptr.have_enter && s_ptr.focus != 0) {
        bool wrote = false;
        if (w->cursor_shape_device_id != 0) {
            uint32_t payload[2] = { s_ptr.enter_serial, s_ptr.wanted_shape };
            // wp_cursor_shape_device_v1.set_shape is request 1.
            wrote = wayland_put_raw(w, w->cursor_shape_device_id, 1u, payload, sizeof payload);
        } else if (s_ptr.cursor_surface != 0 && w->pointer_id != 0) {
            uint32_t payload[4] = { s_ptr.enter_serial, s_ptr.cursor_surface, 0u, 0u };
            // wl_pointer.set_cursor is request 0. Hotspot is the arrow tip.
            wrote = wayland_put_raw(w, w->pointer_id, 0u, payload, sizeof payload);
        }
        if (!wrote) {
            wayland_rewind_send(w, send_mark);
            s_ptr.cursor_inflight = false;
            return -EIO;
        }
        s_ptr.cursor_inflight = true;
        s_ptr.inflight_shape  = s_ptr.wanted_shape;
        s_ptr.inflight_serial = s_ptr.enter_serial;
        any = true;
    }
    if (s_ptr.resize_pending && w->xdg_toplevel_id != 0 && w->seat_id != 0) {
        uint32_t payload[3] = { w->seat_id, s_ptr.resize_serial, s_ptr.resize_edge };
        // xdg_toplevel.resize is request 6.
        if (!wayland_put_raw(w, w->xdg_toplevel_id, 6u, payload, sizeof payload)) {
            wayland_rewind_send(w, send_mark);
            s_ptr.cursor_inflight = false;
            return -EIO;
        }
        s_ptr.resize_pending = false;
        any = true;
    }
    if (s_ptr.move_pending && w->xdg_toplevel_id != 0 && w->seat_id != 0) {
        uint32_t payload[2] = { w->seat_id, s_ptr.move_serial };
        // xdg_toplevel.move is request 5.
        if (!wayland_put_raw(w, w->xdg_toplevel_id, 5u, payload, sizeof payload)) {
            wayland_rewind_send(w, send_mark);
            s_ptr.cursor_inflight = false;
            return -EIO;
        }
        s_ptr.move_pending = false;
        any = true;
    }
    return any ? 1 : 0;
}

void pointer_on_send_done(bool ok) {
    if (s_ptr.cursor_inflight) {
        if (ok) {
            s_ptr.last_shape = s_ptr.inflight_shape;
            s_ptr.have_last = true;
            if (s_ptr.wanted_shape == s_ptr.last_shape && s_ptr.enter_serial == s_ptr.inflight_serial) {
                s_ptr.cursor_dirty = false;
            }
        }
        s_ptr.cursor_inflight = false;
    }
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

int pointer_flush(WaylandContext *w) {
    int enc = pointer_encode(w);
    if (enc <= 0) {
        return enc;
    }
    if (wayland_send_batch(w) < 0) {
        pointer_on_send_done(false);
        return -1;
    }
    pointer_on_send_done(true);
    return 0;
}

void pointer_shutdown(WaylandContext *w) {
    if (w != nullptr && w->is_initialized && !w->failed) {
        if (w->send_len > 0) {
            (void)wayland_send_batch(w);
        }
        if (s_ptr.cursor_surface != 0) {
            (void)wayland_put_raw(w, s_ptr.cursor_surface, 0u, nullptr, 0u);
            s_ptr.cursor_surface = 0;
        }
        if (s_ptr.cursor_buffer != 0) {
            (void)wayland_put_raw(w, s_ptr.cursor_buffer, 0u, nullptr, 0u);
            s_ptr.cursor_buffer = 0;
        }
        if (s_ptr.cursor_pool != 0) {
            (void)wayland_put_raw(w, s_ptr.cursor_pool, 1u, nullptr, 0u);
            s_ptr.cursor_pool = 0;
        }
        if (w->cursor_shape_device_id != 0) {
            (void)wayland_put_raw(w, w->cursor_shape_device_id, 0u, nullptr, 0u);
            w->cursor_shape_device_id = 0;
        }
        if (w->pointer_id != 0 && w->seat_bound_version >= 3u) {
            (void)wayland_put_raw(w, w->pointer_id, 1u, nullptr, 0u);
            w->pointer_id = 0;
        }
        if (w->send_len > 0 && !w->failed) {
            (void)wayland_send_batch(w);
        }
    }
    release_cursor_memory();
    s_ptr.cursor_surface = 0;
    s_ptr.cursor_pool = 0;
    s_ptr.cursor_buffer = 0;
    s_ptr.cursor_dirty = false;
    s_ptr.frame_pending_retarget = false;
    s_ptr.frame_entering = false;
    s_ptr.resize_pending = false;
    s_ptr.move_pending = false;
    s_ptr.have_enter = false;
    s_ptr.focus = 0;
    s_spawn_n = 0;
}