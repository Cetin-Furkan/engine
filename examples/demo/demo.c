#include "chrome/gpu.h"
#include "backend/backend.h"
#include "engine/app.h"
#include "vulkan/bda.h"
#include "frame/camera.h"
#include "frame/cube.h"
#include "frame/gimbal.h"
#include "frame/grid.h"
#include "frame/hud.h"
#include "frame/material.h"
#include "frame/mesh.h"
#include "frame/pass.h"
#include "frame/plane.h"
#include "frame/policy.h"
#include "frame/plot.h"
#include "frame/shape.h"
#include "frame/sheet.h"
#include "frame/solid.h"
#include "frame/terrain.h"
#include "frame/text.h"
#include "frame/world.h"
#include "frame/frame.h"
#include "lighting/lighting.h"
#include "physics/physics.h"
#include "scene/scene.h"
#include "ring/ring.h"
#include "vulkan/core.h"
#include "input/pointer.h"
#include "world/world_int.h"
#include <linux/input-event-codes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// The hosted program. Boot draws the metric grid and no objects.
// opening.world places each cube line on z = 0. Clicks build faces.
// Keys 1-6 raise solids. M cycles the math plot. L reloads the CSV
// chart and the OBJ, and in 2D or Hybrid the sheet document too.
// Tab cycles 3D, 2D, and Hybrid. The engine knows none of these names;
// it only calls the EngineApp hooks below on its io_uring thread.

// ---------------------------------------------------------------- targets

// The engine copies EngineTargets by value (a resize installs a fresh
// slot's handles into the shown slot), so ownership cannot hang off the
// EngineTargets address. Entries are keyed by the depth image handle,
// which survives the copy. Poisoned entries (image zeroed on destroy)
// keep a stale handle from ever matching a recycled one.
enum : uint32_t { k_target_cap = 16 };

typedef struct TargetEntry {
    PassDepth    depth;
    PassCoverage coverage;
    uint32_t     width;
    uint32_t     height;
    uint32_t     generation;
    bool         used;
} TargetEntry;

static TargetEntry s_targets[k_target_cap];
static uint32_t    s_target_generation = 1;
static bool        s_prepare_live      = true;

static TargetEntry *target_find_by_depth(VkImage depth_image) {
    if (depth_image == VK_NULL_HANDLE) {
        return nullptr;
    }
    for (uint32_t i = 0; i < k_target_cap; ++i) {
        if (s_targets[i].used && s_targets[i].depth.image == depth_image) {
            return &s_targets[i];
        }
    }
    return nullptr;
}

static TargetEntry *target_find(const EngineTargets *targets) {
    if (targets == nullptr || targets->depth_image == VK_NULL_HANDLE) {
        return nullptr;
    }
    return target_find_by_depth(targets->depth_image);
}

static int demo_create_targets(VulkanContext *vk, EngineTargets *targets,
                               uint32_t width, uint32_t height) {
    if (vk == nullptr || targets == nullptr) {
        return -1;
    }
    TargetEntry *entry = target_find(targets);
    if (entry == nullptr) {
        for (uint32_t i = 0; i < k_target_cap; ++i) {
            if (!s_targets[i].used) {
                entry = &s_targets[i];
                break;
            }
        }
    }
    if (entry == nullptr) {
        fprintf(stderr, "[Demo] No free render-target slot\n");
        return -1;
    }
    if (pass_depth_create(vk, &entry->depth, width, height) != 0 ||
        pass_coverage_create(vk, &entry->coverage, width, height) != 0) {
        pass_depth_destroy(vk, &entry->depth);
        pass_coverage_destroy(vk, &entry->coverage);
        entry->used = false;
        *targets = (EngineTargets){0};
        return -1;
    }
    entry->width      = width;
    entry->height     = height;
    entry->generation = s_target_generation++;
    if (s_target_generation == 0) {
        s_target_generation = 1;
    }
    entry->used = true;
    targets->depth_image     = entry->depth.image;
    targets->depth_view      = entry->depth.view;
    targets->coverage_image  = entry->coverage.image;
    targets->coverage_view   = entry->coverage.view;
    targets->depth_format    = pass_depth_format();
    targets->samples         = pass_raster_samples();
    return 0;
}

static void demo_destroy_targets(VulkanContext *vk, EngineTargets *targets) {
    if (targets == nullptr) {
        return;
    }
    TargetEntry *entry = target_find(targets);
    if (entry != nullptr) {
        pass_depth_destroy(vk, &entry->depth);
        pass_coverage_destroy(vk, &entry->coverage);
        entry->used = false;
        // Poison: a copied EngineTargets still naming these handles must
        // not match a future entry that recycles the same image handle
        // with a different generation. Handles are zeroed by destroy.
    }
    *targets = (EngineTargets){0};
}

// ------------------------------------------------------------------- bench

