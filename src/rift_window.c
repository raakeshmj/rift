/* rift_window.c — sliding window + SACK engine */

#include "rift_window.h"
#include "rift_log.h"

#include <stdlib.h>
#include <string.h>

int rift_window_init(rift_window_t *win, uint32_t initial_seq,
                    uint32_t window_size) {
  memset(win, 0, sizeof(*win));

  win->base_seq = initial_seq;
  win->next_seq = initial_seq;
  win->max_seq = initial_seq;
  win->window_size = window_size;
  win->recv_window = window_size;
  win->cwnd = RIFT_CWND_INIT;

  if (rift_buffer_init(&win->buffer, window_size * 2) != 0)
    return -1;

  /* Retransmission queue */
  win->retx_capacity = window_size;
  win->retx_queue = calloc(win->retx_capacity, sizeof(rift_retx_entry_t));
  if (!win->retx_queue) {
    rift_buffer_destroy(&win->buffer);
    return -1;
  }
  win->retx_count = 0;

  win->last_ack_num = initial_seq;
  win->dup_ack_count = 0;

  return 0;
}

void rift_window_destroy(rift_window_t *win) {
  rift_buffer_destroy(&win->buffer);
  free(win->retx_queue);
  win->retx_queue = NULL;
  win->retx_count = 0;
}

bool rift_window_can_send(const rift_window_t *win) {
  uint32_t in_flight = win->next_seq - win->base_seq;
  uint32_t effective = rift_window_effective(win);
  return in_flight < effective;
}

uint32_t rift_window_effective(const rift_window_t *win) {
  uint32_t min_val = win->window_size;
  if (win->recv_window < min_val)
    min_val = win->recv_window;
  if (win->cwnd < min_val)
    min_val = win->cwnd;
  if (min_val < 1)
    min_val = 1;
  return min_val;
}

int rift_window_mark_sent(rift_window_t *win, const rift_packet_t *pkt,
                         uint64_t rto_us) {
  if (!rift_window_can_send(win))
    return -1;

  if (rift_buffer_insert(&win->buffer, pkt, RIFT_SLOT_PENDING) != 0)
    return -1;

  uint32_t seq = pkt->header.seq_num;

  /* Add to retransmission queue */
  if (win->retx_count < win->retx_capacity) {
    win->retx_queue[win->retx_count].seq_num = seq;
    win->retx_queue[win->retx_count].deadline_us = rift_timestamp_us() + rto_us;
    win->retx_count++;
  }

  if (seq >= win->next_seq)
    win->next_seq = seq + 1;
  if (seq >= win->max_seq)
    win->max_seq = seq + 1;

  return 0;
}

uint32_t rift_window_process_ack(rift_window_t *win, uint32_t ack_num) {
  if (ack_num <= win->base_seq)
    return 0; /* Old or duplicate ACK */

  /* Check for duplicate ACK */
  if (ack_num == win->last_ack_num) {
    win->dup_ack_count++;
    return 0;
  }

  win->dup_ack_count = 0;
  win->last_ack_num = ack_num;

  uint32_t acked = 0;

  /* Remove all packets from base_seq to ack_num - 1 */
  for (uint32_t seq = win->base_seq; seq < ack_num; seq++) {
    rift_buffer_slot_t *slot = rift_buffer_get(&win->buffer, seq);
    if (slot) {
      rift_buffer_remove(&win->buffer, seq);
      acked++;
    }
  }

  win->base_seq = ack_num;

  /* Remove acknowledged entries from retx queue */
  uint32_t new_count = 0;
  for (uint32_t i = 0; i < win->retx_count; i++) {
    if (win->retx_queue[i].seq_num >= ack_num) {
      win->retx_queue[new_count++] = win->retx_queue[i];
    }
  }
  win->retx_count = new_count;

  return acked;
}

uint32_t rift_window_process_sack(rift_window_t *win,
                                 const rift_sack_block_t *blocks,
                                 uint16_t block_count) {
  uint32_t newly_acked = 0;

  for (uint16_t i = 0; i < block_count; i++) {
    for (uint32_t seq = blocks[i].start_seq; seq < blocks[i].end_seq; seq++) {
      rift_buffer_slot_t *slot = rift_buffer_get(&win->buffer, seq);
      if (slot && slot->state == RIFT_SLOT_PENDING) {
        slot->state = RIFT_SLOT_ACKED;
        newly_acked++;
      }
    }
  }

  return newly_acked;
}

int rift_window_process_nack(rift_window_t *win, uint32_t seq_num) {
  rift_buffer_slot_t *slot = rift_buffer_get(&win->buffer, seq_num);
  if (!slot)
    return -1;

  slot->state = RIFT_SLOT_NACKED;

  /* Schedule immediate retransmission by setting deadline to now */
  for (uint32_t i = 0; i < win->retx_count; i++) {
    if (win->retx_queue[i].seq_num == seq_num) {
      win->retx_queue[i].deadline_us = 0;
      return 0;
    }
  }

  /* If not in retx queue, add it */
  if (win->retx_count < win->retx_capacity) {
    win->retx_queue[win->retx_count].seq_num = seq_num;
    win->retx_queue[win->retx_count].deadline_us = 0;
    win->retx_count++;
  }

  return 0;
}

uint32_t rift_window_check_timeouts(rift_window_t *win, uint64_t now_us,
                                   uint32_t *timed_out, uint32_t max_count) {
  uint32_t count = 0;

  for (uint32_t i = 0; i < win->retx_count && count < max_count; i++) {
    if (win->retx_queue[i].deadline_us <= now_us) {
      /* Check if the packet is still pending */
      rift_buffer_slot_t *slot =
          rift_buffer_get(&win->buffer, win->retx_queue[i].seq_num);
      if (slot &&
          (slot->state == RIFT_SLOT_PENDING || slot->state == RIFT_SLOT_NACKED)) {
        timed_out[count++] = win->retx_queue[i].seq_num;
      }
    }
  }

  return count;
}

void rift_window_set_recv_window(rift_window_t *win, uint32_t recv_window) {
  win->recv_window = recv_window;
}

void rift_window_set_cwnd(rift_window_t *win, uint32_t cwnd) { win->cwnd = cwnd; }

uint16_t rift_window_build_sack_blocks(const rift_window_t *win,
                                      uint32_t expected_seq,
                                      rift_sack_block_t *blocks,
                                      uint16_t max_blocks) {
  uint16_t block_count = 0;
  bool in_block = false;
  uint32_t block_start = 0;

  /* Scan the buffer for received packets beyond expected_seq */
  uint32_t scan_end = expected_seq + win->window_size;

  for (uint32_t seq = expected_seq; seq < scan_end && block_count < max_blocks;
       seq++) {
    rift_buffer_slot_t *slot = rift_buffer_get((rift_buffer_t *)&win->buffer, seq);
    bool is_recv = (slot != NULL && slot->state == RIFT_SLOT_RECVD);

    if (is_recv && !in_block) {
      block_start = seq;
      in_block = true;
    } else if (!is_recv && in_block) {
      blocks[block_count].start_seq = block_start;
      blocks[block_count].end_seq = seq;
      block_count++;
      in_block = false;
    }
  }

  /* Close any open block */
  if (in_block && block_count < max_blocks) {
    blocks[block_count].start_seq = block_start;
    blocks[block_count].end_seq = expected_seq + win->window_size;
    block_count++;
  }

  return block_count;
}
