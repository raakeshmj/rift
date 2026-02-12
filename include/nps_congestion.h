/* nps_congestion.h — TCP Cubic-inspired congestion control */

#ifndef NPS_CONGESTION_H
#define NPS_CONGESTION_H

#include <stdbool.h>
#include <stdint.h>

/* ── Congestion Control Phase ─────────────────────────────────────── */
typedef enum {
  NPS_CC_SLOW_START,
  NPS_CC_CONGESTION_AVOIDANCE,
  NPS_CC_FAST_RECOVERY,
} nps_cc_phase_t;

/* ── Congestion Controller State ──────────────────────────────────── */
typedef struct {
  /* Core state */
  double cwnd;          /* Congestion window (double for Cubic)  */
  double ssthresh;      /* Slow-start threshold                  */
  nps_cc_phase_t phase; /* Current phase                         */

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
} nps_congestion_t;

/*
 * Initialize congestion controller with default values.
 */
void nps_cc_init(nps_congestion_t *cc);

/*
 * Called when an ACK is received for new data.
 * acked: number of packets acknowledged.
 * now_us: current time in microseconds.
 */
void nps_cc_on_ack(nps_congestion_t *cc, uint32_t acked, uint64_t now_us);

/*
 * Called when packet loss is detected (timeout or triple dup ACK).
 * is_timeout: true for RTO timeout, false for fast retransmit.
 * now_us: current time in microseconds.
 */
void nps_cc_on_loss(nps_congestion_t *cc, bool is_timeout, uint64_t now_us);

/*
 * Get the current congestion window as an integer packet count.
 */
uint32_t nps_cc_get_cwnd(const nps_congestion_t *cc);

/*
 * Get the current congestion phase as a string.
 */
const char *nps_cc_phase_str(nps_cc_phase_t phase);

#endif /* NPS_CONGESTION_H */
