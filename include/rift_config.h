/* rift_config.h — protocol configuration constants */

#ifndef RIFT_CONFIG_H
#define RIFT_CONFIG_H

/* ── Protocol Version ─────────────────────────────────────────────── */
#define RIFT_VERSION 1

/* ── Network Defaults ─────────────────────────────────────────────── */
#define RIFT_DEFAULT_PORT 9999
#define RIFT_MAX_PAYLOAD 1400     /* Max payload bytes per packet      */
#define RIFT_MAX_PACKET_SIZE 1500 /* Max total packet size (MTU)       */
#define RIFT_MAX_SACK_BLOCKS 4    /* Max SACK blocks per ACK           */

/* ── Sliding Window ───────────────────────────────────────────────── */
#define RIFT_WINDOW_SIZE 64      /* Default sliding window size       */
#define RIFT_MAX_WINDOW_SIZE 256 /* Maximum configurable window       */
#define RIFT_MIN_WINDOW_SIZE 1   /* Minimum window                    */

/* ── Retransmission ───────────────────────────────────────────────── */
#define RIFT_MAX_RETRIES 8    /* Max retransmit attempts           */
#define RIFT_RTO_INIT_MS 200  /* Initial RTO in milliseconds       */
#define RIFT_RTO_MIN_MS 50    /* Minimum RTO                       */
#define RIFT_RTO_MAX_MS 60000 /* Maximum RTO (60 seconds)          */
#define RIFT_BACKOFF_FACTOR 2 /* Exponential backoff multiplier    */

/* ── RTT Estimation (Jacobson/Karels) ─────────────────────────────── */
#define RIFT_RTT_ALPHA 0.125 /* SRTT smoothing factor (1/8)       */
#define RIFT_RTT_BETA 0.25   /* RTTVAR smoothing factor (1/4)     */
#define RIFT_RTT_K 4         /* RTO = SRTT + K * RTTVAR           */

/* ── Congestion Control (Cubic-inspired) ──────────────────────────── */
#define RIFT_CUBIC_C 0.4          /* Cubic scaling constant            */
#define RIFT_CUBIC_BETA 0.7       /* Multiplicative decrease factor    */
#define RIFT_SLOW_START_THRESH 32 /* Initial ssthresh                  */
#define RIFT_CWND_INIT 2          /* Initial congestion window         */
#define RIFT_DUP_ACK_THRESH 3     /* Fast retransmit threshold         */

/* ── Buffer Sizes ─────────────────────────────────────────────────── */
#define RIFT_SEND_BUFFER_SIZE 256 /* Sender ring buffer slots          */
#define RIFT_RECV_BUFFER_SIZE 256 /* Receiver ring buffer slots        */
#define RIFT_MAX_CONNECTIONS 64   /* Max simultaneous connections       */

/* ── Timeouts ─────────────────────────────────────────────────────── */
#define RIFT_CONNECT_TIMEOUT_MS 5000 /* Connection handshake timeout      */
#define RIFT_KEEPALIVE_MS 10000      /* Keepalive interval                */
#define RIFT_CLOSE_TIMEOUT_MS 5000   /* Graceful close timeout            */
#define RIFT_ACK_DELAY_MS 10         /* Delayed ACK timer                 */

/* ── Stats & Monitoring ───────────────────────────────────────────── */
#define RIFT_STATS_INTERVAL_MS 1000 /* Stats reporting interval          */
#define RIFT_THROUGHPUT_WINDOW 10   /* Sliding window for throughput avg  */

/* ── Logging ──────────────────────────────────────────────────────── */
#define RIFT_LOG_MAX_MSG_LEN 512 /* Max log message length            */
#define RIFT_LOG_FILE_PATH "/tmp/rift.log"

/* ── eBPF / BPF Map Paths ─────────────────────────────────────────── */
#define RIFT_BPF_PIN_PATH "/sys/fs/bpf/rift"
#define RIFT_BPF_MAX_RULES 1024
#define RIFT_BPF_RATE_LIMIT_PPS 10000 /* Default packets/sec limit         */
#define RIFT_BPF_RATE_LIMIT_BPS (100 * 1024 * 1024) /* 100 Mbps default    */

#endif /* RIFT_CONFIG_H */