static uint32_t s_bench_target      = 0u;
static uint32_t s_bench_frames      = 0u;
static uint32_t s_bench_samples     = 0u;
static uint64_t s_bench_last_seq    = 0u;
static uint64_t s_bench_sum_tot_ns  = 0u;
static uint64_t s_bench_sum_comp_ns = 0u;
static uint64_t s_bench_sum_rend_ns = 0u;
static uint64_t s_bench_min_tot_ns  = UINT64_MAX;
static uint64_t s_bench_max_tot_ns  = 0u;
static uint64_t s_bench_t0_ns       = 0u;
static bool     s_bench_live_draw   = false;
static bool     s_bench_done        = false;

static void read_proc_rss_kib(uint64_t *out_rss_kib, uint64_t *out_hwm_kib) {
    uint64_t rss = 0u;
    uint64_t hwm = 0u;
    FILE *fp = fopen("/proc/self/status", "r");
    if (fp != nullptr) {
        char line[256];
        while (fgets(line, sizeof line, fp) != nullptr) {
            unsigned long long v = 0ull;
            if (sscanf(line, "VmRSS: %llu kB", &v) == 1) {
                rss = (uint64_t)v;
            } else if (sscanf(line, "VmHWM: %llu kB", &v) == 1) {
                hwm = (uint64_t)v;
            }
        }
        fclose(fp);
    }
    if (out_rss_kib != nullptr) {
        *out_rss_kib = rss;
    }
    if (out_hwm_kib != nullptr) {
        *out_hwm_kib = hwm;
    }
}

static void emit_bench_summary(uint32_t width, uint32_t height, uint64_t modifier) {
    uint64_t now_ns = ring_mono_ns();
    double wall_ms = (s_bench_t0_ns != 0u && now_ns > s_bench_t0_ns)
        ? ((double)(now_ns - s_bench_t0_ns) / 1000000.0)
        : 0.0;
    double avg_tot  = s_bench_samples > 0u ? ((double)s_bench_sum_tot_ns / (1000.0 * s_bench_samples)) : 0.0;
    double avg_comp = s_bench_samples > 0u ? ((double)s_bench_sum_comp_ns / (1000.0 * s_bench_samples)) : 0.0;
    double avg_rend = s_bench_samples > 0u ? ((double)s_bench_sum_rend_ns / (1000.0 * s_bench_samples)) : 0.0;
    double min_tot  = s_bench_min_tot_ns == UINT64_MAX ? 0.0 : ((double)s_bench_min_tot_ns / 1000.0);
    double max_tot  = (double)s_bench_max_tot_ns / 1000.0;

    BdaStats bda = {0};
    bda_get_stats(&bda);
    uint64_t depth_pool_bytes = 0u;
    uint64_t cover_pool_bytes = 0u;
    pass_pool_stats(&depth_pool_bytes, &cover_pool_bytes);
    BackendBenchStats bstat = {0};
    backend_get_bench_stats(&bstat);
    SceneBenchStats sstat = {0};
    scene_get_stats(&sstat);
    LightingBenchStats lstat = {0};
    lighting_get_stats(&lstat);
    PhysicsBenchStats pstat = {0};
    physics_get_stats(&pstat);
    bool visbuf_on = scene_visbuf_active();
    bool phys_on   = physics_gpu_active();
    bool gs_on     = backend_2dgs_active();
    uint64_t rss_kib = 0u;
    uint64_t hwm_kib = 0u;
    read_proc_rss_kib(&rss_kib, &hwm_kib);
    uint32_t objects = 0u;
    uint32_t verts = 0u;
    world_stats(&objects, &verts);
    VulkanContext *vk = vulkan_get_context();

    printf("[Bench] world=%u res=%ux%u msaa=%ux frames=%u samples=%u wall_ms=%.2f "
           "gpu_total_us(avg=%.2f,min=%.2f,max=%.2f) gpu_compute_us=%.2f gpu_render_us=%.2f "
           "pass_pool_mib=%.2f(depth=%.2f,cover=%.2f) "
           "bda_dev_mib=%.2f/%.2f bda_stg_mib=%.2f/%.2f "
           "terrain_arena_mib=%.2f overlay_arena_mib=%.2f fused_cache_mib=%.2f "
           "tiles_res=%u/%u stream_loads=%u uring_us=%llu uring_boot_us=%llu async_cqes=%u fixed_buf_reads=%u zero_copy_uma=%u "
           "2dgs=%u surfels=%u/%u surfel_kib=%.1f mvs_gpu=%u "
           "scene_slab_kib=%.1f meshlets=%u/%u(p1=%u,p2=%u,cone=%u,frust=%u,hiz=%u) visbuf=%u hiz_pass=%u "
           "gi=%u gi_oracle=%u gi_res=%ux%u gi_rays=%u gi_proxies=%u gi_mib=%.2f "
           "phys_gpu=%u phys_iters=%u phys_verts=%u phys_cons=%u phys_colors=%u phys_disp=%u phys_kib=%.1f "
           "rss_mib=%.2f hwm_mib=%.2f objs=%u verts=%u mod=0x%llx gpu=\"%s\" profile=\"%s\"\n",
           world_selected_id(),
           width, height,
           (uint32_t)pass_raster_samples(),
           s_bench_frames, s_bench_samples, wall_ms,
           avg_tot, min_tot, max_tot, avg_comp, avg_rend,
           (double)(depth_pool_bytes + cover_pool_bytes) / (1024.0 * 1024.0),
           (double)depth_pool_bytes / (1024.0 * 1024.0),
           (double)cover_pool_bytes / (1024.0 * 1024.0),
           (double)bda.device_allocated / (1024.0 * 1024.0),
           (double)bda.device_capacity / (1024.0 * 1024.0),
           (double)bda.staging_allocated / (1024.0 * 1024.0),
           (double)bda.staging_capacity / (1024.0 * 1024.0),
           (double)bstat.bda_arena_bytes / (1024.0 * 1024.0),
           (double)bstat.overlay_arena_bytes / (1024.0 * 1024.0),
           (double)bstat.fused_cache_bytes / (1024.0 * 1024.0),
           bstat.resident_tiles, bstat.slot_capacity,
           bstat.stream_loads_total,
           (unsigned long long)bstat.stream_uring_us_total,
           (unsigned long long)bstat.stream_boot_uring_us,
           bstat.stream_async_cqes,
           bstat.stream_fixed_buf_reads,
           bstat.zero_copy_uma ? 1u : 0u,
           gs_on ? 1u : 0u,
           bstat.visible_surfels, bstat.surfel_count,
           (double)bstat.surfel_bda_bytes / 1024.0,
           bstat.mvs_gpu_dispatches,
           (double)sstat.slab_bytes / 1024.0,
           sstat.visible_meshlets, sstat.active_meshlets,
           sstat.phase1_draws, sstat.phase2_draws,
           sstat.cone_culled, sstat.frustum_culled, sstat.hiz_culled,
           visbuf_on ? 1u : 0u,
           sstat.hiz_enabled ? 1u : 0u,
           lstat.gi_enabled ? 1u : 0u,
           lstat.oracle_mode,
           lstat.gi_width, lstat.gi_height,
           lstat.hrc_total_rays,
           lstat.active_proxies,
           (double)lstat.bda_bytes / (1024.0 * 1024.0),
           (phys_on && pstat.gpu_enabled) ? 1u : 0u,
           pstat.iterations,
           pstat.active_verts,
           pstat.active_constraints,
           pstat.num_colors,
           pstat.dispatches_frame,
           (double)pstat.bda_bytes / 1024.0,
           (double)rss_kib / 1024.0,
           (double)hwm_kib / 1024.0,
           objects, verts,
           (unsigned long long)modifier,
           vk != nullptr ? vk->device_info.name : "unknown",
           vk != nullptr ? vulkan_profile_to_str(vk->device_info.execution_profile) : "unknown");
    fflush(stdout);
}

