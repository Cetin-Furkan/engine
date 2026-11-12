# Engine v0.2 Specification — Adaptive GPU Execution Profiles & 2026 Mathematical Architecture

> **Status**: **Complete** (`v0.1.0` -> `v0.2.0` — All Milestones `v0.2-M1` .. `v0.2-M5` Implemented & Benchmarked)  
> **Scope**: Hardware-specialized execution across **UMA Integrated GPUs (2020+)**, **Direct Discrete GPUs**, and **Muxless Hybrid (`dGPU -> iGPU`)** systems, paired with **2025–2026 real-time graphics and simulation mathematics**.  
> **Related Docs**: [README.md](../README.md) · [BENCHMARK_V01_VS_V02_M1.md](BENCHMARK_V01_VS_V02_M1.md) · [BENCHMARK_V02_M2.md](BENCHMARK_V02_M2.md) · [BENCHMARK_V02_M3.md](BENCHMARK_V02_M3.md) · [BENCHMARK_V02_M4.md](BENCHMARK_V02_M4.md) · [BENCHMARK_V02_M5.md](BENCHMARK_V02_M5.md) · [PLATFORM_AND_RENDER_INTERNALS.md](PLATFORM_AND_RENDER_INTERNALS.md) · [WORKFLOW_CONTROLS_AND_EXTENSIBILITY.md](WORKFLOW_CONTROLS_AND_EXTENSIBILITY.md) · [ARCHITECTURE_COMPARISON.md](ARCHITECTURE_COMPARISON.md) · [TERRAIN_BACKEND_ARCHITECTURE.md](../TERRAIN_BACKEND_ARCHITECTURE.md)

---

## Table of Contents

