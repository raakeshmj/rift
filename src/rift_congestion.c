/* rift_congestion.c — TCP Cubic congestion control */

#include "rift_congestion.h"
#include "rift_config.h"

#include <math.h>

void rift_cc_init(rift_congestion_t *cc) {
  cc->cwnd = RIFT_CWND_INIT;
  cc->ssthresh = RIFT_SLOW_START_THRESH;
  cc->phase = RIFT_CC_SLOW_START;
  cc->w_max = 0.0;
  cc->k = 0.0;
  cc->epoch_start_us = 0;
  cc->acked_bytes = 0;
  cc->lost_count = 0;
  cc->last_loss_us = 0;
  cc->max_cwnd = RIFT_CWND_INIT;
  cc->total_losses = 0;
  cc->total_fast_recoveries = 0;
}

static void cubic_reset_epoch(rift_congestion_t *cc) { cc->epoch_start_us = 0; }

static double cubic_w(rift_congestion_t *cc, uint64_t now_us) {
  if (cc->epoch_start_us == 0) {
    cc->epoch_start_us = now_us;
    /* K = cbrt(W_max * beta / C) */
    if (cc->w_max > 0) {
      cc->k = cbrt(cc->w_max * RIFT_CUBIC_BETA / RIFT_CUBIC_C);
    } else {
      cc->k = 0.0;
    }
  }

  double t = (double)(now_us - cc->epoch_start_us) / 1000000.0; /* seconds */
  double dt = t - cc->k;
  double w_cubic = RIFT_CUBIC_C * dt * dt * dt + cc->w_max;

  if (w_cubic < 1.0)
    w_cubic = 1.0;
  return w_cubic;
}

void rift_cc_on_ack(rift_congestion_t *cc, uint32_t acked, uint64_t now_us) {
  if (acked == 0)
    return;

  switch (cc->phase) {
  case RIFT_CC_SLOW_START:
    /* Exponential growth: increase cwnd by 1 for each ACK */
    cc->cwnd += acked;
    if (cc->cwnd >= cc->ssthresh) {
      cc->phase = RIFT_CC_CONGESTION_AVOIDANCE;
      cubic_reset_epoch(cc);
    }
    break;

  case RIFT_CC_CONGESTION_AVOIDANCE: {
    /* Cubic: W(t) = C * (t - K)^3 + W_max */
    double w_target = cubic_w(cc, now_us);

    /* TCP-friendliness: W_tcp = W_max * (1-β) + 3β / (2-β) * t/RTT
     * Simplified: grow at least as fast as standard AIMD */
    double w_tcp =
        cc->w_max * (1.0 - RIFT_CUBIC_BETA) + (double)acked / cc->cwnd;

    /* Use the larger of Cubic and TCP-friendly */
    double target = (w_target > w_tcp) ? w_target : w_tcp;

    if (target > cc->cwnd) {
      cc->cwnd += (target - cc->cwnd) * ((double)acked / cc->cwnd);
    } else {
      cc->cwnd += (double)acked / cc->cwnd; /* At least AI growth */
    }
    break;
  }

  case RIFT_CC_FAST_RECOVERY:
    /* In fast recovery: inflate window by acked count */
    cc->cwnd += acked;
    /* Exit fast recovery when all lost data is recovered */
    cc->phase = RIFT_CC_CONGESTION_AVOIDANCE;
    cc->total_fast_recoveries++;
    cubic_reset_epoch(cc);
    break;
  }

  /* Enforce minimum */
  if (cc->cwnd < 1.0)
    cc->cwnd = 1.0;

  /* Track peak */
  if (cc->cwnd > cc->max_cwnd)
    cc->max_cwnd = cc->cwnd;
}

void rift_cc_on_loss(rift_congestion_t *cc, bool is_timeout, uint64_t now_us) {
  cc->total_losses++;
  cc->last_loss_us = now_us;

  /* Record the window size before reduction */
  cc->w_max = cc->cwnd;

  if (is_timeout) {
    /* Timeout: severe congestion, reset to slow start */
    cc->ssthresh = cc->cwnd * RIFT_CUBIC_BETA;
    if (cc->ssthresh < 2.0)
      cc->ssthresh = 2.0;
    cc->cwnd = RIFT_CWND_INIT;
    cc->phase = RIFT_CC_SLOW_START;
    cubic_reset_epoch(cc);
  } else {
    /* Fast retransmit: multiplicative decrease */
    cc->ssthresh = cc->cwnd * RIFT_CUBIC_BETA;
    if (cc->ssthresh < 2.0)
      cc->ssthresh = 2.0;
    cc->cwnd = cc->ssthresh;
    cc->phase = RIFT_CC_FAST_RECOVERY;
    cubic_reset_epoch(cc);
  }

  cc->lost_count++;
}

uint32_t rift_cc_get_cwnd(const rift_congestion_t *cc) {
  uint32_t cwnd = (uint32_t)cc->cwnd;
  if (cwnd < 1)
    cwnd = 1;
  return cwnd;
}

const char *rift_cc_phase_str(rift_cc_phase_t phase) {
  switch (phase) {
  case RIFT_CC_SLOW_START:
    return "SLOW_START";
  case RIFT_CC_CONGESTION_AVOIDANCE:
    return "CONGESTION_AVOIDANCE";
  case RIFT_CC_FAST_RECOVERY:
    return "FAST_RECOVERY";
  default:
    return "UNKNOWN";
  }
}
