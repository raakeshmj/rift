/* rift_buffer.h — circular packet buffer (O(1) insert/remove by seq) */

#ifndef RIFT_BUFFER_H
#define RIFT_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rift_config.h"
#include "rift_protocol.h"

/* ── Slot States ──────────────────────────────────────────────────── */
typedef enum {
  RIFT_SLOT_EMPTY = 0,
  RIFT_SLOT_PENDING = 1, /* Sent but not yet acknowledged       */
  RIFT_SLOT_ACKED = 2,   /* Acknowledged                        */
  RIFT_SLOT_NACKED = 3,  /* Negative-acknowledged (needs resend) */
  RIFT_SLOT_RECVD = 4,   /* Received (receiver side)            */
} rift_slot_state_t;

/* ── Buffer Slot ──────────────────────────────────────────────────── */
typedef struct {
  rift_slot_state_t state;
  rift_packet_t packet;
  uint64_t first_sent_us; /* Timestamp of first transmission   */
  uint64_t last_sent_us;  /* Timestamp of last (re)transmission */
  uint32_t retransmit_count;
} rift_buffer_slot_t;

/* ── Packet Buffer ────────────────────────────────────────────────── */
typedef struct {
  rift_buffer_slot_t *slots;
  uint32_t capacity; /* Number of slots (power of 2)      */
  uint32_t mask;     /* capacity - 1 for fast modulo      */
  uint32_t count;    /* Number of occupied slots          */
} rift_buffer_t;

/*
 * Initialize a packet buffer with the given capacity.
 * Capacity will be rounded up to the next power of 2.
 * Returns 0 on success, -1 on allocation failure.
 */
int rift_buffer_init(rift_buffer_t *buf, uint32_t capacity);

/*
 * Free all resources associated with a buffer.
 */
void rift_buffer_destroy(rift_buffer_t *buf);

/*
 * Insert a packet into the buffer at the slot for its sequence number.
 * Returns 0 on success, -1 if the slot is already occupied.
 */
int rift_buffer_insert(rift_buffer_t *buf, const rift_packet_t *pkt,
                      rift_slot_state_t state);

/*
 * Remove a packet from the buffer by sequence number.
 * Returns 0 on success, -1 if the slot was empty.
 */
int rift_buffer_remove(rift_buffer_t *buf, uint32_t seq_num);

/*
 * Get a pointer to the slot for a given sequence number.
 * Returns NULL if the slot is empty.
 */
rift_buffer_slot_t *rift_buffer_get(rift_buffer_t *buf, uint32_t seq_num);

/*
 * Set the state of a slot by sequence number.
 * Returns 0 on success, -1 if the slot is empty.
 */
int rift_buffer_set_state(rift_buffer_t *buf, uint32_t seq_num,
                         rift_slot_state_t state);

/*
 * Clear all slots in the buffer.
 */
void rift_buffer_clear(rift_buffer_t *buf);

/*
 * Check if the buffer is full.
 */
static inline bool rift_buffer_full(const rift_buffer_t *buf) {
  return buf->count >= buf->capacity;
}

/*
 * Check if the buffer is empty.
 */
static inline bool rift_buffer_empty(const rift_buffer_t *buf) {
  return buf->count == 0;
}

#endif /* RIFT_BUFFER_H */
