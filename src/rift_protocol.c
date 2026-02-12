/* rift_protocol.c — serialize/deserialize, timestamps, packet builder */

#include "rift_protocol.h"
#include "rift_crc32.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

/* ── Timestamp ────────────────────────────────────────────────────── */

uint64_t rift_timestamp_us(void) {
#ifdef __APPLE__
  static mach_timebase_info_data_t timebase;
  if (timebase.denom == 0) {
    mach_timebase_info(&timebase);
  }
  uint64_t ns = mach_absolute_time() * timebase.numer / timebase.denom;
  return ns / 1000;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
#endif
}

/* ── Helper: write/read integers in network byte order ────────────── */

static inline void write_u16(uint8_t *buf, uint16_t val) {
  val = htons(val);
  memcpy(buf, &val, 2);
}
static inline void write_u32(uint8_t *buf, uint32_t val) {
  val = htonl(val);
  memcpy(buf, &val, 4);
}
static inline void write_u64(uint8_t *buf, uint64_t val) {
  uint32_t hi = htonl((uint32_t)(val >> 32));
  uint32_t lo = htonl((uint32_t)(val & 0xFFFFFFFF));
  memcpy(buf, &hi, 4);
  memcpy(buf + 4, &lo, 4);
}

static inline uint16_t read_u16(const uint8_t *buf) {
  uint16_t val;
  memcpy(&val, buf, 2);
  return ntohs(val);
}
static inline uint32_t read_u32(const uint8_t *buf) {
  uint32_t val;
  memcpy(&val, buf, 4);
  return ntohl(val);
}
static inline uint64_t read_u64(const uint8_t *buf) {
  uint32_t hi, lo;
  memcpy(&hi, buf, 4);
  memcpy(&lo, buf + 4, 4);
  return ((uint64_t)ntohl(hi) << 32) | (uint64_t)ntohl(lo);
}

/* ── Packet Serialization ─────────────────────────────────────────── */

int rift_packet_serialize(const rift_packet_t *pkt, uint8_t *buf,
                         size_t buf_len) {
  uint16_t sack_count = pkt->header.sack_count;
  if (sack_count > RIFT_MAX_SACK_BLOCKS)
    sack_count = RIFT_MAX_SACK_BLOCKS;

  uint16_t payload_len = pkt->header.payload_len;
  if (payload_len > RIFT_MAX_PAYLOAD)
    return -1;

  size_t sack_size = sack_count * sizeof(rift_sack_block_t);
  size_t total = RIFT_HEADER_SIZE + sack_size + 4 /* CRC32 */ + payload_len;

  if (buf_len < total)
    return -1;

  uint8_t *p = buf;

  /* Header fields */
  *p++ = pkt->header.version;
  *p++ = pkt->header.type;
  write_u16(p, pkt->header.flags);
  p += 2;
  write_u32(p, pkt->header.seq_num);
  p += 4;
  write_u32(p, pkt->header.ack_num);
  p += 4;
  write_u16(p, pkt->header.window_size);
  p += 2;
  write_u16(p, payload_len);
  p += 2;
  write_u64(p, pkt->header.ts_send);
  p += 8;
  write_u64(p, pkt->header.ts_echo);
  p += 8;
  write_u16(p, sack_count);
  p += 2;
  write_u16(p, pkt->header.conn_id);
  p += 2;

  /* SACK blocks */
  for (uint16_t i = 0; i < sack_count; i++) {
    write_u32(p, pkt->sack_blocks[i].start_seq);
    p += 4;
    write_u32(p, pkt->sack_blocks[i].end_seq);
    p += 4;
  }

  /* Placeholder for CRC (will be filled after payload) */
  uint8_t *crc_pos = p;
  p += 4;

  /* Payload */
  if (payload_len > 0) {
    memcpy(p, pkt->payload, payload_len);
    p += payload_len;
  }

  /* Compute CRC32 over everything except the CRC field itself */
  rift_crc32_init();
  uint32_t crc = RIFT_CRC32_INIT;
  /* CRC over header + sack blocks */
  crc = rift_crc32_update(crc, buf, (size_t)(crc_pos - buf));
  /* CRC over payload */
  if (payload_len > 0) {
    crc = rift_crc32_update(crc, crc_pos + 4, payload_len);
  }
  crc = rift_crc32_finalize(crc);
  write_u32(crc_pos, crc);

  return (int)total;
}

/* ── Packet Deserialization ───────────────────────────────────────── */

