/* rift_sender.c — sender state machine (SYN → ESTABLISHED → FIN) */

#include "rift_config.h"
#include "rift_congestion.h"
#include "rift_crc32.h"
#include "rift_log.h"
#include "rift_protocol.h"
#include "rift_rtt.h"
#include "rift_stats.h"
#include "rift_window.h"

#include <arpa/inet.h>
#include <errno.h>

#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ── Sender Context ───────────────────────────────────────────────── */

typedef struct {
  int sockfd;
  struct sockaddr_in peer_addr;
  rift_conn_state_t state;
  uint16_t conn_id;

  rift_window_t window;
  rift_congestion_t congestion;
  rift_rtt_estimator_t rtt;
  rift_stats_t stats;
  rift_throughput_t throughput;

  /* Send data */
  const uint8_t *data;
  size_t data_len;
  size_t data_offset;

  /* Configuration */
  uint32_t window_size;
  uint64_t connect_timeout_us;
} rift_sender_t;

/* ── Internal Helpers ─────────────────────────────────────────────── */

static int sender_send_packet(rift_sender_t *s, rift_packet_t *pkt) {
  uint8_t wire[RIFT_MAX_SERIALIZED_SIZE];
  int len = rift_packet_serialize(pkt, wire, sizeof(wire));
  if (len < 0) {
    RIFT_LOG_ERROR("Failed to serialize packet seq=%u", pkt->header.seq_num);
    return -1;
  }

  ssize_t sent = sendto(s->sockfd, wire, (size_t)len, 0,
                        (struct sockaddr *)&s->peer_addr, sizeof(s->peer_addr));
  if (sent < 0) {
    RIFT_LOG_ERROR("sendto failed: %s", strerror(errno));
    return -1;
  }

  RIFT_STAT_INC(&s->stats, packets_sent);
  RIFT_STAT_ADD(&s->stats, bytes_sent, (uint64_t)sent);

  return 0;
}

static int sender_recv_packet(rift_sender_t *s, rift_packet_t *pkt,
                              int timeout_ms) {
  struct pollfd pfd = {.fd = s->sockfd, .events = POLLIN};
  int ret = poll(&pfd, 1, timeout_ms);

  if (ret <= 0)
    return -1; /* Timeout or error */

  uint8_t wire[RIFT_MAX_SERIALIZED_SIZE];
  ssize_t rcvd = recvfrom(s->sockfd, wire, sizeof(wire), 0, NULL, NULL);
  if (rcvd <= 0)
    return -1;

  if (rift_packet_deserialize(wire, (size_t)rcvd, pkt) != 0) {
    RIFT_STAT_INC(&s->stats, packets_dropped_crc);
    RIFT_LOG_WARN("Received corrupted packet (CRC mismatch)");
    return -1;
  }

  RIFT_STAT_INC(&s->stats, packets_received);
  RIFT_STAT_ADD(&s->stats, bytes_received, (uint64_t)rcvd);

  return 0;
}

/* ── Connection Handshake ─────────────────────────────────────────── */

static int sender_connect(rift_sender_t *s) {
  RIFT_LOG_INFO("Connecting to %s:%d...", inet_ntoa(s->peer_addr.sin_addr),
               ntohs(s->peer_addr.sin_port));

  /* Send SYN */
  rift_packet_t syn;
  rift_packet_build(&syn, RIFT_PKT_SYN, RIFT_FLAG_SYN, s->window.base_seq, 0,
                   (uint16_t)s->window_size, s->conn_id, NULL, 0, NULL, 0);

  s->state = RIFT_STATE_SYN_SENT;

  for (int attempt = 0; attempt < RIFT_MAX_RETRIES; attempt++) {
    syn.header.ts_send = rift_timestamp_us();

    if (sender_send_packet(s, &syn) != 0)
      return -1;

    RIFT_LOG_DEBUG("SYN sent (attempt %d/%d)", attempt + 1, RIFT_MAX_RETRIES);

    /* Wait for SYN_ACK */
    int timeout = RIFT_RTO_INIT_MS * (1 << attempt);
    if (timeout > (int)(s->connect_timeout_us / 1000))
      timeout = (int)(s->connect_timeout_us / 1000);

    rift_packet_t response;
    if (sender_recv_packet(s, &response, timeout) == 0) {
      if (response.header.type == RIFT_PKT_SYN_ACK &&
          response.header.conn_id == s->conn_id) {

        /* Update RTT from SYN-ACK */
        uint64_t now = rift_timestamp_us();
        if (response.header.ts_echo > 0) {
          double rtt = (double)(now - response.header.ts_echo) / 1000.0;
          rift_rtt_update(&s->rtt, rtt);
          RIFT_LOG_DEBUG("Initial RTT: %.2f ms", rtt);
        }

        /* Send ACK to complete handshake */
        rift_packet_t ack;
        rift_packet_build(&ack, RIFT_PKT_ACK, RIFT_FLAG_ACK, s->window.base_seq,
                         response.header.seq_num + 1, (uint16_t)s->window_size,
                         s->conn_id, NULL, 0, NULL, 0);
        sender_send_packet(s, &ack);

        s->state = RIFT_STATE_ESTABLISHED;
        RIFT_STAT_INC(&s->stats, connections_opened);
        RIFT_LOG_INFO("Connection established (conn_id=%u)", s->conn_id);
        return 0;
      } else if (response.header.type == RIFT_PKT_RST) {
        RIFT_LOG_ERROR("Connection refused by peer");
        s->state = RIFT_STATE_CLOSED;
        return -1;
      }
    }
  }

  RIFT_LOG_ERROR("Connection timed out after %d attempts", RIFT_MAX_RETRIES);
  s->state = RIFT_STATE_CLOSED;
  return -1;
}

