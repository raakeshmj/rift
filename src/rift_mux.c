/* rift_mux.c — connection multiplexer */

#include "rift_mux.h"
#include "rift_crc32.h"
#include "rift_log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* ── Initialization ──────────────────────────────────────────────── */

int rift_mux_init(rift_mux_t *mux, int sockfd) {
  memset(mux, 0, sizeof(*mux));
  mux->sockfd = sockfd;
  mux->running = true;
  pthread_mutex_init(&mux->lock, NULL);
  return 0;
}

/* ── Stream Management ───────────────────────────────────────────── */

rift_stream_t *rift_mux_create_stream(rift_mux_t *mux, uint16_t conn_id) {
  pthread_mutex_lock(&mux->lock);

  /* Check for duplicate */
  for (uint32_t i = 0; i < RIFT_MAX_CONNECTIONS; i++) {
    if (mux->streams[i].active && mux->streams[i].conn_id == conn_id) {
      RIFT_LOG_WARN("Stream %u already exists", conn_id);
      pthread_mutex_unlock(&mux->lock);
      return &mux->streams[i];
    }
  }

  /* Find empty slot */
  rift_stream_t *stream = NULL;
  for (uint32_t i = 0; i < RIFT_MAX_CONNECTIONS; i++) {
    if (!mux->streams[i].active) {
      stream = &mux->streams[i];
      break;
    }
  }

  if (!stream) {
    RIFT_LOG_ERROR("Max streams reached (%u)", RIFT_MAX_CONNECTIONS);
    pthread_mutex_unlock(&mux->lock);
    return NULL;
  }

  memset(stream, 0, sizeof(*stream));
  stream->conn_id = conn_id;
  stream->state = RIFT_STREAM_OPEN;
  stream->active = true;
  stream->next_seq = 1;
  stream->expected_seq = 1;

  /* Initialize per-stream protocol state */
  if (rift_window_init(&stream->window, 1, RIFT_WINDOW_SIZE) != 0) {
    RIFT_LOG_ERROR("Failed to init window for stream %u", conn_id);
    stream->active = false;
    pthread_mutex_unlock(&mux->lock);
    return NULL;
  }
  rift_cc_init(&stream->congestion);
  rift_rtt_init(&stream->rtt);
  rift_stats_init(&stream->stats);

  mux->stream_count++;
  RIFT_LOG_INFO("Stream %u created (total: %u)", conn_id, mux->stream_count);

  pthread_mutex_unlock(&mux->lock);
  return stream;
}

rift_stream_t *rift_mux_find_stream(rift_mux_t *mux, uint16_t conn_id) {
  for (uint32_t i = 0; i < RIFT_MAX_CONNECTIONS; i++) {
    if (mux->streams[i].active && mux->streams[i].conn_id == conn_id) {
      return &mux->streams[i];
    }
  }
  return NULL;
}

void rift_mux_destroy_stream(rift_mux_t *mux, uint16_t conn_id) {
  pthread_mutex_lock(&mux->lock);

  rift_stream_t *stream = rift_mux_find_stream(mux, conn_id);
  if (!stream) {
    pthread_mutex_unlock(&mux->lock);
    return;
  }

  rift_window_destroy(&stream->window);
  if (stream->send_buf) {
    free(stream->send_buf);
    stream->send_buf = NULL;
  }

  stream->active = false;
  stream->state = RIFT_STREAM_CLOSED;
  mux->stream_count--;

  RIFT_LOG_INFO("Stream %u destroyed (remaining: %u)", conn_id,
               mux->stream_count);

  pthread_mutex_unlock(&mux->lock);
}

/* ── Send ────────────────────────────────────────────────────────── */

int rift_mux_send(rift_mux_t *mux, uint16_t conn_id, const uint8_t *data,
                 size_t len) {
  pthread_mutex_lock(&mux->lock);

  rift_stream_t *stream = rift_mux_find_stream(mux, conn_id);
  if (!stream || stream->state != RIFT_STREAM_OPEN) {
    pthread_mutex_unlock(&mux->lock);
    return -1;
  }

  /* Copy data into stream's send buffer */
  stream->send_buf = malloc(len);
  if (!stream->send_buf) {
    pthread_mutex_unlock(&mux->lock);
    return -1;
  }
  memcpy(stream->send_buf, data, len);
  stream->send_len = len;
  stream->send_offset = 0;

  pthread_mutex_unlock(&mux->lock);
  return 0;
}

/* ── Transmit All Streams ────────────────────────────────────────── */

