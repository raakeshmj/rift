/* rift_stats.h — atomic performance counters and throughput measurement */

#ifndef RIFT_STATS_H
#define RIFT_STATS_H

#include <stdatomic.h>
#include <stdint.h>

#include "rift_config.h"

/* ── Performance Counters ─────────────────────────────────────────── */
typedef struct {
  /* Packet counters */
  _Atomic uint64_t packets_sent;
  _Atomic uint64_t packets_received;
  _Atomic uint64_t packets_lost;
  _Atomic uint64_t packets_retransmitted;
  _Atomic uint64_t packets_dropped_crc;

  /* Byte counters */
  _Atomic uint64_t bytes_sent;
  _Atomic uint64_t bytes_received;
  _Atomic uint64_t bytes_payload_sent;
  _Atomic uint64_t bytes_payload_received;

  /* ACK counters */
  _Atomic uint64_t acks_sent;
  _Atomic uint64_t acks_received;
  _Atomic uint64_t sacks_sent;
  _Atomic uint64_t sacks_received;
  _Atomic uint64_t nacks_sent;
  _Atomic uint64_t nacks_received;
  _Atomic uint64_t dup_acks;

  /* Connection counters */
  _Atomic uint64_t connections_opened;
  _Atomic uint64_t connections_closed;
  _Atomic uint64_t connections_reset;

  /* Timing */
  uint64_t start_time_us;
  _Atomic uint64_t last_report_us;
} rift_stats_t;

/* ── Throughput Sample ────────────────────────────────────────────── */
typedef struct {
  double mbps;           /* Megabits per second                */
  double goodput_mbps;   /* Payload-only throughput            */
  double pps;            /* Packets per second                 */
  double loss_rate;      /* Packet loss percentage             */
  double retx_rate;      /* Retransmission rate                */
  uint64_t timestamp_us; /* When this sample was taken         */
} rift_throughput_sample_t;

/* ── Throughput Calculator ────────────────────────────────────────── */
typedef struct {
  rift_throughput_sample_t samples[RIFT_THROUGHPUT_WINDOW];
  uint32_t head;
  uint32_t count;
  uint64_t last_bytes;
  uint64_t last_payload_bytes;
  uint64_t last_packets;
  uint64_t last_time_us;
} rift_throughput_t;

/*
 * Initialize performance counters.
 */
void rift_stats_init(rift_stats_t *stats);

/*
 * Reset all counters to zero.
 */
void rift_stats_reset(rift_stats_t *stats);

/*
 * Print a summary of the current stats.
 */
void rift_stats_report(const rift_stats_t *stats);

/*
 * Initialize the throughput calculator.
 */
void rift_throughput_init(rift_throughput_t *tp);

/*
 * Take a throughput sample based on current stats.
 * Returns the latest sample.
 */
rift_throughput_sample_t rift_throughput_sample(rift_throughput_t *tp,
                                              const rift_stats_t *stats);

/*
 * Get the average throughput over the sliding window.
 */
rift_throughput_sample_t rift_throughput_average(const rift_throughput_t *tp);

/*
 * Convenience increment macros for atomic counters.
 */
#define RIFT_STAT_INC(stats, field)                                             \
  atomic_fetch_add_explicit(&(stats)->field, 1, memory_order_relaxed)

#define RIFT_STAT_ADD(stats, field, val)                                        \
  atomic_fetch_add_explicit(&(stats)->field, (val), memory_order_relaxed)

#define RIFT_STAT_GET(stats, field)                                             \
  atomic_load_explicit(&(stats)->field, memory_order_relaxed)

#endif /* RIFT_STATS_H */
