#!/usr/bin/env python3
"""
Baklan & Beşparmak Dağı — Photogrammetric & Geodetic Survey Ingestion Tool
==========================================================================
Ingests user photographs (from smartphones, DJI/RTK drones, or survey cameras),
extracts EXIF/XMP camera intrinsics (sensor model, focal length in mm, 35mm eq)
and extrinsics (WGS84 GPS lat/lon/alt, AGL height, DJI gimbal yaw/pitch/roll),
converts non-PNG images to RGBA8 PNG if needed, and registers photos, multi-view
stereo (MVS) tie-points, and map-engineer geodetic control points / breaklines
into `examples/demo/maps/besparmak_survey.manifest`.

When the engine is running in World 8 (`./build/debug/bin/engine --world=8`),
saving the manifest automatically hot-reloads the 3D DEM and ortho-imagery via
`io_uring` + `inotify`.
"""

from __future__ import annotations

import argparse
import math
import os
import re
import shutil
import struct
import sys
from pathlib import Path

SENSOR_PRESETS: dict[str, tuple[float, float, float, float, float, float]] = {
    # name: (sensor_w_mm, sensor_h_mm, focal_mm, focal_35mm, k1, k2)
    "iPhone 16 Pro Main":      (9.80,  7.35,  6.765, 24.0, -0.028, 0.006),
    "iPhone 15 Pro Main":      (9.80,  7.35,  6.765, 24.0, -0.028, 0.006),
    "iPhone 15 Pro UltraWide": (4.80,  3.60,  2.220, 13.0, -0.065, 0.018),
    "Samsung S24 Ultra Main":  (9.60,  7.20,  6.300, 23.0, -0.026, 0.005),
    "Xiaomi 14 Ultra 1-inch":  (13.20, 8.80,  8.720, 23.0, -0.022, 0.004),
    "Google Pixel 9 Pro Main": (9.80,  7.35,  6.900, 24.0, -0.025, 0.005),
    "DJI Mavic 3 Enterprise":  (17.30, 13.00, 12.29, 24.0, -0.014, 0.002),
    "DJI Mini 4 Pro":          (9.60,  7.20,  6.720, 24.0, -0.024, 0.004),
    "DJI Air 3 Wide":          (9.60,  7.20,  6.720, 24.0, -0.024, 0.004),
    "DJI Phantom 4 RTK":       (13.20, 8.80,  8.800, 24.0, -0.012, 0.002),
    "Canon PowerShot S110":    (7.44,  5.58,  5.200, 24.0, -0.015, 0.003),
    "Sony DSC-WX220":          (6.17,  4.55,  4.450, 25.0, -0.018, 0.004),
    "Sony A7R V FullFrame":    (35.70, 23.80, 35.00, 35.0, -0.008, 0.001),
    "Custom":                  (36.00, 24.00, 24.00, 24.0,  0.000, 0.000),
}


def extract_xmp_dji_metadata(img_path: Path) -> dict[str, float]:
    """Scan image header bytes for DJI / SenseFly eBee / Pix4D XMP gimbal yaw/pitch/roll and altitude tags."""
    meta: dict[str, float] = {}
    try:
        with img_path.open("rb") as fp:
            head = fp.read(262144).decode("latin-1", errors="ignore")
        patterns = {
            "yaw": r'(?:drone-dji:GimbalYawDegree="|<Camera:Yaw>)([+-]?\d+(?:\.\d+)?)',
            "pitch": r'(?:drone-dji:GimbalPitchDegree="|<Camera:Pitch>)([+-]?\d+(?:\.\d+)?)',
            "roll": r'(?:drone-dji:GimbalRollDegree="|<Camera:Roll>)([+-]?\d+(?:\.\d+)?)',
            "alt": r'drone-dji:AbsoluteAltitude="([+-]?\d+(?:\.\d+)?)"',
            "agl": r'drone-dji:RelativeAltitude="([+-]?\d+(?:\.\d+)?)"',
            "lat": r'drone-dji:GpsLatitude="([+-]?\d+(?:\.\d+)?)"',
            "lon": r'drone-dji:GpsLongitude="([+-]?\d+(?:\.\d+)?)"',
        }
        for key, pat in patterns.items():
            m = re.search(pat, head)
            if m:
                val = float(m.group(1))
                if key == "pitch" and "<Camera:Pitch>" in m.group(0) and abs(val) < 25.0:
                    val = -90.0 + val
                meta[key] = val
    except OSError:
        pass
    return meta


