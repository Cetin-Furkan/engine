#include "ring/internal.h"
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
    IORING_SETUP_REGISTERED_FD_ONLY |
    IORING_SETUP_R_DISABLED;

static constexpr uint32_t k_required_features =
    IORING_FEAT_EXT_ARG |
    IORING_FEAT_CQE_SKIP |
    IORING_FEAT_LINKED_FILE |
    IORING_FEAT_REG_REG_RING |
    IORING_FEAT_MIN_TIMEOUT |
    IORING_FEAT_NO_IOWAIT;

long ring_sys6(long nr, long a, long b, long c, long d, long e, long f) {
    long rc = syscall(nr, a, b, c, d, e, f);
    return (rc < 0) ? -errno : rc;
}

long ring_register(RingContext *r, unsigned opcode, long arg, unsigned nr) {
    int fd = r->enter_fd;
    if (r->ring_registered) {
        opcode |= IORING_REGISTER_USE_REGISTERED_RING;
    }
    return ring_sys6(__NR_io_uring_register, fd, (long)opcode, arg, (long)nr, 0, 0);
}

size_t ring_align_up(size_t n, size_t a) {
    return (n + a - 1u) & ~(a - 1u);
}

static size_t query_page_size(void) {
    long p = sysconf(_SC_PAGESIZE);
    return (p > 0) ? (size_t)p : 4096u;
}

static int require_features(uint32_t features) {
    uint32_t missing = k_required_features & ~features;
    if (missing == 0) {
        return 0;
    }
    fprintf(stderr, "[Ring Error] kernel io_uring is missing feature bits 0x%x\n", missing);
    return -EOPNOTSUPP;
}

