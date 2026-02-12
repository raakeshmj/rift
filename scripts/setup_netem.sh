#!/bin/bash
# ═══════════════════════════════════════════════════════════════════
# setup_netem.sh - Network emulation setup using tc/netem
#
# Sets up packet loss, delay, and reordering for NPS testing.
# Requires: root, Linux with tc/netem support
#
# Usage:
#   sudo ./setup_netem.sh setup [OPTIONS]
#   sudo ./setup_netem.sh teardown
# ═══════════════════════════════════════════════════════════════════

set -euo pipefail

# Defaults
IFACE="${NPS_IFACE:-lo}"
LOSS="${NPS_LOSS:-5%}"
DELAY="${NPS_DELAY:-10ms}"
JITTER="${NPS_JITTER:-5ms}"
REORDER="${NPS_REORDER:-2%}"
DUPLICATE="${NPS_DUPLICATE:-0.1%}"
CORRUPT="${NPS_CORRUPT:-0.1%}"
PORT="${NPS_PORT:-9999}"

usage() {
    cat <<EOF
NPS Network Emulation Setup

Usage: $0 <command> [options]

Commands:
  setup       Configure netem rules
  teardown    Remove all netem rules
  status      Show current netem configuration

Options (environment variables):
  NPS_IFACE=lo        Interface (default: lo)
  NPS_LOSS=5%         Packet loss rate
  NPS_DELAY=10ms      Added delay
  NPS_JITTER=5ms      Delay jitter
  NPS_REORDER=2%      Packet reordering rate
  NPS_DUPLICATE=0.1%  Packet duplication rate
  NPS_CORRUPT=0.1%    Packet corruption rate
  NPS_PORT=9999       Target port for filtering

Examples:
  sudo NPS_LOSS=10% NPS_DELAY=50ms $0 setup
  sudo $0 teardown
EOF
}

setup_netem() {
    echo "═══════════════════════════════════════════════════════"
    echo "  Setting up network emulation on $IFACE"
    echo "═══════════════════════════════════════════════════════"
    echo "  Loss:      $LOSS"
    echo "  Delay:     $DELAY ± $JITTER"
    echo "  Reorder:   $REORDER"
    echo "  Duplicate: $DUPLICATE"
    echo "  Corrupt:   $CORRUPT"
    echo "═══════════════════════════════════════════════════════"

    # Remove existing qdisc
    tc qdisc del dev "$IFACE" root 2>/dev/null || true

    # Add netem qdisc
    tc qdisc add dev "$IFACE" root handle 1: prio

    # Add netem to band 3 (filtered traffic)
    tc qdisc add dev "$IFACE" parent 1:3 handle 30: netem \
        loss "$LOSS" \
        delay "$DELAY" "$JITTER" distribution normal \
        reorder "$REORDER" \
        duplicate "$DUPLICATE" \
        corrupt "$CORRUPT"

    # Filter NPS traffic to the netem qdisc
    tc filter add dev "$IFACE" parent 1:0 protocol ip u32 \
        match ip dport "$PORT" 0xffff flowid 1:3

    tc filter add dev "$IFACE" parent 1:0 protocol ip u32 \
        match ip sport "$PORT" 0xffff flowid 1:3

    echo ""
    echo "  [OK] Network emulation active"
    echo "  Run '$0 teardown' to remove"
}

teardown_netem() {
    echo "Removing network emulation from $IFACE..."
    tc qdisc del dev "$IFACE" root 2>/dev/null || true
    echo "  [OK] Network emulation removed"
}

show_status() {
    echo "Current tc configuration for $IFACE:"
    echo ""
    echo "── Qdiscs ──"
    tc qdisc show dev "$IFACE" 2>/dev/null || echo "  (none)"
    echo ""
    echo "── Filters ──"
    tc filter show dev "$IFACE" 2>/dev/null || echo "  (none)"
    echo ""
    echo "── Statistics ──"
    tc -s qdisc show dev "$IFACE" 2>/dev/null || echo "  (none)"
}

# ── Main ──────────────────────────────────────────────────────────
case "${1:-}" in
    setup)
        setup_netem
        ;;
    teardown)
        teardown_netem
        ;;
    status)
        show_status
        ;;
    *)
        usage
        exit 1
        ;;
esac
