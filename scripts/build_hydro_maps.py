#!/usr/bin/env python3
"""Open real-world hydrological catchment builder for .epack v2 deep-quadtree archives.

Datasets (all open & free):
  1. alptal      - Alptal / Erlenbach CH (EPSG:2056): swisstopo swissALTI3D (0.5m) + SWISSIMAGE DOP10 (0.10m)
  2. luquillo    - Luquillo / El Yunque PR (EPSG:26920): USGS 3DEP 1m D24 + NAIP 0.30m COG
  3. hj_andrews  - H.J. Andrews OR (EPSG:26910): USGS 3DEP 1m McKenzieRiver + NAIP 0.60m 2024 COG
  4. wuestebach  - Wuestebach / Rur DE (EPSG:25832): NRW DGM1 (1m) + NRW DOP10 WMS/JP2 (0.10m)
"""

from __future__ import annotations

import argparse
import ctypes
import math
import os
import shutil
import struct
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from io import BytesIO
from pathlib import Path
from typing import Callable, Sequence
from collections import OrderedDict

import numpy as np
import rasterio
import requests
from PIL import Image
from pyproj import Transformer
from rasterio.enums import Resampling
from rasterio.transform import from_bounds
from rasterio.vrt import WarpedVRT
from rasterio.warp import transform_bounds
from rasterio.windows import Window

ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = ROOT / "build"
CACHE_ROOT = BUILD_DIR / "hydro_cache"
MAPS_DIR = ROOT / "examples" / "demo" / "maps"
SO_PATH = BUILD_DIR / "epack_v2_enc.so"
BUILDER_C = ROOT / "scripts" / "epack_v2_builder.c"

EPACK_V2_MAGIC = 0x324B5045
EPACK_V2_HEADER_BYTES = 24576
EPACK_V2_DEM_RGB_BYTES = 86016
EPACK_V2_RGB_BYTES = 36864
EPACK_V2_TILE_DEM_RGB = 0
EPACK_V2_TILE_RGB = 1
WORLD_HALF_M = 10000.0
BASE_DENSE_LOD = 7  # 128x128 tiles @ 0.610 m/px across full 20x20 km


class CEpackV2BuildMeta(ctypes.Structure):
    _fields_ = [
        ("lat_min_deg", ctypes.c_double),
        ("lat_max_deg", ctypes.c_double),
        ("lon_min_deg", ctypes.c_double),
        ("lon_max_deg", ctypes.c_double),
        ("world_half_extent", ctypes.c_float),
        ("elev_min_m", ctypes.c_float),
        ("elev_max_m", ctypes.c_float),
        ("center_east_m", ctypes.c_float),
        ("center_north_m", ctypes.c_float),
        ("total_node_count", ctypes.c_uint32),
        ("max_lod", ctypes.c_uint32),
        ("epsg_code", ctypes.c_uint32),
        ("dem_res_mm", ctypes.c_uint32),
        ("ortho_res_mm", ctypes.c_uint32),
        ("disk_table_offset", ctypes.c_uint64),
        ("tile_data_offset", ctypes.c_uint64),
    ]


# 32-byte EpackDiskNode matching include/backend/terrain/pack.h
DISK_NODE_STRUCT = struct.Struct("<QIHHfffBBBB")
assert DISK_NODE_STRUCT.size == 32


def load_encoder_lib() -> ctypes.CDLL:
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    need_build = (not SO_PATH.exists()) or (
        BUILDER_C.stat().st_mtime > SO_PATH.stat().st_mtime
    )
    if need_build:
        cmd = [
            "gcc",
            "-std=c23",
            "-shared",
            "-fPIC",
            "-O3",
            "-march=native",
            "-D_GNU_SOURCE",
            f"-I{ROOT / 'include'}",
            f"-I{ROOT / 'src'}",
            str(BUILDER_C),
            str(ROOT / "src/backend/terrain/storage/pack_io.c"),
            str(ROOT / "src/backend/math/pointer/relptr2d.c"),
            str(ROOT / "src/backend/terrain/storage/synth_besparmak.c"),
            "-lm",
            "-o",
            str(SO_PATH),
        ]
        subprocess.run(cmd, check=True)
    lib = ctypes.CDLL(str(SO_PATH))
    lib.epack_v2_encode_block_tiles.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_int,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
    ]
    lib.epack_v2_encode_block_tiles.restype = None
    lib.epack_v2_downsample_2x_north_up.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_uint32,
    ]
    lib.epack_v2_downsample_2x_north_up.restype = None
    lib.epack_v2_write_header.argtypes = [
        ctypes.c_char_p,
        ctypes.POINTER(CEpackV2BuildMeta),
    ]
    lib.epack_v2_write_header.restype = ctypes.c_int
    lib.epack_v2_fix_tile_rim.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
    ]
    lib.epack_v2_fix_tile_rim.restype = ctypes.c_int
    return lib


@dataclass
class RoiSpec:
    lod: int
    # Local map offsets (meters relative to center) [-10000, +10000]
    min_wx: float
    min_wy: float
    max_wx: float
    max_wy: float
    has_dem: bool


@dataclass
class CatchmentSpec:
    key: str
    out_filename: str
    epsg: int
    center_e: float
    center_n: float
    dem_res_mm: int
    ortho_res_mm: int
    rois: list[RoiSpec]


CATCHMENTS: dict[str, CatchmentSpec] = {
    "alptal": CatchmentSpec(
        key="alptal",
        out_filename="alptal_ch.epack",
        epsg=2056,
        center_e=2697000.0,
        center_n=1212000.0,
        dem_res_mm=500,
        ortho_res_mm=100,
        rois=[
            # LOD 8: 5x5 km focal Erlenbach / Vogelbach / Alpthal core @ 0.305 m/px (0.5m DEM + 0.1m DOP)
            RoiSpec(8, -2500.0, -2500.0, 2500.0, 2500.0, True),
            # LOD 9: 2.5x2.5 km Erlenbach / Vogelbach @ 0.153 m/px RGB
            RoiSpec(9, -1250.0, -1875.0, 1250.0, 625.0, False),
            # LOD 10: 1.0x1.0 km Erlenbach gauge & torrent channel @ 0.076 m/px RGB
            RoiSpec(10, -500.0, -1250.0, 500.0, -250.0, False),
        ],
    ),
    "luquillo": CatchmentSpec(
        key="luquillo",
        out_filename="luquillo_pr.epack",
        epsg=26920,
        center_e=205000.0,
        center_n=2026000.0,
        dem_res_mm=1000,
        ortho_res_mm=300,
        rois=[
            # LOD 8: 5x5 km El Yunque / Mameyes / Bisley / Icacos rainforest core @ 0.305 m/px native NAIP RGB
            RoiSpec(8, -2500.0, -2500.0, 2500.0, 2500.0, False),
        ],
    ),
    "hj_andrews": CatchmentSpec(
        key="hj_andrews",
        out_filename="hj_andrews_or.epack",
        epsg=26910,
        center_e=564000.0,
        center_n=4890000.0,
        dem_res_mm=1000,
        ortho_res_mm=600,
        rois=[
            # LOD 8: 5x5 km HJA HQ / Lookout Creek / WS 1-3 focal catchment @ 0.305 m/px RGB
            RoiSpec(8, -2500.0, 3750.0, 2500.0, 8750.0, False),
        ],
    ),
    "wuestebach": CatchmentSpec(
        key="wuestebach",
        out_filename="wuestebach_de.epack",
        epsg=25832,
        center_e=318000.0,
        center_n=5606000.0,
        dem_res_mm=1000,
        ortho_res_mm=100,
        rois=[
            # LOD 8: 5x5 km SW quadrant covering Wuestebach & Erkensruhr [308..313km, 5596..5601km]
            RoiSpec(8, -10000.0, -10000.0, -5000.0, -5000.0, False),
            # LOD 9: 2.5x2.5 km Wuestebach catchment [309375..311875, 5596500..5599000]
            RoiSpec(9, -8625.0, -9500.0, -6125.0, -7000.0, False),
            # LOD 10: 1.0x1.0 km Wuestebach gauge & hillslope lysimeter site @ 0.076 m/px
            RoiSpec(10, -7900.0, -8700.0, -6900.0, -7700.0, False),
        ],
    ),
}


@dataclass
class NodeRec:
    disk_idx: int
    lod: int
    gx: int
    gy: int
    first_child_idx: int
    has_dem: bool
    has_rgb: bool
    ssd_offset: int = 0
    elev_min: float = 0.0
    elev_max: float = 0.0
    elev_mean: float = 0.0


