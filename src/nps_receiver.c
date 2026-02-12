/* nps_receiver.c — receiver state machine (LISTEN → ESTABLISHED → CLOSE_WAIT)
 */

#include "nps_config.h"
#include "nps_crc32.h"
#include "nps_log.h"
#include "nps_protocol.h"
#include "nps_stats.h"
#include "nps_window.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ── Data Callback ────────────────────────────────────────────────── */

typedef void (*nps_recv_callback_t)(const uint8_t *data, uint16_t len,
                                    uint32_t seq_num, void *user_data);

/* ── Receiver Context ─────────────────────────────────────────────── */

typedef struct {
  int sockfd;
  uint16_t port;
  struct sockaddr_in bind_addr;
  struct sockaddr_in peer_addr;
  socklen_t peer_addr_len;
  bool has_peer;
  nps_conn_state_t state;
  uint16_t conn_id;

  nps_window_t window;
  nps_stats_t stats;
  nps_throughput_t throughput;

  /* Expected next sequence (cumulative ACK point) */
  uint32_t expected_seq;

  /* Application data buffer */
  uint8_t *recv_buf;
  size_t recv_buf_size;
  size_t recv_buf_offset;

  /* Configuration */
  uint32_t window_size;

  /* Callback for delivered data */
  nps_recv_callback_t callback;
  void *callback_data;
} nps_receiver_t;

/* ── Internal Helpers ─────────────────────────────────────────────── */

static int receiver_send_packet(nps_receiver_t *r, nps_packet_t *pkt) {
  uint8_t wire[NPS_MAX_SERIALIZED_SIZE];
  int len = nps_packet_serialize(pkt, wire, sizeof(wire));
  if (len < 0)
    return -1;

  ssize_t sent = sendto(r->sockfd, wire, (size_t)len, 0,
                        (struct sockaddr *)&r->peer_addr, r->peer_addr_len);
  if (sent < 0) {
    NPS_LOG_ERROR("sendto failed: %s", strerror(errno));
    return -1;
  }

  NPS_STAT_INC(&r->stats, packets_sent);
  NPS_STAT_ADD(&r->stats, bytes_sent, (uint64_t)sent);
  return 0;
}

static int receiver_recv_packet(nps_receiver_t *r, nps_packet_t *pkt,
                                int timeout_ms) {
  struct pollfd pfd = {.fd = r->sockfd, .events = POLLIN};
  int ret = poll(&pfd, 1, timeout_ms);

  if (ret <= 0)
    return -1;

  uint8_t wire[NPS_MAX_SERIALIZED_SIZE];
  struct sockaddr_in from;
  socklen_t from_len = sizeof(from);

  ssize_t rcvd = recvfrom(r->sockfd, wire, sizeof(wire), 0,
                          (struct sockaddr *)&from, &from_len);
  if (rcvd <= 0)
    return -1;

  if (nps_packet_deserialize(wire, (size_t)rcvd, pkt) != 0) {
    NPS_STAT_INC(&r->stats, packets_dropped_crc);
    NPS_LOG_WARN("CRC mismatch, dropping packet");
    return -1;
  }

  /* Remember peer address from first valid packet */
  if (!r->has_peer) {
    r->peer_addr = from;
    r->peer_addr_len = from_len;
    r->has_peer = true;
  }

  NPS_STAT_INC(&r->stats, packets_received);
  NPS_STAT_ADD(&r->stats, bytes_received, (uint64_t)rcvd);
  return 0;
}

