#include "ring/internal.h"
#include <errno.h>

int ring_arm_timeout_abs(RingContext *r, uint64_t deadline_ns, uint64_t user_data) {
    if (r == nullptr || !r->is_initialized) {
        return -EINVAL;
    }
    struct io_uring_sqe *sqe = ring_sqe(r);
    if (sqe == nullptr) {
        return -ENOSPC;
    }
    // 7.3 io_timeout_prep still requires sqe->len == 1 before it reads
    // IORING_TIMEOUT_IMMEDIATE_ARG. The nanosecond deadline is sqe->addr.
    // sqe->off stays 0: this is a pure timeout, not an event count.
    sqe->opcode        = IORING_OP_TIMEOUT;
    sqe->fd            = -1;
    sqe->off           = 0;
    sqe->addr          = deadline_ns;
    sqe->len           = 1;
    sqe->timeout_flags = IORING_TIMEOUT_ABS | IORING_TIMEOUT_IMMEDIATE_ARG;
    sqe->user_data     = user_data;
    return 0;
}
