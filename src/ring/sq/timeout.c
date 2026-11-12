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

int ring_arm_timeout_multishot(RingContext *r, uint64_t period_ns, uint64_t user_data) {
    if (r == nullptr || !r->is_initialized || period_ns == 0ull) {
        return -EINVAL;
    }
    struct io_uring_sqe *sqe = ring_sqe(r);
    if (sqe == nullptr) {
        return -ENOSPC;
    }
    sqe->opcode        = IORING_OP_TIMEOUT;
    sqe->fd            = -1;
    sqe->off           = 0;
    sqe->addr          = period_ns;
    sqe->len           = 1;
    sqe->timeout_flags = IORING_TIMEOUT_MULTISHOT | IORING_TIMEOUT_IMMEDIATE_ARG;
    sqe->user_data     = user_data;
    return 0;
}

int ring_update_timeout_abs(RingContext *r, uint64_t deadline_ns, uint64_t target_ud, uint64_t update_ud) {
    if (r == nullptr || !r->is_initialized) {
        return -EINVAL;
    }
    struct io_uring_sqe *sqe = ring_sqe(r);
    if (sqe == nullptr) {
        return -ENOSPC;
    }
    r->timeout_update_ts.tv_sec  = (long long)(deadline_ns / 1000000000ull);
    r->timeout_update_ts.tv_nsec = (long long)(deadline_ns % 1000000000ull);
    sqe->opcode        = IORING_OP_TIMEOUT_REMOVE;
    sqe->flags         = IOSQE_CQE_SKIP_SUCCESS;
    sqe->fd            = -1;
    sqe->addr          = target_ud;
    sqe->addr2         = (uint64_t)(uintptr_t)&r->timeout_update_ts;
    sqe->timeout_flags = IORING_TIMEOUT_UPDATE | IORING_TIMEOUT_ABS;
    sqe->user_data     = update_ud;
    return 0;
}

int ring_remove_timeout(RingContext *r, uint64_t target_ud, uint64_t remove_ud, bool skip_success) {
    if (r == nullptr || !r->is_initialized) {
        return -EINVAL;
    }
    struct io_uring_sqe *sqe = ring_sqe(r);
    if (sqe == nullptr) {
        return -ENOSPC;
    }
    sqe->opcode        = IORING_OP_TIMEOUT_REMOVE;
    sqe->flags         = skip_success ? IOSQE_CQE_SKIP_SUCCESS : 0;
    sqe->fd            = -1;
    sqe->addr          = target_ud;
    sqe->timeout_flags = 0;
    sqe->user_data     = remove_ud;
    return 0;
}
