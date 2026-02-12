# ═══════════════════════════════════════════════════════════════════
# NPS - Network Protocol Stack
# Top-level Makefile
# ═══════════════════════════════════════════════════════════════════

CC          ?= cc
AR          = ar
CFLAGS      = -Wall -Wextra -Werror -std=c11 -O2 -g -pthread
CFLAGS     += -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
INCLUDES    = -Iinclude
LDFLAGS     = -lm -lpthread

# Detect OS
UNAME_S     := $(shell uname -s)

# Build directories
BUILD_DIR   = build
OBJ_DIR     = $(BUILD_DIR)/obj

# ── Source files ──────────────────────────────────────────────────
LIB_SRCS    = src/nps_crc32.c \
              src/nps_protocol.c \
              src/nps_buffer.c \
              src/nps_rtt.c \
              src/nps_window.c \
              src/nps_congestion.c \
              src/nps_log.c \
              src/nps_stats.c \
              src/nps_sender.c \
              src/nps_receiver.c \
              src/nps_trace.c \
              src/nps_crypto.c \
              src/nps_mux.c

LIB_OBJS    = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(LIB_SRCS))
LIB_TARGET  = $(BUILD_DIR)/libnps.a

# ── Tool binaries ────────────────────────────────────────────────
TOOLS       = $(BUILD_DIR)/nps_server \
              $(BUILD_DIR)/nps_client \
              $(BUILD_DIR)/nps_bench \
              $(BUILD_DIR)/nps_losssim \
              $(BUILD_DIR)/nps_integration_test \
              $(BUILD_DIR)/nps_stress_test \
              $(BUILD_DIR)/nps_mux_test \
              $(BUILD_DIR)/nps_metrics

# ── Default target ────────────────────────────────────────────────
.PHONY: all lib tools ebpf test bench stress mux-test clean install help

all: lib tools
	@echo ""
	@echo "╔═══════════════════════════════════════════════════════╗"
	@echo "║          NPS Build Complete                          ║"
	@echo "╠═══════════════════════════════════════════════════════╣"
	@echo "║ Library:  $(LIB_TARGET)"
	@echo "║ Tools:    $(BUILD_DIR)/nps_server"
	@echo "║           $(BUILD_DIR)/nps_client"
	@echo "║           $(BUILD_DIR)/nps_bench"
	@echo "║           $(BUILD_DIR)/nps_losssim"
	@echo "║           $(BUILD_DIR)/nps_integration_test"
	@echo "║           $(BUILD_DIR)/nps_stress_test"
	@echo "║           $(BUILD_DIR)/nps_mux_test"
	@echo "╚═══════════════════════════════════════════════════════╝"
ifeq ($(UNAME_S),Linux)
	@echo ""
	@echo "  eBPF: Run 'make ebpf' to build eBPF programs (requires clang + libbpf)"
endif

# ── Create directories ────────────────────────────────────────────
$(BUILD_DIR) $(OBJ_DIR):
	@mkdir -p $@

# ── Static library ────────────────────────────────────────────────
lib: $(LIB_TARGET)

$(LIB_TARGET): $(LIB_OBJS) | $(BUILD_DIR)
	$(AR) rcs $@ $^
	@echo "  [AR]    $@"

$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
	@echo "  [CC]    $< → $@"

# ── Tool binaries ────────────────────────────────────────────────
tools: $(TOOLS)

$(BUILD_DIR)/nps_server: tools/nps_server.c $(LIB_TARGET) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L$(BUILD_DIR) -lnps $(LDFLAGS) -o $@
	@echo "  [LINK]  $@"

$(BUILD_DIR)/nps_client: tools/nps_client.c $(LIB_TARGET) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L$(BUILD_DIR) -lnps $(LDFLAGS) -o $@
	@echo "  [LINK]  $@"

$(BUILD_DIR)/nps_bench: tools/nps_bench.c $(LIB_TARGET) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L$(BUILD_DIR) -lnps $(LDFLAGS) -o $@
	@echo "  [LINK]  $@"

$(BUILD_DIR)/nps_losssim: tools/nps_losssim.c $(LIB_TARGET) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L$(BUILD_DIR) -lnps $(LDFLAGS) -o $@
	@echo "  [LINK]  $@"

$(BUILD_DIR)/nps_integration_test: tools/nps_integration_test.c $(LIB_TARGET) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L$(BUILD_DIR) -lnps $(LDFLAGS) -o $@
	@echo "  [LINK]  $@"

$(BUILD_DIR)/nps_stress_test: tools/nps_stress_test.c $(LIB_TARGET) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L$(BUILD_DIR) -lnps $(LDFLAGS) -o $@
	@echo "  [LINK]  $@"

$(BUILD_DIR)/nps_mux_test: tools/nps_mux_test.c $(LIB_TARGET) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L$(BUILD_DIR) -lnps $(LDFLAGS) -o $@
	@echo "  [LINK]  $@"

$(BUILD_DIR)/nps_metrics: tools/nps_metrics.c $(LIB_TARGET) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L$(BUILD_DIR) -lnps $(LDFLAGS) -o $@
	@echo "  [LINK]  $@"

# ── Metrics ───────────────────────────────────────────────────────
metrics: $(BUILD_DIR)/nps_metrics
	@echo "Running full metrics suite (1GB transfer)..."
	./$(BUILD_DIR)/nps_metrics --size 1073741824

ifeq ($(UNAME_S),Linux)
ebpf:
	$(MAKE) -C ebpf
else
ebpf:
	@echo "  [SKIP]  eBPF programs only supported on Linux"
endif

# ── Test ──────────────────────────────────────────────────────────
test: $(BUILD_DIR)/nps_integration_test
	@echo ""
	@echo "Running integration tests..."
	@echo "═══════════════════════════════════════════════════════"
	./$(BUILD_DIR)/nps_integration_test
	@echo "═══════════════════════════════════════════════════════"

# ── Stress Test ───────────────────────────────────────────────────
stress: $(BUILD_DIR)/nps_stress_test
	@echo "Running congestion stress test..."
	./$(BUILD_DIR)/nps_stress_test

# ── Mux Test ─────────────────────────────────────────────────────
mux-test: $(BUILD_DIR)/nps_mux_test
	@echo "Running multiplexing test..."
	./$(BUILD_DIR)/nps_mux_test

# ── Benchmark ─────────────────────────────────────────────────────
bench: $(BUILD_DIR)/nps_bench
	@echo "Running benchmark..."
	./$(BUILD_DIR)/nps_bench

# ── Clean ─────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR)
ifeq ($(UNAME_S),Linux)
	$(MAKE) -C ebpf clean 2>/dev/null || true
endif
	@echo "  [CLEAN] Done"

# ── Help ──────────────────────────────────────────────────────────
help:
	@echo "NPS Makefile targets:"
	@echo "  all       - Build library and tools (default)"
	@echo "  lib       - Build static library (libnps.a)"
	@echo "  tools     - Build test and benchmark tools"
	@echo "  ebpf      - Build eBPF programs (Linux only, CO-RE)"
	@echo "  test      - Run integration tests"
	@echo "  stress    - Run congestion stress test"
	@echo "  mux-test  - Run multiplexing test"
	@echo "  bench     - Run benchmarks"
	@echo "  clean     - Remove build artifacts"
	@echo "  help      - Show this help"
