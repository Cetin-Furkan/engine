# GPU-Math Terrain & Map Streaming Backend (`src/backend/`) — Complete Architecture & Continuation Blueprint

> [!IMPORTANT]
> **Purpose of This Document**
> This document is the authoritative architectural specification and self-contained implementation blueprint for the **GPU-Math & Dynamic Map/Terrain Streaming Backend** (`include/backend/`, `src/backend/`, `shaders/terrain_*.slang`, and `examples/demo/world/kind/terrain.c`). It is written so that any engineer or AI model can immediately understand the exact geographic data, binary on-disk layout, next-generation pointer math (`RelPtr2D` + Lilliput 64-bit Block Headers + 64-Byte Stateful Cells + 256-Byte PULSE Active GPU Windows), `io_uring` SSD streaming pipeline, and Slang 2026 GPU shaders without needing prior conversation history.

---

## 1. Geographic Target & Real-World Dataset Specification

### 1.1 Resolved Target Region (`https://maps.app.goo.gl/B18gGPZMEtULHfkw9` + Beşparmak Dağı)

The short link `https://maps.app.goo.gl/B18gGPZMEtULHfkw9` resolves to:
```
https://www.google.com/maps/place/Baklan,+Denizli/@37.9804566,29.6118873,13.5z/data=!4m6!3m5!1s0x14c6580137cb375f:0x2d6e3172998e44fc!8m2!3d37.976748!4d29.606909
```

Immediately south-southeast of **Baklan, Denizli** lies **Beşparmak Dağı** (the five-crested dolomite/limestone massif rising sharply out of the flat Baklan Plain / Büyük Menderes graben):

| Feature | Latitude ($\phi$, WGS84) | Longitude ($\lambda$, WGS84) | Measured Elevation ($h$) | Topographic Character |
| :--- | :--- | :--- | :--- | :--- |
| **Baklan Town Center** | $37.976748^\circ\text{ N}$ | $29.606909^\circ\text{ E}$ | $\approx 855\text{--}875\text{ m}$ | Alluvial fan at the northern foot of Beşparmak Dağı |
| **Baklan Ovası (Plain Floor)** | $37.995000^\circ\text{ N}$ | $29.595000^\circ\text{ E}$ | $\approx 828\text{--}842\text{ m}$ | Flat agricultural parcel grid northwest of the massif |
| **Beşparmak Dağı (West Peak / OSM Node `11061240540`)** | $37.952201^\circ\text{ N}$ | $29.617113^\circ\text{ E}$ | $\approx 1,307\text{ m}$ | Western prominent rocky crest overlooking Baklan |
| **Beşparmak Dağı (Main Summit Ridge)** | $37.960556^\circ\text{ N}$ | $29.636667^\circ\text{ E}$ | $\approx 1,690.8\text{ m}$ | Highest five-fingered spine of the Beşparmak massif |
| **Full Quadtree Bounding Box** | $37.822802^\circ\text{ N} \dots 38.099983^\circ\text{ N}$ | $29.443359^\circ\text{ E} \dots 29.794922^\circ\text{ E}$ | $700.0\text{ m} \dots 1,690.8\text{ m}$ | $31.0\text{ km} \times 31.0\text{ km}$ ($\approx 960\text{ km}^2$) Web Mercator quadtree pyramid (`world_x, world_y` $\in [-2400, +2400]$) |

### 1.2 Web Mercator Quadtree Tile Pyramid (`LOD 0` $\rightarrow$ `LOD 3`, `128x128` per Tile)

The expanded $31\text{ km} \times 31\text{ km}$ region is mapped onto a **4-Level Restricted Quadtree Pyramid** ($1 + 4 + 16 + 64 = 85$ tiles, `128x128` height samples + `128x128` RGBA8 texels = `98,304 bytes` = `24 × 4 KB` blocks per tile, with $64$ `z=13` outer tiles + $64$ `z=14` high-resolution core tiles over Baklan & Beşparmak Dağı):

