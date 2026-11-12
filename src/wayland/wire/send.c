// 16 KiB circular packet wire queue, io_uring sends, round-trips, and deferred destroys.
#include "wayland/internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void close_packet_fds(WaylandSendPacket *pkt) {
    if (pkt == nullptr || !pkt->close_fds_on_done) {
        return;
    }
    for (uint8_t i = 0; i < pkt->fd_count; ++i) {
        if (pkt->fds[i] >= 0) {
            close(pkt->fds[i]);
            pkt->fds[i] = -1;
        }
    }
    pkt->fd_count          = 0u;
    pkt->close_fds_on_done = false;
}

static void tx_ring_write(WaylandContext *w, uint32_t ring_off,
                          const unsigned char *src, uint32_t len) {
    uint32_t pos   = ring_off & (ENG_WL_SEND_RING_CAP - 1u);
    uint32_t first = ENG_WL_SEND_RING_CAP - pos;
    if (first > len) {
        first = len;
    }
    memcpy(w->tx_ring + pos, src, first);
    if (len > first) {
        memcpy(w->tx_ring, src + first, len - first);
    }
}

static int tx_enqueue_packet(WaylandContext *w, const unsigned char *data, uint32_t len,
                             bool tracks_ack, bool is_internal, bool close_fds,
                             const int *fds, uint8_t fd_count) {
    if (w == nullptr || data == nullptr || len == 0u || fd_count > ENG_WL_MAX_MSG_FDS) {
        return -EINVAL;
    }
    uint32_t used = w->tx_tail - w->tx_head;
    if (len > ENG_WL_SEND_RING_CAP - used || w->tx_pkt_count >= ENG_WL_SEND_PKT_CAP) {
        wl_fail(w, "wayland 16 KiB outgoing wire queue full");
        return -ENOSPC;
    }
    uint32_t slot = (w->tx_pkt_head + w->tx_pkt_count) & (ENG_WL_SEND_PKT_CAP - 1u);
    tx_ring_write(w, w->tx_tail, data, len);

    WaylandSendPacket *pkt  = &w->tx_pkts[slot];
    pkt->offset             = w->tx_tail;
    pkt->len                = len;
    pkt->sent               = 0u;
    pkt->tracks_ack         = tracks_ack;
    pkt->is_internal        = is_internal;
    pkt->kick_armed         = true;
    pkt->close_fds_on_done  = close_fds;
    pkt->fd_count           = fd_count;
    for (uint8_t i = 0; i < fd_count; ++i) {
        pkt->fds[i] = fds[i];
    }
    for (uint8_t i = fd_count; i < ENG_WL_MAX_MSG_FDS; ++i) {
        pkt->fds[i] = -1;
    }

    w->tx_tail += len;
    w->tx_pkt_count++;
    return 0;
}

bool wl_has_client_tx(const WaylandContext *w) {
    if (w == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < w->tx_pkt_count; ++i) {
        uint32_t idx = (w->tx_pkt_head + i) & (ENG_WL_SEND_PKT_CAP - 1u);
        if (!w->tx_pkts[idx].is_internal) {
            return true;
        }
    }
    return false;
}

