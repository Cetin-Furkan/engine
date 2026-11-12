# Engine (`v0.3.0`)

**Pure ISO C23 (`-std=c23`) · Linux 7.1+ `io_uring` · Raw Wayland Wire · DRM Timeline Explicit Sync · Vulkan 1.4 BDA + Push Descriptors · Slang 2026 (SPIR-V 1.6)**

`Engine` is a 3D, 2D, and geospatial simulation/graphics platform written from scratch in strict ISO C23 and Slang 2026. A clean `make release` builds two standalone executables from the same `src/` platform core:
- **`build/release/bin/engine` (`3.02 MiB` / `3,163,432 B`)**: Full 10-world 3D/2D/Hybrid & geodetic showcase (`examples/demo/`), with all **`42` SPIR-V 1.6 shader modules** in `build/release/gen/` and the SDF font `#embed`ded directly inside the executable.
- **`build/release/bin/minimal` (`0.42 MiB` / `438,192 B`)**: Pure-seam standalone application (`examples/minimal/`, `2` `#embed`ded SPIR-V 1.6 modules) supplying **only** the 7 `[REQUIRED]` hooks in [`include/engine/app.h`](include/engine/app.h), advancing on the issued `8 ms` simulation cursor, drawing a single transient BDA vertex buffer, handling `click` + `key`, settling into `Loop idle`, and containing zero demo, terrain, scene, physics, lighting, or FreeType symbols.

It uses **no windowing libraries** (`SDL`, `GLFW`), **no protocol wrappers** (`libwayland-client`), **no ring wrappers** (`liburing`), **no Vulkan WSI swapchain** (`VK_KHR_swapchain`), and **zero `VkDescriptorPool` / `VkDescriptorSet` allocations**. A single thread talks straight to the Linux kernel over a registered `io_uring` instance, speaks the Wayland binary wire protocol over a `16 KiB` packet ring, presents exported `DMA-BUF` images synchronized with Linux DRM `drm_syncobj` timelines (polling GPU retirement with `timeout = 0` — zero blocking waits on the frame-thread success path), addresses structured GPU buffers via raw 64-bit GPU pointers (`VK_KHR_buffer_device_address`), and binds sampled depth/visibility views inline through one `VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT` layout (`vkCmdPushDescriptorSet`).

> **Measured Release Hardware (`v0.3.0`)**: All release figures, benchmarks, and [`doc/IDLE_VERIFICATION_LOG.txt`](doc/IDLE_VERIFICATION_LOG.txt) were measured on **Intel(R) Iris(R) Xe Graphics (`TGL GT2`, 11th Gen Intel Core i7-1165G7, Mesa 26.2.6, Vulkan 1.4.354, Linux 7.3)** running the `UMA_IGPU` profile. `DISCRETE_DIRECT` and `HYBRID_MUXLESS` remain active execution profiles in `src/` (exercised via `ENGINE_GPU_PROFILE=discrete|hybrid` overrides on this host; separate native discrete/hybrid hardware logs are not attached to this commit).

---

## Documentation Map