static void receiver_send_ack(nps_receiver_t *r, uint64_t echo_ts) {
  /* Build SACK blocks for out-of-order data */
  nps_sack_block_t sack_blocks[NPS_MAX_SACK_BLOCKS];
  uint16_t sack_count = nps_window_build_sack_blocks(
      &r->window, r->expected_seq, sack_blocks, NPS_MAX_SACK_BLOCKS);

  uint8_t type = (sack_count > 0) ? NPS_PKT_SACK : NPS_PKT_ACK;
  uint16_t flags = NPS_FLAG_ACK | (sack_count > 0 ? NPS_FLAG_SACK : 0);

  nps_packet_t ack;
  nps_packet_build(&ack, type, flags, 0, r->expected_seq,
                   (uint16_t)r->window_size, r->conn_id,
                   sack_count > 0 ? sack_blocks : NULL, sack_count, NULL, 0);
  ack.header.ts_send = nps_timestamp_us();
  ack.header.ts_echo = echo_ts;

  receiver_send_packet(r, &ack);
  NPS_STAT_INC(&r->stats, acks_sent);
  if (sack_count > 0)
    NPS_STAT_INC(&r->stats, sacks_sent);

  NPS_LOG_TRACE("Sent %s ack=%u sack_blocks=%u",
                type == NPS_PKT_SACK ? "SACK" : "ACK", r->expected_seq,
                sack_count);
}

static void receiver_deliver_inorder(nps_receiver_t *r) {
  /* Deliver consecutive in-order packets from expected_seq */
  while (1) {
    nps_buffer_slot_t *slot =
        nps_buffer_get(&r->window.buffer, r->expected_seq);
    if (!slot || slot->state != NPS_SLOT_RECVD)
      break;

    /* Deliver to application */
    uint16_t plen = slot->packet.header.payload_len;
    if (r->callback && plen > 0) {
      r->callback(slot->packet.payload, plen, r->expected_seq,
                  r->callback_data);
    }

    /* Copy to receive buffer */
    if (r->recv_buf && plen > 0 &&
        r->recv_buf_offset + plen <= r->recv_buf_size) {
      memcpy(r->recv_buf + r->recv_buf_offset, slot->packet.payload, plen);
      r->recv_buf_offset += plen;
    }

    NPS_STAT_ADD(&r->stats, bytes_payload_received, plen);

    nps_buffer_remove(&r->window.buffer, r->expected_seq);
    r->expected_seq++;
  }
}

/* ── Connection Accept ────────────────────────────────────────────── */

static int receiver_accept(nps_receiver_t *r) {
  NPS_LOG_INFO("Listening on port %d...", ntohs(r->bind_addr.sin_port));
  r->state = NPS_STATE_LISTEN;

  while (1) {
    nps_packet_t pkt;
    if (receiver_recv_packet(r, &pkt, -1) != 0)
      continue;

    if (pkt.header.type == NPS_PKT_SYN) {
      r->conn_id = pkt.header.conn_id;
      r->expected_seq = pkt.header.seq_num + 1;
      r->state = NPS_STATE_SYN_RCVD;

      NPS_LOG_INFO("SYN received from %s:%d (conn_id=%u)",
                   inet_ntoa(r->peer_addr.sin_addr),
                   ntohs(r->peer_addr.sin_port), r->conn_id);

      /* Initialize window for receiving */
      if (nps_window_init(&r->window, r->expected_seq, r->window_size) != 0) {
        NPS_LOG_ERROR("Failed to initialize receive window");
        return -1;
      }

      /* Send SYN_ACK */
      uint32_t our_seq = (uint32_t)(nps_timestamp_us() & 0xFFFFFFFF);
      nps_packet_t syn_ack;
      nps_packet_build(&syn_ack, NPS_PKT_SYN_ACK, NPS_FLAG_SYN | NPS_FLAG_ACK,
                       our_seq, r->expected_seq, (uint16_t)r->window_size,
                       r->conn_id, NULL, 0, NULL, 0);
      syn_ack.header.ts_send = nps_timestamp_us();
      syn_ack.header.ts_echo = pkt.header.ts_send;

      for (int attempt = 0; attempt < NPS_MAX_RETRIES; attempt++) {
        receiver_send_packet(r, &syn_ack);

        nps_packet_t response;
        int timeout = NPS_RTO_INIT_MS * (1 << attempt);
        if (receiver_recv_packet(r, &response, timeout) == 0) {
          if (response.header.type == NPS_PKT_ACK &&
              response.header.conn_id == r->conn_id) {
            r->state = NPS_STATE_ESTABLISHED;
            NPS_STAT_INC(&r->stats, connections_opened);
            NPS_LOG_INFO("Connection established");
            return 0;
          } else if (response.header.type == NPS_PKT_DATA) {
            /* Peer may have skipped the ACK and started sending */
            r->state = NPS_STATE_ESTABLISHED;
            NPS_STAT_INC(&r->stats, connections_opened);
            NPS_LOG_INFO("Connection established (implicit ACK)");

            /* Process this data packet */
            goto process_data;
          }
        }
      }

      NPS_LOG_ERROR("Handshake timed out");
      r->state = NPS_STATE_CLOSED;
      return -1;

    process_data:
      /* The data packet received during handshake is in `response`;
         the main receive loop will handle subsequent packets. */
      return 0;
    }
  }
}

