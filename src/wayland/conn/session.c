// Display socket, registry bind, and shutdown.
#include "wayland/internal.h"
#include "input/pointer.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static WaylandContext s_wl_ctx = { .feedback_table_fd = -1 };

static constexpr uint64_t k_ud_wl_cancel = 0x574c434e434c0000ULL;

static void cleanup_wayland_resources(WaylandContext *w) {
    if (w == nullptr) {
        return;
    }
    wl_close_feedback_table(w);

    if (w->ring != nullptr && w->ring->is_initialized) {
        uint32_t cancels = 0u;
        if (w->recv_armed) {
            struct io_uring_sqe *sqe = ring_sqe(w->ring);
            if (sqe != nullptr) {
                sqe->opcode    = IORING_OP_ASYNC_CANCEL;
                sqe->addr      = ENG_UD_WL_RECV;
                sqe->user_data = k_ud_wl_cancel + 1u;
                cancels++;
            }
        }
        if (w->tx_inflight_tag != 0u) {
            struct io_uring_sqe *sqe = ring_sqe(w->ring);
            if (sqe != nullptr) {
                sqe->opcode    = IORING_OP_ASYNC_CANCEL;
                sqe->addr      = w->tx_inflight_tag;
                sqe->user_data = k_ud_wl_cancel + 2u;
                cancels++;
            }
        }
        if (cancels > 0u) {
            uint64_t deadline = ring_mono_ns() + 100000000ull;
            while ((cancels > 0u || w->recv_armed || w->tx_inflight_tag != 0u) &&
                   ring_mono_ns() < deadline) {
                struct io_uring_cqe cqe;
                bool progressed = false;
                while (ring_peek(w->ring, &cqe)) {
                    progressed = true;
                    if (cqe.user_data == k_ud_wl_cancel + 1u ||
                        cqe.user_data == k_ud_wl_cancel + 2u) {
                        if (cancels > 0u) {
                            cancels--;
                        }
                        continue;
                    }
                    if (cqe.user_data == ENG_UD_WL_RECV) {
                        if ((cqe.flags & IORING_CQE_F_BUFFER) != 0) {
                            uint16_t bid = (uint16_t)(cqe.flags >> IORING_CQE_BUFFER_SHIFT);
                            ring_pbuf_finish(w->ring, bid, cqe.flags, cqe.res);
                        }
                        w->recv_armed = (cqe.flags & IORING_CQE_F_MORE) != 0;
                        continue;
                    }
                    if (cqe.user_data == ENG_UD_WL_SEND || cqe.user_data == ENG_UD_WL_PONG) {
                        w->tx_inflight_tag = 0u;
                        w->send_queued     = false;
                        w->pong_in_flight  = false;
                        continue;
                    }
                    if (w->ring->default_cqe_fn != nullptr) {
                        w->ring->default_cqe_fn(&cqe, w->ring->default_cqe_user);
                    }
                }
                if (cancels == 0u && !w->recv_armed && w->tx_inflight_tag == 0u) {
                    break;
                }
                if (!progressed) {
                    unsigned sub = ring_submit(w->ring);
                    int rc = ring_enter(w->ring, sub, 1u, 20);
                    if (rc < 0 && rc != -ETIME && rc != -EINTR) {
                        break;
                    }
                }
            }
        }
        if (w->fixed_socket_installed) {
            (void)ring_update_file(w->ring, ENG_WL_SOCK_SLOT, -1);
            w->fixed_socket_installed = false;
        }
    }

    for (uint32_t i = 0; i < w->tx_pkt_count; ++i) {
        uint32_t idx = (w->tx_pkt_head + i) & (ENG_WL_SEND_PKT_CAP - 1u);
        WaylandSendPacket *pkt = &w->tx_pkts[idx];
        if (pkt->close_fds_on_done) {
            for (uint8_t f = 0; f < pkt->fd_count; ++f) {
                if (pkt->fds[f] >= 0) {
                    close(pkt->fds[f]);
                    pkt->fds[f] = -1;
                }
            }
            pkt->close_fds_on_done = false;
            pkt->fd_count          = 0u;
        }
    }

    w->recv_armed      = false;
    w->tx_inflight_tag = 0u;
    w->send_queued     = false;
    w->pong_in_flight  = false;
    w->tx_pkt_count    = 0u;
    w->tx_pkt_head     = 0u;
    w->tx_head         = 0u;
    w->tx_tail         = 0u;

    for (uint32_t i = 0; i < w->nparked; ++i) {
        uint32_t slot = (w->parked_head + i) & (ENG_FD_PARK - 1u);
        if (w->parked_fds[slot] >= 0) {
            close(w->parked_fds[slot]);
        }
    }
    w->parked_head = 0u;
    w->nparked     = 0u;

    if (w->free_ids != nullptr && w->free_ids != w->free_id_inline) {
        free(w->free_ids);
    }
    w->free_ids = w->free_id_inline;
    w->free_cap = ENG_ID_FREE;
    w->nfree    = 0u;

    if (w->destroy_ids != nullptr && w->destroy_ids != w->destroy_inline) {
        free(w->destroy_ids);
    }
    w->destroy_ids     = w->destroy_inline;
    w->destroy_cap     = ENG_WL_DESTROY_LATER;
    w->destroy_later_n = 0u;
}

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


