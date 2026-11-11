# Platform & Render Internals: How the Low-Level Engine Works

This document walks through the platform half of `engine-v2` (`main.c`, `include/`, and `src/`). Everything described here sits **below** the [`include/engine/app.h`](../include/engine/app.h) seam: it has no knowledge of cubes, terrain tiles, plots, or keybindings. Its job is to talk directly to the Linux 7.1+ kernel, the Wayland compositor socket, and the Vulkan 1.4 driver with zero wrapper libraries in between.

---

## Table of Contents

1. [Boot Order, Signal Isolation & Reverse-LIFO Teardown](#1-boot-order-signal-isolation--reverse-lifo-teardown)
2. [Hardware Introspection & VMA Tuning (`src/system/`)](#2-hardware-introspection--vma-tuning-srcsystem)
3. [The `io_uring` Engine (`src/ring/`)](#3-the-io_uring-engine-srcring)
4. [Hand-Written Wayland Wire Protocol & GPU Subsurface Chrome (`src/wayland/`, `src/chrome/`)](#4-hand-written-wayland-wire-protocol--gpu-subsurface-chrome-srcwayland-srcchrome)
5. [WSI-Free Explicit-Sync Swapchain & Frame Pacing (`src/frame/`)](#5-wsi-free-explicit-sync-swapchain--frame-pacing-srcframe)
6. [Descriptorless Vulkan 1.4 BDA & Slang 2026 Pipelines (`src/vulkan/`, `shaders/`)](#6-descriptorless-vulkan-14-bda--slang-2026-pipelines-srcvulkan-shaders)
7. [Lock-Free GPU Input Page & Build-Time Linker Gate](#7-lock-free-gpu-input-page--build-time-linker-gate)

---

## 1. Boot Order, Signal Isolation & Reverse-LIFO Teardown

[`main.c`](../main.c) brings the process up in five deterministic stages and tears them down in strict reverse order:

```text
Boot:     app->configure(argc, argv)
            -> sigprocmask(SIG_BLOCK, {SIGINT, SIGTERM, SIGHUP})
            -> 1. system_init()
            -> 2. ring_init()
            -> 3. wayland_init()
            -> 4. vulkan_init()
            -> 5. engine_subsystem_init()   [calls app->init() and app->load()]
            -> engine_subsystem_run()

Shutdown: engine_subsystem_shutdown()       [calls app->shutdown(), drains DRM release]
            -> vulkan_shutdown()
            -> wayland_shutdown()
            -> ring_shutdown()
            -> system_shutdown()
```

### Why `sigprocmask` Runs Before Any Init Call
Vulkan ICD drivers (Mesa RADV/ANV, NVIDIA) spawn internal background threads when `vkCreateInstance` or `vkCreateDevice` is called. Because [`main.c`](../main.c) blocks `SIGINT`, `SIGTERM`, and `SIGHUP` on the initial thread *before* any subsystem initializes, every driver thread inherits that blocked signal mask. Signals can never randomly interrupt a driver ioctl; instead, they remain pending in the kernel until the engine's `io_uring` loop reads them cleanly from its registered `signalfd` (`ENG_SIGNAL_FD_SLOT`).

If any boot stage fails, every stage after it is skipped (`rc = -1`), and teardown runs only for the stages that actually succeeded.

---

## 2. Hardware Introspection & VMA Tuning (`src/system/`)

Before opening a ring or allocating graphics memory, [`src/system/system.c`](../src/system/system.c) inspects the machine and tunes the process address space:

- **CPU Topology & ISA Probe ([`src/system/probe/cpu.c`](../src/system/probe/cpu.c))**: Executes raw `cpuid` leaves alongside `sched_getaffinity()` and C23 `<stdbit.h>` intrinsics (`stdc_count_ones_ui`, `stdc_bit_width_ui`) to record logical core count, cache-line width, L1d/L2/L3 cache sizes, and ISA support (`AVX2`, `FMA`, `BMI2`, `F16C`, `AVX-512F`).
- **Kernel `io_uring` Capability Probe ([`src/system/probe/uring.c`](../src/system/probe/uring.c))**: Calls `syscall(__NR_io_uring_register, -1, IORING_REGISTER_QUERY, ...)` to verify that the running Linux kernel supports every setup and feature flag in `k_ring_setup_mask` and `k_ring_required_features` before mapping any ring memory.
- **1 ns Timer Slack & VMA Naming ([`src/system/memory/vma.c`](../src/system/memory/vma.c))**:
  - Calls `prctl(PR_SET_TIMERSLACK, 1UL)` to shrink kernel timer coalescing slack from Linux's default `50,000 ns` (`50 µs`) down to `1 ns`, keeping the `8 ms` simulation `timerfd` and frame-stall watchdog sharp.
  - Raises `RLIMIT_MEMLOCK` (so `io_uring` can pin registered memory arenas without `ENOMEM`) and `RLIMIT_NOFILE`.
  - Pre-faults anonymous `mmap` arenas with `madvise(..., MADV_HUGEPAGE)` and `MADV_POPULATE_WRITE` so page tables are wired before the first frame rather than faulting mid-frame.
  - Names anonymous regions via `prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, ...)` so `/proc/<pid>/maps` and `/proc/<pid>/smaps` display human-readable labels such as `[io_uring:sq_cq]`, `[io_uring:pbuf]`, and `[io_uring:sim]`.

---

## 3. The `io_uring` Engine (`src/ring/`)

There is no `liburing` dependency, no `epoll`, and no worker thread pool. [`include/ring/ring.h`](../include/ring/ring.h) and [`src/ring/`](../src/ring/) build directly on `<linux/io_uring.h>` and raw system calls.

### Ring Configuration Flags
Every ring must satisfy `k_ring_setup_mask`:
- `IORING_SETUP_SINGLE_ISSUER` + `IORING_SETUP_DEFER_TASKRUN` + `IORING_SETUP_TASKRUN_FLAG`: Tells the kernel that only one thread ever touches this ring and defers kernel completion taskwork until the thread explicitly enters `io_uring_enter2` to wait for events. Kernel interrupts do not hijack the thread mid-calculation to push CQEs.
- `IORING_SETUP_NO_MMAP` + `IORING_SETUP_NO_SQARRAY`: Instead of letting the kernel allocate fragmented internal ring pages and an indirection `sq_array`, the engine allocates its own page-aligned, hugepage-backed anonymous arena and hands it to the kernel.
- `IORING_SETUP_REGISTERED_FD_ONLY` + `IORING_FEAT_REG_REG_RING`: Even the `io_uring` file descriptor itself is registered into a kernel slot (`enter_fd`), removing per-syscall `fdget`/`fdput` atomic reference counting on every `io_uring_enter2` call.
- `IORING_FEAT_NO_IOWAIT`: Prevents `io_uring` waits from inflating Linux's global `%iowait` CPU metric and messing up CPU frequency governor decisions while the engine sleeps between frames.

### Registered Slots & Provided-Buffer Rings
All steady-state I/O runs against pre-registered kernel tables (`ENG_FIXED_FILES = 16`):

| Fixed File Slot | Constant | Purpose |
| :---: | :--- | :--- |
| `0` | `ENG_WL_SOCK_SLOT` | Wayland compositor Unix domain socket |
| `1` | `ENG_RELEASE_FD_SLOT` | `eventfd` signaled when the compositor releases a `DMA-BUF` |
| `2` | `ENG_SIGNAL_FD_SLOT` | `signalfd` catching `SIGINT`, `SIGTERM`, `SIGHUP` |
| `3` | `ENG_SCRATCH_FD_SLOT` | Direct slot used by linked `openat -> read -> close` chains |
| `4` | `ENG_INOTIFY_FD_SLOT` | `inotify` watcher for `window.chrome`, `.world`, and `.manifest` |
| `5` | `ENG_SIM_FD_SLOT` | `8 ms` `CLOCK_MONOTONIC` simulation `timerfd` |

Two multishot **provided-buffer rings** (`IORING_OP_READ_MULTISHOT` / `RECVSEND_BUNDLE`) keep persistent reads armed without re-submitting SQEs after every packet:
1. **`ENG_PBUF_BGID = 1`** (`32 × 8,192 B` chunks): Absorbs incoming Wayland wire bursts.
2. **`ENG_SIM_BGID = 2`** (`16 × 8 B` chunks): Absorbs `uint64_t` expiration counts from the `8 ms` simulation `timerfd` without colliding with Wayland socket bursts.

### Linked Async File Chains & `EngineRelay`
When loading files (`ring_submit_read_file` / `ring_read_file_all` in [`src/ring/load/chain.c`](../src/ring/load/chain.c)), the engine links `IORING_OP_OPENAT` (directly into `ENG_SCRATCH_FD_SLOT` via `IORING_FILE_INDEX_ALLOC` / fixed slot), `IORING_OP_READ`, and `IORING_OP_CLOSE` with `IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS`.

If a live reload happens while the main loop already has a Wayland `sendmsg`, `signalfd`, or `inotify` read in flight on the same ring, draining CQEs for the file read could accidentally pull in one of those unrelated completions. [`src/frame/loop/watch.c`](../src/frame/loop/watch.c) solves this cleanly with `EngineRelay` (`engine_relay_begin` / `engine_relay_end`): any non-file CQE encountered during a reload is parked inside `relay->stolen_cqe` and replayed once control returns to the main loop.

---

## 4. Hand-Written Wayland Wire Protocol & GPU Subsurface Chrome (`src/wayland/`, `src/chrome/`)

### Raw Wire Framing (`src/wayland/wire/`)
Wayland's wire format is simple: every message is a 32-bit object ID, a 16-bit message byte length paired with a 16-bit opcode, followed by 32-bit padded arguments and optional out-of-band file descriptors carried in `SCM_RIGHTS` ancillary data (`cmsg`).

Instead of linking `libwayland-client` (which brings internal heap queues, `pthread` locks, and generated proxy boilerplate), [`src/wayland/wire/codec.c`](../src/wayland/wire/codec.c), [`send.c`](../src/wayland/wire/send.c), and [`recv.c`](../src/wayland/wire/recv.c) pack and unpack wire frames directly against the `io_uring` socket buffers.

### `DMA-BUF` Tranche Negotiation & Hybrid Multi-GPU `prime_copy`
During `wayland_init()`, [`src/wayland/buffer/dmabuf.c`](../src/wayland/buffer/dmabuf.c) parses `zwp_linux_dmabuf_feedback_v1` format/modifier tranches and records the DRM render node (`major:minor`) of the display compositor:
- **Single-GPU / Direct Scanout Path**: When the chosen Vulkan GPU matches the compositor's DRM device, [`src/vulkan/memory/memory.c`](../src/vulkan/memory/memory.c) intersects the GPU's `VkDrmFormatModifierPropertiesEXT` list with the compositor's offered modifiers and allocates a single-plane `DMA-BUF` image ready for zero-copy compositor composition or direct scanout.
- **Muxless Hybrid Laptop Path (`prime_copy`)**: If you run with `ENGINE_GPU=NVIDIA` (or another discrete GPU) while the desktop compositor sits on an integrated AMD/Intel iGPU, rendering directly into linear cross-PCIe memory would cripple 3D bandwidth. Instead, the engine renders 3D/MSAA passes in fast optimal device-local VRAM on the discrete GPU and records a `vkCmdCopyImage2` (`prime_copy`) into an exportable linear `DMA-BUF` handed to the display compositor.

### GPU-Rendered Subsurface Window Chrome (`chrome/window.chrome`)
Even the window titlebar avoids CPU `wl_shm` software rendering:
- [`chrome/window.chrome`](../chrome/window.chrome) declaratively defines the titlebar thickness, colors, resize band, and close button.
- [`src/frame/present/bar.c`](../src/frame/present/bar.c) and [`src/chrome/gpu.c`](../src/chrome/gpu.c) open a **desynced `wl_subsurface`** with its own double-buffered pair of Vulkan `DMA-BUF` slots (`bar_slot[0..1]`) and its own independent DRM release timelines.
- Hovering the close button or resizing the window repaints the bar with a lightweight fragment shader (`shaders/chrome.frag.slang`) without stalling behind 3D MSAA scene rendering.

---

## 5. WSI-Free Explicit-Sync Swapchain & Frame Pacing (`src/frame/`)

In standard Vulkan applications, `VK_KHR_swapchain` hides buffer ownership behind `vkAcquireNextImageKHR`, `vkQueuePresentKHR`, and `vkWaitForFences`. In `engine-v2`, [`src/frame/`](../src/frame/) implements its own explicit-sync presentation engine on top of `wp_linux_drm_syncobj_v1`.

```mermaid
sequenceDiagram
    participant Ring as io_uring Thread
    participant Sim as 8 ms timerfd
    participant VK as Vulkan 1.4 Queue
    participant DRM as Linux DRM Syncobj
    participant WL as Wayland Compositor

    Sim->>Ring: CQE (ENG_UD_SIM_TIMER)
    Ring->>Ring: frame_clock_issue() -> app->simulation() & advance()
    WL->>Ring: wl_surface.frame callback (teaches display refresh EMA)
    Ring->>VK: vkCmdWriteTimestamp2 + record_compute + Dynamic Rendering + MSAA Resolve
    Ring->>VK: vkQueueSubmit2 (signals Timeline Semaphore + Binary SYNC_FD Semaphore)
    VK->>DRM: vkGetSemaphoreFdKHR -> DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE (Acquire Point)
    Ring->>WL: set_acquire_point + set_release_point + attach + commit
    Note over Ring: Never calls vkWaitForFences; sleeps in io_uring
    WL-->>DRM: Compositor finishes reading buffer -> signals Release Timeline
    DRM-->>Ring: release_eventfd readable (ENG_UD_GPU_RELEASE CQE) -> Slot reusable
```

### Cache-Line Segregated `FrameState` & `FrameSlot`
Look at [`include/frame/internal.h`](../include/frame/internal.h) (enforced at compile time with `static_assert`):
- **Cache Line 0 (`bytes 0..63` of `FrameState`)**: Holds only the hot boolean gates, `turn_phase`, `front` index, and `sync_point` checked on every `io_uring` wake.
- **Cache Line 1 (`bytes 64..127` of `FrameState`)**: Holds `alignas(64) FrameClock clock` (`sizeof(FrameClock) == 64`), isolating the refresh period EMA (`period_q8`), fixed-step simulation cursor (`sim_ms`), and hardware GPU timings (`gpu_compute_us`, `gpu_render_us`, `gpu_total_us`).
- **Cold Bookkeeping (`bytes 128+`)**: Holds resize state, file descriptors, `slot[2]`, `bar_slot[2]`, and `orphans[8]`, so polling a turn flag never pulls `VkImage` handles into L1d cache.
- Inside each `FrameSlot`, bytes `0..63` hold the release gate polled by `spare_ready()`, while `alignas(64) VulkanExportableImage image` starts cleanly on the next cache line.

### Generation-Tagged `SlotRef` Handles & Orphan Retirement
When a user drags a window edge to resize, the old `DMA-BUF` buffer might still be held on screen by the Wayland compositor. Instead of blocking the thread with `vkDeviceWaitIdle`, [`src/frame/present/orphan.c`](../src/frame/present/orphan.c) moves still-held buffers into `frame_state.orphans[0..7]` and allocates fresh resize targets immediately.

Every slot reference carried inside an async `io_uring` completion uses a 32-bit generation-tagged handle ([`include/frame/handle.h`](../include/frame/handle.h)):
```c
// (generation << 8) | (index + 1); 0 is never valid.
typedef uint32_t SlotRef;
```
When an orphan finally receives its compositor release signal and gets reaped, its `generation` increments—guaranteeing that any late async completion referencing the old handle resolves cleanly to `nullptr` instead of aliasing a newly allocated `DMA-BUF`.

### The Vulkan $\leftrightarrow$ DRM Syncobj Bridge (`src/frame/present/sync.c`)
Each scene submit signals two Vulkan semaphores simultaneously in `vkQueueSubmit2`:
1. **A Vulkan Timeline Semaphore (`acquire`, `VK_SEMAPHORE_TYPE_TIMELINE`)**: Queried non-blockingly on the host (`vkGetSemaphoreCounterValue`) to retire BDA transient ring halves, command buffers, and GPU timestamp queries without stalling.
2. **A Binary External SyncFD Semaphore (`VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT`)**: Exported via `vkGetSemaphoreFdKHR` and imported into the Wayland acquire `drm_syncobj` timeline point using `DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE` (`DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE`).
   - *Edge-case handled properly*: Per the Vulkan specification, if the fast GPU finishes rendering *before* `vkGetSemaphoreFdKHR` executes, the driver may legally return `fd = -1` ("already signaled"). The engine detects `fd < 0` on a successful call and immediately signals the DRM timeline point via `DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL` (and falls back to binary `drm_syncobj` + `DRM_IOCTL_SYNCOBJ_TRANSFER` on older DRM kernels).

### Strict Separation of Simulation Clock vs. Display Refresh
- **`wl_surface.frame` does NOT step physics**: Compositor callbacks only teach `FrameClock` the monitor's refresh period (`16.6 ms` at 60 Hz, `6.9 ms` at 144 Hz) and pace when the next picture is recorded.
- **`8 ms` `timerfd` drives simulation**: `sim_on_timer()` calls `frame_clock_issue()` in fixed `8 ms` (`125 Hz`) quanta, capped at `FRAME_SIM_MAX_STEPS = 5` so a debugger pause or system hitch drops excess wall time rather than causing a physics spiral-of-death.
- **True Zero-Wakeup Idle**: As soon as `frame_sim_wanted()` sees no moving bodies, no active heat pulse, and no held camera keys, `sim_timer_set(ring, false)` disarms the `timerfd`. The thread logs `Loop idle` and blocks in `io_uring_enter2` using **0% CPU and 0% GPU** until the next input packet or file change arrives.

---

## 6. Descriptorless Vulkan 1.4 BDA & Slang 2026 Pipelines (`src/vulkan/`, `shaders/`)

### Zero `VkDescriptorSet` Architecture
Search the entire repository for `vkCreateDescriptorPool`, `vkAllocateDescriptorSets`, or `vkUpdateDescriptorSets`—none exist. Every shader reads and writes GPU memory through **64-bit Buffer Device Addresses (`VkDeviceAddress`)** passed directly inside `vkCmdPushConstants2`.

### Three-Tier BDA Memory Allocator (`src/vulkan/memory/bda.c`)
[`include/vulkan/bda.h`](../include/vulkan/bda.h) manages GPU memory through `BdaSystem`:
1. **`device_pool` (`64 MiB` `DEVICE_LOCAL` slabs)**: Suballocated with a coalescing sorted free-list (`free_list[64]` inline ranges per slab, spilling to `free_extra` only if fragmented beyond 64 disjoint holes). On Resizable BAR (ReBAR / SAM) GPUs where `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT` heaps cover VRAM, slabs are persistently mapped so CPU ray-picks can inspect GPU compute buffers directly.
2. **`staging_pool` (Host-visible coherent cached slabs)**: Feeds dynamic uploads across ring-buffered slices reclaimed via timeline semaphore counter values (never `vkQueueWaitIdle`).
3. **`transient_ring` (`16 MiB` dual-half per-frame bump allocator)**: Partitioned into two `8 MiB` halves selected by `signal_point & 1`. Once a frame slot's previous `acquire_point` retires on the timeline semaphore, its half rewinds to offset `0` in $O(1)$ time.

### Module-Free `Maintenance5` Pipelines & `#embed`ded Slang 2026 Shaders
- At compile time, `Makefile` compiles every `.slang` shader in [`shaders/`](../shaders/) to **SPIR-V 1.6** (`slangc -target spirv -profile spirv_1_6`) and embeds the binary bytecode directly into C translation units via standard **ISO C23 `#embed`** (`--embed-dir=build/<mode>/gen`).
- At runtime, [`src/vulkan/pipe/pipe.c`](../src/vulkan/pipe/pipe.c) uses Vulkan 1.4 / `VK_KHR_maintenance5` to pass `VkShaderModuleCreateInfo` inline via `pNext` on `VkPipelineShaderStageCreateInfo`—creating pipelines without ever instantiating or destroying standalone `VkShaderModule` handles—and persists compiled ISA across launches in a hardware-UUID-verified disk pipeline cache.
- [`shaders/plane.frag.slang`](../shaders/plane.frag.slang) uses SPIR-V 1.6 subgroup `WaveRotate` (`spvGroupNonUniformRotateKHR`) alongside wrapped camera-phase math (`plane_grid_phase`) so the infinite metric ground grid stays crisp and jitter-free across eight orders of magnitude of zoom (`0.0001x` to `10,000x`).

---

## 7. Lock-Free GPU Input Page & Build-Time Linker Gate

### Host-to-GPU Seqlock Input Page (`src/input/page/`)
[`include/input/page.h`](../include/input/page.h) maps two 64-byte cache lines shared between the Wayland seat decoder and the GPU:
- **Line 0**: Written by the CPU seat handler using a sequence counter whenever pointer or key state updates.
- **Line 1**: Written by a tiny GPU compute dispatch (`shaders/input.comp.spv`) recording the exact sequence number observed by the GPU on that frame, harvested back on the host once retired.

### Zero-Dead-Code Linker Enforcement (`Makefile`)
Every `.c` file is compiled with `-ffunction-sections -fdata-sections -Werror=unused-function` and linked with `-Wl,--gc-sections -Wl,--print-gc-sections`. Immediately after linking, [`Makefile`](../Makefile) inspects `link.log` for any discarded `.text.*` section originating from `src/`:
```makefile
dead=$$(grep -E "removing unused section '\.text\.[^']*' in file '.*(src/vulkan|src/frame|src/ring|src/wayland|src/input|src/chrome|src/system|src/backend)" $(BUILD_DIR)/link.log || true);
```
If even one unreferenced function exists anywhere in the engine platform, the build fails immediately and deletes the output binary. Nothing in `src/` is dead weight, speculative scaffolding, or unused wrapper code.