| Quadtree Level | Web Mercator Source ($z$) | Tile Grid ($N \times N$) | Tile Count | World Tile Span | Ground Sample Spacing ($\Delta s$) | Per-Tile Mesh & Payload (`98,304 B`) |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **`LOD 0` (Root Overview)** | $z = 11..13$ | $1 \times 1$ | $1$ | $4800 \times 4800$ | $\approx 242\text{ m/sample}$ | $128 \times 128$ DEM + RGBA8 ($96,774$ verts) |
| **`LOD 1` (Regional Basin)** | $z = 12..13$ | $2 \times 2$ | $4$ | $2400 \times 2400$ | $\approx 121\text{ m/sample}$ | $4 \times (128 \times 128)$ DEM + RGBA8 |
| **`LOD 2` (Ridges & Valleys)** | $z = 13..14$ | $4 \times 4$ | $16$ | $1200 \times 1200$ | $\approx 60\text{ m/sample}$ | $16 \times (128 \times 128)$ DEM + RGBA8 |
| **`LOD 3` (Fine Crags & Fields)** | $z = 13 + z = 14\text{ Core}$ | $8 \times 8$ | $64$ | $600 \times 600$ | $\approx 15\text{--}30\text{ m/sample}$ | $64 \times (128 \times 128)$ DEM + RGBA8 |

### 1.3 Real Data Sources & Offline/Online Self-Healing Pipeline

1. **Measured Digital Elevation Model (DEM)**:
   - Fetched from **AWS Mapzen Terrarium SRTM/GMTED** (`https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png`) across both an $8\times 8$ `z=13` outer grid ($64$ tiles) and an $8\times 8$ `z=14` high-resolution inner core ($64$ tiles) centered directly on Baklan and Beşparmak Dağı.
   - Decoded via the exact Terrarium metric formula:
     $$h(\text{meters}) = \left(R \cdot 256.0 + G + \frac{B}{256.0}\right) - 32768.0$$
2. **True-Color Satellite Ortho-Imagery (`srgb_to_linear` + 16-Tap Bicubic)**:
   - Fetched directly from **Esri World Imagery** (`https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}`) with zero cartographic overlay tint, gently dehazed to remove high-altitude atmospheric veil, and decoded in Slang (`shaders/terrain_math.slang`) with **16-tap Catmull-Rom bicubic interpolation** and exact **IEC 61966-2-1 `srgb_to_linear()`** conversion so the `VK_FORMAT_B8G8R8A8_SRGB` framebuffer preserves rich, authentic Aegean ground, forest, field, and dolomite rock colors.
3. **Self-Contained C23 Synthesis & Persistence Guarantee**:
   - The binary archive is stored on SSD at `examples/demo/maps/baklan_besparmak.gmpack`.
   - `scripts/fetch_besparmak_map.py` downloads the real AWS Terrarium SRTM elevation tiles + Esri/OpenTopoMap imagery for Baklan and Beşparmak Dağı and compiles `examples/demo/maps/baklan_besparmak.gmpack`.
   - Furthermore, if the engine boots on an air-gapped machine where `baklan_besparmak.gmpack` is absent, `src/backend/terrain/storage/synth_besparmak.c` deterministically synthesizes the complete 85-tile Baklan + Beşparmak Dağı multi-peak ridge model (calibrated to the exact WGS84 coordinates and 830 m–1,612 m elevation profile), **writes the `.gmpack` archive to SSD**, and **re-reads its tiles dynamically on demand via `io_uring`** as the user zooms and pans.

---

## 2. Next-Generation Pointer & Memory Architecture (2025–2026)