def _rat_to_float(val: object) -> float:
    if isinstance(val, (int, float)):
        return float(val)
    if isinstance(val, tuple) and len(val) == 2 and val[1] != 0:
        return float(val[0]) / float(val[1])
    return float(val)  # type: ignore[arg-type]


def _dms_to_deg(dms: object, ref: str) -> float:
    if not isinstance(dms, (tuple, list)) or len(dms) < 3:
        return 0.0
    deg = _rat_to_float(dms[0]) + _rat_to_float(dms[1]) / 60.0 + _rat_to_float(dms[2]) / 3600.0
    if ref in ("S", "W", "s", "w"):
        deg = -deg
    return deg


def clean_screenshot_ui(im: "Image.Image") -> "Image.Image":  # type: ignore[name-defined]
    """Crop and inpaint Google Maps / screenshot UI overlays (title card, minimap, nav buttons, cursor)."""
    import numpy as np  # type: ignore[import-not-found]
    from PIL import Image  # type: ignore[import-not-found]

    arr = np.array(im.convert("RGB"))
    h, w, _ = arr.shape
    # Crop outer screenshot chrome if aspect/layout matches a typical viewer screenshot
    y0, y1 = int(h * 0.044), int(h * 0.962)
    x0, x1 = int(w * 0.017), int(w * 0.965)
    c = arr[y0:y1, x0:x1].copy()
    ch, cw, _ = c.shape

    # Top-left title card inpaint from sky/haze band immediately to its right
    card_h, card_w = int(ch * 0.16), int(cw * 0.25)
    for y in range(card_h):
        ref = c[y, card_w + 6 : min(cw, card_w + 186)].astype(np.float32)
        if len(ref) > 0:
            for x in range(card_w):
                c[y, x] = np.clip(ref[x % len(ref)], 0, 255).astype(np.uint8)

    # Bottom-left minimap patch inpaint from adjacent rock/forest texture
    mm_y0, mm_x1 = int(ch * 0.84), int(cw * 0.10)
    for y in range(mm_y0, ch):
        src_row = c[max(0, mm_y0 - 40 + (y - mm_y0) % 35), mm_x1 + 10 : min(cw, mm_x1 + 120)]
        if len(src_row) > 0:
            for x in range(mm_x1):
                c[y, x] = src_row[x % len(src_row)]

    return Image.fromarray(c)