/* ── Data Transmission ────────────────────────────────────────────── */

static int sender_transmit(rift_sender_t *s) {
  RIFT_LOG_INFO("Starting data transfer: %zu bytes", s->data_len);

  uint64_t last_stats = rift_timestamp_us();

  while (s->data_offset < s->data_len || rift_window_in_flight(&s->window) > 0) {

    /* 1. Fill the send window with new data */
    while (s->data_offset < s->data_len && rift_window_can_send(&s->window)) {
      uint16_t chunk = RIFT_MAX_PAYLOAD;
      if (s->data_offset + chunk > s->data_len)
        chunk = (uint16_t)(s->data_len - s->data_offset);

      rift_packet_t pkt;
      rift_packet_build(&pkt, RIFT_PKT_DATA, 0, s->window.next_seq, 0,
                       (uint16_t)s->window_size, s->conn_id, NULL, 0,
                       s->data + s->data_offset, chunk);
      pkt.header.ts_send = rift_timestamp_us();

      uint64_t rto_us = (uint64_t)(s->rtt.rto_ms * 1000.0);
      if (rift_window_mark_sent(&s->window, &pkt, rto_us) != 0)
        break;

      sender_send_packet(s, &pkt);
      RIFT_STAT_ADD(&s->stats, bytes_payload_sent, chunk);

      s->data_offset += chunk;

      RIFT_LOG_TRACE("Sent DATA seq=%u len=%u (offset=%zu/%zu)",
                    pkt.header.seq_num, chunk, s->data_offset, s->data_len);
    }

    /* 2. Wait for ACKs */
    int poll_timeout = (int)(s->rtt.rto_ms);
    if (poll_timeout < 1)
      poll_timeout = 1;

    rift_packet_t response;
    while (sender_recv_packet(s, &response, poll_timeout) == 0) {
      uint64_t now = rift_timestamp_us();

      /* Process RTT from echo timestamp */
      if (response.header.ts_echo > 0) {
        double rtt = (double)(now - response.header.ts_echo) / 1000.0;
        if (rtt > 0 && rtt < 30000.0) {
          rift_rtt_update(&s->rtt, rtt);
        }
      }

      if (response.header.type == RIFT_PKT_ACK ||
          response.header.type == RIFT_PKT_SACK) {

        /* Process cumulative ACK */
        uint32_t acked =
            rift_window_process_ack(&s->window, response.header.ack_num);

        /* Process SACK blocks if present */
        if (response.header.sack_count > 0) {
          uint32_t sack_acked = rift_window_process_sack(
              &s->window, response.sack_blocks, response.header.sack_count);
          acked += sack_acked;
          RIFT_STAT_INC(&s->stats, sacks_received);
        }

        RIFT_STAT_INC(&s->stats, acks_received);

        /* Update congestion window */
        if (acked > 0) {
          rift_cc_on_ack(&s->congestion, acked, now);
          rift_window_set_cwnd(&s->window, rift_cc_get_cwnd(&s->congestion));
        }

        /* Check for duplicate ACKs → fast retransmit */
        if (s->window.dup_ack_count >= RIFT_DUP_ACK_THRESH) {
          RIFT_LOG_DEBUG("Triple dup ACK → fast retransmit (ack=%u)",
                        response.header.ack_num);
          RIFT_STAT_INC(&s->stats, dup_acks);
          rift_cc_on_loss(&s->congestion, false, now);
          rift_window_set_cwnd(&s->window, rift_cc_get_cwnd(&s->congestion));

          /* Retransmit the first unacked packet */
          rift_buffer_slot_t *slot =
              rift_buffer_get(&s->window.buffer, s->window.base_seq);
          if (slot) {
            slot->packet.header.ts_send = now;
            slot->retransmit_count++;
            slot->last_sent_us = now;
            sender_send_packet(s, &slot->packet);
            RIFT_STAT_INC(&s->stats, packets_retransmitted);
          }
          s->window.dup_ack_count = 0;
        }

        /* Update receiver window */
        rift_window_set_recv_window(&s->window, response.header.window_size);

        RIFT_LOG_TRACE("ACK ack=%u cwnd=%u in_flight=%u",
                      response.header.ack_num, rift_cc_get_cwnd(&s->congestion),
                      rift_window_in_flight(&s->window));

      } else if (response.header.type == RIFT_PKT_NACK) {
        RIFT_LOG_DEBUG("NACK for seq=%u", response.header.seq_num);
        RIFT_STAT_INC(&s->stats, nacks_received);
        rift_window_process_nack(&s->window, response.header.seq_num);

      } else if (response.header.type == RIFT_PKT_RST) {
        RIFT_LOG_ERROR("Connection reset by peer");
        s->state = RIFT_STATE_CLOSED;
        RIFT_STAT_INC(&s->stats, connections_reset);
        return -1;
      }

      poll_timeout = 0; /* Drain all pending packets */
    }

    /* 3. Check for retransmission timeouts */
    uint64_t now = rift_timestamp_us();
    uint32_t timed_out[64];
    uint32_t to_count =
        rift_window_check_timeouts(&s->window, now, timed_out, 64);

    for (uint32_t i = 0; i < to_count; i++) {
      rift_buffer_slot_t *slot = rift_buffer_get(&s->window.buffer, timed_out[i]);
      if (!slot)
        continue;

      if (slot->retransmit_count >= RIFT_MAX_RETRIES) {
        RIFT_LOG_ERROR("Max retries exceeded for seq=%u", timed_out[i]);
        RIFT_STAT_INC(&s->stats, packets_lost);
        continue;
      }

      RIFT_LOG_DEBUG("RTO timeout: retransmitting seq=%u (attempt %u)",
                    timed_out[i], slot->retransmit_count + 1);

      /* Congestion: mark timeout loss */
      rift_cc_on_loss(&s->congestion, true, now);
      rift_window_set_cwnd(&s->window, rift_cc_get_cwnd(&s->congestion));

      /* Exponential backoff on RTT */
      rift_rtt_backoff(&s->rtt);

      /* Retransmit */
      slot->packet.header.ts_send = now;
      slot->retransmit_count++;
      slot->last_sent_us = now;
      sender_send_packet(s, &slot->packet);
      RIFT_STAT_INC(&s->stats, packets_retransmitted);

      /* Update deadline */
      uint64_t new_rto_us = (uint64_t)(s->rtt.rto_ms * 1000.0);
      for (uint32_t j = 0; j < s->window.retx_count; j++) {
        if (s->window.retx_queue[j].seq_num == timed_out[i]) {
          s->window.retx_queue[j].deadline_us = now + new_rto_us;
          break;
        }
      }
    }

    /* 4. Periodic stats reporting */
    if (now - last_stats > RIFT_STATS_INTERVAL_MS * 1000ULL) {
      rift_throughput_sample_t sample =
          rift_throughput_sample(&s->throughput, &s->stats);
      RIFT_LOG_INFO("Throughput: %.2f Mbps | Goodput: %.2f Mbps | "
                   "PPS: %.0f | cwnd: %u | in_flight: %u | "
                   "phase: %s | RTO: %.1f ms",
                   sample.mbps, sample.goodput_mbps, sample.pps,
                   rift_cc_get_cwnd(&s->congestion),
                   rift_window_in_flight(&s->window),
                   rift_cc_phase_str(s->congestion.phase), s->rtt.rto_ms);
      last_stats = now;
    }
  }

  RIFT_LOG_INFO("All data transmitted and acknowledged");
  return 0;
}

