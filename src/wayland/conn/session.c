// Display socket, registry bind, and shutdown.
#include "wayland/internal.h"
#include "input/pointer.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static WaylandContext s_wl_ctx = { .feedback_table_fd = -1 };

static int queue_registry_send(WaylandContext *w);
static int finish_linked_handshake(WaylandContext *w);


void wl_arm_recv(WaylandContext *w) {
    struct io_uring_sqe *sqe = ring_sqe(w->ring);
    if (sqe == nullptr) {
        wl_fail(w, "SQ full while arming multishot recvmsg");
        return;
    }
    sqe->opcode    = IORING_OP_RECVMSG;
    sqe->flags     = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    sqe->ioprio    = IORING_RECV_MULTISHOT;
    sqe->fd        = ENG_WL_SOCK_SLOT;
    sqe->addr      = (uint64_t)(uintptr_t)&w->recv_msg;
    sqe->msg_flags = MSG_CMSG_CLOEXEC;
    sqe->buf_group = ENG_PBUF_BGID;
    sqe->user_data = ENG_UD_WL_RECV;
    w->recv_armed  = true;
}


int wayland_ensure_recv(WaylandContext *w) {
    if (w->failed) return -EIO;
    if (w->recv_msg.msg_iov == nullptr) return -EINVAL;
    if (w->recv_armed) return 0;
    wl_arm_recv(w);
    return w->failed ? -EIO : 0;
}


static int queue_registry_send(WaylandContext *w) {
    struct io_uring_sqe *sqe = ring_sqe(w->ring);
    if (sqe == nullptr) {
        wl_fail(w, "SQ full while sending the registry");
        return -ENOSPC;
    }
    sqe->opcode    = IORING_OP_SEND;
    sqe->flags     = IOSQE_FIXED_FILE;
    sqe->ioprio    = IORING_RECVSEND_POLL_FIRST;
    sqe->fd        = ENG_WL_SOCK_SLOT;
    sqe->addr      = (uint64_t)(uintptr_t)(w->send_bytes + w->send_off);
    sqe->len       = w->send_len - w->send_off;
    sqe->msg_flags = MSG_NOSIGNAL;
    sqe->user_data = ENG_UD_WL_SEND;
    return 0;
}


static int finish_linked_handshake(WaylandContext *w) {
    uint64_t deadline = ring_mono_ns() + 3000000000ull;
    bool send_done = false;
    for (;;) {
        struct io_uring_cqe cqe;
        while (ring_peek(w->ring, &cqe)) {
            if (cqe.user_data == ENG_UD_WL_SOCKET) {
                if (cqe.res != 0) {
                    fprintf(stderr, "[Wayland Error] IORING_OP_SOCKET fixed slot 0 failed: %d\n", cqe.res);
                    return -EIO;
                }
                continue;
            }
            if (cqe.user_data == ENG_UD_WL_CONNECT) {
                if (cqe.res < 0) {
                    fprintf(stderr, "[Wayland Error] IORING_OP_CONNECT failed: %s (%d)\n",
                            strerror(-cqe.res), cqe.res);
                    return -EIO;
                }
                continue;
            }
            if (cqe.user_data == ENG_UD_WL_SEND) {
                if (cqe.res == -ECANCELED) {
                    fprintf(stderr, "[Wayland Error] Handshake link canceled before the registry send\n");
                    return -EIO;
                }
                int progress = wl_note_send_progress(w, cqe.res);
                if (progress < 0) {
                    return progress;
                }
                if (progress == 0) {
                    send_done = true;
                    continue;
                }
                if (queue_registry_send(w) != 0) {
                    return -EIO;
                }
                continue;
            }
            wayland_on_cqe(w, &cqe);
            if (w->failed) {
                return -EIO;
            }
        }
        if (send_done && w->ring->sq_pending == 0) {
            return 0;
        }
        if (ring_mono_ns() > deadline) {
            wl_fail(w, "wayland handshake timed out");
            return -ETIMEDOUT;
        }
        unsigned n = ring_submit(w->ring);
        int rc = ring_enter(w->ring, n, 1, 200);
        if (rc < 0 && rc != -ETIME) {
            wl_fail_err(w, "io_uring_enter", rc);
            return rc;
        }
    }
}


