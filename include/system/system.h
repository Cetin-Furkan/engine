#ifndef ENGINE_SYSTEM_H
#define ENGINE_SYSTEM_H

#include <stddef.h>
#include <stdint.h>

typedef struct SystemCpuInfo {
    char     vendor[16];
    char     brand[64];
    uint32_t logical_cores;
    uint32_t cache_line_bytes;
    uint32_t l1d_kb;
    uint32_t l2_kb;
    uint32_t l3_kb;
    bool     has_avx2;
    bool     has_fma;
    bool     has_bmi2;
    bool     has_f16c;
    bool     has_avx512f;
} SystemCpuInfo;

typedef struct SystemMemoryInfo {
    uint64_t total_ram_bytes;
    uint64_t avail_ram_bytes;
    uint64_t page_size;
    uint64_t memlock_bytes;
    uint64_t nofile_limit;
    uint64_t timerslack_ns;
    bool     memlock_capped;
    bool     has_vma_anon_name;
    bool     has_madv_populate;
} SystemMemoryInfo;

typedef struct SystemUringInfo {
    uint64_t setup_flags;
    uint64_t feature_flags;
    uint32_t nr_request_opcodes;
    uint32_t nr_register_opcodes;
    bool     has_no_iowait;
    bool     has_recvsend_bundle;
    bool     has_min_timeout;
    bool     has_cqe_skip;
    bool     has_ext_arg;
    bool     has_reg_reg_ring;
} SystemUringInfo;

typedef struct SystemCapabilities {
    char             sysname[65];
    char             release[65];
    SystemCpuInfo    cpu;
    SystemMemoryInfo mem;
    SystemUringInfo  uring;
    bool             is_initialized;
} SystemCapabilities;

[[nodiscard]] int system_init(void);
void system_shutdown(void);

// Applies MADV_HUGEPAGE and MADV_POPULATE_WRITE pre-faulting to an anonymous
// mmap arena and zeroes it without runtime page-fault jitter.
void system_vma_advise_arena(void *addr, size_t len);

// Names an anonymous VMA via prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME) so it
// is identifiable in /proc/self/maps and /proc/self/smaps.
void system_vma_set_name(void *addr, size_t len, const char *name);

#endif /* ENGINE_SYSTEM_H */