// ------------------------------------------------------------------- hooks

static void print_worlds_usage(const char *prog) {
    printf("Usage: %s [--world=<0..9|name|file.world> | -w <0..9|name|file.world>] [--image=<path.png|jpg>] [--bench[=<frames>]] [--list-worlds] [--help]\n\n"
           "Pure Engine Default & Built-On-Top Worlds:\n"
           "  --world=0  [Pure    | Empty Canvas  | Sealed    ] Pure Empty Engine (Default: 0 objects, 0 grid, 0 HUD)\n"
           "  --world=1  [3D-Only | PERM_DRAW=on  | Tab=locked] Click-to-Draw Studio (Points -> Lines -> Faces)\n"
           "  --world=2  [3D-Only | Gravity+Bounce| Tab=locked] Non-Cube Solids Squash-and-Stretch Drop\n"
           "  --world=3  [2D-Only | Gravity+Soft  | Tab=locked] Deformable Soft Patches & Elastic Polygons\n"
           "  --world=4  [2D-Only | PERM_DRAW=on  | Tab=locked] 2D Click-to-Draw Polygon Workshop & Sheet\n"
           "  --world=5  [Hybrid  | 3D+2D+Draw    | Tab=open  ] Combined 3D+2D Gravity, Soft Bodies & Draw\n"
           "  --world=6  [3D+2D   | Multi-Bounce  | Tab=open  ] Restitution Staircase (0.08..0.94) & Bumper Lab\n"
           "  --world=7  [Hybrid  | C23 Builder   | Tab=open  ] Procedural Kinetic Cathedral & Polyring Cascade\n"
           "  --world=8  [3D/Map  | RelPtr2D+SSD  | Tab=locked] Baklan & Beşparmak Dağı Real DEM + Satellite Map\n"
           "  --world=9  [Sandbox | Full 3D/2D/Hyb| Tab=open  ] Interactive Full Sandbox (opening.world + sheet + Plots)\n"
           "  --world=<path.world>                              Load a custom .world DSL scene file on top of the engine\n"
           "  --image=<path.png|jpg>                            Load a custom bird's-eye image overlay into World 8 (50%% transparent)\n",
           prog != nullptr ? prog : "engine");
    fflush(stdout);
}

