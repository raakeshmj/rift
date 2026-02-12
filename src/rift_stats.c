/* rift_stats.c — performance counters and throughput sampling */

#include "rift_stats.h"
#include "rift_log.h"
#include "rift_protocol.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

void rift_stats_init(rift_stats_t *stats) {
  memset(stats, 0, sizeof(*stats));
  stats->start_time_us = rift_timestamp_us();
  atomic_store(&stats->last_report_us, stats->start_time_us);
}

void rift_stats_reset(rift_stats_t *stats) {
  uint64_t start = stats->start_time_us; /* Preserve start time */
  memset(stats, 0, sizeof(*stats));
  stats->start_time_us = start;
  atomic_store(&stats->last_report_us, rift_timestamp_us());
}

void rift_stats_report(const rift_stats_t *stats) {
  uint64_t now = rift_timestamp_us();
  double elapsed = (double)(now - stats->start_time_us) / 1000000.0;

  uint64_t sent = RIFT_STAT_GET(stats, packets_sent);
  uint64_t recv = RIFT_STAT_GET(stats, packets_received);
  uint64_t lost = RIFT_STAT_GET(stats, packets_lost);
  uint64_t retx = RIFT_STAT_GET(stats, packets_retransmitted);
  uint64_t crc_drop = RIFT_STAT_GET(stats, packets_dropped_crc);
  uint64_t b_sent = RIFT_STAT_GET(stats, bytes_sent);
  uint64_t b_recv = RIFT_STAT_GET(stats, bytes_received);
  uint64_t bp_sent = RIFT_STAT_GET(stats, bytes_payload_sent);
  uint64_t bp_recv = RIFT_STAT_GET(stats, bytes_payload_received);

  double mbps_sent = (double)(b_sent * 8) / (elapsed * 1000000.0);
  double mbps_recv = (double)(b_recv * 8) / (elapsed * 1000000.0);
  double pps_sent = (double)sent / elapsed;
  double pps_recv = (double)recv / elapsed;
  double loss_pct = sent > 0 ? (double)lost / (double)sent * 100.0 : 0.0;

  RIFT_LOG_INFO("═══════════════════ RIFT Statistics ═══════════════════");
  RIFT_LOG_INFO("  Duration:        %.2f seconds", elapsed);
  RIFT_LOG_INFO("  ── Packets ──");
  RIFT_LOG_INFO("  Sent:            %" PRIu64 " (%.1f pps)", sent, pps_sent);
  RIFT_LOG_INFO("  Received:        %" PRIu64 " (%.1f pps)", recv, pps_recv);
  RIFT_LOG_INFO("  Lost:            %" PRIu64 " (%.2f%%)", lost, loss_pct);
  RIFT_LOG_INFO("  Retransmitted:   %" PRIu64, retx);
  RIFT_LOG_INFO("  CRC failures:    %" PRIu64, crc_drop);
  RIFT_LOG_INFO("  ── Throughput ──");
  RIFT_LOG_INFO("  Send:            %.2f Mbps", mbps_sent);
  RIFT_LOG_INFO("  Receive:         %.2f Mbps", mbps_recv);
  RIFT_LOG_INFO("  ── Bytes ──");
  RIFT_LOG_INFO("  Wire sent:       %" PRIu64, b_sent);
  RIFT_LOG_INFO("  Wire received:   %" PRIu64, b_recv);
  RIFT_LOG_INFO("  Payload sent:    %" PRIu64, bp_sent);
  RIFT_LOG_INFO("  Payload received:%" PRIu64, bp_recv);
  RIFT_LOG_INFO("  ── ACKs ──");
  RIFT_LOG_INFO("  ACKs sent:       %" PRIu64, RIFT_STAT_GET(stats, acks_sent));
  RIFT_LOG_INFO("  ACKs received:   %" PRIu64,
               RIFT_STAT_GET(stats, acks_received));
  RIFT_LOG_INFO("  SACKs sent:      %" PRIu64, RIFT_STAT_GET(stats, sacks_sent));
  RIFT_LOG_INFO("  SACKs received:  %" PRIu64,
               RIFT_STAT_GET(stats, sacks_received));
  RIFT_LOG_INFO("  NACKs sent:      %" PRIu64, RIFT_STAT_GET(stats, nacks_sent));
  RIFT_LOG_INFO("  Duplicate ACKs:  %" PRIu64, RIFT_STAT_GET(stats, dup_acks));
  RIFT_LOG_INFO("═════════════════════════════════════════════════════");
}

/* ── Throughput Calculator ────────────────────────────────────────── */

void rift_throughput_init(rift_throughput_t *tp) {
  memset(tp, 0, sizeof(*tp));
  tp->last_time_us = rift_timestamp_us();
}

rift_throughput_sample_t rift_throughput_sample(rift_throughput_t *tp,
                                              const rift_stats_t *stats) {
  uint64_t now = rift_timestamp_us();
  double elapsed = (double)(now - tp->last_time_us) / 1000000.0;

  rift_throughput_sample_t sample = {0};
  sample.timestamp_us = now;

  if (elapsed < 0.001) /* Avoid division by near-zero */
    return sample;

  uint64_t curr_bytes = RIFT_STAT_GET(stats, bytes_sent);
  uint64_t curr_payload = RIFT_STAT_GET(stats, bytes_payload_sent);
  uint64_t curr_pkts = RIFT_STAT_GET(stats, packets_sent);
  uint64_t curr_lost = RIFT_STAT_GET(stats, packets_lost);
  uint64_t curr_retx = RIFT_STAT_GET(stats, packets_retransmitted);

  uint64_t delta_bytes = curr_bytes - tp->last_bytes;
  uint64_t delta_payload = curr_payload - tp->last_payload_bytes;
  uint64_t delta_pkts = curr_pkts - tp->last_packets;

  sample.mbps = (double)(delta_bytes * 8) / (elapsed * 1000000.0);
  sample.goodput_mbps = (double)(delta_payload * 8) / (elapsed * 1000000.0);
  sample.pps = (double)delta_pkts / elapsed;
  sample.loss_rate =
      curr_pkts > 0 ? (double)curr_lost / (double)curr_pkts * 100.0 : 0.0;
  sample.retx_rate =
      curr_pkts > 0 ? (double)curr_retx / (double)curr_pkts * 100.0 : 0.0;

  /* Store in circular buffer */
  tp->samples[tp->head] = sample;
  tp->head = (tp->head + 1) % RIFT_THROUGHPUT_WINDOW;
  if (tp->count < RIFT_THROUGHPUT_WINDOW)
    tp->count++;

  /* Update last values */
  tp->last_bytes = curr_bytes;
  tp->last_payload_bytes = curr_payload;
  tp->last_packets = curr_pkts;
  tp->last_time_us = now;

  return sample;
}

rift_throughput_sample_t rift_throughput_average(const rift_throughput_t *tp) {
  rift_throughput_sample_t avg = {0};

  if (tp->count == 0)
    return avg;

  for (uint32_t i = 0; i < tp->count; i++) {
    avg.mbps += tp->samples[i].mbps;
    avg.goodput_mbps += tp->samples[i].goodput_mbps;
    avg.pps += tp->samples[i].pps;
    avg.loss_rate += tp->samples[i].loss_rate;
    avg.retx_rate += tp->samples[i].retx_rate;
  }

  double n = (double)tp->count;
  avg.mbps /= n;
  avg.goodput_mbps /= n;
  avg.pps /= n;
  avg.loss_rate /= n;
  avg.retx_rate /= n;

  return avg;
}
