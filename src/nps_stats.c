/* nps_stats.c — performance counters and throughput sampling */

#include "nps_stats.h"
#include "nps_log.h"
#include "nps_protocol.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

void nps_stats_init(nps_stats_t *stats) {
  memset(stats, 0, sizeof(*stats));
  stats->start_time_us = nps_timestamp_us();
  atomic_store(&stats->last_report_us, stats->start_time_us);
}

void nps_stats_reset(nps_stats_t *stats) {
  uint64_t start = stats->start_time_us; /* Preserve start time */
  memset(stats, 0, sizeof(*stats));
  stats->start_time_us = start;
  atomic_store(&stats->last_report_us, nps_timestamp_us());
}

void nps_stats_report(const nps_stats_t *stats) {
  uint64_t now = nps_timestamp_us();
  double elapsed = (double)(now - stats->start_time_us) / 1000000.0;

  uint64_t sent = NPS_STAT_GET(stats, packets_sent);
  uint64_t recv = NPS_STAT_GET(stats, packets_received);
  uint64_t lost = NPS_STAT_GET(stats, packets_lost);
  uint64_t retx = NPS_STAT_GET(stats, packets_retransmitted);
  uint64_t crc_drop = NPS_STAT_GET(stats, packets_dropped_crc);
  uint64_t b_sent = NPS_STAT_GET(stats, bytes_sent);
  uint64_t b_recv = NPS_STAT_GET(stats, bytes_received);
  uint64_t bp_sent = NPS_STAT_GET(stats, bytes_payload_sent);
  uint64_t bp_recv = NPS_STAT_GET(stats, bytes_payload_received);

  double mbps_sent = (double)(b_sent * 8) / (elapsed * 1000000.0);
  double mbps_recv = (double)(b_recv * 8) / (elapsed * 1000000.0);
  double pps_sent = (double)sent / elapsed;
  double pps_recv = (double)recv / elapsed;
  double loss_pct = sent > 0 ? (double)lost / (double)sent * 100.0 : 0.0;

  NPS_LOG_INFO("═══════════════════ NPS Statistics ═══════════════════");
  NPS_LOG_INFO("  Duration:        %.2f seconds", elapsed);
  NPS_LOG_INFO("  ── Packets ──");
  NPS_LOG_INFO("  Sent:            %" PRIu64 " (%.1f pps)", sent, pps_sent);
  NPS_LOG_INFO("  Received:        %" PRIu64 " (%.1f pps)", recv, pps_recv);
  NPS_LOG_INFO("  Lost:            %" PRIu64 " (%.2f%%)", lost, loss_pct);
  NPS_LOG_INFO("  Retransmitted:   %" PRIu64, retx);
  NPS_LOG_INFO("  CRC failures:    %" PRIu64, crc_drop);
  NPS_LOG_INFO("  ── Throughput ──");
  NPS_LOG_INFO("  Send:            %.2f Mbps", mbps_sent);
  NPS_LOG_INFO("  Receive:         %.2f Mbps", mbps_recv);
  NPS_LOG_INFO("  ── Bytes ──");
  NPS_LOG_INFO("  Wire sent:       %" PRIu64, b_sent);
  NPS_LOG_INFO("  Wire received:   %" PRIu64, b_recv);
  NPS_LOG_INFO("  Payload sent:    %" PRIu64, bp_sent);
  NPS_LOG_INFO("  Payload received:%" PRIu64, bp_recv);
  NPS_LOG_INFO("  ── ACKs ──");
  NPS_LOG_INFO("  ACKs sent:       %" PRIu64, NPS_STAT_GET(stats, acks_sent));
  NPS_LOG_INFO("  ACKs received:   %" PRIu64,
               NPS_STAT_GET(stats, acks_received));
  NPS_LOG_INFO("  SACKs sent:      %" PRIu64, NPS_STAT_GET(stats, sacks_sent));
  NPS_LOG_INFO("  SACKs received:  %" PRIu64,
               NPS_STAT_GET(stats, sacks_received));
  NPS_LOG_INFO("  NACKs sent:      %" PRIu64, NPS_STAT_GET(stats, nacks_sent));
  NPS_LOG_INFO("  Duplicate ACKs:  %" PRIu64, NPS_STAT_GET(stats, dup_acks));
  NPS_LOG_INFO("═════════════════════════════════════════════════════");
}

/* ── Throughput Calculator ────────────────────────────────────────── */

void nps_throughput_init(nps_throughput_t *tp) {
  memset(tp, 0, sizeof(*tp));
  tp->last_time_us = nps_timestamp_us();
}

nps_throughput_sample_t nps_throughput_sample(nps_throughput_t *tp,
                                              const nps_stats_t *stats) {
  uint64_t now = nps_timestamp_us();
  double elapsed = (double)(now - tp->last_time_us) / 1000000.0;

  nps_throughput_sample_t sample = {0};
  sample.timestamp_us = now;

  if (elapsed < 0.001) /* Avoid division by near-zero */
    return sample;

  uint64_t curr_bytes = NPS_STAT_GET(stats, bytes_sent);
  uint64_t curr_payload = NPS_STAT_GET(stats, bytes_payload_sent);
  uint64_t curr_pkts = NPS_STAT_GET(stats, packets_sent);
  uint64_t curr_lost = NPS_STAT_GET(stats, packets_lost);
  uint64_t curr_retx = NPS_STAT_GET(stats, packets_retransmitted);

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
  tp->head = (tp->head + 1) % NPS_THROUGHPUT_WINDOW;
  if (tp->count < NPS_THROUGHPUT_WINDOW)
    tp->count++;

  /* Update last values */
  tp->last_bytes = curr_bytes;
  tp->last_payload_bytes = curr_payload;
  tp->last_packets = curr_pkts;
  tp->last_time_us = now;

  return sample;
}

nps_throughput_sample_t nps_throughput_average(const nps_throughput_t *tp) {
  nps_throughput_sample_t avg = {0};

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
