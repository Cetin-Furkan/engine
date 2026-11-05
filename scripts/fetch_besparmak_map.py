#!/usr/bin/env python3
"""
Downloads multi-resolution real measured SRTM/GMTED Digital Elevation Model (DEM) tiles
from AWS Mapzen Terrarium (z=14 + z=15) and true-color satellite ortho-imagery from
Esri World Imagery (z=14 + z=15 + z=16 Ultra-HD) for the 24 km x 24 km region around:
  - Baklan, Denizli (https://maps.app.goo.gl/B18gGPZMEtULHfkw9 : 37.976748 N, 29.606909 E)
  - Beşparmak Dağı (37.960556 N, 29.636667 E, Summit ~1,691 m, West Peak ~1,307 m)

Compiles the 149-node (5-level: LOD 0..4), 256x256-per-tile 4KB-aligned binary archive:
  examples/demo/maps/baklan_besparmak.gmpack (58,601,472 bytes = 55.89 MiB)
with 2D Self-Relative Pointers (RelPtr2D) and Lilliput 64-bit Unified Tile Headers.
"""

import concurrent.futures
import io
import math
import os
import struct
import sys
import urllib.request
import numpy as np
from PIL import Image, ImageEnhance, ImageFilter

MAGIC_GMP2          = 0x32504D47  # "GMP2"
VERSION_GMP2        = 4
LOD_COUNT           = 5
NODE_COUNT          = 149         # 1 + 4 + 16 + 64 + 64
TILE_DIM            = 256
TILE_SAMPLES        = TILE_DIM * TILE_DIM
TILE_BYTES          = 393216      # 96 x 4KB blocks per tile (131,072B u16 + 262,144B RGBA8)
TILE_BLOCKS_4K      = 96
HEADER_BLOCKS_4K    = 3
HEADER_BYTES        = 12288       # 3 x 4KB blocks
TOTAL_BYTES         = HEADER_BYTES + NODE_COUNT * TILE_BYTES  # 58,601,472 bytes (55.89 MiB)

WORLD_HALF          = 12000.0     # 1.0 world unit = 1.0 real meter (24,000 m x 24,000 m)
VERTICAL_SCALE      = 1.15

# Base 6x6 z=13 region: X in [4767..4772], Y in [3157..3162]
# In z=14: 12x12 tiles: X in [9534..9545], Y in [6314..6325] (3072 x 3072 px)
Z14_X0              = 9534
Z14_Y0              = 6314
Z14_GRID            = 12

# Central 50% (LOD 4 8x8 core, u in [0.25, 0.75], v in [0.25, 0.75]):
# In z=14: X in [9537..9542], Y in [6317..6322] (6x6 tiles)
# In z=15: X in [19074..19085], Y in [12634..12645] (12x12 tiles = 3072 x 3072 px)
Z15_X0              = 19074
Z15_Y0              = 12634
Z15_GRID            = 12

# Ultra-HD z=16 focal patch over Baklan Town & Beşparmak Dağı Summit:
# X16 in [38156..38165] (dx15 in [4..8]), Y16 in [25280..25289] (dy15 in [6..10])
Z16_X0              = 38156
Z16_Y0              = 25280
Z16_W               = 10
Z16_H               = 10

LAT_MIN             = 37.857507156252030
LAT_MAX             = 38.065392351332490
LON_MIN             = 29.487304687500000
LON_MAX             = 29.750976562500000


def relptr2d_pack(block_delta: int, dx: int, dy: int, lod: int, epoch: int = 1) -> int:
    u_blk = block_delta & 0xFFFFF
    u_dx = dx & 0xFFFF
    u_dy = dy & 0xFFFF
    u_lod = lod & 0xF
    seal = ((epoch ^ 0xA5) + (u_lod * 19) + (u_blk & 0xFF)) & 0xFF
    return (seal << 56) | (u_lod << 52) | (u_blk << 32) | (u_dx << 16) | u_dy


def morton2d_encode(x: int, y: int) -> int:
    def part1by1(n: int) -> int:
        n &= 0xFF
        n = (n | (n << 4)) & 0x0F0F
        n = (n | (n << 2)) & 0x3333
        n = (n | (n << 1)) & 0x5555
        return n
    return part1by1(x) | (part1by1(y) << 1)


