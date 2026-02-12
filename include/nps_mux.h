/* nps_mux.h — connection multiplexer (multiple streams over one UDP socket) */

#ifndef NPS_MUX_H
#define NPS_MUX_H

#include "nps_buffer.h"
#include "nps_config.h"
#include "nps_congestion.h"
#include "nps_protocol.h"
#include "nps_rtt.h"
#include "nps_stats.h"
#include "nps_window.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/* ── Per-Stream State ────────────────────────────────────────────── */

typedef enum {
  NPS_STREAM_IDLE,
  NPS_STREAM_OPEN,
  NPS_STREAM_CLOSING,
  NPS_STREAM_CLOSED,
} nps_stream_state_t;

typedef struct {
  uint16_t conn_id;         /* Unique stream identifier            */
  nps_stream_state_t state; /* Stream state                        */

  nps_window_t window;         /* Independent sliding window          */
  nps_congestion_t congestion; /* Independent congestion control      */
  nps_rtt_estimator_t rtt;     /* Independent RTT estimator           */
  nps_stats_t stats;           /* Per-stream statistics               */

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
} nps_stream_t;

/* ── Multiplexer ─────────────────────────────────────────────────── */

typedef struct {
  int sockfd;                   /* Shared UDP socket           */
  struct sockaddr_in peer_addr; /* Remote peer address         */
  socklen_t peer_len;
  bool has_peer;

  nps_stream_t streams[NPS_MAX_CONNECTIONS]; /* Stream table          */
  uint32_t stream_count;                     /* Active stream count         */

  pthread_mutex_t lock; /* Protects stream table       */
  bool running;
} nps_mux_t;

/* ── API ─────────────────────────────────────────────────────────── */

/**
 * Initialize the multiplexer on a bound socket.
 */
int nps_mux_init(nps_mux_t *mux, int sockfd);

/**
 * Create a new logical stream.
 * @return Pointer to the stream, or NULL on error.
 */
nps_stream_t *nps_mux_create_stream(nps_mux_t *mux, uint16_t conn_id);

/**
 * Look up a stream by conn_id.
 */
nps_stream_t *nps_mux_find_stream(nps_mux_t *mux, uint16_t conn_id);

/**
 * Close and destroy a stream.
 */
void nps_mux_destroy_stream(nps_mux_t *mux, uint16_t conn_id);

/**
 * Send data on a stream (queues into the stream's send buffer).
 */
int nps_mux_send(nps_mux_t *mux, uint16_t conn_id, const uint8_t *data,
                 size_t len);

/**
 * Process one incoming packet: demux to the correct stream.
 * Called from the receive loop.
 */
int nps_mux_dispatch(nps_mux_t *mux, const nps_packet_t *pkt,
                     const struct sockaddr_in *from, socklen_t from_len);

/**
 * Transmit pending data for all active streams (one round).
 */
int nps_mux_transmit_all(nps_mux_t *mux);

/**
 * Get count of active streams.
 */
uint32_t nps_mux_active_count(const nps_mux_t *mux);

/**
 * Shut down the multiplexer and all streams.
 */
void nps_mux_shutdown(nps_mux_t *mux);

#endif /* NPS_MUX_H */