int wl_tx_kick(WaylandContext *w) {
    if (w == nullptr || w->ring == nullptr) {
        return -EINVAL;
    }
    if (w->failed) {
        return -EIO;
    }
    if (w->tx_inflight_tag != 0u || w->noted_send) {
        return 0;
    }
    if (w->tx_pkt_count == 0u) {
        return 0;
    }

    WaylandSendPacket *pkt = &w->tx_pkts[w->tx_pkt_head];
    if (pkt->sent >= pkt->len) {
        w->tx_pkt_head = (w->tx_pkt_head + 1u) & (ENG_WL_SEND_PKT_CAP - 1u);
        w->tx_pkt_count--;
        return wl_tx_kick(w);
    }
    if (!pkt->kick_armed) {
        return 0;
    }

    struct io_uring_sqe *sqe = ring_sqe(w->ring);
    if (sqe == nullptr) {
        wl_fail(w, "SQ full while arming wayland wire send");
        return -ENOSPC;
    }

    uint32_t rem    = pkt->len - pkt->sent;
    uint32_t pos    = (pkt->offset + pkt->sent) & (ENG_WL_SEND_RING_CAP - 1u);
    uint32_t first  = ENG_WL_SEND_RING_CAP - pos;
    if (first > rem) {
        first = rem;
    }
    uint32_t second    = rem - first;
    bool     with_cmsg = (pkt->fd_count > 0u && pkt->sent == 0u);
    uint64_t tag       = pkt->is_internal ? ENG_UD_WL_PONG : ENG_UD_WL_SEND;

    if (!pkt->is_internal) {
        w->send_tracks_ack = pkt->tracks_ack;
    }

    if (with_cmsg || second > 0u) {
        w->send_iov[0].iov_base = w->tx_ring + pos;
        w->send_iov[0].iov_len  = first;
        w->send_iov[1].iov_base = w->tx_ring;
        w->send_iov[1].iov_len  = second;
        w->send_msg.msg_name    = nullptr;
        w->send_msg.msg_namelen = 0u;
        w->send_msg.msg_iov     = w->send_iov;
        w->send_msg.msg_iovlen  = (second > 0u) ? 2u : 1u;

        if (with_cmsg) {
            size_t fd_bytes = (size_t)pkt->fd_count * sizeof(int);
            size_t ctl_space = CMSG_SPACE(fd_bytes);
            memset(w->send_control, 0, sizeof(w->send_control));
            w->send_msg.msg_control    = w->send_control;
            w->send_msg.msg_controllen = ctl_space;

            struct cmsghdr *cmsg = CMSG_FIRSTHDR(&w->send_msg);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type  = SCM_RIGHTS;
            cmsg->cmsg_len   = CMSG_LEN(fd_bytes);
            memcpy(CMSG_DATA(cmsg), pkt->fds, fd_bytes);
        } else {
            w->send_msg.msg_control    = nullptr;
            w->send_msg.msg_controllen = 0u;
        }

        sqe->opcode    = IORING_OP_SENDMSG;
        sqe->flags     = IOSQE_FIXED_FILE;
        sqe->ioprio    = IORING_RECVSEND_POLL_FIRST;
        sqe->fd        = ENG_WL_SOCK_SLOT;
        sqe->addr      = (uint64_t)(uintptr_t)&w->send_msg;
        sqe->msg_flags = MSG_NOSIGNAL;
        sqe->user_data = tag;
    } else {
        w->send_msg.msg_control    = nullptr;
        w->send_msg.msg_controllen = 0u;

        sqe->opcode    = IORING_OP_SEND;
        sqe->flags     = IOSQE_FIXED_FILE;
        sqe->ioprio    = IORING_RECVSEND_POLL_FIRST;
        sqe->fd        = ENG_WL_SOCK_SLOT;
        sqe->addr      = (uint64_t)(uintptr_t)(w->tx_ring + pos);
        sqe->len       = first;
        sqe->msg_flags = MSG_NOSIGNAL;
        sqe->user_data = tag;
    }

    w->tx_inflight_tag = tag;
    w->send_queued     = (tag == ENG_UD_WL_SEND);
    w->pong_in_flight  = (tag == ENG_UD_WL_PONG);
    return 0;
}

int wl_tx_step_head(WaylandContext *w, int32_t res) {
    if (w == nullptr || w->tx_pkt_count == 0u) {
        return 0;
    }
    WaylandSendPacket *pkt = &w->tx_pkts[w->tx_pkt_head];
    uint32_t remaining = (pkt->sent <= pkt->len) ? (pkt->len - pkt->sent) : 0u;
    if (remaining == 0u) {
        close_packet_fds(pkt);
        w->tx_pkt_head = (w->tx_pkt_head + 1u) & (ENG_WL_SEND_PKT_CAP - 1u);
        w->tx_pkt_count--;
        return 0;
    }
    if (res < 0) {
        if (res == -EAGAIN || res == -EINTR) {
            if (!pkt->is_internal) {
                pkt->kick_armed = false;
            }
            return 1;
        }
        close_packet_fds(pkt);
        wl_fail_err(w, "wayland send", res);
        w->send_tracks_ack = false;
        return -EIO;
    }
    if (res == 0) {
        if (!pkt->is_internal) {
            pkt->kick_armed = false;
        }
        return 1;
    }
    if ((uint32_t)res > remaining) {
        close_packet_fds(pkt);
        wl_fail(w, "wayland send wrote past packet boundary");
        w->send_tracks_ack = false;
        return -EIO;
    }
    // SCM_RIGHTS ancillary FDs are transferred with the first byte sent; close
    // owned pipe write ends immediately so the compositor observes EOF on close.
    close_packet_fds(pkt);
    pkt->sent  += (uint32_t)res;
    w->tx_head += (uint32_t)res;
    if (pkt->sent < pkt->len) {
        if (!pkt->is_internal) {
            pkt->kick_armed = false;
        }
        return 1;
    }
    w->tx_pkt_head = (w->tx_pkt_head + 1u) & (ENG_WL_SEND_PKT_CAP - 1u);
    w->tx_pkt_count--;
    return 0;
}