| Document | What It Covers |
| :--- | :--- |
| **[`doc/PLATFORM_AND_RENDER_INTERNALS.md`](doc/PLATFORM_AND_RENDER_INTERNALS.md)** | Low-level walkthrough of `src/`: `io_uring` arena flags & `EngineRelay`, `16 KiB` Wayland wire packet ring, WSI-free `DMA-BUF` + DRM syncobj swapchain, `timeout = 0` timeline polling, 4-slice transient BDA ring, `#embed`ded Slang 2026 pipelines, and the `--print-gc-sections` dead-code gate. |
| **[`doc/WORKFLOW_CONTROLS_AND_EXTENSIBILITY.md`](doc/WORKFLOW_CONTROLS_AND_EXTENSIBILITY.md)** | `EngineApp` (`include/engine/app.h`: 7 `[REQUIRED]` + 24 `[OPTIONAL]` hooks), `examples/minimal/` vs `examples/demo/`, keyboard/mouse routing, capability masking (`PolicyMask` & sealing), `F1`–`F10` world switching, `.world` / `sheet.world` / `.manifest` / `window.chrome` DSLs, and C23 `WorldBuilder`. |
| **[`TERRAIN_BACKEND_ARCHITECTURE.md`](TERRAIN_BACKEND_ARCHITECTURE.md)** | World 8's $24\times 24\text{ km}$ Baklan & Beşparmak Dağı `.epack`/`.gmpack` streaming backend, `RelPtr2D` packed tile-relative offsets, 64-bit tile headers, 64-byte `GpuMathStreamCell` host closures, GPU `vkCmdDrawIndirectCount` culling, Brown-Conrady calibration, MVS, and WGS84 fusion. |
| **[`doc/ARCHITECTURE_COMPARISON.md`](doc/ARCHITECTURE_COMPARISON.md)** | Concrete layer-by-layer comparison of `Engine` (`v0.3.0`) against Unreal/Unity/Godot, `SDL3`/`GLFW` + `VkSwapchainKHR` stacks, and `GDAL` + `Cesium` + offline photogrammetry pipelines—including honest trade-offs. |
| **[`doc/V02_GPU_PROFILES_AND_2026_ALGORITHMS.md`](doc/V02_GPU_PROFILES_AND_2026_ALGORITHMS.md)** | Dual-architecture profile specification (`UMA_IGPU`, `DISCRETE_DIRECT`, `HYBRID_MUXLESS`) and implemented mathematical kernels (`.epack` v1, quantized meshlets, `SSILVB`, Split `HRC`, single-owner GPU `AVBD`, tangent-quad `2DGS`). |
| **[`doc/IDLE_VERIFICATION_LOG.txt`](doc/IDLE_VERIFICATION_LOG.txt)** | Measured `v0.3.0` Wayland boot & `Loop idle` logs for both `build/release/bin/minimal` and `build/release/bin/engine --world=0` on Intel Iris Xe (`TGL GT2`), verifying `sim_timer=0` and zero GPU submits after `Loop idle`. |
| **[`doc/BENCHMARK_V01_VS_V02_M1.md`](doc/BENCHMARK_V01_VS_V02_M1.md)** .. **[`doc/BENCHMARK_V02_M5.md`](doc/BENCHMARK_V02_M5.md)** | Milestone-by-milestone empirical progression reports (`M1` through `M5`) measured on Intel Iris Xe (`TGL GT2`). |
| **[`pointers.md`](pointers.md)** | Layout reference for `RelPtr2D` (packed 64-bit tile offset), `GpuMathTileHeader` (64-bit compact node header), and `GpuMathStreamCell` (64-byte host closure). |
| **[`doc/engineers_note.md`](doc/engineers_note.md)** | Build segregation (`build/debug` vs `build/release`), semantic version tagging, and Vulkan/DRM runtime contracts. |

---

## Table of Contents

