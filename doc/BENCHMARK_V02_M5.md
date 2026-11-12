# Engine `v0.2` Milestone 5 (Final `v0.2` Release) — Architecture & Six-Way Benchmark Report (`v0.1` vs `M1` vs `M2` vs `M3` vs `M4` vs `v0.2-M5`)

**Repository:** `Engine`  
**Date:** November 12, 2026  
**Raw Snapshots:** `benchmarks/v0.1_baseline/` · `benchmarks/v0.2_milestone1/` · `benchmarks/v0.2_milestone2/` · `benchmarks/v0.2_milestone3/` · `benchmarks/v0.2_milestone4/` · `benchmarks/v0.2_milestone5/`  
**Companion Specifications:** [`doc/V02_GPU_PROFILES_AND_2026_ALGORITHMS.md`](./V02_GPU_PROFILES_AND_2026_ALGORITHMS.md) · [`doc/BENCHMARK_V01_VS_V02_M1.md`](./BENCHMARK_V01_VS_V02_M1.md) · [`doc/BENCHMARK_V02_M2.md`](./BENCHMARK_V02_M2.md) · [`doc/BENCHMARK_V02_M3.md`](./BENCHMARK_V02_M3.md) · [`doc/BENCHMARK_V02_M4.md`](./BENCHMARK_V02_M4.md)

---

## 1. Executive Summary

`Engine v0.2` Milestone 5 (`v0.2-M5`) completes the full **`Engine v0.2` Dual-Architecture & 2026 Mathematical Upgrade** planned across `M1`–`M5`. It promotes **coupled rigid/soft-body simulation** and **GPU photogrammetric / geodetic surfel reconstruction** into first-class, descriptorless BDA subsystems inside `src/`:

1. **First-Class Unified GPU/CPU Physics Kernel — 2026 Augmented Vertex Block Descent (`AVBD`, `include/physics/physics.h`, `src/physics/`, `shaders/physics_avbd.comp.slang`):**
   - Replaces ad-hoc sequential pairwise projections and uncolored Gauss-Seidel spring loops with **Augmented Vertex Block Descent** (Giles, Diaz & Yuksel, SIGGRAPH 2025/2026): per-vertex $3 \times 3$ symmetric positive-definite block minimization paired with Rockafellar-Wets Augmented Lagrangian dual updates and warm-started multiplier caching (`AvbdWarmCache`, $0.82$ temporal decay).
   - **Square-Root-Free $3 \times 3$ $L D L^\top$ Register Solver (`avbd_solve_sym3x3`)**: Replaces $3 \times 3$ cofactor / determinant inversion ($\det(H) \propto k^3$, which overflows IEEE-754 `fp32` at $k > 10^{12}$) with an exact 18-FMA unpivoted $L D L^\top$ factorization that remains finite and accurate up to $k = 10^{18}$ (`1000:1` mass-ratio stack verified in `tests/test_physics.c`).
   - **Fused Single-Dispatch SLM Compute (`physics_avbd.comp.slang`)**: Caches vertex positions and inertial targets in `groupshared` SLM (`<= 256` vertices) and loops over all primal-dual iterations and greedy graph colors inside **one `vkCmdDispatch`**, cutting host/GPU barrier overhead from $O(N_{\text{iter}} \cdot N_{\text{colors}})$ command-buffer dispatches to **$1$ dispatch per step**.
   - **Lazy `256 KiB` BDA Ring**: Allocates `0.00 MiB` on `World 0`, `World 8`, and `World 9` at rest, spinning up a `4 x 64 KiB = 256 KiB` timeline-ringed BDA slab only when active bodies step (`ENGINE_PHYSICS=auto|gpu|cpu`, live **`KEY_8`** toggle).

