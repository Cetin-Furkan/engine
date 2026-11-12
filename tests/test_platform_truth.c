#include "lighting/lighting.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// End-to-end 10/10 platform & documentation truth gate:
// 1. Clean-build `make release` into an isolated BUILD_DIR and compare
//    compiled binary sizes (`bin/engine`, `bin/minimal`) and SPIR-V module
//    counts against README.md.
// 2. Call `lighting_compute_hrc_rays` (shared with `ensure_gi_slab`) at
//    1280x720 (and 1280x688) and compare UMA + Discrete totals against README.md.
// 3. Enforce zero "CHERI" occurrences across docs, headers, and C sources.
// 4. Enforce symbol & shared-library purity on `bin/minimal` (zero demo,
//    world, terrain, backend, scene, physics, lighting, FreeType, HarfBuzz,
//    fontconfig, or libpng symbols/libraries).
// 5. Verify `doc/IDLE_VERIFICATION_LOG.txt` and, when a live Wayland socket is
//    reachable, boot BOTH `bin/minimal` and `bin/engine --world=0` to
//    `[Engine] Loop idle.`, hold 150 ms (~19 sim quanta), SIGTERM, and fail if
//    `sim_timer != 0` or any GPU submit happens after `Loop idle`.

static int s_failures = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_failures++;                                                   \
        }                                                                   \
    } while (0)

static char *slurp_text_file(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        return nullptr;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return nullptr;
    }
    long sz = ftell(fp);
    if (sz < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return nullptr;
    }
    char *buf = malloc((size_t)sz + 1u);
    if (buf == nullptr) {
        fclose(fp);
        return nullptr;
    }
    size_t got = sz > 0 ? fread(buf, 1u, (size_t)sz, fp) : 0u;
    fclose(fp);
    buf[got] = '\0';
    if (out_len != nullptr) {
        *out_len = got;
    }
    return buf;
}

static void format_u64_commas(uint64_t val, char *out, size_t cap) {
    char raw[32];
    int len = snprintf(raw, sizeof(raw), "%llu", (unsigned long long)val);
    if (len <= 0 || cap == 0u) {
        if (cap > 0u) out[0] = '\0';
        return;
    }
    size_t pos = 0u;
    for (int i = 0; i < len && pos + 1u < cap; ++i) {
        if (i > 0 && ((len - i) % 3) == 0) {
            out[pos++] = ',';
            if (pos + 1u >= cap) break;
        }
        out[pos++] = raw[i];
    }
    out[pos] = '\0';
}

static uint32_t count_spv_in_dir(const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (d == nullptr) {
        return 0u;
    }
    uint32_t count = 0u;
    struct dirent *ent = nullptr;
    while ((ent = readdir(d)) != nullptr) {
        size_t n = strlen(ent->d_name);
        if (n > 4u && strcmp(ent->d_name + n - 4u, ".spv") == 0) {
            count++;
        }
    }
    closedir(d);
    return count;
}

