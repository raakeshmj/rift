# NPS — Userspace Network Protocol Stack

Custom reliable transport protocol over UDP with eBPF kernel-space packet filtering, ChaCha20 encryption, and connection multiplexing.

## Build

```bash
make                # library + all tools
make test           # run integration tests (20 tests)
make stress         # run congestion stress test
make mux-test       # run multiplexing test
make bench          # run throughput benchmark
make clean          # full clean
```

Requires GCC or Clang with C11 support. No external dependencies for the core library.

## Usage

### Basic Transfer

```bash
# terminal 1 — start receiver
./build/nps_server -p 9999

# terminal 2 — send 10 MB
./build/nps_client -h 127.0.0.1 -p 9999 -s 10485760
```

### Benchmark

```bash
./build/nps_bench --size 1048576 --runs 3
```

### Loss Simulation

```bash
# receiver
./build/nps_server -p 9999

# UDP proxy: 5% loss, 10ms delay, 5ms jitter
./build/nps_losssim -L 9998 -T 9999 -l 0.05 -d 10 -j 5

# sender through proxy
./build/nps_client -h 127.0.0.1 -p 9998 -s 1048576
```

### Congestion Stress Test

```bash
./build/nps_stress_test -p 10010
```

Runs 5 phases: ramp → saturate → loss burst (20%) → recovery measurement → multi-burst.

### Multiplexing Test

```bash
./build/nps_mux_test
```

Opens 4 concurrent streams over a single socket, sends 256 KB per stream, verifies data integrity.

### eBPF (Linux only)

```bash
# build eBPF programs (requires clang, libbpf, bpftool, root)
make ebpf

# load XDP filter on an interface
sudo ./build/ebpf/nps_loader --iface eth0 --attach xdp

# view live stats
sudo ./build/ebpf/nps_map_reader watch

# add a rate limit rule (1000 pps on port 9999)
sudo ./build/ebpf/nps_map_reader add-rule 0 0 0 9999 17 2 1000
```

### Wireshark

```bash
wireshark -X lua_script:wireshark/nps_dissector.lua -k -i lo
```

## Configuration

All parameters are in `include/nps_config.h`:

| Parameter | Default | Description |
|---|---|---|
| `NPS_MAX_PAYLOAD` | 1400 | Max payload per packet (bytes) |
| `NPS_WINDOW_SIZE` | 64 | Sliding window size |
| `NPS_RTO_INIT_MS` | 200 | Initial retransmission timeout (ms) |
| `NPS_CWND_INIT` | 2 | Initial congestion window |
| `NPS_MAX_RETRIES` | 8 | Max retransmit attempts |
| `NPS_CUBIC_BETA` | 0.7 | Multiplicative decrease factor |

## Project Layout

```
include/          header files (12 modules)
src/              source implementations (13 files)
tools/            test and benchmark tools (7 programs)
ebpf/             XDP/TC eBPF programs + loader (Linux only)
wireshark/        Lua protocol dissector
scripts/          automation scripts
```

See [ARCHITECTURE.md](ARCHITECTURE.md) for design details.

## Requirements

- **Core**: GCC/Clang, C11, POSIX (macOS & Linux)
- **eBPF**: Linux 5.4+, clang, libbpf, bpftool, CAP_BPF
- **Wireshark**: 3.x+ with Lua support
