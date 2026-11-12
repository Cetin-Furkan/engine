#include "wayland/internal.h"
#include "input/pointer.h"
#include "system/system.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// Minimal input stubs so src/wayland/* can be exercised in isolation
int pointer_open_devices(WaylandContext *wl) {
    (void)wl;
    return 0;
}

void pointer_shutdown(WaylandContext *wl) {
    (void)wl;
}

bool pointer_dispatch(WaylandContext *wl, uint32_t id, uint16_t op,
                      const unsigned char *body, uint16_t body_len) {
    (void)wl;
    (void)id;
    (void)op;
    (void)body;
    (void)body_len;
    return false;
}

void pointer_on_clip_cqe(WaylandContext *wl, const struct io_uring_cqe *cqe) {
    (void)wl;
    (void)cqe;
}

static int s_failures = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_failures++;                                                   \
        }                                                                   \
    } while (0)

static void init_mock_wl(WaylandContext *w, RingContext *r, int sock_fd) {
    *w = (WaylandContext){
        .ring                   = r,
        .fixed_socket_installed = true,
        .display_id             = 1u,
        .xdg_wm_base_id         = 10u,
        .xdg_surface_id         = 11u,
        .xdg_toplevel_id        = 12u,
        .surface_id             = 13u,
        .next_id                = 20u,
        .feedback_table_fd      = -1,
        .is_initialized         = true,
    };
    w->control_reserve         = (uint32_t)sizeof(w->recv_control);
    w->recv_iov.iov_base       = nullptr;
    w->recv_iov.iov_len        = ENG_PBUF_LEN;
    w->recv_msg.msg_iov        = &w->recv_iov;
    w->recv_msg.msg_iovlen     = 1u;
    w->recv_msg.msg_control    = w->recv_control;
    w->recv_msg.msg_controllen = w->control_reserve;
    CHECK(ring_update_file(r, ENG_WL_SOCK_SLOT, sock_fd) == 0);
}

static void free_mock_wl_arrays(WaylandContext *w) {
    if (w->free_ids != nullptr && w->free_ids != w->free_id_inline) {
        free(w->free_ids);
    }
    if (w->destroy_ids != nullptr && w->destroy_ids != w->destroy_inline) {
        free(w->destroy_ids);
    }
}

static void test_burst_ids_and_client_cap(void) {
    WaylandContext w = {
        .display_id = 1u,
        .next_id    = 2u,
    };
    enum { N_BURST = 600 };
    uint32_t ids[N_BURST];
    for (int i = 0; i < N_BURST; ++i) {
        ids[i] = wayland_alloc_id(&w);
        CHECK(ids[i] == (uint32_t)(2 + i));
    }
    // Free all 600 IDs (exceeding ENG_ID_FREE=64 inline capacity by ~10x)
    for (int i = 0; i < N_BURST; ++i) {
        wl_free_id(&w, ids[i]);
    }
    CHECK(!w.failed);
    CHECK(w.nfree == N_BURST);
    CHECK(w.free_cap >= N_BURST);

    // Server-range object IDs (>= 0xff000000) must be ignored on delete_id
    wl_free_id(&w, 0xff000000u);
    CHECK(w.nfree == N_BURST);

    // Re-allocate all 600 recycled IDs without advancing next_id
    uint32_t prev_next = w.next_id;
    for (int i = 0; i < N_BURST; ++i) {
        uint32_t got = wayland_alloc_id(&w);
        CHECK(got >= 2u && got < prev_next);
    }
    CHECK(w.nfree == 0u);
    CHECK(w.next_id == prev_next);

    // Exhausting client ID ceiling must fail cleanly instead of crossing into 0xff000000
    w.next_id = ENG_WL_CLIENT_ID_MAX;
    CHECK(wayland_alloc_id(&w) == ENG_WL_CLIENT_ID_MAX);
    CHECK(wayland_alloc_id(&w) == 0u);
    CHECK(w.failed);

    free_mock_wl_arrays(&w);
}

static size_t drain_nonblocking(int fd, unsigned char *dst, size_t cap) {
    size_t total = 0u;
    while (total < cap) {
        ssize_t n = recv(fd, dst + total, cap - total, MSG_DONTWAIT);
        if (n <= 0) {
            break;
        }
        total += (size_t)n;
    }
    return total;
}