static bool has_world_suffix_or_sep(const char *arg) {
    if (arg == nullptr || arg[0] == '\0') {
        return false;
    }
    if (strchr(arg, '/') != nullptr) {
        return true;
    }
    size_t len = strlen(arg);
    return len > 6u && strcmp(arg + len - 6u, ".world") == 0;
}

static bool parse_world_arg(const char *arg, uint32_t *out_id) {
    if (arg == nullptr || arg[0] == '\0' || out_id == nullptr) {
        return false;
    }
    if (strcmp(arg, "pure") == 0 || strcmp(arg, "empty") == 0 || strcmp(arg, "none") == 0) {
        *out_id = 0u;
        return true;
    }
    if (strcmp(arg, "draw3d") == 0) {
        *out_id = 1u;
        return true;
    }
    if (strcmp(arg, "gravity3d") == 0 || strcmp(arg, "solids") == 0) {
        *out_id = 2u;
        return true;
    }
    if (strcmp(arg, "soft2d") == 0 || strcmp(arg, "2d") == 0) {
        *out_id = 3u;
        return true;
    }
    if (strcmp(arg, "draw2d") == 0) {
        *out_id = 4u;
        return true;
    }
    if (strcmp(arg, "hybrid") == 0) {
        *out_id = 5u;
        return true;
    }
    if (strcmp(arg, "bounce") == 0 || strcmp(arg, "elastic") == 0) {
        *out_id = 6u;
        return true;
    }
    if (strcmp(arg, "procedural") == 0 || strcmp(arg, "cathedral") == 0 || strcmp(arg, "builder") == 0) {
        *out_id = 7u;
        return true;
    }
    if (strcmp(arg, "map") == 0 || strcmp(arg, "besparmak") == 0 ||
        strcmp(arg, "baklan") == 0 || strcmp(arg, "terrain") == 0) {
        *out_id = 8u;
        return true;
    }
    if (strcmp(arg, "sandbox") == 0 || strcmp(arg, "default") == 0 || strcmp(arg, "all") == 0) {
        *out_id = 9u;
        return true;
    }
    char *end = nullptr;
    unsigned long v = strtoul(arg, &end, 10);
    if (end == arg || *end != '\0' || v > (unsigned long)k_world_preset_max) {
        return false;
    }
    *out_id = (uint32_t)v;
    return true;
}

static int demo_configure(int argc, char **argv) {
    uint32_t chosen = 0u;
    const char *custom_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (a == nullptr) {
            continue;
        }
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0 || strcmp(a, "--list-worlds") == 0) {
            print_worlds_usage(argv[0]);
            return 1;
        }
        if (strcmp(a, "--bench") == 0) {
            s_bench_target = 90u;
            frame_set_bench_mode(true);
            continue;
        }
        if (strncmp(a, "--bench=", 8) == 0) {
            char *end = nullptr;
            unsigned long n = strtoul(a + 8, &end, 10);
            if (end == a + 8 || *end != '\0' || n < 10ul || n > 100000ul) {
                fprintf(stderr, "[Engine] Invalid --bench frame count '%s' (expected 10..100000)\n", a + 8);
                return -1;
            }
            s_bench_target = (uint32_t)n;
            frame_set_bench_mode(true);
            continue;
        }
        if (strncmp(a, "--image=", 8) == 0) {
            backend_overlay_set_custom_path(a + 8);
            continue;
        }
        if (strncmp(a, "--overlay=", 10) == 0) {
            backend_overlay_set_custom_path(a + 10);
            continue;
        }
        if (strcmp(a, "--image") == 0 || strcmp(a, "--overlay") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "[Engine] Missing file path after '%s'\n", a);
                return -1;
            }
            backend_overlay_set_custom_path(argv[++i]);
            continue;
        }
        const char *val = nullptr;
        if (strncmp(a, "--world=", 8) == 0) {
            val = a + 8;
        } else if (strncmp(a, "-w=", 3) == 0) {
            val = a + 3;
        } else if (strcmp(a, "--world") == 0 || strcmp(a, "-w") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "[Engine] Missing world number after '%s' (expected 0..%u or .world path)\n",
                        a, k_world_preset_max);
                print_worlds_usage(argv[0]);
                return -1;
            }
            val = argv[++i];
        } else if (strncmp(a, "-w", 2) == 0 && a[2] != '\0') {
            val = a + 2;
        } else {
            fprintf(stderr, "[Engine] Unknown option '%s'\n", a);
            print_worlds_usage(argv[0]);
            return -1;
        }
        if (parse_world_arg(val, &chosen)) {
            custom_path = nullptr;
        } else if (has_world_suffix_or_sep(val)) {
            custom_path = val;
        } else {
            fprintf(stderr, "[Engine] Invalid world '%s' (expected 0..%u, preset name, or .world path)\n",
                    val, k_world_preset_max);
            print_worlds_usage(argv[0]);
            return -1;
        }
    }
    if (custom_path != nullptr) {
        if (!world_select_file(custom_path)) {
            fprintf(stderr, "[Engine] Invalid world file path '%s'\n", custom_path);
            return -1;
        }
    } else {
        world_select(chosen);
    }
    return 0;
}

