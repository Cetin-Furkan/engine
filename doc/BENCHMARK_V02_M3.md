# Engine `v0.2` Milestone 3 Architecture & Four-Way Benchmark Report (`v0.1` vs `v0.2-M1` vs `v0.2-M2` vs `v0.2-M3`)

**Repository:** `Engine`  
**Date:** November 12, 2026  
**Raw Snapshots:** `benchmarks/v0.1_baseline/` · `benchmarks/v0.2_milestone1/` · `benchmarks/v0.2_milestone2/` · `benchmarks/v0.2_milestone3/`  
**Companion Specifications:** [`doc/V02_GPU_PROFILES_AND_2026_ALGORITHMS.md`](./V02_GPU_PROFILES_AND_2026_ALGORITHMS.md) · [`doc/BENCHMARK_V01_VS_V02_M1.md`](./BENCHMARK_V01_VS_V02_M1.md) · [`doc/BENCHMARK_V02_M2.md`](./BENCHMARK_V02_M2.md)

---

## 1. Executive Summary

`Engine v0.2` Milestone 3 (`v0.2-M3`) replaces legacy per-object CPU vertex draw loops and unindexed 36-byte float vertex rings with a unified **GPU-driven 64-vertex / 124-triangle quantized meshlet scene kernel** (`include/scene/scene.h`, `src/scene/`, and `shaders/scene_*.slang`):

1. **`16 B/vert` Quantized Meshlets & `5.35x` Geometry Compression (`src/scene/meshlet/`):**
   Clusterizes all 12 procedural faceted/rounded solid cages (`examples/demo/render/solid.c`) and arbitrary `.obj` meshes (`examples/demo/render/mesh.c`) into cache-line-aligned `64-byte` `SceneMeshlet` headers (`origin[3]`, `scale[3]`, post-quantization bounding sphere, octahedral `snorm16` normal cone) and `16-byte` `ScenePackedVertex` records (`uint16` local AABB coordinates + octahedral `snorm16` normal + packed `RGBA8`/roughness):
   - All 12 procedural solid cages (`5,964` corners = `209.7 KiB` raw `36 B` unindexed vertices) compress down to **`39.2 KiB` (`26` meshlets — `5.35x` smaller)** in the device-local BDA slab.
   - Retires the legacy `5.06 MiB` 12-slice `ObjCorner` BDA ring and `884 KiB` static BSS buffers in `mesh.c`, cutting World 2 and World 7 device BDA allocations by **`-5.47 MiB`** (`30.92 -> 25.45 MiB` on World 2; `31.67 -> 26.20 MiB` on World 7) while keeping World 0 and World 8 at **`0.0 KiB`** via lazy `scene_ensure_gpu_ready()`.
2. **Subgroup Single-Pass `Hi-Z` Pyramid & Two-Phase GPU Occlusion Culling (`shaders/scene_hiz.comp.slang`, `shaders/scene_cull.comp.slang`):**
   - Evaluates backface normal-cone culling (`dot(axis, w_model) < -sin(half_angle)`), bounding-sphere frustum culling, and Two-Phase Hierarchical-Z (`Hi-Z`) occlusion culling on the GPU, compacting surviving `(instance_id, meshlet_id)` pairs via subgroup `WavePrefixCountBits` straight into `vkCmdDrawIndirectCount` buffers—with **zero** `VkDescriptorPool` or `VkDescriptorSet` allocations (sampled depth/visibility views bind inline via Vulkan 1.4 `vkCmdPushDescriptorSet`).
   - **Adaptive Dual-Architecture `Hi-Z` Policy (`ENGINE_HIZ=auto|1|0`) & GPU-Predicated Phase-2 Skip:** Avoids burning `3.36 MiB` full-screen depth prepass bandwidth on sparse `UMA_IGPU` scenes (`< 128` meshlets on `UMA_IGPU`, `< 64` on `DISCRETE_DIRECT`), and predicates the post-Phase-2 `Hi-Z` rebuild directly on the GPU (`((uint *)pc.cull_slot_bda)[1] > 0u`) so steady-state frames with zero newly disoccluded meshlets skip re-reading the full-screen depth buffer.