def node_world_bounds(lod: int, gx: int, gy: int) -> tuple[float, float, float, float]:
    span = (2.0 * WORLD_HALF_M) / float(1 << lod)
    x0 = -WORLD_HALF_M + gx * span
    y0 = -WORLD_HALF_M + gy * span
    return (x0, y0, x0 + span, y0 + span)


def build_quadtree_topology(spec: CatchmentSpec) -> tuple[list[NodeRec], dict[tuple[int, int, int], int], int]:
    """Builds breadth-first quadtree with complete 4-child quads matching lod_select.c."""
    roi_by_child_lod = {r.lod: r for r in spec.rois}
    max_lod = max([BASE_DENSE_LOD] + [r.lod for r in spec.rois])

    nodes: list[NodeRec] = [
        NodeRec(0, 0, 0, 0, 0xFFFFFFFF, True, True)
    ]
    index_map: dict[tuple[int, int, int], int] = {(0, 0, 0): 0}

    q_head = 0
    while q_head < len(nodes):
        cur = nodes[q_head]
        q_head += 1
        next_lod = cur.lod + 1
        if next_lod > max_lod:
            continue

        should_sub = False
        child_has_dem = True
        if next_lod <= BASE_DENSE_LOD:
            should_sub = True
            child_has_dem = True
        elif next_lod in roi_by_child_lod:
            r = roi_by_child_lod[next_lod]
            x0, y0, x1, y1 = node_world_bounds(cur.lod, cur.gx, cur.gy)
            if x1 > r.min_wx and x0 < r.max_wx and y1 > r.min_wy and y0 < r.max_wy:
                should_sub = True
                child_has_dem = r.has_dem

        if not should_sub:
            continue

        fc = len(nodes)
        cur.first_child_idx = fc
        for dy in (0, 1):
            for dx in (0, 1):
                cgx = cur.gx * 2 + dx
                cgy = cur.gy * 2 + dy
                rec = NodeRec(len(nodes), next_lod, cgx, cgy, 0xFFFFFFFF, child_has_dem, True)
                nodes.append(rec)
                index_map[(next_lod, cgx, cgy)] = rec.disk_idx

    return nodes, index_map, max_lod


def retry_call(fn: Callable, label: str, attempts: int = 6):
    last_err = None
    for i in range(attempts):
        try:
            return fn()
        except Exception as e:
            last_err = e
            wait_s = min(15.0, 1.5 * (1.6 ** i))
            print(f"  [retry {i+1}/{attempts}] {label}: {e} (sleep {wait_s:.1f}s)", flush=True)
            time.sleep(wait_s)
    raise RuntimeError(f"Failed after {attempts} attempts for {label}: {last_err}")


def download_file_cached(url: str, dst: Path, min_bytes: int = 256) -> Path:
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists() and dst.stat().st_size >= min_bytes:
        return dst
    tmp = dst.with_suffix(dst.suffix + ".part")

    def _do():
        with requests.get(url, stream=True, timeout=45) as r:
            r.raise_for_status()
            with open(tmp, "wb") as f:
                for chunk in r.iter_content(chunk_size=131072):
                    if chunk:
                        f.write(chunk)
        os.replace(tmp, dst)
        return dst

    return retry_call(_do, f"GET {url.split('/')[-1]}")


def bilinear_upsample_2d(src: np.ndarray, out_h: int, out_w: int) -> np.ndarray:
    """Endpoint-preserving bilinear upsample from (in_h, in_w) to (out_h, out_w) in float32."""
    in_h, in_w = src.shape[:2]
    if in_h == out_h and in_w == out_w:
        return src
    gy = np.linspace(0.0, in_h - 1.0, out_h, dtype=np.float32)
    gx = np.linspace(0.0, in_w - 1.0, out_w, dtype=np.float32)
    y0 = np.floor(gy).astype(np.int32)
    x0 = np.floor(gx).astype(np.int32)
    y1 = np.minimum(y0 + 1, in_h - 1)
    x1 = np.minimum(x0 + 1, in_w - 1)
    wy = (gy - y0)[:, None]
    wx = (gx - x0)[None, :]

    if src.ndim == 2:
        a = src[np.ix_(y0, x0)]
        b = src[np.ix_(y0, x1)]
        c = src[np.ix_(y1, x0)]
        d = src[np.ix_(y1, x1)]
        return (
            a * ((1.0 - wy) * (1.0 - wx))
            + b * ((1.0 - wy) * wx)
            + c * (wy * (1.0 - wx))
            + d * (wy * wx)
        ).astype(np.float32)
    else:
        wy = wy[:, :, None]
        wx = wx[:, :, None]
        a = src[np.ix_(y0, x0)].astype(np.float32)
        b = src[np.ix_(y0, x1)].astype(np.float32)
        c = src[np.ix_(y1, x0)].astype(np.float32)
        d = src[np.ix_(y1, x1)].astype(np.float32)
        out = (
            a * ((1.0 - wy) * (1.0 - wx))
            + b * ((1.0 - wy) * wx)
            + c * (wy * (1.0 - wx))
            + d * (wy * wx)
        )
        return np.clip(np.rint(out), 0, 255).astype(np.uint8)


def _get_cached_dataset(path: str, ds_cache: OrderedDict | None, cap: int = 64):
    if ds_cache is None:
        return rasterio.open(path), True
    ds = ds_cache.get(path)
    if ds is not None and not ds.closed:
        ds_cache.move_to_end(path)
        return ds, False
    while len(ds_cache) >= cap:
        _, old = ds_cache.popitem(last=False)
        try:
            old.close()
        except Exception:
            pass
    ds = rasterio.open(path)
    ds_cache[path] = ds
    return ds, False


def close_ds_cache(ds_cache: OrderedDict | None) -> None:
    if not ds_cache:
        return
    for ds in list(ds_cache.values()):
        try:
            ds.close()
        except Exception:
            pass
    ds_cache.clear()