2. **2026 Geodetic & Photogrammetric Reconstruction — Tiled Slang Compute `ZNCC` MVS + Oriented `2DGS` Surfels (`src/backend/terrain/survey/`, `shaders/survey_*.slang`):**
   - **Frame-Decoupled GPU Plane-Sweep `ZNCC` MVS (`shaders/survey_mvs.comp.slang` + `gpu_survey.c`)**: Moves $5 \times 5$ Zero-Mean Normalized Cross-Correlation (`ZNCC`), sub-step 3-point parabolic elevation peak refinement, Brown-Conrady radial/tangential camera projection, and Wendland $C^2$ compact-support RBF geodetic fusion onto an `8x8` Slang 2026 compute pipeline (`0.00 ms` render-thread stall, writing `.epack` tiles directly into BDA memory).
   - **Oriented 2D Gaussian Splatting (`2DGS`) Geodetic Surfels (Huang et al., SIGGRAPH 2024–2026)**: Represents steep limestone cliffs, karst ridges, RTK-GNSS Ground Control Points (`GCP`s), stereo tie-points, and breaklines as compact **32-byte** BDA surfels (`TerrainSurfel2DGS`) parameterized by an exact Duff-Frisvad orthonormal tangent basis $(T_u, T_v, N)$ (`oct_snorm8` + `16-bit` strike angle + `2x fp16` tangent scales).
   - **Zero-`OpKill` Hardware Early-Z & Deterministic `WavePrefixCountBits` Compaction**: Culls surfels in `survey_2dgs_cull.comp.slang` via subgroup ballot prefix sums (`0` atomic race order flicker across frames), applies energy-conserving **Mip-2DGS** low-pass screen-space dilation in `survey_2dgs.vert.slang`, and shades exact ray-plane 2D Gaussians in `survey_2dgs.frag.slang` with live `SSILVB` + Split `HRC` canyon GI while keeping `100%` hardware Early-Z intact (`ENGINE_2DGS=auto|1|0`, `ENGINE_SURVEY_MANIFEST`, live **`KEY_7`** toggle).

3. **Single `40.00 MiB` Unified UMA Slab Preserved Across All Worlds**:
   Even with **all five `Engine v0.2` GPU kernels active simultaneously** (`.epack` `io_uring` terrain + quantized `16 B/vert` meshlets + Subgroup `Hi-Z`/`VisBuf` + `SSILVB`/`HRC` Global Illumination + `AVBD` GPU physics + `2DGS`/Compute `MVS`), **every benchmark world (`Worlds 0, 2, 3, 7, 8, 9`) fits inside 1 single `40.00 MiB` unified `UMA_IGPU` `VkDeviceMemory` slab** (`0` overflow allocations, `0` `VkDescriptorSet`s, `2.91 MiB` stripped LTO binary).

---

## 2. Mathematical & Data-Oriented Architecture

### 2.1 2026 Augmented Vertex Block Descent (`AVBD`) & $L D L^\top$ Stability (`src/physics/`)

Given timestep $h$, vertex mass $m_i$, and inertial target $y_i = x_i^t + h v_i^t + h^2 g$, `AVBD` minimizes the augmented variational energy:

$$F(x, \lambda) = \sum_i \frac{m_i}{2 h^2} \|x_i - y_i\|^2 + \sum_{c \in \mathcal{C}} \Phi_c(x, \lambda_c, k_c)$$

Within each conflict-free greedy vertex color (`avbd_color_graph`, built over CSR vertex-to-constraint incidence `avbd_build_csr`), every dynamic vertex $i$ assembles its local force $f_i = -\nabla_{x_i} F \in \mathbb{R}^3$ and $3 \times 3$ SPD Hessian $H_i \in \mathbb{R}^{3 \times 3}$ in registers:

$$H_i = \left(\frac{m_i}{h^2} + 10^{-7}\right) I_3 + \sum_{c \ni i} H_{c,i}, \qquad H_i \Delta x_i = f_i$$

- **Rockafellar-Wets Exact Unilateral Contact (`AVBD_CON_CONTACT_PLANE`, `AVBD_CON_CONTACT_PAIR`)**:
  For gap $C(x) = n^\top x - d \ge 0$ with non-positive dual compression multiplier $\lambda \le 0$, the exact smooth-to-active augmented force is $\hat{\lambda} = \min(0,\, \lambda + k C(x))$, contributing rank-1 normal stiffness $k\, n n^\top$ + isotropic Coulomb tangent damping when $\hat{\lambda} < 0$, and clamping $\lambda \leftarrow 0$ cleanly on separation.
- **SPD-Stabilized Hookean Springs (`AVBD_CON_SPRING_3D`)**:
  Under compression ($C(x) = \|d\| - L_0 < 0$), geometric stiffness $\frac{\hat{\lambda}}{\|d\|}(I_3 - \hat{d}\hat{d}^\top)$ has negative transverse eigenvalues that break Cholesky/cofactor solvers; `AVBD` clamps $\max(\hat{\lambda}, 0)$ in the geometric curvature term so $H_{c,i} \succ 0$ unconditionally under extreme buckling.
