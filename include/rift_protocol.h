/* rift_protocol.h — packet format and protocol types */

#ifndef RIFT_PROTOCOL_H
#define RIFT_PROTOCOL_H

#include <arpa/inet.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rift_config.h"

/* ── Packet Types ─────────────────────────────────────────────────── */
typedef enum {
  RIFT_PKT_DATA = 0x01,
  RIFT_PKT_ACK = 0x02,
  RIFT_PKT_SACK = 0x03,
  RIFT_PKT_NACK = 0x04,
  RIFT_PKT_SYN = 0x10,
  RIFT_PKT_SYN_ACK = 0x11,
  RIFT_PKT_FIN = 0x20,
  RIFT_PKT_FIN_ACK = 0x21,
  RIFT_PKT_RST = 0x30,
  RIFT_PKT_PING = 0x40,
  RIFT_PKT_PONG = 0x41,
} rift_pkt_type_t;

/* ── Packet Flags (bitmask) ───────────────────────────────────────── */
#define RIFT_FLAG_SYN (1 << 0)
#define RIFT_FLAG_FIN (1 << 1)
#define RIFT_FLAG_ACK (1 << 2)
#define RIFT_FLAG_SACK (1 << 3)
#define RIFT_FLAG_NACK (1 << 4)
#define RIFT_FLAG_RST (1 << 5)
#define RIFT_FLAG_URG (1 << 6)
#define RIFT_FLAG_ENC (1 << 7) /* ChaCha20 encrypted payload */

/* ── Connection States ────────────────────────────────────────────── */
typedef enum {
  RIFT_STATE_CLOSED,
  RIFT_STATE_LISTEN,
  RIFT_STATE_SYN_SENT,
  RIFT_STATE_SYN_RCVD,
  RIFT_STATE_ESTABLISHED,
  RIFT_STATE_FIN_WAIT,
  RIFT_STATE_CLOSE_WAIT,
  RIFT_STATE_LAST_ACK,
  RIFT_STATE_TIME_WAIT,
} rift_conn_state_t;

/* ── SACK Block ───────────────────────────────────────────────────── */
typedef struct {
  uint32_t start_seq; /* First sequence number in block  */
  uint32_t end_seq;   /* Last sequence number in block +1 */
} __attribute__((packed)) rift_sack_block_t;

/* ── Packet Header (fixed portion) ────────────────────────────────── */
/*
 * Wire format (network byte order):
 *   version(1) | type(1) | flags(2) | seq(4) | ack(4) |
 *   window(2) | payload_len(2) | ts_send(8) | ts_echo(8) |
 *   sack_count(2) | conn_id(2)
 *   Total fixed header: 36 bytes
 */
typedef struct {
  uint8_t version;      /* Protocol version                  */
  uint8_t type;         /* Packet type (rift_pkt_type_t)      */
  uint16_t flags;       /* Packet flags bitmask              */
  uint32_t seq_num;     /* Sequence number                   */
  uint32_t ack_num;     /* Acknowledgment number             */
  uint16_t window_size; /* Receiver window advertisement     */
  uint16_t payload_len; /* Payload length in bytes           */
  uint64_t ts_send;     /* Send timestamp (microseconds)     */
  uint64_t ts_echo;     /* Echoed timestamp                  */
  uint16_t sack_count;  /* Number of SACK blocks             */
  uint16_t conn_id;     /* Connection identifier             */
} __attribute__((packed)) rift_header_t;

#define RIFT_HEADER_SIZE sizeof(rift_header_t) /* 36 bytes */

/* ── Full Packet ──────────────────────────────────────────────────── */
typedef struct {
  rift_header_t header;
  rift_sack_block_t sack_blocks[RIFT_MAX_SACK_BLOCKS]; /* Variable */
  uint32_t checksum;                                 /* CRC32    */
  uint8_t payload[RIFT_MAX_PAYLOAD];
} rift_packet_t;

/* Maximum serialized size (header + max sack + checksum + max payload) */
#define RIFT_MAX_SERIALIZED_SIZE                                                \
  (RIFT_HEADER_SIZE + (RIFT_MAX_SACK_BLOCKS * sizeof(rift_sack_block_t)) +        \
   sizeof(uint32_t) + RIFT_MAX_PAYLOAD)

/* ── Packet Serialization / Deserialization ────────────────────────── */

/*
 * Serialize a packet into a network-byte-order wire buffer.
 * Returns the number of bytes written, or -1 on error.
 */
int rift_packet_serialize(const rift_packet_t *pkt, uint8_t *buf, size_t buf_len);

/*
 * Deserialize a wire buffer into a packet structure.
 * Returns 0 on success, -1 on error (malformed packet).
 */
int rift_packet_deserialize(const uint8_t *buf, size_t buf_len,
                           rift_packet_t *pkt);

/*
 * Build a packet with the given parameters.
 * sack_blocks can be NULL if sack_count is 0.
 * payload can be NULL if payload_len is 0.
 */
void rift_packet_build(rift_packet_t *pkt, uint8_t type, uint16_t flags,
                      uint32_t seq, uint32_t ack, uint16_t window,
                      uint16_t conn_id, const rift_sack_block_t *sack_blocks,
                      uint16_t sack_count, const uint8_t *payload,
                      uint16_t payload_len);

/*
 * Get the current timestamp in microseconds (monotonic clock).
 */
uint64_t rift_timestamp_us(void);

/*
 * Return a human-readable string for a packet type.
 */
const char *rift_pkt_type_str(uint8_t type);

/*
 * Return a human-readable string for a connection state.
 */
const char *rift_conn_state_str(rift_conn_state_t state);

/* ── Transfer Result Stats ────────────────────────────────────────── */
typedef struct {
  uint64_t elapsed_us;
  uint64_t bytes_transferred;
  uint64_t packets_sent;
  uint64_t packets_lost;
  uint64_t packets_retransmitted;
  double throughput_mbps;
  double goodput_mbps;

  /* RTT Stats */
  double min_rtt_ms;
  double max_rtt_ms;
  double avg_srtt_ms;

  /* Congestion */
  uint32_t final_cwnd;
  uint32_t max_cwnd;
} rift_transfer_result_t;

#endif /* RIFT_PROTOCOL_H */
