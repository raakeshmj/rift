#!/bin/bash

# eBPF XDP benchmark script (Linux only)
if [[ "$(uname -s)" != "Linux" ]]; then
    echo "Skipping eBPF benchmark (not on Linux)"
    exit 0
fi

echo "Running eBPF XDP benchmark..."
# Load XDP program
ip link set dev lo xdpgeneric off 2>/dev/null || true
./ebpf/loader/loader --attach --iface lo --prog build/ebpf/xdp_filter.o

# Run high-PPS flood
./build/nps_metrics --mode nps --size 104857600

# Read map stats
./ebpf/loader/map_reader stats

# Detach
ip link set dev lo xdpgeneric off
