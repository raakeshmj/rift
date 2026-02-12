/* rift_rtt.c — Jacobson/Karels RTT estimator (RFC 6298) */

#include "rift_rtt.h"
#include "rift_config.h"

#include <math.h>

void rift_rtt_init(rift_rtt_estimator_t *est) {
  est->srtt_ms = 0.0;
  est->rttvar_ms = 0.0;
  est->rto_ms = RIFT_RTO_INIT_MS;
  est->has_measurement = false;
  est->sample_count = 0;
  est->min_rtt_ms = 1e9;
  est->max_rtt_ms = 0.0;
  est->last_rtt_ms = 0.0;
}

void rift_rtt_update(rift_rtt_estimator_t *est, double rtt_ms) {
  est->last_rtt_ms = rtt_ms;
  est->sample_count++;

  if (rtt_ms < est->min_rtt_ms)
    est->min_rtt_ms = rtt_ms;
  if (rtt_ms > est->max_rtt_ms)
    est->max_rtt_ms = rtt_ms;

  if (!est->has_measurement) {
    /* First measurement: RFC 6298 Section 2.2 */
    est->srtt_ms = rtt_ms;
    est->rttvar_ms = rtt_ms / 2.0;
    est->has_measurement = true;
  } else {
    /* Subsequent measurements: RFC 6298 Section 2.3 */
    double delta = fabs(est->srtt_ms - rtt_ms);
    est->rttvar_ms =
        (1.0 - RIFT_RTT_BETA) * est->rttvar_ms + RIFT_RTT_BETA * delta;
    est->srtt_ms =
        (1.0 - RIFT_RTT_ALPHA) * est->srtt_ms + RIFT_RTT_ALPHA * rtt_ms;
  }

  /* RTO = SRTT + max(G, K * RTTVAR), G = 1ms clock granularity */
  double k_rttvar = RIFT_RTT_K * est->rttvar_ms;
  if (k_rttvar < 1.0)
    k_rttvar = 1.0;
  est->rto_ms = est->srtt_ms + k_rttvar;

  /* Clamp RTO */
  if (est->rto_ms < RIFT_RTO_MIN_MS)
    est->rto_ms = RIFT_RTO_MIN_MS;
  if (est->rto_ms > RIFT_RTO_MAX_MS)
    est->rto_ms = RIFT_RTO_MAX_MS;
}

void rift_rtt_backoff(rift_rtt_estimator_t *est) {
  est->rto_ms *= RIFT_BACKOFF_FACTOR;
  if (est->rto_ms > RIFT_RTO_MAX_MS)
    est->rto_ms = RIFT_RTO_MAX_MS;
}

double rift_rtt_from_timestamps(uint64_t ts_send_us, uint64_t ts_echo_us) {
  if (ts_echo_us == 0 || ts_send_us == 0)
    return -1.0;
  if (ts_send_us >= ts_echo_us)
    return -1.0;
  return (double)(ts_echo_us - ts_send_us) / 1000.0; /* us → ms */
}