static int demo_init(VulkanContext *vk, RingCqeFn forward_cqe, void *forward_user) {
    world_apply_policy();
    if (pass_init(vk) != 0 ||
        lighting_init(vk) != 0 ||
        physics_init(vk) != 0 ||
        scene_init(vk, pass_depth_format(), pass_raster_samples()) != 0 ||
        chrome_gpu_bind_pass(vk, pass_depth_format(), pass_raster_samples()) != 0 ||
        cube_init(vk) != 0 || solid_init(vk) != 0 || shape_init(vk) != 0 ||
        plane_init(vk) != 0 || material_init(vk) != 0 ||
        plot_init(vk, forward_cqe, forward_user) != 0 ||
        mesh_init(vk, forward_cqe, forward_user) != 0 ||
        terrain_init(vk) != 0 ||
        gimbal_init(vk) != 0 || hud_init(vk) != 0 || text_init(vk) != 0 || grid_init(vk) != 0) {
        return -1;
    }
    return 0;
}

static void demo_shutdown(VulkanContext *vk) {
    if (s_bench_target == 0u) {
        (void)world_save();
        (void)sheet_save();
    }
    grid_shutdown(vk);
    text_shutdown(vk);
    hud_shutdown(vk);
    gimbal_shutdown(vk);
    terrain_shutdown(vk);
    mesh_shutdown(vk);
    plot_shutdown(vk);
    material_shutdown(vk);
    plane_shutdown(vk);
    shape_shutdown(vk);
    solid_shutdown(vk);
    cube_shutdown(vk);
    scene_shutdown(vk);
    physics_shutdown(vk);
    lighting_shutdown(vk);
    pass_shutdown(vk);
}

static void demo_record_compute(VkCommandBuffer cmd, uint32_t heat_dt, bool live) {
    s_prepare_live = live;
    world_record(cmd, heat_dt, live);
}

static void demo_prepare_draw(VkCommandBuffer cmd, VkImage depth_image) {
    plot_prepare(cmd);
    const TargetEntry *entry = target_find_by_depth(depth_image);
    if (entry != nullptr) {
        world_prepare_draw(cmd, entry->width, entry->height, s_prepare_live);
    }
}

static void demo_claim_frame(VkSemaphore acquire, uint64_t signal_point) {
    // This slot's previous acquire has already retired. Rewind only its
    // half of the transient bump. The other slot may still be reading.
    bda_transient_begin_frame(signal_point);
    cube_bind_acquire(acquire);
    shape_begin(acquire, signal_point);
    text_begin(acquire, signal_point);
    plot_begin(acquire, signal_point);
}

static void demo_release_frame(uint64_t signal_point) {
    shape_release(signal_point);
    text_release(signal_point);
    plot_release(signal_point);
}

static void demo_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live) {
    s_bench_live_draw = live;
    world_draw(cmd, width, height, live);
}

static void demo_draw_overlay(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                              VkFormat format, VkFormat depth_format,
                              VkSampleCountFlagBits samples, uint32_t frame_ms,
                              bool have_stamp, uint64_t modifier) {
    hud_prepare(frame_ms, have_stamp, modifier);
    hud_draw(cmd, width, height, format, depth_format, samples);
    text_draw(cmd, width, height, format, depth_format, samples);

    bool is_live_scene = s_bench_live_draw;
    s_bench_live_draw = false;
    if (s_bench_target > 0u && !s_bench_done && is_live_scene) {
        if (s_bench_frames == 0u) {
            s_bench_t0_ns = ring_mono_ns();
        }
        s_bench_frames++;
        if (s_bench_frames > 10u) {
            uint64_t comp_ns = 0u;
            uint64_t rend_ns = 0u;
            uint64_t tot_ns  = 0u;
            uint64_t seq     = 0u;
            frame_gpu_raw_ns(&comp_ns, &rend_ns, &tot_ns, &seq);
            if (seq != s_bench_last_seq && tot_ns > 0u) {
                s_bench_last_seq = seq;
                s_bench_sum_tot_ns  += tot_ns;
                s_bench_sum_comp_ns += comp_ns;
                s_bench_sum_rend_ns += rend_ns;
                if (tot_ns < s_bench_min_tot_ns) {
                    s_bench_min_tot_ns = tot_ns;
                }
                if (tot_ns > s_bench_max_tot_ns) {
                    s_bench_max_tot_ns = tot_ns;
                }
                s_bench_samples++;
            }
        }
        if (s_bench_frames >= s_bench_target) {
            s_bench_done = true;
            emit_bench_summary(width, height, modifier);
            (void)kill(getpid(), SIGTERM);
        }
    }
}

static bool demo_simulation(uint32_t time_ms, const FrameStep *step, uint32_t *heat_dt) {
    uint32_t sim_dt = step != nullptr ? step->sim_dt_ms : 0u;
    return world_simulation(time_ms, sim_dt, heat_dt);
}