/* ── Connection Teardown ──────────────────────────────────────────── */

static int sender_close(rift_sender_t *s) {
  RIFT_LOG_INFO("Closing connection...");

  rift_packet_t fin;
  rift_packet_build(&fin, RIFT_PKT_FIN, RIFT_FLAG_FIN, s->window.next_seq, 0, 0,
                   s->conn_id, NULL, 0, NULL, 0);

  s->state = RIFT_STATE_FIN_WAIT;

  for (int attempt = 0; attempt < RIFT_MAX_RETRIES; attempt++) {
    fin.header.ts_send = rift_timestamp_us();
    sender_send_packet(s, &fin);

    rift_packet_t response;
    int timeout = RIFT_RTO_INIT_MS * (1 << attempt);
    if (sender_recv_packet(s, &response, timeout) == 0) {
      if (response.header.type == RIFT_PKT_FIN_ACK) {
        s->state = RIFT_STATE_CLOSED;
        RIFT_STAT_INC(&s->stats, connections_closed);
        RIFT_LOG_INFO("Connection closed gracefully");
        return 0;
      }
    }
  }

  RIFT_LOG_WARN("Close timed out, forcing close");
  s->state = RIFT_STATE_CLOSED;
  RIFT_STAT_INC(&s->stats, connections_closed);
  return 0;
}

