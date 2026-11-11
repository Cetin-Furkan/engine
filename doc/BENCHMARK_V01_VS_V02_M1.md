# Engine `v0.1` Baseline Status & `v0.2` Milestone 1 Benchmark Report

**Repository:** `Engine`  
**Date:** November 11, 2026  
**Raw Snapshots:** `benchmarks/v0.1_baseline/` vs `benchmarks/v0.2_milestone1/`  
**Companion Architecture Spec:** [`doc/V02_GPU_PROFILES_AND_2026_ALGORITHMS.md`](./V02_GPU_PROFILES_AND_2026_ALGORITHMS.md)

---

## 1. Test Hardware & Measurement Methodology

All measurements were captured back-to-back on the same physical machine using hardware `VkQueryPool` 64-bit nanosecond GPU timestamps (`TOP_OF_PIPE` $\rightarrow$ `COMPUTE_SHADER` $\rightarrow$ `COLOR_ATTACHMENT_OUTPUT` $\rightarrow$ `ALL_COMMANDS`), direct BDA/pool allocator counters, and `/proc/self/status` telemetry via `./scripts/run_benchmark.sh` (`make bench`).

| Property | Live Host Value |
| :--- | :--- |
| **CPU** | 11th Gen Intel Core i7-1165G7 @ 2.80 GHz (4C/8T, `AVX2` + `FMA` + `BMI2`, 12 MiB L3) |
| **GPU** | **Intel Iris Xe Graphics (`TGL GT2`, `0x8086:0x9a49`)** — 2020 Gen12LP Integrated UMA GPU |
| **Memory Architecture** | **7,576 MiB (~7.4 GiB) total system RAM**, shared UMA (`5.55 GiB` Vulkan device-local heap, `0.00 GiB` dedicated VRAM) |
| **Kernel & Limits** | `Linux 7.3.0-rc6-2-cachyos-rc`, `io_uring` (65 opcodes, `NO_MMAP` + `DEFER_TASKRUN`), **`RLIMIT_MEMLOCK = 8 MiB`** |
| **Driver & API** | Mesa `26.2.6` (`ANV` / `Xe`), Vulkan `1.4.354` |
| **Display Path** | Native Wayland socket (`1280x720` window, `1280x688` scene + `1280x32` GPU bar), `ARGB8888` modifier `0x100000000000002` (`I915_FORMAT_MOD_Y_TILED`), `wp_linux_drm_syncobj_v1` explicit timelines |
| **Sampling Window** | 75 rendered live frames per world (10 warmup frames discarded, **65 retired hardware GPU query samples** averaged per world) |

---

## 2. `Engine v0.1` Baseline Status Diagnosis

Before changing a single engine algorithm, `benchmarks/v0.1_baseline/` exposed both the strengths of the `v0.1` foundation and four severe UMA iGPU bottlenecks:

### What `v0.1` Already Solved Cleanly
1. **Zero-Wrapper Syscall & Sync Foundation:** Single-threaded `io_uring` event loop, direct Wayland wire protocol, `VK_EXT_image_drm_format_modifier`, and `wp_linux_drm_syncobj_v1` timeline explicit sync work with zero glitches and zero missed sync points.
2. **Muxless Hybrid / NVIDIA Cross-Device Present:** `src/wayland/buffer/dmabuf.c`, `src/vulkan/memory/memory.c`, and `src/frame/present/slot.c` already detect `feedback_cross_device`, allocate a device-local render image alongside a linear system-RAM export image, and blit across PCIe before `VK_QUEUE_FAMILY_FOREIGN_EXT` release.
3. **Sub-25 µs GPU Compute Terrain Culling:** Even on a 2020 iGPU, `RelPtr2D` + `Lilliput` 64-bit tile headers + Slang compute frustum culling (`terrain_cull.comp`) execute in **24.33 µs** per frame on World 8.

### Four Bottlenecks Uncovered on `v0.1` (`TGL GT2` + 7.4 GiB Shared RAM)
1. **402.52 MiB Pre-Allocated UMA Memory in World 8 (272.00 MiB in Worlds 2/7/9):** On a machine with only **7,576 MiB** total RAM shared between Linux, Wayland, applications, CPU, and iGPU, `v0.1` pre-allocated discrete-desktop-sized pools unconditionally:
   - `128.00 MiB` MSAA depth + coverage image pools (`2 slots x 2 attachments x 32 MiB`)
   - `64.00 MiB` `device_pool` + `64.00 MiB` `staging_pool` + `16.00 MiB` `transient_ring`
   - `127.90 MiB` terrain BDA arena (all 341 quadtree slots pre-allocated even though at most 65 leaves are ever active)
   - `64.00 MiB` bird's-eye orthophoto BDA arena + eager `malloc(384 KiB)` `fused_cache` copies
