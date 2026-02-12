/* rift_congestion.h — TCP Cubic-inspired congestion control */

#ifndef RIFT_CONGESTION_H
#define RIFT_CONGESTION_H

#include <stdbool.h>
#include <stdint.h>

/* ── Congestion Control Phase ─────────────────────────────────────── */
typedef enum {
  RIFT_CC_SLOW_START,
  RIFT_CC_CONGESTION_AVOIDANCE,
  RIFT_CC_FAST_RECOVERY,
} rift_cc_phase_t;

/* ── Congestion Controller State ──────────────────────────────────── */
typedef struct {
  /* Core state */
  double cwnd;          /* Congestion window (double for Cubic)  */
  double ssthresh;      /* Slow-start threshold                  */
  rift_cc_phase_t phase; /* Current phase                         */

  /* Cubic parameters */
  double w_max;            /* Window size before last reduction     */
  double k;                /* Time to reach w_max again             */
  uint64_t epoch_start_us; /* Start time of current Cubic epoch     */

  /* Tracking */
  uint32_t acked_bytes;  /* Bytes acked in current window         */
  uint32_t lost_count;   /* Consecutive loss events               */
  uint64_t last_loss_us; /* Time of last loss event               */

  /* Statistics */
  double max_cwnd;       /* Peak cwnd achieved                    */
  uint64_t total_losses; /* Total loss events                     */
  uint64_t total_fast_recoveries;
} rift_congestion_t;

/*
 * Initialize congestion controller with default values.
 */
void rift_cc_init(rift_congestion_t *cc);

/*
 * Called when an ACK is received for new data.
 * acked: number of packets acknowledged.
 * now_us: current time in microseconds.
 */
void rift_cc_on_ack(rift_congestion_t *cc, uint32_t acked, uint64_t now_us);

/*
 * Called when packet loss is detected (timeout or triple dup ACK).
 * is_timeout: true for RTO timeout, false for fast retransmit.
 * now_us: current time in microseconds.
 */
void rift_cc_on_loss(rift_congestion_t *cc, bool is_timeout, uint64_t now_us);

/*
 * Get the current congestion window as an integer packet count.
 */
uint32_t rift_cc_get_cwnd(const rift_congestion_t *cc);

/*
 * Get the current congestion phase as a string.
 */
const char *rift_cc_phase_str(rift_cc_phase_t phase);

#endif /* RIFT_CONGESTION_H */
