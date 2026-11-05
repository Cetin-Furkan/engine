#include "system/internal.h"
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#endif
#ifndef PR_SET_VMA_ANON_NAME
#define PR_SET_VMA_ANON_NAME 0
#endif
#ifndef MADV_POPULATE_WRITE
#define MADV_POPULATE_WRITE 23
#endif

static int raise_rlimit_target(int resource, rlim_t want, uint64_t *out_cur, bool *out_capped) {
    struct rlimit rl = {0};
    if (getrlimit(resource, &rl) != 0) {
        return -errno;
    }

    rlim_t target = want;
    if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < want) {
        struct rlimit raised = {
            .rlim_cur = want,
            .rlim_max = want,
        };
        if (setrlimit(resource, &raised) == 0) {
            rl = raised;
        } else {
            target = rl.rlim_max;
        }
    }
    if (rl.rlim_cur < target) {
        rl.rlim_cur = target;
        if (setrlimit(resource, &rl) != 0) {
            return -errno;
        }
    }
    if (getrlimit(resource, &rl) != 0) {
        return -errno;
    }

    if (out_cur != nullptr) {
        *out_cur = (uint64_t)rl.rlim_cur;
    }
    if (out_capped != nullptr) {
        *out_capped = (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < want);
    }
    return 0;
}

void system_vma_set_name(void *addr, size_t len, const char *name) {
    if (addr == nullptr || len == 0u || name == nullptr) {
        return;
    }
    (void)prctl(PR_SET_VMA, (unsigned long)PR_SET_VMA_ANON_NAME,
                (unsigned long)(uintptr_t)addr, (unsigned long)len,
                (unsigned long)(uintptr_t)name);
}

void system_vma_advise_arena(void *addr, size_t len) {
    if (addr == nullptr || len == 0u) {
        return;
    }
#ifdef MADV_HUGEPAGE
    (void)madvise(addr, len, MADV_HUGEPAGE);
#endif
    if (madvise(addr, len, MADV_POPULATE_WRITE) != 0) {
        memset(addr, 0, len);
    }
}

int system_probe_memory_and_limits(SystemMemoryInfo *out) {
    if (out == nullptr) {
        return -EINVAL;
    }
    *out = (SystemMemoryInfo){0};

    long page = sysconf(_SC_PAGESIZE);
    out->page_size = (page > 0) ? (uint64_t)page : 4096ull;

    struct sysinfo si = {0};
    if (sysinfo(&si) == 0) {
        uint64_t unit = si.mem_unit > 0u ? (uint64_t)si.mem_unit : 1ull;
        out->total_ram_bytes = (uint64_t)si.totalram * unit;
        out->avail_ram_bytes = (uint64_t)(si.freeram + si.bufferram) * unit;
    }

    constexpr rlim_t want_memlock = 64ull * 1024ull * 1024ull;
    int rc = raise_rlimit_target(RLIMIT_MEMLOCK, want_memlock, &out->memlock_bytes, &out->memlock_capped);
    if (rc < 0) {
        return rc;
    }

    constexpr rlim_t want_nofile = 4096ull;
    (void)raise_rlimit_target(RLIMIT_NOFILE, want_nofile, &out->nofile_limit, nullptr);

    // Tighten timer slack on the single-issuer io_uring thread to 1 ns so
    // 8 ms simulation ticks and frame watchdog timeouts do not suffer 50 us coalescing.
    if (prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL) == 0) {
        long slack = prctl(PR_GET_TIMERSLACK, 0UL, 0UL, 0UL, 0UL);
        out->timerslack_ns = (slack > 0) ? (uint64_t)slack : 1ull;
    }

    // Probe MADV_POPULATE_WRITE and PR_SET_VMA_ANON_NAME on a 1-page scratch mapping.
    void *probe_page = mmap(nullptr, (size_t)out->page_size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (probe_page != MAP_FAILED) {
        out->has_madv_populate = (madvise(probe_page, (size_t)out->page_size, MADV_POPULATE_WRITE) == 0);
        out->has_vma_anon_name = (prctl(PR_SET_VMA, (unsigned long)PR_SET_VMA_ANON_NAME,
                                        (unsigned long)(uintptr_t)probe_page,
                                        (unsigned long)out->page_size,
                                        (unsigned long)(uintptr_t)"engine:probe") == 0);
        (void)munmap(probe_page, (size_t)out->page_size);
    }

    return 0;
}
