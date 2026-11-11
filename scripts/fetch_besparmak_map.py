#!/usr/bin/env python3
"""
Downloads multi-resolution real measured SRTM/GMTED Digital Elevation Model (DEM) tiles
from AWS Mapzen Terrarium (z=14 + z=15) and true-color satellite ortho-imagery from
Esri World Imagery (z=14 + z=15 + z=16 Ultra-HD) for the 24 km x 24 km region around:
  - Baklan, Denizli (https://maps.app.goo.gl/B18gGPZMEtULHfkw9 : 37.976748 N, 29.606909 E)
  - Beşparmak Dağı (37.960556 N, 29.636667 E, Summit ~1,691 m, West Peak ~1,307 m)

Compiles the complete 341-node (5-level: LOD 0..4, full 16x16 LOD 4), 256x256-per-tile
4KB-aligned binary archive:
  examples/demo/maps/baklan_besparmak.gmpack (134,111,232 bytes = 127.90 MiB)
with a single unified 6144x6144 cosine-feathered & color-matched Master DEM + Satellite Mosaic,
2D Self-Relative Pointers (RelPtr2D), and Lilliput 64-bit Unified Tile Headers.
"""

import concurrent.futures
import io
import os
import struct
import sys
import urllib.request
import numpy as np
from PIL import Image, ImageEnhance, ImageFilter

MAGIC_GMP2          = 0x32504D47  # "GMP2"
VERSION_GMP2        = 5
LOD_COUNT           = 5
NODE_COUNT          = 341         # 1 + 4 + 16 + 64 + 256 (complete 5-level quadtree)
TILE_DIM            = 256
TILE_SAMPLES        = TILE_DIM * TILE_DIM
TILE_BYTES          = 393216      # 96 x 4KB blocks per tile (131,072B u16 + 262,144B RGBA8)
TILE_BLOCKS_4K      = 96
HEADER_BLOCKS_4K    = 6
HEADER_BYTES        = 24576       # 6 x 4KB blocks (256B dir + 341*64B nodes + 2496B pad)
TOTAL_BYTES         = HEADER_BYTES + NODE_COUNT * TILE_BYTES  # 134,111,232 bytes (127.90 MiB)

WORLD_HALF          = 12000.0     # 1.0 world unit = 1.0 real meter (24,000 m x 24,000 m)
VERTICAL_SCALE      = 1.15

# Base 6x6 z=13 region: X in [4767..4772], Y in [3157..3162]
# In z=14: 12x12 tiles: X in [9534..9545], Y in [6314..6325] (3072 x 3072 px)
Z14_X0              = 9534
Z14_Y0              = 6314
Z14_GRID            = 12

# Extended 16x16 z=15 region covering [4/24 .. 20/24] = [0.1667 .. 0.8333] across the entire
# Beşparmak Dağı mountain range (south of 37.89°N to north of Baklan at 38.03°N):
# In z=15 (where full map is X in [19068..19091], Y in [12628..12651]):
Z15_X0              = 19072       # offset 4 tiles from 19068 (px 1024..5120 in 6144x6144 master)
Z15_Y0              = 12632       # offset 4 tiles from 12628 (px 1024..5120 in 6144x6144 master)
Z15_GRID            = 16          # 16x16 tiles = 4096 x 4096 px

# Extended 14x14 Ultra-HD z=16 focal patch over the entire Beşparmak Dağı massif & Baklan:
# In z=16 (where full map is X in [38136..38183], Y in [25256..25303]):
# X16 in [38152..38165] (offset 16..30 in z=16 -> px 2048..3840 in 6144x6144 master)
# Y16 in [25278..25291] (offset 22..36 in z=16 -> px 2816..4608 in 6144x6144 master)
Z16_X0              = 38152
Z16_Y0              = 25278
Z16_W               = 14
Z16_H               = 14

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
        return 85 + gy * 16 + gx
    return 0


def fetch_image_rgb(url: str) -> np.ndarray | None:
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "EngineV2-TerrainStreamer/5.0 (C23 Vulkan 1.4)"},
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


def make_cosine_feather_mask(h: int, w: int, border_px: int) -> np.ndarray:
    """Builds a smooth 2D raised-cosine feather mask in [0.0, 1.0] that tapers to 0 at all 4 edges."""
    ys = np.arange(h, dtype=np.float32)
    xs = np.arange(w, dtype=np.float32)
    dist_y = np.minimum(ys, (h - 1) - ys)
    dist_x = np.minimum(xs, (w - 1) - xs)
    ty = np.clip(dist_y / max(float(border_px), 1.0), 0.0, 1.0)
    tx = np.clip(dist_x / max(float(border_px), 1.0), 0.0, 1.0)
    wy = 0.5 - 0.5 * np.cos(np.pi * ty)
    wx = 0.5 - 0.5 * np.cos(np.pi * tx)
    return wy[:, None] * wx[None, :]