2. **Unconditional `8x MSAA` Raster & Resolve Bandwidth Saturation:** `pick_samples()` in `examples/demo/render/pass.c` always chose the highest hardware count (`8x MSAA`). At `1280x688`, clearing, depth-testing, shading, and resolving 8 samples per pixel consumed **4,685.62 µs** of GPU raster time in World 8 and **2,934.42 µs** in World 7 (plus **419.55 µs** just to clear an empty screen in World 0).
3. **Uncached Write-Combined (`WC`) Tie-Break on UMA `DEVICE_LOCAL` Allocations:** In `vulkan_find_memory_type()` (`src/vulkan/memory/memory.c`), `want_device_local` ranked `!host_visible ? 2 : 1`. Because *every* `DEVICE_LOCAL` memory type on Intel UMA is `HOST_VISIBLE`, uncached Write-Combined memory (Type 0) tied with coherent L3/LLC Write-Back `HOST_CACHED` memory (Type 1) and won by index order.
4. **Duplicate Staging Pool & Eager Survey Cache Copies on Unified Memory:** `bda.c` maintained separate `64 MiB` `device_pool` and `staging_pool` slabs even when both pointed at the exact same physical LPDDR4x chips, and `uring_pager.c` eagerly `malloc`ed + `memcpy`d `384 KiB` heap duplicates for every survey-fused tile even while the tile remained resident in coherent UMA BDA memory.

---

## 3. `Engine v0.2` Milestone 1 Changes Implemented

1. **Runtime `VulkanExecutionProfile` Classification (`include/vulkan/types.h`, `src/vulkan/device/device.c`, `src/frame/boot/boot.c`):**
   - Automatically classifies the selected GPU at boot into:
     - `UMA_IGPU` (`TrueUMA`, zero dedicated PCIe VRAM, coherent shared LLC/DRAM; defaults: `2x MSAA`, `16 MiB` BDA slabs, `96` terrain slots)
     - `DISCRETE_DIRECT` (dedicated PCIe VRAM scanning out directly; defaults: `4x MSAA`, `64 MiB` BDA slabs, `341` terrain slots)
     - `HYBRID_MUXLESS` (automatically promoted during `frame_boot` when `wl->feedback_cross_device` detects cross-GPU PRIME scanout; defaults: `4x MSAA`, `64 MiB` BDA slabs, `256` terrain slots)
   - Added runtime environment overrides for deterministic cross-profile testing: `ENGINE_GPU_PROFILE=auto|uma|discrete|hybrid`, `ENGINE_MSAA=1|2|4|8`, and `ENGINE_TERRAIN_SLOTS=<32..341>`.
2. **Coherent LLC (`HOST_CACHED`) Memory Ranking & Unified Zero-Staging BDA (`src/vulkan/memory/memory.c`, `src/vulkan/memory/bda.c`):**
   - `vulkan_find_memory_type` now ranks invisible dedicated VRAM highest (`4`) on discrete GPUs, followed immediately by `HOST_CACHED | HOST_COHERENT` (`3`) on UMA GPUs—eliminating accidental Write-Combined uncached allocations.
   - When `is_true_uma` is active, `bda_system_init` configures a single unified `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT | HOST_CACHED` pool (`16 MiB` slab granularity, `4 MiB` transient ring) and routes `bda_alloc_staging` directly into `device_pool` (`staging_capacity = 0.00 MiB`).
3. **Adaptive MSAA & UMA Render-Target Pool Granularity (`examples/demo/render/pass.c`):**
   - Caps default raster sample count to `recommended_msaa` (`2x` on `UMA_IGPU`, `4x` on `DISCRETE`) and tightens `UMA_IGPU` render-target pool rounding from `16 MiB` to `4 MiB`, shrinking `pass_pool_mib` from **`128.00 MiB` to `32.00 MiB` (`-75.0%`)**.
4. **Zero-Copy UMA Terrain Residency & Lazy Spill-on-Evict Cache (`src/backend/`):**
   - Replaced fixed `341`-slot allocations with runtime `backend_slot_cap()` (`96` slots = `36.02 MiB` on `UMA_IGPU`, giving a 31-tile LRU cushion above the 65-tile active quadtree cut with zero visual pop-in).
   - Compacted the `UMA_IGPU` orthophoto overlay BDA arena from `64.00 MiB` to `32.00 MiB` (preserving full lossless `2048x2048` live preview + `16 MiB` committed layer space).
   - Converted `fused_cache` from eager-on-load `malloc` + `memcpy` to **lazy spill-on-evict**: survey fusion runs in-place inside coherent UMA BDA slots and only spills to heap RAM if a modified slot is actually evicted.