- **Square-Root-Free $3 \times 3$ $L D L^\top$ (`avbd_solve_sym3x3`)**:
  Computes $d_0 = H_{00}$, $l_{10} = H_{01}/d_0$, $l_{20} = H_{02}/d_0$, $d_1 = H_{11} - l_{10}^2 d_0$, $l_{21} = (H_{12} - l_{20} l_{10} d_0)/d_1$, and $d_2 = H_{22} - l_{20}^2 d_0 - l_{21}^2 d_1$. Every intermediate term scales linearly ($O(k^1)$) rather than cubically ($O(k^3)$), preventing `fp32` `Inf`/`NaN` blowups across stiffnesses up to $10^{18}$.

| Structure (`include/physics/physics.h`) | Size / Align | Purpose & Bandwidth Invariant |
| :--- | ---: | :--- |
| `AvbdVertex` | `64 B` (`alignas(64)`) | Exact 1-cache-line vertex state (`pos`, `inv_mass`, `prev_pos`, `radius`, `vel`, `inert_pos`, `restitution`, `friction`, `flags`). |
| `AvbdConstraint` | `64 B` (`alignas(64)`) | Exact 1-cache-line primal-dual constraint (`vert_a`, `vert_b`, `kind`, `axis/anchor`, `rest_or_offset`, `compliance`, `stiffness`, `k_min/k_max`, `beta`, `lambda[3]`, `warm_hash`). |
| `AvbdStepHeader` | `128 B` (`alignas(64)`) | BDA pointers (`verts_bda`, `cons_bda`, `csr_row_bda`, `csr_col_bda`, `color_verts_bda`) + `color_start[16]` / `color_count[16]` schedule. |

---

### 2.2 Oriented `2DGS` Geodetic Surfels & Tiled Compute `ZNCC` MVS (`src/backend/terrain/survey/`)

Unlike 3D Gaussian Splatting (`3DGS`), whose affine screen-space EWA covariance projection warps and pops under steep terrain grazing angles, **Oriented 2D Gaussian Splatting (`2DGS`)** embeds planar elliptical Gaussian disks directly in world space:

$$P(u, v) = c + (\sigma_u^{\text{eff}} u)\, T_u + (\sigma_v^{\text{eff}} v)\, T_v, \qquad G(u, v) = \kappa_{\text{mip}} \exp\!\left(-\tfrac{1}{2}(u^2 + v^2)\right) w(u^2 + v^2)$$

where $(T_u, T_v, N)$ is reconstructed branchlessly in registers via Duff et al.'s orthonormal basis (`build_frisvad_onb`) rotated by strike angle $\phi \in [0, 2\pi)$, and $\sigma_{\{u,v\}}^{\text{eff}} = \sqrt{\sigma_{\{u,v\}}^2 + \sigma_{\text{AA}}^2}$ with $\kappa_{\text{mip}} = \sqrt{(\sigma_u \sigma_v) / (\sigma_u^{\text{eff}} \sigma_v^{\text{eff}})}$ preserves integrated screen-space energy across arbitrary zoom factors (`0.025x` .. `16x`):

| Structure / Kernel | Footprint | Mathematical / Hardware Property |
| :--- | ---: | :--- |
| `TerrainSurfel2DGS` (`survey.h`) | **`32 B`** (`2` per cache line) | `center_xy` (`8 B`), `elev_m` (`4 B`), `oct_snorm8` normal + `u16` tangent angle (`4 B`), `2x fp16` $(\sigma_u, \sigma_v)$ (`4 B`), `RGBA8` albedo+opacity (`4 B`), `node_id + flags` (`4 B`). |
| `survey_2dgs_cull.comp.slang` | `512 KiB` (`UMA`) / `2.0 MiB` (`dGPU`) | Deterministic single-workgroup `WavePrefixCountBits` compaction (`0` atomic race flicker) into `VkDrawIndirectCommand` (`6` verts/quad). |
| `survey_mvs.comp.slang` | `1.27 MiB` lazy BDA workspace | `8x8` tiled plane-sweep $5 \times 5$ `ZNCC` + 3-point parabolic peak interpolation + Wendland $C^2$ $(1-r)^4(4r+1)$ RBF geodetic blending. |