static int finish_linked_handshake(WaylandContext *w) {
    uint64_t deadline = ring_mono_ns() + 3000000000ull;
    bool send_done = false;
    bool sock_failed = false;
    for (;;) {
        struct io_uring_cqe cqe;
        while (ring_peek(w->ring, &cqe)) {
            if (cqe.user_data == ENG_UD_WL_SOCKET) {
                if (cqe.res != 0) {
                    sock_failed = true;
                    fprintf(stderr, "[Wayland Error] IORING_OP_SOCKET fixed slot 0 failed: %d\n", cqe.res);
                    return -EIO;
                }
                w->fixed_socket_installed = true;
                continue;
            }
            if (cqe.user_data == ENG_UD_WL_CONNECT) {
                if (!sock_failed) {
                    w->fixed_socket_installed = true;
                }
                if (cqe.res < 0) {
                    fprintf(stderr, "[Wayland Error] IORING_OP_CONNECT failed: %s (%d)\n",
                            strerror(-cqe.res), cqe.res);
                    return -EIO;
                }
                continue;
            }
            if (cqe.user_data == ENG_UD_WL_SEND) {
                if (!sock_failed) {
                    w->fixed_socket_installed = true;
                }
                w->tx_inflight_tag = 0u;
                w->send_queued     = false;
                if (cqe.res == -ECANCELED) {
                    fprintf(stderr, "[Wayland Error] Handshake link canceled before the registry send\n");
                    return -EIO;
                }
                int progress = wl_tx_step_head(w, cqe.res);
                if (progress < 0) {
                    return progress;
                }
                if (progress == 0) {
                    send_done = true;
                    continue;
                }
                w->tx_pkts[w->tx_pkt_head].kick_armed = true;
                if (wl_tx_kick(w) != 0) {
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
            w->fixed_socket_installed = true;
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
        .scale_120         = 120u,
        .scene_vp_w        = -1,
        .scene_vp_h        = -1,
        .bar_vp_w          = -1,
        .bar_vp_h          = -1,
        .feedback_table_fd = -1,
    };
    w->free_ids    = w->free_id_inline;
    w->free_cap    = ENG_ID_FREE;
    w->destroy_ids = w->destroy_inline;
    w->destroy_cap = ENG_WL_DESTROY_LATER;

    w->registry_id = wayland_alloc_id(w);
    w->sync_id     = wayland_alloc_id(w);
    if (w->registry_id == 0 || w->sync_id == 0) {
        fprintf(stderr, "[Wayland Error] Initial object ID allocation failed\n");
        return 1;
    }

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
    if (!wayland_put_raw(w, w->display_id, 1u, &w->registry_id, sizeof(w->registry_id)) ||
        !wl_append_display_sync(w, w->sync_id)) {
        fprintf(stderr, "[Wayland Error] Registry handshake encode failed: %s\n", w->err);
        cleanup_wayland_resources(w);
        return 1;
    }

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
    if (sock == nullptr || conn == nullptr || w->send_len == 0) {
        r->sq_pending = held;
        fprintf(stderr, "[Wayland Error] Registry handshake was not queued\n");
        cleanup_wayland_resources(w);
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

    if (wl_tx_enqueue_stage(w, false) != 0) {
        r->sq_pending = held;
        fprintf(stderr, "[Wayland Error] Registry handshake send queue failed\n");
        cleanup_wayland_resources(w);
        return 1;
    }

    if (finish_linked_handshake(w) != 0) {
        fprintf(stderr, "[Wayland Error] Registry handshake send failed: %s\n",
                w->err[0] != '\0' ? w->err : "socket, connect, or registry send");
        cleanup_wayland_resources(w);
        return 1;
    }

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
        cleanup_wayland_resources(w);
        return 1;
    }

    if (w->compositor_global_name == 0 || w->xdg_wm_base_global_name == 0 ||
        w->linux_dmabuf_global_name == 0) {
        fprintf(stderr,
                "[Wayland Error] Missing required compositor globals (compositor=%u, xdg_wm_base=%u, linux_dmabuf=%u)\n",
                w->compositor_global_name, w->xdg_wm_base_global_name, w->linux_dmabuf_global_name);
        cleanup_wayland_resources(w);
        return 1;
    }
    if (w->linux_dmabuf_version < 4u) {
        fprintf(stderr,
                "[Wayland Error] zwp_linux_dmabuf_v1 version is %u, but engine requires v4+\n",
                w->linux_dmabuf_version);
        cleanup_wayland_resources(w);
        return 1;
    }

    // 5. Bind the core interfaces and verify with a second wl_display.sync roundtrip
    bool bind_ok = true;
    w->compositor_id            = wayland_alloc_id(w);
    w->compositor_bound_version = wl_min_u32(w->compositor_version, 6u);
    bind_ok = bind_ok && (w->compositor_id != 0) &&
              wl_append_registry_bind(w, w->compositor_global_name, "wl_compositor",
                                      w->compositor_bound_version, w->compositor_id);

    w->xdg_wm_base_id = wayland_alloc_id(w);
    bind_ok = bind_ok && (w->xdg_wm_base_id != 0) &&
              wl_append_registry_bind(w, w->xdg_wm_base_global_name, "xdg_wm_base",
                                      wl_min_u32(w->xdg_wm_base_version, 6u), w->xdg_wm_base_id);

    w->linux_dmabuf_id            = wayland_alloc_id(w);
    w->linux_dmabuf_bound_version = wl_min_u32(w->linux_dmabuf_version, 4u);
    bind_ok = bind_ok && (w->linux_dmabuf_id != 0) &&
              wl_append_registry_bind(w, w->linux_dmabuf_global_name, "zwp_linux_dmabuf_v1",
                                      w->linux_dmabuf_bound_version, w->linux_dmabuf_id);

    const char *force_imp = getenv("ENGINE_FORCE_IMPLICIT_SYNC");
    const bool skip_syncobj = (force_imp != nullptr && force_imp[0] == '1');
    if (w->drm_syncobj_global_name != 0 && !skip_syncobj) {
        w->drm_syncobj_id = wayland_alloc_id(w);
        bind_ok = bind_ok && (w->drm_syncobj_id != 0) &&
                  wl_append_registry_bind(w, w->drm_syncobj_global_name, "wp_linux_drm_syncobj_manager_v1",
                                          1u, w->drm_syncobj_id);
    }

    if (w->seat_global_name != 0) {
        w->seat_id = wayland_alloc_id(w);
        w->seat_bound_version = wl_min_u32(w->seat_version, 9u);
        bind_ok = bind_ok && (w->seat_id != 0) &&
                  wl_append_registry_bind(w, w->seat_global_name, "wl_seat",
                                          w->seat_bound_version, w->seat_id);
    }
    // The bar is cleared into the dma-buf. wl_shm is only the cursor fallback.
    if (w->cursor_shape_global_name == 0 && w->shm_global_name != 0) {
        w->shm_id = wayland_alloc_id(w);
        bind_ok = bind_ok && (w->shm_id != 0) &&
                  wl_append_registry_bind(w, w->shm_global_name, "wl_shm",
                                          wl_min_u32(w->shm_version, 1u), w->shm_id);
    }
    if (w->cursor_shape_global_name != 0) {
        w->cursor_shape_manager_id = wayland_alloc_id(w);
        w->cursor_shape_bound_version = wl_min_u32(w->cursor_shape_version, 2u);
        bind_ok = bind_ok && (w->cursor_shape_manager_id != 0) &&
                  wl_append_registry_bind(w, w->cursor_shape_global_name, "wp_cursor_shape_manager_v1",
                                          w->cursor_shape_bound_version, w->cursor_shape_manager_id);
    }
    if (w->viewporter_global_name != 0) {
        w->viewporter_id = wayland_alloc_id(w);
        bind_ok = bind_ok && (w->viewporter_id != 0) &&
                  wl_append_registry_bind(w, w->viewporter_global_name, "wp_viewporter",
                                          wl_min_u32(w->viewporter_version, 1u), w->viewporter_id);
    }
    if (w->subcompositor_global_name != 0) {
        w->subcompositor_id = wayland_alloc_id(w);
        bind_ok = bind_ok && (w->subcompositor_id != 0) &&
                  wl_append_registry_bind(w, w->subcompositor_global_name, "wl_subcompositor",
                                          wl_min_u32(w->subcompositor_version, 1u), w->subcompositor_id);
    }
    if (w->fractional_scale_global_name != 0) {
        w->fractional_scale_manager_id = wayland_alloc_id(w);
        bind_ok = bind_ok && (w->fractional_scale_manager_id != 0) &&
                  wl_append_registry_bind(w, w->fractional_scale_global_name,
                                          "wp_fractional_scale_manager_v1", 1u,
                                          w->fractional_scale_manager_id);
    }
    if (w->fifo_global_name != 0) {
        w->fifo_manager_id = wayland_alloc_id(w);
        bind_ok = bind_ok && (w->fifo_manager_id != 0) &&
                  wl_append_registry_bind(w, w->fifo_global_name,
                                          "wp_fifo_manager_v1", 1u, w->fifo_manager_id);
    }
    if (w->commit_timing_global_name != 0) {
        w->commit_timing_manager_id = wayland_alloc_id(w);
        bind_ok = bind_ok && (w->commit_timing_manager_id != 0) &&
                  wl_append_registry_bind(w, w->commit_timing_global_name,
                                          "wp_commit_timing_manager_v1", 1u,
                                          w->commit_timing_manager_id);
    }
    if (w->presentation_global_name != 0) {
        w->presentation_id = wayland_alloc_id(w);
        bind_ok = bind_ok && (w->presentation_id != 0) &&
                  wl_append_registry_bind(w, w->presentation_global_name,
                                          "wp_presentation",
                                          wl_min_u32(w->presentation_version, 2u),
                                          w->presentation_id);
    }
    if (w->relative_pointer_global_name != 0) {
        w->relative_pointer_manager_id = wayland_alloc_id(w);
        bind_ok = bind_ok && (w->relative_pointer_manager_id != 0) &&
                  wl_append_registry_bind(w, w->relative_pointer_global_name,
                                          "zwp_relative_pointer_manager_v1", 1u,
                                          w->relative_pointer_manager_id);
    }
    if (w->pointer_constraints_global_name != 0) {
        w->pointer_constraints_id = wayland_alloc_id(w);
        bind_ok = bind_ok && (w->pointer_constraints_id != 0) &&
                  wl_append_registry_bind(w, w->pointer_constraints_global_name,
                                          "zwp_pointer_constraints_v1", 1u,
                                          w->pointer_constraints_id);
    }
    if (w->data_device_manager_global_name != 0) {
        w->data_device_manager_id            = wayland_alloc_id(w);
        w->data_device_manager_bound_version = wl_min_u32(w->data_device_manager_version, 3u);
        bind_ok = bind_ok && (w->data_device_manager_id != 0) &&
                  wl_append_registry_bind(w, w->data_device_manager_global_name,
                                          "wl_data_device_manager",
                                          w->data_device_manager_bound_version,
                                          w->data_device_manager_id);
    }
    if (!bind_ok || !wl_bind_decoration_manager(w)) {
        fprintf(stderr, "[Wayland Error] Global registry bind encode failed: %s\n", w->err);
        cleanup_wayland_resources(w);
        return 1;
    }

    w->synced  = false;
    w->sync_id = wayland_alloc_id(w);
    if (w->sync_id == 0 || !wl_append_display_sync(w, w->sync_id)) {
        fprintf(stderr, "[Wayland Error] Global bind sync encode failed: %s\n", w->err);
        cleanup_wayland_resources(w);
        return 1;
    }

    if (wl_send_batch(w) < 0) {
        fprintf(stderr, "[Wayland Error] Global bind batch failed: %s\n", w->err);
        cleanup_wayland_resources(w);
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
        cleanup_wayland_resources(w);
        return 1;
    }

    if (w->ring->sq_pending > 0) {
        unsigned n = ring_submit(r);
        (void)ring_enter(r, n, 0, -1);
    }

    w->send_len = 0;
    if (pointer_open_devices(w) != 0) {
        pointer_shutdown(w);
        cleanup_wayland_resources(w);
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
    printf("[Wayland] Input Object IDs: wl_seat=%u, wl_shm=%u, cursor_shape=%u, viewporter=%u, rel_ptr=%u, ptr_lock=%u, data_mgr=%u\n",
           w->seat_id, w->shm_id, w->cursor_shape_manager_id, w->viewporter_id,
           w->relative_pointer_manager_id, w->pointer_constraints_id, w->data_device_manager_id);
    printf("[Wayland] Surface Pacing & Scale IDs: frac_scale=%u, fifo=%u, commit_timing=%u, presentation=%u (clk=%s:%u)\n",
           w->fractional_scale_manager_id, w->fifo_manager_id,
           w->commit_timing_manager_id, w->presentation_id,
           w->has_presentation_clk ? "ok" : "none", w->presentation_clk_id);
    return 0;
}


void wayland_shutdown(void) {
    WaylandContext *w = &s_wl_ctx;
    if (!w->is_initialized) {
        return;
    }
    if (w->presented_frames > 0u || w->discarded_frames > 0u) {
        printf("[Wayland] Presentation telemetry: presented=%llu, zero_copy_scanout=%llu, discarded=%llu\n",
               (unsigned long long)w->presented_frames,
               (unsigned long long)w->zero_copy_scanout_frames,
               (unsigned long long)w->discarded_frames);
    }
    pointer_shutdown(w);
    cleanup_wayland_resources(w);
    w->is_initialized = false;
    printf("goodbye from wayland\n");
}


WaylandContext *wayland_get_context(void) {
    return &s_wl_ctx;
}