void wl_queue_xdg_pong(WaylandContext *w, uint32_t serial) {
    if (w == nullptr || w->xdg_wm_base_id == 0u || w->failed) {
        return;
    }
    unsigned char msg[12];
    wl_put_u32(msg + 0, w->xdg_wm_base_id);
    wl_put_u16(msg + 4, 3u); // xdg_wm_base.pong opcode = 3
    wl_put_u16(msg + 6, 12u);
    wl_put_u32(msg + 8, serial);

    // Coalesce back-to-back pings into any trailing unsent pong packet in tx_pkts.
    if (w->tx_pkt_count > 0u) {
        uint32_t last_idx = (w->tx_pkt_head + w->tx_pkt_count - 1u) & (ENG_WL_SEND_PKT_CAP - 1u);
        bool     last_live = (w->tx_pkt_count == 1u && w->tx_inflight_tag != 0u);
        WaylandSendPacket *last = &w->tx_pkts[last_idx];
        if (!last_live && last->is_internal && last->fd_count == 0u &&
            last->sent == 0u && last->len == 12u) {
            tx_ring_write(w, last->offset, msg, sizeof(msg));
            return;
        }
    }

    if (tx_enqueue_packet(w, msg, sizeof(msg), false, true, false, nullptr, 0u) != 0) {
        return;
    }
    (void)wl_tx_kick(w);
}

int wl_queue_data_receive(WaylandContext *w, uint32_t offer_id, const char *mime, int write_fd) {
    if (w == nullptr || w->failed || offer_id == 0u || mime == nullptr || write_fd < 0) {
        return -EINVAL;
    }
    size_t slen = strlen(mime) + 1u;
    if (slen > 64u) {
        return -EINVAL;
    }
    uint32_t pad   = (4u - ((uint32_t)slen & 3u)) & 3u;
    uint32_t total = 12u + (uint32_t)slen + pad;
    unsigned char msg[88];
    memset(msg, 0, total);
    wl_put_u32(msg + 0, offer_id);
    wl_put_u16(msg + 4, 1u); // wl_data_offer.receive opcode = 1
    wl_put_u16(msg + 6, (uint16_t)total);
    wl_put_u32(msg + 8, (uint32_t)slen);
    memcpy(msg + 12, mime, slen);

    int rc = tx_enqueue_packet(w, msg, total, false, true, true, &write_fd, 1u);
    if (rc != 0) {
        return rc;
    }
    (void)wl_tx_kick(w);
    return 0;
}

int wl_wait_tag(WaylandContext *w, uint64_t tag, struct io_uring_cqe *out) {
    uint64_t deadline = ring_mono_ns() + 3000000000ull;
    for (;;) {
        struct io_uring_cqe cqe;
        while (ring_peek(w->ring, &cqe)) {
            if (cqe.user_data == tag) {
                *out = cqe;
                return 0;
            }
            wayland_on_cqe(w, &cqe);
            if (w->failed) return -EIO;
        }
        if (ring_mono_ns() > deadline) {
            wl_fail(w, "wayland io_uring operation timed out");
            return -ETIMEDOUT;
        }
        if (w->recv_msg.msg_iov != nullptr && !w->recv_armed) {
            (void)wayland_ensure_recv(w);
        }
        unsigned n = ring_submit(w->ring);
        int rc = ring_enter(w->ring, n, 1, 200);
        if (rc < 0 && rc != -ETIME) {
            wl_fail_err(w, "io_uring_enter", rc);
            return rc;
        }
    }
}

bool wl_send_blocked(const WaylandContext *w) {
    return w != nullptr && (w->send_queued || w->noted_send || wl_has_client_tx(w));
}