def mosaic_warp_window(
    tile_items: Sequence[tuple[tuple[float, float, float, float], str]],
    crs: str,
    bounds: tuple[float, float, float, float],
    width: int,
    height: int,
    is_dem: bool,
    overview_mode: str = "AUTO",
    nodata_fill: float = 0.0,
    ds_cache: OrderedDict | None = None,
) -> np.ndarray:
    """Composites intersecting GeoTIFF/COG sources directly into exact endpoint-inclusive window."""
    bx0, by0, bx1, by1 = bounds
    span_x = bx1 - bx0
    span_y = by1 - by0
    pad_x = 0.5 * span_x / float(max(width - 1, 1))
    pad_y = 0.5 * span_y / float(max(height - 1, 1))
    w_bounds = (bx0 - pad_x, by0 - pad_y, bx1 + pad_x, by1 + pad_y)
    dst_tf = from_bounds(*w_bounds, width, height)

    wx0, wy0, wx1, wy1 = w_bounds
    px_x = (wx1 - wx0) / float(width)
    px_y = (wy1 - wy0) / float(height)

    margin = max(10.0, pad_x * 4.0)
    hits = [
        path
        for (tb, path) in tile_items
        if tb[2] >= bx0 - margin and tb[0] <= bx1 + margin and tb[3] >= by0 - margin and tb[1] <= by1 + margin
    ]

    if is_dem:
        accum = np.full((height, width), np.nan, dtype=np.float32)
    else:
        fill_rgb = (14, 36, 54) if "pr_" in str(tile_items[0][1]) or "2022" in str(tile_items[0][1]) else (0, 0, 0)
        accum = np.empty((height, width, 3), dtype=np.uint8)
        accum[:, :, :] = fill_rgb
        filled = np.zeros((height, width), dtype=bool)

    if not hits:
        return np.nan_to_num(accum, nan=nodata_fill) if is_dem else accum

    resamp = Resampling.bilinear
    for path in hits:
        def _read_one(p=path):
            src, owned = _get_cached_dataset(p, ds_cache)
            try:
                sb = src.bounds
                if src.crs is not None and src.crs.to_string() != crs:
                    stb = transform_bounds(src.crs, crs, sb.left, sb.bottom, sb.right, sb.top, densify_pts=8)
                else:
                    stb = (sb.left, sb.bottom, sb.right, sb.top)
                ix0, iy0 = max(wx0, stb[0]), max(wy0, stb[1])
                ix1, iy1 = min(wx1, stb[2]), min(wy1, stb[3])
                if ix1 <= ix0 or iy1 <= iy0:
                    return None
                c0 = max(0, int(math.floor((ix0 - wx0) / px_x)) - 4)
                c1 = min(width, int(math.ceil((ix1 - wx0) / px_x)) + 4)
                r0 = max(0, int(math.floor((wy1 - iy1) / px_y)) - 4)
                r1 = min(height, int(math.ceil((wy1 - iy0) / px_y)) + 4)
                if c1 <= c0 or r1 <= r0:
                    return None
                win = Window(c0, r0, c1 - c0, r1 - r0)
                if is_dem:
                    with WarpedVRT(
                        src,
                        crs=crs,
                        transform=dst_tf,
                        width=width,
                        height=height,
                        src_nodata=src.nodata,
                        nodata=np.nan,
                        resampling=resamp,
                        overview_level=None,
                        warp_mem_limit=128,
                    ) as vrt:
                        arr = vrt.read(1, window=win).astype(np.float32, copy=False)
                        return ((r0, r1, c0, c1), arr)
                else:
                    bands = [1, 2, 3] if src.count >= 3 else [1, 1, 1]
                    with WarpedVRT(
                        src,
                        crs=crs,
                        transform=dst_tf,
                        width=width,
                        height=height,
                        resampling=resamp,
                        warp_mem_limit=128,
                    ) as vrt:
                        arr = vrt.read(bands, window=win).transpose(1, 2, 0).astype(np.uint8, copy=False)
                        mask = vrt.dataset_mask(window=win) > 0
                        return ((r0, r1, c0, c1), arr, mask)
            except Exception:
                if ds_cache is not None and p in ds_cache:
                    try:
                        ds_cache.pop(p).close()
                    except Exception:
                        pass
                raise
            finally:
                if owned:
                    src.close()

        res = retry_call(_read_one, f"warp {os.path.basename(path)[:40]}")
        if res is None:
            continue
        r0, r1, c0, c1 = res[0]
        if is_dem:
            arr = res[1]
            valid = np.isfinite(arr) & (arr > -500.0) & (arr < 9000.0)
            sub_acc = accum[r0:r1, c0:c1]
            need = (~np.isfinite(sub_acc)) & valid
            sub_acc[need] = arr[need]
        else:
            arr, m = res[1], res[2]
            sub_acc = accum[r0:r1, c0:c1]
            sub_fil = filled[r0:r1, c0:c1]
            need = (~sub_fil) & m
            sub_acc[need] = arr[need]
            sub_fil |= need
            if filled.all():
                break

    if is_dem:
        if not np.isfinite(accum).any():
            return np.full((height, width), nodata_fill, dtype=np.float32)
        for _ in range(4):
            bad = ~np.isfinite(accum)
            if not bad.any():
                break
            nxt = accum.copy()
            if bad[1:, :].any():
                m = bad[1:, :] & np.isfinite(accum[:-1, :])
                nxt[1:, :][m] = accum[:-1, :][m]
            bad = ~np.isfinite(nxt)
            if bad[:-1, :].any():
                m = bad[:-1, :] & np.isfinite(accum[1:, :])
                nxt[:-1, :][m] = accum[1:, :][m]
            bad = ~np.isfinite(nxt)
            if bad[:, 1:].any():
                m = bad[:, 1:] & np.isfinite(accum[:, :-1])
                nxt[:, 1:][m] = accum[:, :-1][m]
            bad = ~np.isfinite(nxt)
            if bad[:, :-1].any():
                m = bad[:, :-1] & np.isfinite(accum[:, 1:])
                nxt[:, :-1][m] = accum[:, 1:][m]
            accum = nxt
        if not np.isfinite(accum).all():
            # Keep offshore/open ocean smoothly at nodata_fill (0.0m)
            accum = np.nan_to_num(accum, nan=nodata_fill)
        return accum

    if filled.any() and not filled.all():
        for _ in range(2):
            bad = ~filled
            if not bad.any():
                break
            nxt_img = accum.copy()
            nxt_f = filled.copy()
            for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                src_y0 = max(0, -dy)
                src_y1 = height - max(0, dy)
                dst_y0 = max(0, dy)
                dst_y1 = height - max(0, -dy)
                src_x0 = max(0, -dx)
                src_x1 = width - max(0, dx)
                dst_x0 = max(0, dx)
                dst_x1 = width - max(0, -dx)
                m = (~nxt_f[dst_y0:dst_y1, dst_x0:dst_x1]) & filled[src_y0:src_y1, src_x0:src_x1]
                if m.any():
                    nxt_img[dst_y0:dst_y1, dst_x0:dst_x1][m] = accum[src_y0:src_y1, src_x0:src_x1][m]
                    nxt_f[dst_y0:dst_y1, dst_x0:dst_x1][m] = True
            accum = nxt_img
            filled = nxt_f
    return accum


def make_vsicurl_env() -> rasterio.Env:
    return rasterio.Env(
        GDAL_HTTP_MAX_RETRY="5",
        GDAL_HTTP_RETRY_DELAY="2",
        CPL_VSIL_CURL_CHUNK_SIZE="262144",
        CPL_VSIL_CURL_ALLOWED_EXTENSIONS=".tif,.TIF,.jp2,.JP2,.vrt",
        VSI_CACHE="TRUE",
        VSI_CACHE_SIZE="268435456",
        GDAL_CACHEMAX=384,
        GDAL_NUM_THREADS="ALL_CPUS",
        GDAL_DISABLE_READDIR_ON_OPEN="EMPTY_DIR",
    )


def _build_local_vrt(vrt_path: Path, paths: Sequence[Path]) -> Path:
    if vrt_path.exists():
        return vrt_path
    list_path = vrt_path.with_suffix(".txt")
    list_path.write_text("\n".join(str(p) for p in paths) + "\n")
    subprocess.run(["gdalbuildvrt", "-q", "-input_file_list", str(list_path), str(vrt_path)], check=True)
    return vrt_path


def _stage_remote_tif(
    vpath: str,
    dst_path: Path,
    ovr_level: int | None,
    projwin: tuple[float, float, float, float] | None,
    is_dem: bool,
    add_ovr: bool = False,
) -> Path:
    """Downloads/clips a single COG or overview slice once to a local tiled GeoTIFF cleanly."""
    if dst_path.exists() and dst_path.stat().st_size > 16384:
        return dst_path
    dst_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = dst_path.with_suffix(".tif.tmp")

    attempt = 0

    def _run() -> Path:
        nonlocal attempt
        if tmp_path.exists():
            tmp_path.unlink()
        eff_vpath = _refresh_pc_naip_url(vpath, force=(attempt > 0))
        attempt += 1
        cmd = [
            "gdal_translate", "-q",
            "--config", "GDAL_DISABLE_READDIR_ON_OPEN", "EMPTY_DIR",
            "--config", "CPL_VSIL_CURL_CHUNK_SIZE", "524288",
            "--config", "GDAL_HTTP_MAX_RETRY", "4",
            "--config", "GDAL_HTTP_RETRY_DELAY", "2",
            "--config", "GDAL_HTTP_TIMEOUT", "30",
            "--config", "GDAL_HTTP_LOW_SPEED_LIMIT", "2048",
            "--config", "GDAL_HTTP_LOW_SPEED_TIME", "20",
        ]
        if ovr_level is not None:
            cmd.extend(["-oo", f"OVERVIEW_LEVEL={ovr_level}"])
        if not is_dem:
            cmd.extend(["-b", "1", "-b", "2", "-b", "3"])
        if projwin is not None:
            x0, y0, x1, y1 = projwin
            cmd.extend(["-projwin", f"{x0:.3f}", f"{y1:.3f}", f"{x1:.3f}", f"{y0:.3f}"])
        if is_dem:
            cmd.extend(["-of", "GTiff", "-co", "TILED=YES", "-co", "BLOCKXSIZE=256", "-co", "BLOCKYSIZE=256", "-co", "COMPRESS=LZW"])
        else:
            cmd.extend([
                "-of", "GTiff", "-co", "TILED=YES", "-co", "BLOCKXSIZE=256", "-co", "BLOCKYSIZE=256",
                "-co", "COMPRESS=JPEG", "-co", "JPEG_QUALITY=92", "-co", "PHOTOMETRIC=YCBCR",
            ])
        cmd.extend([eff_vpath, str(tmp_path)])
        subprocess.run(cmd, check=True, timeout=900)
        if add_ovr:
            subprocess.run(["gdaladdo", "-q", "-r", "average", str(tmp_path), "2", "4", "8"], check=True, timeout=600)
        os.replace(tmp_path, dst_path)
        return dst_path

    return retry_call(_run, f"stage {dst_path.name}")


