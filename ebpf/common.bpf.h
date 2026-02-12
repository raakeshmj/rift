/* common.bpf.h — shared BPF definitions for NPS eBPF programs */

#ifndef NPS_COMMON_BPF_H
#define NPS_COMMON_BPF_H

#include <stdint.h>

/* ── Filter Rule Key ──────────────────────────────────────────────── */
struct nps_filter_key {
  uint32_t src_ip;   /* Source IP (network byte order)     */
  uint32_t dst_ip;   /* Destination IP (network byte order) */
  uint16_t src_port; /* Source port (network byte order)   */
  uint16_t dst_port; /* Destination port (network byte order) */
  uint8_t protocol;  /* IP protocol (IPPROTO_UDP/TCP)     */
  uint8_t pad[3];    /* Alignment padding                 */
};

/* ── Filter Rule Action ───────────────────────────────────────────── */
enum nps_filter_action {
  NPS_ACTION_PASS = 0,  /* Allow packet                      */
  NPS_ACTION_DROP = 1,  /* Drop packet                       */
  NPS_ACTION_LIMIT = 2, /* Rate limit                        */
};

/* ── Filter Rule Value ────────────────────────────────────────────── */
struct nps_filter_rule {
  uint32_t action;     /* enum nps_filter_action             */
  uint64_t rate_pps;   /* Packets per second limit           */
  uint64_t rate_bps;   /* Bytes per second limit             */
  uint64_t created_ns; /* Rule creation timestamp            */
};

/* ── Connection Stats ─────────────────────────────────────────────── */
struct nps_conn_stats {
  uint64_t packets;         /* Total packets seen                 */
  uint64_t bytes;           /* Total bytes seen                   */
  uint64_t packets_passed;  /* Packets that passed filter         */
  uint64_t packets_dropped; /* Packets dropped                    */
  uint64_t first_seen_ns;   /* First packet timestamp             */
  uint64_t last_seen_ns;    /* Latest packet timestamp            */
};

/* ── Rate Limiter State ───────────────────────────────────────────── */
struct nps_rate_state {
  uint64_t tokens;         /* Available tokens                   */
  uint64_t last_update_ns; /* Last token refill time             */
  uint64_t max_tokens;     /* Token bucket capacity              */
  uint64_t refill_rate;    /* Tokens per second                  */
};

/* ── Global Statistics (per-CPU array index 0) ────────────────────── */
struct nps_global_stats {
  uint64_t total_packets;
  uint64_t total_bytes;
  uint64_t total_passed;
  uint64_t total_dropped;
  uint64_t total_rate_limited;
  uint64_t nps_protocol_packets; /* Packets matching NPS port    */
};

/* ── Map Names (for pinning) ──────────────────────────────────────── */
#define NPS_MAP_FILTER_RULES "nps_filter_rules"
#define NPS_MAP_CONN_STATS "nps_conn_stats"
#define NPS_MAP_RATE_STATE "nps_rate_state"
#define NPS_MAP_GLOBAL_STATS "nps_global_stats"

/* ── Constants ────────────────────────────────────────────────────── */
#define NPS_MAX_FILTER_RULES 1024
#define NPS_MAX_CONNECTIONS 65536
#define NPS_DEFAULT_PORT_NBO __builtin_bswap16(9999)

/* Token bucket constants */
#define NPS_TOKEN_BUCKET_SIZE 10000 /* Max burst */
#define NPS_TOKENS_PER_SEC 10000    /* Default rate */
#define NPS_NS_PER_SEC 1000000000ULL

#endif /* NPS_COMMON_BPF_H */