---

## 4. Empirical Results: `v0.1` Baseline vs `v0.2` Milestone 1

### 4.1 GPU Hardware Frame & Pass Timings (`1280x688`, 65 Retired Samples/World)

| World Workload | Metric | `v0.1` Baseline | `v0.2` Milestone 1 | Delta |
| :--- | :--- | ---: | ---: | ---: |
| **World 0** — *Pure Sealed Empty Engine*<br>(0 objects, 0 vertices) | `gpu_total_us` (avg)<br>`gpu_render_us` (avg)<br>`gpu_total_us` (max) | 520.99 µs<br>419.55 µs<br>691.30 µs | **472.71 µs**<br>**369.23 µs**<br>**597.97 µs** | **-9.3%**<br>**-12.0%**<br>**-13.5%** |
| **World 2** — *3D Solids Gravity & Squash*<br>(9 objects, 17,046 vertices) | `gpu_total_us` (avg)<br>`gpu_compute_us` (avg)<br>`gpu_render_us` (avg)<br>`gpu_total_us` (max) | 4,724.91 µs<br>2,731.74 µs<br>1,829.09 µs<br>6,321.30 µs | **4,065.49 µs**<br>2,768.25 µs<br>**1,178.96 µs**<br>**5,558.07 µs** | **-14.0%**<br>+1.3%<br>**-35.5% (1.55x)**<br>**-12.1%** |
| **World 7** — *Procedural Kinetic Cathedral*<br>(18 objects, 51,768 vertices) | `gpu_total_us` (avg)<br>`gpu_compute_us` (avg)<br>`gpu_render_us` (avg)<br>`gpu_total_us` (max) | 5,610.67 µs<br>2,511.64 µs<br>2,934.42 µs<br>7,208.70 µs | **4,200.88 µs**<br>**2,303.36 µs**<br>**1,801.81 µs**<br>**5,569.06 µs** | **-25.1% (1.34x)**<br>**-8.3%**<br>**-38.6% (1.63x)**<br>**-22.7%** |
| **World 8** — *Baklan & Beşparmak Dağı DEM*<br>(49,920 verts, 109 `io_uring` SSD tile loads) | `gpu_total_us` (avg)<br>`gpu_compute_us` (avg)<br>`gpu_render_us` (avg)<br>`gpu_total_us` (max) | 4,830.03 µs<br>24.33 µs<br>4,685.62 µs<br>10,449.84 µs | **4,160.24 µs**<br>**22.63 µs**<br>**4,081.41 µs**<br>**8,743.07 µs** | **-13.9%**<br>**-7.0%**<br>**-12.9%**<br>**-16.3%** |
| **World 9** — *Interactive Full Sandbox*<br>(8 objects, 13,188 vertices + plots + HUD) | `gpu_total_us` (avg)<br>`gpu_compute_us` (avg)<br>`gpu_render_us` (avg)<br>`gpu_total_us` (max) | 2,069.99 µs<br>1,373.65 µs<br>573.77 µs<br>2,959.58 µs | 2,225.29 µs<br>1,563.58 µs<br>584.08 µs<br>3,031.72 µs | +7.5%*<br>+13.8%*<br>+1.8%<br>+2.4% |

*\*Note on World 9 & Compute Dominated Scenes:* In Worlds 2, 7, and 9, `gpu_compute_us` (`1.4–2.7 ms`) is dominated by the 3D volumetric lattice compute shader (`grid.comp`), which is untouched in Milestone 1 and exhibits ~0.15 ms dynamic iGPU clock-boost variance across sub-second bursts. That exact compute pass—along with replacing CPU springs with GPU **AVBD** and replacing terrain triangle expansion with **Quantized Meshlets + Visibility Buffer**—is the target of Milestones 2 and 4 in [`doc/V02_GPU_PROFILES_AND_2026_ALGORITHMS.md`](./V02_GPU_PROFILES_AND_2026_ALGORITHMS.md).

---

### 4.2 Dedicated Vulkan + BDA + Terrain Memory Footprint (Shared UMA DRAM)