/* ── Data Reception ───────────────────────────────────────────────── */

static int receiver_receive(nps_receiver_t *r) {
  NPS_LOG_INFO("Receiving data...");

  uint64_t last_stats = nps_timestamp_us();
  uint64_t idle_start = nps_timestamp_us();

  while (r->state == NPS_STATE_ESTABLISHED) {
    nps_packet_t pkt;
    if (receiver_recv_packet(r, &pkt, 100) != 0) {
      /* Check idle timeout */
      if (nps_timestamp_us() - idle_start > NPS_KEEPALIVE_MS * 1000ULL * 3) {
        NPS_LOG_WARN("Idle timeout, peer may have disconnected");
        break;
      }
      continue;
    }

    idle_start = nps_timestamp_us();

    if (pkt.header.conn_id != r->conn_id) {
      NPS_LOG_WARN("Wrong conn_id: %u (expected %u)", pkt.header.conn_id,
                   r->conn_id);
      continue;
    }

    switch (pkt.header.type) {
    case NPS_PKT_DATA: {
      uint32_t seq = pkt.header.seq_num;

      if (seq == r->expected_seq) {
        /* In-order packet */
        nps_buffer_insert(&r->window.buffer, &pkt, NPS_SLOT_RECVD);
        receiver_deliver_inorder(r);

      } else if (seq > r->expected_seq) {
        /* Out-of-order: buffer it */
        if (nps_buffer_get(&r->window.buffer, seq) == NULL) {
          nps_buffer_insert(&r->window.buffer, &pkt, NPS_SLOT_RECVD);
          NPS_LOG_DEBUG("Out-of-order: seq=%u expected=%u", seq,
                        r->expected_seq);
        }
      }
      /* else: duplicate, ignore */

      /* Send ACK (with SACK if there are gaps) */
      receiver_send_ack(r, pkt.header.ts_send);
      break;
    }

    case NPS_PKT_FIN: {
      NPS_LOG_INFO("FIN received, closing connection");
      r->state = NPS_STATE_CLOSE_WAIT;

      /* Send FIN_ACK */
      nps_packet_t fin_ack;
      nps_packet_build(&fin_ack, NPS_PKT_FIN_ACK, NPS_FLAG_FIN | NPS_FLAG_ACK,
                       0, pkt.header.seq_num + 1, 0, r->conn_id, NULL, 0, NULL,
                       0);
      fin_ack.header.ts_echo = pkt.header.ts_send;
      receiver_send_packet(r, &fin_ack);

      r->state = NPS_STATE_CLOSED;
      NPS_STAT_INC(&r->stats, connections_closed);
      NPS_LOG_INFO("Connection closed gracefully");
      return 0;
    }

    case NPS_PKT_RST:
      NPS_LOG_WARN("RST received");
      r->state = NPS_STATE_CLOSED;
      NPS_STAT_INC(&r->stats, connections_reset);
      return -1;

    case NPS_PKT_PING: {
      nps_packet_t pong;
      nps_packet_build(&pong, NPS_PKT_PONG, 0, 0, 0, 0, r->conn_id, NULL, 0,
                       NULL, 0);
      pong.header.ts_echo = pkt.header.ts_send;
      receiver_send_packet(r, &pong);
      break;
    }

    default:
      NPS_LOG_DEBUG("Ignoring packet type: %s",
                    nps_pkt_type_str(pkt.header.type));
      break;
    }

    /* Periodic stats */
    uint64_t now = nps_timestamp_us();
    if (now - last_stats > NPS_STATS_INTERVAL_MS * 1000ULL) {
      nps_throughput_sample_t sample =
          nps_throughput_sample(&r->throughput, &r->stats);
      NPS_LOG_INFO("Recv throughput: %.2f Mbps | PPS: %.0f | "
                   "delivered up to seq=%u",
                   sample.mbps, sample.pps, r->expected_seq);
      last_stats = now;
    }
  }

  return 0;
}