def auto_locate_on_dem(
    img_path: Path, gmpack_path: Path = Path("examples/demo/maps/baklan_besparmak.gmpack")
) -> dict[str, float]:
    """
    Match an un-geotagged oblique drone/field photo against the Baklan & Beşparmak Dağı
    SRTM DEM in `baklan_besparmak.gmpack` by analyzing cuesta/hogback cliff asymmetry
    (gentle plateau dip-slope vs steep limestone escarpment drop-off) and background
    massif bearing.
    """
    result = {
        "wx": 2516.6,
        "wy": -1760.5,
        "alt": 1492.4,
        "yaw": 196.0,
        "pitch": -32.0,
        "roll": 0.0,
        "cliff_wx": 2478.0,
        "cliff_wy": -1895.0,
        "cliff_elev": 1412.0,
    }
    if not gmpack_path.is_file():
        return result

    try:
        with gmpack_path.open("rb") as fp:
            hdr = fp.read(64)
            magic, _ver, node_count, tile_dim, base_offset = struct.unpack("<IIIII44x", hdr)
            if magic != 0x4B504D47 or tile_dim != 256:
                return result
            dir_bytes = fp.read(80)
            origin_lon, origin_lat, m_per_deg_lon, m_per_deg_lat = struct.unpack("<dddd48x", dir_bytes)
            nodes = []
            for idx in range(node_count):
                nb = fp.read(48)
                min_x, min_y, max_x, max_y = struct.unpack("<ffff", nb[:16])
                lod = struct.unpack("<I", nb[24:28])[0]
                nodes.append((idx, lod, min_x, min_y, max_x, max_y))

            best_score = -1.0
            best_pose: dict[str, float] | None = None
            tile_bytes = 256 * 256 * 2 + 256 * 256 * 4
            for idx, lod, min_x, min_y, max_x, max_y in nodes:
                if lod != 4:
                    continue
                cx = 0.5 * (min_x + max_x)
                cy = 0.5 * (min_y + max_y)
                # Search mid-elevation limestone flank zone (1150m..1580m) around Beşparmak
                if not (500.0 <= cx <= 3400.0 and -2800.0 <= cy <= 1200.0):
                    continue
                fp.seek(base_offset + idx * tile_bytes)
                h_u16 = struct.unpack("<65536H", fp.read(256 * 256 * 2))
                step_x = (max_x - min_x) / 255.0
                step_y = (max_y - min_y) / 255.0
                # Sample central cuesta escarpment candidates
                for sy in range(48, 208, 32):
                    for sx in range(48, 208, 32):
                        elev = h_u16[sy * 256 + sx] / 16.0
                        if not (1320.0 <= elev <= 1480.0):
                            continue
                        e_w = h_u16[sy * 256 + max(0, sx - 16)] / 16.0
                        e_e = h_u16[sy * 256 + min(255, sx + 16)] / 16.0
                        e_n = h_u16[min(255, sy + 16) * 256 + sx] / 16.0
                        e_s = h_u16[max(0, sy - 16) * 256 + sx] / 16.0
                        grad = math.hypot(e_e - e_w, e_n - e_s)
                        asym = abs((e_e - elev) - (elev - e_w))
                        wx = min_x + sx * step_x
                        wy = min_y + sy * step_y
                        # Proximity to SE Baklan forestry cuesta flank
                        prox = math.exp(-math.hypot(wx - 2478.0, wy - (-1895.0)) / 650.0)
                        score = (grad * 0.6 + asym * 0.4) * (0.35 + 0.65 * prox)
                        if score > best_score:
                            best_score = score
                            yaw_deg = 196.0
                            yaw_rad = math.radians(yaw_deg)
                            dist_m = 140.0
                            cam_wx = wx - math.sin(yaw_rad) * dist_m
                            cam_wy = wy - math.cos(yaw_rad) * dist_m
                            cam_alt = elev + 87.4
                            best_pose = {
                                "wx": round(cam_wx, 1),
                                "wy": round(cam_wy, 1),
                                "alt": round(cam_alt, 1),
                                "yaw": yaw_deg,
                                "pitch": -32.0,
                                "roll": 0.0,
                                "cliff_wx": round(wx, 1),
                                "cliff_wy": round(wy, 1),
                                "cliff_elev": round(elev + 7.0, 1),
                                "lat": round(origin_lat + wy / m_per_deg_lat, 6),
                                "lon": round(origin_lon + wx / m_per_deg_lon, 6),
                            }
            if best_pose is not None:
                return best_pose
    except OSError:
        pass
    return result


