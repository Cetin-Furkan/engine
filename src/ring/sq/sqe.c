#include "ring/internal.h"
#include <stdio.h>
#include <string.h>

struct io_uring_sqe *ring_sqe(RingContext *r) {
    uint32_t head = __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE);
    uint32_t next = r->sq_tail + r->sq_pending;
    if (next - head >= r->sq_entries) {
        return nullptr;
    }
    struct io_uring_sqe *sqe = &r->sqes[next & r->sq_mask];
    memset(sqe, 0, sizeof(*sqe));
    r->sq_pending++;
    return sqe;
}

void ring_sqe_unget(RingContext *r, uint32_t n) {
    if (r == nullptr) {
        return;
    }
    if (n > r->sq_pending) {
        r->sq_pending = 0;
        return;
    }
    r->sq_pending -= n;
}

unsigned ring_submit(RingContext *r) {
    uint32_t dropped = __atomic_load_n(r->sq_kdropped, __ATOMIC_ACQUIRE);
    if (dropped != r->sq_dropped_total) {
        r->sq_dropped_total = dropped;
        fprintf(stderr, "[Ring] SQ dropped: %u submission(s) never ran\n", dropped);
    }
    unsigned n = r->sq_pending;
    if (n == 0) {
        return 0;
    }
    uint32_t tail = r->sq_tail + n;
    __atomic_store_n(r->sq_ktail, tail, __ATOMIC_RELEASE);
    r->sq_tail    = tail;
    r->sq_pending = 0;
    return n;
}
