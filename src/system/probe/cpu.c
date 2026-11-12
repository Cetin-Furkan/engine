#include "system/internal.h"
#include <cpuid.h>
#include <sched.h>
#include <stdbit.h>
#include <string.h>
#include <unistd.h>

static void trim_brand_spaces(char *s) {
    char *src = s;
    while (*src == ' ') {
        src++;
    }
    if (src != s) {
        memmove(s, src, strlen(src) + 1u);
    }
    size_t n = strlen(s);
    while (n > 0u && s[n - 1u] == ' ') {
        s[--n] = '\0';
    }
}

static uint32_t probe_affinity_cores(void) {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        int count = CPU_COUNT(&set);
        if (count > 0) {
            return (uint32_t)count;
        }
    }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (uint32_t)n : 1u;
}

static uint64_t read_xcr0(void) {
    uint32_t eax = 0u, edx = 0u;
    __asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(eax), "=d"(edx) : "c"(0u));
    return ((uint64_t)edx << 32u) | (uint64_t)eax;
}

static void enumerate_deterministic_leaf(uint32_t leaf, SystemCpuInfo *out) {
    for (uint32_t sub = 0u; sub < 8u; ++sub) {
        unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
        __cpuid_count(leaf, sub, eax, ebx, ecx, edx);
        uint32_t cache_type = eax & 0x1Fu;
        if (cache_type == 0u) {
            break;
        }
        uint32_t level      = (eax >> 5) & 0x7u;
        uint32_t line_size  = (ebx & 0xFFFu) + 1u;
        uint32_t partitions = ((ebx >> 12) & 0x3FFu) + 1u;
        uint32_t ways       = ((ebx >> 22) & 0x3FFu) + 1u;
        uint32_t sets       = ecx + 1u;
        uint64_t bytes      = (uint64_t)line_size * partitions * ways * sets;
        uint32_t kb         = (uint32_t)(bytes / 1024ull);
        if (line_size >= 32u && line_size <= 256u) {
            out->cache_line_bytes = line_size;
        }
        if (level == 1u && (cache_type == 1u || cache_type == 3u)) {
            out->l1d_kb = kb;
        } else if (level == 2u) {
            out->l2_kb = kb;
        } else if (level == 3u && kb > out->l3_kb) {
            out->l3_kb = kb;
        }
    }
}

static void probe_deterministic_caches(uint32_t max_leaf, uint32_t max_ext, SystemCpuInfo *out) {
    if (max_leaf >= 4u) {
        enumerate_deterministic_leaf(4u, out);
    }
    // AMD Zen deterministic cache topology leaf (CPUID Fn8000_001D)
    if ((out->l1d_kb == 0u || out->l3_kb == 0u) && max_ext >= 0x8000001Du) {
        enumerate_deterministic_leaf(0x8000001Du, out);
    }
    if (out->l1d_kb == 0u && max_ext >= 0x80000006u) {
        unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
        __cpuid(0x80000005u, eax, ebx, ecx, edx);
        out->l1d_kb = (ecx >> 24) & 0xFFu;
        if (out->cache_line_bytes == 0u) {
            out->cache_line_bytes = ecx & 0xFFu;
        }
        __cpuid(0x80000006u, eax, ebx, ecx, edx);
        out->l2_kb = (ecx >> 16) & 0xFFFFu;
        if (out->l3_kb == 0u) {
            out->l3_kb = ((edx >> 18) & 0x3FFFu) * 512u;
        }
    }
    if (out->cache_line_bytes == 0u || stdc_count_ones_ui(out->cache_line_bytes) != 1u) {
        out->cache_line_bytes = 64u;
    }
}

void system_probe_cpu(SystemCpuInfo *out) {
    if (out == nullptr) {
        return;
    }
    *out = (SystemCpuInfo){
        .logical_cores    = probe_affinity_cores(),
        .cache_line_bytes = 64u,
    };

    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid(0, &eax, &ebx, &ecx, &edx)) {
        memcpy(out->vendor, "x86_64", 7);
        memcpy(out->brand, "Generic x86-64 Processor", 25);
        return;
    }
    uint32_t max_leaf = eax;
    memcpy(out->vendor + 0, &ebx, 4);
    memcpy(out->vendor + 4, &edx, 4);
    memcpy(out->vendor + 8, &ecx, 4);
    out->vendor[12] = '\0';

    if (max_leaf >= 1u) {
        __cpuid(1, eax, ebx, ecx, edx);
        uint32_t clflush_line = ((ebx >> 8) & 0xFFu) * 8u;
        if (clflush_line >= 32u && clflush_line <= 256u) {
            out->cache_line_bytes = clflush_line;
        }
        const bool osxsave   = (ecx & (1u << 27)) != 0u;
        const uint64_t xcr0  = osxsave ? read_xcr0() : 0ull;
        const bool os_avx    = osxsave && ((xcr0 & 0x6ull) == 0x6ull);
        const bool os_avx512 = os_avx && ((xcr0 & 0xe0ull) == 0xe0ull);
        out->has_fma  = os_avx && ((ecx & (1u << 12)) != 0u);
        out->has_f16c = os_avx && ((ecx & (1u << 29)) != 0u);

        if (max_leaf >= 7u) {
            unsigned int eax7 = 0, ebx7 = 0, ecx7 = 0, edx7 = 0;
            __cpuid_count(7, 0, eax7, ebx7, ecx7, edx7);
            out->has_avx2    = os_avx && ((ebx7 & (1u << 5)) != 0u);
            out->has_bmi2    = (ebx7 & (1u << 8)) != 0u;
            out->has_avx512f = os_avx512 && ((ebx7 & (1u << 16)) != 0u);
        }
    }

    unsigned int max_ext = 0;
    if (__get_cpuid(0x80000000u, &max_ext, &ebx, &ecx, &edx) && max_ext >= 0x80000004u) {
        uint32_t words[12] = {0};
        for (uint32_t i = 0u; i < 3u; ++i) {
            __cpuid(0x80000002u + i,
                    words[i * 4u + 0u],
                    words[i * 4u + 1u],
                    words[i * 4u + 2u],
                    words[i * 4u + 3u]);
        }
        memcpy(out->brand, words, 48u);
        out->brand[48] = '\0';
        trim_brand_spaces(out->brand);
    }
    if (out->brand[0] == '\0') {
        memcpy(out->brand, out->vendor, strlen(out->vendor) + 1u);
    }

    probe_deterministic_caches(max_leaf, max_ext, out);
}