static bool demo_advance(uint32_t time_ms, const FrameStep *step) {
    (void)step;  // time_ms is the sim cursor issued with step->sim_dt_ms.
    int32_t px = 0;
    int32_t py = 0;
    bool have = pointer_client_xy(&px, &py);
    text_arm_tick(text_is_visible() && pointer_key_down(KEY_T), have, px, py);
    bool moved = world_advance(time_ms);
    if (s_bench_target > 0u && !s_bench_done) {
        uint32_t wid = world_selected_id();
        if (wid == 8u) {
            // Deterministic LOD 0 -> LOD 4 zoom + pan sweep to exercise io_uring SSD tile streaming
            (void)camera_zoom_steps(0.18f);
            (void)camera_pointer_drag(4.0f, 2.0f, ENG_BUTTON_MIDDLE);
        } else if (wid != 0u) {
            if (policy_mode() == MODE_2D) {
                (void)camera_pointer_drag(2.0f, 1.0f, ENG_BUTTON_MIDDLE);
            } else {
                (void)camera_pointer_drag(2.2f, 0.4f, ENG_BUTTON_RIGHT);
            }
        } else {
            // Static Pure Engine canvas: report only bounded HUD region dirty
            // so cross-GPU PRIME DMA copy and Wayland damage_buffer transfer <2% of pixels.
            frame_note_damage_rect(0, 0, 320u, 64u);
        }
        return true;
    }
    return moved;
}

static bool demo_click(float origin_x, float origin_y) {
    if (text_is_visible() && text_click(origin_x, origin_y, pointer_key_down(KEY_T))) {
        return true;
    }
    return world_click(origin_x, origin_y);
}

static bool demo_text(const char *utf8, uint32_t bytes) {
    if (!text_is_visible()) {
        return false;
    }
    return text_insert(utf8, bytes);
}

static bool demo_scroll(float steps) {
    return camera_zoom_steps(steps);
}

static bool demo_shift_down(void) {
    return pointer_key_down(KEY_LEFTSHIFT) || pointer_key_down(KEY_RIGHTSHIFT);
}

static uint8_t demo_route_key(uint32_t key) {
    if (text_editing()) {
        if (key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT) {
            return ENGINE_KEY_HOLD;
        }
        return ENGINE_KEY_TAP;
    }
    if (key == KEY_T && text_is_visible()) {
        return ENGINE_KEY_HOLD;
    }
    if ((key >= KEY_F1 && key <= KEY_F10) || key == KEY_0 ||
        key == KEY_K || key == KEY_KP0 || key == KEY_Y || key == KEY_H ||
        key == KEY_7 || key == KEY_8) {
        return ENGINE_KEY_TAP;
    }
    if (world_selected_id() == 8u) {
        if (key == KEY_O || key == KEY_P ||
            key == KEY_W || key == KEY_A || key == KEY_S || key == KEY_D ||
            key == KEY_I || key == KEY_U || key == KEY_J || key == KEY_N ||
            key == KEY_LEFTBRACE || key == KEY_RIGHTBRACE ||
            key == KEY_B || key == KEY_ENTER || key == KEY_KPENTER) {
            return ENGINE_KEY_TAP;
        }
    }
    KeyRoute route = policy_route(policy_active(), (uint16_t)key, demo_shift_down());
    if (route == KEY_ROUTE_HOLD) {
        return ENGINE_KEY_HOLD;
    }
    if (route == KEY_ROUTE_TAP) {
        return ENGINE_KEY_TAP;
    }
    return ENGINE_KEY_IGNORE;
}

