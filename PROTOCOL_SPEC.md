# RIFT Protocol Specification

Version: 1.0

## 1. Overview

RIFT (Network Protocol Stack) is a custom reliable transport protocol built on top of UDP. It provides:

- Reliable, ordered delivery of data streams
- Selective Acknowledgment (SACK) for gap recovery
- Congestion control via TCP Cubic-inspired algorithm
- CRC32 integrity verification on every packet

## 2. Packet Format

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|    Version    |     Type      |            Flags              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       Sequence Number                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Acknowledgment Number                      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|         Window Size           |        Payload Length          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
|                     Send Timestamp (64-bit)                   |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
|                     Echo Timestamp (64-bit)                   |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|        SACK Count             |        Connection ID          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    SACK Block 0 [Start]                       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    SACK Block 0 [End]                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    ... up to 3 more SACK blocks ...           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      CRC32 Checksum                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
|                        Payload Data                           |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

**Header size**: 36 bytes (fixed) + 8 bytes per SACK block + 4 bytes CRC32

## 3. Field Definitions

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | Version | Protocol version (currently 1) |
| 1 | 1 | Type | Packet type (see §4) |
| 2 | 2 | Flags | Bitfield flags (see §5) |
| 4 | 4 | Sequence Number | Sender sequence number |
| 8 | 4 | Acknowledgment Number | Next expected sequence number |
| 12 | 2 | Window Size | Receiver's advertised window |
| 14 | 2 | Payload Length | Length of payload data in bytes |
| 16 | 8 | Send Timestamp | Sender's monotonic clock (microseconds) |
| 24 | 8 | Echo Timestamp | Echoed timestamp for RTT calculation |
| 32 | 2 | SACK Count | Number of SACK blocks (0-4) |
| 34 | 2 | Connection ID | Connection identifier |
| 36 | 8×N | SACK Blocks | Variable-length SACK block array |
| 36+8N | 4 | CRC32 | CRC32 over header + SACK + payload |
| 40+8N | P | Payload | Application data |

## 4. Packet Types

| Value | Name | Description |
|-------|------|-------------|
| 0x01 | DATA | Data-carrying packet |
| 0x02 | ACK | Cumulative acknowledgment |
| 0x03 | SACK | Selective acknowledgment with SACK blocks |
| 0x04 | NACK | Negative acknowledgment (request retransmit) |
| 0x10 | SYN | Connection initiation |
| 0x11 | SYN_ACK | Connection initiation response |
| 0x20 | FIN | Connection termination |
| 0x21 | FIN_ACK | Termination acknowledgment |
| 0x30 | RST | Connection reset |
| 0x40 | PING | Keepalive probe |
| 0x41 | PONG | Keepalive response |

## 5. Flags

| Bit | Name | Description |
|-----|------|-------------|
| 0 | SYN | Synchronize sequence numbers |
| 1 | FIN | Sender has finished sending |
| 2 | ACK | Acknowledgment number is valid |
| 3 | SACK | SACK blocks are present |
| 4 | NACK | Negative acknowledgment |
| 5 | RST | Reset the connection |
| 6 | URG | Urgent data present |

## 6. Connection Lifecycle

### 6.1 Establishment (3-Way Handshake)

```
Client                          Server
  |                               |
  |  SYN (seq=x)                  |
  |------------------------------>|  LISTEN → SYN_RCVD
  |                               |
  |  SYN_ACK (seq=y, ack=x+1)    |
  |<------------------------------|
  |  SYN_SENT → ESTABLISHED      |
  |                               |
  |  ACK (ack=y+1)                |
  |------------------------------>|  SYN_RCVD → ESTABLISHED
  |                               |
```

### 6.2 Data Transfer

```
Sender                          Receiver
  |  DATA (seq=1, payload)        |
  |------------------------------>|
  |  DATA (seq=2, payload)        |
  |------------------------------>|
  |                               |
  |  ACK (ack=3)                  |  ← cumulative ACK
  |<------------------------------|
  |                               |
  |  DATA (seq=3, payload)        |
  |------------------------------>|
  |  DATA (seq=5, payload)        |  ← seq 4 lost
  |------------------------------>|
  |                               |
  |  SACK (ack=4, SACK[5,6])     |  ← gap signaled via SACK
  |<------------------------------|
  |                               |
  |  DATA (seq=4, payload)        |  ← retransmission
  |------------------------------>|
  |                               |
```

### 6.3 Termination

```
Client                          Server
  |  FIN (seq=n)                  |
  |------------------------------>|  ESTABLISHED → CLOSE_WAIT
  |                               |
  |  FIN_ACK (ack=n+1)           |
  |<------------------------------|  CLOSE_WAIT → CLOSED
  |  FIN_WAIT → CLOSED           |
  |                               |
```

## 7. Congestion Control

RIFT implements a TCP Cubic-inspired congestion algorithm:

| Phase | Behavior |
|-------|----------|
| Slow Start | cwnd grows by 1 per ACK (exponential) until ssthresh |
| Congestion Avoidance | Cubic growth function: W(t) = C(t-K)³ + W_max |
| Fast Recovery | On triple dup ACK: ssthresh = cwnd × 0.7, cwnd = ssthresh |
| Timeout | ssthresh = cwnd / 2, cwnd = 1 (back to slow start) |

**Constants**: C = 0.4, β = 0.7

## 8. RTT Estimation

Per RFC 6298 (Jacobson/Karels):

```
SRTT    = (1 - α) × SRTT + α × RTT       α = 1/8
RTTVAR  = (1 - β) × RTTVAR + β × |SRTT - RTT|  β = 1/4
RTO     = SRTT + 4 × RTTVAR
RTO     = max(RTO_MIN, min(RTO, RTO_MAX))
```

Exponential backoff on timeout: RTO = RTO × 2 (capped at 60 seconds).

## 9. CRC32 Checksum

- Algorithm: CRC32 (ISO 3309 / ITU-T V.42)
- Polynomial: 0xEDB88320 (reversed)
- Compatible with zlib `crc32()` and Wireshark
- Computed over: header + SACK blocks + payload (excluding the checksum field itself)

## 10. Byte Order

All multi-byte fields are transmitted in **network byte order** (big-endian).

## 11. Maximum Sizes

| Parameter | Value |
|-----------|-------|
| Max payload | 1400 bytes |
| Max SACK blocks | 4 |
| Max header size | 36 + 32 + 4 = 72 bytes |
| Max packet size | 72 + 1400 = 1472 bytes |
| Max serialized size | 1500 bytes |