def tile_header_pack(block_idx: int, morton: int, lod: int, flags: int, min_e: float, max_e: float) -> int:
    q_min = max(0, min(1023, int(round(min_e * 0.5))))
    q_max = max(0, min(1023, int(round(max_e * 0.5))))
    return (
        ((block_idx & 0xFFFFF) << 44)
        | ((morton & 0xFFFF) << 28)
        | ((lod & 0xF) << 24)
        | ((flags & 0xF) << 20)
        | (q_max << 10)
        | q_min
    )


def quadtree_node_index(lod: int, gx: int, gy: int) -> int:
    if lod == 0:
        return 0
    if lod == 1:
        return 1 + gy * 2 + gx
    if lod == 2:
        return 5 + gy * 4 + gx
    if lod == 3:
        return 21 + gy * 8 + gx
    if lod == 4:
        return 85 + (gy - 4) * 8 + (gx - 4)
    return 0


def fetch_image_rgb(url: str) -> np.ndarray | None:
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "EngineV2-TerrainStreamer/4.0 (C23 Vulkan 1.4)"},
    )
    for _ in range(3):
        try:
            with urllib.request.urlopen(req, timeout=12) as resp:
                raw = resp.read()
            img = Image.open(io.BytesIO(raw)).convert("RGB")
            if img.size != (256, 256):
                img = img.resize((256, 256), Image.Resampling.BICUBIC)
            return np.asarray(img, dtype=np.uint8)
        except Exception:
            pass
    return None


def enhance_satellite_mosaic(rgb_u8: np.ndarray) -> np.ndarray:
    # Convert to PIL Image for high-quality unsharp mask + contrast/color enhancement
    img = Image.fromarray(rgb_u8, mode="RGB")
    img = img.filter(ImageFilter.UnsharpMask(radius=1.2, percent=135, threshold=2))
    img = ImageEnhance.Color(img).enhance(1.18)
    img = ImageEnhance.Contrast(img).enhance(1.08)
    arr = np.asarray(img, dtype=np.float32) / 255.0
    # Gentle atmospheric dehaze
    arr[:, :, 0] = np.clip((arr[:, :, 0] - 0.020) / 0.955, 0.0, 1.0)
    arr[:, :, 1] = np.clip((arr[:, :, 1] - 0.020) / 0.955, 0.0, 1.0)
    arr[:, :, 2] = np.clip((arr[:, :, 2] - 0.028) / 0.955, 0.0, 1.0)
    return (arr * 255.0 + 0.5).astype(np.uint8)