static void test_gi_rays_match_readme(const char *readme) {
    uint32_t gi_w = 0, gi_h = 0, p_w = 0, p_h = 0;
    uint32_t offsets[K_LIGHTING_MAX_CASCADES] = {0};

    uint32_t uma_720 = lighting_compute_hrc_rays(
        1280u, 720u, LIGHTING_ORACLE_ANALYTICAL_UMA, &gi_w, &gi_h, &p_w, &p_h, offsets);
    CHECK(uma_720 == 107520u);
    CHECK(gi_w == 640u && gi_h == 360u && p_w == 160u && p_h == 90u);

    uint32_t disc_720 = lighting_compute_hrc_rays(
        1280u, 720u, LIGHTING_ORACLE_DISCRETE_HIGH, &gi_w, &gi_h, &p_w, &p_h, offsets);
    CHECK(disc_720 == 431360u);
    CHECK(gi_w == 960u && gi_h == 540u && p_w == 320u && p_h == 180u);

    uint32_t uma_688 = lighting_compute_hrc_rays(
        1280u, 688u, LIGHTING_ORACLE_ANALYTICAL_UMA, nullptr, nullptr, nullptr, nullptr, nullptr);
    uint32_t disc_688 = lighting_compute_hrc_rays(
        1280u, 688u, LIGHTING_ORACLE_DISCRETE_HIGH, nullptr, nullptr, nullptr, nullptr, nullptr);
    CHECK(uma_688 == 102400u);
    CHECK(disc_688 == 412160u);

    char uma_str[32], disc_str[32], uma688_str[32], disc688_str[32];
    format_u64_commas(uma_720, uma_str, sizeof(uma_str));
    format_u64_commas(disc_720, disc_str, sizeof(disc_str));
    format_u64_commas(uma_688, uma688_str, sizeof(uma688_str));
    format_u64_commas(disc_688, disc688_str, sizeof(disc688_str));

    CHECK(strstr(readme, uma_str) != nullptr);
    CHECK(strstr(readme, disc_str) != nullptr);
    CHECK(strstr(readme, uma688_str) != nullptr);
    CHECK(strstr(readme, disc688_str) != nullptr);
}