1. [Design Thesis: Why Hardware Specialization + 2026 Math Belong Together](#1-design-thesis-why-hardware-specialization--2026-math-belong-together)
2. [Foundation Already Built in Engine v0.1 (NVIDIA, Muxless & Explicit Sync)](#2-foundation-already-built-in-engine-v01-nvidia-muxless--explicit-sync)
3. [Three-Tier Hardware Execution Profiles (`GpuExecutionProfile`)](#3-three-tier-hardware-execution-profiles-gpuexecutionprofile)
4. [Memory, BDA & `io_uring` Streaming Split (Zero-Staging UMA vs. Async PCIe Ring)](#4-memory-bda--io_uring-streaming-split-zero-staging-uma-vs-async-pcie-ring)
5. [Bandwidth, Culling & Raster Math: Adaptive MSAA, Meshlets, Hi-Z & Visibility Buffers](#5-bandwidth-culling--raster-math-adaptive-msaa-meshlets-hi-z--visibility-buffers)
6. [2026 Rayless & Hybrid Lighting: Visibility Bitmasks (`SSILVB`) + Radiance Cascades (`HRC`)](#6-2026-rayless--hybrid-lighting-visibility-bitmasks-ssilvb--radiance-cascades-hrc)
7. [2026 Unified GPU Physics: Augmented Vertex Block Descent (`AVBD`)](#7-2026-unified-gpu-physics-augmented-vertex-block-descent-avbd)
8. [2026 Geodetic & Photogrammetric Reconstruction: Compute ZNCC + `2DGS` Surfels](#8-2026-geodetic--photogrammetric-reconstruction-compute-zncc--2dgs-surfels)
9. [Milestone Breakdown (`v0.2-M1` .. `v0.2-M5`)](#9-milestone-breakdown-v02-m1--v02-m5)

---

## 1. Design Thesis: Why Hardware Specialization + 2026 Math Belong Together

Most engines treat integrated GPUs as "slow discrete GPUs"—running the exact same PCIe staging copies, multi-sample attachment resolves, and queue submission patterns, then turning off features until frame rates recover. That wastes the biggest physical advantage an integrated GPU actually has (**Unified Memory Architecture**) while slamming straight into its real physical ceiling (**shared system DRAM bandwidth**).

Compare the hardware physics of a **2020 Vulkan 1.4 iGPU** (such as an Intel Gen12 TigerLake-LP Iris Xe or AMD Renoir Vega iGPU) against a **discrete GPU**:

| Physical Property | 2020 UMA Integrated GPU (`iGPU`) | Dedicated Discrete GPU (`dGPU`) | Muxless Hybrid (`dGPU` + Display `iGPU`) |
| :--- | :--- | :--- | :--- |
| **Memory Bus** | Shared LPDDR4x / DDR4 (`~35–68 GB/s` shared with CPU cores and display scanout) | Dedicated GDDR6 / GDDR6X / GDDR7 (`360–1,500+ GB/s`) | Render in GDDR6/7; final scanout buffer crosses PCIe into system RAM |
| **CPU <-> GPU Visibility** | **100% True UMA**: `DEVICE_LOCAL \| HOST_VISIBLE \| HOST_COHERENT \| HOST_CACHED` shares physical LLC/DRAM with the CPU | Split heap: fast non-host-visible `DEVICE_LOCAL` VRAM + PCIe BAR / ReBAR window | dGPU VRAM is *unreadable* by the display iGPU's DMA engine (`!DEVICE_LOCAL` export required) |
| **Hardware Queues** | Usually **1 single unified engine** (`family 0 x1`, `submission_aliased == true`) | Separate Graphics, Async Compute (`ACE`), and PCIe DMA Copy Engines (`SDMA`/`CE`) | dGPU Copy Engine can push frames to system RAM while Graphics starts frame $N+1$ |
| **Ray Tracing Hardware** | **None** (no RT cores / BVH traversal units) | Hardware BVH + ray-triangle units (`VK_KHR_ray_query`) on RTX / RDNA2+ / Arc | Available on the render dGPU |
| **Fast Native ALU** | Full Vulkan 1.4, `fp16` (`shaderFloat16`), `int16`, 64-bit BDA, 1-cycle bitwise shifts/`popcount`, hardware wave/subgroup ops | Wide `fp32`/`fp16` SIMD + tensor/ray units | Wide `fp32`/`fp16` SIMD on render dGPU |

At the exact same time, **2025–2026 graphics and simulation mathematics** pivoted away from noisy stochastic ray tracing + heavy neural denoisers toward **structured deterministic mathematics**:
* **32-bit Visibility Bitmasks (`VBAO`/`SSILVB`)** replace multi-ray hemisphere sampling with single-cycle integer bitmasks and `popcount`.
* **Split / Holographic Radiance Cascades (`HRC`)** compute noise-free Global Illumination deterministically via multi-scale ray intervals—where the interval oracle can be a **heightfield / Hi-Z depth pyramid on a 2020 iGPU** or **hardware `VK_KHR_ray_query` on a dGPU**.
* **Augmented Vertex Block Descent (`AVBD`, SIGGRAPH 2025)** solves coupled rigid stacking and soft-body elasticity with unconditional stability in **4–5 parallel vertex iterations** using register-resident $3 \times 3$ linear algebra.

That alignment means `Engine v0.2` does not need a "low-end fallback engine" and a "high-end engine." It uses **one unified mathematical architecture** parameterized by a hardware execution profile.

---

## 2. Foundation Already Built in Engine v0.1 (NVIDIA, Muxless & Explicit Sync)

`Engine v0.1` already solved the hardest Linux multi-GPU compatibility and explicit-sync bugs at the kernel/driver boundary:

1. **Cross-Device Wayland Tranche Negotiation**: [`src/wayland/buffer/dmabuf.c`](../src/wayland/buffer/dmabuf.c) separates producer tranches (matching the Vulkan render/primary DRM `major:minor`) from consumer tranches (`main_device`, i.e., the display iGPU on a muxless laptop). When a discrete NVIDIA/AMD GPU has no direct scanout tranche, `finish_feedback_round` activates `wl->feedback_cross_device = true`.
2. **VRAM Render + System-RAM Foreign Export (`prime_copy`)**: [`find_foreign_import_memory`](../src/vulkan/memory/memory.c) forces cross-GPU exported `DMA-BUF` allocations into coherent system RAM (`!DEVICE_LOCAL`, avoiding Resizeable BAR heaps that a second GPU's DMA controller cannot import), while [`ensure_prime_color`](../src/frame/present/slot.c) renders at full speed inside `VK_IMAGE_TILING_OPTIMAL` `DEVICE_LOCAL` VRAM (`frame->prime_image`) and [`frame_cmd_finish_present`](../src/frame/present/barrier.c) executes `vkCmdCopyImage2` + queue-ownership release (`VK_QUEUE_FAMILY_FOREIGN_EXT` or `VK_QUEUE_FAMILY_EXTERNAL` when NVIDIA omits foreign-queue support).
3. **NVIDIA & Kernel Explicit-Sync Edge Cases**: [`src/frame/present/sync.c`](../src/frame/present/sync.c) pairs a Vulkan `TIMELINE` semaphore with an exportable binary `SYNC_FD` semaphore, handles `vkGetSemaphoreFdKHR` returning `VK_SUCCESS` with `sync_fd == -1` (already signaled on fast dGPUs) via direct `DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL`, and preserves point `0` of the kernel `dma_fence_chain` via a two-step `DRM_IOCTL_SYNCOBJ_TRANSFER` fallback.
4. **Live Ranked Multi-GPU Failover**: [`vulkan_device_use_next`](../src/vulkan/device/device.c) and [`frame_boot`](../src/frame/boot/boot.c) automatically tear down a non-presenting GPU and open the next ranked device without restarting the process.

`Engine v0.2` extends this exact compatibility architecture into **performance and memory specialization**.

---

## 3. Three-Tier Hardware Execution Profiles (`GpuExecutionProfile`)

### Zero-Dead-Code Constraint
`Engine`'s [`Makefile`](../Makefile) links with `-ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--print-gc-sections` and **fails the build** if any function in `src/` is unreferenced. Therefore, hardware-specific code paths must never sit behind dead compile-time branches. Instead:

1. `VulkanDeviceInfo` classifies the physical device's memory and queue topology during [`open_candidate`](../src/vulkan/device/device.c), and refines presentation topology once [`wayland_query_surface_dmabuf_feedback`](../src/wayland/buffer/dmabuf.c) completes.
2. Subsystem policy parameters (`msaa_cap`, `zero_copy_uma`, `terrain_resident_slot_cap`, `async_compute_enabled`, `async_transfer_enabled`, `ray_query_enabled`) live in a plain C23 value struct (`VulkanExecutionProfile`) read directly by `memory.c`, `bda.c`, `pass.c`, `uring_pager.c`, and Slang pipeline specialization constants (`VkSpecializationInfo`).
3. Environment overrides (`ENGINE_GPU_PROFILE=auto|uma|discrete|hybrid` and `ENGINE_MSAA=1|2|4|8`) allow deterministic A/B benchmarking on a single machine.

```c
typedef enum GpuProfileKind : uint8_t {
    GPU_PROFILE_UMA_IGPU        = 0, // Unified-memory integrated GPU (e.g., 2020 Intel Iris Xe / AMD Vega)
    GPU_PROFILE_DISCRETE_DIRECT = 1, // Discrete GPU presenting directly to its own display/compositor
    GPU_PROFILE_HYBRID_MUXLESS  = 2, // Discrete GPU rendering in VRAM + prime_copy to display iGPU
} GpuProfileKind;
```

### Detection Invariants
* **True UMA (`is_true_uma`)**: `deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU` (or all `DEVICE_LOCAL` heaps expose a `HOST_VISIBLE | HOST_COHERENT` memory type covering $\ge 90\%$ of device-local bytes).
* **Cached Coherent UMA (`has_uma_cached`)**: At least one memory type carries `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT | HOST_CACHED`—allowing CPU reads/writes at L3/LLC cache speed and GPU BDA reads at native speed with zero flushes or copies.
* **Muxless Hybrid (`GPU_PROFILE_HYBRID_MUXLESS`)**: Upgraded from `GPU_PROFILE_DISCRETE_DIRECT` when `wl->feedback_cross_device` (`prime_copy`) activates in `frame_boot`.

---

## 4. Memory, BDA & `io_uring` Streaming Split (Zero-Staging UMA vs. Async PCIe Ring)

### 4.1 Fixing the UMA Double-Allocation & Write-Combine Trap (`memory.c` & `bda.c`)
In `v0.1`, [`vulkan_find_memory_type`](../src/vulkan/memory/memory.c) ranked `want_device_local` strictly by `!host_visible ? 2 : 1`, ignoring `HOST_CACHED` when `host_visible` was true. On Mesa ANV (Intel Gen11/Gen12 Xe), memory type `0` is often **Write-Combined (`!HOST_CACHED`)** while memory type `1` is **Write-Back LLC Cached (`HOST_CACHED`)**—both in the exact same UMA heap!
* **v0.2 Rule (`UMA_IGPU`)**:
  1. When `is_true_uma` is true and a `DEVICE_LOCAL` buffer is requested, prefer `HOST_VISIBLE | HOST_COHERENT | HOST_CACHED` so mapped BDA slabs get full CPU L1/L2/LLC read+write caching alongside native GPU BDA access.
  2. `bda_alloc_staging` aliases directly to the unified `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT | HOST_CACHED` slab pool instead of spinning up a separate 16 MiB `staging_pool` slab—eliminating redundant RAM pool fragmentation and eliminating RAM-to-RAM `vkCmdCopyBuffer2` staging passes.
* **v0.2 Rule (`DISCRETE_DIRECT` & `HYBRID_MUXLESS`)**:
  1. Pure `DEVICE_LOCAL` (`!HOST_VISIBLE`) wins for static/GPU-written slabs (`device_pool`), keeping maximum GDDR6/GDDR7 bandwidth.
  2. `transient_ring` (16 MiB per-frame bump) targets ReBAR (`DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT`).
  3. Large streaming transfers stage through a `HOST_VISIBLE | HOST_COHERENT` ring registered with `io_uring` and copy asynchronously on `vk->queues.transfer_queue`.

### 4.2 Adaptive Resident Working-Set & Zero-Copy `io_uring` Paging (`uring_pager.c`)
In `v0.1`, the terrain backend unconditionally pre-allocated all `341` quadtree slots (`127.90 MiB`) plus a `64.00 MiB` bird's-eye overlay arena (`191.90 MiB` total) even though `k_gmpack_active_cut_cap = 64` leaves at most 64 tiles visible in any camera cut, and `uring_pager.c` duplicated fused tiles in `malloc` heap while blocking inside `submit_uring_tile_batch`.

* **Milestone 1 (`UMA_IGPU` vs `DISCRETE` Working-Set & Zero-Copy Path)**:
  * **`UMA_IGPU` Profile**:
    * Allocates a **96-slot** resident BDA tile window (`36.02 MiB` instead of `127.90 MiB`—50% headroom above the 64-leaf active cut so normal panning still hits 0 evictions!) and sizes the bird's-eye overlay BDA arena dynamically to actual committed layers (`16.00 MiB` cap instead of `64.00 MiB`), immediately saving **~140 MiB of shared laptop RAM**.
    * Registers the mapped UMA BDA tile pool directly as an **`io_uring` fixed buffer (`IORING_REGISTER_BUFFERS` / `IORING_OP_READ_FIXED`)** when supported by kernel `RLIMIT_MEMLOCK`, so the NVMe controller DMAs 384 KiB `.gmpack` tiles **straight into the exact `VkDeviceAddress` read by Slang shaders**—zero kernel per-I/O page pinning (`get_user_pages`) and zero GPU staging copies.
  * **`DISCRETE_DIRECT` / `HYBRID_MUXLESS` Profile**:
    * Retains the full `341`-slot (`127.90 MiB`) zero-eviction resident cache when dedicated VRAM $\ge 4\text{ GiB}$, staging NVMe reads through a pinned host ring onto `transfer_queue`.
* **Milestone 2 (Asynchronous Frame-Decoupled Pager)**:
  * Decouples `IORING_OP_READ_FIXED` SQE submission from CQE harvesting on the main `io_uring` loop: ancestors (`LOD 0..2`) render immediately while newly requested `LOD 3..4` leaves flip `k_tile_flag_resident` on the next frame once their CQE arrives—guaranteeing **0.00 ms** main-loop I/O wait.

### 4.3 `HYBRID_MUXLESS` Cross-GPU Presentation Upgrade
Building on [`frame_cmd_finish_present`](../src/frame/present/barrier.c):
1. **Damage-Tracked Cross-PCIe Copy**: Track screen-space dirty AABBs for UI/overlay-only frames (e.g., text cursor blink or HUD tick) and emit bounded `VkImageCopy2` sub-regions + `wl_surface.damage_buffer` instead of copying full 1080p/4K frames over PCIe when the 3D camera and simulation are idle.
2. **Async Copy-Engine Pipeline (`has_dedicated_transfer`)**: Signal an internal timeline point at `COLOR_ATTACHMENT_OUTPUT_BIT` on `graphics_queue` and execute the `prime_image -> foreign_dmabuf` copy on `transfer_queue` (NVIDIA/AMD hardware DMA engine) so PCIe write-combining never stalls shader execution on frame $N+1$.

---

## 5. Bandwidth, Culling & Raster Math: Adaptive MSAA, Meshlets, Hi-Z & Visibility Buffers

### 5.1 Fixing the Immediate-Mode MSAA Bandwidth Wall (`pass.c`)
Desktop/laptop GPUs (including Intel Iris Xe and AMD Vega/RDNA) are **immediate-mode rasterizers**, not mobile Tile-Based Deferred Renderers (`TBDR`). Every sample in an $S$-sample MSAA pass multiplies color + depth attachment DRAM footprint and clear/write/resolve traffic by $S$:

$$\text{Attachment Bytes (Double-Buffered)} = 2 \times W \times H \times S \times (\underbrace{4}_{\text{BGRA8}} + \underbrace{4}_{\text{D32}})$$

At $1920 \times 1080$ with `8x` MSAA, two `FrameSlot`s require $2 \times 2{,}073{,}600 \times 8 \times 8 = 253.1\text{ MiB}$ of attachment memory (`132.7 MiB` per slot)! On a 2020 iGPU with `~40 GB/s` shared RAM bandwidth, `8x` MSAA dominates `gpu_render_us`.

* **Adaptive Sample Policy (`v0.2-M1`)**:
  * `UMA_IGPU`: Caps default scene MSAA at **`2x` (`VK_SAMPLE_COUNT_2_BIT`)**—cutting MSAA memory and ROP/DRAM traffic by **75% (`4x` reduction)** while keeping hardware diagonal edge antialiasing (combined with `Engine`'s existing analytical SDF antialiasing on chrome, 2D sheet shapes, plots, and glyphs).
  * `DISCRETE_DIRECT` / `HYBRID_MUXLESS`: Caps default scene MSAA at **`4x` (`VK_SAMPLE_COUNT_4_BIT`)** (`8x` remains selectable via `ENGINE_MSAA=8`).

### 5.2 Unified `src/scene/` Meshlet Kernel & 16-Bit Local Quantization (`v0.2-M2`)
All 3D geometry moves into clusterized **64-vertex / 124-triangle meshlets** stored in SoA BDA slabs:
* **Quantized Positions**: Per-meshlet AABB origin $o \in \mathbb{R}^3$ and scale $s \in \mathbb{R}^3$ in the 64-byte `MeshletHeader`; vertex coordinates stored as `uint16_t[3]` (`6 B`/vertex):
  $$p_i = o + s \odot \left(\frac{q_i}{65535}\right)$$
* **Octahedral `snorm16` Normals (`4 B`/vertex)**: Maps $n \in \mathbb{S}^2$ onto $[-1, 1]^2$ via $\|n\|_1$ projection with fold-over for $n_z < 0$, achieving `< 0.02°` angular error at 3x lower bandwidth than `float3`.
* **Two-Phase Hierarchical-Z (`Hi-Z`) Occlusion Culling**:
  1. **Phase 1**: Slang compute tests meshlet bounding spheres + backface normal cones against frame $N-1$'s Hi-Z mip pyramid, emitting visible clusters into a BDA indirect argument buffer (`vkCmdDrawMeshTasksIndirectCount` / `vkCmdDrawIndexedIndirectCount`).
  2. **Hi-Z Pyramid Build**: Single-pass compute downsampler using `WaveReadLaneAt` / `SubgroupShuffleXor` reduces depth `2x2 -> 1` across mips in shared memory.
  3. **Phase 2**: Re-tests clusters rejected in Phase 1 against the fresh frame-$N$ Hi-Z pyramid and draws only newly disoccluded geometry—achieving zero CPU draw-call overhead and near-zero overdraw.

### 5.3 Visibility Buffer (`VisBuf`) Deferred Shading Path (`v0.2-M3`)
For complex PBR/terrain scenes on `UMA_IGPU`, a `1x` `R32_UINT` Visibility Buffer packs `(instance_id : 14) | (meshlet_id : 11) | (prim_id : 7)` per pixel alongside `D32_SFLOAT` depth. A single compute pass reconstructs exact ray-triangle barycentrics $(\lambda_0, \lambda_1, \lambda_2)$ and screen-space analytic partial derivatives $(\partial \lambda / \partial x, \partial \lambda / \partial y)$, shading **every visible pixel exactly once** with **0% 2x2 quad helper-lane waste**.

---

## 6. 2026 Rayless & Hybrid Lighting: Visibility Bitmasks (`SSILVB`) + Radiance Cascades (`HRC`)

### 6.1 Screen-Space Indirect Lighting with Visibility Bitmask (`VBAO` / `SSILVB`)
Instead of noisy SSAO or horizon-clamped GTAO, each hemisphere slice around view-space normal $N$ is discretized into $N_b = 32$ uniform angular sectors stored in one `uint32_t` register `mask = 0u`:

1. For each depth sample along screen-space direction $\phi$ (marched in `fp16`), compute front elevation angle $\theta_f$ (from sample surface point $P_s$) and back elevation angle $\theta_b$ (from $P_s - \tau V$, where $\tau$ is thickness).
2. Map $[\theta_b, \theta_f] \subset [0, \pi]$ to integer bit indices $[k_0, k_1] \subset [0, 31]$ and build the sector bit-run in two shifts:
   ```hlsl
   uint count   = max(k1 - k0, 0u);
   uint hitBits = count >= 32u ? 0xffffffffu : (((1u << count) - 1u) << k0);
   uint newlyOccluded = hitBits & ~visMask;
   visMask |= hitBits;
   ```
3. **1-Cycle Bitwise Integration**:
   * **Ambient Visibility**: $V = 1 - \frac{\operatorname{countbits}(\text{visMask})}{32}$.
   * **Single-Bounce Indirect GI**: Weight each sample's color radiance $L_s$ by $\operatorname{countbits}(\text{newlyOccluded}) / 32.0$, automatically preventing occluded back-surfaces from leaking light while allowing light to pass behind thin foreground poles or ridges!

Because `countbits`, shifts, and bitwise `AND`/`OR` execute in 1 cycle on 2020 Intel/AMD integer ALUs, `SSILVB` delivers contact shadows, bent normals, and local indirect color bounce on a 2020 iGPU in `< 0.45 ms` at half-resolution with bilateral `fp16` spatial-temporal resolve.

### 6.2 Split / Holographic Radiance Cascades (`HRC`, 2025–2026)
To capture world-space sky/sun/emissive bounce without stochastic path-tracing noise:
* **Mathematical Principle**: A localized emitter of radius $r$ at distance $d$ requires spatial sampling spacing $\Delta x = O(d)$ and angular ray spacing $\Delta \theta = O(r / d)$. Therefore, cascade level $c \in \{0, \dots, C-1\}$ stores probes at spacing $\Delta x_c = \Delta x_0 \cdot 2^c$, ray intervals $[t_c, t_{c+1}] = [t_0 2^c, t_0 2^{c+1}]$, and angular count $M_c = M_0 \cdot 2^{d_{\text{dim}} c}$, keeping **space-angle memory constant per cascade**.
* **Cascade Merge Operator** (from far cascade $c+1$ into near cascade $c$ along direction $\omega$):
  $$L_c(x, \omega) = L_{\text{interval}}(x, \omega, [t_c, t_{c+1}]) + \beta_c(x, \omega) \cdot \mathcal{I}\!\left(L_{c+1}(\cdot, \omega)\right)(x)$$
  where $\beta_c \in [0, 1]$ is the interval transmittance ($0$ on hit, $1$ on miss) and $\mathcal{I}$ is bilinear/trilinear probe interpolation.
* **Dual Hardware Oracle (Compiled via Slang Specialization Constant `k_oracle_mode`)**:
  * **`UMA_IGPU` (`k_oracle_mode = 0`)**: Evaluates $[t_c, t_{c+1}]$ against `Engine`'s resident **2.5D Terrain Heightfield Pyramid + World-Space Jump-Flooded SDF / Hi-Z**. Zero RT hardware required; 100% deterministic, zero-noise GI.
  * **`DISCRETE_DIRECT` / `HYBRID_MUXLESS` (`k_oracle_mode = 1` when `VK_KHR_ray_query` is present)**: Evaluates $[t_c, t_{c+1}]$ with hardware `RayQueryKHR` (`Proceed()`) against a two-level BVH (`TLAS`/`BLAS`) for exact off-screen 3D architectural occlusion and glossy reflections.

---

## 7. 2026 Unified GPU Physics: Augmented Vertex Block Descent (`AVBD`)

`Engine v0.1` already runs elastic solids, soft patches, and restitution labs (`Worlds 2–7`). In `v0.2`, simulation unifies under **Augmented Vertex Block Descent (`AVBD`, Giles, Diaz & Yuksel — SIGGRAPH 2025/2026)**.

### Mathematical Formulation
Given inertial prediction $y_i = x_i^t + h v_i^t + h^2 g$, implicit Euler seeks positions $x = \{x_i\}$ minimizing the augmented variational potential:

$$F(x, \lambda) = \sum_i \frac{m_i}{2 h^2} \|x_i - y_i\|^2 + \sum_{c \in \mathcal{C}} \left( \lambda_c^\top C_c(x) + \frac{k_c}{2} \|C_c(x)\|^2 \right)$$

Instead of assembling a global sparse matrix or taking sequential scalar XPBD projections that struggle at high mass ratios:
1. **Parallel Per-Vertex $3 \times 3$ Block Solve**: Holding neighbors fixed within each graph color, vertex $i$ has gradient $g_i \in \mathbb{R}^3$ and symmetric positive-definite $3 \times 3$ Hessian:
   $$H_i = \frac{m_i}{h^2} I_3 + \sum_{c \ni i} k_c J_{c,i}^\top J_{c,i} + \nabla_{x_i}^2 \left(\lambda_c^\top C_c\right)$$
   A single Slang compute thread solves $H_i \Delta x_i = -g_i$ **in registers** via analytical $3 \times 3$ Cholesky / cofactor inversion (`< 25` FMA ops, zero shared-memory spills) and updates $x_i \leftarrow x_i + \Delta x_i$.
2. **Dual Augmented-Lagrangian Step**: After each primal sweep, constraints update Lagrange multipliers $\lambda_c \leftarrow \lambda_c + k_c C_c(x)$ and adaptively ramp penalty stiffness $k_c \leftarrow \min(k_{\max}, \gamma k_c)$.

### Hardware Split
* **`UMA_IGPU`**: Runs **4 primal-dual iterations** per 8 ms tick inline on the unified `graphics+compute` command buffer before vertex draw, storing constraint Jacobians in `fp16` (`half3`) and vertex positions in `fp32` inside coherent BDA slabs (`0` PCIe readback cost when CPU hit-testing reads vertex positions!).
* **`DISCRETE_DIRECT` / `HYBRID_MUXLESS`**: Dispatches **8 iterations / substeps** + GPU spatial-hash broadphase on `vk->queues.compute_queue` (`Async Compute`), synchronized via timeline semaphore at `VERTEX_SHADER_BIT`.

---

## 8. 2026 Geodetic & Photogrammetric Reconstruction: Compute ZNCC + `2DGS` Surfels

1. **GPU Compute Plane-Sweep MVS (`survey_mvs.comp.slang`)**:
   Replaces CPU `pthread` loops in [`mvs_fusion.c`](../src/backend/terrain/survey/mvs_fusion.c) with a tiled Slang compute kernel that loads rectified reference/source photo tiles into `groupshared half` memory, evaluates $5 \times 5$ Zero-Mean Normalized Cross-Correlation (`ZNCC`) across candidate elevations, refines sub-post peaks via 3-point parabola fitting, and writes fused `height_u16` directly into the resident BDA tile slot (`< 3 ms` per tile on a 2020 iGPU; `< 0.3 ms` on a dGPU).
2. **2D Gaussian Splatting (`2DGS`) Geodetic Surfels**:
   Represents non-heightfield survey features (rock overhangs, facades, vegetation) as oriented 2D planar Gaussian disks $(p_k \in \mathbb{R}^3, t_u, t_v \in \mathbb{R}^3, \text{SH/RGB}, \alpha)$ with exact ray-plane intersection depth and surface normals:
   * **`UMA_IGPU`**: Hardware-instanced quad rasterization with frustum + normal-cone cull and wave-local bucketed alpha blending (avoiding global memory radix sorts).
   * **`DISCRETE_DIRECT`**: Subgroup One-Sweep Radix Sort + tile-based compute rasterizer + TSDF mesh extraction.
3. **Compressed `.epack` v1 Container (`100 KiB` = `25 x 4,096 B` / Tile — `3.84x` Smaller)**:
   Replaces raw `RGBA8` (`262 KiB`/tile) + raw `u16` (`131 KiB`/tile = `384 KiB`) with a 4 KiB-aligned GPU-decoded `EpackTileBlock`:
   * **`48 KiB` `8x8` Planar + 5-Bit Residual DEM (`1,024 x 48 B` `EpackElevPatch`)**: Least-squares plane $(h_0 + s_x lx + s_y ly)$ + 5-bit quantized residual (`1.21 cm` interior RMSE on Baklan & Beşparmak Dağı).
   * **`2 KiB` Lossless `u16` Perimeter Seam Ring (`4 x 256 x 2 B`)**: Guarantees exact `0.0000 m` bit-identical boundary invariance across all quadtree LOD neighbors.
   * **`2 KiB` `RGB565` Border Color Ring + `32 KiB` `4x4` BC1 Satellite Albedo (`4,096 x 8 B`) + `16 KiB` `128x128` Horizon AO (`33.9 dB` PSNR)**: Decoded directly in Slang 2026 via a fused scalar-kernel BC1 distributive stencil (`12` live scalar registers, zero SIMD16 divergence).

---

## 9. Milestone Breakdown (`v0.2-M1` .. `v0.2-M5`)

| Milestone | Status | Subsystem | Key Deliverables |
| :--- | :---: | :--- | :--- |
| **`v0.2-M1`** | **Done** | **Hardware Execution Profiles, Bandwidth Cap & Zero-Staging UMA Pager** | `VulkanExecutionProfile` (`UMA_IGPU` / `DISCRETE_DIRECT` / `HYBRID_MUXLESS`) in `src/vulkan/`; UMA `HOST_CACHED` memory ranking + unified zero-staging BDA pool in `memory.c` & `bda.c`; adaptive MSAA cap (`2x` iGPU / `4x` dGPU + `ENGINE_MSAA`) in `pass.c`; profile-scaled resident terrain window (`96` slots on UMA vs `341` on dGPU), compact overlay arena, and lazy spill-on-evict survey cache (`-69.7%` UMA memory). |
| **`v0.2-M2`** | **Done** | **Non-Blocking Async `io_uring` Streamer, `.epack` v1 Codec & `HYBRID_MUXLESS` Damage/DMA** | Frame-decoupled SQE/CQE tile pager (`0.00 ms` live render-thread stall, `ENG_PACK_FD_SLOT` + `16 x 100 KiB` `IORING_OP_READ_FIXED` staging on discrete/hybrid, zero-copy `IORING_OP_READ` into unified BDA on `UMA_IGPU`); `3.84x` `.epack` v1 codec (`33.32 MiB` SSD archive, `9.40 MiB` UMA arena sub-allocated inside unified BDA pool, `0.0000 m` lossless seam); `HYBRID_MUXLESS` screen-space damage-rect `prime_copy` + `wl_surface.damage_buffer` + two-queue async DMA transfer handoff. |
| **`v0.2-M3`** | **Done** | **`src/scene/` Meshlet Kernel, Two-Phase Subgroup `Hi-Z` & `R32_UINT` Visibility Buffer** | Unified 64-vert / 124-tri quantized meshlets (`16 B/vert`: `uint16` local AABB pos + octahedral `snorm16` normals + `RGBA8`, **`5.35x` geometry compression**, `-5.69 MiB` BDA on Worlds 2 & 7); single-pass `WaveReadLaneAt` Subgroup `Hi-Z` pyramid + GPU-predicated Two-Phase occlusion culling (`vkCmdDrawIndirectCount` + `vkCmdPushDescriptorSet`, **zero** `VkDescriptorSet`s); `1x R32_UINT` Visibility Buffer deferred shading + analytical screen-space barycentric derivatives + Kaplanyan/Toksvig geometric specular AA (`ENGINE_VISBUF`, `ENGINE_HIZ`, live `Y` toggle; **`2.32x` faster World 2**, **`2.03x` faster World 7** — see [BENCHMARK_V02_M3.md](BENCHMARK_V02_M3.md)). |
| **`v0.2-M4`** | **Done** | **`src/lighting/` 2026 `SSILVB` + 4-Level Split Radiance Cascades (`HRC`) GI** | Descriptorless BDA Global Illumination & look kernel (`include/lighting/`, `src/lighting/`, `0` `VkDescriptorSet`s, `0.00 MiB` lazy cost on `World 0`); 32-bit `SSILVB` visibility bitmasks (`countbits` thickness-aware AO, Jimenez multi-bounce AO, Lagarde specular occlusion, bent normals & local bounce) + 4-cascade Lambert equal-area Split `HRC` (`102,400` analytical/DEM rays at half-res inside `1` single `40 MiB` UMA slab on `UMA_IGPU`; `412,160` rays at `3/4`-res + `VK_KHR_ray_query` probing on `DISCRETE_DIRECT`; `1x VisBuf + GI` beats `2x MSAA + GI` by `-397 µs` in `8.00 MiB` attachment pool; `ENGINE_GI=auto\|1\|0` & live `H` toggle — see [BENCHMARK_V02_M4.md](BENCHMARK_V02_M4.md)). |
| **`v0.2-M5`** | **Done** | **2026 `AVBD` GPU Physics & Compute `2DGS` Survey** | First-class `src/physics/` Augmented Vertex Block Descent (`AVBD`) unified rigid + soft-body kernel (18-FMA square-root-free $3 \times 3$ $L D L^\top$ up to $k=10^{18}$, Rockafellar-Wets unilateral contacts, SPD-stabilized springs, greedy CSR coloring, fused single-dispatch SLM compute `physics_avbd.comp.slang`, `4` iters on `UMA_IGPU` vs `8` iters on `DISCRETE_DIRECT`, lazy `256 KiB` BDA ring, `ENGINE_PHYSICS` & live `8` toggle); World 8 frame-decoupled Slang compute plane-sweep `ZNCC` MVS + Wendland $C^2$ RBF (`survey_mvs.comp.slang`) and 32-byte Oriented `2DGS` geodetic surfels (`surfel_2dgs.c`, `WavePrefixCountBits` culling, Mip-2DGS anti-aliasing, Early-Z fragment shading + live GI, `ENGINE_2DGS` & live `7` toggle — see [BENCHMARK_V02_M5.md](BENCHMARK_V02_M5.md)). |