def match_patch_radiometry(patch_u8: np.ndarray, base_u8: np.ndarray, blur_radius: int = 96) -> np.ndarray:
    """
    Matches the low-frequency color/luminance envelope of `patch_u8` to `base_u8` while
    preserving 100% of `patch_u8`'s high-frequency crispness, eliminating multi-pass satellite
    color jumps across zoom levels.
    """
    h, w, _ = patch_u8.shape
    small_w = max(w // 8, 16)
    small_h = max(h // 8, 16)
    rad = max(blur_radius // 8, 4)

    patch_low = np.asarray(
        Image.fromarray(patch_u8, mode="RGB")
        .resize((small_w, small_h), Image.Resampling.BILINEAR)
        .filter(ImageFilter.GaussianBlur(radius=rad))
        .resize((w, h), Image.Resampling.BILINEAR),
        dtype=np.float32,
    )
    base_low = np.asarray(
        Image.fromarray(base_u8, mode="RGB")
        .resize((small_w, small_h), Image.Resampling.BILINEAR)
        .filter(ImageFilter.GaussianBlur(radius=rad))
        .resize((w, h), Image.Resampling.BILINEAR),
        dtype=np.float32,
    )
    # High-frequency detail from high-res patch + low-frequency radiometry blended toward base
    patch_f = patch_u8.astype(np.float32)
    corrected = patch_f + (base_low - patch_low) * 0.85
    return np.clip(corrected + 0.5, 0.0, 255.0).astype(np.uint8)


def enhance_satellite_mosaic(rgb_u8: np.ndarray) -> np.ndarray:
    img = Image.fromarray(rgb_u8, mode="RGB")
    img = img.filter(ImageFilter.UnsharpMask(radius=1.1, percent=130, threshold=2))
    img = ImageEnhance.Color(img).enhance(1.16)
    img = ImageEnhance.Contrast(img).enhance(1.07)
    arr = np.asarray(img, dtype=np.float32) / 255.0
    arr[:, :, 0] = np.clip((arr[:, :, 0] - 0.018) / 0.960, 0.0, 1.0)
    arr[:, :, 1] = np.clip((arr[:, :, 1] - 0.018) / 0.960, 0.0, 1.0)
    arr[:, :, 2] = np.clip((arr[:, :, 2] - 0.025) / 0.960, 0.0, 1.0)
    return (arr * 255.0 + 0.5).astype(np.uint8)


def build_pack(out_path: str) -> None:
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    print(
        "[MapFetch] Downloading multi-scale SRTM DEM (z=14..15) + Esri World Imagery (z=14..16 Ultra-HD) "
        "for Baklan & Beşparmak Dağı (24km x 24km, complete 341-node 256x256 quadtree)..."
    )

    tasks = []
    # 1. z=14 12x12 full region (144 DEM + 144 Sat)
    for dy in range(Z14_GRID):
        for dx in range(Z14_GRID):
            tx, ty = Z14_X0 + dx, Z14_Y0 + dy
            tasks.append(("dem14", dx, dy, f"https://s3.amazonaws.com/elevation-tiles-prod/terrarium/14/{tx}/{ty}.png"))
            tasks.append(("sat14", dx, dy, f"https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/14/{ty}/{tx}"))

    # 2. z=15 16x16 extended region covering entire Beşparmak Dağı & Baklan (256 DEM + 256 Sat)
    for dy in range(Z15_GRID):
        for dx in range(Z15_GRID):
            tx, ty = Z15_X0 + dx, Z15_Y0 + dy
            tasks.append(("dem15", dx, dy, f"https://s3.amazonaws.com/elevation-tiles-prod/terrarium/15/{tx}/{ty}.png"))
            tasks.append(("sat15", dx, dy, f"https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/15/{ty}/{tx}"))

    # 3. z=16 14x14 ultra-HD focal patch over Baklan & Beşparmak Dağı (196 Sat)
    for dy in range(Z16_H):
        for dx in range(Z16_W):
            tx, ty = Z16_X0 + dx, Z16_Y0 + dy
            tasks.append(("sat16", dx, dy, f"https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/16/{ty}/{tx}"))

    results = {"dem14": {}, "sat14": {}, "dem15": {}, "sat15": {}, "sat16": {}}

    def worker(item):
        kind, dx, dy, url = item
        return kind, dx, dy, fetch_image_rgb(url)

    with concurrent.futures.ThreadPoolExecutor(max_workers=32) as ex:
        for kind, dx, dy, arr in ex.map(worker, tasks):
            if arr is not None:
                results[kind][(dx, dy)] = arr

    n_dem15 = Z15_GRID * Z15_GRID
    n_sat16 = Z16_W * Z16_H
    print(
        f"[MapFetch] Downloaded z=14: {len(results['dem14'])}/144 DEM, {len(results['sat14'])}/144 Sat | "
        f"z=15: {len(results['dem15'])}/{n_dem15} DEM, {len(results['sat15'])}/{n_dem15} Sat | "
        f"z=16 Ultra-HD: {len(results['sat16'])}/{n_sat16} Sat."
    )
    if len(results["dem14"]) < 144 or len(results["sat14"]) < 144:
        raise RuntimeError("Incomplete z=14 base download")

    # Assemble 3072x3072 outer z=14 arrays (North at row 0 initially)
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
    outer_elev = np.clip(outer_elev, 750.0, 2200.0).astype(np.float32)

    # Build Unified 6144x6144 Master Elevation & Master Satellite Canvas (covering 100% of 24km x 24km)
    MASTER_PX = OUTER_PX * 2  # 6144 (exact z=15 resolution across the full 24x24 z=15 grid)
    master_elev = np.asarray(
        Image.fromarray(outer_elev, mode="F").resize((MASTER_PX, MASTER_PX), Image.Resampling.BICUBIC),
        dtype=np.float32,
    ).copy()
    master_sat = np.asarray(
        Image.fromarray(sat14_rgb, mode="RGB").resize((MASTER_PX, MASTER_PX), Image.Resampling.LANCZOS),
        dtype=np.uint8,
    ).copy()

    # Blend 16x16 z=15 DEM + Satellite Mosaic into [1024:5120, 1024:5120] of the 6144x6144 Master Canvas
    if len(results["dem15"]) == n_dem15 and len(results["sat15"]) == n_dem15:
        CORE15_PX = Z15_GRID * 256  # 4096
        dem15_rgb = np.zeros((CORE15_PX, CORE15_PX, 3), dtype=np.float32)
        sat15_rgb = np.zeros((CORE15_PX, CORE15_PX, 3), dtype=np.uint8)
        for dy in range(Z15_GRID):
            for dx in range(Z15_GRID):
                y0, y1 = dy * 256, (dy + 1) * 256
                x0, x1 = dx * 256, (dx + 1) * 256
                dem15_rgb[y0:y1, x0:x1] = results["dem15"][(dx, dy)]
                sat15_rgb[y0:y1, x0:x1] = results["sat15"][(dx, dy)]
        core15_elev = (dem15_rgb[:, :, 0] * 256.0 + dem15_rgb[:, :, 1] + dem15_rgb[:, :, 2] / 256.0) - 32768.0
        core15_elev = np.clip(core15_elev, 750.0, 2200.0).astype(np.float32)

        ox15 = (Z15_X0 - 2 * Z14_X0) * 256  # 1024
        oy15 = (Z15_Y0 - 2 * Z14_Y0) * 256  # 1024
        mask15 = make_cosine_feather_mask(CORE15_PX, CORE15_PX, border_px=384)

        base_elev_sub = master_elev[oy15 : oy15 + CORE15_PX, ox15 : ox15 + CORE15_PX]
        master_elev[oy15 : oy15 + CORE15_PX, ox15 : ox15 + CORE15_PX] = (
            base_elev_sub * (1.0 - mask15) + core15_elev * mask15
        )

        base_sat_sub = master_sat[oy15 : oy15 + CORE15_PX, ox15 : ox15 + CORE15_PX]
        sat15_matched = match_patch_radiometry(sat15_rgb, base_sat_sub, blur_radius=96)
        blended15 = (
            base_sat_sub.astype(np.float32) * (1.0 - mask15[:, :, None])
            + sat15_matched.astype(np.float32) * mask15[:, :, None]
        )
        master_sat[oy15 : oy15 + CORE15_PX, ox15 : ox15 + CORE15_PX] = (blended15 + 0.5).astype(np.uint8)

    # Blend 14x14 z=16 Ultra-HD Satellite Mosaic into the 6144x6144 Master Canvas
    if len(results["sat16"]) == n_sat16:
        RAW16_W = Z16_W * 256  # 3584
        RAW16_H = Z16_H * 256  # 3584
        sat16_raw = np.zeros((RAW16_H, RAW16_W, 3), dtype=np.uint8)
        for dy in range(Z16_H):
            for dx in range(Z16_W):
                y0, y1 = dy * 256, (dy + 1) * 256
                x0, x1 = dx * 256, (dx + 1) * 256
                sat16_raw[y0:y1, x0:x1] = results["sat16"][(dx, dy)]

        # In the 6144x6144 (z=15 scale) master canvas, each z=16 tile covers 128x128 master pixels
        DST16_W = Z16_W * 128  # 1792
        DST16_H = Z16_H * 128  # 1792
        ox16 = (Z16_X0 - 4 * Z14_X0) * 128  # (38152 - 38136) * 128 = 2048
        oy16 = (Z16_Y0 - 4 * Z14_Y0) * 128  # (25278 - 25256) * 128 = 2816
        sat16_res = np.asarray(
            Image.fromarray(sat16_raw, mode="RGB").resize((DST16_W, DST16_H), Image.Resampling.LANCZOS),
            dtype=np.uint8,
        )
        base16_sub = master_sat[oy16 : oy16 + DST16_H, ox16 : ox16 + DST16_W]
        sat16_matched = match_patch_radiometry(sat16_res, base16_sub, blur_radius=80)
        mask16 = make_cosine_feather_mask(DST16_H, DST16_W, border_px=256)
        blended16 = (
            base16_sub.astype(np.float32) * (1.0 - mask16[:, :, None])
            + sat16_matched.astype(np.float32) * mask16[:, :, None]
        )
        master_sat[oy16 : oy16 + DST16_H, ox16 : ox16 + DST16_W] = (blended16 + 0.5).astype(np.uint8)

    # Enhance the unified master satellite canvas once so tone and sharpness are 100% consistent everywhere
    master_sat = enhance_satellite_mosaic(master_sat)

    # Flip Y axis so row 0 is South (lat_min, world_min_y) and row N-1 is North (lat_max, world_max_y)
    master_elev = np.flipud(master_elev)
    master_sat  = np.flipud(master_sat)
    master_sat_f = master_sat.astype(np.float32)

    global_min_e = float(np.min(master_elev))
    global_max_e = float(np.max(master_elev))
    print(
        f"[MapFetch] Unified 6144x6144 Master DEM & Imagery ready: "
        f"{global_min_e:.1f} m .. {global_max_e:.1f} m across 341 quadtree nodes"
    )

    def extract_patch_elev_and_rgb(u0: float, u1: float, v0: float, v1: float) -> tuple[np.ndarray, np.ndarray]:
        eh, ew = master_elev.shape
        # Sample both 256x256 elevation and 256x256 RGB on the EXACT same continuous coordinate grid
        # so adjacent tiles share bit-for-bit identical boundary posts (0 and 255) and texels!
        xs = np.linspace(u0 * (ew - 1), u1 * (ew - 1), TILE_DIM, dtype=np.float32)
        ys = np.linspace(v0 * (eh - 1), v1 * (eh - 1), TILE_DIM, dtype=np.float32)
        x0 = np.clip(np.floor(xs).astype(np.int32), 0, ew - 1)
        x1 = np.clip(x0 + 1, 0, ew - 1)
        y0 = np.clip(np.floor(ys).astype(np.int32), 0, eh - 1)
        y1 = np.clip(y0 + 1, 0, eh - 1)
        wx = (xs - x0)[None, :]
        wy = (ys - y0)[:, None]

        h00 = master_elev[np.ix_(y0, x0)]
        h10 = master_elev[np.ix_(y0, x1)]
        h01 = master_elev[np.ix_(y1, x0)]
        h11 = master_elev[np.ix_(y1, x1)]
        elev_patch = (h00 * (1.0 - wx) + h10 * wx) * (1.0 - wy) + (h01 * (1.0 - wx) + h11 * wx) * wy

        wx3 = wx[:, :, None]
        wy3 = wy[:, :, None]
        c00 = master_sat_f[np.ix_(y0, x0)]
        c10 = master_sat_f[np.ix_(y0, x1)]
        c01 = master_sat_f[np.ix_(y1, x0)]
        c11 = master_sat_f[np.ix_(y1, x1)]
        rgb_f = (c00 * (1.0 - wx3) + c10 * wx3) * (1.0 - wy3) + (c01 * (1.0 - wx3) + c11 * wx3) * wy3
        rgb_patch = np.clip(rgb_f + 0.5, 0.0, 255.0).astype(np.uint8)
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

        for gy in range(grid_n):
            for gx in range(grid_n):
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
                has_children = lod + 1 < LOD_COUNT
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
