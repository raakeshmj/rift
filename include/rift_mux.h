/* rift_mux.h — connection multiplexer (multiple streams over one UDP socket) */

#ifndef RIFT_MUX_H
#define RIFT_MUX_H

#include "rift_buffer.h"
#include "rift_config.h"
#include "rift_congestion.h"
#include "rift_protocol.h"
#include "rift_rtt.h"
#include "rift_stats.h"
#include "rift_window.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/* ── Per-Stream State ────────────────────────────────────────────── */

typedef enum {
  RIFT_STREAM_IDLE,
  RIFT_STREAM_OPEN,
  RIFT_STREAM_CLOSING,
  RIFT_STREAM_CLOSED,
} rift_stream_state_t;

typedef struct {
  uint16_t conn_id;         /* Unique stream identifier            */
  rift_stream_state_t state; /* Stream state                        */

  rift_window_t window;         /* Independent sliding window          */
  rift_congestion_t congestion; /* Independent congestion control      */
  rift_rtt_estimator_t rtt;     /* Independent RTT estimator           */
  rift_stats_t stats;           /* Per-stream statistics               */

  uint32_t next_seq;     /* Next sequence to send               */
  uint32_t expected_seq; /* Next expected sequence (receiver)   */

  /* Application data */
  uint8_t *send_buf;
  size_t send_len;
  size_t send_offset;

  uint8_t *recv_buf;
  size_t recv_buf_size;
  size_t recv_offset;

  bool active; /* Slot is in use                      */
} rift_stream_t;

/* ── Multiplexer ─────────────────────────────────────────────────── */

typedef struct {
  int sockfd;                   /* Shared UDP socket           */
  struct sockaddr_in peer_addr; /* Remote peer address         */
  socklen_t peer_len;
  bool has_peer;

  rift_stream_t streams[RIFT_MAX_CONNECTIONS]; /* Stream table          */
  uint32_t stream_count;                     /* Active stream count         */

  pthread_mutex_t lock; /* Protects stream table       */
  bool running;
} rift_mux_t;

/* ── API ─────────────────────────────────────────────────────────── */

/**
 * Initialize the multiplexer on a bound socket.
 */
int rift_mux_init(rift_mux_t *mux, int sockfd);

/**
 * Create a new logical stream.
 * @return Pointer to the stream, or NULL on error.
 */
rift_stream_t *rift_mux_create_stream(rift_mux_t *mux, uint16_t conn_id);

/**
 * Look up a stream by conn_id.
 */
rift_stream_t *rift_mux_find_stream(rift_mux_t *mux, uint16_t conn_id);

/**
 * Close and destroy a stream.
 */
void rift_mux_destroy_stream(rift_mux_t *mux, uint16_t conn_id);

/**
 * Send data on a stream (queues into the stream's send buffer).
 */
int rift_mux_send(rift_mux_t *mux, uint16_t conn_id, const uint8_t *data,
                 size_t len);

/**
 * Process one incoming packet: demux to the correct stream.
 * Called from the receive loop.
 */
int rift_mux_dispatch(rift_mux_t *mux, const rift_packet_t *pkt,
                     const struct sockaddr_in *from, socklen_t from_len);

/**
 * Transmit pending data for all active streams (one round).
 */
int rift_mux_transmit_all(rift_mux_t *mux);

/**
 * Get count of active streams.
 */
uint32_t rift_mux_active_count(const rift_mux_t *mux);

/**
 * Shut down the multiplexer and all streams.
 */
void rift_mux_shutdown(rift_mux_t *mux);

#endif /* RIFT_MUX_H */