---

## 3. Six-Way Empirical Results (`v0.1` -> `M1` -> `M2` -> `M3` -> `M4` -> `v0.2-M5`)

All six milestone snapshots were captured on the exact same physical **Intel Core i7-1165G7 + Intel Iris Xe (`TGL GT2`, 7.4 GiB shared UMA RAM, Linux `7.3.0-rc6`, Mesa `26.2.6`, Vulkan `1.4.354`)** at `1280x688` (`75` live frames, `65` retired hardware `VkQueryPool` timestamp samples per world). In `v0.2-M5`, **every v0.2 subsystem (`UMA` Zero-Copy BDA, `.epack` `io_uring`, `16B` Meshlets, `SSILVB`+`HRC` GI, GPU `AVBD` Physics, and `2DGS`/Compute `MVS`) is enabled simultaneously by default**.

### 3.1 Complete `v0.1` -> `v0.2` Six-Milestone Progression (`UMA_IGPU`)

| World Workload | Metric | `v0.1` Baseline | `v0.2-M1`<br>*(Profiles)* | `v0.2-M2`<br>*(Async/.epack)* | `v0.2-M3`<br>*(Meshlets/VisBuf)* | `v0.2-M4`<br>*(+2026 GI)* | **`v0.2-M5` (Final `v0.2`)**<br>*(+GPU `AVBD` & `2DGS`/MVS)* | Net `v0.1 -> v0.2` Outcome |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | :--- |
| **World 0**<br>*Pure Sealed Empty Engine* | `gpu_total_us`<br>`bda_dev_mib`<br>`pass_pool_mib` | 520.99 µs<br>0.00/0 MiB<br>128.00 MiB | 472.71 µs<br>0.00/32 MiB<br>32.00 MiB | 477.43 µs<br>0.00/32 MiB<br>32.00 MiB | 351.58 µs<br>0.00/32 MiB<br>32.00 MiB | 497.15 µs<br>0.00/40 MiB<br>32.00 MiB | **`469.51 µs`** (`min 422.66`)<br>**`0.00 / 40.00 MiB`**<br>**`32.00 MiB`** (`8 MiB` @ `1x`) | **`-9.9%` GPU time**, **`-75%` pass RAM**, **`0.00 MiB`** lazy GI/Physics/2DGS overhead. |
| **World 2**<br>*3D Gravity & Elastic Solids* (`9` bodies) | `gpu_total_us`<br>`gpu_render_us`<br>`bda_dev_mib` | 4,724.91 µs<br>1,829.09 µs<br>31.14/64 MiB | 4,065.49 µs<br>1,178.96 µs<br>30.92/32 MiB | 4,095.88 µs<br>1,187.61 µs<br>30.92/32 MiB | 2,033.22 µs<br>690.72 µs<br>25.45/32 MiB | 5,458.00 µs<br>1,688.41 µs<br>36.35/40 MiB | **`5,299.38 µs`** (`189 FPS`)<br>**`1,613.72 µs`**<br>**`36.60 / 40.00 MiB` (`1` slab)** | Adds full 4-cascade `HRC` GI + `SSILVB` + GPU `AVBD` (`phys_kib=256`) at **`189 FPS`** in 1 UMA slab! |
| **World 7**<br>*Procedural Kinetic Cathedral* (`18` bodies) | `gpu_total_us`<br>`gpu_render_us`<br>`bda_dev_mib` | 5,610.67 µs<br>2,934.42 µs<br>31.89/64 MiB | 4,200.88 µs<br>1,801.81 µs<br>31.67/32 MiB | 5,545.08 µs<br>2,387.18 µs<br>31.67/32 MiB | 2,757.29 µs<br>1,269.99 µs<br>26.20/32 MiB | 6,597.96 µs<br>2,584.68 µs<br>37.10/40 MiB | **`6,552.91 µs`** (`153 FPS`)<br>**`2,497.06 µs`** (`6,206.94 µs` @ `VisBuf`)<br>**`37.35 / 40.00 MiB` (`1` slab)** | 18-body `AVBD` (`2` colors, `1` SLM dispatch) + `102,400`-ray `HRC` GI + `SSILVB` at **`153–161 FPS`**! |
| **World 8**<br>*24x24 km DEM + Survey + `2DGS`* | `gpu_total_us`<br>`uring_us` (stall)<br>Terrain + BDA | 7,264.61 µs<br>5,321 µs<br>211.6 MiB | 7,058.31 µs<br>5,276 µs<br>75.99 MiB | 7,027.84 µs<br>**0 µs**<br>49.15 MiB | 6,625.10 µs<br>**0 µs**<br>49.15 MiB | 9,022.97 µs<br>**0 µs**<br>28.05/40 MiB | **`8,572.37 µs`** (`min 7,115.78`)<br>**`0 µs`** (`49` async CQEs)<br>**`28.61 / 40.00 MiB` (`1` slab)** | **`-86.5%` memory vs `v0.1`**, **`0 µs` I/O stall**, + Canyon GI + `1,681` `2DGS` surfels + GPU `ZNCC` MVS! |
| **World 9**<br>*Full Interactive Sandbox* | `gpu_total_us`<br>`gpu_render_us`<br>`bda_dev_mib` | 2,069.99 µs<br>573.77 µs<br>32.82/64 MiB | 2,225.29 µs<br>584.08 µs<br>32.60/32 MiB | 1,870.59 µs<br>490.68 µs<br>32.60/32 MiB | 1,674.56 µs<br>491.22 µs<br>27.13/32 MiB | 5,151.95 µs<br>1,465.85 µs<br>38.03/40 MiB | **`5,193.10 µs`** (`193 FPS`)<br>**`1,479.52 µs`**<br>**`38.03 / 40.00 MiB` (`1` slab)** | **`193 FPS`** with meshlets, GI, 3D CSV plots & SDF text; resting `AVBD` allocates **`0.0 KiB`**. |