3. **`1x` `R32_UINT` Visibility Buffer (`VisBuf`) Deferred Shading & Analytical Specular AA (`shaders/scene_visbuf_resolve.frag.slang`):**
   - Packs `((instance_id + 1) : 14) | (meshlet_id : 11) | (prim_id : 7)` into a `1x` `VK_FORMAT_R32_UINT` attachment (`0u` clear sentinel) and shades each visible pixel **once** (`0%` $2\times 2$ quad helper-lane waste) via analytical perspective-correct barycentrics $(\lambda_0, \lambda_1, \lambda_2)$, closed-form screen-space partial derivatives $(\partial\lambda/\partial x, \partial\lambda/\partial y)$ driving Kaplanyan/Toksvig geometric specular anti-aliasing, and `SV_Depth` output for exact compositing with 3D cubes, grids, and 2D overlays (`ENGINE_VISBUF=auto|1|0`, live toggle on `Y`).
   - At `1x` (`ENGINE_MSAA=1`), `VisBuf` auto-activates and shrinks attachment memory (`pass_pool_mib`) from `128.00 MiB` (`v0.1` `8x`) / `32.00 MiB` (`M1/M2` `2x`) down to **`8.00 MiB` (`-93.8%` vs `v0.1`)**.
4. **2x End-to-End Frame Speedup on 3D Meshlet Worlds (`UMA_IGPU`):**
   - **World 2 (`3D Solids Gravity & Squash`)**: `gpu_total_us` drops from `4,724.91 µs` (`v0.1`) and `4,095.88 µs` (`M2`) to **`2,033.22 µs`** (**`-57.0%`, `2.32x` faster**); `gpu_render_us` drops from `1,829.09 µs` (`v0.1`) to **`690.72 µs`** (**`-62.2%`, `2.65x` faster**).
   - **World 7 (`Procedural Kinetic Cathedral`)**: `gpu_total_us` drops from `5,610.67 µs` (`v0.1`) and `5,545.08 µs` (`M2`) to **`2,757.29 µs`** (**`-50.9%`, `2.03x` faster**); `gpu_render_us` drops from `2,934.42 µs` (`v0.1`) to **`1,269.99 µs`** (**`-56.7%`, `2.31x` faster**).
   - **World 9 (`Interactive Full Sandbox`)**: GPU frustum culling rejects `15/26` (`57.7%`) offscreen meshlets on the GPU, dropping `gpu_total_us` to **`1,674.56 µs`** (**`-19.1%` vs `v0.1`, `-10.5%` vs `M2`**).

---

## 2. Mathematical & Data-Oriented Architecture

### 2.1 `16 B` `ScenePackedVertex` & `64 B` `SceneMeshlet` Layout

Every meshlet caps local unique vertices at `64` and triangles at `124` (packed 3-per-`uint32_t` as `i0 | (i1 << 8) | (i2 << 16)`), fitting local indices in 7 bits (`prim_id < 124`) for Visibility Buffer packing and guaranteeing full L1/shared-memory locality:

| Structure | Size / Align | Fields | Compression / Mathematical Invariant |
| :--- | ---: | :--- | :--- |
| `ScenePackedVertex` | `16 B` (`alignas(16)`) | `uint16_t px, py, pz, pad_u16`<br>`int16_t oct_nx, oct_ny`<br>`uint32_t rgba8` | **`2.25x` smaller** per unique vertex (`5.35x` after local vertex weld + `uint8` indexing vs `36 B` unindexed `SceneCorner`); single 128-bit BDA vector load. |
| `SceneMeshlet` | `64 B` (`alignas(64)`) | `origin[3]`, `vert_offset`<br>`scale[3]`, `tri_offset`<br>`center[3]`, `radius`<br>`cone_axis_oct[2]`, `cone_cutoff_s16`<br>`vert_count`, `tri_count`, `mesh_id`, `flags` | Exact 64-byte CPU/GPU cache line; bounding sphere `(center, radius)` is computed **post-dequantization** so 16-bit rounding never causes false frustum or `Hi-Z` clipping. |
| `SceneInstance` | `64 B` (`alignas(64)`) | `origin_{x,y,z}`, `uniform_scale`, `pitch_rad`, `yaw_rad`, `squash`, `roughness`, `selected`, `depth_bias`, `mat_index`, `mesh_id`, `first_meshlet`, `meshlet_count`, `flags`, `pad0` | Exact 64-byte per-instance transform & material descriptor in ring-buffered `SceneCullSlot`. |