1. [Why We Built Directly on Linux 7.1+ and Vulkan 1.4](#1-why-we-built-directly-on-linux-71-and-vulkan-14)
2. [How `Engine` Compares to Other Engines & Stacks](#2-how-engine-compares-to-other-engines--stacks)
3. [How the Platform Works (`src/` vs Hosted Programs)](#3-how-the-platform-works-src-vs-hosted-programs)
4. [What `v0.3.0` Delivers in This Binary](#4-what-v030-delivers-in-this-binary)
5. [Roadmap (`v0.4+` — Not in `v0.3.0`)](#5-roadmap-v04--not-in-v030)
6. [Quick Start: Build, Test, Run & GPU Selection](#6-quick-start-build-test-run--gpu-selection)
7. [Selectable Worlds (`--world=0..9` & Live `F1`–`F10` Switching)](#7-selectable-worlds---world09--live-f1f10-switching)
8. [Interactive Controls Reference](#8-interactive-controls-reference)
9. [Repository Directory Layout](#9-repository-directory-layout)

---

## 1. Why We Built Directly on Linux 7.1+ and Vulkan 1.4

Conventional Linux graphics stacks pass every frame through five or six general-purpose layers (`App -> Scene Graph -> SDL3/GLFW -> libwayland-client -> epoll/threads -> VkSwapchainKHR/vkWaitForFences -> VkDescriptorSet`). Together they introduce continuous idle wakeups, hidden `vkWaitForFences` / `vkDeviceWaitIdle` stalls during resize and upload, descriptor-pool bookkeeping, and pointer serialization copies between SSD, CPU RAM, and GPU VRAM.

`Engine` talks directly to Linux 7.1+ (`io_uring` + DRM `drm_syncobj` + `DMA-BUF`) and Vulkan 1.4 (`VK_KHR_buffer_device_address` + `VK_KHR_maintenance5` + inline push descriptors):
- **Zero-wakeup `Loop idle`**: When motion settles and keys are released, `sim_timer_set(ring, false)` cancels the kernel multishot `IORING_OP_TIMEOUT`. Once the final dirty frame and Wayland wire send retire, `[Engine] Loop idle.` prints with `sim_timer=0` and **zero** further GPU submits occur until external input or `inotify` arrives.
- **Zero blocking waits on the success path**: `bda_transient_begin_frame`, staged `scene_gpu` DMA uploads, and `orphan` slot retirement poll `vkGetSemaphoreCounterValue` with `timeout = 0`. Blocking drains (`vkDeviceWaitIdle`) run only on shutdown, on a real resize of the GI or visibility-buffer attachments, or after a failed DMA submit.
- **Single-owner GPU pose**: In GPU `AVBD` mode (`KEY_8`), the CPU builds the CSR constraint graph and greedy coloring without integrating positions; `physics_avbd.comp.slang` solves into mapped BDA ring buffers, and the CPU copies solved positions back on a later frame only after `vkGetSemaphoreCounterValue` (`timeout = 0`) confirms retirement (1-frame latency).

---

## 2. How `Engine` Compares to Other Engines & Stacks

| Area | Typical Engine (`UE5` / `Unity` / `Godot`) | Custom Native (`SDL3`/`GLFW` + Vulkan WSI) | **`Engine` (`v0.3.0`)** |
| :--- | :--- | :--- | :--- |
| **Compiled Footprint** | `80 MiB` – `500+ MiB` + external shader/pak folders | `5 MiB` – `30 MiB` + wrapper `.so`s + external `.spv`s | **`3.02 MiB` (`engine`, `42` `.spv` `#embed`ded)** · **`0.42 MiB` (`minimal`, `2` `.spv`)** |
| **Idle Resource Usage** | Continuous tick loop (`60–240 Hz` CPU/GPU wakeups) | Poll loop or coarse event wait | **`0.0%` CPU & `0.0%` GPU**. Cancels multishot `IORING_OP_TIMEOUT`, logs `Loop idle` (`sim_timer=0`) |
| **OS I/O & Timers** | Thread pools + `epoll`/`poll` + mutexes/`futex` | `poll()` on socket + blocking file `read()` | **Single-issuer `io_uring`** (`NO_MMAP`, `DEFER_TASKRUN`, `REGISTERED_FD_ONLY`, `1 ns` timer slack) |
| **Windowing & Swapchain** | Wrapper $\to$ `libwayland` + `VkSwapchainKHR` | `libwayland` + `VkSwapchainKHR` + `vkWaitForFences` | **Hand-rolled `16 KiB` Wayland wire ring + exported `DMA-BUF` + `wp_linux_drm_syncobj_v1` timelines** |
| **Window Titlebar** | XWayland / `libdecor` (`wl_shm` CPU software copy) | Compositor SSD or `libdecor` `wl_shm` | **GPU fragment shader into its own `DMA-BUF` on a desynced `wl_subsurface`** |
| **GPU Resource Binding** | Descriptor pools / global bindless descriptor heaps | `VkDescriptorSet` + `vkUpdateDescriptorSets` | **`0` `VkDescriptorPool`/`Set`s**. 64-bit `VkDeviceAddress` (`BDA`) + 1 `PUSH_DESCRIPTOR_BIT` layout (`vkCmdPushDescriptorSet`) |
| **Spatial Data Pointers** | Host `void*` + deserialization & pointer fixup passes | Separate disk, CPU, and GPU data structures | **`RelPtr2D` (packed 64-bit tile offset)** + 64b tile headers + 64B `GpuMathStreamCell` host closures |
| **Dead Code Policy** | Millions of lines of unexercised engine branches | Unused helpers silently kept or stripped | **Hard linker gate**: build fails (`[Link Error]`) if *any* function in `src/` is unused in `bin/engine` |

---

## 3. How the Platform Works (`src/` vs Hosted Programs)

### One Null-Safe Seam (`include/engine/app.h`)
The repository is separated across [`EngineApp`](include/engine/app.h):
- **Platform Core (`main.c`, `src/{system,ring,wayland,vulkan,input,chrome,frame}`)**: Owns CPU/`io_uring` probing, the `16 KiB` Wayland packet ring, `DMA-BUF` + DRM syncobj swapchain (`64`-slot generation-tagged orphan cap), Vulkan 1.4 BDA pools (`4`-slice non-blocking transient ring + coalescing free-list), `FrameClock` (`8 ms` quanta, `FRAME_SIM_MAX_STEPS = 5`), and hardware GPU timestamp queries (`vkResetQueryPool`).
- **Optional First-Class GPU/Math Subsystems (`src/{backend,scene,lighting,physics}`)**: Linked only by programs that call them (`bin/engine`), and stripped cleanly (`0` symbols) from pure-platform programs (`bin/minimal`).
- **Two Hosted Programs**:
  1. **`examples/minimal/` (`build/<mode>/bin/minimal`)**: Supplies **only** the **7 `[REQUIRED]` hooks** (`init`, `shutdown`, `draw`, `advance`, `busy`, `click`, `key`), leaving all **24 `[OPTIONAL]` hooks** null to exercise platform defaults (`1`-sample `B8G8R8A8_SRGB` pass, `bda_transient_begin_frame`/`abort_frame`, default tap routing, and `Loop idle`).
  2. **`examples/demo/` (`build/<mode>/bin/engine`)**: Supplies the 10 worlds (`--world=0..9`), `WorkspacePolicy`, `WorldBuilder`, SDF text, and full 3D/2D/Hybrid + World 8 terrain/survey integration.

### Non-Blocking Failure Contracts
- **Wayland `16 KiB` Send Ring**: If `WaylandContext::tx_ring` (`ENG_WL_SEND_RING_CAP = 16,384 B`, `64` packets) is full, `wayland_queue_send` / `tx_enqueue_packet` fails (`-ENOSPC`) without dropping or corrupting already-queued packets (and `wayland_put_raw` returns `false` if the `4,096 B` `send_bytes` request stage fills).
- **Transient BDA Ring (`4` Slices)**: If a requested transient slice exceeds capacity or its timeline point has not yet retired (`timeout = 0` poll), `bda_alloc_transient` returns a null `BdaSlice` (`cpu == nullptr, gpu == 0`); callers skip that transient draw and the frame still presents.
- **Orphan Table Cap (`64` Slots)**: At `k_orphan_max_cap = 64`, `make_orphan_room` refuses a new slot (`-1`) immediately without blocking and without sliding existing `SlotRef` handles.
- **Device Lost (`VK_ERROR_DEVICE_LOST`)**: `vulkan_report_device_fault` logs BDA telemetry and queries `VK_EXT_device_fault` diagnostics when enabled, then returns cleanly without hanging so `frame_submit_slot_commands` fails fast.

---

## 4. What `v0.3.0` Delivers in This Binary

- **Pure Empty Boot (`World 0`) & Second Hosted Program (`bin/minimal`)**: `./build/release/bin/engine` boots `World 0` with zero objects and `0.00 MiB` lazy GI/physics/terrain allocations (`3` submits -> `Loop idle`), while `./build/release/bin/minimal` exercises the issued `8 ms` sim cursor, 1 transient BDA draw, `click`/`key`, and `Loop idle` in a `0.42 MiB` binary with zero demo/FreeType/terrain/physics/lighting symbols.
- **Hardware-Adaptive Execution Profiles (`UMA_IGPU` · `DISCRETE_DIRECT` · `HYBRID_MUXLESS`)**: Measured on **Intel Iris Xe (`TGL GT2`)** under `UMA_IGPU` (`40 MiB` unified `HOST_CACHED` LLC BDA slab, `2x MSAA` default). `DISCRETE_DIRECT` (`64 MiB` VRAM slabs, `4x MSAA`) and `HYBRID_MUXLESS` (partial-damage `prime_copy`) remain selectable via `ENGINE_GPU_PROFILE`.
- **Quantized `16 B/vert` Meshlets, Subgroup `Hi-Z` & `R32_UINT` Visibility Buffer (`src/scene/`)**: `64`-vertex / `124`-triangle meshlets (`uint16` local AABB + octahedral `snorm16` normals), non-blocking staged DMA uploads, Subgroup `Hi-Z` + Two-Phase `vkCmdDrawIndirectCount` culling, and `Y`-key toggle between MSAA indirect and `1x R32_UINT` Visibility Buffer deferred shading (`1` push-descriptor layout + `vkCmdPushDescriptorSet`).
- **`SSILVB` 32-Bit Visibility Bitmasks & 4-Level Split Radiance Cascades GI (`src/lighting/`)**: Descriptor-pool-free BDA Global Illumination (`shade_surface_gi` across cubes, terrain, meshlets, and surfels). At **`1280×720`**, [`lighting_compute_hrc_rays()`](include/lighting/lighting.h) (`ensure_gi_slab`) traces **`107,520` rays** on `UMA_IGPU` (`640×360` GI, `160×90` C0 probes) and **`431,360` rays** on `DISCRETE_DIRECT` (`960×540` GI, `320×180` C0 probes; `102,400` and `412,160` rays on the `1280×688` client area below the `32 px` chrome bar). Live `H`-key toggle.
- **Single-Owner GPU `AVBD` Physics (`src/physics/`)**: Coupled 3D rigid + 2D soft-body Augmented Vertex Block Descent ($3 \times 3$ $L D L^\top$ register solver + CSR graph coloring + fused `groupshared` SLM compute in `physics_avbd.comp.slang`). In GPU mode, the GPU buffer is the sole owner of integrated positions, read back on a later frame after `timeout = 0` timeline retirement (`1`-frame latency; `KEY_8` switches between GPU owner and CPU fallback).
- **$24\times 24\text{ km}$ `.epack` DEM Terrain, Compute `ZNCC` MVS & Tangent-Quad `2DGS` Surfels (`World 8`, `src/backend/`)**:
  - Committed `examples/demo/maps/baklan_besparmak.gmpack` (`58,601,472 B` / `55.89 MiB`, 59 MB decimal) transcodes to `33.32 MiB` `.epack` v1 (`341` quadtree tiles $\times$ `100 KiB`, addressed by `RelPtr2D` packed tile offsets and `64 B` `GpuMathStreamCell` host closures) and streams asynchronously via `io_uring` (`0 µs` render stall), with offline C23 self-synthesis if absent.
  - Refines tiles via Brown-Conrady calibration, Slang compute plane-sweep `ZNCC` MVS (`survey_mvs.comp.slang`), and WGS84 Wendland $C^2$ RBF, and renders steep cliffs/survey beacons as **32-byte oriented tangent quads shaded with an `exp(-0.5 r²)` Gaussian alpha falloff** (`survey_2dgs.{vert,frag}.slang`, `WavePrefixCountBits` culling, live `KEY_7` toggle).

---

## 5. Roadmap (`v0.4+` — Not in `v0.3.0`)

The following capabilities are **future `v0.4+` work** and are **not** claimed as part of `v0.3.0`:
1. **Hardware `TLAS`/`BLAS` `VK_KHR_ray_query` Scene Tracing**: `device.c` probes capability bits, but `v0.3.0` traces analytical `LightingProxy` primitives, `Hi-Z`, and `.epack` heightfields—no `VkAccelerationStructureKHR` scene is built yet.
2. **General glTF / PBR Meshlet Archive on `RelPtr2D`**: Today `RelPtr2D` streams `.epack`/`.gmpack` terrain tiles; `.obj` meshes load through `examples/demo/`.
3. **Zero-Copy `io_uring` Networking, Compute Audio & Secondary Worker Rings**: `v0.3.0` runs strictly on a single `io_uring` instance with local file/socket/timer I/O.
4. **Native Discrete & Muxless Hardware Benchmark Logs**: Collecting commit-pinned telemetry logs on physical discrete NVIDIA/AMD and muxless laptop setups alongside this Intel Iris Xe (`TGL GT2`) baseline.

---

## 6. Quick Start: Build, Test, Run & GPU Selection

### System Prerequisites
- **OS & Session**: Linux kernel `7.1+` (measured on `7.3`) inside an active Wayland session (`$WAYLAND_DISPLAY` with `zwp_linux_dmabuf_v1` and `wp_linux_drm_syncobj_v1`).
- **Toolchain**: `gcc` (`>= 14`, `-std=c23` + `#embed`), `slangc` (SPIR-V 1.6), Vulkan 1.4 loader & headers, `xkbcommon` (platform core), plus `freetype2`, `harfbuzz`, `fontconfig`, and `libpng` (`examples/demo/` & `src/backend/`).

### Build & Verify
```bash
# 1. Build debug binaries (build/debug/bin/engine + build/debug/bin/minimal)
make

# 2. Build LTO release binaries (build/release/bin/engine [3.02 MiB] + build/release/bin/minimal [0.42 MiB])
make release

# 3. Build only the standalone minimal EngineApp target
make minimal

# 4. Run all 16 automated unit, failure-path, symbol-purity, doc-truth & Loop-idle test suites
make test
```

### Run & Select GPUs
```bash
# Run the standalone minimal EngineApp program (7 required hooks, BDA draw, Loop idle)
./build/release/bin/minimal

# Default demo boot: Pure Empty Engine (World 0 — blank viewport, sealed policy)
./build/release/bin/engine

# List and launch built-in demo worlds (0..9)
./build/release/bin/engine --list-worlds
./build/release/bin/engine --world=8               # Baklan & Beşparmak Dağı DEM + Survey Map
./build/release/bin/engine --world=5               # Hybrid 3D + 2D Soft-Body & Draw Sandbox

# Override execution profile, MSAA, VisBuf, Hi-Z, GI, AVBD physics, or 2DGS survey policy
ENGINE_GPU_PROFILE=auto|uma|discrete|hybrid ./build/release/bin/engine --world=8
ENGINE_MSAA=1 ENGINE_VISBUF=1 ENGINE_HIZ=auto ENGINE_GI=auto|1|0 ./build/release/bin/engine --world=7
ENGINE_PHYSICS=auto|gpu|cpu ./build/release/bin/engine --world=2
ENGINE_2DGS=auto|1|0 ./build/release/bin/engine --world=8
```

---

## 7. Selectable Worlds (`--world=0..9` & Live `F1`–`F10` Switching)

Press **`F1`–`F9`** inside `bin/engine` to switch straight into Worlds `1..9`, or **`F10` / `0`** to return to `World 0` in place without recreating the Vulkan device:

| World Flag | Alias | Mode | `Tab` Switch | `PERM_DRAW` | What It Demonstrates |
| :--- | :--- | :--- | :---: | :---: | :--- |
| `--world=0` | `pure` / `empty` | `MODE_3D` | **Sealed** | Off | **Pure Empty Engine (Default)**: Zero 3D/2D objects, no grid, no gimbal, no HUD, sealed `PERM_NONE` policy. |
| `--world=1` | `draw3d` | `MODE_3D` | Locked | **ON** | **3D Click-to-Draw Studio**: Ground clicks place vertices; clicking the first corner or pressing `F` ear-clips a 3D face. |
| `--world=2` | `gravity3d` | `MODE_3D` | Locked | Off (`KP9`) | **3D Gravity & Elastic Solids**: 6 solid kinds and cubes under single-owner GPU `AVBD` physics (`KEY_8` toggles GPU/CPU owner). |
| `--world=3` | `soft2d` | `MODE_2D` | Locked | Off (`KP9`) | **2D Soft Bodies & Polygons**: 3 deformable `AVBD` soft patches (`spring=22, 58, 145`) and SAT convex polygons. |
| `--world=4` | `draw2d` | `MODE_2D` | Locked | **ON** | **2D Polygon Workshop**: Open 2D ear-clipped drawing canvas surrounded by live soft-body patches. |
| `--world=5` | `hybrid` | `MODE_HYBRID` | Open | **ON** | **Hybrid 3D + 2D Sandbox**: Simultaneous 3D falling solids, 2D soft patches, and click-to-draw faces. |
| `--world=6` | `bounce` | `MODE_3D` | Open | Off (`KP9`) | **Restitution Staircase & Bumper Lab**: Graded restitution lineup (`0.14..0.92`) paired with a 2D bumper arena. |
| `--world=7` | `procedural` | `MODE_HYBRID` | Open | **ON** | **Procedural C23 Cathedral**: Built purely via `WorldBuilder` (`builder.c`) with zero external `.world` files. |
| `--world=8` | `besparmak` / `map` | `MODE_3D` | Locked | Survey 2-Pt | **Baklan & Beşparmak Dağı $24\times 24\text{ km}$ DEM & Survey**: 341-tile `io_uring` `.epack` terrain, compute MVS, and tangent-quad `2DGS` surfels. |
| `--world=9` | `sandbox` | `MODE_3D` | Open | Off (`KP9`) | **Full Interactive Sandbox**: Persistent `live.world`, `sheet.world`, 3D math/CSV plots, and HarfBuzz/FreeType SDF text. |

---

## 8. Interactive Controls Reference

| Input | `bin/minimal` | `bin/engine` (`MODE_3D` / `MODE_2D` / `MODE_HYBRID`) |
| :--- | :--- | :--- |
| **Left Click** | Move BDA prism to `(origin_x, origin_y)` + `24`-tick spin | Pick/shove object, place polygon vertex (`PERM_DRAW`), or set World 8 geodesic points |
| **`Space` / `1..4`** | Cycle prism palette + `24`-tick spin | `1..6` spawn/select 3D solids (`MODE_3D` / `MODE_HYBRID`) |
| **`R` / `Home`** | Recenter prism to `(0, 0)` + `24`-tick spin | `R` toggles continuous spin; `Home` / `KP1` restores opening camera |
| **`Esc` / `Q`** | Cleanly exit loop (`*ctx->stop = true`) | `Esc` cancels/clears; `Q`/`E` & `C`/`V` orbit camera yaw & pitch |
| **`F1..F9` / `F10`, `0`** | — | Switch live to World `1..9` / `World 0` |
| **`Y` / `H` / `7` / `8`** | — | Toggle `R32_UINT` VisBuf (`Y`), `SSILVB`+`HRC` GI (`H`), `2DGS` surfels (`7`), GPU/CPU `AVBD` owner (`8`) |
| **`K` / `M` / `L` / `T`** | — | Contours (`K`), cycle math/CSV plot (`M`), hot-reload files (`L`), place SDF text box (`Hold T` + 2 clicks) |

---

## 9. Repository Directory Layout

```text
main.c               Process entrypoint: CLI configure, signal block, ordered boot & reverse-LIFO shutdown
Makefile             Segregated debug/release build, libengine.a, Slang->SPIR-V 1.6 #embed, dead-code gate
VERSION              Semantic version string (0.3.0)
chrome/              Declarative window.chrome specification for the GPU subsurface titlebar
doc/                 Architecture guides, benchmark reports & measured IDLE_VERIFICATION_LOG.txt

include/
├── backend/         RelPtr2D packed tile offsets, 64B GpuMathStreamCell closures, .epack & survey APIs
├── chrome/          Window bar layout, layer/control spec & GPU pass headers
├── engine/          app.h — 7 [REQUIRED] + 24 [OPTIONAL] null-safe hooks between platform and program
├── frame/           64B FrameClock, SlotRef generation handles & frame subsystem entrypoints
├── input/           Lock-free seqlock GPU input page & Wayland seat/pointer/keyboard headers
├── lighting/        SSILVB 32-bit visibility bitmasks, 4-cascade Split HRC GI & lighting_compute_hrc_rays
├── physics/         AVBD 3x3 LDL^T solver, CSR coloring & single-owner GPU BDA compute API
├── ring/            Registered io_uring context, SQ/CQ/PBUF arenas, multishot timeout & file chains
├── scene/           64v/124t quantized meshlets, non-blocking DMA uploads, Subgroup Hi-Z & VisBuf API
├── system/          CPU topology/ISA probe, io_uring kernel feature query & VMA/hugepage tuning
├── util/            Zero-allocation FmtBuf integer/string formatting
├── vulkan/          Vulkan 1.4 context, device scorer, 4-slice non-blocking BDA system & pipeline bank
└── wayland/         Raw Wayland wire codec, 16 KiB send ring, DMA-BUF tranches & DRM syncobj timelines

src/                 Platform core (system, ring, wayland, vulkan, input, chrome, frame) + optional GPU kernels
shaders/             40 shared/demo Slang 2026 shaders (+ 2 text shaders -> 42 SPIR-V 1.6 modules in gen/)
examples/minimal/    Second standalone EngineApp program (7 required hooks, 2 shaders in gen_minimal/)
examples/demo/       Full 10-world showcase, WorkspacePolicy, WorldBuilder, .gmpack/.epack & SDF text
tests/               16 automated C23 test suites (clock, wire, BDA, failure paths, platform/doc truth, etc.)
```