def build_pack(out_path: str) -> None:
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    print(
        "[MapFetch] Downloading multi-scale SRTM DEM (z=14..15) + Esri World Imagery (z=14..16 Ultra-HD) "
        "for Baklan & Beşparmak Dağı (24km x 24km, 149 x 256x256 tiles)..."
    )

    tasks = []
    # 1. z=14 12x12 full region (144 DEM + 144 Sat)
    for dy in range(Z14_GRID):
        for dx in range(Z14_GRID):
            tx, ty = Z14_X0 + dx, Z14_Y0 + dy
            tasks.append(("dem14", dx, dy, f"https://s3.amazonaws.com/elevation-tiles-prod/terrarium/14/{tx}/{ty}.png"))
            tasks.append(("sat14", dx, dy, f"https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/14/{ty}/{tx}"))

    # 2. z=15 12x12 inner core (144 DEM + 144 Sat)
    for dy in range(Z15_GRID):
        for dx in range(Z15_GRID):
            tx, ty = Z15_X0 + dx, Z15_Y0 + dy
            tasks.append(("dem15", dx, dy, f"https://s3.amazonaws.com/elevation-tiles-prod/terrarium/15/{tx}/{ty}.png"))
            tasks.append(("sat15", dx, dy, f"https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/15/{ty}/{tx}"))

    # 3. z=16 10x10 ultra-HD focal patch over Baklan & Beşparmak Dağı (100 Sat)
    for dy in range(Z16_H):
        for dx in range(Z16_W):
            tx, ty = Z16_X0 + dx, Z16_Y0 + dy
            tasks.append(("sat16", dx, dy, f"https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/16/{ty}/{tx}"))

    results = {"dem14": {}, "sat14": {}, "dem15": {}, "sat15": {}, "sat16": {}}

    def worker(item):
        kind, dx, dy, url = item
        return kind, dx, dy, fetch_image_rgb(url)

    with concurrent.futures.ThreadPoolExecutor(max_workers=24) as ex:
        for kind, dx, dy, arr in ex.map(worker, tasks):
            if arr is not None:
                results[kind][(dx, dy)] = arr

    print(
        f"[MapFetch] Downloaded z=14: {len(results['dem14'])}/144 DEM, {len(results['sat14'])}/144 Sat | "
        f"z=15: {len(results['dem15'])}/144 DEM, {len(results['sat15'])}/144 Sat | "
        f"z=16 Ultra-HD: {len(results['sat16'])}/100 Sat."
    )
    if len(results["dem14"]) < 144 or len(results["sat14"]) < 144:
        raise RuntimeError("Incomplete z=14 base download")

    # Assemble 3072x3072 outer z=14 arrays (North at row 0 initially, then flip Y so row 0 is South/-Y)
    OUTER_PX = Z14_GRID * 256  # 3072
    dem14_rgb = np.zeros((OUTER_PX, OUTER_PX, 3), dtype=np.float32)
    sat14_rgb = np.zeros((OUTER_PX, OUTER_PX, 3), dtype=np.uint8)
    for dy in range(Z14_GRID):
        for dx in range(Z14_GRID):
            y0, y1 = dy * 256, (dy + 1) * 256
            x0, x1 = dx * 256, (dx + 1) * 256
            dem14_rgb[y0:y1, x0:x1] = results["dem14"][(dx, dy)]
            sat14_rgb[y0:y1, x0:x1] = results["sat14"][(dx, dy)]

    outer_elev = (dem14_rgb[:, :, 0] * 256.0 + dem14_rgb[:, :, 1] + dem14_rgb[:, :, 2] / 256.0) - 32768.0
    outer_elev = np.clip(outer_elev, 750.0, 2200.0)
    sat14_rgb = enhance_satellite_mosaic(sat14_rgb)

    # Assemble 3072x3072 inner core z=15 arrays (covering u in [0.25, 0.75], v in [0.25, 0.75])
    CORE_PX = Z15_GRID * 256  # 3072
    have_core15 = len(results["dem15"]) == 144 and len(results["sat15"]) == 144
    if have_core15:
        dem15_rgb = np.zeros((CORE_PX, CORE_PX, 3), dtype=np.float32)
        sat15_rgb = np.zeros((CORE_PX, CORE_PX, 3), dtype=np.uint8)
        for dy in range(Z15_GRID):
            for dx in range(Z15_GRID):
                y0, y1 = dy * 256, (dy + 1) * 256
                x0, x1 = dx * 256, (dx + 1) * 256
                dem15_rgb[y0:y1, x0:x1] = results["dem15"][(dx, dy)]
                sat15_rgb[y0:y1, x0:x1] = results["sat15"][(dx, dy)]
        core_elev = (dem15_rgb[:, :, 0] * 256.0 + dem15_rgb[:, :, 1] + dem15_rgb[:, :, 2] / 256.0) - 32768.0
        core_elev = np.clip(core_elev, 750.0, 2200.0)

        # Upsample core_elev to 6144x6144 (z=16 resolution) with bicubic interpolation
        # and fuse z=16 Ultra-HD satellite tiles directly into the focal region!
        CORE16_PX = CORE_PX * 2  # 6144
        sat16_full = np.asarray(
            Image.fromarray(sat15_rgb, mode="RGB").resize((CORE16_PX, CORE16_PX), Image.Resampling.LANCZOS),
            dtype=np.uint8,
        ).copy()
        if len(results["sat16"]) == Z16_W * Z16_H:
            # Z16_X0 = 38156 corresponds to (38156 - 2*Z15_X0) = 38156 - 38148 = 8 tiles offset in z=16
            # Z16_Y0 = 25280 corresponds to (25280 - 2*Z15_Y0) = 25280 - 25268 = 12 tiles offset in z=16
            ox16 = (Z16_X0 - 2 * Z15_X0) * 256
            oy16 = (Z16_Y0 - 2 * Z15_Y0) * 256
            for dy in range(Z16_H):
                for dx in range(Z16_W):
                    y0 = oy16 + dy * 256
                    x0 = ox16 + dx * 256
                    if 0 <= y0 and y0 + 256 <= CORE16_PX and 0 <= x0 and x0 + 256 <= CORE16_PX:
                        sat16_full[y0 : y0 + 256, x0 : x0 + 256] = results["sat16"][(dx, dy)]
        sat16_full = enhance_satellite_mosaic(sat16_full)
    else:
        core_elev = outer_elev
        sat16_full = sat14_rgb

    # Flip Y axis so row 0 is South (lat_min, world_min_y) and row N-1 is North (lat_max, world_max_y)
    outer_elev = np.flipud(outer_elev)
    sat14_rgb  = np.flipud(sat14_rgb)
    core_elev  = np.flipud(core_elev)
    sat16_full = np.flipud(sat16_full)

    global_min_e = float(np.min(outer_elev))
    global_max_e = float(max(np.max(outer_elev), np.max(core_elev)))
    print(
        f"[MapFetch] Measured SRTM elevation across 24km x 24km Baklan & Beşparmak Dağı region: "
        f"{global_min_e:.1f} m .. {global_max_e:.1f} m"
    )

    def extract_patch_elev_and_rgb(u0: float, u1: float, v0: float, v1: float) -> tuple[np.ndarray, np.ndarray]:
        # Use high-res z=15/z=16 core whenever the tile lies inside [0.25, 0.75]^2
        if have_core15 and u0 >= 0.2499 and u1 <= 0.7501 and v0 >= 0.2499 and v1 <= 0.7501:
            cu0 = (u0 - 0.25) * 2.0
            cu1 = (u1 - 0.25) * 2.0
            cv0 = (v0 - 0.25) * 2.0
            cv1 = (v1 - 0.25) * 2.0
            src_elev = core_elev
            src_sat  = sat16_full
        else:
            cu0, cu1, cv0, cv1 = u0, u1, v0, v1
            src_elev = outer_elev
            src_sat  = sat14_rgb

        eh, ew = src_elev.shape
        # Sample 256x256 elevation grid via bilinear interpolation on float32 numpy array
        xs = np.linspace(cu0 * (ew - 1), cu1 * (ew - 1), TILE_DIM, dtype=np.float32)
        ys = np.linspace(cv0 * (eh - 1), cv1 * (eh - 1), TILE_DIM, dtype=np.float32)
        x0 = np.clip(np.floor(xs).astype(np.int32), 0, ew - 1)
        x1 = np.clip(x0 + 1, 0, ew - 1)
        y0 = np.clip(np.floor(ys).astype(np.int32), 0, eh - 1)
        y1 = np.clip(y0 + 1, 0, eh - 1)
        wx = (xs - x0)[None, :]
        wy = (ys - y0)[:, None]

        h00 = src_elev[np.ix_(y0, x0)]
        h10 = src_elev[np.ix_(y0, x1)]
        h01 = src_elev[np.ix_(y1, x0)]
        h11 = src_elev[np.ix_(y1, x1)]
        elev_patch = (h00 * (1.0 - wx) + h10 * wx) * (1.0 - wy) + (h01 * (1.0 - wx) + h11 * wx) * wy

        # Crop and Lanczos-resample satellite imagery patch to 256x256
        sh, sw, _ = src_sat.shape
        sx0 = max(0, min(sw - 1, int(round(cu0 * sw))))
        sx1 = max(sx0 + 1, min(sw, int(round(cu1 * sw))))
        sy0 = max(0, min(sh - 1, int(round(cv0 * sh))))
        sy1 = max(sy0 + 1, min(sh, int(round(cv1 * sh))))
        crop = Image.fromarray(src_sat[sy0:sy1, sx0:sx1], mode="RGB")
        if crop.size != (TILE_DIM, TILE_DIM):
            crop = crop.resize((TILE_DIM, TILE_DIM), Image.Resampling.LANCZOS)
        rgb_patch = np.asarray(crop, dtype=np.uint8)
        return elev_patch, rgb_patch

    archive = bytearray(TOTAL_BYTES)

    root_node_rel = relptr2d_pack(0, 0, 0, 0, 1)
    tile_pool_rel = relptr2d_pack(HEADER_BLOCKS_4K, 0, 0, 0, 1)

    dir_prefix = struct.pack(
        "<IIIIffffddddQQIIIIfffI",
        MAGIC_GMP2,
        VERSION_GMP2,
        NODE_COUNT,
        TILE_DIM,
        WORLD_HALF,
        float(global_min_e),
        float(global_max_e),
        VERTICAL_SCALE,
        LAT_MIN,
        LAT_MAX,
        LON_MIN,
        LON_MAX,
        root_node_rel,
        tile_pool_rel,
        1,  # active_leaf_count
        1,  # resident_tile_count
        0,  # stream_loads_total
        0,  # deepest_lod_active
        0.0,
        0.0,
        1.0,
        1,
    )
    archive[0 : len(dir_prefix)] = dir_prefix

    for lod in range(LOD_COUNT):
        grid_n = 1 << lod
        tile_span_world = (WORLD_HALF * 2.0) / grid_n
        sample_spacing_m = 24000.0 / (grid_n * (TILE_DIM - 1))
        gx_range = range(4, 12) if lod == 4 else range(grid_n)
        gy_range = range(4, 12) if lod == 4 else range(grid_n)

        for gy in gy_range:
            for gx in gx_range:
                idx = quadtree_node_index(lod, gx, gy)
                w_min_x = -WORLD_HALF + gx * tile_span_world
                w_max_x = w_min_x + tile_span_world
                w_max_y = WORLD_HALF - gy * tile_span_world
                w_min_y = w_max_y - tile_span_world

                u0 = gx / grid_n
                u1 = (gx + 1) / grid_n
                v1 = 1.0 - (gy / grid_n)
                v0 = 1.0 - ((gy + 1) / grid_n)

                elev_grid, rgb_grid = extract_patch_elev_and_rgb(u0, u1, v0, v1)
                min_e = float(np.min(elev_grid))
                max_e = float(np.max(elev_grid))
                mean_e = float(np.mean(elev_grid))

                # Quantize elevation to 1/16th meter u16
                h_u16 = np.clip(np.rint(elev_grid * 16.0), 0, 65535).astype("<u2")

                # Compute initial slope AO into alpha channel
                dzdy, dzdx = np.gradient(elev_grid, sample_spacing_m)
                grad = np.sqrt(dzdx * dzdx + dzdy * dzdy)
                ao_u8 = np.clip(np.rint((0.84 - 0.16 * grad) * 255.0), 32, 255).astype(np.uint8)

                rgba_u8 = np.empty((TILE_DIM, TILE_DIM, 4), dtype=np.uint8)
                rgba_u8[:, :, 0:3] = rgb_grid
                rgba_u8[:, :, 3]   = ao_u8

                tile_off = HEADER_BYTES + idx * TILE_BYTES
                archive[tile_off : tile_off + 131072] = h_u16.tobytes()
                archive[tile_off + 131072 : tile_off + TILE_BYTES] = rgba_u8.tobytes()

                ssd_block_4k = HEADER_BLOCKS_4K + idx * TILE_BLOCKS_4K
                morton = morton2d_encode(gx, gy)
                parent_idx = quadtree_node_index(lod - 1, gx >> 1, gy >> 1) if lod > 0 else 0
                has_children = (lod < 3) or (lod == 3 and 2 <= gx < 6 and 2 <= gy < 6)
                if has_children:
                    first_child = quadtree_node_index(lod + 1, gx * 2, gy * 2)
                    child_rel = relptr2d_pack(first_child - idx, gx * 2, gy * 2, lod + 1, 1)
                else:
                    child_rel = 0

                rel_ssd = relptr2d_pack(ssd_block_4k, gx, gy, lod, 1)
                rel_gpu = relptr2d_pack(HEADER_BLOCKS_4K, 0, 0, 0, 1) if idx == 0 else 0
                flags = 0x5 if idx == 0 else 0x0
                hdr_u64 = tile_header_pack(ssd_block_4k, morton, lod, flags, min_e, max_e)

                node_bytes = struct.pack(
                    "<QQQQffffHHHBBBBHf",
                    rel_ssd,
                    rel_gpu,
                    child_rel,
                    hdr_u64,
                    w_min_x,
                    w_min_y,
                    w_max_x,
                    w_max_y,
                    idx,
                    parent_idx,
                    0,
                    lod,
                    gx,
                    gy,
                    1 if idx == 0 else 0,
                    0,
                    mean_e,
                )
                node_off = 256 + idx * 64
                archive[node_off : node_off + 64] = node_bytes

    with open(out_path, "wb") as f:
        f.write(archive)

    print(
        f"[MapFetch] Successfully wrote {len(archive)} bytes ({len(archive)/(1024*1024):.2f} MiB) "
        f"to {out_path}"
    )


if __name__ == "__main__":
    target = (
        sys.argv[1]
        if len(sys.argv) > 1
        else os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            "examples",
            "demo",
            "maps",
            "baklan_besparmak.gmpack",
        )
    )
    build_pack(target)