#### Octahedral `snorm16` Normal Codec (`quantize.c` & `scene_common.slang`)
Unit normals $n \in S^2$ project onto the $\ell_1$ octahedron via $p = n_{xy} / (\|n_x\| + \|n_y\| + \|n_z\|)$, folding the lower hemisphere ($n_z < 0$) across the diagonals via $(1 - |p_{yx}|) \odot \operatorname{sign}_{\ge 0}(p_{xy})$ into `int16_t[2]` (`[-32767, +32767]`). Across 40,000 unit-sphere directions verified in `tests/test_scene.c`, worst-case reconstruction error is **`< 0.04°`** (`0.08°` hard test gate).

#### Conservative Normal-Cone Backface Bound (`builder.c` & `scene_cull.comp.slang`)
Given meshlet average face normal $a \in S^2$ and maximum face deviation $\cos\theta_{\max} = \min_t (a \cdot n_t)$, a meshlet is provably 100% backfacing under orthographic/view projection along model-space view vector $w_{\text{model}} = \operatorname{normalize}(R_{\text{pose}}^\top S^{-1} R_{\text{view}}^\top \hat{z})$ whenever:
$$a \cdot w_{\text{model}} < -\sin\theta_{\max}$$
`builder.c` pads $\sin\theta_{\max}$ by `+0.015` before encoding `cone_cutoff_s16` to absorb octahedral `snorm16` quantization of `cone_axis_oct`, verified across 512 random camera orientations with **zero** false-positive culls in `tests/test_scene.c`.

---

### 2.2 Analytical Perspective-Correct Barycentrics & Screen-Space Partial Derivatives

In `VisBuf` mode (`shaders/scene_visbuf_resolve.frag.slang`), the fullscreen resolve shader loads `uint vis_id` from `VK_FORMAT_R32_UINT`, unpacks `(inst_id, meshlet_id, prim_id)`, fetches the 3 `ScenePackedVertex` records via BDA, transforms clip coordinates $c_k = (x_k, y_k, z_k, w_k)$, and solves perspective-correct barycentrics analytically without $2\times 2$ quad finite differences (`ddx`/`ddy`):

Let $s_k = (x_k / w_k, y_k / w_k)$ be NDC vertex positions, $e_1 = s_1 - s_0$, $e_2 = s_2 - s_0$, and $D = e_{1x} e_{2y} - e_{1y} e_{2x}$. Screen-linear barycentrics $(w_{\text{scr}}, u_{\text{scr}}, v_{\text{scr}})$ have constant analytical pixel partials:
$$\frac{\partial u_{\text{scr}}}{\partial x_{\text{px}}} = \frac{2 e_{2y}}{W \cdot D}, \qquad \frac{\partial u_{\text{scr}}}{\partial y_{\text{px}}} = \frac{-2 e_{2x}}{H \cdot D}, \qquad \frac{\partial v_{\text{scr}}}{\partial x_{\text{px}}} = \frac{-2 e_{1y}}{W \cdot D}, \qquad \frac{\partial v_{\text{scr}}}{\partial y_{\text{px}}} = \frac{2 e_{1x}}{H \cdot D}$$
Weighting by reciprocal clip-$w$ ($n = (w_{\text{scr}}/w_0,\, u_{\text{scr}}/w_1,\, v_{\text{scr}}/w_2)$, $Z^{-1} = n_0 + n_1 + n_2$, $\lambda = n / Z^{-1}$), the quotient rule yields exact perspective-correct pixel derivatives:
$$\frac{\partial \lambda_i}{\partial x_{\text{px}}} = \frac{1}{Z^{-1}}\left(\frac{\partial n_i}{\partial x_{\text{px}}} - \lambda_i \sum_{j=0}^{2} \frac{\partial n_j}{\partial x_{\text{px}}}\right), \qquad \frac{\partial \lambda_i}{\partial y_{\text{px}}} = \frac{1}{Z^{-1}}\left(\frac{\partial n_i}{\partial y_{\text{px}}} - \lambda_i \sum_{j=0}^{2} \frac{\partial n_j}{\partial y_{\text{px}}}\right)$$
These feed directly into Kaplanyan/Toksvig normal-variance roughness filtering:
$$\sigma_n^2 = \min\!\left(0.18,\, 0.35\left(\left\|\frac{\partial N}{\partial x_{\text{px}}}\right\|^2 + \left\|\frac{\partial N}{\partial y_{\text{px}}}\right\|^2\right)\right), \qquad \alpha_{\text{AA}} = \sqrt{\operatorname{clamp}(\alpha^2 + \sigma_n^2,\, 0.04,\, 1.0)}$$
Verified in `tests/test_scene.c` against central finite differences at $\Delta = 10^{-2}\text{ px}$ to `< 2 \times 10^{-4}` precision under strong perspective foreshortening ($w = \{1.1, 2.7, 4.3\}$).