int wl_send_with_fds(WaylandContext *w, const int *fds, uint8_t fd_count) {
    if (w == nullptr) {
        return -EINVAL;
    }
    if (w->failed) {
        return -EIO;
    }

    if (w->noted_send) {
        w->noted_send = false;
        int prog = wl_tx_step_head(w, w->noted_send_res);
        w->noted_send_res = 0;
        if (prog < 0) {
            return prog;
        }
    }

    if (w->send_len > 0u) {
        int enq = tx_enqueue_packet(w, w->send_bytes, w->send_len, false, false, false, fds, fd_count);
        w->send_len = 0u;
        w->send_off = 0u;
        if (enq != 0) {
            return enq;
        }
    }

    while (w->tx_pkt_count > 0u && !w->failed) {
        w->tx_pkts[w->tx_pkt_head].kick_armed = true;
        if (w->tx_inflight_tag == 0u) {
            int krc = wl_tx_kick(w);
            if (krc != 0) {
                return krc;
            }
        }
        uint64_t wait_tag = w->tx_inflight_tag;
        if (wait_tag == 0u) {
            break;
        }
        struct io_uring_cqe cqe = {0};
        int rc = wl_wait_tag(w, wait_tag, &cqe);
        if (rc < 0) {
            w->send_msg.msg_control    = nullptr;
            w->send_msg.msg_controllen = 0u;
            return rc;
        }
        w->tx_inflight_tag = 0u;
        w->send_queued     = false;
        w->pong_in_flight  = false;
        int prog = wl_tx_step_head(w, cqe.res);
        if (prog < 0) {
            w->send_msg.msg_control    = nullptr;
            w->send_msg.msg_controllen = 0u;
            return prog;
        }
    }

    w->send_msg.msg_control    = nullptr;
    w->send_msg.msg_controllen = 0u;
    return w->failed ? -EIO : 0;
}

int wl_send_with_fd(WaylandContext *w, int fd_to_send) {
    if (fd_to_send >= 0) {
        return wl_send_with_fds(w, &fd_to_send, 1u);
    }
    return wl_send_with_fds(w, nullptr, 0u);
}

int wl_send_batch(WaylandContext *w) {
    return wl_send_with_fds(w, nullptr, 0u);
}

int wl_flush_pending(WaylandContext *w) {
    if (w == nullptr) {
        return -EINVAL;
    }
    if (w->noted_send) {
        w->noted_send = false;
        int prog = wl_tx_step_head(w, w->noted_send_res);
        w->noted_send_res = 0;
        if (prog < 0) {
            return prog;
        }
    }
    if (w->send_len == 0u && w->tx_pkt_count == 0u) {
        return 0;
    }
    return wl_send_batch(w);
}

int wayland_send_batch(WaylandContext *w) {
    return wl_send_batch(w);
}

int wayland_roundtrip_batch(WaylandContext *w, int fd_to_send) {
    return wl_finish_roundtrip(w, fd_to_send);
}

int wl_finish_roundtrip(WaylandContext *w, int fd_to_send) {
    w->synced  = false;
    w->sync_id = wayland_alloc_id(w);
    if (w->sync_id == 0 || !wl_append_display_sync(w, w->sync_id)) {
        return -EIO;
    }
    if (wl_send_with_fd(w, fd_to_send) < 0) {
        return -EIO;
    }
    struct io_uring_cqe cqe = {0};
    while (!w->synced && !w->failed) {
        if (wl_wait_tag(w, ENG_UD_WL_RECV, &cqe) < 0) {
            break;
        }
        wayland_on_cqe(w, &cqe);
    }
    if (w->ring->sq_pending > 0) {
        unsigned n = ring_submit(w->ring);
        (void)ring_enter(w->ring, n, 0, -1);
    }
    if (w->failed || !w->synced) {
        return -EIO;
    }
    return 0;
}

int wl_tx_enqueue_stage(WaylandContext *w, bool tracks_ack) {
    if (w == nullptr || w->failed) {
        return -EINVAL;
    }
    if (w->send_len == 0u) {
        if (!wl_has_client_tx(w)) {
            return -EINVAL;
        }
        if (w->send_queued || w->noted_send) {
            return -EBUSY;
        }
        for (uint32_t i = 0; i < w->tx_pkt_count; ++i) {
            uint32_t idx = (w->tx_pkt_head + i) & (ENG_WL_SEND_PKT_CAP - 1u);
            if (!w->tx_pkts[idx].is_internal) {
                w->tx_pkts[idx].kick_armed = true;
                break;
            }
        }
        return wl_tx_kick(w);
    }
    if (wl_send_blocked(w)) {
        return -EBUSY;
    }
    int enq = tx_enqueue_packet(w, w->send_bytes, w->send_len, tracks_ack, false, false, nullptr, 0u);
    if (enq != 0) {
        return enq;
    }
    w->send_len        = 0u;
    w->send_off        = 0u;
    w->send_tracks_ack = tracks_ack;
    return wl_tx_kick(w);
}