def prepare_alptal_sources(cache_dir: Path) -> tuple[list, list, list, list]:
    """Discovers all 400 swissALTI3D & SWISSIMAGE-DOP10 tiles and stages local 1m/0.4m + core 0.5m/0.1m GeoTIFFs."""
    t2056 = Transformer.from_crs("EPSG:2056", "EPSG:4326", always_xy=True)
    x0, y0 = t2056.transform(2687000.0, 1202000.0)
    x1, y1 = t2056.transform(2707000.0, 1222000.0)
    bbox = f"{x0:.5f},{y0:.5f},{x1:.5f},{y1:.5f}"

    def _stac(col: str) -> dict[tuple[int, int], tuple[int, str]]:
        url = f"https://data.geo.admin.ch/api/stac/v0.9/collections/{col}/items"
        params = {"bbox": bbox, "limit": 100}
        best: dict[tuple[int, int], tuple[int, str]] = {}
        while url:
            r = retry_call(lambda u=url, p=params: requests.get(u, params=p, timeout=25).json(), col)
            params = None
            for f in r.get("features", []):
                parts = f["id"].split("_")
                yr = int(parts[1])
                en = parts[2].split("-")
                ekm, nkm = int(en[0]), int(en[1])
                if 2687 <= ekm < 2707 and 1202 <= nkm < 1222:
                    href = next((v["href"] for k, v in f["assets"].items() if k.endswith(".tif")), None)
                    if href and ((ekm, nkm) not in best or yr > best[(ekm, nkm)][0]):
                        best[(ekm, nkm)] = (yr, href)
            url = next((l["href"] for l in r.get("links", []) if l.get("rel") == "next"), None)
        return best

    alt = _stac("ch.swisstopo.swissalti3d")
    dop = _stac("ch.swisstopo.swissimage-dop10")
    print(f"[alptal] Discovered STAC tiles: swissALTI3D={len(alt)}/400, swissIMAGE-DOP10={len(dop)}/400", flush=True)

    core_tasks = []
    outer_dem_tasks = []
    for (e, n), (_, href) in sorted(alt.items()):
        if 2694 <= e < 2700 and 1209 <= n < 1215:
            core_tasks.append((e, n, href, cache_dir / "alti_core" / f"{e}_{n}.tif"))
        else:
            outer_dem_tasks.append((e, n, "/vsicurl/" + href, cache_dir / "alti_1m" / f"{e}_{n}.tif"))

    dop_core_tasks = []
    outer_dop_tasks = []
    for (e, n), (_, href) in sorted(dop.items()):
        if 2695 <= e < 2699 and 1210 <= n < 1214:
            dop_core_tasks.append((e, n, href, cache_dir / "dop_core" / f"{e}_{n}.tif"))
        else:
            outer_dop_tasks.append((e, n, "/vsicurl/" + href, cache_dir / "dop_04m" / f"{e}_{n}.tif"))

    print(
        f"[alptal] Staging {len(core_tasks)} full-res 0.50m ALTI + {len(dop_core_tasks)} full-res 0.10m DOP10 core "
        f"+ {len(outer_dem_tasks)} outer 1.0m ALTI + {len(outer_dop_tasks)} outer 0.40m DOP10 tiles locally...",
        flush=True,
    )
    t_dl = time.time()
    with ThreadPoolExecutor(max_workers=6) as ex:
        futs = [ex.submit(download_file_cached, href, path) for (_, _, href, path) in core_tasks + dop_core_tasks]
        futs += [ex.submit(_stage_remote_tif, vp, path, 0, None, True, False) for (_, _, vp, path) in outer_dem_tasks]
        futs += [ex.submit(_stage_remote_tif, vp, path, 1, None, False, False) for (_, _, vp, path) in outer_dop_tasks]
        done = 0
        for fut in as_completed(futs):
            fut.result()
            done += 1
            if done % 60 == 0 or done == len(futs):
                print(f"  [alptal] staged {done}/{len(futs)} files ({time.time()-t_dl:.0f}s elapsed)", flush=True)

    def _box(e: int, n: int) -> tuple[float, float, float, float]:
        return (e * 1000.0, n * 1000.0, (e + 1) * 1000.0, (n + 1) * 1000.0)

    dem_core = [(_box(e, n), str(p)) for (e, n, _, p) in core_tasks]
    rgb_core = [(_box(e, n), str(p)) for (e, n, _, p) in dop_core_tasks]
    dem_all = dem_core + [(_box(e, n), str(p)) for (e, n, _, p) in outer_dem_tasks]
    rgb_all = rgb_core + [(_box(e, n), str(p)) for (e, n, _, p) in outer_dop_tasks]
    return dem_all, dem_core, rgb_all, rgb_core


_PC_SAS_LOCK = threading.Lock()
_PC_SAS_STATE = {"token": "", "ts": 0.0}


def get_pc_naip_token(force_refresh: bool = False) -> str:
    with _PC_SAS_LOCK:
        now = time.time()
        if not force_refresh and _PC_SAS_STATE["token"] and (now - _PC_SAS_STATE["ts"]) < 120.0:
            return _PC_SAS_STATE["token"]
        r = retry_call(lambda: requests.get("https://planetarycomputer.microsoft.com/api/sas/v1/token/naip", timeout=15).json(), "PC SAS token")
        _PC_SAS_STATE["token"] = r["token"]
        _PC_SAS_STATE["ts"] = time.time()
        return _PC_SAS_STATE["token"]


def _refresh_pc_naip_url(vpath: str, force: bool = False) -> str:
    if "blob.core.windows.net/naip/" not in vpath:
        return vpath
    base = vpath.split("?", 1)[0]
    return f"{base}?{get_pc_naip_token(force_refresh=force)}"


def _clip_bounds_to_box(tb: tuple[float, float, float, float], box: tuple[float, float, float, float], pad: float = 16.0):
    x0 = max(tb[0], box[0] - pad)
    y0 = max(tb[1], box[1] - pad)
    x1 = min(tb[2], box[2] + pad)
    y1 = min(tb[3], box[3] + pad)
    if x1 - x0 < 5.0 or y1 - y0 < 5.0:
        return None
    return (x0, y0, x1, y1)


def prepare_luquillo_sources(cache_dir: Path) -> tuple[list, list, list]:
    box = (195000.0, 2016000.0, 215000.0, 2036000.0)
    core_box = (202500.0, 2023500.0, 207500.0, 2028500.0)
    t20 = Transformer.from_crs("EPSG:26920", "EPSG:4326", always_xy=True)
    lx0, ly0 = t20.transform(box[0], box[1])
    lx1, ly1 = t20.transform(box[2], box[3])
    tnm = retry_call(
        lambda: requests.get(
            "https://tnmaccess.nationalmap.gov/api/v1/products",
            params={"bbox": f"{lx0},{ly0},{lx1},{ly1}", "datasets": "Digital Elevation Model (DEM) 1 meter", "max": 200},
            timeout=25,
        ).json(),
        "USGS PR TNM",
    )
    d24_urls = [it["downloadURL"] for it in tnm["items"] if "D24" in it["title"] and " 20 " in it["title"]]

    naip = retry_call(
        lambda: requests.post(
            "https://planetarycomputer.microsoft.com/api/stac/v1/search",
            json={"collections": ["naip"], "bbox": [lx0, ly0, lx1, ly1], "limit": 100},
            timeout=20,
        ).json(),
        "PC NAIP PR",
    )["features"]
    tok = get_pc_naip_token()

    dem_jobs = []
    rgb_06_jobs = []
    rgb_core_jobs = []
    with make_vsicurl_env():
        for i, u in enumerate(d24_urls):
            vp = "/vsicurl/" + u
            with rasterio.open(vp) as s:
                tb = (s.bounds.left, s.bounds.bottom, s.bounds.right, s.bounds.top)
                cw = _clip_bounds_to_box(tb, box)
                if cw:
                    dem_jobs.append((vp, cw, cache_dir / "dem_local" / f"dem_{i:02d}.tif"))
        for i, f in enumerate(naip):
            vp = "/vsicurl/" + f["assets"]["image"]["href"] + "?" + tok
            with rasterio.open(vp) as s:
                tb = (s.bounds.left, s.bounds.bottom, s.bounds.right, s.bounds.top)
                ovr = 0 if len(s.overviews(1)) > 0 else None
                cw = _clip_bounds_to_box(tb, box)
                if cw:
                    rgb_06_jobs.append((vp, cw, ovr, cache_dir / "rgb_06m" / f"naip_{i:02d}.tif"))
                cw_core = _clip_bounds_to_box(tb, core_box, pad=32.0)
                if cw_core:
                    rgb_core_jobs.append((vp, cw_core, None, cache_dir / "rgb_core" / f"core_{i:02d}.tif"))

    print(
        f"[luquillo] Staging {len(dem_jobs)} DEMs (1m) + {len(rgb_06_jobs)} NAIPs (0.60m ovr) "
        f"+ {len(rgb_core_jobs)} core NAIPs (0.30m native)...",
        flush=True,
    )
    t_dl = time.time()
    with ThreadPoolExecutor(max_workers=6) as ex:
        futs = (
            [(ex.submit(_stage_remote_tif, vp, p, None, cw, True, False), cw, p) for (vp, cw, p) in dem_jobs]
            + [(ex.submit(_stage_remote_tif, vp, p, ovr, cw, False, True), cw, p) for (vp, cw, ovr, p) in rgb_06_jobs]
            + [(ex.submit(_stage_remote_tif, vp, p, ovr, cw, False, False), cw, p) for (vp, cw, ovr, p) in rgb_core_jobs]
        )
        dem_items = []
        rgb_items = []
        rgb_core_items = []
        for idx, (fut, cw, p) in enumerate(futs, 1):
            fut.result()
            if idx <= len(dem_jobs):
                dem_items.append((cw, str(p)))
            elif idx <= len(dem_jobs) + len(rgb_06_jobs):
                rgb_items.append((cw, str(p)))
            else:
                rgb_core_items.append((cw, str(p)))
            if idx % 8 == 0 or idx == len(futs):
                print(f"  [luquillo] staged {idx}/{len(futs)} files ({time.time()-t_dl:.0f}s elapsed)", flush=True)
    return dem_items, rgb_items, rgb_core_items