---

## 3. Four-Way Empirical Results (`v0.1` vs `v0.2-M1` vs `v0.2-M2` vs `v0.2-M3`)

All four milestone snapshots were captured on the same physical **Intel Core i7-1165G7 + Intel Iris Xe (`TGL GT2`, 7.4 GiB shared UMA RAM, Linux `7.3.0-rc6`, Mesa `26.2.6`, Vulkan `1.4.354`)** at `1280x688` (`75` live frames, `65` retired hardware `VkQueryPool` samples per world).

### 3.1 GPU Hardware Timings Across All Five Benchmark Worlds (`UMA_IGPU`)

| World Workload | Metric | `v0.1` Baseline | `v0.2-M1` | `v0.2-M2` | **`v0.2-M3`** | Net Change (`v0.1` -> `M3`) |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: |
| **World 0** — *Pure Sealed Empty Engine* | `gpu_total_us` (avg)<br>`gpu_render_us` (avg)<br>`scene_slab_kib` | 520.99 µs<br>419.55 µs<br>— | 472.71 µs<br>369.23 µs<br>— | 477.43 µs<br>378.47 µs<br>— | **351.58 µs**<br>**292.12 µs**<br>**0.0 KiB** | **-32.5% (`1.48x` faster)**<br>**-30.4%**<br>**Zero lazy overhead** |
| **World 2** — *3D Solids Gravity & Squash*<br>(`meshlets=23/26`, `cone=3`) | `gpu_total_us` (avg)<br>`gpu_render_us` (avg)<br>`bda_dev_mib` (used) | 4,724.91 µs<br>1,829.09 µs<br>31.14 MiB | 4,065.49 µs<br>1,178.96 µs<br>30.92 MiB | 4,095.88 µs<br>1,187.61 µs<br>30.92 MiB | **2,033.22 µs**<br>**690.72 µs**<br>**25.45 MiB** | **-57.0% (`2.32x` faster)**<br>**-62.2% (`2.65x` faster)**<br>**-5.69 MiB BDA** |
| **World 7** — *Procedural Kinetic Cathedral*<br>(`meshlets=24/26`, `cone=2`) | `gpu_total_us` (avg)<br>`gpu_render_us` (avg)<br>`bda_dev_mib` (used) | 5,610.67 µs<br>2,934.42 µs<br>31.89 MiB | 4,200.88 µs<br>1,801.81 µs<br>31.67 MiB | 5,545.08 µs<br>2,387.18 µs<br>31.67 MiB | **2,757.29 µs**<br>**1,269.99 µs**<br>**26.20 MiB** | **-50.9% (`2.03x` faster)**<br>**-56.7% (`2.31x` faster)**<br>**-5.69 MiB BDA** |
| **World 8** — *Baklan & Beşparmak Dağı DEM*<br>(`104` `.epack` loads, `40` async CQEs) | `wall_ms` (75 frames)<br>`uring_us` (frame stall)<br>`scene_slab_kib` | 674.12 ms<br>5,321 µs<br>— | 655.95 ms<br>5,276 µs<br>— | 648.26 ms<br>0 µs<br>— | **616.50 ms**<br>**0 µs**<br>**0.0 KiB** | **-8.5% wall time**<br>**-100.0% CPU stall**<br>**Zero lazy overhead** |
| **World 9** — *Interactive Full Sandbox*<br>(`meshlets=11/26`, **`frust=15` [`57.7%` culled]**) | `gpu_total_us` (avg)<br>`gpu_compute_us` (avg)<br>`gpu_render_us` (avg) | 2,069.99 µs<br>1,373.65 µs<br>573.77 µs | 2,225.29 µs<br>1,563.58 µs<br>584.08 µs | 1,870.59 µs<br>1,315.87 µs<br>490.68 µs | **1,674.56 µs**<br>**1,097.81 µs**<br>**491.22 µs** | **-19.1%**<br>**-20.1%**<br>**-14.4%** |

