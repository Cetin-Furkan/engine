# Engine `v0.2` Milestone 4 Architecture & Five-Way Benchmark Report (`v0.1` vs `M1` vs `M2` vs `M3` vs `v0.2-M4`)

**Repository:** `Engine`  
**Date:** November 12, 2026  
**Raw Snapshots:** `benchmarks/v0.1_baseline/` · `benchmarks/v0.2_milestone1/` · `benchmarks/v0.2_milestone2/` · `benchmarks/v0.2_milestone3/` · `benchmarks/v0.2_milestone4/`  
**Companion Specifications:** [`doc/V02_GPU_PROFILES_AND_2026_ALGORITHMS.md`](./V02_GPU_PROFILES_AND_2026_ALGORITHMS.md) · [`doc/BENCHMARK_V01_VS_V02_M1.md`](./BENCHMARK_V01_VS_V02_M1.md) · [`doc/BENCHMARK_V02_M2.md`](./BENCHMARK_V02_M2.md) · [`doc/BENCHMARK_V02_M3.md`](./BENCHMARK_V02_M3.md)

---

## 1. Executive Summary

`Engine v0.2` Milestone 4 (`v0.2-M4`) introduces a first-class, descriptorless **Global Illumination & Visibility Kernel** (`include/lighting/lighting.h`, `src/lighting/`, and `shaders/lighting*.slang`) built directly into the core platform engine rather than demo world code:

1. **2026 `SSILVB` (Screen-Space Indirect Lighting with 32-Bit Visibility Bitmasks, `shaders/lighting_ssilvb.comp.slang`):**
   Replaces horizon-clamped SSAO/GTAO and noisy stochastic ray sampling with 1-cycle integer bitmask integration (`vis_mask`, thickness-aware `hit_bits`, `newly_occluded = hit_bits & ~vis_mask`, `countbits`). Delivers horizon-free thickness-aware **Ambient Visibility (`VBAO`)**, **Bent Normals**, **Jimenez Chromatic Multi-Bounce AO**, **Lagarde Specular Occlusion**, **Contact Micro-Shadowing**, and **Single-Bounce Local Diffuse Radiance**—letting bounce light pass underneath thin foreground occluders while blocking hidden back-surfaces with zero energy leaks.
2. **4-Level Split / Holographic Radiance Cascades (`HRC`, `shaders/lighting_hrc.comp.slang`):**
   Solves world-space sky, sun, ground, emissive-solid, and canyon indirect bounce deterministically (zero Monte Carlo noise, zero neural denoiser) across 4 constant-space-angle cascades ($c \in \{3, 2, 1, 0\}$, probes $(W_0 / 2^c) \times (H_0 / 2^c)$, rays $M_c = 4 \cdot 2^c \in \{4, 8, 16, 32\}$) over an exact **Lambert Cylindrical Equal-Area $S^2$ KD-partition**, merged coarse-to-fine via exact interval transmittance and fed with multi-bounce temporal irradiance from `gi_prev_bda`.
3. **Dual Hardware Oracles & Cooperative SLM Culling (`UMA_IGPU` vs `DISCRETE_DIRECT`):**
   - **`VULKAN_PROFILE_UMA_IGPU` (2020 Intel Iris Xe)**: Runs half-resolution (`640x344`) `SSILVB` (`2` slices $\times$ `6` steps $\times$ $\pm 1$ directions) + `160x86` C0 `HRC` (`102,400` analytical + `.epack` DEM rays, `10.90 MiB` GI slab), keeping **every benchmark world (`World 0`, `2`, `7`, `8`, `9`) inside 1 single `40.00 MiB` unified UMA `VkDeviceMemory` slab** (`0` overflow allocations, `0` `VkDescriptorSet`s, `0.00 MiB` lazy cost on `World 0`).
   - **`VULKAN_PROFILE_DISCRETE_DIRECT` / `HYBRID_MUXLESS`**: Scales automatically to `3/4`-resolution (`960x516`) `SSILVB` (`4` slices $\times$ `8` steps) + `320x172` C0 `HRC` (`412,160` rays, `25.86 MiB` GI slab) and probes/enables `VK_KHR_acceleration_structure` + `VK_KHR_ray_query` in `src/vulkan/device/device.c`.
   - **Cooperative `8x8` Tile-Frustum SLM Proxy Culling & Zero-SFU Math**: Precomputes `fp16` `(sin, cos)` pose pairs and tight circumspheres in `LightingProxy`, culls proxies once per `8x8` workgroup into `groupshared` SLM (`88x` fewer per-pixel proxy tests), and replaces per-sample GPU `atan2` with a 4-FMA minimax polynomial (`< 0.0009°` max error, `0` SFU stalls).