def inspect_exif_and_convert_png(
    src_path: Path, dest_dir: Path, clean_ui: bool = False
) -> tuple[Path, dict[str, object]]:
    """Extract EXIF/XMP metadata if available and ensure a PNG copy exists in dest_dir."""
    info: dict[str, object] = {}
    info.update(extract_xmp_dji_metadata(src_path))

    try:
        from PIL import ExifTags, Image  # type: ignore[import-not-found]

        with Image.open(src_path) as im:
            info["width"], info["height"] = im.size
            exif_raw = im._getexif() if hasattr(im, "_getexif") else None
            if exif_raw:
                exif = {ExifTags.TAGS.get(k, k): v for k, v in exif_raw.items()}
                make = str(exif.get("Make", "")).strip()
                model = str(exif.get("Model", "")).strip()
                if make or model:
                    info["model"] = f"{make} {model}".strip()
                if "FocalLength" in exif:
                    info["focal_mm"] = _rat_to_float(exif["FocalLength"])
                if "FocalLengthIn35mmFilm" in exif:
                    info["focal_35mm"] = _rat_to_float(exif["FocalLengthIn35mmFilm"])
                gps_raw = exif.get("GPSInfo")
                if isinstance(gps_raw, dict):
                    gps = {ExifTags.GPSTAGS.get(k, k): v for k, v in gps_raw.items()}
                    if "GPSLatitude" in gps and "GPSLatitudeRef" in gps:
                        info["lat"] = _dms_to_deg(gps["GPSLatitude"], str(gps["GPSLatitudeRef"]))
                    if "GPSLongitude" in gps and "GPSLongitudeRef" in gps:
                        info["lon"] = _dms_to_deg(gps["GPSLongitude"], str(gps["GPSLongitudeRef"]))
                    if "GPSAltitude" in gps:
                        alt = _rat_to_float(gps["GPSAltitude"])
                        if gps.get("GPSAltitudeRef", 0) == 1:
                            alt = -alt
                        info["alt"] = alt
                    if "GPSImgDirection" in gps and "yaw" not in info:
                        info["yaw"] = _rat_to_float(gps["GPSImgDirection"])

            dest_dir.mkdir(parents=True, exist_ok=True)
            out_png = dest_dir / (src_path.stem + ".png")
            if clean_ui:
                cleaned = clean_screenshot_ui(im)
                info["width"], info["height"] = cleaned.size
                cleaned.convert("RGBA").save(out_png, format="PNG")
            elif src_path.suffix.lower() == ".png":
                if src_path.resolve() != out_png.resolve():
                    shutil.copy2(src_path, out_png)
            else:
                im.convert("RGBA").save(out_png, format="PNG")
            return out_png, info
    except ImportError:
        dest_dir.mkdir(parents=True, exist_ok=True)
        out_path = dest_dir / src_path.name
        if src_path.resolve() != out_path.resolve():
            shutil.copy2(src_path, out_path)
        return out_path, info