int wayland_init(void) {
    RingContext *r = ring_get_context();
    if (r == nullptr || !r->is_initialized) {
        fprintf(stderr, "[Wayland Error] Primary io_uring must be initialized before wayland_init\n");
        return 1;
    }
    WaylandContext *w = &s_wl_ctx;
    *w = (WaylandContext){
        .ring              = r,
        .display_id        = 1,
        .next_id           = 2,
        .feedback_table_fd = -1,
    };
    w->registry_id = wayland_alloc_id(w);
    w->sync_id     = wayland_alloc_id(w);

    const char *dir  = getenv("XDG_RUNTIME_DIR");
    const char *disp = getenv("WAYLAND_DISPLAY");
    if (disp == nullptr || disp[0] == '\0') {
        fprintf(stderr, "[Wayland Error] WAYLAND_DISPLAY is not set\n");
        return 1;
    }

    int path_n;
    if (disp[0] == '/') {
        path_n = snprintf(w->path, sizeof w->path, "%s", disp);
    } else if (dir == nullptr || dir[0] == '\0') {
        fprintf(stderr, "[Wayland Error] XDG_RUNTIME_DIR or WAYLAND_DISPLAY is not set\n");
        return 1;
    } else {
        path_n = snprintf(w->path, sizeof w->path, "%s/%s", dir, disp);
    }
    if (path_n < 0 || (size_t)path_n >= sizeof w->path ||
        (size_t)path_n >= sizeof w->addr.sun_path) {
        fprintf(stderr, "[Wayland Error] Wayland socket path exceeds sockaddr_un limit\n");
        return 1;
    }
    if (access(w->path, F_OK) != 0) {
        fprintf(stderr, "[Wayland Error] Socket '%s' not accessible: %s\n", w->path, strerror(errno));
        return 1;
    }

    w->addr.sun_family = AF_UNIX;
    memcpy(w->addr.sun_path, w->path, (size_t)path_n + 1u);
    w->addr_len = (unsigned)(offsetof(struct sockaddr_un, sun_path) + (size_t)path_n + 1u);

    // Socket, connect, and the registry send share one enter. The send CQE stays
    // visible so a short write can be retried. Recv is armed after that send.
    w->send_len = 0;
    w->send_off = 0;
    (void)wayland_put_raw(w, w->display_id, 1u, &w->registry_id, sizeof(w->registry_id));
    (void)wl_append_display_sync(w, w->sync_id);

    w->control_reserve         = 256u;
    w->recv_iov.iov_base       = r->bufs;
    w->recv_iov.iov_len        = ENG_PBUF_LEN;
    w->recv_msg.msg_iov        = &w->recv_iov;
    w->recv_msg.msg_iovlen     = 1u;
    w->recv_msg.msg_control    = w->recv_control;
    w->recv_msg.msg_controllen = w->control_reserve;

    uint32_t held = r->sq_pending;
    struct io_uring_sqe *sock = ring_sqe(r);
    struct io_uring_sqe *conn = ring_sqe(r);
    struct io_uring_sqe *send = ring_sqe(r);
    if (sock == nullptr || conn == nullptr || send == nullptr || w->send_len == 0) {
        r->sq_pending = held;
        fprintf(stderr, "[Wayland Error] Registry handshake was not queued\n");
        return 1;
    }
    sock->opcode     = IORING_OP_SOCKET;
    sock->flags      = IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS;
    sock->fd         = AF_UNIX;
    sock->off        = SOCK_STREAM | SOCK_NONBLOCK;
    sock->file_index = ENG_WL_SOCK_SLOT + 1u;
    sock->user_data  = ENG_UD_WL_SOCKET;

    conn->opcode    = IORING_OP_CONNECT;
    conn->flags     = IOSQE_FIXED_FILE | IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS;
    conn->fd        = ENG_WL_SOCK_SLOT;
    conn->addr      = (uint64_t)(uintptr_t)&w->addr;
    conn->addr2     = w->addr_len;
    conn->user_data = ENG_UD_WL_CONNECT;

    send->opcode    = IORING_OP_SEND;
    send->flags     = IOSQE_FIXED_FILE;
    send->ioprio    = IORING_RECVSEND_POLL_FIRST;
    send->fd        = ENG_WL_SOCK_SLOT;
    send->addr      = (uint64_t)(uintptr_t)w->send_bytes;
    send->len       = w->send_len;
    send->msg_flags = MSG_NOSIGNAL;
    send->user_data = ENG_UD_WL_SEND;

    if (finish_linked_handshake(w) != 0) {
        fprintf(stderr, "[Wayland Error] Registry handshake send failed: %s\n",
                w->err[0] != '\0' ? w->err : "socket, connect, or registry send");
        return 1;
    }
    w->fixed_socket_installed = true;

    struct io_uring_cqe cqe = {0};
    wl_arm_recv(w);
    while (!w->synced && !w->failed) {
        if (wl_wait_tag(w, ENG_UD_WL_RECV, &cqe) < 0) {
            break;
        }
        wayland_on_cqe(w, &cqe);
    }
    if (w->failed || !w->synced) {
        fprintf(stderr, "[Wayland Error] Wire registry sync failed: %s\n", w->err);
        return 1;
    }

    if (w->compositor_global_name == 0 || w->xdg_wm_base_global_name == 0 ||
        w->linux_dmabuf_global_name == 0) {
        fprintf(stderr,
                "[Wayland Error] Missing required compositor globals (compositor=%u, xdg_wm_base=%u, linux_dmabuf=%u)\n",
                w->compositor_global_name, w->xdg_wm_base_global_name, w->linux_dmabuf_global_name);
        return 1;
    }
    if (w->linux_dmabuf_version < 4u) {
        fprintf(stderr,
                "[Wayland Error] zwp_linux_dmabuf_v1 version is %u, but engine requires v4+\n",
                w->linux_dmabuf_version);
        return 1;
    }

    // 5. Bind the core interfaces and verify with a second wl_display.sync roundtrip
    w->compositor_id            = wayland_alloc_id(w);
    w->compositor_bound_version = wl_min_u32(w->compositor_version, 6u);
    (void)wl_append_registry_bind(w, w->compositor_global_name, "wl_compositor",
                               w->compositor_bound_version, w->compositor_id);

    w->xdg_wm_base_id = wayland_alloc_id(w);
    (void)wl_append_registry_bind(w, w->xdg_wm_base_global_name, "xdg_wm_base",
                               wl_min_u32(w->xdg_wm_base_version, 6u), w->xdg_wm_base_id);

    w->linux_dmabuf_id            = wayland_alloc_id(w);
    w->linux_dmabuf_bound_version = wl_min_u32(w->linux_dmabuf_version, 4u);
    (void)wl_append_registry_bind(w, w->linux_dmabuf_global_name, "zwp_linux_dmabuf_v1",
                               w->linux_dmabuf_bound_version, w->linux_dmabuf_id);

    if (w->drm_syncobj_global_name != 0) {
        w->drm_syncobj_id = wayland_alloc_id(w);
        (void)wl_append_registry_bind(w, w->drm_syncobj_global_name, "wp_linux_drm_syncobj_manager_v1",
                                   1u, w->drm_syncobj_id);
    }

    if (w->seat_global_name != 0) {
        w->seat_id = wayland_alloc_id(w);
        w->seat_bound_version = wl_min_u32(w->seat_version, 9u);
        (void)wl_append_registry_bind(w, w->seat_global_name, "wl_seat",
                                   w->seat_bound_version, w->seat_id);
    }
    // The bar is cleared into the dma-buf. wl_shm is only the cursor fallback.
    if (w->cursor_shape_global_name == 0 && w->shm_global_name != 0) {
        w->shm_id = wayland_alloc_id(w);
        (void)wl_append_registry_bind(w, w->shm_global_name, "wl_shm",
                                   wl_min_u32(w->shm_version, 1u), w->shm_id);
    }
    if (w->cursor_shape_global_name != 0) {
        w->cursor_shape_manager_id = wayland_alloc_id(w);
        w->cursor_shape_bound_version = wl_min_u32(w->cursor_shape_version, 2u);
        (void)wl_append_registry_bind(w, w->cursor_shape_global_name, "wp_cursor_shape_manager_v1",
                                   w->cursor_shape_bound_version, w->cursor_shape_manager_id);
    }
    if (w->viewporter_global_name != 0) {
        w->viewporter_id = wayland_alloc_id(w);
        (void)wl_append_registry_bind(w, w->viewporter_global_name, "wp_viewporter",
                                   wl_min_u32(w->viewporter_version, 1u), w->viewporter_id);
    }
    if (w->subcompositor_global_name != 0) {
        w->subcompositor_id = wayland_alloc_id(w);
        (void)wl_append_registry_bind(w, w->subcompositor_global_name, "wl_subcompositor",
                                   wl_min_u32(w->subcompositor_version, 1u), w->subcompositor_id);
    }
    if (!wl_bind_decoration_manager(w)) {
        fprintf(stderr, "[Wayland Error] Decoration manager bind failed: %s\n", w->err);
        return 1;
    }

    w->synced  = false;
    w->sync_id = wayland_alloc_id(w);
    (void)wl_append_display_sync(w, w->sync_id);

    if (wl_send_batch(w) < 0) {
        fprintf(stderr, "[Wayland Error] Global bind batch failed: %s\n", w->err);
        return 1;
    }

    while (!w->synced && !w->failed) {
        if (wl_wait_tag(w, ENG_UD_WL_RECV, &cqe) < 0) {
            break;
        }
        wayland_on_cqe(w, &cqe);
    }
    if (w->failed || !w->synced) {
        fprintf(stderr, "[Wayland Error] Global bind verification failed: %s\n", w->err);
        return 1;
    }

    if (w->ring->sq_pending > 0) {
        unsigned n = ring_submit(r);
        (void)ring_enter(r, n, 0, -1);
    }

    w->send_len = 0;
    if (pointer_open_devices(w) != 0) {
        return 1;
    }

    w->is_initialized = true;
    printf("[Wayland] Wire connected (fixed slot 0: %s) | Globals: %u\n",
           w->path, w->globals_seen);
    printf("[Wayland] Bound Object IDs: wl_compositor=%u, xdg_wm_base=%u, linux_dmabuf=%u, drm_syncobj=%u, decoration=%u\n",
           w->compositor_id,
           w->xdg_wm_base_id,
           w->linux_dmabuf_id,
           w->drm_syncobj_id,
           w->decoration_manager_id);
    printf("[Wayland] Input Object IDs: wl_seat=%u, wl_shm=%u, cursor_shape=%u, viewporter=%u\n",
           w->seat_id, w->shm_id, w->cursor_shape_manager_id, w->viewporter_id);
    return 0;
}


void wayland_shutdown(void) {
    WaylandContext *w = &s_wl_ctx;
    if (!w->is_initialized) {
        return;
    }
    pointer_shutdown(w);
    wl_close_feedback_table(w);
    for (uint32_t i = 0; i < w->nparked; ++i) {
        uint32_t slot = (w->parked_head + i) & (ENG_FD_PARK - 1u);
        close(w->parked_fds[slot]);
    }
    w->parked_head    = 0;
    w->nparked        = 0;
    w->is_initialized = false;
    printf("goodbye from wayland\n");
}


WaylandContext *wayland_get_context(void) {
    return &s_wl_ctx;
}