Classic 64-bit host pointers (`void *`) cannot be serialized to disk, break when moved between CPU `mmap` and GPU `VkDeviceAddress` (`BDA`) arenas, and require multi-hop pointer chasing across L1 cache lines. Building on the architectures in [`pointers.md`](file:///home/arch-cetin/projects/engine-v2/pointers.md), the `src/backend/` subsystem introduces four co-designed pointer primitives that execute identically on the CPU and inside Slang 2026 SPIR-V 1.6 shaders:

```mermaid
flowchart LR
    subgraph SSD["SSD Storage (.gmpack Archive)"]
        PACK["256B PULSE Root Window\n+ 85 RelPtr2D Quadtree Nodes\n+ 4KB-Aligned Tile Blocks"]
    end
    subgraph CPU["CPU L1 Cache (src/backend/)"]
        CELL["64-Byte Stateful Function Cell\nalignas(64) GpuMathStreamCell\n(8B fn_ptr + 56B Inline State)"]
        URING["io_uring Async Pager\n(PREAD into 4KB Slot)"]
    end
    subgraph GPU["Vulkan 1.4 BDA Arena (VkDeviceAddress)"]
        DIR["256B Active GPU Window\n(GpuMathPageDirectory)"]
        NODES["RelPtr2D Quadtree Array\n+ Lilliput 64-Bit Tile Headers"]
        TILES["4KB-Aligned Resident Tile Pool\n(64x64 fp16/u16 DEM + RGBA8)"]
    end

    PACK -->|"io_uring PREAD (4KB Block)"| URING
    CELL -->|"Single L1 Line Dispatch"| URING
    URING -->|"Zero-Fixup Copy (RelPtr2D Intact)"| TILES
    DIR -->|"Base + (BlockOff << 12)"| NODES
    NODES -->|"RelPtr2D + (dX, dY) GPU Math"| TILES
```

### 2.1 `RelPtr2D` — 64-Bit 2D Self-Relative Spatial Block Pointer (`include/backend/math/relptr2d.h`)

Every link between the directory header, quadtree nodes, parent/child tiles, and 4 KB data blocks is encoded as a **64-bit 2D Self-Relative Spatial Block Pointer (`RelPtr2D`)**:

```
 63       56 55    52 51              32 31               16 15                0
+-----------+--------+------------------+-------------------+-------------------+
| Epoch/Tag |LOD (4b)|  20-Bit Signed   |  16-Bit Signed dX |  16-Bit Signed dY |
|  (8-Bit)  | 0..15  |  4KB Block Delta |  (Grid X Offset)  |  (Grid Y Offset)  |
+-----------+--------+------------------+-------------------+-------------------+
```

1. **Position-Independent Address Arithmetic (Zero Pointer Fixup)**:
   - Let $A_{\text{self}}$ be the byte address (either a file offset on SSD, a `uint8_t *` in CPU memory, or a `uint64_t` `VkDeviceAddress` in GPU BDA memory) of the structure holding `RelPtr2D`.
   - The target byte address $A_{\text{target}}$ is resolved in **one shift-add instruction** on both CPU and GPU:
     $$A_{\text{target}} = A_{\text{self}} + \left(\text{sign\_extend}_{20}(\text{block\_delta}) \ll 12\right)$$
   - Because $2^{19} \times 4,096\text{ B} = \pm 2\text{ GiB}$, a 20-bit signed block delta addresses any 4 KB block across a 4 GiB terrain arena while leaving 44 bits for inline 2D spatial mathematics and temporal epoch verification!
2. **Inline 2D Coordinate Transform (`dX`, `dY`, `LOD`)**:
   - Bits `[31:16]` (`dX`) and `[15:0]` (`dY`) store the signed 16-bit local grid coordinates (in Q8.8 fixed-point or integer tile units) of the target tile relative to the quadtree root.
   - A GPU vertex or compute shader scanning a `RelPtr2D` computes both the memory address of the tile *and* its 2D world bounding box in a single register read without touching a second cache line.
3. **CHERI-D / LIPPEN-Inspired Epoch & Canonical Integrity Tag (`bits [63:56]`)**:
   - Bits `[63:56]` store an 8-bit `epoch ^ 0xA5` seal tag. If a tile slot in the GPU cache is evicted and reincarnated for a newer zoom region, its epoch increments; any stale `RelPtr2D` immediately fails tag verification in $O(1)$ time.

### 2.2 Project Lilliput-Style 64-Bit Unified Tile Header (`GpuMathTileHeader`)

Inspired by **JDK 25 Project Lilliput (JEP 519)** from [`pointers.md`](file:///home/arch-cetin/projects/engine-v2/pointers.md), every resident tile in the BDA pool is aligned to a **$4,096\text{-byte}$ ($2^{12}$) boundary** and begins with a single **64-bit Unified Tile Header**:

```
 63              44 43          28 27    24 23    20 19          10 9            0
+------------------+--------------+--------+--------+--------------+--------------+
| 20b 4KB BlockIdx | 16b Morton Z | 4b LOD |4b Flags| 10b MaxElev  | 10b MinElev  |
| (Arena Offset)   | (Interleaved)| (0..3) |Resident| (0..2047 m)  | (0..2047 m)  |
+------------------+--------------+--------+--------+--------------+--------------+
```

- **16-Bit Morton ($Z$-Order) Spatial Key**: Interleaves the 8-bit tile $X$ and 8-bit tile $Y$ coordinates ($\text{morton\_encode2d}(x, y)$) so spatially adjacent 2D tiles on Beşparmak Dağı lie adjacent in 1D SSD file order and GPU L2 cache lines.
- **Inline Elevation Bounds (`MinElev`, `MaxElev`)**: Quantizes the tile's minimum and maximum elevation in meters ($[0, 2047]\text{ m}$, covering Baklan's $828\text{ m}$ floor through Beşparmak Dağı's $1,612\text{ m}$ peak) so GPU frustum/ray culling and hypsometric shading know the exact vertical bounds before reading a single elevation sample.

### 2.3 64-Byte Stateful Function Cell (`GpuMathStreamCell`, `alignas(64)`)

Inspired by **Clef / TVM FFI 64-Byte Stateful Function Cells** in [`pointers.md`](file:///home/arch-cetin/projects/engine-v2/pointers.md), every asynchronous SSD $\rightarrow$ GPU streaming operation in `src/backend/core/cell/dispatch.c` is represented as a single L1-cache-line-aligned **Flat Closure Cell**:

```c
typedef struct GpuMathStreamCell GpuMathStreamCell;
typedef int (*GpuMathCellFn)(GpuMathStreamCell *cell, void *ctx);

struct alignas(64) GpuMathStreamCell {
    GpuMathCellFn     fn;              // Bytes 0..7:   Target execution / completion code pointer
    RelPtr2D          rel_tile;        // Bytes 8..15:  2D Self-Relative pointer to SSD tile block
    uint64_t          gpu_bda_dst;     // Bytes 16..23: Target Vulkan BDA address in resident pool
    uint64_t          ssd_file_offset; // Bytes 24..31: Exact byte offset in .gmpack archive
    uint32_t          tile_id;         // Bytes 32..35: Quadtree node index (0..84)
    uint32_t          morton_key;      // Bytes 36..39: 2D Morton Z-order key + LOD
    uint32_t          bytes;           // Bytes 40..43: Payload size (24,576 bytes = 6 x 4KB blocks)
    uint32_t          slot_index;      // Bytes 44..47: Resident GPU cache slot index
    uint32_t          generation;      // Bytes 48..51: Temporal safety generation counter
    uint8_t           lod;             // Byte  52:     Quadtree LOD (0..3)
    uint8_t           state;           // Byte  53:     CELL_IDLE, CELL_ARMED, CELL_READY, CELL_POISON
    uint8_t           epoch;           // Byte  54:     CHERI-D inline 1-byte lifetime ID
    uint8_t           inline_id;       // Byte  55:     Cache-line tail verification byte
    uint64_t          user_tag;        // Bytes 56..63: io_uring / timeline correlation stamp
};
static_assert(sizeof(GpuMathStreamCell) == 64u, "Stateful Function Cell must be exactly 1 L1 cache line");
```

### 2.4 256-Byte PULSE Active GPU Window (`GpuMathPageDirectory`, `alignas(256)`)

At the base of the GPU BDA terrain buffer sits a **256-byte aligned Active Descriptor Window** (`GpuMathPageDirectory`), followed immediately by the 85 quadtree node descriptors (`GpuMathQuadNode`, 32 bytes each) and the resident 4 KB-aligned tile pool:

- **Each Tile Payload (`GpuMathTileBlock`, $98,304\text{ bytes} = 24 \times 4,096\text{ B}$)**:
  - `uint16_t height_u16[128 * 128]` ($32,768\text{ bytes} = 8\text{ blocks}$): Measured elevation in $1/16\text{th}$ meter fixed-point ($0.0625\text{ m}$ vertical resolution across $[0, 4095.9375]\text{ m}$), plus GPU-computed normal/slope derivatives.
  - `uint32_t imagery_rgba8[128 * 128]` ($65,536\text{ bytes} = 16\text{ blocks}$): Real true-color Esri satellite photography for every grid square in the tile (sampled via 16-tap Catmull-Rom bicubic interpolation and converted via IEC 61966-2-1 `srgb_to_linear` in `shaders/terrain_math.slang`).

---

## 3. Multi-Layer Subsystem & File Hierarchy (3+ Levels Deep)

```
include/backend/
├── backend.h                              # Public GPU-Math & Terrain Streaming API
├── math/
│   ├── relptr2d.h                         # RelPtr2D, Lilliput 64b TileHeader, Morton Z-curve
│   └── cell64.h                           # 64-Byte Stateful Function Cell (alignas(64))
└── terrain/
    ├── pack.h                             # .gmpack SSD binary format & Baklan/Beşparmak constants
    └── stream.h                           # Dynamic zoom-based quadtree cut & SSD pager API

src/backend/
├── core/
│   ├── context/
│   │   └── lifecycle.c                    # Backend context init, BDA arena allocation, teardown
│   └── cell/
│       └── dispatch.c                     # 64B Stateful Cell ring scheduler & epoch verifier
├── math/
│   ├── pointer/
│   │   └── relptr2d.c                     # 2D self-relative pointer pack/unpack, Morton & bounds math
│   └── gpu/
│       └── directory.c                    # 256B PULSE BDA window & Slang compute normal/slope dispatch
└── terrain/
    ├── storage/
    │   ├── pack_io.c                      # SSD .gmpack writer, verifier, and io_uring/pread loader
    │   └── synth_besparmak.c              # Calibrated Baklan + Beşparmak Dağı DEM/imagery generator
    ├── stream/
    │   ├── lod_select.c                   # Camera zoom & focus -> adaptive quadtree LOD cut + frustum culling
    │   └── uring_pager.c                  # Demand-paged SSD -> GPU BDA tile streaming + live survey fusion
    └── survey/
        ├── camera_model.c                 # Sensor presets, Brown-Conrady lens model, ENU pose & N-ray 3D triangulation
        ├── mvs_fusion.c                   # Multi-View Stereo Patch-ZNCC 3D DEM sweep, Wendland C2 RBF & ortho-blending
        └── manifest_io.c                  # libpng/PPM image I/O, CSV point-cloud loader & .manifest parser

shaders/
├── terrain_math.slang                     # Shared Slang 2026 module: RelPtr2D & Morton BDA addressing
├── terrain_filter.comp.slang              # Compute shader: bicubic gradient, slope & AO baking
├── terrain_cull.comp.slang                # Compute shader: GPU frustum + elevation AABB indirect culling
├── terrain.vert.slang                     # Adaptive quadtree CDLOD vertex displacement + morphing
└── terrain.frag.slang                     # Satellite imagery + metric grid + hypsometric contour shader

examples/demo/
├── include/frame/terrain.h                # Demo render interface for the streamed terrain surface
├── render/terrain.c                       # Graphics & compute pipeline driver for Baklan/Beşparmak map
├── world/kind/terrain_kind.c              # WorldOps object integration (ray-pick elevation & click marker)
└── maps/
    ├── baklan_besparmak.gmpack            # Real SRTM DEM + Satellite/Topo imagery SSD archive
    ├── besparmak_survey.manifest          # Live-watched photogrammetric & geodetic survey manifest
    ├── survey_calib_nadir.png             # Stereo calibration nadir view over Beşparmak summit
    └── survey_calib_oblique.png           # Stereo calibration oblique view over Beşparmak summit

scripts/
├── fetch_besparmak_map.py                 # Downloads AWS Terrarium DEM + Esri/Topo tiles -> .gmpack
└── ingest_survey_photo.py                 # Extracts EXIF/XMP camera/GPS/gimbal tags & appends to manifest
```

---

## 4. How Zoom-Driven Dynamic SSD Streaming & GPU Rendering Work

1. **Camera Zoom & Focus Point Inspection (`lod_select.c`)**:
   - Every frame, `backend_update_camera_zoom(cam_x, cam_y, zoom)` transforms the camera's world focus point `(cam_x, cam_y)` and `zoom` factor into normalized map coordinates $(u, v) \in [0, 1]^2$ over the Baklan–Beşparmak Dağı bounding box ($\pm 12,000\text{ m} = 24\text{ km} \times 24\text{ km}$).
   - Starting from `LOD 0` (the $1\times 1$ root tile covering the entire Baklan plain and Beşparmak massif), the quadtree evaluator computes the screen-space projected size of each tile and subdivides into its 4 child tiles via its self-relative `RelPtr2D` child pointer (`LOD 0` $\rightarrow$ `LOD 4`, 341 tiles total).
2. **Demand-Paged SSD Streaming & On-the-Fly Survey Fusion (`uring_pager.c`)**:
   - If a selected high-resolution child tile (`LOD 1..4`) is not yet resident in the GPU BDA pool, the pager arms a batch of **64-Byte Stateful Function Cells (`GpuMathStreamCell`)**, resolves each tile's exact 4 KB-aligned SSD offset from its `RelPtr2D` without any lookup table, reads the $393,216\text{-byte}$ tile block ($256\times 256$ `u16` DEM + $256\times 256$ `RGBA8` imagery) from `examples/demo/maps/baklan_besparmak.gmpack` on SSD, fuses any active photogrammetric survey photos, Multi-View Stereo depth posts, and geodetic control points (`backend_survey_fuse_tile`), and dispatches `terrain_filter.comp.slang` on the GPU to recompute Sobel surface normals and slope/aspect octants.

---

## 5. Photogrammetric Camera Model, Multi-View Stereo (MVS) & Geodetic Survey Fusion (`src/backend/terrain/survey/`)

Users and map engineers can refine both the 3D `height_u16` DEM elevation and `imagery_rgba8` ortho-imagery of Baklan and Beşparmak Dağı using their own photographs (from smartphones, DJI/RTK drones, or full-frame survey cameras) and ground-truth survey data via [`include/backend/terrain/survey.h`](include/backend/terrain/survey.h):

### 5.1 Physical Lens & Brown-Conrady Camera Model (`camera_model.c`)
- **Built-in Phone/Drone Sensor Presets (`backend_survey_find_sensor_preset`)**: Includes calibrated physical sensor dimensions ($\text{mm} \times \text{mm}$), default physical focal lengths ($\text{mm}$), $35\text{ mm}$ full-frame equivalents, and radial distortion coefficients for `iPhone 16 Pro Main`, `iPhone 15 Pro Main`, `iPhone 15 Pro UltraWide`, `Samsung S24 Ultra Main`, `Xiaomi 14 Ultra 1-inch`, `Google Pixel 9 Pro Main`, `DJI Mavic 3 Enterprise`, `DJI Mini 4 Pro`, `DJI Air 3 Wide`, `DJI Phantom 4 RTK`, `Sony A7R V FullFrame`, and `Custom`.
- **Intrinsic Matrix & FOV (`backend_survey_init_intrinsics`)**:
  $$f_{x,\text{px}} = \frac{f_{\text{mm}}}{W_{\text{sensor,mm}}} W_{\text{px}}, \qquad f_{y,\text{px}} = \frac{f_{\text{mm}}}{H_{\text{sensor,mm}}} H_{\text{px}}, \qquad \text{HFOV} = 2\arctan\!\left(\frac{W_{\text{sensor,mm}}}{2 f_{\text{mm}}}\right)$$
  If only the $35\text{ mm}$ equivalent focal length $f_{35}$ is supplied, $f_{\text{mm}}$ is derived automatically from the sensor diagonal relative to $\sqrt{36^2 + 24^2} = 43.2666\text{ mm}$.
- **Extrinsic Gimbal Pose (`backend_survey_init_pose`)**: Converts WGS84 $(\phi, \lambda)$ or metric world $(X, Y)$, MSL altitude or AGL height, and gimbal Euler angles $(\text{yaw}, \text{pitch}, \text{roll})$ into an orthonormal East-North-Up (ENU) camera basis $(\mathbf{r}, \mathbf{u}, \mathbf{f})$ where $\text{yaw} = 0^\circ$ is North ($+Y$), $90^\circ$ is East ($+X$), and $\text{pitch} = -90^\circ$ is straight-down nadir ($-Z$).
- **Brown-Conrady Forward Projection & Iterative Inverse Undistortion**:
  - `backend_survey_project_world_to_pixel` projects any 3D world point $(X, Y, Z_{\text{elev}})$ into camera coordinates, applies radial ($k_1, k_2, k_3$) and tangential ($p_1, p_2$) distortion, and outputs sub-pixel coordinates $(u_{\text{px}}, v_{\text{px}})$ and depth.
  - `backend_survey_pixel_to_world_ray` inverts the Brown-Conrady distortion via 10-step fixed-point Gauss-Newton iteration and transforms the undistorted camera ray into a unit 3D ENU world ray.

### 5.2 Multi-View 3D Reconstruction & Geodetic Fusion (`mvs_fusion.c`)
- **N-View 3D Least-Squares Ray Triangulation (`backend_survey_triangulate_rays`)**:
  - Given $N \ge 2$ camera rays $(\mathbf{o}_i, \mathbf{d}_i)$ observing the same feature from different positions, heights, and gimbal angles, solves the $3\times 3$ normal system:
    $$\left(\sum_{i=1}^N (\mathbf{I} - \mathbf{d}_i \mathbf{d}_i^\top)\right) \mathbf{P}^* = \sum_{i=1}^N (\mathbf{I} - \mathbf{d}_i \mathbf{d}_i^\top)\mathbf{o}_i$$
    via Gaussian elimination with partial pivoting, enforcing minimum parallax angle ($\ge 0.5^\circ$) and reporting 3D RMSE in meters.
- **Dense Multi-View Stereo (MVS) Patch-ZNCC Elevation Sweep**:
  - When $\ge 2$ overlapping photos with `mvs=1` cover a DEM post $(X, Y)$, `backend_survey_fuse_tile` sweeps candidate elevations $h \in [h_{\text{prior}} - \Delta H, h_{\text{prior}} + \Delta H]$, projects a $3\times 3$ world tangent patch around $(X, Y, h)$ into each view, computes Zero-Mean Normalized Cross-Correlation (ZNCC), refines the correlation peak to sub-step precision via 3-point parabolic fitting, and blends the recovered 3D elevation into `height_u16` ($1/16\text{ m}$ precision).
- **Geodetic Control Points (GCPs) & Breaklines**:
  - Blends point elevations (`point_wgs84`, `point_world`, `survey_csv`) and linear ridge/fault breakline segments (`breakline_wgs84`, `breakline_world`) into the DEM using a $C^2$-continuous Wendland radial basis kernel $\phi(r) = (1 - r)^4 (4r + 1)$ combined with inverse-distance weighting.
- **Occlusion-Aware Projective Ortho-Photo Rectification**:
  - Projects every tile texel $(X, Y, h_{\text{refined}}(X, Y))$ into all overlapping cameras, performs a ray-march self-occlusion check against the DEM so hidden back-slopes are never painted with foreground ridge pixels, and blends bilinearly sampled RGBA8 texels weighted by incidence angle $\cos\theta$, ground sample distance (GSD), and smooth border feathering.

### 5.3 Ingesting Photos & Surveys (`scripts/ingest_survey_photo.py` & `besparmak_survey.manifest`)
```bash
# List all built-in phone & drone camera sensor presets
./scripts/ingest_survey_photo.py --list-presets

# Ingest a smartphone or drone photo (auto-extracts EXIF/XMP GPS, focal length & DJI gimbal tags, or use CLI flags)
./scripts/ingest_survey_photo.py \
    --image my_ridge_photo.png \
    --phone "iPhone 15 Pro Main" \
    --focal-mm 6.765 \
    --lat 37.90582 --lon 29.68415 --alt 1960 \
    --yaw 45 --pitch -56 --roll 0

# Add a map-engineer geodetic control point or CSV point cloud
./scripts/ingest_survey_photo.py --point-wgs84 37.90582 29.68415 1697.5 140.0 0.95
./scripts/ingest_survey_photo.py --survey-csv my_rtk_points.csv
```
Saving `examples/demo/maps/besparmak_survey.manifest` while World 8 is running immediately hot-reloads and re-fuses the 3D terrain in place via `io_uring` `inotify` (or press `L`).