/* ── Public API ───────────────────────────────────────────────────── */

int rift_sender_run_ex(const char *host, uint16_t port, const uint8_t *data,
                      size_t data_len, uint32_t window_size,
                      rift_transfer_result_t *out_result) {
  rift_sender_t sender;
  memset(&sender, 0, sizeof(sender));

  /* Configuration */
  sender.window_size = window_size > 0 ? window_size : RIFT_WINDOW_SIZE;
  sender.connect_timeout_us = RIFT_CONNECT_TIMEOUT_MS * 1000ULL;
  sender.conn_id = (uint16_t)(rift_timestamp_us() & 0xFFFF);
  sender.data = data;
  sender.data_len = data_len;
  sender.data_offset = 0;

  /* Initialize subsystems */
  rift_crc32_init();
  rift_rtt_init(&sender.rtt);
  rift_cc_init(&sender.congestion);
  rift_stats_init(&sender.stats);
  rift_throughput_init(&sender.throughput);

  uint32_t initial_seq = (uint32_t)(rift_timestamp_us() & 0xFFFFFFFF);
  if (rift_window_init(&sender.window, initial_seq, sender.window_size) != 0) {
    RIFT_LOG_ERROR("Failed to initialize window");
    return -1;
  }

  /* Create UDP socket */
  sender.sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sender.sockfd < 0) {
    RIFT_LOG_ERROR("socket() failed: %s", strerror(errno));
    rift_window_destroy(&sender.window);
    return -1;
  }

  /* Set up peer address */
  memset(&sender.peer_addr, 0, sizeof(sender.peer_addr));
  sender.peer_addr.sin_family = AF_INET;
  sender.peer_addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &sender.peer_addr.sin_addr) <= 0) {
    RIFT_LOG_ERROR("Invalid address: %s", host);
    close(sender.sockfd);
    rift_window_destroy(&sender.window);
    return -1;
  }

  sender.state = RIFT_STATE_CLOSED;
  int result = -1;

  /* Connect → Send → Close */
  if (sender_connect(&sender) == 0) {
    if (sender_transmit(&sender) == 0) {
      result = 0;
    }
    sender_close(&sender);
  }

  /* Capture final statistics if requested */
  if (out_result) {
    rift_stats_t *s = &sender.stats;
    uint64_t now = rift_timestamp_us();
    double elapsed = (double)(now - s->start_time_us) / 1000000.0;

    out_result->elapsed_us = now - s->start_time_us;
    out_result->bytes_transferred = RIFT_STAT_GET(s, bytes_payload_sent);
    out_result->packets_sent = RIFT_STAT_GET(s, packets_sent);
    out_result->packets_lost = RIFT_STAT_GET(s, packets_lost);
    out_result->packets_retransmitted = RIFT_STAT_GET(s, packets_retransmitted);

    if (elapsed > 0) {
      out_result->throughput_mbps =
          (double)(RIFT_STAT_GET(s, bytes_sent) * 8) / (elapsed * 1000000.0);
      out_result->goodput_mbps =
          (double)(out_result->bytes_transferred * 8) / (elapsed * 1000000.0);
    }

    out_result->min_rtt_ms = sender.rtt.min_rtt_ms;
    out_result->max_rtt_ms = sender.rtt.max_rtt_ms;
    out_result->avg_srtt_ms = sender.rtt.srtt_ms;
    out_result->final_cwnd = (uint32_t)sender.congestion.cwnd;
    // Note: max_cwnd tracking would require adding a field to rift_congestion_t
    // or rift_sender_t
    out_result->max_cwnd = 0;
  }

  /* Final statistics log */
  RIFT_LOG_INFO("═══════ Final Statistics ═══════");
  rift_stats_report(&sender.stats);

  /* Cleanup */
  close(sender.sockfd);
  rift_window_destroy(&sender.window);

  return result;
}

int rift_sender_run(const char *host, uint16_t port, const uint8_t *data,
                   size_t data_len, uint32_t window_size) {
  return rift_sender_run_ex(host, port, data, data_len, window_size, NULL);
}