static bool demo_key(uint32_t key, EngineKeyContext *ctx) {
    if (text_is_visible() && text_on_key(key)) {
        return true;
    }
    if (key == KEY_F10 || key == KEY_0) {
        return world_switch_to(0u);
    }
    if (key >= KEY_F1 && key <= KEY_F9) {
        if (key == KEY_F5 && !demo_shift_down() &&
            policy_can(policy_active(), ACT_RESPIN) &&
            table_selected() != nullptr) {
            return world_respin_selected();
        }
        return world_switch_to((uint32_t)(key - KEY_F1 + 1u));
    }
    if (key == KEY_K || key == KEY_KP0) {
        return terrain_toggle_contours();
    }
    if (key == KEY_Y) {
        return scene_toggle_visbuf();
    }
    if (key == KEY_H) {
        return lighting_toggle_gi();
    }
    if (key == KEY_7) {
        return backend_toggle_2dgs();
    }
    if (key == KEY_8) {
        return physics_toggle_gpu();
    }
    if (world_selected_id() == 8u) {
        bool shift = demo_shift_down();
        if (key == KEY_O) {
            // 'o' key -> more transparent (+5% transparency)
            return backend_overlay_adjust_transparency(0.05f);
        }
        if (key == KEY_P) {
            // 'p' key -> less transparent (-5% transparency)
            return backend_overlay_adjust_transparency(-0.05f);
        }
        if (key == KEY_W || key == KEY_A || key == KEY_S || key == KEY_D) {
            int ix = (key == KEY_D ? 1 : 0) - (key == KEY_A ? 1 : 0);
            int iy = (key == KEY_W ? 1 : 0) - (key == KEY_S ? 1 : 0);
            if (shift) {
                // Shift + WASD stretches the bird's-eye image by 0.5% in X or Y
                return backend_overlay_stretch_2d(ix, iy);
            }
            float rx = 1.0f;
            float ry = 0.0f;
            float fx = 0.0f;
            float fy = 1.0f;
            float zoom = 0.25f;
            camera_map_basis(&rx, &ry, &fx, &fy, &zoom);
            float step = 8.0f / fmaxf(zoom, 0.02f);
            float dx = ((float)ix * rx + (float)iy * fx) * step;
            float dy = ((float)ix * ry + (float)iy * fy) * step;
            if (backend_overlay_move(dx, dy, true)) {
                return true;
            }
            return false;
        }
        if (key == KEY_I) {
            return backend_overlay_stretch_2d(1, 0);
        }
        if (key == KEY_U) {
            return backend_overlay_stretch_2d(-1, 0);
        }
        if (key == KEY_J) {
            return backend_overlay_stretch_2d(0, 1);
        }
        if (key == KEY_N) {
            return backend_overlay_stretch_2d(0, -1);
        }
        if (key == KEY_RIGHTBRACE) {
            return backend_overlay_stretch_2d(1, 1);
        }
        if (key == KEY_LEFTBRACE) {
            return backend_overlay_stretch_2d(-1, -1);
        }
        if (key == KEY_B) {
            float cx = 1595.0f;
            float cy = -650.0f;
            (void)backend_overlay_get_center(&cx, &cy);
            return camera_snap_birdseye(cx, cy);
        }
        if (key == KEY_ENTER || key == KEY_KPENTER) {
            return backend_overlay_commit_upload();
        }
    }
    EngineAction act = policy_lookup(policy_active(), (uint16_t)key, demo_shift_down());
    if (!policy_can(policy_active(), act)) {
        return false;
    }
    switch (act) {
    case ACT_CANCEL:
        return world_cancel();
    case ACT_DELETE:
        return world_delete();
    case ACT_RESTORE:
        return world_restore();
    case ACT_CLOSE_FACE:
        return world_close_face();
    case ACT_RESPIN:
        return world_respin_selected();
    case ACT_TOGGLE_SPIN:
        return world_toggle_spin();
    case ACT_TOGGLE_SMOOTH:
        return world_toggle_smooth();
    case ACT_TOGGLE_GLOW:
        return world_toggle_glow();
    case ACT_PLOT_CYCLE:
        return plot_cycle();
    case ACT_DATA_RELOAD: {
        EngineRelay relay;
        engine_relay_begin(&relay, ctx->signal_armed, ctx->got_signal,
                           ctx->signo, ctx->inotify_armed);
        bool changed = world_reload();
        if (plot_reload(ctx->reload_cqe, nullptr)) {
            changed = true;
        }
        if (policy_mode() != MODE_2D) {
            bool mesh_has = false;
            bool mesh_bytes = mesh_reload(ctx->reload_cqe, nullptr, &mesh_has);
            if (world_apply_mesh(mesh_has, mesh_bytes)) {
                changed = true;
            }
            if (terrain_reload()) {
                changed = true;
            }
        }
        if (policy_mode() != MODE_3D && sheet_reload()) {
            changed = true;
        }
        if (engine_relay_end(&relay, ctx->wl, ctx->vk, ctx->pending_serial,
                             ctx->send_in_flight, ctx->resend_wire,
                             ctx->run_rc, ctx->why)) {
            *ctx->stop = true;
        }
        return changed;
    }
    case ACT_SNAP_FRONT:
        return camera_snap_front();
    case ACT_HOME:
        return camera_home();
    case ACT_VIEW_TOP:
        return camera_view_top();
    case ACT_VIEW_BOTTOM:
        return camera_view_bottom();
    case ACT_VIEW_LEFT:
        return camera_view_left();
    case ACT_VIEW_RIGHT:
        return camera_view_right();
    case ACT_VIEW_FRONT:
        return camera_view_front();
    case ACT_TOGGLE_DRAW: {
        WorkspacePolicy *live = policy_active();
        if (policy_allows(PERM_DRAW)) {
            if (!policy_revoke(live, PERM_DRAW)) {
                return false;
            }
            (void)face_cancel_chain();
            printf("[Draw] Points are off.\n");
            fflush(stdout);
            return true;
        }
        if (!policy_grant(live, PERM_DRAW)) {
            return false;
        }
        printf("[Draw] Points are on.\n");
        fflush(stdout);
        return true;
    }
    case ACT_TOGGLE_GIMBAL:
        return gimbal_toggle();
    case ACT_SPAWN_SOLID_1:
    case ACT_SPAWN_SOLID_2:
    case ACT_SPAWN_SOLID_3:
    case ACT_SPAWN_SOLID_4:
    case ACT_SPAWN_SOLID_5:
    case ACT_SPAWN_SOLID_6:
        return world_raise_solid((uint32_t)(act - ACT_SPAWN_SOLID_1));
    case ACT_SWITCH_MODE:
        if (!policy_switch()) {
            return false;
        }
        if (policy_active()->lock_2d_quat) {
            (void)camera_snap_front();
        }
        return true;
    default:
        return false;
    }
}

