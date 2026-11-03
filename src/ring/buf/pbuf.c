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

static int register_group(RingContext *r, struct io_uring_buf_ring *ring, uint16_t entries, uint16_t bgid) {
    struct io_uring_buf_reg breg = {
        .ring_addr    = (uint64_t)(uintptr_t)ring,
        .ring_entries = entries,
        .bgid         = bgid,
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
    publish_group(r->pbuf, r->bufs, ENG_PBUF_ENTRIES, ENG_PBUF_LEN, &r->pbuf_tail, &r->pbuf_mask);
    int rc = register_group(r, r->pbuf, ENG_PBUF_ENTRIES, ENG_PBUF_BGID);
    if (rc == 0) {
        r->pbuf_on = true;
    }
    return rc;
}

int ring_sim_pbuf_register(RingContext *r) {
    publish_group(r->sim_pbuf, r->sim_bufs, ENG_SIM_PBUF_ENTRIES, ENG_SIM_PBUF_LEN,
                  &r->sim_pbuf_tail, &r->sim_pbuf_mask);
    int rc = register_group(r, r->sim_pbuf, ENG_SIM_PBUF_ENTRIES, ENG_SIM_BGID);
    if (rc == 0) {
        r->sim_pbuf_on = true;
    }
    return rc;
}

uint8_t *ring_buf(const RingContext *r, uint16_t bid) {
    if (r == nullptr || bid >= ENG_PBUF_ENTRIES || r->bufs == nullptr) {
        return nullptr;
    }
    return r->bufs + (size_t)bid * ENG_PBUF_LEN;
}

void ring_pbuf_recycle(RingContext *r, uint16_t bid) {
    if (r == nullptr || bid >= ENG_PBUF_ENTRIES || r->pbuf == nullptr) {
        return;
    }
    recycle(r->pbuf, r->bufs, bid, ENG_PBUF_LEN, &r->pbuf_tail, r->pbuf_mask);
}

uint8_t *ring_pbuf_chunk(const RingContext *r, uint16_t bid, uint32_t nbytes) {
    if (r == nullptr || bid >= ENG_PBUF_ENTRIES || r->bufs == nullptr || nbytes > ENG_PBUF_LEN) {
        return nullptr;
    }
    return r->bufs + (size_t)bid * ENG_PBUF_LEN;
}

void ring_pbuf_finish(RingContext *r, uint16_t bid) {
    ring_pbuf_recycle(r, bid);
}

static uint8_t *sim_buf(const RingContext *r, uint16_t bid) {
    if (r == nullptr || bid >= ENG_SIM_PBUF_ENTRIES || r->sim_bufs == nullptr) {
        return nullptr;
    }
    return r->sim_bufs + (size_t)bid * ENG_SIM_PBUF_LEN;
}

bool ring_take_sim_count(RingContext *r, const struct io_uring_cqe *cqe, uint64_t *out) {
    if (r == nullptr || cqe == nullptr || out == nullptr) {
        return false;
    }
    if ((cqe->flags & IORING_CQE_F_BUFFER) == 0) {
        return false;
    }
    uint16_t bid = (uint16_t)(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
    uint8_t *p = sim_buf(r, bid);
    if (p == nullptr) {
        return false;
    }
    bool full = cqe->res == (int32_t)ENG_SIM_PBUF_LEN;
    if (full) {
        uint64_t n = 0;
        memcpy(&n, p, sizeof n);
        *out = n;
    }
    recycle(r->sim_pbuf, r->sim_bufs, bid, ENG_SIM_PBUF_LEN, &r->sim_pbuf_tail, r->sim_pbuf_mask);
    return full;
}

int ring_arm_read_mshot(RingContext *r, uint32_t slot, bool fixed_file, int raw_fd,
                        uint16_t bgid, uint64_t user_data) {
    if (r == nullptr || !r->is_initialized || !r->sim_pbuf_on || (!fixed_file && raw_fd < 0)) {
        return -EINVAL;
    }
    if (fixed_file && slot >= ENG_FIXED_FILES) {
        return -EINVAL;
    }
    struct io_uring_sqe *sqe = ring_sqe(r);
    if (sqe == nullptr) {
        return -ENOSPC;
    }
    // io_read_mshot_prep rejects a fixed buffer and any non-zero addr or len.
    // buf_group is the provided-buffer ring the kernel pops on each tick.
    sqe->opcode    = IORING_OP_READ_MULTISHOT;
    sqe->flags     = IOSQE_BUFFER_SELECT;
    sqe->fd        = fixed_file ? (int)slot : raw_fd;
    if (fixed_file) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }
    sqe->addr      = 0;
    sqe->len       = 0;
    sqe->off       = 0;
    sqe->buf_group = bgid;
    sqe->user_data = user_data;
    return 0;
}
