/* === FILE: ./include/init/wayland_init/wayland_init.h === */

#ifndef WAYLAND_INIT_H
#define WAYLAND_INIT_H

#include "init/ring_init/ring_init.h"
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>

enum : uint32_t {
    ENG_WL_GLOBALS = 96,
    ENG_WL_NAME    = 128,
    ENG_FD_PARK    = 32,
    ENG_ID_FREE    = 64,
};

typedef struct WaylandGlobal {
    uint32_t name;
    uint32_t version;
    char     interface[ENG_WL_NAME];
} WaylandGlobal;

typedef struct WaylandContext {
    RingContext *ring;
    bool         fixed_socket_installed;
    bool         recv_armed;
    bool         synced;
    bool         failed;
    char         err[160];

    uint32_t display_id;
    uint32_t registry_id;
    uint32_t sync_id;
    uint32_t next_id;
    uint32_t free_id[ENG_ID_FREE];
    uint32_t nfree;

    // Discovered global registry names & versions
    uint32_t compositor_global_name;
    uint32_t compositor_version;
    uint32_t subcompositor_global_name;
    uint32_t subcompositor_version;
    uint32_t xdg_wm_base_global_name;
    uint32_t xdg_wm_base_version;
    uint32_t linux_dmabuf_global_name;
    uint32_t linux_dmabuf_version;
    uint32_t drm_syncobj_global_name;
    uint32_t drm_syncobj_version;

    // Bound client object IDs ready for surface/popup/DMA-BUF/syncobj requests
    uint32_t compositor_id;
    uint32_t subcompositor_id;
    uint32_t xdg_wm_base_id;
    uint32_t linux_dmabuf_id;
    uint32_t drm_syncobj_id;

    uint32_t      control_reserve;
    struct msghdr recv_msg;
    struct iovec  recv_iov;
    unsigned char recv_control[256];

    struct msghdr send_msg;
    struct iovec  send_iov;
    unsigned char send_bytes[512];
    uint32_t      send_len;

    struct sockaddr_un addr;
    unsigned           addr_len;
    char               path[108];

    unsigned char stream[16384];
    uint32_t      stream_len;

    WaylandGlobal globals[ENG_WL_GLOBALS];
    uint32_t      nglobals;
    uint32_t      globals_seen;

    int      parked_fds[ENG_FD_PARK];
    uint32_t nparked;
    uint32_t recv_cqes;
    bool     is_initialized;
} WaylandContext;

[[nodiscard]] int wayland_init(void);
void wayland_shutdown(void);
[[nodiscard]] WaylandContext *wayland_get_context(void);
[[nodiscard]] uint32_t wayland_alloc_id(WaylandContext *w);
void wayland_on_cqe(WaylandContext *w, const struct io_uring_cqe *cqe);
[[nodiscard]] int wayland_ensure_recv(WaylandContext *w);

#endif /* WAYLAND_INIT_H */