int wayland_queue_send(WaylandContext *w, bool tracks_ack) {
    if (w == nullptr || !w->is_initialized || w->failed) {
        return -EINVAL;
    }
    return wl_tx_enqueue_stage(w, tracks_ack);
}

static bool ensure_destroy_cap(WaylandContext *w) {
    if (w->destroy_ids == nullptr || w->destroy_cap == 0u) {
        w->destroy_ids = w->destroy_inline;
        w->destroy_cap = ENG_WL_DESTROY_LATER;
    }
    if (w->destroy_later_n < w->destroy_cap) {
        return true;
    }
    uint32_t new_cap = w->destroy_cap * 2u;
    if (new_cap <= w->destroy_cap) {
        return false;
    }
    uint32_t *grown = nullptr;
    if (w->destroy_ids == w->destroy_inline) {
        grown = (uint32_t *)malloc((size_t)new_cap * sizeof(uint32_t));
        if (grown == nullptr) {
            return false;
        }
        memcpy(grown, w->destroy_inline, (size_t)w->destroy_later_n * sizeof(uint32_t));
    } else {
        grown = (uint32_t *)realloc(w->destroy_ids, (size_t)new_cap * sizeof(uint32_t));
        if (grown == nullptr) {
            return false;
        }
    }
    w->destroy_ids = grown;
    w->destroy_cap = new_cap;
    return true;
}

bool wayland_defer_destroy(WaylandContext *w, uint32_t id) {
    if (w == nullptr || id == 0u) {
        return false;
    }
    if (w->destroy_ids == nullptr || w->destroy_cap == 0u) {
        w->destroy_ids = w->destroy_inline;
        w->destroy_cap = ENG_WL_DESTROY_LATER;
    }
    for (uint32_t i = 0; i < w->destroy_later_n; ++i) {
        if (w->destroy_ids[i] == id) {
            return true;
        }
    }
    if (!ensure_destroy_cap(w)) {
        return false;
    }
    w->destroy_ids[w->destroy_later_n++] = id;
    return true;
}

uint32_t wl_encode_deferred_destroys(WaylandContext *w) {
    if (w == nullptr || w->destroy_later_n == 0u) {
        return 0u;
    }
    uint32_t count = 0u;
    while (count < w->destroy_later_n && w->send_len + 256u <= sizeof(w->send_bytes)) {
        if (!wayland_put_raw(w, w->destroy_ids[count], 0u, nullptr, 0u)) {
            break;
        }
        count++;
    }
    return count;
}

void wl_consume_deferred_destroys(WaylandContext *w, uint32_t count) {
    if (w == nullptr || count == 0u || w->destroy_later_n == 0u) {
        return;
    }
    if (count >= w->destroy_later_n) {
        w->destroy_later_n = 0u;
        return;
    }
    memmove(w->destroy_ids,
            w->destroy_ids + count,
            (size_t)(w->destroy_later_n - count) * sizeof(uint32_t));
    w->destroy_later_n -= count;
}

int wayland_queue_destroys(WaylandContext *w) {
    if (w == nullptr || !w->is_initialized || w->failed) {
        return -EINVAL;
    }
    if (w->destroy_later_n == 0u) {
        return 0;
    }
    if (wl_send_blocked(w) || w->send_len != 0u) {
        return -EBUSY;
    }
    uint32_t mark  = w->send_len;
    uint32_t count = wl_encode_deferred_destroys(w);
    if (count == 0u) {
        return 0;
    }
    int queued = wayland_queue_send(w, false);
    if (queued != 0) {
        wayland_rewind_send(w, mark);
        return queued;
    }
    wl_consume_deferred_destroys(w, count);
    return 1;
}

int wayland_note_send_cqe(
    WaylandContext            *w,
    const struct io_uring_cqe *cqe,
    uint32_t                   acked_serial
) {
    if (w == nullptr || cqe == nullptr) {
        return -EINVAL;
    }
    w->tx_inflight_tag = 0u;
    w->send_queued     = false;
    int progress = wl_tx_step_head(w, cqe->res);
    if (progress != 0) {
        return progress;
    }
    bool track = w->send_tracks_ack;
    w->send_tracks_ack = false;
    if (track) {
        w->acked_configure_serial = acked_serial;
        w->configure_ack_valid    = true;
    }
    // If an internal xdg_wm_base.pong queued behind this client packet, kick it immediately.
    if (w->tx_pkt_count > 0u) {
        (void)wl_tx_kick(w);
    }
    return 0;
}
