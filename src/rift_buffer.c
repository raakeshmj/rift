/* rift_buffer.c — power-of-2 ring buffer indexed by seq % capacity */

#include "rift_buffer.h"

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

int rift_buffer_init(rift_buffer_t *buf, uint32_t capacity) {
  capacity = next_pow2(capacity);
  buf->slots = calloc(capacity, sizeof(rift_buffer_slot_t));
  if (!buf->slots)
    return -1;

  buf->capacity = capacity;
  buf->mask = capacity - 1;
  buf->count = 0;
  return 0;
}

void rift_buffer_destroy(rift_buffer_t *buf) {
  if (buf->slots) {
    free(buf->slots);
    buf->slots = NULL;
  }
  buf->capacity = 0;
  buf->mask = 0;
  buf->count = 0;
}

int rift_buffer_insert(rift_buffer_t *buf, const rift_packet_t *pkt,
                      rift_slot_state_t state) {
  uint32_t idx = pkt->header.seq_num & buf->mask;
  rift_buffer_slot_t *slot = &buf->slots[idx];

  if (slot->state != RIFT_SLOT_EMPTY)
    return -1;

  slot->packet = *pkt;
  slot->state = state;
  slot->first_sent_us = rift_timestamp_us();
  slot->last_sent_us = slot->first_sent_us;
  slot->retransmit_count = 0;
  buf->count++;
  return 0;
}

int rift_buffer_remove(rift_buffer_t *buf, uint32_t seq_num) {
  uint32_t idx = seq_num & buf->mask;
  rift_buffer_slot_t *slot = &buf->slots[idx];

  if (slot->state == RIFT_SLOT_EMPTY)
    return -1;

  slot->state = RIFT_SLOT_EMPTY;
  buf->count--;
  return 0;
}

rift_buffer_slot_t *rift_buffer_get(rift_buffer_t *buf, uint32_t seq_num) {
  uint32_t idx = seq_num & buf->mask;
  rift_buffer_slot_t *slot = &buf->slots[idx];

  if (slot->state == RIFT_SLOT_EMPTY)
    return NULL;

  /* Verify this is actually the right sequence number */
  if (slot->packet.header.seq_num != seq_num)
    return NULL;

  return slot;
}

int rift_buffer_set_state(rift_buffer_t *buf, uint32_t seq_num,
                         rift_slot_state_t state) {
  uint32_t idx = seq_num & buf->mask;
  rift_buffer_slot_t *slot = &buf->slots[idx];

  if (slot->state == RIFT_SLOT_EMPTY)
    return -1;
  if (slot->packet.header.seq_num != seq_num)
    return -1;

  slot->state = state;
  return 0;
}

void rift_buffer_clear(rift_buffer_t *buf) {
  memset(buf->slots, 0, buf->capacity * sizeof(rift_buffer_slot_t));
  buf->count = 0;
}