---

### 3.2 Geometry Memory & Static Footprint Compression

| Allocation / Buffer | Legacy `v0.1`–`M2` (`solid.c` + `mesh.c`) | `v0.2-M3` Unified `src/scene/` Kernel | Net Reduction |
| :--- | ---: | ---: | ---: |
| **12 Procedural Solid Cages (`5,964` corners)** | `209.67 KiB` (`36 B/vert` unindexed BDA) | **`39.20 KiB` (`26` meshlets, `16 B/vert` indexed)** | **`5.35x` smaller (`-81.3%`)** |
| **Legacy 12-Slice `.obj` BDA Ring (`mesh.c`)** | `5,184.00 KiB` (`5.06 MiB` always allocated) | **`0.00 KiB` (unified `832 KiB` meshlet slab)** | **`-4.25 MiB` BDA** |
| **Static CPU BSS Arrays in `mesh.c`** | `884.74 KiB` (`s_pos` + `s_vn` + `s_out`) | **`0.00 KiB` (transient parse, freed on bake)** | **`-884.74 KiB` (`-100%` BSS)** |
| **World 0 / World 8 Scene BDA Reservation** | `0.00 KiB` (`solid`) / `0.00 KiB` | **`0.00 KiB` (lazy `scene_ensure_gpu_ready`)** | **Zero idle cost preserved** |
| **`1x` `VisBuf` Attachment Pool (`pass_pool_mib`)** | `128.00 MiB` (`v0.1` `8x`) / `32.00 MiB` (`2x`) | **`8.00 MiB` (`ENGINE_MSAA=1` auto-`VisBuf`)** | **`-93.8%` vs `v0.1` (`16x` smaller)** |

---

### 3.3 Rendering Path & Cross-Profile Matrix (`VisBuf`, `Two-Phase Hi-Z`, and `DISCRETE_DIRECT`)

All four orthogonal combinations of `(VisBuf, Two-Phase Subgroup Hi-Z)` plus the `DISCRETE_DIRECT` dedicated-VRAM execution profile were verified on live hardware (`benchmarks/v0.2_milestone3/*.log`):