---

### 3.2 Milestone 5 Direct A/B Isolation (`AVBD` CPU vs GPU, `2DGS` + Compute `MVS`, & Dual-Profile Split)

All measurements below were captured back-to-back on the exact same release binary (`benchmarks/v0.2_milestone5/ab_matrix.log`):

| Workload & Command | Active `M5` Kernel Telemetry | `bda_dev_mib` (`pass_pool`) | `gpu_total_us` (avg / min / max) | Key Architectural Finding |
| :--- | :--- | ---: | ---: | :--- |
| **`World 2` — CPU `AVBD`** (`ENGINE_PHYSICS=cpu`) | `phys_gpu=0` · `4` iters (`9` bodies) | `36.35 / 40.00 MiB` (`32 MiB`) | `5,307.28` / `4,918.18` / `6,964.95 µs` | Exact C23 reference `AVBD` solver (`avbd_solve_step`, $3 \times 3$ $L D L^\top$). |
| **`World 2` — GPU `AVBD`** (`ENGINE_PHYSICS=gpu`) | `phys_gpu=1` · `4` iters · `1` color · `1` SLM dispatch · `256 KiB` | `36.60 / 40.00 MiB` (`32 MiB`) | **`5,290.46`** / `4,932.24` / **`6,692.24 µs`** | Zero-copy UMA coherent BDA harvest (`0` staging copies) + **`-272.7 µs` lower max frame spike**. |
| **`World 3` — 2D Soft-Body `AVBD` (`CPU` vs `GPU`)** | `64` verts · `250` springs/pins · **`7` greedy colors** · `3` dispatches | `8.50 -> 8.75 / 40.00 MiB` | `572.11 µs` (`CPU`) / **`826.64 µs`** (`GPU`) | Solves `250` coupled structural + shear soft-body springs across `7` parallel colors in **`0.83 ms` (`> 1,200 FPS`)**. |
| **`World 7` — `2x MSAA` + `GI` + GPU `AVBD`** | `18` verts · `17` cons · `2` colors · `1` SLM dispatch · `256 KiB` | `37.35 / 40.00 MiB` (`32 MiB`) | **`6,566.92`** / `6,061.87` / **`8,208.44 µs`** | Beats CPU `AVBD` max spike (`8,443.44 µs`) by **`-235 µs`** while keeping all 18 bodies + GI inside `37.35 / 40.00 MiB`. |
| **`World 7` — `1x VisBuf` + `GI` + GPU `AVBD`** (`ENGINE_MSAA=1`) | `visbuf=1` + `gi=1` + `phys_gpu=1` (`1` SLM dispatch) | **`37.35 / 40.00 MiB` (`8.00 MiB`)** | **`6,206.94`** / **`5,898.23`** / **`7,312.45 µs`** | **Fastest full-stack 3D mode (`161 FPS`)**: Meshlets + `VisBuf` + `SSILVB` + `HRC` + GPU `AVBD` in **`8 MiB` pass RAM**! |
| **`World 8` — Summit Survey (`2DGS=0` vs `2DGS=1`, Wide View)** | `mvs_gpu=4` GPU `ZNCC` dispatches · `surfels=16..68 / 1,815` (`1.83 MiB`) | `29.88 / 40.00 MiB` (`32 MiB`) | `10,903.32 -> 11,709.27 µs` (`75f` sweep: **`8,265.13 µs`**) | Non-blocking GPU `survey_mvs.comp` refines 5 tiles (`mvs_gpu=5`) with `0 µs` stall; `survey_2dgs_cull.comp` costs **`+18 µs`** compute and culls `100%` of off-screen surfels when zoomed into flat terrain. |
| **`World 7` & `World 8` — `DISCRETE_DIRECT` Profile** (`ENGINE_GPU_PROFILE=discrete`) | **`phys_iters=8`** (`2` dispatches) · **`7,449` surfels (`4.1x` UMA)** · `mvs_gpu=4` (`2x` sweep density) · `gi_oracle=1` (`412,160` rays) | `17.48 MiB` BDA + dedicated VRAM (`64 MiB` pass pool) | `15,029.51 µs` (`W7`) · `20,632.00 µs` (`W8` wide) | Verifies every dGPU execution upgrade (`8` `AVBD` iterations, `65,536` `2DGS` cap, `2x` finer `ZNCC` MVS step, `4x MSAA`, `3/4`-res GI) on the same binary! |

