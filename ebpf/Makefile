# ═══════════════════════════════════════════════════════════════════
# NPS eBPF - Makefile for eBPF programs
# Requires: clang, linux headers, libbpf
# Supports BPF CO-RE (Compile Once, Run Everywhere)
# ═══════════════════════════════════════════════════════════════════

CLANG       ?= clang
LLC         ?= llc
CC          ?= gcc
BPFTOOL     ?= bpftool

# Architecture detection
ARCH        := $(shell uname -m | sed 's/x86_64/x86/' | sed 's/aarch64/arm64/')

# Kernel headers
KERN_HEADERS ?= /usr/include
LIBBPF_HEADERS ?= /usr/include

CFLAGS_BPF  = -O2 -target bpf -g \
              -D__TARGET_ARCH_$(ARCH) \
              -I$(KERN_HEADERS) \
              -I$(LIBBPF_HEADERS) \
              -I. \
              -Wall

CFLAGS_USER = -Wall -Wextra -O2 -g \
              -I$(LIBBPF_HEADERS) \
              -I. \
              -I../include

LDFLAGS_USER = -lbpf -lelf -lz

BUILD_DIR   = ../build/ebpf
SKEL_DIR    = $(BUILD_DIR)/skel

# ── Targets ───────────────────────────────────────────────────────
BPF_OBJS    = $(BUILD_DIR)/xdp_filter.bpf.o \
              $(BUILD_DIR)/tc_filter.bpf.o

BPF_SKELS   = $(SKEL_DIR)/xdp_filter.skel.h \
              $(SKEL_DIR)/tc_filter.skel.h

USER_BINS   = $(BUILD_DIR)/nps_loader \
              $(BUILD_DIR)/nps_map_reader

.PHONY: all bpf skeletons user clean vmlinux

all: bpf skeletons user
	@echo ""
	@echo "eBPF build complete (CO-RE enabled):"
	@echo "  BPF objects:  $(BPF_OBJS)"
	@echo "  Skeletons:    $(BPF_SKELS)"
	@echo "  Loader:       $(BUILD_DIR)/nps_loader"
	@echo "  Map reader:   $(BUILD_DIR)/nps_map_reader"

$(BUILD_DIR) $(SKEL_DIR):
	@mkdir -p $@

# ── Generate vmlinux.h from running kernel BTF ───────────────────
vmlinux.h:
	@if [ ! -f vmlinux.h ]; then \
		echo "  [BTF]   Generating vmlinux.h from /sys/kernel/btf/vmlinux"; \
		$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h 2>/dev/null || \
		echo "  [WARN]  Could not generate vmlinux.h (non-fatal)"; \
	fi

# ── BPF Objects (CO-RE compiled) ─────────────────────────────────
bpf: vmlinux.h $(BPF_OBJS)

$(BUILD_DIR)/xdp_filter.bpf.o: xdp_filter.bpf.c common.bpf.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS_BPF) -c $< -o $@
	@echo "  [BPF]   $@ (CO-RE)"

$(BUILD_DIR)/tc_filter.bpf.o: tc_filter.bpf.c common.bpf.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS_BPF) -c $< -o $@
	@echo "  [BPF]   $@ (CO-RE)"

# ── BPF Skeletons (.skel.h) ─────────────────────────────────────
skeletons: $(BPF_SKELS)

$(SKEL_DIR)/xdp_filter.skel.h: $(BUILD_DIR)/xdp_filter.bpf.o | $(SKEL_DIR)
	$(BPFTOOL) gen skeleton $< > $@ 2>/dev/null || \
		echo "  [WARN]  Could not generate XDP skeleton (bpftool unavailable)"
	@echo "  [SKEL]  $@"

$(SKEL_DIR)/tc_filter.skel.h: $(BUILD_DIR)/tc_filter.bpf.o | $(SKEL_DIR)
	$(BPFTOOL) gen skeleton $< > $@ 2>/dev/null || \
		echo "  [WARN]  Could not generate TC skeleton (bpftool unavailable)"
	@echo "  [SKEL]  $@"

# ── Userspace Tools ───────────────────────────────────────────────
user: $(USER_BINS)

$(BUILD_DIR)/nps_loader: loader/loader.c common.bpf.h | $(BUILD_DIR)
	$(CC) $(CFLAGS_USER) -I$(SKEL_DIR) $< $(LDFLAGS_USER) -o $@
	@echo "  [LINK]  $@"

$(BUILD_DIR)/nps_map_reader: loader/map_reader.c common.bpf.h | $(BUILD_DIR)
	$(CC) $(CFLAGS_USER) $< $(LDFLAGS_USER) -o $@
	@echo "  [LINK]  $@"

# ── Clean ─────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR)
	@echo "  [CLEAN] eBPF artifacts removed"
