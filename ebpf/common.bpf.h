/* common.bpf.h — shared BPF definitions for RIFT eBPF programs */

#ifndef RIFT_COMMON_BPF_H
#define RIFT_COMMON_BPF_H

#include <stdint.h>

/* ── Filter Rule Key ──────────────────────────────────────────────── */
struct rift_filter_key {
  uint32_t src_ip;   /* Source IP (network byte order)     */
  uint32_t dst_ip;   /* Destination IP (network byte order) */
  uint16_t src_port; /* Source port (network byte order)   */
  uint16_t dst_port; /* Destination port (network byte order) */
  uint8_t protocol;  /* IP protocol (IPPROTO_UDP/TCP)     */
  uint8_t pad[3];    /* Alignment padding                 */
};

/* ── Filter Rule Action ───────────────────────────────────────────── */
enum rift_filter_action {
  RIFT_ACTION_PASS = 0,  /* Allow packet                      */
  RIFT_ACTION_DROP = 1,  /* Drop packet                       */
  RIFT_ACTION_LIMIT = 2, /* Rate limit                        */
};

/* ── Filter Rule Value ────────────────────────────────────────────── */
struct rift_filter_rule {
  uint32_t action;     /* enum rift_filter_action             */
  uint64_t rate_pps;   /* Packets per second limit           */
  uint64_t rate_bps;   /* Bytes per second limit             */
  uint64_t created_ns; /* Rule creation timestamp            */
};

/* ── Connection Stats ─────────────────────────────────────────────── */
struct rift_conn_stats {
  uint64_t packets;         /* Total packets seen                 */
  uint64_t bytes;           /* Total bytes seen                   */
  uint64_t packets_passed;  /* Packets that passed filter         */
  uint64_t packets_dropped; /* Packets dropped                    */
  uint64_t first_seen_ns;   /* First packet timestamp             */
  uint64_t last_seen_ns;    /* Latest packet timestamp            */
};

/* ── Rate Limiter State ───────────────────────────────────────────── */
struct rift_rate_state {
  uint64_t tokens;         /* Available tokens                   */
  uint64_t last_update_ns; /* Last token refill time             */
  uint64_t max_tokens;     /* Token bucket capacity              */
  uint64_t refill_rate;    /* Tokens per second                  */
};

/* ── Global Statistics (per-CPU array index 0) ────────────────────── */
struct rift_global_stats {
  uint64_t total_packets;
  uint64_t total_bytes;
  uint64_t total_passed;
  uint64_t total_dropped;
  uint64_t total_rate_limited;
  uint64_t rift_protocol_packets; /* Packets matching RIFT port    */
};

/* ── Map Names (for pinning) ──────────────────────────────────────── */
#define RIFT_MAP_FILTER_RULES "rift_filter_rules"
#define RIFT_MAP_CONN_STATS "rift_conn_stats"
#define RIFT_MAP_RATE_STATE "rift_rate_state"
#define RIFT_MAP_GLOBAL_STATS "rift_global_stats"

/* ── Constants ────────────────────────────────────────────────────── */
#define RIFT_MAX_FILTER_RULES 1024
#define RIFT_MAX_CONNECTIONS 65536
#define RIFT_DEFAULT_PORT_NBO __builtin_bswap16(9999)

/* Token bucket constants */
#define RIFT_TOKEN_BUCKET_SIZE 10000 /* Max burst */
#define RIFT_TOKENS_PER_SEC 10000    /* Default rate */
#define RIFT_NS_PER_SEC 1000000000ULL

#endif /* RIFT_COMMON_BPF_H */