def prepare_hja_sources(cache_dir: Path) -> tuple[list, list]:
    box = (554000.0, 4880000.0, 574000.0, 4900000.0)
    t10 = Transformer.from_crs("EPSG:26910", "EPSG:4326", always_xy=True)
    hx0, hy0 = t10.transform(box[0], box[1])
    hx1, hy1 = t10.transform(box[2], box[3])
    tnm = retry_call(
        lambda: requests.get(
            "https://tnmaccess.nationalmap.gov/api/v1/products",
            params={"bbox": f"{hx0},{hy0},{hx1},{hy1}", "datasets": "Digital Elevation Model (DEM) 1 meter", "max": 100},
            timeout=25,
        ).json(),
        "USGS HJA TNM",
    )
    naip = retry_call(
        lambda: requests.post(
            "https://planetarycomputer.microsoft.com/api/stac/v1/search",
            json={"collections": ["naip"], "bbox": [hx0, hy0, hx1, hy1], "limit": 100},
            timeout=20,
        ).json(),
        "PC NAIP HJA",
    )["features"]
    naip_24 = [
        f
        for f in naip
        if "2024" in str(f["properties"].get("naip:year", "")) or f["properties"].get("datetime", "").startswith("2024")
    ]
    naip_24.sort(key=lambda f: f["properties"].get("datetime", ""), reverse=True)
    tok = get_pc_naip_token()

    dem_jobs = []
    rgb_jobs = []
    with make_vsicurl_env():
        for i, it in enumerate(tnm["items"]):
            vp = "/vsicurl/" + it["downloadURL"]
            with rasterio.open(vp) as s:
                tb = (s.bounds.left, s.bounds.bottom, s.bounds.right, s.bounds.top)
                cw = _clip_bounds_to_box(tb, box)
                if cw:
                    dem_jobs.append((vp, cw, cache_dir / "dem_local" / f"dem_{i:02d}.tif"))
        for i, f in enumerate(naip_24):
            vp = "/vsicurl/" + f["assets"]["image"]["href"] + "?" + tok
            with rasterio.open(vp) as s:
                tb = (s.bounds.left, s.bounds.bottom, s.bounds.right, s.bounds.top)
                cw = _clip_bounds_to_box(tb, box)
                if cw:
                    rgb_jobs.append((vp, cw, cache_dir / "rgb_local" / f"naip_{i:02d}.tif"))

    print(f"[hj_andrews] Staging {len(dem_jobs)} clipped DEMs + {len(rgb_jobs)} clipped NAIP 2024 TIFs...", flush=True)
    t_dl = time.time()
    with ThreadPoolExecutor(max_workers=6) as ex:
        futs = (
            [(ex.submit(_stage_remote_tif, vp, p, None, cw, True, False), cw, p, True) for (vp, cw, p) in dem_jobs]
            + [(ex.submit(_stage_remote_tif, vp, p, None, cw, False, True), cw, p, False) for (vp, cw, p) in rgb_jobs]
        )
        dem_items = []
        rgb_items = []
        for idx, (fut, cw, p, is_d) in enumerate(futs, 1):
            fut.result()
            if is_d:
                dem_items.append((cw, str(p)))
            else:
                rgb_items.append((cw, str(p)))
            if idx % 6 == 0 or idx == len(futs):
                print(f"  [hj_andrews] staged {idx}/{len(futs)} files ({time.time()-t_dl:.0f}s elapsed)", flush=True)

    tnm_fb = retry_call(
        lambda: requests.get(
            "https://tnmaccess.nationalmap.gov/api/v1/products",
            params={"bbox": f"{hx0},{hy0},{hx1},{hy1}", "datasets": "National Elevation Dataset (NED) 1/3 arc-second", "max": 20},
            timeout=25,
        ).json(),
        "USGS HJA 3DEP 1/3",
    )
    fb_items = [it for it in tnm_fb.get("items", []) if it.get("downloadURL", "").endswith(".tif")]
    fb_items.sort(key=lambda it: it.get("title", ""), reverse=True)
    fb_path = cache_dir / "dem_local" / "dem_3dep_10m.tif"
    if fb_items and not (fb_path.exists() and fb_path.stat().st_size > 16384):
        fb_path.parent.mkdir(parents=True, exist_ok=True)
        fb_tmp = fb_path.with_suffix(".tif.tmp")
        if fb_tmp.exists():
            fb_tmp.unlink()
        subprocess.run([
            "gdalwarp", "-q", "-t_srs", "EPSG:26910",
            "-te", f"{box[0]-64.0:.1f}", f"{box[1]-64.0:.1f}", f"{box[2]+64.0:.1f}", f"{box[3]+64.0:.1f}",
            "-tr", "5", "5", "-r", "bilinear", "-of", "GTiff", "-co", "COMPRESS=LZW",
            "/vsicurl/" + fb_items[0]["downloadURL"], str(fb_tmp),
        ], check=True)
        os.replace(fb_tmp, fb_path)
    if fb_path.exists():
        dem_items.append((box, str(fb_path)))
    return dem_items, rgb_items


def prepare_wuestebach_sources(cache_dir: Path) -> tuple[list, list]:
    """Caches 400 NRW DGM1 tiles (~840MB) and SW-core 0.10m DOP10 JP2s (~950MB) for fast local reads."""
    dgm_base = "https://www.opengeodata.nrw.de/produkte/geobasis/hm/dgm1_tiff/dgm1_tiff/"
    dop_base = "https://www.opengeodata.nrw.de/produkte/geobasis/lusat/akt/dop/dop_jp2_f10/"
    dgm_idx = retry_call(lambda: requests.get(dgm_base + "index.json", timeout=25).json(), "NRW DGM1 idx")
    dop_idx = retry_call(lambda: requests.get(dop_base + "index.json", timeout=25).json(), "NRW DOP10 idx")

    dgm_map: dict[tuple[int, int], str] = {}
    for f in dgm_idx["datasets"][0]["files"]:
        p = f["name"].split("_")
        if len(p) >= 4 and p[1] == "32":
            ekm, nkm = int(p[2]), int(p[3])
            if 308 <= ekm < 328 and 5596 <= nkm < 5616:
                dgm_map[(ekm, nkm)] = f["name"]

    dop_map: dict[tuple[int, int], str] = {}
    for f in dop_idx["datasets"][0]["files"]:
        p = f["name"].split("_")
        if len(p) >= 4 and p[1] == "32":
            ekm, nkm = int(p[2]), int(p[3])
            if 308 <= ekm < 313 and 5596 <= nkm < 5601:
                dop_map[(ekm, nkm)] = f["name"]

    print(f"[wuestebach] Caching {len(dgm_map)} NRW DGM1 TIFs + {len(dop_map)} core 0.10m DOP10 JP2s...", flush=True)
    dem_jobs = [(ekm, nkm, dgm_base + fn, cache_dir / "dgm1" / fn) for (ekm, nkm), fn in sorted(dgm_map.items())]
    dop_jobs = [(ekm, nkm, dop_base + fn, cache_dir / "dop10" / fn) for (ekm, nkm), fn in sorted(dop_map.items())]

    with ThreadPoolExecutor(max_workers=6) as ex:
        futs = [ex.submit(download_file_cached, u, p) for (_, _, u, p) in dem_jobs + dop_jobs]
        done = 0
        for fut in as_completed(futs):
            fut.result()
            done += 1
            if done % 80 == 0 or done == len(futs):
                print(f"  [wuestebach] cached {done}/{len(futs)} files", flush=True)

    # Build fast unified VRT for DGM1 and transcode OpenJPEG JP2s -> 512x512 tiled GeoTIFFs + VRT once
    dgm_vrt = cache_dir / "dgm1.vrt"
    if not dgm_vrt.exists():
        dgm_list = cache_dir / "dgm1_list.txt"
        dgm_list.write_text("\n".join(str(p) for (_, _, _, p) in dem_jobs) + "\n")
        subprocess.run(["gdalbuildvrt", "-q", "-input_file_list", str(dgm_list), str(dgm_vrt)], check=True)

    dop_tif_dir = cache_dir / "dop10_tif"
    dop_tif_dir.mkdir(parents=True, exist_ok=True)

    def _jp2_to_tiled_tif(jp2_path: Path) -> Path:
        tif_path = dop_tif_dir / (jp2_path.stem + ".tif")
        if tif_path.exists() and tif_path.stat().st_size > 100000:
            return tif_path
        tmp_tif = tif_path.with_suffix(".tif.tmp")
        subprocess.run(
            [
                "gdal_translate", "-q",
                "-b", "1", "-b", "2", "-b", "3",
                "-of", "GTiff",
                "-co", "TILED=YES",
                "-co", "BLOCKXSIZE=512",
                "-co", "BLOCKYSIZE=512",
                "-co", "COMPRESS=JPEG",
                "-co", "JPEG_QUALITY=92",
                "-co", "PHOTOMETRIC=YCBCR",
                str(jp2_path), str(tmp_tif),
            ],
            check=True,
        )
        subprocess.run(["gdaladdo", "-q", "-r", "average", str(tmp_tif), "2", "4", "8", "16"], check=True)
        os.replace(tmp_tif, tif_path)
        return tif_path

    print(f"[wuestebach] Converting {len(dop_jobs)} JP2 tiles -> 512x512 tiled GeoTIFF + overviews...", flush=True)
    with ThreadPoolExecutor(max_workers=4) as ex:
        dop_tifs = list(ex.map(_jp2_to_tiled_tif, [p for (_, _, _, p) in dop_jobs]))

    dop_vrt = cache_dir / "dop_core.vrt"
    if not dop_vrt.exists():
        dop_list = cache_dir / "dop_list.txt"
        dop_list.write_text("\n".join(str(p) for p in dop_tifs) + "\n")
        subprocess.run(["gdalbuildvrt", "-q", "-input_file_list", str(dop_list), str(dop_vrt)], check=True)

    full_box = (308000.0, 5596000.0, 328000.0, 5616000.0)
    core_box = (308000.0, 5596000.0, 313000.0, 5601000.0)
    return [(full_box, str(dgm_vrt))], [(core_box, str(dop_vrt))]