4. **Single-Pass MSAA Preservation & `1x R32_UINT` Visibility Buffer Synergy:**
   Executes the GI compute prepass in `lighting_prepare()` before dynamic rendering so hardware MSAA transient attachments keep `VK_ATTACHMENT_STORE_OP_DONT_CARE`. On **World 7 (`Procedural Kinetic Cathedral`, 18 bodies)**, pairing 2026 GI with Milestone 3's **`1x R32_UINT` Visibility Buffer** (`ENGINE_MSAA=1`) runs in **`6,200.82 µs` (`161 FPS`)** inside an **`8.00 MiB` attachment pool**—beating `2x MSAA + GI` (`6,597.96 µs`, `32.00 MiB` pool) by **`-397.14 µs`** because `lighting_sample_gi()` executes strictly once per visible pixel with `0%` $2\times 2$ quad helper-lane waste.

---

## 2. Mathematical & Data-Oriented Architecture

### 2.1 Descriptorless BDA Memory Layout (`include/lighting/lighting.h`)

All material emission slots, frame headers, 3D analytical proxies, G-buffer texels, Radiance Cascade probes, and ping-pong temporal GI grids live in 64-bit BDA slabs (`0` `VkDescriptorPool` / `0` `VkDescriptorSet` allocations). `World 0` and pure-2D worlds (`Worlds 3, 4`) never trigger `ensure_gi_slab()`, incurring **`0.00 MiB`** GI slab allocation:

| Structure | Size / Align | Bit-Packed Fields | Bandwidth / ALU Invariant |
| :--- | ---: | :--- | :--- |
| `LightingMatSlot` | `32 B` | `emit_{r,g,b}`, `glow`, `reserved[4]` | Unified engine-level material table (`64` slots $\times$ `4` timeline slices in `s_env_buf`), retiring demo-only `examples/demo/render/material.c` GPU buffers. |
| `LightingProxy` | `64 B` (`alignas(64)`) | `center[3]`, `kind`, `extents[3]`, `roughness`, `pitch_rad`, `yaw_rad`, `alb_f16[3]`, `flags`, `emi_f16[3]`, `bound_radius_f16`, `trig_pitch`, `trig_yaw` | Exact 64-byte CPU/GPU cache line. Packs `fp16` $(\sin\theta, \cos\theta)$ for pitch/yaw and tight circumsphere radius $R$ (`sqrt(ex^2+ey^2+ez^2)` for OBB, `max(ex,ey,ez)` for Ellipsoid) -> **`0` GPU `sin`/`cos` SFU ops**. |
| `LightingGbufTexel` | `16 B` (`alignas(16)`) | `float view_z`, `uint32_t oct_nrm_s16`, `uint32_t direct_rgb9e5`, `uint32_t alb_flags` | Single 128-bit vector load/store (`1.41 MiB` at `640x344`). Encodes HDR direct + thermal radiance in Khronos shared-exponent `RGB9E5` across 6 orders of magnitude. |
| `LightingGiTexel` | `16 B` (`alignas(16)`) | `float view_z`, `uint32_t packed_nrm_bent` (`2x oct_snorm8`), `uint32_t irr_rgb9e5`, `uint16_t ao_unorm16`, `uint16_t conf_unorm16` | Single 128-bit load per bilateral tap in surface shaders (`cube`, `scene` meshlets, `VisBuf` resolve, `plane`, and World 8 `terrain`). |
| `LightingFrameHeader` | `256 B` | Resolution, cascade ray offsets, `proxies_bda`, `gbuf_bda`, `hrc_bda`, `gi_curr_bda`, `gi_prev_bda`, `terrain_dir_bda`, `scene_hiz_bda`, current/prev camera quaternions | Broadcast once into `groupshared` SLM per compute workgroup; surface shaders read only required scalar fields via pointer dereference (`hdr->...`). |

