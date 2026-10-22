#include "system/system.h"
#include "util/fmt.h"
#include <errno.h>
#include <linux/io_uring.h>
#include <linux/io_uring/query.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

static constexpr uint32_t k_required_ring_setup_mask =
    IORING_SETUP_SINGLE_ISSUER |
    IORING_SETUP_DEFER_TASKRUN |
    IORING_SETUP_TASKRUN_FLAG  |
    IORING_SETUP_NO_SQARRAY    |
    IORING_SETUP_CQSIZE        |
    IORING_SETUP_CLAMP         |
    IORING_SETUP_SUBMIT_ALL    |
    IORING_SETUP_NO_MMAP       |
    IORING_SETUP_R_DISABLED;

static SystemCapabilities s_sys_caps = {0};

static long sys_uring_register_raw(int fd, unsigned int opcode, const void *arg, unsigned int nr_args) {
    long rc = syscall(__NR_io_uring_register, fd, opcode, arg, nr_args, 0L, 0L);
    return (rc < 0) ? -errno : rc;
}

static int raise_memlock_64m(uint64_t *out_cur) {
    struct rlimit rl = {0};
    constexpr rlim_t want = 64ull * 1024ull * 1024ull;
    if (getrlimit(RLIMIT_MEMLOCK, &rl) != 0) {
        return -errno;
    }

    rlim_t target = want;
    if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < want) {
        struct rlimit raised = {
            .rlim_cur = want,
            .rlim_max = want,
        };
        if (setrlimit(RLIMIT_MEMLOCK, &raised) == 0) {
            rl = raised;
        } else {
            target = rl.rlim_max;
        }
    }
    if (rl.rlim_cur < target) {
        rl.rlim_cur = target;
        if (setrlimit(RLIMIT_MEMLOCK, &rl) != 0) {
            return -errno;
        }
    }
    if (getrlimit(RLIMIT_MEMLOCK, &rl) != 0) {
        return -errno;
    }

    *out_cur = (uint64_t)rl.rlim_cur;
    return (rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur >= want) ? 0 : 1;
}

static int probe_io_uring_kernel(SystemCapabilities *caps) {
    struct io_uring_query_opcode opcodes = {0};
    struct io_uring_query_hdr hdr = {
        .next_entry = 0,
        .query_data = (uint64_t)(uintptr_t)&opcodes,
        .query_op   = IO_URING_QUERY_OPCODES,
        .size       = sizeof(opcodes),
    };
    long rc = sys_uring_register_raw(-1, IORING_REGISTER_QUERY, &hdr, 0);
    if (rc < 0) {
        return (int)rc;
    }
    if (hdr.result < 0) {
        return hdr.result;
    }

    if ((opcodes.ring_setup_flags & k_required_ring_setup_mask) != k_required_ring_setup_mask) {
        return -ENOTSUP;
    }
    if ((opcodes.feature_flags & IORING_FEAT_NODROP) == 0) {
        return -ENOTSUP;
    }
    if (opcodes.nr_request_opcodes <= IORING_OP_SOCKET ||
        opcodes.nr_register_opcodes <= IORING_REGISTER_MEM_REGION) {
        return -ENOTSUP;
    }

    caps->uring_setup_flags         = opcodes.ring_setup_flags;
    caps->uring_feature_flags       = opcodes.feature_flags;
    caps->uring_nr_request_opcodes  = opcodes.nr_request_opcodes;
    caps->uring_nr_register_opcodes = opcodes.nr_register_opcodes;
    caps->has_no_iowait             = (opcodes.feature_flags & IORING_FEAT_NO_IOWAIT) != 0;
    return 0;
}

int system_init(void) {
    s_sys_caps = (SystemCapabilities){0};
    struct utsname uts = {0};
    if (uname(&uts) != 0) {
        log_err_raw("[System Error] uname syscall failed\n");
        return 1;
    }
    memcpy(s_sys_caps.sysname, uts.sysname, sizeof(s_sys_caps.sysname) - 1);
    memcpy(s_sys_caps.release, uts.release, sizeof(s_sys_caps.release) - 1);

    int rc = raise_memlock_64m(&s_sys_caps.memlock_bytes);
    if (rc < 0) {
        log_err_raw("[System Error] getrlimit RLIMIT_MEMLOCK failed\n");
        return 1;
    }
    const bool memlock_capped = (rc > 0);

    rc = probe_io_uring_kernel(&s_sys_caps);
    if (rc < 0) {
        char buf[192];
        FmtBuf fb = fmt_init(buf, sizeof(buf));
        fmt_str(&fb, "[System Error] io_uring kernel query failed: ");
        fmt_str(&fb, strerror(-rc));
        fmt_putc(&fb, '\n');
        fmt_flush_stderr(&fb);
        return 1;
    }

    char out[256];
    FmtBuf fb = fmt_init(out, sizeof(out));
    fmt_str(&fb, "[System] Kernel: ");
    fmt_str(&fb, s_sys_caps.sysname);
    fmt_putc(&fb, ' ');
    fmt_str(&fb, s_sys_caps.release);
    fmt_str(&fb, " | MEMLOCK: ");
    fmt_u64(&fb, s_sys_caps.memlock_bytes / (1024ull * 1024ull));
    fmt_str(&fb, memlock_capped ? " MiB (kernel cap, wanted 64) | io_uring opcodes: "
                                : " MiB | io_uring opcodes: ");
    fmt_u64(&fb, s_sys_caps.uring_nr_request_opcodes);
    fmt_str(&fb, " | NO_IOWAIT: ");
    fmt_str(&fb, s_sys_caps.has_no_iowait ? "yes\n" : "no\n");
    fmt_flush_stdout(&fb);

    s_sys_caps.is_initialized = true;
    return 0;
}

void system_shutdown(void) {
    if (s_sys_caps.is_initialized) {
        log_info_raw("[System] Shutdown complete.\n");
        s_sys_caps.is_initialized = false;
    }
}

const SystemCapabilities *system_get_capabilities(void) {
    return &s_sys_caps;
}