/* ── Public API ───────────────────────────────────────────────────── */

int nps_receiver_run_ex(uint16_t port, uint8_t *recv_buf, size_t recv_buf_size,
                        size_t *bytes_received, uint32_t window_size,
                        nps_transfer_result_t *out_result) {
  nps_receiver_t receiver;
  memset(&receiver, 0, sizeof(receiver));

  /* Configuration */
  receiver.port = port;
  receiver.window_size = window_size > 0 ? window_size : NPS_WINDOW_SIZE;
  receiver.recv_buf = recv_buf;
  receiver.recv_buf_size = recv_buf_size;
  receiver.recv_buf_offset = 0;

  /* Initialize subsystems */
  nps_crc32_init();
  nps_stats_init(&receiver.stats);
  nps_throughput_init(&receiver.throughput);

  /* Initialize window - sequence number will be set on SYN */
  if (nps_window_init(&receiver.window, 0, receiver.window_size) != 0) {
    NPS_LOG_ERROR("Failed to initialize window");
    return -1;
  }

  /* Create UDP socket */
  receiver.sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (receiver.sockfd < 0) {
    NPS_LOG_ERROR("socket() failed: %s", strerror(errno));
    nps_window_destroy(&receiver.window);
    return -1;
  }

  /* Bind socket */
  memset(&receiver.bind_addr, 0, sizeof(receiver.bind_addr));
  receiver.bind_addr.sin_family = AF_INET;
  receiver.bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  receiver.bind_addr.sin_port = htons(port);

  int reused = 1;
  setsockopt(receiver.sockfd, SOL_SOCKET, SO_REUSEADDR, &reused,
             sizeof(reused));

  if (bind(receiver.sockfd, (struct sockaddr *)&receiver.bind_addr,
           sizeof(receiver.bind_addr)) < 0) {
    NPS_LOG_ERROR("bind() failed: %s", strerror(errno));
    close(receiver.sockfd);
    nps_window_destroy(&receiver.window);
    return -1;
  }

  receiver.state = NPS_STATE_LISTEN;
  int result = -1;

  /* Listen → Accept → Receive */
  if (receiver_accept(&receiver) == 0) {
    if (receiver_receive(&receiver) == 0) {
      result = 0;
    }
  }

  if (bytes_received) {
    *bytes_received = receiver.recv_buf_offset;
  }

  /* Capture final statistics if requested */
  if (out_result) {
    nps_stats_t *s = &receiver.stats;
    uint64_t now = nps_timestamp_us();
    double elapsed = (double)(now - s->start_time_us) / 1000000.0;

    out_result->elapsed_us = now - s->start_time_us;
    out_result->bytes_transferred = receiver.recv_buf_offset;
    out_result->packets_sent = NPS_STAT_GET(s, packets_sent); // ACKs
    out_result->packets_lost =
        NPS_STAT_GET(s, packets_lost); // Not tracked on RX usually
    out_result->packets_retransmitted = 0;

    if (elapsed > 0) {
      out_result->throughput_mbps =
          (double)(NPS_STAT_GET(s, bytes_received) * 8) / (elapsed * 1000000.0);
      out_result->goodput_mbps =
          (double)(receiver.recv_buf_offset * 8) / (elapsed * 1000000.0);
    }
  }

  /* Final statistics */
  NPS_LOG_INFO("═══════ Final Statistics ═══════");
  nps_stats_report(&receiver.stats);

  /* Cleanup */
  close(receiver.sockfd);
  nps_window_destroy(&receiver.window);

  return result;
}

int nps_receiver_run(uint16_t port, uint8_t *recv_buf, size_t recv_buf_size,
                     size_t *bytes_received, uint32_t window_size) {
  return nps_receiver_run_ex(port, recv_buf, recv_buf_size, bytes_received,
                             window_size, NULL);
}
