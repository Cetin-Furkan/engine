#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

OUT_DIR="${1:-benchmarks/latest}"
FRAMES="${BENCH_FRAMES:-75}"
mkdir -p "$OUT_DIR"

echo "==> [Benchmark] Building debug + release + unit tests..."
make debug > "$OUT_DIR/build_debug.log" 2>&1
make release > "$OUT_DIR/build_release.log" 2>&1
make test > "$OUT_DIR/unit_tests.log" 2>&1

DEBUG_BYTES=$(stat -c%s build/debug/bin/engine)
RELEASE_BYTES=$(stat -c%s build/release/bin/engine)
size build/debug/bin/engine build/release/bin/engine > "$OUT_DIR/binary_sections.txt"
uname -a > "$OUT_DIR/host_uname.txt"
lscpu > "$OUT_DIR/host_cpu.txt" 2>/dev/null || true
lspci | grep -iE 'vga|3d|display' > "$OUT_DIR/host_gpu_pci.txt" 2>/dev/null || true
free -h > "$OUT_DIR/host_mem.txt" 2>/dev/null || true

SUMMARY_LOG="$OUT_DIR/bench_lines.log"
: > "$SUMMARY_LOG"

WORLDS=(0 2 7 8 9)
for w in "${WORLDS[@]}"; do
    echo "==> [Benchmark] Running release engine --world=$w --bench=$FRAMES ..."
    LOG_FILE="$OUT_DIR/world_${w}.log"
    timeout 25s ./build/release/bin/engine --world="$w" --bench="$FRAMES" > "$LOG_FILE" 2>&1
    grep -E '^\[Bench\]' "$LOG_FILE" | tee -a "$SUMMARY_LOG"
done

cat > "$OUT_DIR/build_sizes.env" <<EOF
DEBUG_BYTES=$DEBUG_BYTES
RELEASE_BYTES=$RELEASE_BYTES
BENCH_FRAMES=$FRAMES
TIMESTAMP_ISO=$(date -Iseconds)
GIT_HEAD=$(git rev-parse --short HEAD)
EOF

echo "==> [Benchmark] Saved full snapshot to $OUT_DIR"