def fetch_nrw_wms_rgb(bx0: float, by0: float, bx1: float, by1: float, side: int) -> np.ndarray:
    pad = 0.5 * (bx1 - bx0) / float(side - 1)
    url = (
        "https://www.wms.nrw.de/geobasis/wms_nw_dop?SERVICE=WMS&VERSION=1.3.0&REQUEST=GetMap"
        f"&LAYERS=nw_dop_rgb&STYLES=&CRS=EPSG:25832&BBOX={bx0 - pad:.4f},{by0 - pad:.4f},{bx1 + pad:.4f},{by1 + pad:.4f}"
        f"&WIDTH={side}&HEIGHT={side}&FORMAT=image/jpeg"
    )

    def _get():
        r = requests.get(url, timeout=35)
        r.raise_for_status()
        if "image" not in r.headers.get("Content-Type", ""):
            raise RuntimeError(f"Non-image WMS response: {r.text[:200]}")
        return np.array(Image.open(BytesIO(r.content)).convert("RGB"), dtype=np.uint8)

    return retry_call(_get, f"NRW WMS {side}x{side}")


def fetch_block_dem_rgb(
    spec: CatchmentSpec,
    bounds: tuple[float, float, float, float],
    side: int,
    sources: dict,
    use_core_dem: bool = False,
    fast_1m_dem: bool = False,
    ds_cache: OrderedDict | None = None,
) -> tuple[np.ndarray, np.ndarray]:
    bx0, by0, bx1, by1 = bounds
    crs = f"EPSG:{spec.epsg}"

    if spec.key == "wuestebach":
        dem = mosaic_warp_window(sources["dem"], crs, bounds, 2501, 2501, is_dem=True, ds_cache=ds_cache)
        dem = bilinear_upsample_2d(dem, side, side)
        # Use native 0.10m JP2 inside SW [308..313, 5596..5601], otherwise WMS
        if bx0 >= 307999.0 and by0 >= 5595999.0 and bx1 <= 313001.0 and by1 <= 5601001.0:
            rgb = mosaic_warp_window(sources["dop_core"], crs, bounds, side, side, is_dem=False, ds_cache=ds_cache)
        else:
            rgb = fetch_nrw_wms_rgb(bx0, by0, bx1, by1, side)
        return dem, rgb

    if spec.key == "alptal":
        if use_core_dem:
            dem = mosaic_warp_window(sources["dem_core"], crs, bounds, side, side, is_dem=True, ds_cache=ds_cache)
        elif fast_1m_dem:
            dem_src = sources["dem_core"] if (
                bx0 >= 2694000.0 and by0 >= 1209000.0 and bx1 <= 2700000.0 and by1 <= 1215000.0
            ) else sources["dem"]
            dem_1m = mosaic_warp_window(dem_src, crs, bounds, 2501, 2501, is_dem=True, ds_cache=ds_cache)
            dem = bilinear_upsample_2d(dem_1m, side, side)
        else:
            dem = mosaic_warp_window(sources["dem"], crs, bounds, side, side, is_dem=True, ds_cache=ds_cache)
        rgb_src = sources["rgb_core"] if (
            "rgb_core" in sources and bx0 >= 2695000.0 and by0 >= 1210000.0 and bx1 <= 2699000.0 and by1 <= 1214000.0
        ) else sources["rgb"]
        rgb = mosaic_warp_window(rgb_src, crs, bounds, side, side, is_dem=False, ds_cache=ds_cache)
        return dem, rgb

    # Luquillo or HJA (staged local GeoTIFFs)
    if side > 2501 and fast_1m_dem:
        dem_1m = mosaic_warp_window(sources["dem"], crs, bounds, 2501, 2501, is_dem=True, ds_cache=ds_cache)
        dem = bilinear_upsample_2d(dem_1m, side, side)
    else:
        dem = mosaic_warp_window(sources["dem"], crs, bounds, side, side, is_dem=True, ds_cache=ds_cache)
    rgb_src = sources["rgb_core"] if (
        spec.key == "luquillo" and not fast_1m_dem and "rgb_core" in sources
        and bx0 >= 202500.0 and by0 >= 2023500.0 and bx1 <= 207500.0 and by1 <= 2028500.0
    ) else sources["rgb"]
    rgb = mosaic_warp_window(rgb_src, crs, bounds, side, side, is_dem=False, ds_cache=ds_cache)
    return dem, rgb


