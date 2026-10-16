#!/usr/bin/env python3
"""
Packs all C/C++/header/shader/build files in a project without truncation.
Outputs:
  1. all_code.txt       -> Single combined file with 100% of the code.
  2. chunk_01.txt, ...  -> 90 KB chunks guaranteed not to be truncated by file upload limits.
"""

import os
import sys
from pathlib import Path

# Extensions and filenames to include
INCLUDE_EXTS = {".c", ".h", ".cpp", ".hpp", ".slang", ".glsl", ".vert", ".frag", ".comp", ".sh"}
INCLUDE_NAMES = {"Makefile", "CMakeLists.txt", "meson.build"}

# Directories and generated files to skip
SKIP_DIRS = {".git", "build", "out", "bin", "obj", "logs", ".vscode", ".idea", "__pycache__"}
SKIP_PREFIXES = ("all_code", "chunk_", "combined_code", "part1_", "part2_", "part3_")

MAX_CHUNK_BYTES = 90_000  # 90 KB safety margin (upload limit is ~100 KB per file)


def should_include(path: Path) -> bool:
    if path.name.startswith(SKIP_PREFIXES):
        return False
    if path.name == "pack_project.py":
        return False
    return path.suffix in INCLUDE_EXTS or path.name in INCLUDE_NAMES


def collect_files(root: Path) -> list[Path]:
    collected = []
    for dirpath, dirnames, filenames in os.walk(root):
        # Prune ignored directories in-place
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS and not d.startswith("."))
        for fname in sorted(filenames):
            fpath = Path(dirpath) / fname
            if should_include(fpath):
                collected.append(fpath)
    return collected


def format_file_block(root: Path, fpath: Path) -> str:
    rel = fpath.relative_to(root).as_posix()
    content = fpath.read_text(encoding="utf-8", errors="replace")
    if not content.endswith("\n"):
        content += "\n"
    return f"/* === FILE: ./{rel} === */\n\n{content}\n"


def split_large_block(block: str, max_bytes: int) -> list[str]:
    """Splits a single file block by lines only if that one file exceeds max_bytes."""
    lines = block.splitlines(keepends=True)
    parts = []
    cur = []
    cur_bytes = 0
    for line in lines:
        line_bytes = len(line.encode("utf-8"))
        if cur and cur_bytes + line_bytes > max_bytes:
            parts.append("".join(cur))
            cur = [line]
            cur_bytes = line_bytes
        else:
            cur.append(line)
            cur_bytes += line_bytes
    if cur:
        parts.append("".join(cur))
    return parts


def main() -> None:
    root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd()
    files = collect_files(root)

    if not files:
        print(f"No matching source files found in {root}")
        sys.exit(1)

    blocks = [format_file_block(root, f) for f in files]

    # 1. Write the single combined file
    all_out = root / "all_code.txt"
    full_text = "".join(blocks)
    all_out.write_text(full_text, encoding="utf-8")
    total_bytes = len(full_text.encode("utf-8"))
    print(f"[1] Wrote {len(files)} files to {all_out.name} ({total_bytes:,} bytes)")

    # Remove old chunk files if any exist
    for old_chunk in root.glob("chunk_*.txt"):
        old_chunk.unlink()

    # 2. Write upload-safe chunks if total size > MAX_CHUNK_BYTES
    if total_bytes <= MAX_CHUNK_BYTES:
        print(f"    -> Under {MAX_CHUNK_BYTES // 1000} KB! You can upload {all_out.name} directly.")
        return

    chunks: list[str] = []
    cur_chunk: list[str] = []
    cur_size = 0

    for block in blocks:
        b_size = len(block.encode("utf-8"))
        if b_size > MAX_CHUNK_BYTES:
            if cur_chunk:
                chunks.append("".join(cur_chunk))
                cur_chunk = []
                cur_size = 0
            chunks.extend(split_large_block(block, MAX_CHUNK_BYTES))
            continue

        if cur_chunk and (cur_size + b_size > MAX_CHUNK_BYTES):
            chunks.append("".join(cur_chunk))
            cur_chunk = [block]
            cur_size = b_size
        else:
            cur_chunk.append(block)
            cur_size += b_size

    if cur_chunk:
        chunks.append("".join(cur_chunk))

    print(f"[2] Project exceeds single-file upload limit; split into {len(chunks)} safe chunks:")
    for idx, chunk_text in enumerate(chunks, start=1):
        c_path = root / f"chunk_{idx:02d}.txt"
        c_path.write_text(chunk_text, encoding="utf-8")
        print(f"    -> {c_path.name} ({len(chunk_text.encode('utf-8')):,} bytes)")


if __name__ == "__main__":
    main()