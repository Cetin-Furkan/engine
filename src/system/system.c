#include "system/internal.h"
#include "util/fmt.h"
#include <string.h>
#include <sys/utsname.h>

static SystemCapabilities s_sys_caps = {0};

int system_init(void) {
    s_sys_caps = (SystemCapabilities){0};
    struct utsname uts = {0};
    if (uname(&uts) != 0) {
        log_err_raw("[System Error] uname syscall failed\n");
        return 1;
    }
    memcpy(s_sys_caps.sysname, uts.sysname, sizeof(s_sys_caps.sysname) - 1);
    memcpy(s_sys_caps.release, uts.release, sizeof(s_sys_caps.release) - 1);

    system_probe_cpu(&s_sys_caps.cpu);

    int rc = system_probe_memory_and_limits(&s_sys_caps.mem);
    if (rc < 0) {
        log_err_raw("[System Error] getrlimit RLIMIT_MEMLOCK failed\n");
        return 1;
    }

    rc = system_probe_uring_kernel(&s_sys_caps.uring);
    if (rc < 0) {
        char buf[192];
        FmtBuf fb = fmt_init(buf, sizeof(buf));
        fmt_str(&fb, "[System Error] io_uring kernel query failed: ");
        fmt_str(&fb, strerror(-rc));
        fmt_putc(&fb, '\n');
        fmt_flush_stderr(&fb);
        return 1;
    }

    char out[512];
    FmtBuf fb = fmt_init(out, sizeof(out));
    fmt_str(&fb, "[System] Kernel: ");
    fmt_str(&fb, s_sys_caps.sysname);
    fmt_putc(&fb, ' ');
    fmt_str(&fb, s_sys_caps.release);
    fmt_str(&fb, " | MEMLOCK: ");
    fmt_u64(&fb, s_sys_caps.mem.memlock_bytes / (1024ull * 1024ull));
    fmt_str(&fb, s_sys_caps.mem.memlock_capped ? " MiB (kernel cap, wanted 64) | io_uring opcodes: "
                                               : " MiB | io_uring opcodes: ");
    fmt_u64(&fb, s_sys_caps.uring.nr_request_opcodes);
    fmt_str(&fb, " | NO_IOWAIT: ");
    fmt_str(&fb, s_sys_caps.uring.has_no_iowait ? "yes" : "no");
    fmt_str(&fb, " | BUNDLE: ");
    fmt_str(&fb, s_sys_caps.uring.has_recvsend_bundle ? "yes" : "no");
    fmt_str(&fb, " | MIN_TIMEOUT: ");
    fmt_str(&fb, s_sys_caps.uring.has_min_timeout ? "yes\n" : "no\n");
    fmt_str(&fb, "[System] CPU: ");
    fmt_str(&fb, s_sys_caps.cpu.brand);
    fmt_str(&fb, " (");
    fmt_u64(&fb, s_sys_caps.cpu.logical_cores);
    fmt_str(&fb, " threads, line=");
    fmt_u64(&fb, s_sys_caps.cpu.cache_line_bytes);
    fmt_str(&fb, "B, L1d=");
    fmt_u64(&fb, s_sys_caps.cpu.l1d_kb);
    fmt_str(&fb, "K, L2=");
    fmt_u64(&fb, s_sys_caps.cpu.l2_kb);
    fmt_str(&fb, "K, L3=");
    fmt_u64(&fb, s_sys_caps.cpu.l3_kb / 1024u);
    fmt_str(&fb, "M, AVX2=");
    fmt_str(&fb, s_sys_caps.cpu.has_avx2 ? "yes" : "no");
    fmt_str(&fb, ", FMA=");
    fmt_str(&fb, s_sys_caps.cpu.has_fma ? "yes" : "no");
    fmt_str(&fb, ", BMI2=");
    fmt_str(&fb, s_sys_caps.cpu.has_bmi2 ? "yes" : "no");
    fmt_str(&fb, ") | RAM: ");
    fmt_u64(&fb, s_sys_caps.mem.total_ram_bytes / (1024ull * 1024ull));
    fmt_str(&fb, " MiB | VMA-Name: ");
    fmt_str(&fb, s_sys_caps.mem.has_vma_anon_name ? "yes" : "no");
    fmt_str(&fb, " | MADV_POPULATE: ");
    fmt_str(&fb, s_sys_caps.mem.has_madv_populate ? "yes" : "no");
    fmt_str(&fb, " | TimerSlack: ");
    fmt_u64(&fb, s_sys_caps.mem.timerslack_ns);
    fmt_str(&fb, "ns\n");
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
