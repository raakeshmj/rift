/* nps_protocol.h — packet format and protocol types */

#ifndef NPS_PROTOCOL_H
#define NPS_PROTOCOL_H

#include <arpa/inet.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nps_config.h"

/* ── Packet Types ─────────────────────────────────────────────────── */
typedef enum {
  NPS_PKT_DATA = 0x01,
  NPS_PKT_ACK = 0x02,
  NPS_PKT_SACK = 0x03,
  NPS_PKT_NACK = 0x04,
  NPS_PKT_SYN = 0x10,
  NPS_PKT_SYN_ACK = 0x11,
  NPS_PKT_FIN = 0x20,
  NPS_PKT_FIN_ACK = 0x21,
  NPS_PKT_RST = 0x30,
  NPS_PKT_PING = 0x40,
  NPS_PKT_PONG = 0x41,
} nps_pkt_type_t;

/* ── Packet Flags (bitmask) ───────────────────────────────────────── */
#define NPS_FLAG_SYN (1 << 0)
#define NPS_FLAG_FIN (1 << 1)
#define NPS_FLAG_ACK (1 << 2)
#define NPS_FLAG_SACK (1 << 3)
#define NPS_FLAG_NACK (1 << 4)
#define NPS_FLAG_RST (1 << 5)
#define NPS_FLAG_URG (1 << 6)
#define NPS_FLAG_ENC (1 << 7) /* ChaCha20 encrypted payload */

/* ── Connection States ────────────────────────────────────────────── */
typedef enum {
  NPS_STATE_CLOSED,
  NPS_STATE_LISTEN,
  NPS_STATE_SYN_SENT,
  NPS_STATE_SYN_RCVD,
  NPS_STATE_ESTABLISHED,
  NPS_STATE_FIN_WAIT,
  NPS_STATE_CLOSE_WAIT,
  NPS_STATE_LAST_ACK,
  NPS_STATE_TIME_WAIT,
} nps_conn_state_t;

/* ── SACK Block ───────────────────────────────────────────────────── */
typedef struct {
  uint32_t start_seq; /* First sequence number in block  */
  uint32_t end_seq;   /* Last sequence number in block +1 */
} __attribute__((packed)) nps_sack_block_t;

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
  uint8_t type;         /* Packet type (nps_pkt_type_t)      */
  uint16_t flags;       /* Packet flags bitmask              */
  uint32_t seq_num;     /* Sequence number                   */
  uint32_t ack_num;     /* Acknowledgment number             */
  uint16_t window_size; /* Receiver window advertisement     */
  uint16_t payload_len; /* Payload length in bytes           */
  uint64_t ts_send;     /* Send timestamp (microseconds)     */
  uint64_t ts_echo;     /* Echoed timestamp                  */
  uint16_t sack_count;  /* Number of SACK blocks             */
  uint16_t conn_id;     /* Connection identifier             */
} __attribute__((packed)) nps_header_t;

#define NPS_HEADER_SIZE sizeof(nps_header_t) /* 36 bytes */

/* ── Full Packet ──────────────────────────────────────────────────── */
typedef struct {
  nps_header_t header;
  nps_sack_block_t sack_blocks[NPS_MAX_SACK_BLOCKS]; /* Variable */
  uint32_t checksum;                                 /* CRC32    */
  uint8_t payload[NPS_MAX_PAYLOAD];
} nps_packet_t;

/* Maximum serialized size (header + max sack + checksum + max payload) */
#define NPS_MAX_SERIALIZED_SIZE                                                \
  (NPS_HEADER_SIZE + (NPS_MAX_SACK_BLOCKS * sizeof(nps_sack_block_t)) +        \
   sizeof(uint32_t) + NPS_MAX_PAYLOAD)

/* ── Packet Serialization / Deserialization ────────────────────────── */

/*
 * Serialize a packet into a network-byte-order wire buffer.
 * Returns the number of bytes written, or -1 on error.
 */
int nps_packet_serialize(const nps_packet_t *pkt, uint8_t *buf, size_t buf_len);

/*
 * Deserialize a wire buffer into a packet structure.
 * Returns 0 on success, -1 on error (malformed packet).
 */
int nps_packet_deserialize(const uint8_t *buf, size_t buf_len,
                           nps_packet_t *pkt);

/*
 * Build a packet with the given parameters.
 * sack_blocks can be NULL if sack_count is 0.
 * payload can be NULL if payload_len is 0.
 */
void nps_packet_build(nps_packet_t *pkt, uint8_t type, uint16_t flags,
                      uint32_t seq, uint32_t ack, uint16_t window,
                      uint16_t conn_id, const nps_sack_block_t *sack_blocks,
                      uint16_t sack_count, const uint8_t *payload,
                      uint16_t payload_len);

/*
 * Get the current timestamp in microseconds (monotonic clock).
 */
uint64_t nps_timestamp_us(void);

/*
 * Return a human-readable string for a packet type.
 */
const char *nps_pkt_type_str(uint8_t type);

/*
 * Return a human-readable string for a connection state.
 */
const char *nps_conn_state_str(nps_conn_state_t state);

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
} nps_transfer_result_t;

#endif /* NPS_PROTOCOL_H */
