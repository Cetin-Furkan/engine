#include "ring/internal.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>

uint64_t ring_mono_ns(void) {
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int ring_enter_retry(RingContext *r, unsigned submit, unsigned min_complete,
                            unsigned flags, long arg, size_t argz) {
    int fd = r->enter_fd;
    if (r->ring_registered) {
        flags |= IORING_ENTER_REGISTERED_RING;
    }
    int again = 0;
    for (;;) {
        long rc = ring_sys6(__NR_io_uring_enter, fd, submit, min_complete, flags, arg, (long)argz);
        if (rc >= 0) {
            return (int)rc;
        }
        if (rc == -EINTR) {
            continue;
        }
        if (rc == -EAGAIN && submit > 0 && again++ < 8) {
            continue;
        }
        return (int)rc;
    }
}

int ring_enter(RingContext *r, unsigned submit, unsigned min_complete, int timeout_ms) {
    unsigned flags = 0;
    void *arg      = nullptr;
    size_t argz    = 0;

    if (min_complete > 0) {
        flags |= IORING_ENTER_GETEVENTS;
    }
    uint32_t sq_flags = __atomic_load_n(r->sq_kflags, __ATOMIC_ACQUIRE);
    if ((sq_flags & IORING_SQ_TASKRUN) != 0) {
        flags |= IORING_ENTER_GETEVENTS;
    }
    if (timeout_ms >= 0) {
        if (!r->wait_reg_on || r->wait == nullptr) {
            return -EOPNOTSUPP;
        }
        r->wait[0] = (struct io_uring_reg_wait){
            .ts = {
                .tv_sec  = timeout_ms / 1000,
                .tv_nsec = (long)(timeout_ms % 1000) * 1000000L,
            },
            // IORING_FEAT_MIN_TIMEOUT is required. The kernel may batch this
            // wakeup for up to 500us.
            .min_wait_usec = 500u,
            .flags         = IORING_REG_WAIT_TS,
            .sigmask       = 0,
            .sigmask_sz    = 0,
        };
        __atomic_thread_fence(__ATOMIC_RELEASE);
        flags |= IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG | IORING_ENTER_EXT_ARG_REG;
        arg    = (void *)(uintptr_t)0u;
        argz   = sizeof(struct io_uring_reg_wait);
    }
    if ((flags & IORING_ENTER_GETEVENTS) != 0) {
        flags |= r->wait_flags;
    }
    return ring_enter_retry(r, submit, min_complete, flags, (long)arg, argz);
}

bool ring_peek(RingContext *r, struct io_uring_cqe *out) {
    // CQ overflow means completions were dropped by the kernel: count it
    // and say so once, never silently. SQ dropped is checked on submit.
    uint32_t overflow = __atomic_load_n(r->cq_koverflow, __ATOMIC_ACQUIRE);
    if (overflow != r->cq_overflow_total) {
        r->cq_overflow_total = overflow;
        if (!r->overflow_warned) {
            r->overflow_warned = true;
            fprintf(stderr, "[Ring] CQ overflow: %u completion(s) dropped by the kernel\n", overflow);
        }
    }
    uint32_t tail = __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE);
    uint32_t head = r->cq_head;
    if (head == tail) {
        // DEFER_TASKRUN holds interrupt completions off the CQ until this
        // thread enters. A zero-submit enter runs that task_work.
        uint32_t sq_flags = __atomic_load_n(r->sq_kflags, __ATOMIC_ACQUIRE);
        if ((sq_flags & IORING_SQ_TASKRUN) != 0) {
            (void)ring_enter_retry(r, 0, 0, IORING_ENTER_GETEVENTS | r->wait_flags, 0, 0);
            tail = __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE);
        }
    }
    if (head == tail) {
        return false;
    }
    *out = r->cqes[head & r->cq_mask];
    head++;
    __atomic_store_n(r->cq_khead, head, __ATOMIC_RELEASE);
    r->cq_head = head;
    return true;
}
