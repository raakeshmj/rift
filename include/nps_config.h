/* nps_config.h — protocol configuration constants */

#ifndef NPS_CONFIG_H
#define NPS_CONFIG_H

/* ── Protocol Version ─────────────────────────────────────────────── */
#define NPS_VERSION 1

/* ── Network Defaults ─────────────────────────────────────────────── */
#define NPS_DEFAULT_PORT 9999
#define NPS_MAX_PAYLOAD 1400     /* Max payload bytes per packet      */
#define NPS_MAX_PACKET_SIZE 1500 /* Max total packet size (MTU)       */
#define NPS_MAX_SACK_BLOCKS 4    /* Max SACK blocks per ACK           */

/* ── Sliding Window ───────────────────────────────────────────────── */
#define NPS_WINDOW_SIZE 64      /* Default sliding window size       */
#define NPS_MAX_WINDOW_SIZE 256 /* Maximum configurable window       */
#define NPS_MIN_WINDOW_SIZE 1   /* Minimum window                    */

/* ── Retransmission ───────────────────────────────────────────────── */
#define NPS_MAX_RETRIES 8    /* Max retransmit attempts           */
#define NPS_RTO_INIT_MS 200  /* Initial RTO in milliseconds       */
#define NPS_RTO_MIN_MS 50    /* Minimum RTO                       */
#define NPS_RTO_MAX_MS 60000 /* Maximum RTO (60 seconds)          */
#define NPS_BACKOFF_FACTOR 2 /* Exponential backoff multiplier    */

/* ── RTT Estimation (Jacobson/Karels) ─────────────────────────────── */
#define NPS_RTT_ALPHA 0.125 /* SRTT smoothing factor (1/8)       */
#define NPS_RTT_BETA 0.25   /* RTTVAR smoothing factor (1/4)     */
#define NPS_RTT_K 4         /* RTO = SRTT + K * RTTVAR           */

/* ── Congestion Control (Cubic-inspired) ──────────────────────────── */
#define NPS_CUBIC_C 0.4          /* Cubic scaling constant            */
#define NPS_CUBIC_BETA 0.7       /* Multiplicative decrease factor    */
#define NPS_SLOW_START_THRESH 32 /* Initial ssthresh                  */
#define NPS_CWND_INIT 2          /* Initial congestion window         */
#define NPS_DUP_ACK_THRESH 3     /* Fast retransmit threshold         */

/* ── Buffer Sizes ─────────────────────────────────────────────────── */
#define NPS_SEND_BUFFER_SIZE 256 /* Sender ring buffer slots          */
#define NPS_RECV_BUFFER_SIZE 256 /* Receiver ring buffer slots        */
#define NPS_MAX_CONNECTIONS 64   /* Max simultaneous connections       */

/* ── Timeouts ─────────────────────────────────────────────────────── */
#define NPS_CONNECT_TIMEOUT_MS 5000 /* Connection handshake timeout      */
#define NPS_KEEPALIVE_MS 10000      /* Keepalive interval                */
#define NPS_CLOSE_TIMEOUT_MS 5000   /* Graceful close timeout            */
#define NPS_ACK_DELAY_MS 10         /* Delayed ACK timer                 */

/* ── Stats & Monitoring ───────────────────────────────────────────── */
#define NPS_STATS_INTERVAL_MS 1000 /* Stats reporting interval          */
#define NPS_THROUGHPUT_WINDOW 10   /* Sliding window for throughput avg  */

/* ── Logging ──────────────────────────────────────────────────────── */
#define NPS_LOG_MAX_MSG_LEN 512 /* Max log message length            */
#define NPS_LOG_FILE_PATH "/tmp/nps.log"

/* ── eBPF / BPF Map Paths ─────────────────────────────────────────── */
#define NPS_BPF_PIN_PATH "/sys/fs/bpf/nps"
#define NPS_BPF_MAX_RULES 1024
#define NPS_BPF_RATE_LIMIT_PPS 10000 /* Default packets/sec limit         */
#define NPS_BPF_RATE_LIMIT_BPS (100 * 1024 * 1024) /* 100 Mbps default    */

#endif /* NPS_CONFIG_H */