---

### 2.2 2026 `SSILVB` 32-Bit Sector Bitmask Formulation (`lighting_math.c` & `lighting_ssilvb.comp.slang`)

For each view-space pixel $(P, N)$ and slice tangent $T(\phi) = (\cos\phi, \sin\phi, 0)$, the projected normal $N_{\text{proj}}$ defines reference angle $\theta_n$ in the slice plane. Each stepped G-buffer neighbor $P_s = P + \Delta$ with finite thickness $\tau$ subtends front vector $\Delta_f = \Delta$ and back vector $\Delta_b = (P_s - (0, 0, \tau)) - P$, mapping elevation interval $[\alpha_{\min}, \alpha_{\max}] \subset [0, 1]$ onto 32 uniform $5.625^\circ$ hemisphere sectors:

$$b_0 = \lfloor 32 \cdot \alpha_{\min} \rfloor, \qquad b_1 = \lceil 32 \cdot \alpha_{\max} \rceil, \qquad M_{\text{hit}} = \left(\left(1 \ll (b_1 - b_0)\right) - 1\right) \ll b_0$$

Newly occluded angular sectors are isolated in 1 ALU cycle before updating the running slice bitmask $M_{\text{vis}}$:

$$M_{\text{new}} = M_{\text{hit}} \mathbin{\&} \sim M_{\text{vis}}, \qquad M_{\text{vis}} \leftarrow M_{\text{vis}} \mathbin{|} M_{\text{hit}}, \qquad \Delta\Omega_s = \frac{\operatorname{countbits}(M_{\text{new}})}{32}$$

- **Ambient Visibility (`VBAO`)**: $V = 1 - \frac{1}{S}\sum_{s=1}^{S} \frac{\operatorname{countbits}(M_{\text{vis}, s})}{32}$, lifted via Jimenez chromatic multi-bounce AO:
  $$\operatorname{AO}_{\text{MB}}(V, \rho) = V \cdot \Big(V \cdot \big(V \cdot (2.0404\rho - 0.3324) + (-4.7951\rho + 0.6417)\big) + (2.7552\rho + 0.6903)\Big)$$
- **Single-Bounce Local Indirect Radiance**: Accumulates $\Delta\Omega_s \cdot L_s \cdot \max(0, -N_s \cdot \hat{\Delta}_f) \cdot \max(0, N \cdot \hat{\Delta}_f)$—so hidden walls behind thick occluders ($M_{\text{new}} = 0$) leak **zero** radiance while distant lit walls remain visible underneath thin floating geometry (verified directly in `tests/test_lighting.c`).
- **Spatial-Temporal Resolve**: Filtered in `8x8` `groupshared` SLM via depth + normal cross-bilateral weights, then reprojected through `prev_quat` / `prev_cam` into a variance-clamped (`1.35` $\sigma$ neighborhood box) `16`-frame exponential history buffer.

---

### 2.3 4-Level Split Radiance Cascades (`HRC`) & Lambert Equal-Area $S^2$ Partition (`lighting_hrc.comp.slang`)

Across cascades $c \in \{3, 2, 1, 0\}$, probe count halves per spatial axis $(W_c, H_c) = (W_0 / 2^c, H_0 / 2^c)$ while ray count doubles $M_c = 4 \cdot 2^c \in \{4, 8, 16, 32\}$, holding storage strictly constant per cascade (`4 * W0 * H0` `uint32_t` `RGB9E5 + 1b transmittance` entries):

