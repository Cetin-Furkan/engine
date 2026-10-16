/* === FILE: ./src/init/ring_init/ring_init.c === */

#include "init/ring_init/ring_init.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static constexpr uint32_t k_ring_setup_mask =
    IORING_SETUP_SINGLE_ISSUER |
    IORING_SETUP_DEFER_TASKRUN |
    IORING_SETUP_TASKRUN_FLAG  |
    IORING_SETUP_NO_SQARRAY    |
    IORING_SETUP_CQSIZE        |
    IORING_SETUP_CLAMP         |
    IORING_SETUP_SUBMIT_ALL    |
    IORING_SETUP_NO_MMAP       |
    IORING_SETUP_R_DISABLED;

static RingContext s_primary_ring = { .fd = -1 };

uint64_t ring_mono_ns(void) {
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static long sys6(long nr, long a, long b, long c, long d, long e, long f) {
    long rc = syscall(nr, a, b, c, d, e, f);
    return (rc < 0) ? -errno : rc;
}

static size_t query_page_size(void) {
    long p = sysconf(_SC_PAGESIZE);
    return (p > 0) ? (size_t)p : 4096u;
}

static size_t align_up(size_t n, size_t a) {
    return (n + a - 1u) & ~(a - 1u);
}

static int ring_enter_retry(RingContext *r, unsigned submit, unsigned min_complete,
                            unsigned flags, long arg, size_t argz) {
    int again = 0;
    for (;;) {
        long rc = sys6(__NR_io_uring_enter, r->fd, submit, min_complete, flags, arg, (long)argz);
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

int ring_context_init(RingContext *r, const char *name, bool with_pbuf) {
    if (r == nullptr) {
        return -EINVAL;
    }

    *r = (RingContext){ .fd = -1 };
    if (name != nullptr) {
        snprintf(r->name, sizeof r->name, "%s", name);
    }

    size_t page = query_page_size();
    size_t span = 64u * 1024u;
    if (span < page) {
        span = page;
    }

    r->sqes_off = span;
    size_t len = span * 2u;
    if (with_pbuf) {
        r->pbuf_off = len;
        r->bufs_off = r->pbuf_off + page;
        len = r->bufs_off + (size_t)ENG_PBUF_ENTRIES * ENG_PBUF_LEN;
    }
    r->wait_off = align_up(len, page);
    len = r->wait_off + page;

    void *arena = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena == MAP_FAILED) {
        fprintf(stderr, "[Ring Error] Arena mmap of %zu bytes failed: %s\n", len, strerror(errno));
        return -ENOMEM;
    }
    memset(arena, 0, len);

    r->arena     = arena;
    r->arena_len = len;
    r->rings     = arena;
    r->sqes_mem  = (char *)arena + r->sqes_off;
    if (with_pbuf) {
        r->pbuf = (struct io_uring_buf_ring *)((char *)arena + r->pbuf_off);
        r->bufs = (uint8_t *)arena + r->bufs_off;
    }
    r->wait = (struct io_uring_reg_wait *)((char *)arena + r->wait_off);

    struct io_uring_params p = {
        .flags      = k_ring_setup_mask,
        .cq_entries = ENG_CQ_ENTRIES,
        .cq_off     = { .user_addr = (uint64_t)(uintptr_t)r->rings },
        .sq_off     = { .user_addr = (uint64_t)(uintptr_t)r->sqes_mem },
    };

    long rc = sys6(__NR_io_uring_setup, ENG_SQ_ENTRIES, (long)&p, 0, 0, 0, 0);
    if (rc < 0) {
        fprintf(stderr, "[Ring Error] io_uring_setup failed: %s\n", strerror((int)-rc));
        ring_context_shutdown(r);
        return (int)rc;
    }

    r->fd           = (int)rc;
    r->features     = p.features;
    r->sq_entries   = p.sq_entries;
    r->cq_entries   = p.cq_entries;
    r->sqes         = (struct io_uring_sqe *)r->sqes_mem;
    r->sq_khead     = (uint32_t *)((char *)r->rings + p.sq_off.head);
    r->sq_ktail     = (uint32_t *)((char *)r->rings + p.sq_off.tail);
    r->sq_kflags    = (uint32_t *)((char *)r->rings + p.sq_off.flags);
    r->sq_kdropped  = (uint32_t *)((char *)r->rings + p.sq_off.dropped);
    r->cq_khead     = (uint32_t *)((char *)r->rings + p.cq_off.head);
    r->cq_ktail     = (uint32_t *)((char *)r->rings + p.cq_off.tail);
    r->cq_koverflow = (uint32_t *)((char *)r->rings + p.cq_off.overflow);
    r->cqes         = (struct io_uring_cqe *)((char *)r->rings + p.cq_off.cqes);
    r->sq_mask      = *(uint32_t *)((char *)r->rings + p.sq_off.ring_mask);
    r->cq_mask      = *(uint32_t *)((char *)r->rings + p.cq_off.ring_mask);

    struct io_uring_clock_register clk = { .clockid = CLOCK_MONOTONIC };
    rc = sys6(__NR_io_uring_register, r->fd, IORING_REGISTER_CLOCK, (long)&clk, 0, 0, 0);
    if (rc < 0) {
        fprintf(stderr, "[Ring Error] IORING_REGISTER_CLOCK failed: %s\n", strerror((int)-rc));
        ring_context_shutdown(r);
        return (int)rc;
    }

    struct io_uring_region_desc rd = {
        .user_addr = (uint64_t)(uintptr_t)r->wait,
        .size      = page,
        .flags     = IORING_MEM_REGION_TYPE_USER,
    };
    struct io_uring_mem_region_reg mreg = {
        .region_uptr = (uint64_t)(uintptr_t)&rd,
        .flags       = IORING_MEM_REGION_REG_WAIT_ARG,
    };
    rc = sys6(__NR_io_uring_register, r->fd, IORING_REGISTER_MEM_REGION, (long)&mreg, 1, 0, 0);
    if (rc < 0) {
        fprintf(stderr, "[Ring Error] IORING_REGISTER_MEM_REGION failed: %s\n", strerror((int)-rc));
        ring_context_shutdown(r);
        return (int)rc;
    }

    if (with_pbuf) {
        r->pbuf_mask = ENG_PBUF_ENTRIES - 1u;
        for (uint16_t i = 0; i < ENG_PBUF_ENTRIES; ++i) {
            struct io_uring_buf *b = &r->pbuf->bufs[i];
            b->addr = (uint64_t)(uintptr_t)(r->bufs + (size_t)i * ENG_PBUF_LEN);
            b->len  = ENG_PBUF_LEN;
            b->bid  = i;
        }
        r->pbuf_tail = ENG_PBUF_ENTRIES;
        __atomic_store_n(&r->pbuf->tail, r->pbuf_tail, __ATOMIC_RELEASE);

        struct io_uring_buf_reg breg = {
            .ring_addr    = (uint64_t)(uintptr_t)r->pbuf,
            .ring_entries = ENG_PBUF_ENTRIES,
            .bgid         = ENG_PBUF_BGID,
        };
        rc = sys6(__NR_io_uring_register, r->fd, IORING_REGISTER_PBUF_RING, (long)&breg, 1, 0, 0);
        if (rc < 0) {
            fprintf(stderr, "[Ring Error] IORING_REGISTER_PBUF_RING failed: %s\n", strerror((int)-rc));
            ring_context_shutdown(r);
            return (int)rc;
        }
        r->pbuf_on = true;
    }

    rc = sys6(__NR_io_uring_register, r->fd, IORING_REGISTER_ENABLE_RINGS, 0, 0, 0, 0);
    if (rc < 0) {
        fprintf(stderr, "[Ring Error] IORING_REGISTER_ENABLE_RINGS failed: %s\n", strerror((int)-rc));
        ring_context_shutdown(r);
        return (int)rc;
    }

    int sparse_files[ENG_FIXED_FILES];
    for (uint32_t i = 0; i < ENG_FIXED_FILES; ++i) {
        sparse_files[i] = -1;
    }
    rc = sys6(__NR_io_uring_register, r->fd, IORING_REGISTER_FILES,
              (long)sparse_files, ENG_FIXED_FILES, 0, 0);
    if (rc < 0) {
        fprintf(stderr, "[Ring Error] IORING_REGISTER_FILES failed: %s\n", strerror((int)-rc));
        ring_context_shutdown(r);
        return (int)rc;
    }

    if ((r->features & IORING_FEAT_NO_IOWAIT) != 0) {
        r->wait_flags = IORING_ENTER_NO_IOWAIT;
    }

    r->is_initialized = true;
    printf("[Ring] Initialized '%s' (fd=%d, SQ=%u, CQ=%u, PBUF=%ux%uB, FixedFiles=%u, NO_MMAP+SINGLE_ISSUER+DEFER_TASKRUN)\n",
           r->name,
           r->fd,
           r->sq_entries,
           r->cq_entries,
           with_pbuf ? ENG_PBUF_ENTRIES : 0u,
           ENG_PBUF_LEN,
           ENG_FIXED_FILES);
    return 0;
}

void ring_context_shutdown(RingContext *r) {
    if (r == nullptr) {
        return;
    }
    if (r->fd >= 0) {
        close(r->fd);
        r->fd = -1;
    }
    if (r->arena != nullptr && r->arena_len > 0) {
        munmap(r->arena, r->arena_len);
    }
    *r = (RingContext){ .fd = -1 };
}

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

unsigned ring_submit(RingContext *r) {
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

int ring_enter(RingContext *r, unsigned submit, unsigned min_complete, int timeout_ms) {
    struct __kernel_timespec ts = {0};
    struct io_uring_getevents_arg garg = {0};
    unsigned flags = 0;
    void *arg      = nullptr;
    size_t argz    = 0;

    if (min_complete > 0) {
        flags |= IORING_ENTER_GETEVENTS;
    }
    if (timeout_ms >= 0) {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        garg.ts    = (uint64_t)(uintptr_t)&ts;
        flags     |= IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG;
        arg        = &garg;
        argz       = sizeof(garg);
    }
    if ((flags & IORING_ENTER_GETEVENTS) != 0) {
        flags |= r->wait_flags;
    }
    return ring_enter_retry(r, submit, min_complete, flags, (long)arg, argz);
}

bool ring_peek(RingContext *r, struct io_uring_cqe *out) {
    uint32_t tail = __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE);
    uint32_t head = r->cq_head;
    if (head == tail) {
        return false;
    }
    *out = r->cqes[head & r->cq_mask];
    head++;
    __atomic_store_n(r->cq_khead, head, __ATOMIC_RELEASE);
    r->cq_head = head;
    return true;
}

uint8_t *ring_buf(const RingContext *r, uint16_t bid) {
    if (bid >= ENG_PBUF_ENTRIES) {
        return nullptr;
    }
    return r->bufs + (size_t)bid * ENG_PBUF_LEN;
}

void ring_pbuf_recycle(RingContext *r, uint16_t bid) {
    uint16_t slot = r->pbuf_tail & r->pbuf_mask;
    struct io_uring_buf *b = &r->pbuf->bufs[slot];
    b->addr = (uint64_t)(uintptr_t)ring_buf(r, bid);
    b->len  = ENG_PBUF_LEN;
    b->bid  = bid;
    r->pbuf_tail++;
    __atomic_store_n(&r->pbuf->tail, r->pbuf_tail, __ATOMIC_RELEASE);
}

int ring_init(void) {
    return (ring_context_init(&s_primary_ring, "primary", true) == 0) ? 0 : 1;
}

void ring_shutdown(void) {
    if (s_primary_ring.is_initialized) {
        ring_context_shutdown(&s_primary_ring);
        printf("goodbye from ring\n");
    }
}

RingContext *ring_get_context(void) {
    return &s_primary_ring;
}