def build_single_map(spec: CatchmentSpec, lib: ctypes.CDLL, clean_cache: bool) -> Path:
    MAPS_DIR.mkdir(parents=True, exist_ok=True)
    cache_dir = CACHE_ROOT / spec.key
    cache_dir.mkdir(parents=True, exist_ok=True)
    out_path = MAPS_DIR / spec.out_filename
    tmp_path = MAPS_DIR / (spec.out_filename + ".tmp")

    print(f"\n================ BUILDING {spec.key.upper()} -> {out_path.name} ================", flush=True)
    nodes, idx_map, max_lod = build_quadtree_topology(spec)
    total_nodes = len(nodes)
    disk_table_off = EPACK_V2_HEADER_BYTES
    tile_data_off = (disk_table_off + total_nodes * DISK_NODE_STRUCT.size + 4095) & ~4095
    print(f"[{spec.key}] Quadtree topology: {total_nodes} nodes (max_lod={max_lod}, data_off={tile_data_off})", flush=True)

    # Prepare data sources
    sources: dict = {}
    if spec.key == "alptal":
        d_all, d_core, r_all, r_core = prepare_alptal_sources(cache_dir)
        sources = {"dem": d_all, "dem_core": d_core, "rgb": r_all, "rgb_core": r_core}
    elif spec.key == "luquillo":
        d_all, r_all, r_core = prepare_luquillo_sources(cache_dir)
        sources = {"dem": d_all, "rgb": r_all, "rgb_core": r_core}
    elif spec.key == "hj_andrews":
        d_all, r_all = prepare_hja_sources(cache_dir)
        sources = {"dem": d_all, "rgb": r_all}
    elif spec.key == "wuestebach":
        d_all, d_dop = prepare_wuestebach_sources(cache_dir)
        sources = {"dem": d_all, "dop_core": d_dop}

    # Global overview grid for LOD 0..3: 8x8 LOD3 tiles stitched -> 2041x2041
    ov_dem = np.zeros((2041, 2041), dtype=np.float32)
    ov_rgb = np.zeros((2041, 2041, 3), dtype=np.uint8)

    ds_cache: OrderedDict = OrderedDict()
    try:
        with make_vsicurl_env():
            with open(tmp_path, "wb") as fout:
                fout.write(b"\x00" * tile_data_off)
                cur_off = tile_data_off

                def encode_and_write_grid(
                    lod: int,
                    gx0: int,
                    gy0: int,
                    block_k: int,
                    dem_arr: np.ndarray | None,
                    rgb_arr: np.ndarray,
                    rgb_only: int,
                ):
                    nonlocal cur_off
                    n_tiles = block_k * block_k
                    stride_px = rgb_arr.shape[1]
                    tile_bytes = EPACK_V2_RGB_BYTES if rgb_only else EPACK_V2_DEM_RGB_BYTES
                    payloads = np.empty(n_tiles * tile_bytes, dtype=np.uint8)
                    mins = np.empty(n_tiles, dtype=np.float32)
                    maxs = np.empty(n_tiles, dtype=np.float32)
                    means = np.empty(n_tiles, dtype=np.float32)

                    lib.epack_v2_encode_block_tiles(
                        dem_arr.ctypes.data if dem_arr is not None else 0,
                        rgb_arr.ctypes.data,
                        block_k,
                        stride_px,
                        rgb_only,
                        payloads.ctypes.data,
                        mins.ctypes.data,
                        maxs.ctypes.data,
                        means.ctypes.data,
                    )

                    for ty in range(block_k):
                        for tx in range(block_k):
                            node_id = idx_map.get((lod, gx0 + tx, gy0 + ty))
                            if node_id is None:
                                continue
                            ti = ty * block_k + tx
                            rec = nodes[node_id]
                            rec.ssd_offset = cur_off
                            if not rgb_only:
                                rec.elev_min = float(mins[ti])
                                rec.elev_max = float(maxs[ti])
                                rec.elev_mean = float(means[ti])
                            else:
                                # Propagate DEM bounds from parent when RGB-only
                                p_id = idx_map[(lod - 1, (gx0 + tx) >> 1, (gy0 + ty) >> 1)]
                                rec.elev_min = nodes[p_id].elev_min
                                rec.elev_max = nodes[p_id].elev_max
                                rec.elev_mean = nodes[p_id].elev_mean
                            fout.write(payloads[ti * tile_bytes : (ti + 1) * tile_bytes].tobytes())
                            cur_off += tile_bytes

                # 1. Stream all 64 LOD 3 blocks (2500 x 2500 m) -> write LOD 7..3 + populate 2041x2041 overview
                t_start = time.time()
                block_list = [(bx, by) for by in range(8) for bx in range(8)]

                for (bx, by) in block_list:
                    wx0 = -WORLD_HALF_M + bx * 2500.0
                    wy0 = -WORLD_HALF_M + by * 2500.0
                    bounds = (spec.center_e + wx0, spec.center_n + wy0, spec.center_e + wx0 + 2500.0, spec.center_n + wy0 + 2500.0)
                    use_core = False
                    d7, r7 = fetch_block_dem_rgb(
                        spec, bounds, 4081, sources, use_core_dem=use_core, fast_1m_dem=True, ds_cache=ds_cache
                    )
                    d7 = np.ascontiguousarray(d7, dtype=np.float32)
                    r7 = np.ascontiguousarray(r7, dtype=np.uint8)
                    encode_and_write_grid(7, bx * 16, by * 16, 16, d7, r7, 0)

                    cur_d, cur_r = d7, r7
                    for step_lod, k in [(6, 8), (5, 4), (4, 2), (3, 1)]:
                        ns = k * 255 + 1
                        nxt_d = np.empty((ns, ns), dtype=np.float32)
                        nxt_r = np.empty((ns, ns, 3), dtype=np.uint8)
                        lib.epack_v2_downsample_2x_north_up(
                            cur_d.ctypes.data, cur_r.ctypes.data, cur_d.shape[0],
                            nxt_d.ctypes.data, nxt_r.ctypes.data, ns
                        )
                        encode_and_write_grid(step_lod, bx * k, by * k, k, nxt_d, nxt_r, 0)
                        cur_d, cur_r = nxt_d, nxt_r

                    # Stitch 256x256 LOD3 block into 2041x2041 North-up overview
                    oy0 = (7 - by) * 255
                    ox0 = bx * 255
                    ov_dem[oy0 : oy0 + 256, ox0 : ox0 + 256] = cur_d
                    ov_rgb[oy0 : oy0 + 256, ox0 : ox0 + 256] = cur_r

                    done = by * 8 + bx + 1
                    dt = time.time() - t_start
                    eta = (dt / done) * (64 - done)
                    if done % 4 == 0 or done == 64:
                        print(f"  [{spec.key}] LOD 3..7 block {done:2d}/64 ({dt:.0f}s elapsed, ~{eta:.0f}s ETA)", flush=True)

                # 2. Encode LOD 2, 1, 0 from stitched 2041x2041 overview
                cur_d, cur_r = ov_dem, ov_rgb
                for step_lod, k in [(2, 4), (1, 2), (0, 1)]:
                    ns = k * 255 + 1
                    nxt_d = np.empty((ns, ns), dtype=np.float32)
                    nxt_r = np.empty((ns, ns, 3), dtype=np.uint8)
                    lib.epack_v2_downsample_2x_north_up(
                        cur_d.ctypes.data, cur_r.ctypes.data, cur_d.shape[0],
                        nxt_d.ctypes.data, nxt_r.ctypes.data, ns
                    )
                    encode_and_write_grid(step_lod, 0, 0, k, nxt_d, nxt_r, 0)
                    cur_d, cur_r = nxt_d, nxt_r

                # 3. Encode sparse focal ROIs for LOD 8, 9, 10 grouped cleanly by 625m (LOD5) or 312.5m (LOD6) parents
                for rspec in spec.rois:
                    target_lod = rspec.lod
                    if target_lod == 8:
                        chunk_lod = 5  # 625m chunk -> k=8 tiles (2041px @ 0.305m)
                    elif target_lod == 9:
                        chunk_lod = 5  # 625m chunk -> k=16 tiles (4081px @ 0.153m)
                    elif target_lod == 10:
                        chunk_lod = 6  # 312.5m chunk -> k=16 tiles (4081px @ 0.076m)
                    else:
                        chunk_lod = target_lod - 3
                    k = 1 << (target_lod - chunk_lod)
                    side = k * 255 + 1
                    span = (2.0 * WORLD_HALF_M) / float(1 << chunk_lod)
                    n_chunks_per_axis = 1 << chunk_lod

                    chunk_coords = []
                    for cy in range(n_chunks_per_axis):
                        for cx in range(n_chunks_per_axis):
                            gx0 = cx * k
                            gy0 = cy * k
                            has_any = any((target_lod, gx0 + dx, gy0 + dy) in idx_map for dy in range(k) for dx in range(k))
                            if has_any:
                                chunk_coords.append((cx, cy))

                    print(f"  [{spec.key}] Encoding LOD {target_lod} ({len(chunk_coords)} chunks of {k}x{k} tiles, {side}x{side}px, has_dem={rspec.has_dem})...", flush=True)
                    t_roi = time.time()
                    for ci, (cx, cy) in enumerate(chunk_coords, 1):
                        wx0 = -WORLD_HALF_M + cx * span
                        wy0 = -WORLD_HALF_M + cy * span
                        bounds = (spec.center_e + wx0, spec.center_n + wy0, spec.center_e + wx0 + span, spec.center_n + wy0 + span)
                        d_sub, r_sub = fetch_block_dem_rgb(
                            spec, bounds, side, sources, use_core_dem=rspec.has_dem, fast_1m_dem=not rspec.has_dem, ds_cache=ds_cache
                        )
                        d_sub = np.ascontiguousarray(d_sub, dtype=np.float32) if rspec.has_dem else None
                        r_sub = np.ascontiguousarray(r_sub, dtype=np.uint8)
                        encode_and_write_grid(target_lod, cx * k, cy * k, k, d_sub, r_sub, 0 if rspec.has_dem else 1)
                        if ci % 16 == 0 or ci == len(chunk_coords):
                            dt_roi = time.time() - t_roi
                            print(f"    [{spec.key}] LOD {target_lod}: {ci}/{len(chunk_coords)} chunks ({dt_roi:.0f}s elapsed)", flush=True)

                # 4. Write complete EpackDiskNode[total_nodes] table @ disk_table_off
                fout.seek(disk_table_off)
                for rec in nodes:
                    assert rec.ssd_offset != 0, f"Unpopulated disk node {rec.disk_idx} lod={rec.lod}"
                    tk = EPACK_V2_TILE_DEM_RGB if rec.has_dem else EPACK_V2_TILE_RGB
                    packed = DISK_NODE_STRUCT.pack(
                        rec.ssd_offset,
                        rec.first_child_idx,
                        rec.gx,
                        rec.gy,
                        rec.elev_min,
                        rec.elev_max,
                        rec.elev_mean,
                        rec.lod,
                        tk,
                        1 if rec.has_dem else 0,
                        1 if rec.has_rgb else 0,
                    )
                    fout.write(packed)
                fout.flush()
    finally:
        close_ds_cache(ds_cache)

    # 5. Patch authoritative 24,576-byte C header
    tr_wgs = Transformer.from_crs(f"EPSG:{spec.epsg}", "EPSG:4326", always_xy=True)
    lon_min, lat_min = tr_wgs.transform(spec.center_e - WORLD_HALF_M, spec.center_n - WORLD_HALF_M)
    lon_max, lat_max = tr_wgs.transform(spec.center_e + WORLD_HALF_M, spec.center_n + WORLD_HALF_M)

    root = nodes[0]
    c_meta = CEpackV2BuildMeta(
        lat_min_deg=lat_min,
        lat_max_deg=lat_max,
        lon_min_deg=lon_min,
        lon_max_deg=lon_max,
        world_half_extent=float(WORLD_HALF_M),
        elev_min_m=float(root.elev_min),
        elev_max_m=float(root.elev_max),
        center_east_m=float(spec.center_e),
        center_north_m=float(spec.center_n),
        total_node_count=total_nodes,
        max_lod=max_lod,
        epsg_code=spec.epsg,
        dem_res_mm=spec.dem_res_mm,
        ortho_res_mm=spec.ortho_res_mm,
        disk_table_offset=disk_table_off,
        tile_data_offset=tile_data_off,
    )
    rc = lib.epack_v2_write_header(str(tmp_path).encode(), ctypes.byref(c_meta))
    if rc != 0:
        raise RuntimeError(f"epack_v2_write_header failed for {tmp_path}")

    os.replace(tmp_path, out_path)
    verify_epack_v2(out_path)
    if clean_cache and cache_dir.exists():
        shutil.rmtree(cache_dir, ignore_errors=True)
        print(f"[{spec.key}] Cleaned download cache {cache_dir}", flush=True)
    return out_path