1. **Exact Binary $S^2$ Partition**: Using Lambert's cylindrical equal-area map $z = 1 - 2 u_1 \in [-1, 1]$, $\phi = 2\pi u_2$ ($d\Omega = dz\,d\phi = 4\pi\,du_1\,du_2$), ray index $m \in [0, M_c)$ splits $(u_1, u_2)$ via alternating binary KD-halving so child rays $(2m, 2m+1)$ in cascade $c+1$ partition parent ray $m$'s **exact** solid angle with zero stochastic jitter.
2. **Coarse-to-Fine Transmittance Merge** across world-space intervals $[12, 58, 220, 760, 2400] \cdot \text{px\_world}$:
   $$L_c(x, m) = L_{\text{near}}(x, m) + \beta_{\text{near}}(x, m) \cdot \frac{1}{2}\Big(\mathcal{I}\big(L_{c+1}(\cdot, 2m)\big)(x) + \mathcal{I}\big(L_{c+1}(\cdot, 2m + 1)\big)(x)\Big)$$
   where $\beta_{\text{near}} \in \{0, 1\}$ is exact interval transmittance (`0` on proxy/ground/DEM hit, blocking all far-cascade light leaks) and $\mathcal{I}$ is bilinear spatial probe interpolation (augmented at $c=0$ with 4-cardinal silhouette anchor snapping into G-buffer surface points).

---

## 3. Five-Way Empirical Results (`v0.1` vs `v0.2-M1` vs `v0.2-M2` vs `v0.2-M3` vs `v0.2-M4`)

All five milestone snapshots were captured on the same physical **Intel Core i7-1165G7 + Intel Iris Xe (`TGL GT2`, 7.4 GiB shared UMA RAM, Linux `7.3.0-rc6`, Mesa `26.2.6`, Vulkan `1.4.354`)** at `1280x688` (`75` live frames, `65` retired hardware `VkQueryPool` samples per world). In `v0.2-M4`, `ENGINE_GI=auto` enables full 2026 `SSILVB` + 4-level Split `HRC` GI by default on all 3D/terrain worlds (`Worlds 2, 7, 8, 9`).

### 3.1 Five-Milestone Progression Across All Benchmark Worlds (`UMA_IGPU`)

| World Workload | Metric | `v0.1` Baseline<br>*(No GI)* | `v0.2-M1`<br>*(No GI)* | `v0.2-M2`<br>*(No GI)* | `v0.2-M3`<br>*(No GI)* | **`v0.2-M4` (`GI=0` / `GI=1`)** | Key Architectural Takeaway |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | :--- |
| **World 0** — *Pure Sealed Empty Engine* | `gpu_total_us` (avg)<br>`gi_mib` / `bda_dev_mib` | 520.99 µs<br>`0.00 MiB` | 472.71 µs<br>`0.00 MiB` | 477.43 µs<br>`0.00 MiB` | 351.58 µs<br>`0.00 MiB` | **`497.15 µs`** (`min 439.95`)<br>**`0.00` / `0.00/40.00 MiB`** | **Zero lazy GI overhead** (`gi=0`, `0` bytes allocated). |
| **World 2** — *3D Solids Gravity & Squash*<br>(`9` GI proxies, `102,400` HRC rays) | `gpu_total_us` (avg)<br>`gpu_render_us` (avg)<br>`bda_dev_mib` (slab) | 4,724.91 µs<br>1,829.09 µs<br>31.14/64 MiB | 4,065.49 µs<br>1,178.96 µs<br>30.92/32 MiB | 4,095.88 µs<br>1,187.61 µs<br>30.92/32 MiB | 2,033.22 µs<br>690.72 µs<br>25.45/32 MiB | `2,090.70 µs` / **`5,458.00 µs`**<br>`719.08 µs` / **`1,688.41 µs`**<br>**`36.35 / 40.00 MiB` (`1` slab)** | **`183 FPS` (`min 4.94 ms`)** with full 4-cascade `HRC` + 32-bit `SSILVB` + ground contact shadows! |
| **World 7** — *Procedural Kinetic Cathedral*<br>(`18` GI proxies, `102,400` HRC rays) | `gpu_total_us` (avg)<br>`gpu_render_us` (avg)<br>`bda_dev_mib` (slab) | 5,610.67 µs<br>2,934.42 µs<br>31.89/64 MiB | 4,200.88 µs<br>1,801.81 µs<br>31.67/32 MiB | 5,545.08 µs<br>2,387.18 µs<br>31.67/32 MiB | 2,757.29 µs<br>1,269.99 µs<br>26.20/32 MiB | `3,917.26 µs` / **`6,597.96 µs`**<br>`1,732.20 µs` / **`2,584.68 µs`**<br>**`37.10 / 40.00 MiB` (`1` slab)** | **`152 FPS` (`min 6.14 ms`)** (`161 FPS` / `6.20 ms` in `1x VisBuf` mode); net 18-body GI cost is **`+2.68 ms`**. |
| **World 8** — *Baklan & Beşparmak Dağı DEM*<br>(`96` `.epack` tiles + Canyon `SSILVB`+`HRC`) | `gpu_total_us` (avg)<br>`uring_us` (stall)<br>`bda_dev_mib` (slab) | 7,264.61 µs<br>5,321 µs<br>19.72/64 MiB | 7,058.31 µs<br>5,276 µs<br>23.97/32 MiB | 7,027.84 µs<br>0 µs<br>17.15/32 MiB | 6,625.10 µs<br>0 µs<br>17.15/32 MiB | `5,013.16 µs` / **`9,022.97 µs`**<br>**`0 µs`**<br>**`28.05 / 40.00 MiB` (`1` slab)** | **`111–135 FPS` (`min 7.43 ms`)** ray-marching 24 km `.epack` mountains for canyon AO + cliff-to-valley GI bounce! |
| **World 9** — *Interactive Full Sandbox*<br>(`14/26` meshlets, `8` GI proxies) | `gpu_total_us` (avg)<br>`gpu_render_us` (avg)<br>`bda_dev_mib` (slab) | 2,069.99 µs<br>573.77 µs<br>32.82/64 MiB | 2,225.29 µs<br>584.08 µs<br>32.60/32 MiB | 1,870.59 µs<br>490.68 µs<br>32.60/32 MiB | 1,674.56 µs<br>491.22 µs<br>27.13/32 MiB | **`5,151.95 µs`** (`min 4,705.31`)<br>**`1,465.85 µs`**<br>**`38.03 / 40.00 MiB` (`1` slab)** | **`194 FPS`** with live frustum-culled meshlets, `SSILVB`+`HRC` GI, 3D CSV plots & SDF text in 1 UMA slab. |

