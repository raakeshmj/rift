#!/bin/bash
# ═══════════════════════════════════════════════════════════════════
# run_bench.sh - Automated benchmark runner for RIFT
# ═══════════════════════════════════════════════════════════════════

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-./build}"
RESULTS_DIR="${RESULTS_DIR:-./bench_results}"

mkdir -p "$RESULTS_DIR"

timestamp=$(date +%Y%m%d_%H%M%S)
result_file="$RESULTS_DIR/bench_$timestamp.txt"

echo "═══════════════════════════════════════════════════════" | tee "$result_file"
echo "  RIFT Benchmark Suite - $(date)" | tee -a "$result_file"
echo "═══════════════════════════════════════════════════════" | tee -a "$result_file"

# ── Benchmark 1: Small transfers ──────────────────────────────────
echo "" | tee -a "$result_file"
echo "── Test 1: Small transfer (64 KB) ──" | tee -a "$result_file"
"$BUILD_DIR/rift_bench" --size 65536 --window 16 --runs 3 \
    2>&1 | tee -a "$result_file"

# ── Benchmark 2: Medium transfers ─────────────────────────────────
echo "" | tee -a "$result_file"
echo "── Test 2: Medium transfer (1 MB) ──" | tee -a "$result_file"
"$BUILD_DIR/rift_bench" --size 1048576 --window 32 --runs 3 \
    --port 10001 2>&1 | tee -a "$result_file"

# ── Benchmark 3: Large transfers ──────────────────────────────────
echo "" | tee -a "$result_file"
echo "── Test 3: Large transfer (10 MB) ──" | tee -a "$result_file"
"$BUILD_DIR/rift_bench" --size 10485760 --window 64 --runs 3 \
    --port 10004 2>&1 | tee -a "$result_file"

# ── Benchmark 4: Window size comparison ───────────────────────────
echo "" | tee -a "$result_file"
echo "── Test 4: Window size comparison (1 MB) ──" | tee -a "$result_file"
for ws in 4 16 32 64 128; do
    echo "  Window size: $ws" | tee -a "$result_file"
    "$BUILD_DIR/rift_bench" --size 1048576 --window "$ws" --runs 1 \
        --port $((10007 + ws)) 2>&1 | tee -a "$result_file"
done

echo "" | tee -a "$result_file"
echo "═══════════════════════════════════════════════════════" | tee -a "$result_file"
echo "  Results saved to: $result_file" | tee -a "$result_file"
echo "═══════════════════════════════════════════════════════" | tee -a "$result_file"