static void test_wire_wraparound_pong_and_destroys(RingContext *r) {
    int sv[2] = { -1, -1 };
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);

    WaylandContext w;
    init_mock_wl(&w, r, sv[0]);

    // 1. Defer 48 object destroys (growing past ENG_WL_DESTROY_LATER=16) and flush
    for (uint32_t i = 0; i < 48u; ++i) {
        CHECK(wayland_defer_destroy(&w, 100u + i));
    }
    CHECK(w.destroy_later_n == 48u);
    CHECK(w.destroy_cap >= 48u);
    CHECK(wayland_queue_destroys(&w) == 1);
    CHECK(w.destroy_later_n == 0u);

    struct io_uring_cqe cqe = {0};
    CHECK(wl_wait_tag(&w, ENG_UD_WL_SEND, &cqe) == 0);
    CHECK(wayland_note_send_cqe(&w, &cqe, 0u) == 0);

    unsigned char rx[2048];
    size_t got = drain_nonblocking(sv[1], rx, sizeof(rx));
    CHECK(got == 48u * 8u);
    for (uint32_t i = 0; i < 48u; ++i) {
        uint32_t obj = 0;
        uint16_t op = 0, sz = 0;
        memcpy(&obj, rx + i * 8u, 4);
        memcpy(&op,  rx + i * 8u + 4u, 2);
        memcpy(&sz,  rx + i * 8u + 6u, 2);
        CHECK(obj == 100u + i && op == 0u && sz == 8u);
    }

    // 2. Position tx_head/tx_tail 96 bytes before the 16 KiB wrap boundary, queue a
    //    256-byte client batch (tracks_ack=true) AND interleave two xdg_wm_base.pongs
    //    (which coalesce into the latest ping serial while client send is in flight!).
    w.tx_head = ENG_WL_SEND_RING_CAP - 96u;
    w.tx_tail = ENG_WL_SEND_RING_CAP - 96u;

    uint32_t payload[2] = { 0x11223344u, 0x55667788u };
    for (uint32_t i = 0; i < 16u; ++i) {
        payload[0] = 0x1000u + i;
        CHECK(wayland_put_raw(&w, 50u + i, 2u, payload, sizeof(payload)));
    }
    CHECK(w.send_len == 16u * 16u);
    CHECK(wayland_queue_send(&w, true) == 0);

    // Queue two pongs while the wrapped client packet is still in flight on SQ!
    wl_queue_xdg_pong(&w, 0xaaaa0001u);
    wl_queue_xdg_pong(&w, 0xbbbb0002u);

    // Retire the client send CQE -> must record acked_configure_serial AND auto-kick pong
    CHECK(wl_wait_tag(&w, ENG_UD_WL_SEND, &cqe) == 0);
    CHECK(wayland_note_send_cqe(&w, &cqe, 777u) == 0);
    CHECK(w.configure_ack_valid && w.acked_configure_serial == 777u);

    // Retire the internal pong CQE
    CHECK(wl_wait_tag(&w, ENG_UD_WL_PONG, &cqe) == 0);
    wayland_on_cqe(&w, &cqe);
    CHECK(!w.pong_in_flight && w.tx_pkt_count == 0u);

    got = drain_nonblocking(sv[1], rx, sizeof(rx));
    CHECK(got == 256u + 12u);
    // Verify all 16 wrapped client messages arrived contiguously without pong splicing
    for (uint32_t i = 0; i < 16u; ++i) {
        uint32_t obj = 0, p0 = 0, p1 = 0;
        uint16_t op = 0, sz = 0;
        memcpy(&obj, rx + i * 16u + 0u, 4);
        memcpy(&op,  rx + i * 16u + 4u, 2);
        memcpy(&sz,  rx + i * 16u + 6u, 2);
        memcpy(&p0,  rx + i * 16u + 8u, 4);
        memcpy(&p1,  rx + i * 16u + 12u, 4);
        CHECK(obj == 50u + i && op == 2u && sz == 16u);
        CHECK(p0 == 0x1000u + i && p1 == 0x55667788u);
    }
    // Verify coalesced pong packet arrived intact immediately after client packet
    uint32_t pong_obj = 0, pong_serial = 0;
    uint16_t pong_op = 0, pong_sz = 0;
    memcpy(&pong_obj,    rx + 256u + 0u, 4);
    memcpy(&pong_op,     rx + 256u + 4u, 2);
    memcpy(&pong_sz,     rx + 256u + 6u, 2);
    memcpy(&pong_serial, rx + 256u + 8u, 4);
    CHECK(pong_obj == w.xdg_wm_base_id && pong_op == 3u && pong_sz == 12u);
    CHECK(pong_serial == 0xbbbb0002u);

    // 3. Multi-FD SCM_RIGHTS transmission (3 eventfds in one packet)
    int fds[3] = {
        eventfd(11, EFD_CLOEXEC),
        eventfd(22, EFD_CLOEXEC),
        eventfd(33, EFD_CLOEXEC),
    };
    CHECK(fds[0] >= 0 && fds[1] >= 0 && fds[2] >= 0);
    uint32_t tag_word = 0xcafebabeu;
    CHECK(wayland_put_raw(&w, 77u, 1u, &tag_word, sizeof(tag_word)));
    CHECK(wl_send_with_fds(&w, fds, 3u) == 0);
    close(fds[0]);
    close(fds[1]);
    close(fds[2]);

    unsigned char fd_data[32] = {0};
    unsigned char fd_ctl[CMSG_SPACE(sizeof(int) * 4)] = {0};
    struct iovec iov = { .iov_base = fd_data, .iov_len = sizeof(fd_data) };
    struct msghdr msg = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1u,
        .msg_control    = fd_ctl,
        .msg_controllen = sizeof(fd_ctl),
    };
    ssize_t rcv = recvmsg(sv[1], &msg, MSG_CMSG_CLOEXEC);
    CHECK(rcv == 12);
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    CHECK(cm != nullptr && cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS);
    int rx_fds[3] = { -1, -1, -1 };
    memcpy(rx_fds, CMSG_DATA(cm), sizeof(rx_fds));
    for (int i = 0; i < 3; ++i) {
        uint64_t val = 0;
        CHECK(read(rx_fds[i], &val, sizeof(val)) == 8);
        CHECK(val == (uint64_t)(11 * (i + 1)));
        close(rx_fds[i]);
    }

    // 4. Retired feedback_id format_table FD sweep + compositor EOF detection
    w.retired_feedback_id = 44u;
    CHECK(wayland_ensure_recv(&w) == 0);
    int stale_fd = eventfd(99, EFD_CLOEXEC);
    CHECK(stale_fd >= 0);
    unsigned char stale_ev[12];
    wl_put_u32(stale_ev + 0, 44u);  // retired_feedback_id
    wl_put_u16(stale_ev + 4, 1u);   // opcode 1 = format_table
    wl_put_u16(stale_ev + 6, 12u);
    wl_put_u32(stale_ev + 8, 16u);  // size
    unsigned char tx_ctl[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec tx_iov = { .iov_base = stale_ev, .iov_len = sizeof(stale_ev) };
    struct msghdr tx_msg = {
        .msg_iov        = &tx_iov,
        .msg_iovlen     = 1u,
        .msg_control    = tx_ctl,
        .msg_controllen = sizeof(tx_ctl),
    };
    struct cmsghdr *tcm = CMSG_FIRSTHDR(&tx_msg);
    tcm->cmsg_level = SOL_SOCKET;
    tcm->cmsg_type  = SCM_RIGHTS;
    tcm->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(tcm), &stale_fd, sizeof(int));
    CHECK(sendmsg(sv[1], &tx_msg, MSG_NOSIGNAL) == 12);
    close(stale_fd);

    CHECK(wl_wait_tag(&w, ENG_UD_WL_RECV, &cqe) == 0);
    wayland_on_cqe(&w, &cqe);
    CHECK(!w.failed);
    // Stale FD for retired_feedback_id MUST have been popped and closed immediately!
    CHECK(w.nparked == 0u);

    // Close peer socket -> multishot recvmsg must detect EOF cleanly and stop re-arming
    close(sv[1]);
    CHECK(wl_wait_tag(&w, ENG_UD_WL_RECV, &cqe) == 0);
    wayland_on_cqe(&w, &cqe);
    CHECK(w.failed);
    CHECK(!w.recv_armed);

    (void)ring_update_file(r, ENG_WL_SOCK_SLOT, -1);
    close(sv[0]);
    free_mock_wl_arrays(&w);
}