---

### 3.2 Direct A/B Isolation (`ENGINE_GI=0` vs `ENGINE_GI=1`) & Cross-Profile Matrix

All configurations below were recorded back-to-back on the exact same release binary (`benchmarks/v0.2_milestone4/*.log`):

| Configuration & Command | GI Resolution & Ray Budget | `pass_pool_mib` | `gi_mib` / `bda_dev_mib` | `gpu_total_us` (avg / min) | `gpu_render_us` | Key Verified Behavior |
| :--- | :--- | ---: | ---: | ---: | ---: | :--- |
| **`World 7` — `UMA_IGPU` + `1x R32_UINT VisBuf` + `GI`**<br>`ENGINE_MSAA=1 ./build/release/bin/engine --world=7 --bench=75` | `640x344` (`2x6` `SSILVB`)<br>`102,400` `HRC` rays | **`8.00 MiB`** | `10.90` / `37.10 MiB` | **`6,200.82 µs`** (`5,858.96 µs`) | **`2,221.84 µs`** | **Fastest full-GI World 7 mode (`-397 µs` vs `2x MSAA`)** + **`-93.8%` attachment RAM** vs `v0.1`: shades GI once per visible pixel (`0%` quad helper waste). |
| **`World 7` — `UMA_IGPU` Default (`2x MSAA` + `GI`)**<br>`./build/release/bin/engine --world=7 --bench=75` | `640x344` (`2x6` `SSILVB`)<br>`102,400` `HRC` rays | `32.00 MiB` | `10.90` / `37.10 MiB` | **`6,597.96 µs`** (`6,136.87 µs`) | `2,584.68 µs` | Single-pass `2x MSAA` (`STORE_OP_DONT_CARE` preserved) + depth/normal joint $2\times 2$ bilateral GI upsample across 18 bodies + ground plane. |
| **`World 7` — `1x VisBuf` + `Two-Phase Subgroup Hi-Z` + `GI`**<br>`ENGINE_MSAA=1 ENGINE_HIZ=1 .../engine --world=7 --bench=75` | `640x344` (`2x6` `SSILVB`)<br>`102,400` `HRC` rays | **`8.00 MiB`** | `10.90` / `38.22 MiB` | **`6,982.06 µs`** (`6,515.99 µs`) | `2,226.57 µs` | All four v0.2 GPU pipelines (`Meshlets` + `Two-Phase Subgroup Hi-Z` + `R32_UINT VisBuf` + `SSILVB/HRC GI`) active together inside **`1` `40 MiB` UMA slab** and **`8 MiB` pass pool**. |
| **`World 7` — `UMA_IGPU` (`ENGINE_GI=0` Baseline)**<br>`ENGINE_GI=0 .../engine --world=7 --bench=75` | `gi=0` (`0` rays) | `32.00 MiB` | `0.04` / `26.24 MiB` | `3,917.26 µs` (`3,585.00 µs`) | `1,732.20 µs` | Allocates only the `41 KiB` 4-slice `s_env_buf` material ring; confirms net 18-body GI delta is **`+2.68 ms`**. |
| **`World 7` — `DISCRETE_DIRECT` (`4x MSAA` + High-Density GI)**<br>`ENGINE_GPU_PROFILE=discrete .../engine --world=7 --bench=45` | `960x516` (`4x8` `SSILVB`)<br>**`412,160` `HRC` rays** | `64.00 MiB` | **`25.86`** / `17.48 MiB` *(+ dedicated)* | `15,127.19 µs` (`14,479.43 µs`) | `3,014.74 µs` | Verifies `DISCRETE_DIRECT` `3/4`-res `SSILVB` + `4x` `HRC` probe/ray budget + dedicated `>16 MiB` BDA GI slab (`66+ FPS` even emulated on 2020 iGPU!). |
| **`World 8` — `UMA_IGPU` (`ENGINE_GI=0` vs `ENGINE_GI=1`)**<br>`[ENGINE_GI=0] .../engine --world=8 --bench=75` | `640x344` (`2x6` `SSILVB`)<br>`102,400` DEM rays | `32.00 MiB` | `0.04 -> 10.90 MiB` | `5,013.16 -> 9,022.97 µs` | `4,922 -> 4,995 µs` | Scalar `.epack` heightfield ray-bracketing adds **`+4.01 ms`** compute GI while adding only **`+73 µs`** to terrain fragment shading (`gpu_render_us`). |

