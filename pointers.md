# Pointer & Closure Layout Notes (`src/backend/`)

This document summarizes the concrete pointer, tile-header, and host-closure layouts implemented in `include/backend/math/` and `src/backend/` for `Engine` (`v0.3.0`), together with the memory-addressing trade-offs that shaped them.

| Structure | Header / Source | Exact Layout | Purpose in `Engine` |
| :--- | :--- | :--- | :--- |
| **`RelPtr2D`** | [`include/backend/math/relptr2d.h`](include/backend/math/relptr2d.h), [`src/backend/math/pointer/relptr2d.c`](src/backend/math/pointer/relptr2d.c) | **64-bit packed tile offset**: signed 20-bit `4 KB` page delta (`bits [19:0]`), 16-bit signed `(dX, dY)` grid coordinates (`bits [51:20]`), 4-bit `LOD` (`bits [55:52]`), and 8-bit `epoch ^ 0xA5` tag (`bits [63:56]`). | Resolves a tile's `4 KB`-aligned block offset (`self + (delta << 12)`) and its 2D world grid coordinates from the same 64-bit word on SSD, in CPU RAM, and inside GPU BDA shaders without pointer swizzling. |
| **`GpuMathTileHeader`** | [`include/backend/math/relptr2d.h`](include/backend/math/relptr2d.h) | **64-bit compact tile header**: 20-bit `4 KB` block index, 16-bit 2D Morton Z-order key, 4-bit `LOD`, 2-bit residency state, and quantized 11-bit `min_m` / `max_m` elevation bounds (`0..2047 m`). | Keeps quadtree node metadata inside a single 8-byte word so GPU frustum and screen-space error culling (`shaders/terrain_cull.comp.slang`) stream linearly across L1/L2 cache lines. |
| **`GpuMathStreamCell`** | [`include/backend/math/cell64.h`](include/backend/math/cell64.h), [`src/backend/core/cell/dispatch.c`](src/backend/core/cell/dispatch.c) | **`alignas(64)` 64-byte host closure**: 8-byte `GpuMathCellFn` function pointer + 56 bytes of inline `io_uring`/BDA state (`RelPtr2D`, `gpu_bda_dst`, `ssd_file_offset`, `tile_id`, `morton_key`, `slot_index`, `generation`, `epoch`, `inline_id`, `user_tag`). | Packs the completion callback and its captured paging state into one exact 64-byte CPU cache line with zero heap allocations and $O(1)$ stale-slot rejection via `epoch == inline_id`. |
| **`GpuMathActiveWindow`** | [`include/backend/terrain/pack.h`](include/backend/terrain/pack.h) | **256-byte directory header** (`4 x 64 B` cache lines) preceding the 341-node quadtree table. | Stores WGS84 origin, metric tile span, elevation scale, and root `RelPtr2D` offsets in a fixed-size header shared verbatim between `.epack`/`.gmpack` files and the GPU BDA directory buffer. |

---

## 1. `RelPtr2D`: Packed Tile-Relative Offset (`include/backend/math/relptr2d.h`)

Standard 64-bit host `void *` pointers cannot be stored directly inside an SSD archive or copied verbatim into a Vulkan `VkDeviceAddress` slab without relocation tables. `RelPtr2D` replaces absolute virtual addresses with a packed 64-bit tile-relative offset:

```text
 63          56 55   52 51            36 35            20 19                   0
+--------------+-------+----------------+----------------+----------------------+
| epoch ^ 0xA5 |  LOD  |   grid_dy_16   |   grid_dx_16   |   signed page_delta  |
|   (8 bits)   | (4 b) |   (int16_t)    |   (int16_t)    |  (20b * 4096 B page) |
+--------------+-------+----------------+----------------+----------------------+
```

- **Page-Aligned Relative Offset (`bits [19:0]`)**: Because `.epack` (`100 KiB` = `25 x 4,096 B`) and `.gmpack` (`384 KiB` = `96 x 4,096 B`) tile blocks are aligned to `4,096 B` boundaries, shifting a 20-bit signed delta left by `12` addresses a $\pm 2\text{ GiB}$ span around the containing directory or node (`addr = self_base + (delta << 12)`).
- **Inline Spatial Coordinates (`bits [55:20]`)**: Embedding `(grid_dx, grid_dy, lod)` directly inside the word lets CPU LOD selection (`lod_select.c`) and GPU culling (`terrain_math.slang`) compute a tile's metric bounding box without dereferencing the target tile payload first.
- **Epoch Tag (`bits [63:56]`)**: Stores `(epoch ^ 0xA5) & 0xFF`. When a resident cache slot is recycled for a new tile, incrementing the slot's 8-bit `epoch` invalidates stale `RelPtr2D` values in $O(1)$ time (`relptr2d_verify`).

---

## 2. `GpuMathStreamCell`: 64-Byte Host Closure (`include/backend/math/cell64.h`)

Asynchronous `io_uring` terrain reads need to remember which quadtree node, SSD file offset, resident GPU BDA slot, and slot generation a pending read belongs to when its CQE arrives on a later turn. Rather than allocating heap callback structs per read, `Engine` uses a fixed array of `alignas(64) GpuMathStreamCell` host closures:

```text
+-----------------------------------------------------------------------+
|              alignas(64) GpuMathStreamCell (64 Bytes)                 |
+------------------+----------------------------------------------------+
|  Bytes 0..7      |  Bytes 8..63 (56 Bytes Inline Paging State)        |
|  [GpuMathCellFn] |  rel_tile, gpu_bda_dst, ssd_file_offset, tile_id,  |
|                  |  morton_key, bytes, slot_index, generation,        |
|                  |  lod, state, epoch, inline_id, user_tag            |
+------------------+----------------------------------------------------+
```

- **Single Cache Line**: `sizeof(GpuMathStreamCell) == 64` and `alignof(GpuMathStreamCell) == 64` are enforced at compile time via `static_assert`. Invoking `cell64_invoke()` loads the function pointer and all 56 bytes of captured state from the same L1 cache line.
- **Stale Completion Rejection**: `cell64_poison_and_reincarnate()` advances both the 32-bit `generation` (packed into the upper 32 bits of `user_tag`) and the 1-byte `epoch` / `inline_id` pair when a cache slot is evicted, ensuring out-of-date completions or mismatched `RelPtr2D` tags are rejected cleanly.

---

## 3. `GpuMathTileHeader`: 64-Bit Compact Quadtree Node (`include/backend/math/relptr2d.h`)

Inspired by compact block-indexed object headers (such as OpenJDK Project Lilliput's 64-bit header compression), `GpuMathTileHeader` packs per-tile quadtree metadata into one `uint64_t`:
- `20-bit` `4 KB` block offset into the resident BDA tile pool or SSD pack.
- `16-bit` 2D Morton Z-order curve key (`relptr2d_morton2d_encode`) for spatially coherent tile traversal.
- `4-bit` `LOD` level (`0..4`) and residency bits.
- Quantized `min_m` and `max_m` elevation bounds (`0..2047 m`) so `terrain_cull.comp.slang` tests tight 3D AABBs against the camera frustum before emitting `VkDrawIndirectCommand` records.
