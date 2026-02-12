/* nps_rtt.h — RTT estimation (Jacobson/Karels, RFC 6298) */

#ifndef NPS_RTT_H
#define NPS_RTT_H

#include <stdbool.h>
#include <stdint.h>

/* ── RTT Estimator State ──────────────────────────────────────────── */
typedef struct {
  double srtt_ms;        /* Smoothed RTT in milliseconds      */
  double rttvar_ms;      /* RTT variance in milliseconds      */
  double rto_ms;         /* Retransmission timeout in ms      */
  bool has_measurement;  /* True after first RTT sample       */
  uint32_t sample_count; /* Number of RTT samples taken       */
  double min_rtt_ms;     /* Minimum observed RTT              */
  double max_rtt_ms;     /* Maximum observed RTT              */
  double last_rtt_ms;    /* Most recent RTT sample            */
} nps_rtt_estimator_t;

/*
 * Initialize the RTT estimator with default values.
 */
void nps_rtt_init(nps_rtt_estimator_t *est);

/*
 * Update the RTT estimator with a new measurement.
 * rtt_ms: the measured round-trip time in milliseconds.
 *
 * Uses Jacobson/Karels:
 *   RTTVAR = (1-β) * RTTVAR + β * |SRTT - R|
 *   SRTT   = (1-α) * SRTT   + α * R
 *   RTO    = SRTT + max(G, K * RTTVAR)   [G=clock granularity, assumed 1ms]
 */
void nps_rtt_update(nps_rtt_estimator_t *est, double rtt_ms);

/*
 * Apply exponential backoff to the current RTO.
 * Called when a retransmission timeout occurs.
 */
void nps_rtt_backoff(nps_rtt_estimator_t *est);

/*
 * Get the current RTO in milliseconds.
 */
static inline double nps_rtt_get_rto(const nps_rtt_estimator_t *est) {
  return est->rto_ms;
}

/*
 * Get the current SRTT in milliseconds.
 */
static inline double nps_rtt_get_srtt(const nps_rtt_estimator_t *est) {
  return est->srtt_ms;
}

/*
 * Compute RTT from send and echo timestamps (in microseconds).
 * Returns RTT in milliseconds.
 */
double nps_rtt_from_timestamps(uint64_t ts_send_us, uint64_t ts_echo_us);

#endif /* NPS_RTT_H */
