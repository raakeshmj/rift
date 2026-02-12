/* rift_trace.h — structured packet event logger (JSON Lines + text) */

#ifndef RIFT_TRACE_H
#define RIFT_TRACE_H

#include <stdbool.h>
#include <stdint.h>

/* ── Trace Event Types ───────────────────────────────────────────── */
typedef enum {
  RIFT_TRACE_TX,      /* Packet transmitted                       */
  RIFT_TRACE_RX,      /* Packet received                          */
  RIFT_TRACE_ACK,     /* ACK sent/received                        */
  RIFT_TRACE_SACK,    /* SACK sent/received                       */
  RIFT_TRACE_RETX,    /* Packet retransmitted                     */
  RIFT_TRACE_DROP,    /* Packet dropped (CRC fail, etc.)          */
  RIFT_TRACE_CWND,    /* Congestion window changed                */
  RIFT_TRACE_RTT,     /* RTT measurement                          */
  RIFT_TRACE_STATE,   /* Connection state transition              */
  RIFT_TRACE_LOSS,    /* Loss detected                            */
  RIFT_TRACE_TIMEOUT, /* Retransmission timeout                   */
} rift_trace_event_t;

/* ── Output Format ───────────────────────────────────────────────── */
typedef enum {
  RIFT_TRACE_FMT_JSON = 1, /* JSON Lines (one object per line)     */
  RIFT_TRACE_FMT_TEXT = 2, /* Human-readable columnar              */
  RIFT_TRACE_FMT_BOTH = 3, /* Both formats to separate files       */
} rift_trace_format_t;

/* ── Trace Entry ─────────────────────────────────────────────────── */
typedef struct {
  uint64_t timestamp_us;   /* Monotonic microsecond timestamp   */
  rift_trace_event_t event; /* Event type                        */
  char direction;          /* 'S' = send side, 'R' = recv side  */
  uint8_t pkt_type;        /* RIFT_PKT_* type                    */
  uint16_t flags;          /* Packet flags                      */
  uint32_t seq_num;        /* Sequence number                   */
  uint32_t ack_num;        /* Acknowledgment number             */
  uint16_t payload_len;    /* Payload length                    */
  uint16_t window_size;    /* Advertised window                 */
  uint16_t conn_id;        /* Connection ID                     */

  /* Congestion / RTT context */
  uint32_t cwnd;     /* Current congestion window         */
  uint32_t ssthresh; /* Slow-start threshold              */
  double rtt_ms;     /* RTT measurement (if applicable)   */
  double rto_ms;     /* Current RTO                       */

  /* State transition */
  uint8_t old_state; /* Previous connection state         */
  uint8_t new_state; /* New connection state              */

  /* SACK info */
  uint16_t sack_count;    /* Number of SACK blocks             */
  uint32_t sack_start[4]; /* SACK block starts                 */
  uint32_t sack_end[4];   /* SACK block ends                   */

  /* Extra context */
  const char *detail; /* Optional free-text detail         */
} rift_trace_entry_t;

/* ── Trace Statistics ────────────────────────────────────────────── */
typedef struct {
  uint64_t count_tx;
  uint64_t count_rx;
  uint64_t count_ack;
  uint64_t count_sack;
  uint64_t count_retx;
  uint64_t count_drop;
  uint64_t count_cwnd;
  uint64_t count_rtt;
  uint64_t count_state;
  uint64_t count_loss;
  uint64_t count_timeout;
  uint64_t total_events;
  uint64_t first_ts;
  uint64_t last_ts;
} rift_trace_stats_t;

/* ── API ─────────────────────────────────────────────────────────── */

/**
 * Initialize the tracer.
 * @param base_path  Base file path (e.g., "/tmp/rift_trace")
 *                   Appends .jsonl / .txt based on format.
 *                   Pass NULL to disable file output (stderr only).
 * @param format     Output format (JSON, TEXT, or BOTH).
 * @return 0 on success, -1 on error.
 */
int rift_trace_init(const char *base_path, rift_trace_format_t format);

/**
 * Log a trace entry. Thread-safe.
 */
void rift_trace_log(const rift_trace_entry_t *entry);

/**
 * Get accumulated trace statistics.
 */
rift_trace_stats_t rift_trace_get_stats(void);

/**
 * Print a summary of trace statistics to the log.
 */
void rift_trace_dump_summary(void);

/**
 * Shut down the tracer and flush/close files.
 */
void rift_trace_shutdown(void);

/**
 * Return a human-readable name for a trace event type.
 */
const char *rift_trace_event_str(rift_trace_event_t event);

#endif /* RIFT_TRACE_H */
