#include "ring/internal.h"
#include <errno.h>
#include <string.h>

static void publish_group(struct io_uring_buf_ring *ring, uint8_t *bufs, uint16_t entries,
                          uint32_t stride, uint16_t *tail, uint16_t *mask) {
    *mask = (uint16_t)(entries - 1u);
    for (uint16_t i = 0; i < entries; ++i) {
        struct io_uring_buf *b = &ring->bufs[i];
        b->addr = (uint64_t)(uintptr_t)(bufs + (size_t)i * stride);
        b->len  = stride;
        b->bid  = i;
    }
    *tail = entries;
    __atomic_store_n(&ring->tail, *tail, __ATOMIC_RELEASE);
}

static int register_group(RingContext *r, struct io_uring_buf_ring *ring, uint16_t entries,
                          uint16_t bgid, uint16_t flags, uint32_t min_left) {
    struct io_uring_buf_reg breg = {
        .ring_addr    = (uint64_t)(uintptr_t)ring,
        .ring_entries = entries,
        .bgid         = bgid,
        .flags        = flags,
        .min_left     = min_left,
    };
    long rc = ring_register(r, IORING_REGISTER_PBUF_RING, (long)&breg, 1);
    return (rc < 0) ? (int)rc : 0;
}

static void recycle(struct io_uring_buf_ring *ring, uint8_t *bufs, uint16_t bid, uint32_t stride,
                    uint16_t *tail, uint16_t mask) {
    uint16_t slot = (uint16_t)(*tail & mask);
    struct io_uring_buf *b = &ring->bufs[slot];
    b->addr = (uint64_t)(uintptr_t)(bufs + (size_t)bid * stride);
    b->len  = stride;
    b->bid  = bid;
    (*tail)++;
    __atomic_store_n(&ring->tail, *tail, __ATOMIC_RELEASE);
}

int ring_pbuf_register(RingContext *r) {
    memset(r->pbuf_bid_off, 0, sizeof r->pbuf_bid_off);
    publish_group(r->pbuf, r->bufs, ENG_PBUF_ENTRIES, ENG_PBUF_LEN, &r->pbuf_tail, &r->pbuf_mask);

    // Prefer Linux 6.12+/7.x incremental buffer consumption with min_left guard so
    // each 8 KiB recv buffer packs dozens of Wayland wire messages without truncating
    // struct io_uring_recvmsg_out + SCM_RIGHTS control headers.
    int rc = register_group(r, r->pbuf, ENG_PBUF_ENTRIES, ENG_PBUF_BGID,
                            IOU_PBUF_RING_INC, ENG_PBUF_MIN_LEFT);
    if (rc == 0) {
        r->pbuf_on     = true;
        r->pbuf_inc_on = true;
        return 0;
    }
    rc = register_group(r, r->pbuf, ENG_PBUF_ENTRIES, ENG_PBUF_BGID, 0u, 0u);
    if (rc == 0) {
        r->pbuf_on     = true;
        r->pbuf_inc_on = false;
    }
    return rc;
}

void ring_pbuf_recycle(RingContext *r, uint16_t bid) {
    if (r == nullptr || bid >= ENG_PBUF_ENTRIES || r->pbuf == nullptr) {
        return;
    }
    r->pbuf_bid_off[bid] = 0u;
    recycle(r->pbuf, r->bufs, bid, ENG_PBUF_LEN, &r->pbuf_tail, r->pbuf_mask);
}

uint8_t *ring_pbuf_chunk(const RingContext *r, uint16_t bid, uint32_t nbytes) {
    if (r == nullptr || bid >= ENG_PBUF_ENTRIES || r->bufs == nullptr || nbytes > ENG_PBUF_LEN) {
        return nullptr;
    }
    uint32_t off = r->pbuf_inc_on ? r->pbuf_bid_off[bid] : 0u;
    if (off > ENG_PBUF_LEN - nbytes) {
        return nullptr;
    }
    return r->bufs + (size_t)bid * ENG_PBUF_LEN + off;
}

void ring_pbuf_finish(RingContext *r, uint16_t bid, uint32_t cqe_flags, int32_t cqe_res) {
    if (r == nullptr || bid >= ENG_PBUF_ENTRIES || r->pbuf == nullptr) {
        return;
    }
    if (r->pbuf_inc_on && (cqe_flags & IORING_CQE_F_BUF_MORE) != 0 && cqe_res > 0) {
        uint32_t next_off = r->pbuf_bid_off[bid] + (uint32_t)cqe_res;
        if (next_off <= ENG_PBUF_LEN) {
            r->pbuf_bid_off[bid] = next_off;
            return;
        }
    }
    ring_pbuf_recycle(r, bid);
}
