/* nps_buffer.c — power-of-2 ring buffer indexed by seq % capacity */

#include "nps_buffer.h"

#include <stdlib.h>
#include <string.h>

/* Round up to next power of 2 */
static uint32_t next_pow2(uint32_t v) {
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  v++;
  return v < 4 ? 4 : v;
}

int nps_buffer_init(nps_buffer_t *buf, uint32_t capacity) {
  capacity = next_pow2(capacity);
  buf->slots = calloc(capacity, sizeof(nps_buffer_slot_t));
  if (!buf->slots)
    return -1;

  buf->capacity = capacity;
  buf->mask = capacity - 1;
  buf->count = 0;
  return 0;
}

void nps_buffer_destroy(nps_buffer_t *buf) {
  if (buf->slots) {
    free(buf->slots);
    buf->slots = NULL;
  }
  buf->capacity = 0;
  buf->mask = 0;
  buf->count = 0;
}

int nps_buffer_insert(nps_buffer_t *buf, const nps_packet_t *pkt,
                      nps_slot_state_t state) {
  uint32_t idx = pkt->header.seq_num & buf->mask;
  nps_buffer_slot_t *slot = &buf->slots[idx];

  if (slot->state != NPS_SLOT_EMPTY)
    return -1;

  slot->packet = *pkt;
  slot->state = state;
  slot->first_sent_us = nps_timestamp_us();
  slot->last_sent_us = slot->first_sent_us;
  slot->retransmit_count = 0;
  buf->count++;
  return 0;
}

int nps_buffer_remove(nps_buffer_t *buf, uint32_t seq_num) {
  uint32_t idx = seq_num & buf->mask;
  nps_buffer_slot_t *slot = &buf->slots[idx];

  if (slot->state == NPS_SLOT_EMPTY)
    return -1;

  slot->state = NPS_SLOT_EMPTY;
  buf->count--;
  return 0;
}

nps_buffer_slot_t *nps_buffer_get(nps_buffer_t *buf, uint32_t seq_num) {
  uint32_t idx = seq_num & buf->mask;
  nps_buffer_slot_t *slot = &buf->slots[idx];

  if (slot->state == NPS_SLOT_EMPTY)
    return NULL;

  /* Verify this is actually the right sequence number */
  if (slot->packet.header.seq_num != seq_num)
    return NULL;

  return slot;
}

int nps_buffer_set_state(nps_buffer_t *buf, uint32_t seq_num,
                         nps_slot_state_t state) {
  uint32_t idx = seq_num & buf->mask;
  nps_buffer_slot_t *slot = &buf->slots[idx];

  if (slot->state == NPS_SLOT_EMPTY)
    return -1;
  if (slot->packet.header.seq_num != seq_num)
    return -1;

  slot->state = state;
  return 0;
}

void nps_buffer_clear(nps_buffer_t *buf) {
  memset(buf->slots, 0, buf->capacity * sizeof(nps_buffer_slot_t));
  buf->count = 0;
}
