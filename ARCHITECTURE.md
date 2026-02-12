# Architecture

RIFT is a reliable transport protocol built on UDP. It provides ordered,
congestion-controlled delivery with selective acknowledgements, payload
encryption, and optional eBPF-based packet filtering.

## Directory Layout

```
include/          Public headers (one per module)
src/              Library implementation → build/librift.a
tools/            Standalone binaries linked against librift
ebpf/             BPF programs (XDP + TC) and userspace loader
```

## Module Dependency Graph

```
┌─────────────┐
│  rift_config  │  Compile-time constants (MTU, window, timeouts, ports)
└──────┬───────┘
       │
 ┌─────▼──────┐     ┌────────────┐
 │ rift_protocol│────▶│  rift_crc32  │   Packet format, serialize/deserialize, CRC
 └─────┬───────┘     └────────────┘
       │
 ┌─────▼──────┐     ┌────────────┐
 │  rift_buffer │────▶│  rift_window │   Ring buffer → sliding window + SACK
 └─────┬───────┘     └────────────┘
       │
 ┌─────▼──────────┐  ┌──────────────┐  ┌──────────┐
 │ rift_congestion  │  │   rift_rtt    │  │ rift_stats │
 │ (TCP Cubic)     │  │ (RFC 6298)   │  │ (atomic)  │
 └─────┬───────────┘  └──────┬───────┘  └──────┬────┘
       │                     │                  │
 ┌─────▼─────────────────────▼──────────────────▼────┐
 │              rift_sender / rift_receiver              │
 │        Connection state machines over UDP           │
 └─────────────────────┬─────────────────────────────-─┘
                       │
             ┌─────────▼─────────┐
             │     rift_mux       │  Multiplexes N streams over 1 socket
             └───────────────────┘

 Cross-cutting:
   rift_log     Severity-filtered, colored, optional file output
   rift_trace   JSON Lines / columnar packet event logger
   rift_crypto  ChaCha20 payload encryption (RFC 8439)
```

## Key Design Decisions

### Packet Format
- Fixed 32-byte header: version, type, flags, sequence/ack numbers,
  connection ID, window advertisement, SACK block count, payload length,
  timestamps (send + echo), and CRC32 checksum.
- Up to 4 SACK blocks appended after the header, followed by payload data.
- All multi-byte fields use network byte order.

### Reliability
- **Sliding window** with configurable size (default 64 packets).
- **Selective ACK** (up to 4 blocks) avoids head-of-line retransmission.
- **RTO** computed via Jacobson/Karels (SRTT, RTTVAR, clamped between
  100 ms and 30 s, exponential backoff on timeout).
- **Duplicate ACK** counter triggers fast retransmit at 3 dup-ACKs.

### Congestion Control
- TCP Cubic: `W(t) = C·(t − K)³ + W_max`.
- Three phases: slow start → congestion avoidance → fast recovery.
- TCP-friendliness check ensures at least AIMD growth.

### Connection Lifecycle

```
Sender:  CLOSED → SYN_SENT → ESTABLISHED → FIN_SENT → CLOSED
Receiver: LISTEN → SYN_RCVD → ESTABLISHED → CLOSE_WAIT → CLOSED
```

Three-way handshake (SYN, SYN-ACK, ACK), then data transfer, then
FIN/FIN-ACK teardown. Retransmission applies to handshake and teardown
packets as well.

### Multiplexing
`rift_mux` maps connection IDs to independent stream contexts (window,
congestion, RTT, stats). A single UDP socket handles all streams,
dispatching inbound packets by `conn_id`.

### Encryption
Optional ChaCha20 (RFC 8439) with a 256-bit key and 96-bit nonce
derived from the connection ID and sequence number. Zero-copy XOR
over the payload buffer.

### eBPF Integration
- **XDP filter** (`xdp_filter.bpf.c`): ingress path, earliest possible
  drop/rate-limit.
- **TC filter** (`tc_filter.bpf.c`): egress path, outbound filtering.
- Shared BPF maps: per-connection stats, global counters, filter rules.
- Userspace tools: `loader.c` (attach/detach/pin) and `map_reader.c`
  (read stats, manage rules at runtime).

## Build Artifacts

`make` produces:

| Artifact | Description |
|---|---|
| `build/librift.a` | Static library (all `src/*.c` objects) |
| `build/rift_server` | Test receiver |
| `build/rift_client` | Test sender |
| `build/rift_bench` | Throughput/latency benchmark |
| `build/rift_losssim` | UDP loss simulator proxy |
| `build/rift_integration_test` | Unit + integration tests |
| `build/rift_stress_test` | Congestion recovery stress test |
| `build/rift_mux_test` | Multiplexed stream test |

## Threading Model

- **Sender**: single-threaded event loop using `poll()` with
  configurable RTO as the poll timeout. Sends new data when the window
  allows, processes incoming ACKs, retransmits on timeout.
- **Receiver**: single-threaded, blocks on `recvfrom()`, buffers
  out-of-order packets, sends cumulative ACK + SACK blocks.
- **Mux**: per-stream state is lock-free within a single thread; the
  mux struct itself uses a mutex for concurrent stream creation.
- **Logging / tracing**: global mutex per subsystem.