static bool contains_word_ci(const char *buf, size_t len, const char *needle_lower) {
    size_t nlen = strlen(needle_lower);
    if (nlen == 0u || len < nlen) {
        return false;
    }
    for (size_t i = 0u; i + nlen <= len; ++i) {
        bool match = true;
        for (size_t j = 0u; j < nlen; ++j) {
            if (tolower((unsigned char)buf[i + j]) != (unsigned char)needle_lower[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

static void scan_path_no_cheri(const char *path) {
    struct stat st = {0};
    if (stat(path, &st) != 0) {
        return;
    }
    if (S_ISREG(st.st_mode)) {
        size_t len = 0u;
        char *buf = slurp_text_file(path, &len);
        if (buf != nullptr) {
            if (contains_word_ci(buf, len, "cheri")) {
                fprintf(stderr, "FAIL: forbidden word 'CHERI' found in %s\n", path);
                s_failures++;
            }
            free(buf);
        }
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
        return;
    }
    DIR *d = opendir(path);
    if (d == nullptr) {
        return;
    }
    struct dirent *ent = nullptr;
    while ((ent = readdir(d)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        scan_path_no_cheri(child);
    }
    closedir(d);
}

static void test_zero_cheri_across_tree(void) {
    scan_path_no_cheri("README.md");
    scan_path_no_cheri("pointers.md");
    scan_path_no_cheri("TERRAIN_BACKEND_ARCHITECTURE.md");
    scan_path_no_cheri("doc");
    scan_path_no_cheri("include");
    scan_path_no_cheri("src");
    scan_path_no_cheri("examples/minimal");
}

static void test_minimal_symbol_and_library_purity(const char *minimal_bin) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "nm '%s' 2>&1", minimal_bin);
    FILE *fp = popen(cmd, "r");
    CHECK(fp != nullptr);
    if (fp != nullptr) {
        static const char *const k_forbidden_prefixes[] = {
            "demo_",
            "world_",
            "terrain_",
            "backend_",
            "physics_",
            "avbd_",
            "lighting_",
            "scene_init",
            "scene_shutdown",
            "scene_upload",
            "scene_update",
            "scene_prepare",
            "scene_draw",
            "meshlet_",
            "FT_",
            "hb_",
            "FcConfig",
            "png_",
        };
        uint32_t sym_lines = 0u;
        char line[512];
        while (fgets(line, sizeof(line), fp) != nullptr) {
            sym_lines++;
            // Symbol name is the last whitespace-delimited token on each nm line
            size_t len = strlen(line);
            while (len > 0u && (line[len - 1u] == '\n' || line[len - 1u] == '\r')) {
                line[--len] = '\0';
            }
            const char *sym = strrchr(line, ' ');
            sym = (sym != nullptr) ? (sym + 1) : line;
            for (size_t i = 0u; i < sizeof(k_forbidden_prefixes) / sizeof(k_forbidden_prefixes[0]); ++i) {
                const char *pfx = k_forbidden_prefixes[i];
                if (strncmp(sym, pfx, strlen(pfx)) == 0) {
                    fprintf(stderr, "FAIL: forbidden symbol '%s' linked into %s\n", sym, minimal_bin);
                    s_failures++;
                }
            }
        }
        int status = pclose(fp);
        CHECK(status == 0);
        CHECK(sym_lines >= 100u); // Ensure unstripped symbol table was actually inspected
    }

    snprintf(cmd, sizeof(cmd), "readelf -d '%s' 2>&1", minimal_bin);
    fp = popen(cmd, "r");
    CHECK(fp != nullptr);
    if (fp != nullptr) {
        bool saw_vulkan = false;
        bool saw_xkb    = false;
        char line[512];
        while (fgets(line, sizeof(line), fp) != nullptr) {
            if (strstr(line, "(NEEDED)") == nullptr) {
                continue;
            }
            if (strstr(line, "libvulkan.so") != nullptr) saw_vulkan = true;
            if (strstr(line, "libxkbcommon.so") != nullptr) saw_xkb = true;
            CHECK(strstr(line, "freetype") == nullptr);
            CHECK(strstr(line, "harfbuzz") == nullptr);
            CHECK(strstr(line, "fontconfig") == nullptr);
            CHECK(strstr(line, "libpng") == nullptr);
        }
        int status = pclose(fp);
        CHECK(status == 0);
        CHECK(saw_vulkan);
        CHECK(saw_xkb);
    }
}

static bool parse_idle_line(const char *line, uint64_t *out_submits, uint32_t *out_timer) {
    const char *p = strstr(line, "stalls, ");
    if (p == nullptr) {
        return false;
    }
    unsigned long long subs = 0ull;
    unsigned int tmr = 99u;
    if (sscanf(p, "stalls, %llu submits, sim_timer=%u", &subs, &tmr) != 2) {
        return false;
    }
    *out_submits = (uint64_t)subs;
    *out_timer   = (uint32_t)tmr;
    return true;
}

static bool parse_idle_ended_line(const char *line, uint64_t *out_submits, uint32_t *out_timer) {
    const char *p = strstr(line, "submits=");
    if (p == nullptr) {
        return false;
    }
    unsigned long long subs = 0ull;
    unsigned int tmr = 99u;
    if (sscanf(p, "submits=%llu, sim_timer=%u", &subs, &tmr) != 2) {
        return false;
    }
    *out_submits = (uint64_t)subs;
    *out_timer   = (uint32_t)tmr;
    return true;
}

static void test_committed_idle_log(void) {
    size_t len = 0u;
    char *log_txt = slurp_text_file("doc/IDLE_VERIFICATION_LOG.txt", &len);
    CHECK(log_txt != nullptr && len > 200u);
    if (log_txt == nullptr) {
        return;
    }
    CHECK(strstr(log_txt, "Intel(R) Iris(R) Xe Graphics") != nullptr);
    CHECK(strstr(log_txt, "=== build/release/bin/minimal ===") != nullptr);
    CHECK(strstr(log_txt, "=== build/release/bin/engine --world=0 ===") != nullptr);

    uint32_t idle_sections  = 0u;
    uint32_t ended_sections = 0u;
    uint64_t last_idle_submits = 0u;
    char *save = nullptr;
    for (char *ln = strtok_r(log_txt, "\n", &save); ln != nullptr; ln = strtok_r(nullptr, "\n", &save)) {
        if (strstr(ln, "[Engine] Loop idle.") != nullptr) {
            uint64_t subs = 0u;
            uint32_t tmr  = 1u;
            CHECK(parse_idle_line(ln, &subs, &tmr));
            CHECK(subs >= 1u);
            CHECK(tmr == 0u);
            last_idle_submits = subs;
            idle_sections++;
        } else if (strstr(ln, "[Engine] Idle ended") != nullptr) {
            uint64_t subs = 0u;
            uint32_t tmr  = 1u;
            CHECK(parse_idle_ended_line(ln, &subs, &tmr));
            CHECK(tmr == 0u);
            CHECK(subs == last_idle_submits);
            ended_sections++;
        }
    }
    CHECK(idle_sections == 2u);
    CHECK(ended_sections == 2u);
    free(log_txt);
}

static bool wayland_socket_reachable(void) {
    const char *disp = getenv("WAYLAND_DISPLAY");
    const char *xdg  = getenv("XDG_RUNTIME_DIR");
    if (disp == nullptr || disp[0] == '\0') {
        return false;
    }
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (disp[0] == '/') {
        if (strlen(disp) >= sizeof(addr.sun_path)) return false;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", disp);
    } else {
        if (xdg == nullptr || xdg[0] == '\0') return false;
        if (strlen(xdg) + 1u + strlen(disp) >= sizeof(addr.sun_path)) return false;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/%s", xdg, disp);
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    bool ok = (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) == 0);
    close(fd);
    return ok;
}

static uint64_t mono_ms_now(void) {
    struct timespec ts = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000ull);
}

static void verify_live_boot_to_quiescent_idle(const char *bin_path, const char *opt_arg) {
    int pfd[2] = { -1, -1 };
    CHECK(pipe2(pfd, O_CLOEXEC) == 0);
    if (pfd[0] < 0) {
        return;
    }

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return;
    }
    if (pid == 0) {
        (void)dup2(pfd[1], STDOUT_FILENO);
        (void)dup2(pfd[1], STDERR_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        setenv("LC_ALL", "C", 1);
        if (opt_arg != nullptr) {
            execl(bin_path, bin_path, opt_arg, (char *)nullptr);
        } else {
            execl(bin_path, bin_path, (char *)nullptr);
        }
        _exit(127);
    }
    close(pfd[1]);

    int flags = fcntl(pfd[0], F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);
    }

    char accum[65536];
    size_t used = 0u;
    size_t scan_off = 0u;
    bool saw_loop_idle   = false;
    bool saw_idle_ended  = false;
    bool sent_sigterm    = false;
    uint64_t idle_at_ms  = 0u;
    uint64_t idle_subs   = 0u;
    uint32_t idle_timer  = 1u;
    uint64_t end_subs    = 0u;
    uint32_t end_timer   = 1u;
    uint64_t deadline_ms = mono_ms_now() + 10000ull;

    while (mono_ms_now() < deadline_ms) {
        // Hold 150 ms (~19 x 8 ms quanta) after Loop idle before sending SIGTERM
        // so any armed sim timer or stray post-idle submit is caught red-handed!
        if (saw_loop_idle && !sent_sigterm && (mono_ms_now() - idle_at_ms) >= 150ull) {
            (void)kill(pid, SIGTERM);
            sent_sigterm = true;
        }

        struct pollfd p = { .fd = pfd[0], .events = POLLIN | POLLHUP };
        int prc = poll(&p, 1u, 20);
        if (prc > 0 && (p.revents & (POLLIN | POLLHUP)) != 0) {
            ssize_t n = read(pfd[0], accum + used, sizeof(accum) - 1u - used);
            if (n > 0) {
                used += (size_t)n;
                accum[used] = '\0';
                for (;;) {
                    char *nl = strchr(accum + scan_off, '\n');
                    if (nl == nullptr) {
                        break;
                    }
                    *nl = '\0';
                    const char *line = accum + scan_off;
                    if (!saw_loop_idle && strstr(line, "[Engine] Loop idle.") != nullptr) {
                        saw_loop_idle = parse_idle_line(line, &idle_subs, &idle_timer);
                        idle_at_ms    = mono_ms_now();
                    } else if (strstr(line, "[Engine] Idle ended") != nullptr) {
                        saw_idle_ended = parse_idle_ended_line(line, &end_subs, &end_timer);
                    }
                    scan_off = (size_t)(nl - accum) + 1u;
                }
            } else if (n == 0) {
                break;
            }
        }
    }
    close(pfd[0]);

    if (!sent_sigterm) {
        (void)kill(pid, SIGKILL);
    }
    int wstatus = 0;
    CHECK(waitpid(pid, &wstatus, 0) == pid);
    CHECK(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);
    CHECK(saw_loop_idle);
    CHECK(idle_subs >= 1u);
    CHECK(idle_timer == 0u);
    CHECK(saw_idle_ended);
    CHECK(end_timer == 0u);
    CHECK(end_subs == idle_subs); // Zero submits after [Engine] Loop idle.!
}

int main(void) {
    size_t readme_len = 0u;
    char *readme = slurp_text_file("README.md", &readme_len);
    CHECK(readme != nullptr && readme_len > 500u);
    if (readme == nullptr) {
        return 1;
    }

    // 1. GI ray-count verification (1280x720 + 1280x688 UMA & Discrete vs README)
    test_gi_rays_match_readme(readme);

    // 2. Zero "CHERI" gate across docs, headers, and sources
    test_zero_cheri_across_tree();

    // 3. Clean-tree `make release` into isolated BUILD_DIR and compare sizes & SPIR-V counts
    const char *clean_dir = "build/truth_clean_release";
    int rm_rc = system("rm -rf build/truth_clean_release");
    CHECK(rm_rc == 0);

    int build_rc = system("env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL make --no-print-directory -j4 MODE=release BUILD_DIR=build/truth_clean_release all");
    CHECK(build_rc == 0);

    if (build_rc == 0) {
        uint32_t engine_spv  = count_spv_in_dir("build/truth_clean_release/gen");
        uint32_t minimal_spv = count_spv_in_dir("build/truth_clean_release/gen_minimal");
        CHECK(engine_spv == 42u);
        CHECK(minimal_spv == 2u);

        char spv_needle[64];
        snprintf(spv_needle, sizeof(spv_needle), "`%u` SPIR-V", engine_spv);
        CHECK(strstr(readme, spv_needle) != nullptr);

        struct stat st_eng = {0};
        struct stat st_min = {0};
        CHECK(stat("build/truth_clean_release/bin/engine", &st_eng) == 0);
        CHECK(stat("build/truth_clean_release/bin/minimal", &st_min) == 0);

        char eng_mib[32], min_mib[32];
        snprintf(eng_mib, sizeof(eng_mib), "`%.2f MiB`",
                 (double)st_eng.st_size / (1024.0 * 1024.0));
        snprintf(min_mib, sizeof(min_mib), "`%.2f MiB`",
                 (double)st_min.st_size / (1024.0 * 1024.0));
        CHECK(strstr(readme, eng_mib) != nullptr);
        CHECK(strstr(readme, min_mib) != nullptr);

        // 4. Symbol & dynamic library purity gate on bin/minimal
        test_minimal_symbol_and_library_purity("build/truth_clean_release/bin/minimal");

        // 5. Committed idle log + live Wayland boot-to-idle quiescence check
        test_committed_idle_log();
        if (wayland_socket_reachable()) {
            verify_live_boot_to_quiescent_idle("build/truth_clean_release/bin/minimal", nullptr);
            verify_live_boot_to_quiescent_idle("build/truth_clean_release/bin/engine", "--world=0");
        }
    }

    (void)clean_dir;
    (void)system("rm -rf build/truth_clean_release");
    free(readme);

    if (s_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_platform_truth: all checks passed\n");
    return 0;
}
