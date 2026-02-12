#!/bin/bash
# ═══════════════════════════════════════════════════════════════════
# cleanup.sh - Cleanup script for NPS
# Removes build artifacts, netem rules, and BPF programs
# ═══════════════════════════════════════════════════════════════════

set -euo pipefail

echo "NPS Cleanup"
echo "═══════════════════════════════════════════════════════"

# Clean build
echo "  Cleaning build artifacts..."
make clean 2>/dev/null || true

# Remove netem rules (Linux only)
if command -v tc &>/dev/null; then
    echo "  Removing netem rules..."
    sudo tc qdisc del dev lo root 2>/dev/null || true
fi

# Detach XDP programs (Linux only)
if command -v ip &>/dev/null; then
    echo "  Detaching XDP programs..."
    sudo ip link set dev lo xdpgeneric off 2>/dev/null || true
fi

# Remove pinned BPF maps
if [ -d /sys/fs/bpf/nps ]; then
    echo "  Removing pinned BPF maps..."
    sudo rm -rf /sys/fs/bpf/nps 2>/dev/null || true
fi

# Remove log files
if [ -f /tmp/nps.log ]; then
    echo "  Removing log files..."
    rm -f /tmp/nps.log
fi

# Remove benchmark results
if [ -d bench_results ]; then
    echo "  Removing benchmark results..."
    rm -rf bench_results
fi

echo "═══════════════════════════════════════════════════════"
echo "  [OK] Cleanup complete"
