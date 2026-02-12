#!/bin/bash
# ═══════════════════════════════════════════════════════════════════
# setup_netem.sh - Network emulation setup using tc/netem
#
# Sets up packet loss, delay, and reordering for RIFT testing.
# Requires: root, Linux with tc/netem support
#
# Usage:
#   sudo ./setup_netem.sh setup [OPTIONS]
#   sudo ./setup_netem.sh teardown
# ═══════════════════════════════════════════════════════════════════

set -euo pipefail

# Defaults
IFACE="${RIFT_IFACE:-lo}"
LOSS="${RIFT_LOSS:-5%}"
DELAY="${RIFT_DELAY:-10ms}"
JITTER="${RIFT_JITTER:-5ms}"
REORDER="${RIFT_REORDER:-2%}"
DUPLICATE="${RIFT_DUPLICATE:-0.1%}"
CORRUPT="${RIFT_CORRUPT:-0.1%}"
PORT="${RIFT_PORT:-9999}"

usage() {
    cat <<EOF
RIFT Network Emulation Setup

Usage: $0 <command> [options]

Commands:
  setup       Configure netem rules
  teardown    Remove all netem rules
  status      Show current netem configuration

Options (environment variables):
  RIFT_IFACE=lo        Interface (default: lo)
  RIFT_LOSS=5%         Packet loss rate
  RIFT_DELAY=10ms      Added delay
  RIFT_JITTER=5ms      Delay jitter
  RIFT_REORDER=2%      Packet reordering rate
  RIFT_DUPLICATE=0.1%  Packet duplication rate
  RIFT_CORRUPT=0.1%    Packet corruption rate
  RIFT_PORT=9999       Target port for filtering

Examples:
  sudo RIFT_LOSS=10% RIFT_DELAY=50ms $0 setup
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

    # Filter RIFT traffic to the netem qdisc
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