---

## 4. Build Health, Mathematical Test Gates & Final `v0.2` Footprint

| Verification Gate | Final `Engine v0.2` (`v0.2-M5`) Result |
| :--- | :--- |
| Compiler Warnings (`-std=c23 -Wall -Wextra -Werror=unused-function -Wmissing-declarations`) | **`0` warnings** (`debug`, `release`, and `test`) |
| Linker Dead-Code Gate (`-Wl,--gc-sections --print-gc-sections` across all `src/`, incl. `src/physics` & `src/backend`) | **`0` dead functions** |
| Vulkan Descriptor Pools / Descriptor Sets (`VkDescriptorPool` / `VkDescriptorSet`) | **`0` allocated** (64-bit `VK_KHR_buffer_device_address` + inline push descriptors) |
| Unified `UMA_IGPU` BDA Slab Count (`Worlds 0, 2, 3, 7, 8, 9`) | **`1` single `40.00 MiB` `VkDeviceMemory` slab** (`0` overflow allocations; `0.00 MiB` on `World 0`) |
| Standalone Unit Test Suites (`make test` — **12 binaries**, incl. `test_physics`, `test_lighting`, `test_scene`, `test_backend`) | **`12 / 12 PASS`** |
| `AVBD` $3 \times 3$ $L D L^\top$ Extreme Stiffness Gate ($k = 10^{18}$, `test_physics`) | **Exact finite solution (`< 1e-4` rel error; `0` `fp32` cofactor overflow)** |
| `AVBD` `1000:1` Mass-Ratio Rigid Stack (`1000 kg` on `1 kg` in `8` iterations, `test_physics`) | **Zero collapse (`< 2 mm` residual compression, unconditional stability)** |
| `AVBD` CSR Graph Coloring & Soft-Body Convergence (`8x8` cloth, `306` constraints, `test_physics`) | **`7` conflict-free colors (`0` adjacent conflicts), Rockafellar-Wets $\lambda \le 0$ exact** |
| `2DGS` Duff-Frisvad Basis & $0^\circ$ vs $78^\circ$ Grazing Ray-Plane Invariance (`test_backend` Sec. 13) | **Orthonormal (`< 1e-4`), identical $(u, v, \alpha)$ (`< 1e-3`, zero affine EWA warp)** |
| Stripped + LTO Release ELF (`v0.2-M5` snapshot / current `v0.3.0` `build/release/bin/engine` with **42** SPIR-V 1.6 modules & `build/release/bin/minimal`) | `3,050,216 B` (`2.91 MiB`) @ M5 / **`3,163,432 B` (`3.02 MiB`)** + **`438,192 B` (`0.42 MiB`)** @ `v0.3.0` |