int rift_mux_transmit_all(rift_mux_t *mux) {
  int total_sent = 0;

  pthread_mutex_lock(&mux->lock);

  for (uint32_t i = 0; i < RIFT_MAX_CONNECTIONS; i++) {
    rift_stream_t *s = &mux->streams[i];
    if (!s->active || s->state != RIFT_STREAM_OPEN)
      continue;
    if (!s->send_buf || s->send_offset >= s->send_len)
      continue;

    /* Send up to cwnd packets for this stream */
    uint32_t can_send = rift_cc_get_cwnd(&s->congestion);
    uint32_t in_flight = s->window.next_seq > s->window.base_seq
                             ? s->window.next_seq - s->window.base_seq
                             : 0;

    if (in_flight >= can_send)
      continue;

    uint32_t budget = can_send - in_flight;

    for (uint32_t p = 0; p < budget && s->send_offset < s->send_len; p++) {
      size_t remain = s->send_len - s->send_offset;
      uint16_t plen =
          (uint16_t)(remain > RIFT_MAX_PAYLOAD ? RIFT_MAX_PAYLOAD : remain);

      rift_packet_t pkt;
      rift_packet_build(&pkt, RIFT_PKT_DATA, 0, s->next_seq, 0, RIFT_WINDOW_SIZE,
                       s->conn_id, NULL, 0, s->send_buf + s->send_offset, plen);
      pkt.header.ts_send = rift_timestamp_us();

      uint8_t wire[RIFT_MAX_SERIALIZED_SIZE];
      int len = rift_packet_serialize(&pkt, wire, sizeof(wire));
      if (len > 0) {
        ssize_t sent =
            sendto(mux->sockfd, wire, (size_t)len, 0,
                   (struct sockaddr *)&mux->peer_addr, mux->peer_len);
        if (sent > 0) {
          RIFT_STAT_INC(&s->stats, packets_sent);
          RIFT_STAT_ADD(&s->stats, bytes_sent, (uint64_t)sent);
          s->next_seq++;
          s->send_offset += plen;
          total_sent++;
        }
      }
    }
  }

  pthread_mutex_unlock(&mux->lock);
  return total_sent;
}

/* ── Dispatch Incoming Packet ────────────────────────────────────── */

int rift_mux_dispatch(rift_mux_t *mux, const rift_packet_t *pkt,
                     const struct sockaddr_in *from, socklen_t from_len) {
  uint16_t conn_id = pkt->header.conn_id;

  /* Remember peer address */
  if (!mux->has_peer) {
    mux->peer_addr = *from;
    mux->peer_len = from_len;
    mux->has_peer = true;
  }

  pthread_mutex_lock(&mux->lock);

  rift_stream_t *stream = rift_mux_find_stream(mux, conn_id);

  if (!stream) {
    /* Auto-create stream for unknown conn_id (server mode) */
    pthread_mutex_unlock(&mux->lock);
    stream = rift_mux_create_stream(mux, conn_id);
    if (!stream)
      return -1;
    pthread_mutex_lock(&mux->lock);
  }

  RIFT_STAT_INC(&stream->stats, packets_received);
  RIFT_STAT_ADD(&stream->stats, bytes_received,
               (uint64_t)pkt->header.payload_len);

  switch (pkt->header.type) {
  case RIFT_PKT_DATA: {
    uint32_t seq = pkt->header.seq_num;

    if (seq == stream->expected_seq) {
      /* In-order: deliver to recv buffer */
      if (stream->recv_buf && pkt->header.payload_len > 0) {
        size_t space = stream->recv_buf_size - stream->recv_offset;
        uint16_t copy_len = pkt->header.payload_len;
        if (copy_len > space)
          copy_len = (uint16_t)space;
        memcpy(stream->recv_buf + stream->recv_offset, pkt->payload, copy_len);
        stream->recv_offset += copy_len;
      }
      stream->expected_seq++;
    }
    /* Out-of-order or duplicate: just ACK expected */

    /* Send ACK */
    rift_packet_t ack;
    rift_packet_build(&ack, RIFT_PKT_ACK, RIFT_FLAG_ACK, 0, stream->expected_seq,
                     RIFT_WINDOW_SIZE, conn_id, NULL, 0, NULL, 0);
    ack.header.ts_send = rift_timestamp_us();
    ack.header.ts_echo = pkt->header.ts_send;

    uint8_t wire[RIFT_MAX_SERIALIZED_SIZE];
    int len = rift_packet_serialize(&ack, wire, sizeof(wire));
    if (len > 0) {
      sendto(mux->sockfd, wire, (size_t)len, 0, (struct sockaddr *)from,
             from_len);
      RIFT_STAT_INC(&stream->stats, acks_sent);
    }
    break;
  }

  case RIFT_PKT_ACK: {
    /* Process ACK for sender side */
    if (pkt->header.ack_num > stream->window.base_seq) {
      rift_window_process_ack(&stream->window, pkt->header.ack_num);
      rift_cc_on_ack(&stream->congestion, 1, rift_timestamp_us());

      /* RTT measurement */
      if (pkt->header.ts_echo > 0) {
        double rtt =
            rift_rtt_from_timestamps(rift_timestamp_us(), pkt->header.ts_echo);
        rift_rtt_update(&stream->rtt, rtt);
      }
    }
    RIFT_STAT_INC(&stream->stats, acks_received);
    break;
  }

  default:
    break;
  }

  pthread_mutex_unlock(&mux->lock);
  return 0;
}

/* ── Utility ─────────────────────────────────────────────────────── */

uint32_t rift_mux_active_count(const rift_mux_t *mux) {
  return mux->stream_count;
}

void rift_mux_shutdown(rift_mux_t *mux) {
  pthread_mutex_lock(&mux->lock);
  mux->running = false;

  for (uint32_t i = 0; i < RIFT_MAX_CONNECTIONS; i++) {
    if (mux->streams[i].active) {
      rift_window_destroy(&mux->streams[i].window);
      if (mux->streams[i].send_buf)
        free(mux->streams[i].send_buf);
      mux->streams[i].active = false;
    }
  }
  mux->stream_count = 0;

  pthread_mutex_unlock(&mux->lock);
  pthread_mutex_destroy(&mux->lock);

  RIFT_LOG_INFO("Multiplexer shut down");
}