---

## 4. Build Health, Mathematical Test Gates & Binary Footprint

| Verification Gate | Result in `v0.2-M4` |
| :--- | :--- |
| Compiler Warnings (`-std=c23 -Wall -Wextra -Werror=unused-function -Wmissing-declarations`) | **`0` warnings** (`debug`, `release`, and `test`) |
| Linker Dead-Code Gate (`-Wl,--gc-sections --print-gc-sections` across `src/`, incl. `src/lighting`) | **`0` dead functions** |
| Vulkan Descriptor Pools / Descriptor Sets (`VkDescriptorPool` / `VkDescriptorSet`) | **`0` allocated** (64-bit `VK_KHR_buffer_device_address` + inline push descriptors) |
| Hardware Ray-Query Probing (`VK_KHR_acceleration_structure` + `VK_KHR_ray_query` in `device.c`) | **Probed & enabled automatically when supported** (`RayQuery=no` cleanly detected on Gen12 `TGL GT2`) |
| Unit Test Suites (`make test` — **11 binaries**, incl. `build/test_lighting`) | **`11 / 11 PASS`** |
| Khronos `RGB9E5` Shared-Exponent HDR Codec (`[0.001, 420.0]` dynamic range, `test_lighting`) | **`<= 1.0%` max relative error**, negative/zero clamp exact |
| Octahedral `snorm16` & `snorm8` Normal/Bent-Normal Codecs (`all 8 octants`, `test_lighting`) | **`dot > 0.9998` (`snorm16`)** · **`dot > 0.994` (`snorm8`)** |
| `SSILVB` 32-Bit Bitmask Thin-Occluder Pass-Through & Shadow Energy Conservation (`test_lighting`) | **Verified (`indirect_rgb > 0.15` under floating pole; `0.0` leaked radiance behind thick wall)** |
| Split `HRC` Interval Merge (`beta=0` leak block & `beta=1` child solid-angle conservation, `test_lighting`) | **Exact (`< 1e-6` error)** |
| Stripped + LTO Release ELF (`build/release/bin/engine`, all 32 SPIR-V 1.6 shaders & font `#embed`ded) | **`2,883,448 B` (`2.75 MiB`)** |
