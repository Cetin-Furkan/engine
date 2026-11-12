#include "input/internal.h"
#include "wayland/internal.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static constexpr uint32_t k_cursor_side = 16u;
static constexpr int k_cursor_seals =
    F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL;

void cursor_release_memory(void) {
    if (s_ptr.cursor_map != nullptr && s_ptr.cursor_bytes > 0) {
        munmap(s_ptr.cursor_map, s_ptr.cursor_bytes);
    }
    s_ptr.cursor_map = nullptr;
    s_ptr.cursor_bytes = 0;
    s_ptr.cursor_sealed = false;
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

static int cursor_memfd(bool *can_seal) {
    *can_seal = false;
    int fd = memfd_create("engine-cursor", MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL);
    if (fd >= 0) {
        *can_seal = true;
        return fd;
    }
    fd = memfd_create("engine-cursor", MFD_CLOEXEC | MFD_NOEXEC_SEAL);
    if (fd >= 0) {
        return fd;
    }
    return memfd_create("engine-cursor", MFD_CLOEXEC);
}

// F_SEAL_WRITE is rejected while this process still holds a writable map.
// Paint, drop the map, then seal. The compositor maps the fd itself.
static bool seal_cursor(int fd) {
    if (fcntl(fd, F_ADD_SEALS, k_cursor_seals) != 0) {
        return false;
    }
    int got = fcntl(fd, F_GET_SEALS);
    return got >= 0 && (got & k_cursor_seals) == k_cursor_seals;
}

int cursor_make_fallback(WaylandContext *w) {
    if (w->shm_id == 0 || !w->shm_argb8888 || w->compositor_id == 0) {
        fprintf(stderr, "[Pointer Error] No cursor-shape global and no wl_shm ARGB8888 for a cursor\n");
        return -1;
    }
    const uint32_t bytes = k_cursor_side * k_cursor_side * 4u;
    bool can_seal = false;
    int fd = cursor_memfd(&can_seal);
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

    bool sealed = false;
    if (can_seal && munmap(map, bytes) == 0) {
        map = nullptr;
        sealed = seal_cursor(fd);
    }
    s_ptr.cursor_fd = fd;
    s_ptr.cursor_map = map;
    s_ptr.cursor_bytes = map != nullptr ? bytes : 0u;
    s_ptr.cursor_sealed = sealed;

    uint32_t pool_id = wayland_alloc_id(w);
    uint32_t buf_id  = wayland_alloc_id(w);
    uint32_t surf_id = wayland_alloc_id(w);
    if (pool_id == 0 || buf_id == 0 || surf_id == 0) {
        wl_release_unsent(w, surf_id);
        wl_release_unsent(w, buf_id);
        wl_release_unsent(w, pool_id);
        cursor_release_memory();
        return -1;
    }
    int32_t pool_payload[2] = { (int32_t)pool_id, (int32_t)bytes };
    int32_t buf_payload[6] = {
        (int32_t)buf_id, 0, (int32_t)k_cursor_side, (int32_t)k_cursor_side,
        (int32_t)(k_cursor_side * 4u), 0,
    };
    uint32_t attach[3] = { buf_id, 0u, 0u };
    int32_t damage[4] = { 0, 0, (int32_t)k_cursor_side, (int32_t)k_cursor_side };
    uint16_t damage_op = (w->compositor_bound_version >= 4u) ? 9u : 2u;
    uint32_t send_mark = w->send_len;
    if (!wayland_put_raw(w, w->shm_id, 0u, pool_payload, sizeof pool_payload) ||
        !wayland_put_raw(w, pool_id, 0u, buf_payload, sizeof buf_payload) ||
        !wayland_put_raw(w, w->compositor_id, 0u, &surf_id, sizeof surf_id) ||
        !wayland_put_raw(w, surf_id, 1u, attach, sizeof attach) ||
        !wayland_put_raw(w, surf_id, damage_op, damage, sizeof damage) ||
        !wayland_put_raw(w, surf_id, 6u, nullptr, 0u)) {
        wayland_rewind_send(w, send_mark);
        wl_release_unsent(w, surf_id);
        wl_release_unsent(w, buf_id);
        wl_release_unsent(w, pool_id);
        cursor_release_memory();
        return -1;
    }
    s_ptr.cursor_pool    = pool_id;
    s_ptr.cursor_buffer  = buf_id;
    s_ptr.cursor_surface = surf_id;
    if (wayland_roundtrip_batch(w, fd) < 0) {
        fprintf(stderr, "[Pointer Error] Cursor surface failed: %s\n", w->err);
        s_ptr.cursor_pool    = 0;
        s_ptr.cursor_buffer  = 0;
        s_ptr.cursor_surface = 0;
        cursor_release_memory();
        return -1;
    }
    printf("[Pointer] cursor-shape unavailable; using a client cursor surface%s\n",
           sealed ? " (memfd sealed)" : "");
    return 0;
}