static void test_hidpi_fourcc_data_pipe_and_buffer_tracking(RingContext *r) {
    // 1. Fractional HiDPI scale rounding (denominator 120)
    CHECK(wayland_phys_px(0u, 180u) == 0u);
    CHECK(wayland_phys_px(1280u, 120u) == 1280u);
    CHECK(wayland_phys_px(1280u, 150u) == 1600u);
    CHECK(wayland_phys_px(1280u, 180u) == 1920u);
    CHECK(wayland_phys_px(1280u, 240u) == 2560u);
    CHECK(wayland_phys_px(3u, 180u) == 5u); // (3 * 180 + 60) / 120 = 600 / 120 = 5

    // 2. Opaque XRGB8888 vs ARGB8888 modifier selection
    WaylandContext fmt_w = {
        .xrgb_modifiers      = { 0x1001ull, 0x1002ull },
        .xrgb_modifier_count = 2u,
        .argb_modifiers      = { 0x1001ull, 0x1003ull },
        .argb_modifier_count = 2u,
    };
    CHECK(wayland_drm_fourcc_for_modifier(&fmt_w, 0x1001ull, true)  == ENG_DRM_FORMAT_XRGB8888);
    CHECK(wayland_drm_fourcc_for_modifier(&fmt_w, 0x1001ull, false) == ENG_DRM_FORMAT_ARGB8888);
    CHECK(wayland_drm_fourcc_for_modifier(&fmt_w, 0x1002ull, false) == ENG_DRM_FORMAT_XRGB8888);
    CHECK(wayland_drm_fourcc_for_modifier(&fmt_w, 0x1003ull, true)  == ENG_DRM_FORMAT_ARGB8888);

    // 3. Multi-buffer wl_buffer.release tracking + wl_queue_data_receive pipe auto-close
    int sv[2] = { -1, -1 };
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
    WaylandContext w;
    init_mock_wl(&w, r, sv[0]);
    w.recv_iov.iov_base = r->bufs;

    wayland_track_buffer(&w, 301u);
    wayland_track_buffer(&w, 302u);
    CHECK(wayland_buffer_is_released(&w, 301u));
    CHECK(wayland_buffer_is_released(&w, 302u));

    wayland_mark_buffer_busy(&w, 301u);
    wayland_mark_buffer_busy(&w, 302u);
    CHECK(!wayland_buffer_is_released(&w, 301u));
    CHECK(!wayland_buffer_is_released(&w, 302u));

    // Send wl_buffer.release (opcode 0, size 8) for buffer 302 over the wire
    CHECK(wayland_ensure_recv(&w) == 0);
    unsigned char rel_ev[8];
    wl_put_u32(rel_ev + 0, 302u);
    wl_put_u16(rel_ev + 4, 0u);
    wl_put_u16(rel_ev + 6, 8u);
    CHECK(send(sv[1], rel_ev, sizeof(rel_ev), MSG_NOSIGNAL) == 8);

    struct io_uring_cqe cqe = {0};
    CHECK(wl_wait_tag(&w, ENG_UD_WL_RECV, &cqe) == 0);
    wayland_on_cqe(&w, &cqe);
    CHECK(!w.failed);
    CHECK(!wayland_buffer_is_released(&w, 301u));
    CHECK(wayland_buffer_is_released(&w, 302u));

    wayland_untrack_buffer(&w, 301u);
    wayland_untrack_buffer(&w, 302u);
    CHECK(wayland_buffer_is_released(&w, 301u));

    // Queue wl_data_offer.receive with pipe write_fd -> sender's copy MUST auto-close on CQE!
    int pfd[2] = { -1, -1 };
    CHECK(pipe2(pfd, O_CLOEXEC | O_NONBLOCK) == 0);
    CHECK(wl_queue_data_receive(&w, 88u, "text/plain;charset=utf-8", pfd[1]) == 0);
    CHECK(wl_wait_tag(&w, ENG_UD_WL_PONG, &cqe) == 0);
    wayland_on_cqe(&w, &cqe);

    unsigned char rx_buf[64] = {0};
    unsigned char rx_ctl[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = { .iov_base = rx_buf, .iov_len = sizeof(rx_buf) };
    struct msghdr msg = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1u,
        .msg_control    = rx_ctl,
        .msg_controllen = sizeof(rx_ctl),
    };
    ssize_t n = recvmsg(sv[1], &msg, MSG_CMSG_CLOEXEC);
    CHECK(n == 40); // 8B header + 4B len(25) + 28B padded string
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    CHECK(cm != nullptr && cm->cmsg_type == SCM_RIGHTS);
    int peer_write_fd = -1;
    memcpy(&peer_write_fd, CMSG_DATA(cm), sizeof(int));
    CHECK(peer_write_fd >= 0);
    CHECK(write(peer_write_fd, "clip-ok", 7) == 7);
    close(peer_write_fd);

    // Because wl_queue_data_receive closed pfd[1] upon send completion, pfd[0] must hit clean EOF (0) after 7 bytes!
    char clip_out[16] = {0};
    CHECK(read(pfd[0], clip_out, sizeof(clip_out)) == 7);
    CHECK(memcmp(clip_out, "clip-ok", 7) == 0);
    CHECK(read(pfd[0], clip_out, sizeof(clip_out)) == 0);
    close(pfd[0]);

    close(sv[1]);
    CHECK(wl_wait_tag(&w, ENG_UD_WL_RECV, &cqe) == 0);
    wayland_on_cqe(&w, &cqe);
    CHECK(!w.recv_armed);
    (void)ring_update_file(r, ENG_WL_SOCK_SLOT, -1);
    close(sv[0]);
    free_mock_wl_arrays(&w);
}

