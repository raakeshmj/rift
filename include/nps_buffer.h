/* nps_buffer.h — circular packet buffer (O(1) insert/remove by seq) */

#ifndef NPS_BUFFER_H
#define NPS_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nps_config.h"
#include "nps_protocol.h"

/* ── Slot States ──────────────────────────────────────────────────── */
typedef enum {
  NPS_SLOT_EMPTY = 0,
  NPS_SLOT_PENDING = 1, /* Sent but not yet acknowledged       */
  NPS_SLOT_ACKED = 2,   /* Acknowledged                        */
  NPS_SLOT_NACKED = 3,  /* Negative-acknowledged (needs resend) */
  NPS_SLOT_RECVD = 4,   /* Received (receiver side)            */
} nps_slot_state_t;

/* ── Buffer Slot ──────────────────────────────────────────────────── */
typedef struct {
  nps_slot_state_t state;
  nps_packet_t packet;
  uint64_t first_sent_us; /* Timestamp of first transmission   */
  uint64_t last_sent_us;  /* Timestamp of last (re)transmission */
  uint32_t retransmit_count;
} nps_buffer_slot_t;

/* ── Packet Buffer ────────────────────────────────────────────────── */
typedef struct {
  nps_buffer_slot_t *slots;
  uint32_t capacity; /* Number of slots (power of 2)      */
  uint32_t mask;     /* capacity - 1 for fast modulo      */
  uint32_t count;    /* Number of occupied slots          */
} nps_buffer_t;

/*
 * Initialize a packet buffer with the given capacity.
 * Capacity will be rounded up to the next power of 2.
 * Returns 0 on success, -1 on allocation failure.
 */
int nps_buffer_init(nps_buffer_t *buf, uint32_t capacity);

/*
 * Free all resources associated with a buffer.
 */
void nps_buffer_destroy(nps_buffer_t *buf);

/*
 * Insert a packet into the buffer at the slot for its sequence number.
 * Returns 0 on success, -1 if the slot is already occupied.
 */
int nps_buffer_insert(nps_buffer_t *buf, const nps_packet_t *pkt,
                      nps_slot_state_t state);

/*
 * Remove a packet from the buffer by sequence number.
 * Returns 0 on success, -1 if the slot was empty.
 */
int nps_buffer_remove(nps_buffer_t *buf, uint32_t seq_num);

/*
 * Get a pointer to the slot for a given sequence number.
 * Returns NULL if the slot is empty.
 */
nps_buffer_slot_t *nps_buffer_get(nps_buffer_t *buf, uint32_t seq_num);

/*
 * Set the state of a slot by sequence number.
 * Returns 0 on success, -1 if the slot is empty.
 */
int nps_buffer_set_state(nps_buffer_t *buf, uint32_t seq_num,
                         nps_slot_state_t state);

/*
 * Clear all slots in the buffer.
 */
void nps_buffer_clear(nps_buffer_t *buf);

/*
 * Check if the buffer is full.
 */
static inline bool nps_buffer_full(const nps_buffer_t *buf) {
  return buf->count >= buf->capacity;
}

/*
 * Check if the buffer is empty.
 */
static inline bool nps_buffer_empty(const nps_buffer_t *buf) {
  return buf->count == 0;
}

#endif /* NPS_BUFFER_H */