| Mode & Command (`World 7`, `18` Objects, `26` Meshlets) | Active Pipeline Path | `pass_pool_mib` | `bda_dev_mib` / `bda_stg_mib` | `gpu_total_us` (avg / min) | `gpu_render_us` | Key Verified Behavior |
| :--- | :--- | ---: | :--- | ---: | ---: | :--- |
| **`UMA_IGPU` Default (`2x MSAA` Indirect)**<br>`./build/release/bin/engine --world=7 --bench=75` | `scene_cull` (Cone + Frustum) -> `2x MSAA` `vkCmdDrawIndirectCount` | `32.00 MiB` | `26.20 MiB` / `0.00 MiB` | **`2,757.29 µs`** (`2,179.32 µs`) | **`1,269.99 µs`** | Fastest path for `< 128` meshlets on `UMA_IGPU`: zero offscreen prepass bandwidth, `2/26` backfacing meshlets cone-culled. |
| **`1x VisBuf` + `Two-Phase Subgroup Hi-Z`**<br>`ENGINE_MSAA=1 ENGINE_HIZ=1 .../engine --world=7 --bench=75` | `scene_cull` P1 -> `1x R32_UINT` VisBuf -> `scene_hiz` (`WaveReadLaneAt`) -> `scene_cull` P2 -> Predicated `Hi-Z` -> Analytical Resolve + Specular AA | **`8.00 MiB`** | `27.32 MiB` / `0.00 MiB` | **`3,518.69 µs`** (`3,203.75 µs`) | **`1,192.11 µs`** | Full GPU-driven Visibility Buffer + Two-Phase Subgroup `Hi-Z` + Kaplanyan/Toksvig Specular AA in **`8.00 MiB`** attachment pool; GPU-predicated Phase-2 skips redundant `3.36 MiB` depth scan when `p2=0`. |
| **`1x VisBuf` Auto (`ENGINE_MSAA=1`)**<br>`ENGINE_MSAA=1 .../engine --world=7 --bench=75` | `scene_cull` (Cone + Frustum) -> Single-Pass `1x R32_UINT` VisBuf -> Analytical Barycentric Resolve | **`8.00 MiB`** | `26.20 MiB` / `0.00 MiB` | `4,665.69 µs` (`4,312.55 µs`) | `2,002.98 µs` | Auto-activates `VisBuf` at `1x` MSAA (`visbuf=1 hiz_pass=0`), shading every visible pixel once (`0%` helper-lane waste) without allocating `s_hiz_buf` below `128` meshlets. |
| **`2x MSAA` + Forced `Two-Phase Hi-Z`**<br>`ENGINE_HIZ=1 .../engine --world=7 --bench=75` | `1x` Depth Prepass + Subgroup `Hi-Z` + P2 Disocclusion + `2x MSAA` Indirect | `32.00 MiB` | `27.32 MiB` / `0.00 MiB` | `5,140.11 µs` (`4,355.94 µs`) | `1,604.60 µs` | Demonstrates why `ENGINE_HIZ=auto` defers offscreen depth prepasses on `UMA_IGPU` until `>= 128` meshlets amortize the `1280x688` prepass cost. |
| **`DISCRETE_DIRECT` (`4x MSAA` + Staged VRAM Slab)**<br>`ENGINE_GPU_PROFILE=discrete .../engine --world=7 --bench=45` | `upload_slab_bytes` staged `vkCmdCopyBuffer2` -> `4x MSAA` `vkCmdDrawIndirectCount` | `64.00 MiB` | `17.48 MiB` / `17.21 MiB` | `4,305.59 µs` (`3,879.95 µs`) | `2,033.75 µs` | Verifies dedicated PCIe staging upload into unmapped `DEVICE_LOCAL` VRAM meshlet slab (`meshlets=22/26`, `cone=4`). |

---

## 4. Build Health, Mathematical Test Gates & Binary Footprint

| Verification Gate | Result in `v0.2-M3` |
| :--- | :--- |
| Compiler Warnings (`-std=c23 -Wall -Wextra -Werror=unused-function -Wmissing-declarations`) | **`0` warnings** (`debug` & `release`) |
| Linker Dead-Code Gate (`-Wl,--gc-sections --print-gc-sections` across `src/`, incl. `src/scene`) | **`0` dead functions** |
| Vulkan Descriptor Pools / Descriptor Sets (`VkDescriptorPool` / `VkDescriptorSet`) | **`0` allocated** (`vkCmdPushDescriptorSet` inline push descriptors + BDA) |
| Unit Test Suites (`make test` — **10 binaries**, incl. `build/test_scene`) | **`10 / 10 PASS`** |
| Octahedral `snorm16` Normal Codec Max Error (`40,000` sphere rays, `test_scene`) | **`< 0.04°`** (gate: `<= 0.08°`) |
| 16-Bit Local AABB Dequantization Max Error (`[-120, +380] m` extent, `test_scene`) | **`<= 1/(2 * 65535)` per axis** (`< 4 mm` over `500 m`) |
| Post-Quantization Meshlet Sphere & Normal-Cone False-Positive Cull Rate (`512` views, `test_scene`) | **`0 / 512` (`0.00%` false culls)** |
| Analytical Perspective-Correct Barycentric Partial Derivatives (`dlambda/dx`, `dlambda/dy`, `test_scene`) | **`< 2e-4` error vs central finite differences** |
| Stripped + LTO Release ELF (`build/release/bin/engine`, all SPIR-V 1.6 & font `#embed`ded) | **`2,512,576 B` (`2.40 MiB`)** |