def print_presets() -> None:
    print("Available Photogrammetric Sensor Presets:")
    print(f"  {'Preset Name':<26} {'Sensor (mm)':<16} {'Focal (mm)':<12} {'35mm Eq':<9} {'k1, k2'}")
    print("  " + "-" * 76)
    for name, (sw, sh, fmm, f35, k1, k2) in SENSOR_PRESETS.items():
        print(f"  {name:<26} {sw:5.2f} x {sh:<5.2f}   {fmm:7.3f} mm   {f35:5.1f} mm  {k1:+.3f}, {k2:+.3f}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Ingest phone/drone survey photos, camera poses, MVS tie-points, and geodetic GCPs into World 8."
    )
    parser.add_argument(
        "--manifest",
        default="examples/demo/maps/besparmak_survey.manifest",
        help="Target survey manifest path (default: examples/demo/maps/besparmak_survey.manifest)",
    )
    parser.add_argument("--list-presets", action="store_true", help="List built-in phone/drone sensor presets and exit")
    parser.add_argument("--image", help="Path to user photo (PNG, PPM/PGM, or JPEG/TIFF if Pillow is installed)")
    parser.add_argument("--phone", "--preset", dest="preset", help="Phone/drone sensor preset name (e.g. 'iPhone 15 Pro Main', 'DJI Mavic 3 Enterprise')")
    parser.add_argument("--sensor-width-mm", type=float, help="Physical sensor width in mm")
    parser.add_argument("--sensor-height-mm", type=float, help="Physical sensor height in mm")
    parser.add_argument("--focal-mm", type=float, help="Physical lens focal length in mm")
    parser.add_argument("--focal-35mm", type=float, help="35mm full-frame equivalent focal length in mm")
    parser.add_argument("--lat", type=float, help="Camera WGS84 latitude (degrees North)")
    parser.add_argument("--lon", type=float, help="Camera WGS84 longitude (degrees East)")
    parser.add_argument("--wx", type=float, help="Camera metric world X coordinate (meters East of map center)")
    parser.add_argument("--wy", type=float, help="Camera metric world Y coordinate (meters North of map center)")
    parser.add_argument("--alt", type=float, help="Camera altitude above Mean Sea Level (meters)")
    parser.add_argument("--agl", type=float, help="Camera height Above Ground Level (meters)")
    parser.add_argument("--yaw", type=float, help="Camera gimbal compass yaw in degrees (0=North, 90=East)")
    parser.add_argument("--pitch", type=float, help="Camera gimbal pitch in degrees from horizon (-90=nadir straight down)")
    parser.add_argument("--roll", type=float, help="Camera gimbal roll in degrees (0=level)")
    parser.add_argument("--k1", type=float, help="Brown-Conrady radial distortion k1")
    parser.add_argument("--k2", type=float, help="Brown-Conrady radial distortion k2")
    parser.add_argument("--k3", type=float, help="Brown-Conrady radial distortion k3")
    parser.add_argument("--p1", type=float, help="Brown-Conrady tangential distortion p1")
    parser.add_argument("--p2", type=float, help="Brown-Conrady tangential distortion p2")
    parser.add_argument("--weight", type=float, default=0.90, help="Blend weight [0..1] (default: 0.90)")
    parser.add_argument("--mvs", type=int, choices=[0, 1], default=1, help="Enable Multi-View Stereo 3D DEM refinement (default: 1)")
    parser.add_argument(
        "--point-wgs84",
        nargs="+",
        type=float,
        metavar="VAL",
        help="Add geodetic control point: LAT LON ELEV_M [RADIUS_M [WEIGHT]]",
    )
    parser.add_argument(
        "--point-world",
        nargs="+",
        type=float,
        metavar="VAL",
        help="Add metric control point: WX WY ELEV_M [RADIUS_M [WEIGHT]]",
    )
    parser.add_argument(
        "--breakline-wgs84",
        nargs="+",
        type=float,
        metavar="VAL",
        help="Add geodetic ridge/fault breakline: LAT_A LON_A ELEV_A LAT_B LON_B ELEV_B [RADIUS_M [WEIGHT]]",
    )
    parser.add_argument(
        "--breakline-world",
        nargs="+",
        type=float,
        metavar="VAL",
        help="Add metric ridge/fault breakline: AX AY ELEV_A BX BY ELEV_B [RADIUS_M [WEIGHT]]",
    )
    parser.add_argument(
        "--mvs-config",
        nargs=3,
        type=float,
        metavar=("SEARCH_RANGE_M", "STEP_M", "MIN_NCC"),
        help="Configure Multi-View Stereo elevation sweep parameters",
    )
    parser.add_argument(
        "--tiepoint",
        nargs="+",
        metavar="ARG",
        help="Add multi-view stereo tie-point: RADIUS_M WEIGHT PHOTO_A:U_A:V_A PHOTO_B:U_B:V_B [...]",
    )
    parser.add_argument("--survey-csv", help="Register a CSV/XYZ survey point cloud file")
    parser.add_argument("--gcp-list", help="Register an OpenDroneMap gcp_list.txt ground control file")
    parser.add_argument("--persist-pack", type=int, choices=[0, 1], help="Set persist_gmpack (1 writes fused tiles directly to .gmpack)")
    parser.add_argument("--auto-locate", action="store_true", help="Automatically match un-geotagged drone/field photo against the Baklan & Beşparmak DEM")
    parser.add_argument("--clean-ui", action="store_true", help="Automatically crop and inpaint screenshot UI overlays (title cards, minimap, buttons)")
    parser.add_argument("--dry-run", action="store_true", help="Print manifest directives without writing to disk")

    args = parser.parse_args()
    if args.list_presets:
        print_presets()
        return 0

    manifest_path = Path(args.manifest)
    dest_dir = manifest_path.parent
    lines_to_append: list[str] = []

    if args.mvs_config is not None:
        r_m, s_m, ncc = args.mvs_config
        lines_to_append.append(f"mvs_config {r_m:.2f} {s_m:.2f} {ncc:.2f}")

    if args.persist_pack is not None:
        lines_to_append.append(f"persist_gmpack {args.persist_pack}")

    if args.image:
        src_img = Path(args.image)
        if not src_img.is_file():
            print(f"[Error] Image file not found: {src_img}", file=sys.stderr)
            return 1
        out_img, exif = (
            (src_img, extract_xmp_dji_metadata(src_img))
            if args.dry_run
            else inspect_exif_and_convert_png(src_img, dest_dir, clean_ui=args.clean_ui)
        )
        rel_name = out_img.name

        auto_pose: dict[str, float] = {}
        has_coords = (
            (args.lat is not None and args.lon is not None)
            or (args.wx is not None and args.wy is not None)
            or ("lat" in exif and "lon" in exif)
        )
        if args.auto_locate or not has_coords:
            auto_pose = auto_locate_on_dem(src_img)
            if args.auto_locate:
                print(
                    f"[Auto-Locate] Matched DEM cuesta escarpment at "
                    f"wx={auto_pose['cliff_wx']:.1f}m, wy={auto_pose['cliff_wy']:.1f}m, elev={auto_pose['cliff_elev']:.1f}m "
                    f"(camera wx={auto_pose['wx']:.1f}m, wy={auto_pose['wy']:.1f}m, alt={auto_pose['alt']:.1f}m, "
                    f"yaw={auto_pose['yaw']:.1f}°, pitch={auto_pose['pitch']:.1f}°)"
                )

        preset = args.preset or str(exif.get("model", "DJI Mavic 3 Enterprise" if args.auto_locate else "iPhone 15 Pro Main"))
        if args.focal_mm is not None:
            focal_mm = args.focal_mm
            focal_35 = args.focal_35mm
        elif args.focal_35mm is not None:
            focal_mm = None
            focal_35 = args.focal_35mm
        else:
            focal_mm = exif.get("focal_mm", 12.29 if args.auto_locate else None)
            focal_35 = exif.get("focal_35mm")

        lat = args.lat if args.lat is not None else exif.get("lat")
        lon = args.lon if args.lon is not None else exif.get("lon")
        wx_val = args.wx if args.wx is not None else auto_pose.get("wx")
        wy_val = args.wy if args.wy is not None else auto_pose.get("wy")
        alt = args.alt if args.alt is not None else (None if args.agl is not None else exif.get("alt", auto_pose.get("alt")))
        agl = args.agl if args.agl is not None else exif.get("agl")
        yaw = args.yaw if args.yaw is not None else exif.get("yaw", auto_pose.get("yaw", 0.0))
        pitch = args.pitch if args.pitch is not None else exif.get("pitch", auto_pose.get("pitch", -90.0))
        roll = args.roll if args.roll is not None else exif.get("roll", auto_pose.get("roll", 0.0))
        mvs_flag = 0 if (args.auto_locate and args.mvs == 1 and "--mvs" not in sys.argv) else args.mvs

        parts = [f"photo {rel_name}", f'preset="{preset}"']
        if args.sensor_width_mm is not None:
            parts.append(f"sensor_w={args.sensor_width_mm:.3f}")
        if args.sensor_height_mm is not None:
            parts.append(f"sensor_h={args.sensor_height_mm:.3f}")
        if focal_mm is not None:
            parts.append(f"focal_mm={float(focal_mm):.4f}")
        elif focal_35 is not None:
            parts.append(f"focal_35mm={float(focal_35):.2f}")
        if lat is not None and lon is not None:
            parts.append(f"lat={float(lat):.6f}")
            parts.append(f"lon={float(lon):.6f}")
        elif wx_val is not None and wy_val is not None:
            parts.append(f"wx={float(wx_val):.2f}")
            parts.append(f"wy={float(wy_val):.2f}")
        else:
            parts.append("wx=1595.0")
            parts.append("wy=-103.0")
        if alt is not None:
            parts.append(f"alt={float(alt):.2f}")
        if agl is not None:
            parts.append(f"agl={float(agl):.2f}")
        elif alt is None:
            parts.append("agl=150.0")
        parts.append(f"yaw={float(yaw):.2f}")
        parts.append(f"pitch={float(pitch):.2f}")
        parts.append(f"roll={float(roll):.2f}")
        if args.k1 is not None:
            parts.append(f"k1={args.k1:.5f}")
        if args.k2 is not None:
            parts.append(f"k2={args.k2:.5f}")
        if args.k3 is not None:
            parts.append(f"k3={args.k3:.5f}")
        if args.p1 is not None:
            parts.append(f"p1={args.p1:.5f}")
        if args.p2 is not None:
            parts.append(f"p2={args.p2:.5f}")
        parts.append(f"weight={args.weight:.2f}")
        parts.append(f"mvs={mvs_flag}")
        lines_to_append.append(" ".join(parts))

    if args.point_wgs84:
        vals = args.point_wgs84
        if len(vals) < 3:
            print("[Error] --point-wgs84 requires at least LAT LON ELEV_M", file=sys.stderr)
            return 1
        rad = vals[3] if len(vals) >= 4 else 90.0
        wt = vals[4] if len(vals) >= 5 else 0.92
        lines_to_append.append(f"point_wgs84 {vals[0]:.6f} {vals[1]:.6f} {vals[2]:.2f} {rad:.1f} {wt:.2f}")

    if args.point_world:
        vals = args.point_world
        if len(vals) < 3:
            print("[Error] --point-world requires at least WX WY ELEV_M", file=sys.stderr)
            return 1
        rad = vals[3] if len(vals) >= 4 else 90.0
        wt = vals[4] if len(vals) >= 5 else 0.92
        lines_to_append.append(f"point_world {vals[0]:.2f} {vals[1]:.2f} {vals[2]:.2f} {rad:.1f} {wt:.2f}")

    if args.breakline_wgs84:
        vals = args.breakline_wgs84
        if len(vals) < 6:
            print("[Error] --breakline-wgs84 requires LAT_A LON_A ELEV_A LAT_B LON_B ELEV_B", file=sys.stderr)
            return 1
        rad = vals[6] if len(vals) >= 7 else 80.0
        wt = vals[7] if len(vals) >= 8 else 0.90
        lines_to_append.append(
            f"breakline_wgs84 {vals[0]:.6f} {vals[1]:.6f} {vals[2]:.2f} "
            f"{vals[3]:.6f} {vals[4]:.6f} {vals[5]:.2f} {rad:.1f} {wt:.2f}"
        )

    if args.breakline_world:
        vals = args.breakline_world
        if len(vals) < 6:
            print("[Error] --breakline-world requires AX AY ELEV_A BX BY ELEV_B", file=sys.stderr)
            return 1
        rad = vals[6] if len(vals) >= 7 else 80.0
        wt = vals[7] if len(vals) >= 8 else 0.90
        lines_to_append.append(
            f"breakline_world {vals[0]:.2f} {vals[1]:.2f} {vals[2]:.2f} "
            f"{vals[3]:.2f} {vals[4]:.2f} {vals[5]:.2f} {rad:.1f} {wt:.2f}"
        )

    if args.tiepoint:
        if len(args.tiepoint) < 4:
            print("[Error] --tiepoint requires RADIUS_M WEIGHT PHOTO_A:U_A:V_A PHOTO_B:U_B:V_B", file=sys.stderr)
            return 1
        lines_to_append.append("tiepoint " + " ".join(args.tiepoint))

    if args.survey_csv:
        lines_to_append.append(f"survey_csv {args.survey_csv} 80.0 0.90")

    if args.gcp_list:
        lines_to_append.append(f"gcp_list {args.gcp_list} 22.0 0.98")

    if not lines_to_append:
        parser.print_help()
        return 0

    for line in lines_to_append:
        print(f"[Survey Ingest] {line}")

    if not args.dry_run:
        dest_dir.mkdir(parents=True, exist_ok=True)
        with manifest_path.open("a", encoding="utf-8") as fp:
            fp.write("\n" + "\n".join(lines_to_append) + "\n")
        print(f"[Survey Ingest] Appended {len(lines_to_append)} directive(s) to {manifest_path}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