def verify_epack_v2(path: Path) -> None:
    sz = path.stat().st_size
    with open(path, "rb") as f:
        hdr = f.read(EPACK_V2_HEADER_BYTES)
        magic, version, node_cnt, tile_dim = struct.unpack_from("<IIII", hdr, 0)
        world_half, emin, emax, vscale = struct.unpack_from("<ffff", hdr, 16)
        v2_magic, total_nodes, max_lod, flags, disk_off, data_off, epsg, ce, cn, dres, ores = struct.unpack_from(
            "<IIIIQQIffII", hdr, 23232
        )
        assert magic == EPACK_V2_MAGIC and version == 2, f"Bad magic/ver: {hex(magic)} v{version}"
        assert v2_magic == EPACK_V2_MAGIC and total_nodes >= 21845, f"Bad v2 meta nodes={total_nodes}"
        f.seek(disk_off)
        raw_nodes = f.read(total_nodes * DISK_NODE_STRUCT.size)
        zero_off = 0
        dem_nodes = 0
        rgb_only = 0
        lod_counts: dict[int, int] = {}
        for i in range(total_nodes):
            off, fc, gx, gy, nmin, nmax, nmean, lod, tk, hdem, hrgb = DISK_NODE_STRUCT.unpack_from(raw_nodes, i * 32)
            if off == 0:
                zero_off += 1
            if hdem:
                dem_nodes += 1
            else:
                rgb_only += 1
            lod_counts[lod] = lod_counts.get(lod, 0) + 1
            expect_sz = EPACK_V2_DEM_RGB_BYTES if hdem else EPACK_V2_RGB_BYTES
            assert off + expect_sz <= sz, f"Node {i} exceeds file size"
        assert zero_off == 0, f"Found {zero_off} zero-offset nodes!"
    print(
        f"[verify OK] {path.name}: {sz / (1024**3):.2f} GiB | EPSG:{epsg} center=({ce:.0f},{cn:.0f}) "
        f"| elev=[{emin:.1f}..{emax:.1f}] m | nodes={total_nodes} (DEM+RGB={dem_nodes}, RGB={rgb_only}) "
        f"| LODs={dict(sorted(lod_counts.items()))}",
        flush=True,
    )


def fix_epack_v2_rim(path: Path, lib: ctypes.CDLL) -> None:
    """Clamps 0.0m 1px WarpedVRT edge overhangs on inland .epack v2 archives in-place."""
    buf_tile = np.empty(EPACK_V2_DEM_RGB_BYTES, dtype=np.uint8)
    tmin = ctypes.c_float()
    tmax = ctypes.c_float()
    tmean = ctypes.c_float()
    fixed_tiles = 0

    with open(path, "r+b") as f:
        hdr = bytearray(f.read(EPACK_V2_HEADER_BYTES))
        _, total_nodes, _, _, disk_off, _, _, _, _, _, _ = struct.unpack_from("<IIIIQQIffII", hdr, 23232)
        f.seek(disk_off)
        raw = bytearray(f.read(total_nodes * DISK_NODE_STRUCT.size))
        nodes_meta = [list(DISK_NODE_STRUCT.unpack_from(raw, i * 32)) for i in range(total_nodes)]

        for i, m in enumerate(nodes_meta):
            off, fc, gx, gy, nmin, nmax, nmean, lod, tk, hdem, hrgb = m
            if not hdem or nmin >= 1.0:
                continue
            f.seek(off)
            raw_tile = f.read(EPACK_V2_DEM_RGB_BYTES)
            buf_tile[:] = np.frombuffer(raw_tile, dtype=np.uint8)
            rc = lib.epack_v2_fix_tile_rim(
                buf_tile.ctypes.data, ctypes.byref(tmin), ctypes.byref(tmax), ctypes.byref(tmean)
            )
            if rc > 0:
                f.seek(off)
                f.write(buf_tile.tobytes())
                m[4] = float(tmin.value)
                m[5] = float(tmax.value)
                m[6] = float(tmean.value)
                DISK_NODE_STRUCT.pack_into(raw, i * 32, *m)
                fixed_tiles += 1

        # Propagate updated min/max/mean to RGB-only children
        for i, m in enumerate(nodes_meta):
            off, fc, gx, gy, nmin, nmax, nmean, lod, tk, hdem, hrgb = m
            if not hdem and nmin < 1.0:
                p_off_min = nodes_meta[0][4]
                for j, pm in enumerate(nodes_meta):
                    if pm[7] == lod - 1 and pm[2] == (gx >> 1) and pm[3] == (gy >> 1):
                        m[4], m[5], m[6] = pm[4], pm[5], pm[6]
                        DISK_NODE_STRUCT.pack_into(raw, i * 32, *m)
                        break

        f.seek(disk_off)
        f.write(raw)
        # Update root elev_min_m / elev_max_m in GpuMathPageDirectory (offset 20, 24)
        root_min = nodes_meta[0][4]
        root_max = nodes_meta[0][5]
        struct.pack_into("<ff", hdr, 20, root_min, root_max)
        f.seek(0)
        f.write(hdr)
        f.flush()
    print(f"[fix-rim OK] {path.name}: repaired {fixed_tiles} DEM rim tiles -> elev=[{root_min:.1f}..{root_max:.1f}] m", flush=True)
    verify_epack_v2(path)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", choices=["alptal", "luquillo", "hj_andrews", "wuestebach", "all"], default="all")
    ap.add_argument("--verify", type=Path, help="Verify an existing .epack v2 file and exit")
    ap.add_argument("--fix-rim", type=Path, help="Repair 0.0m WarpedVRT perimeter overhang tiles in an inland .epack v2 file")
    ap.add_argument("--keep-cache", action="store_true", help="Do not delete build/hydro_cache/<map> after finish")
    args = ap.parse_args()

    if args.verify:
        verify_epack_v2(args.verify)
        return 0

    lib = load_encoder_lib()
    if args.fix_rim:
        fix_epack_v2_rim(args.fix_rim, lib)
        return 0

    keys = ["wuestebach", "hj_andrews", "luquillo", "alptal"] if args.map == "all" else [args.map]
    for k in keys:
        build_single_map(CATCHMENTS[k], lib, clean_cache=not args.keep_cache)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