static bool demo_camera_active(void) {
    if (s_bench_target > 0u && !s_bench_done) {
        return true;
    }
    if (world_selected_id() == 8u && !demo_shift_down()) {
        if (pointer_key_down(KEY_W) || pointer_key_down(KEY_A) ||
            pointer_key_down(KEY_S) || pointer_key_down(KEY_D)) {
            return true;
        }
    }
    return camera_motion_down() || camera_busy();
}

static bool demo_drag(float dx, float dy, uint32_t buttons) {
    return camera_pointer_drag(dx, dy, buttons);
}

static const EngineApp s_app = {
    .help = "One turn, then io_uring idle. Points stay off until numpad 9. "
            "Empty clicks then place points, and F closes them into a face that stays. "
            "R spins until R again. F5 turns the selection again. F1-F9 switch worlds 1-9, "
            "F10 or 0 switches to Pure Engine (World 0). K or numpad 0 toggles topographic "
            "elevation contours in World 8. Y toggles R32_UINT Visibility Buffer deferred shading. "
            "H toggles 2026 SSILVB + Split Radiance Cascade Global Illumination. "
            "7 toggles Oriented 2D Gaussian Splatting (2DGS) geodetic surfels in World 8. "
            "8 toggles GPU Augmented Vertex Block Descent (AVBD) physics compute. "
            "A click on a mesh "
            "selects it. A cube click plants heat for five seconds and shoves the cube. "
            "It stops on the ground and against other cubes, and that place is saved. "
            "It can be selected while other meshes are up. Its colors sweep on that clock, then hold. "
            "Keys 1-6 raise a tetrahedron, octahedron, icosahedron, cylinder, cone, and torus. "
            "S smooths edges. G opens or closes glow. Esc clears. Backspace, Delete, or X removes. "
            "Z puts the last removed cube, solid, or mesh back. "
            "M cycles the math plot. L reloads the active .world scene, CSV chart, and OBJ mesh. "
            "In 2D and Hybrid, L also reads sheet.world again. "
            "Numpad 8, 2, 4, 6, and 5 set top, bottom, left, right, and front. "
            "Numpad 5 again sets the back. Home or numpad 1 restores the opening camera, "
            "including zoom and position. Numpad 7 shows or hides the view ball. "
            "The wheel and the touchpad zoom. Right-drag orbits. Middle-drag pans. "
            "Arrows move on the screen axes. Q and E "
            "yaw, C and V pitch. Minus zooms out. Plus is numpad plus, shift+4, or equals. "
            "Tab cycles 3D, 2D, and Hybrid. 2D is a flat sheet of rigid boxes, "
            "a soft patch, and shapes from sheet.world. It does not step the fillets "
            "or the lattice. Hybrid draws the 3D scene and that sheet. "
            "Hold T and click twice to place a text box, then type. Escape leaves it. "
            "Edits are written to examples/demo/world/live.world. Faces and heat stay unsaved.",
    .configure         = demo_configure,
    .init              = demo_init,
    .shutdown          = demo_shutdown,
    .load              = world_load,
    .reset             = world_reset,
    .create_targets    = demo_create_targets,
    .destroy_targets   = demo_destroy_targets,
    .clear_color       = world_clear_color,
    .record_compute    = demo_record_compute,
    .prepare_draw      = demo_prepare_draw,
    .claim_frame       = demo_claim_frame,
    .draw              = demo_draw,
    .draw_overlay      = demo_draw_overlay,
    .record_abort      = world_record_abort,
    .record_ok         = world_record_ok,
    .record_published  = world_record_published,
    .record_step_open  = world_record_step_open,
    .record_poke_pending = world_record_poke_pending,
    .release_frame     = demo_release_frame,
    .simulation        = demo_simulation,
    .simulation_hold   = world_simulation_hold,
    .simulation_commit = world_simulation_commit,
    .advance           = demo_advance,
    .busy              = world_busy,
    .consume_spin      = world_consume_spin,
    .commit            = world_commit,
    .note_cqe          = hud_note_cqe,
    .on_cqe            = backend_on_cqe,
    .turn_report       = hud_turn_report,
    .click             = demo_click,
    .drag              = demo_drag,
    .scroll            = demo_scroll,
    .text              = demo_text,
    .key               = demo_key,
    .camera_active     = demo_camera_active,
    .route_key         = demo_route_key,
    .watched_path      = world_source_path,
    .reload_watched    = world_reload,
};

const EngineApp *engine_app(void) {
    return &s_app;
}