int rift_packet_deserialize(const uint8_t *buf, size_t buf_len,
                           rift_packet_t *pkt) {
  if (buf_len < RIFT_HEADER_SIZE + 4) /* Minimum: header + CRC */
    return -1;

  memset(pkt, 0, sizeof(*pkt));
  const uint8_t *p = buf;

  /* Header */
  pkt->header.version = *p++;
  pkt->header.type = *p++;
  pkt->header.flags = read_u16(p);
  p += 2;
  pkt->header.seq_num = read_u32(p);
  p += 4;
  pkt->header.ack_num = read_u32(p);
  p += 4;
  pkt->header.window_size = read_u16(p);
  p += 2;
  pkt->header.payload_len = read_u16(p);
  p += 2;
  pkt->header.ts_send = read_u64(p);
  p += 8;
  pkt->header.ts_echo = read_u64(p);
  p += 8;
  pkt->header.sack_count = read_u16(p);
  p += 2;
  pkt->header.conn_id = read_u16(p);
  p += 2;

  /* Validate version */
  if (pkt->header.version != RIFT_VERSION)
    return -1;

  /* Validate SACK count */
  if (pkt->header.sack_count > RIFT_MAX_SACK_BLOCKS)
    return -1;

  /* Validate payload length */
  if (pkt->header.payload_len > RIFT_MAX_PAYLOAD)
    return -1;

  size_t sack_size = pkt->header.sack_count * sizeof(rift_sack_block_t);
  size_t expected = RIFT_HEADER_SIZE + sack_size + 4 + pkt->header.payload_len;
  if (buf_len < expected)
    return -1;

  /* SACK blocks */
  for (uint16_t i = 0; i < pkt->header.sack_count; i++) {
    pkt->sack_blocks[i].start_seq = read_u32(p);
    p += 4;
    pkt->sack_blocks[i].end_seq = read_u32(p);
    p += 4;
  }

  /* CRC32 */
  pkt->checksum = read_u32(p);
  const uint8_t *crc_pos = p;
  p += 4;

  /* Payload */
  if (pkt->header.payload_len > 0) {
    memcpy(pkt->payload, p, pkt->header.payload_len);
  }

  /* Verify CRC32 */
  rift_crc32_init();
  uint32_t crc = RIFT_CRC32_INIT;
  crc = rift_crc32_update(crc, buf, (size_t)(crc_pos - buf));
  if (pkt->header.payload_len > 0) {
    crc = rift_crc32_update(crc, crc_pos + 4, pkt->header.payload_len);
  }
  crc = rift_crc32_finalize(crc);

  if (crc != pkt->checksum)
    return -1;

  return 0;
}

/* ── Packet Builder ───────────────────────────────────────────────── */

void rift_packet_build(rift_packet_t *pkt, uint8_t type, uint16_t flags,
                      uint32_t seq, uint32_t ack, uint16_t window,
                      uint16_t conn_id, const rift_sack_block_t *sack_blocks,
                      uint16_t sack_count, const uint8_t *payload,
                      uint16_t payload_len) {
  memset(pkt, 0, sizeof(*pkt));

  pkt->header.version = RIFT_VERSION;
  pkt->header.type = type;
  pkt->header.flags = flags;
  pkt->header.seq_num = seq;
  pkt->header.ack_num = ack;
  pkt->header.window_size = window;
  pkt->header.conn_id = conn_id;
  pkt->header.ts_send = rift_timestamp_us();
  pkt->header.ts_echo = 0;
  pkt->header.payload_len = payload_len;

  if (sack_blocks && sack_count > 0) {
    if (sack_count > RIFT_MAX_SACK_BLOCKS)
      sack_count = RIFT_MAX_SACK_BLOCKS;
    pkt->header.sack_count = sack_count;
    memcpy(pkt->sack_blocks, sack_blocks,
           sack_count * sizeof(rift_sack_block_t));
  }

  if (payload && payload_len > 0) {
    if (payload_len > RIFT_MAX_PAYLOAD)
      payload_len = RIFT_MAX_PAYLOAD;
    pkt->header.payload_len = payload_len;
    memcpy(pkt->payload, payload, payload_len);
  }
}

/* ── String Helpers ───────────────────────────────────────────────── */

const char *rift_pkt_type_str(uint8_t type) {
  switch (type) {
  case RIFT_PKT_DATA:
    return "DATA";
  case RIFT_PKT_ACK:
    return "ACK";
  case RIFT_PKT_SACK:
    return "SACK";
  case RIFT_PKT_NACK:
    return "NACK";
  case RIFT_PKT_SYN:
    return "SYN";
  case RIFT_PKT_SYN_ACK:
    return "SYN_ACK";
  case RIFT_PKT_FIN:
    return "FIN";
  case RIFT_PKT_FIN_ACK:
    return "FIN_ACK";
  case RIFT_PKT_RST:
    return "RST";
  case RIFT_PKT_PING:
    return "PING";
  case RIFT_PKT_PONG:
    return "PONG";
  default:
    return "UNKNOWN";
  }
}

const char *rift_conn_state_str(rift_conn_state_t state) {
  switch (state) {
  case RIFT_STATE_CLOSED:
    return "CLOSED";
  case RIFT_STATE_LISTEN:
    return "LISTEN";
  case RIFT_STATE_SYN_SENT:
    return "SYN_SENT";
  case RIFT_STATE_SYN_RCVD:
    return "SYN_RCVD";
  case RIFT_STATE_ESTABLISHED:
    return "ESTABLISHED";
  case RIFT_STATE_FIN_WAIT:
    return "FIN_WAIT";
  case RIFT_STATE_CLOSE_WAIT:
    return "CLOSE_WAIT";
  case RIFT_STATE_LAST_ACK:
    return "LAST_ACK";
  case RIFT_STATE_TIME_WAIT:
    return "TIME_WAIT";
  default:
    return "UNKNOWN";
  }
}