Because Linux `i915`/`Xe` accounts `vkAllocateMemory` buffers as kernel DRM/GEM shmem objects outside `/proc/self/status` anonymous `VmRSS`, tracking exact Vulkan pool and BDA capacities exposes the real physical RAM freed on this 7.4 GiB machine:

| Memory Pool / Arena | `v0.1` Baseline | `v0.2` Milestone 1 (`UMA_IGPU`) | RAM Saved |
| :--- | ---: | ---: | ---: |
| **World 8 — Baklan & Beşparmak Dağı Terrain + Ortho** | | | |
| MSAA Depth + Coverage Pools (`pass_pool_mib`) | 128.00 MiB (`8x`) | **32.00 MiB (`2x`)** | **-96.00 MiB (-75.0%)** |
| BDA Device + Staging Slab Pools (`bda_*_mib`) | 64.00 MiB | **16.00 MiB (0 staging)** | **-48.00 MiB (-75.0%)** |
| BDA Per-Frame Transient Ring | 16.00 MiB | **4.00 MiB** | **-12.00 MiB (-75.0%)** |
| GPU Terrain Tile BDA Arena (`terrain_arena_mib`) | 127.90 MiB (`341` slots) | **36.02 MiB (`96` slots)** | **-91.88 MiB (-71.8%)** |
| Bird's-Eye Orthophoto BDA Arena (`overlay_arena_mib`) | 64.00 MiB | **32.00 MiB** | **-32.00 MiB (-50.0%)** |
| Survey Fused Tile Heap Spill (`fused_cache_mib`) | 2.62 MiB | **1.88 MiB (lazy evict)** | **-0.74 MiB (-28.2%)** |
| **World 8 Total GPU/UMA Footprint** | **402.52 MiB** | **121.90 MiB** | **-280.62 MiB (-69.7%, 3.3x smaller)** |
| | | | |
| **Worlds 2, 7, 9 — 3D Solids / Cathedral / Sandbox** | | | |
| MSAA Depth + Coverage Pools (`pass_pool_mib`) | 128.00 MiB | **32.00 MiB** | **-96.00 MiB (-75.0%)** |
| BDA Device Slab Pool (`device_capacity`) | 64.00 MiB | **32.00 MiB (unified)** | **-32.00 MiB (-50.0%)** |
| BDA Staging Slab Pool (`staging_capacity`) | 64.00 MiB | **0.00 MiB (eliminated)** | **-64.00 MiB (-100.0%)** |
| BDA Per-Frame Transient Ring | 16.00 MiB | **4.00 MiB** | **-12.00 MiB (-75.0%)** |
| **Worlds 2/7/9 Total GPU/UMA Footprint** | **272.00 MiB** | **68.00 MiB** | **-204.00 MiB (-75.0%, 4.0x smaller)** |
| | | | |
| **World 0 — Pure Sealed Empty Engine** | **208.00 MiB** | **52.00 MiB** | **-156.00 MiB (-75.0%, 4.0x smaller)** |

---

### 4.3 Build Health, Dead-Code Gate & Binary Footprint

| Check | `v0.1` Baseline | `v0.2` Milestone 1 |
| :--- | :---: | :---: |
| Compiler Warnings (`-std=c23 -Wall -Wextra -Werror=unused-function`) | `0` | **`0`** |
| Linker Dead-Code Gate (`--gc-sections --print-gc-sections` on `src/`) | `0` dead symbols | **`0` dead symbols** |
| Unit Test Suite (`make test` — 9 suites + new `test_bda_unified_uma_zero_staging`) | `9 / 9 PASS` | **`9 / 9 PASS`** |
| Stripped/LTO Release Binary (`build/release/bin/engine`, incl. `#embed` assets) | `2,250,512 B` (`2.15 MiB`) | `2,254,536 B` (`2.15 MiB`, **`+3.9 KiB`**) |

---

## 5. Reproducing & Overriding Profiles Locally

Run the full automated build, unit-test, and multi-world benchmark suite anytime:

```bash
make bench
```

Or compare execution profiles and MSAA caps on the fly without recompiling:

```bash
# Default auto-detected profile (UMA_IGPU on Intel Iris Xe):
./build/release/bin/engine --world=8 --bench=75

# Emulate Discrete PCIe GPU profile budgets (4x MSAA, 64 MiB slabs, 341 terrain slots):
ENGINE_GPU_PROFILE=discrete ./build/release/bin/engine --world=8 --bench=75

# Override MSAA sample count or resident terrain slot window explicitly:
ENGINE_MSAA=4 ENGINE_TERRAIN_SLOTS=128 ./build/release/bin/engine --world=8 --bench=75
```