int ring_context_init(RingContext *r, const char *name, bool with_pbuf) {
    if (r == nullptr) {
        return -EINVAL;
    }
    *r = (RingContext){ .fd = -1, .enter_fd = -1 };
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
        r->sim_pbuf_off = ring_align_up(len, page);
        r->sim_bufs_off = r->sim_pbuf_off + page;
        len = r->sim_bufs_off + (size_t)ENG_SIM_PBUF_ENTRIES * ENG_SIM_PBUF_LEN;
    }
    r->fixed_off = ring_align_up(len, page);
    len = r->fixed_off + (size_t)ENG_FBUF_INOTIFY_LEN + ENG_FBUF_SIGNAL_LEN + ENG_FBUF_RELEASE_LEN;
    r->wait_off = ring_align_up(len, page);
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
        r->sim_pbuf = (struct io_uring_buf_ring *)((char *)arena + r->sim_pbuf_off);
        r->sim_bufs = (uint8_t *)arena + r->sim_bufs_off;
    }
    r->fixed_base = (uint8_t *)arena + r->fixed_off;
    r->wait = (struct io_uring_reg_wait *)((char *)arena + r->wait_off);

    struct io_uring_params p = {
        .flags      = k_ring_setup_mask,
        .cq_entries = ENG_CQ_ENTRIES,
        .cq_off     = { .user_addr = (uint64_t)(uintptr_t)r->rings },
        .sq_off     = { .user_addr = (uint64_t)(uintptr_t)r->sqes_mem },
    };

    long rc = ring_sys6(__NR_io_uring_setup, ENG_SQ_ENTRIES, (long)&p, 0, 0, 0, 0);
    if (rc < 0) {
        fprintf(stderr, "[Ring Error] io_uring_setup failed: %s\n", strerror((int)-rc));
        ring_context_shutdown(r);
        return (int)rc;
    }

    // The return is a registered-ring index. Index 0 is valid and is not stdin.
    r->fd              = -1;
    r->enter_fd        = (int)rc;
    r->ring_registered = true;
    r->features        = p.features;
    r->sq_entries      = p.sq_entries;
    r->cq_entries      = p.cq_entries;
    r->sqes            = (struct io_uring_sqe *)r->sqes_mem;
    r->sq_khead        = (uint32_t *)((char *)r->rings + p.sq_off.head);
    r->sq_ktail        = (uint32_t *)((char *)r->rings + p.sq_off.tail);
    r->sq_kflags       = (uint32_t *)((char *)r->rings + p.sq_off.flags);
    r->sq_kdropped     = (uint32_t *)((char *)r->rings + p.sq_off.dropped);
    r->cq_khead        = (uint32_t *)((char *)r->rings + p.cq_off.head);
    r->cq_ktail        = (uint32_t *)((char *)r->rings + p.cq_off.tail);
    r->cq_koverflow    = (uint32_t *)((char *)r->rings + p.cq_off.overflow);
    r->cqes            = (struct io_uring_cqe *)((char *)r->rings + p.cq_off.cqes);
    r->sq_mask         = *(uint32_t *)((char *)r->rings + p.sq_off.ring_mask);
    r->cq_mask         = *(uint32_t *)((char *)r->rings + p.cq_off.ring_mask);

    int feat = require_features(r->features);
    if (feat != 0) {
        ring_context_shutdown(r);
        return feat;
    }

    struct io_uring_clock_register clk = { .clockid = CLOCK_MONOTONIC };
    rc = ring_register(r, IORING_REGISTER_CLOCK, (long)&clk, 0);
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
    rc = ring_register(r, IORING_REGISTER_MEM_REGION, (long)&mreg, 1);
    if (rc < 0) {
        fprintf(stderr, "[Ring Error] IORING_REGISTER_MEM_REGION failed: %s\n", strerror((int)-rc));
        ring_context_shutdown(r);
        return (int)rc;
    }
    r->wait_reg_on = true;

    if (with_pbuf) {
        int prc = ring_pbuf_register(r);
        if (prc < 0) {
            fprintf(stderr, "[Ring Error] IORING_REGISTER_PBUF_RING failed: %s\n", strerror(-prc));
            ring_context_shutdown(r);
            return prc;
        }
        prc = ring_sim_pbuf_register(r);
        if (prc < 0) {
            fprintf(stderr, "[Ring Error] sim provided buffers failed: %s\n", strerror(-prc));
            ring_context_shutdown(r);
            return prc;
        }
    }

    int frc = ring_fixed_register(r);
    if (frc < 0) {
        fprintf(stderr, "[Ring Error] IORING_REGISTER_BUFFERS failed: %s\n", strerror(-frc));
        ring_context_shutdown(r);
        return frc;
    }

    rc = ring_register(r, IORING_REGISTER_ENABLE_RINGS, 0, 0);
    if (rc < 0) {
        fprintf(stderr, "[Ring Error] IORING_REGISTER_ENABLE_RINGS failed: %s\n", strerror((int)-rc));
        ring_context_shutdown(r);
        return (int)rc;
    }

    int files = ring_register_files(r);
    if (files < 0) {
        fprintf(stderr, "[Ring Error] IORING_REGISTER_FILES2 failed: %s\n", strerror(-files));
        ring_context_shutdown(r);
        return files;
    }

    r->wait_flags = IORING_ENTER_NO_IOWAIT;
    r->min_timeout_on = true;
    r->is_initialized = true;
    printf("[Ring] Initialized '%s' (registered index %d, SQ=%u, CQ=%u, PBUF=%ux%uB, SIM=%ux%uB, FixedBuf=%u, FixedFiles=%u, NO_MMAP+REGISTERED_FD_ONLY+DEFER_TASKRUN)\n",
           r->name,
           r->enter_fd,
           r->sq_entries,
           r->cq_entries,
           with_pbuf ? ENG_PBUF_ENTRIES : 0u,
           ENG_PBUF_LEN,
           with_pbuf ? ENG_SIM_PBUF_ENTRIES : 0u,
           ENG_SIM_PBUF_LEN,
           ENG_FBUF_COUNT,
           ENG_FIXED_FILES);
    return 0;
}