static uint64_t mono_ns_now(void) {
    struct timespec ts = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void test_send_ring_full_fails_without_drop(RingContext *r) {
    int sv[2] = { -1, -1 };
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);

    WaylandContext w;
    init_mock_wl(&w, r, sv[0]);

    // 1. Fill the 16 KiB outgoing packet ring (ENG_WL_SEND_RING_CAP == 16384)
    //    with 13,000 bytes already queued in tx_ring, then attempt to enqueue a
    //    3,584-byte client batch (13,000 + 3,584 = 16,584 > 16,384).
    //    Must fail immediately (-ENOSPC) without blocking and without dropping
    //    or overwriting any of the 13,000 queued bytes!
    enum { PREFILLED_BYTES = 13000u };
    for (uint32_t i = 0; i < PREFILLED_BYTES; ++i) {
        w.tx_ring[(5000u + i) & (ENG_WL_SEND_RING_CAP - 1u)] = (unsigned char)((i * 31u + 7u) & 0xffu);
    }
    w.tx_head         = 5000u;
    w.tx_tail         = 5000u + PREFILLED_BYTES;
    w.tx_pkt_head     = 0u;
    w.tx_pkt_count    = 1u;
    w.tx_inflight_tag = ENG_UD_WL_PONG;
    w.pong_in_flight  = true;
    w.tx_pkts[0]      = (WaylandSendPacket){
        .offset      = 5000u,
        .len         = PREFILLED_BYTES,
        .sent        = 0u,
        .is_internal = true,
        .kick_armed  = true,
        .fds         = { -1, -1, -1, -1 },
    };

    uint32_t words[2] = { 0xaabbccddu, 0x11223344u };
    for (uint32_t i = 0; i < 224u; ++i) { // 224 * 16 = 3584 bytes
        CHECK(wayland_put_raw(&w, 90u + i, 1u, words, sizeof(words)));
    }
    CHECK(w.send_len == 3584u);

    uint64_t t0 = mono_ns_now();
    int rc = wayland_queue_send(&w, true);
    uint64_t elapsed_ns = mono_ns_now() - t0;
    CHECK(rc == -ENOSPC);
    CHECK(w.failed);
    CHECK(elapsed_ns < 5000000ull); // < 5 ms: zero blocking wait
    CHECK(w.tx_pkt_count == 1u);
    CHECK(w.tx_head == 5000u && w.tx_tail == 5000u + PREFILLED_BYTES);
    for (uint32_t i = 0; i < PREFILLED_BYTES; ++i) {
        unsigned char expected = (unsigned char)((i * 31u + 7u) & 0xffu);
        CHECK(w.tx_ring[(5000u + i) & (ENG_WL_SEND_RING_CAP - 1u)] == expected);
    }

    // 2. Exhaust ENG_WL_SEND_PKT_CAP (64 packets) via real wl_queue_data_receive
    //    and verify the 65th packet returns -ENOSPC without dropping packets 0..63.
    init_mock_wl(&w, r, sv[0]);
    int dummy_fds[ENG_WL_SEND_PKT_CAP + 1u];
    for (uint32_t i = 0; i <= ENG_WL_SEND_PKT_CAP; ++i) {
        dummy_fds[i] = eventfd(i + 1u, EFD_CLOEXEC);
        CHECK(dummy_fds[i] >= 0);
    }
    for (uint32_t i = 0; i < ENG_WL_SEND_PKT_CAP; ++i) {
        char mime[16];
        snprintf(mime, sizeof(mime), "x/p%02u", i);
        CHECK(wl_queue_data_receive(&w, 200u + i, mime, dummy_fds[i]) == 0);
    }
    CHECK(w.tx_pkt_count == ENG_WL_SEND_PKT_CAP);
    CHECK(!w.failed);

    t0 = mono_ns_now();
    rc = wl_queue_data_receive(&w, 999u, "x/overflow", dummy_fds[ENG_WL_SEND_PKT_CAP]);
    elapsed_ns = mono_ns_now() - t0;
    CHECK(rc == -ENOSPC);
    CHECK(w.failed);
    CHECK(elapsed_ns < 5000000ull);
    CHECK(w.tx_pkt_count == ENG_WL_SEND_PKT_CAP);
    for (uint32_t i = 0; i < ENG_WL_SEND_PKT_CAP; ++i) {
        uint32_t obj = 0;
        memcpy(&obj, w.tx_ring + w.tx_pkts[i].offset, sizeof(obj));
        CHECK(obj == 200u + i);
    }
    close(dummy_fds[ENG_WL_SEND_PKT_CAP]);
    // Clear failed flag and flush all 64 surviving packets over the socketpair
    // to prove not a single packet or SCM_RIGHTS FD was dropped!
    w.failed = false;
    CHECK(wl_flush_pending(&w) == 0);
    CHECK(w.tx_pkt_count == 0u);
    for (uint32_t i = 0; i < ENG_WL_SEND_PKT_CAP; ++i) {
        unsigned char pkt_buf[64] = {0};
        unsigned char pkt_ctl[CMSG_SPACE(sizeof(int))] = {0};
        struct iovec pkt_iov = { .iov_base = pkt_buf, .iov_len = sizeof(pkt_buf) };
        struct msghdr pkt_msg = {
            .msg_iov        = &pkt_iov,
            .msg_iovlen     = 1u,
            .msg_control    = pkt_ctl,
            .msg_controllen = sizeof(pkt_ctl),
        };
        ssize_t n = recvmsg(sv[1], &pkt_msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
        CHECK(n == 20);
        uint32_t got_obj = 0;
        memcpy(&got_obj, pkt_buf, sizeof(got_obj));
        CHECK(got_obj == 200u + i);
        struct cmsghdr *pcm = CMSG_FIRSTHDR(&pkt_msg);
        CHECK(pcm != nullptr && pcm->cmsg_type == SCM_RIGHTS);
        int rx_fd = -1;
        memcpy(&rx_fd, CMSG_DATA(pcm), sizeof(rx_fd));
        if (rx_fd >= 0) {
            close(rx_fd);
        }
    }
    struct io_uring_cqe cqe = {0};
    close(sv[1]);
    CHECK(wl_wait_tag(&w, ENG_UD_WL_RECV, &cqe) == 0);
    wayland_on_cqe(&w, &cqe);
    CHECK(!w.recv_armed);
    (void)ring_update_file(r, ENG_WL_SOCK_SLOT, -1);
    close(sv[0]);

    // 3. Fill ENG_WL_SEND_STAGE_CAP (4,096 bytes) and verify overflow returns false
    //    without truncating or corrupting staged messages.
    w = (WaylandContext){ .is_initialized = true };
    for (uint32_t i = 0; i < ENG_WL_SEND_STAGE_CAP / 16u; ++i) {
        words[0] = 0x7000u + i;
        CHECK(wayland_put_raw(&w, 30u, 4u, words, sizeof(words)));
    }
    CHECK(w.send_len == ENG_WL_SEND_STAGE_CAP);
    CHECK(!wayland_put_raw(&w, 30u, 4u, words, sizeof(words)));
    CHECK(w.failed);
    CHECK(w.send_len == ENG_WL_SEND_STAGE_CAP);
    uint32_t first_word = 0, last_word = 0;
    memcpy(&first_word, w.send_bytes + 8u, 4u);
    memcpy(&last_word, w.send_bytes + ENG_WL_SEND_STAGE_CAP - 8u, 4u);
    CHECK(first_word == 0x7000u);
    CHECK(last_word == 0x7000u + (ENG_WL_SEND_STAGE_CAP / 16u) - 1u);

    free_mock_wl_arrays(&w);
}

int main(void) {
    CHECK(system_init() == 0);
    RingContext r = {0};
    CHECK(ring_context_init(&r, "test_wl", true) == 0);

    test_burst_ids_and_client_cap();
    test_wire_wraparound_pong_and_destroys(&r);
    test_hidpi_fourcc_data_pipe_and_buffer_tracking(&r);
    test_send_ring_full_fails_without_drop(&r);

    ring_context_shutdown(&r);
    system_shutdown();
    if (s_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_wayland_wire: all checks passed\n");
    return 0;
}
