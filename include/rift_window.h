/* rift_window.h — sliding window + SACK engine */

#ifndef RIFT_WINDOW_H
#define RIFT_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

#include "rift_buffer.h"
#include "rift_config.h"
#include "rift_protocol.h"

/* ── Retransmission Entry ─────────────────────────────────────────── */
typedef struct {
  uint32_t seq_num;
  uint64_t deadline_us; /* When to retransmit (absolute time) */
} rift_retx_entry_t;

/* ── Sliding Window State ─────────────────────────────────────────── */
typedef struct {
  /* Sequence number tracking */
  uint32_t base_seq; /* Oldest unacknowledged sequence     */
  uint32_t next_seq; /* Next sequence to send              */
  uint32_t max_seq;  /* Highest sequence allocated         */

  /* Window sizes */
  uint32_t window_size; /* Current effective window size      */
  uint32_t recv_window; /* Advertised receiver window         */
  uint32_t cwnd;        /* Congestion window                  */

  /* Packet buffer */
  rift_buffer_t buffer;

  /* Retransmission queue */
  rift_retx_entry_t *retx_queue;
  uint32_t retx_count;
  uint32_t retx_capacity;

  /* Duplicate ACK tracking */
  uint32_t last_ack_num;
  uint32_t dup_ack_count;
} rift_window_t;

/*
 * Initialize the sliding window.
 * initial_seq: the starting sequence number.
 * window_size: maximum window size.
 * Returns 0 on success, -1 on failure.
 */
int rift_window_init(rift_window_t *win, uint32_t initial_seq,
                    uint32_t window_size);

/*
 * Free all resources associated with the window.
 */
void rift_window_destroy(rift_window_t *win);

/*
 * Check if the window allows sending more packets.
 */
bool rift_window_can_send(const rift_window_t *win);

/*
 * Record a packet as sent in the window.
 * Returns 0 on success, -1 if window is full.
 */
int rift_window_mark_sent(rift_window_t *win, const rift_packet_t *pkt,
                         uint64_t rto_us);

/*
 * Process a cumulative ACK.
 * Advances the window base and frees acknowledged slots.
 * Returns the number of packets acknowledged.
 */
uint32_t rift_window_process_ack(rift_window_t *win, uint32_t ack_num);

/*
 * Process SACK blocks from a received ACK/SACK packet.
 * Marks selectively acknowledged packets and identifies gaps.
 * Returns the number of newly acknowledged packets.
 */
uint32_t rift_window_process_sack(rift_window_t *win,
                                 const rift_sack_block_t *blocks,
                                 uint16_t block_count);

/*
 * Process a NACK for a specific sequence number.
 * Schedules immediate retransmission.
 * Returns 0 on success, -1 if the sequence is not in the window.
 */
int rift_window_process_nack(rift_window_t *win, uint32_t seq_num);

/*
 * Check for retransmission timeouts.
 * Fills `timed_out` with sequence numbers that have expired.
 * Returns the count of timed-out packets.
 */
uint32_t rift_window_check_timeouts(rift_window_t *win, uint64_t now_us,
                                   uint32_t *timed_out, uint32_t max_count);

/*
 * Update the receiver window advertisement.
 */
void rift_window_set_recv_window(rift_window_t *win, uint32_t recv_window);

/*
 * Update the congestion window.
 */
void rift_window_set_cwnd(rift_window_t *win, uint32_t cwnd);

/*
 * Get the effective window (min of cwnd, recv_window, window_size).
 */
uint32_t rift_window_effective(const rift_window_t *win);

/*
 * Get the number of in-flight (unacknowledged) packets.
 */
static inline uint32_t rift_window_in_flight(const rift_window_t *win) {
  return win->next_seq - win->base_seq;
}

/*
 * Build SACK blocks from the receiver buffer (for the receiver side).
 * Scans the buffer for received-but-not-yet-delivered packets
 * and generates SACK blocks describing them.
 * Returns the number of blocks generated.
 */
uint16_t rift_window_build_sack_blocks(const rift_window_t *win,
                                      uint32_t expected_seq,
                                      rift_sack_block_t *blocks,
                                      uint16_t max_blocks);

#endif /* RIFT_WINDOW_H */
