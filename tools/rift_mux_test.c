/* rift_mux_test.c — multiplexed stream test (4 × 256 KB concurrent) */

#include "rift_config.h"
#include "rift_crc32.h"
#include "rift_log.h"
#include "rift_mux.h"
#include "rift_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NUM_STREAMS 4
#define STREAM_DATA_SIZE (256 * 1024) /* 256 KB per stream */
#define BASE_PORT 10100

/* ── Test Data ───────────────────────────────────────────────────── */

static uint8_t send_data[NUM_STREAMS][STREAM_DATA_SIZE];
static uint8_t recv_data[NUM_STREAMS][STREAM_DATA_SIZE];

/* ── Receiver Thread ─────────────────────────────────────────────── */

typedef struct {
  rift_mux_t *mux;
  int sockfd;
  volatile int *running;
} recv_thread_arg_t;

static void *mux_recv_thread(void *arg) {
  recv_thread_arg_t *rta = (recv_thread_arg_t *)arg;
  uint8_t buf[RIFT_MAX_SERIALIZED_SIZE];

  while (*rta->running) {
    struct pollfd pfd = {.fd = rta->sockfd, .events = POLLIN};
    int ret = poll(&pfd, 1, 100);
    if (ret <= 0)
      continue;

    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    ssize_t n = recvfrom(rta->sockfd, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &from_len);
    if (n <= 0)
      continue;

    rift_packet_t pkt;
    if (rift_packet_deserialize(buf, (size_t)n, &pkt) != 0)
      continue;

    rift_mux_dispatch(rta->mux, &pkt, &from, from_len);
  }

  return NULL;
}

/* ── Sender Thread ───────────────────────────────────────────────── */

typedef struct {
  rift_mux_t *mux;
  volatile int *running;
} send_thread_arg_t;

static void *mux_send_thread(void *arg) {
  send_thread_arg_t *sta = (send_thread_arg_t *)arg;

  while (*sta->running) {
    int sent = rift_mux_transmit_all(sta->mux);
    if (sent == 0)
      usleep(1000); /* Nothing to send, back off */
  }

  return NULL;
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(void) {
  rift_crc32_init();
  rift_log_init(RIFT_LOG_INFO, NULL);

  printf("\n╔═══════════════════════════════════════════════════════╗\n");
  printf("║       RIFT Connection Multiplexing Test              ║\n");
  printf("╠═══════════════════════════════════════════════════════╣\n");
  printf("║  Streams:   %d                                       ║\n",
         NUM_STREAMS);
  printf("║  Data/stream: %d KB                                 ║\n",
         STREAM_DATA_SIZE / 1024);
  printf("╚═══════════════════════════════════════════════════════╝\n\n");

  /* Generate unique test data per stream */
  for (int i = 0; i < NUM_STREAMS; i++) {
    for (int j = 0; j < STREAM_DATA_SIZE; j++) {
      send_data[i][j] = (uint8_t)((i * 37 + j * 13) & 0xFF);
    }
    memset(recv_data[i], 0, STREAM_DATA_SIZE);
  }

  /* Create socket pair */
  int send_fd = socket(AF_INET, SOCK_DGRAM, 0);
  int recv_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (send_fd < 0 || recv_fd < 0) {
    RIFT_LOG_ERROR("socket() failed: %s", strerror(errno));
    return 1;
  }

  int reuse = 1;
  setsockopt(recv_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in recv_addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = INADDR_ANY,
      .sin_port = htons(BASE_PORT),
  };

  if (bind(recv_fd, (struct sockaddr *)&recv_addr, sizeof(recv_addr)) != 0) {
    RIFT_LOG_ERROR("bind() failed: %s", strerror(errno));
    return 1;
  }

  struct sockaddr_in send_target = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(0x7f000001),
      .sin_port = htons(BASE_PORT),
  };

  /* Initialize sender and receiver multiplexers */
  rift_mux_t sender_mux, receiver_mux;
  rift_mux_init(&sender_mux, send_fd);
  rift_mux_init(&receiver_mux, recv_fd);

  /* Set peer address on sender */
  sender_mux.peer_addr = send_target;
  sender_mux.peer_len = sizeof(send_target);
  sender_mux.has_peer = true;

  /* Create streams and queue data */
  for (int i = 0; i < NUM_STREAMS; i++) {
    uint16_t cid = (uint16_t)(100 + i);

    rift_stream_t *ss = rift_mux_create_stream(&sender_mux, cid);
    if (!ss) {
      RIFT_LOG_ERROR("Failed to create sender stream %u", cid);
      return 1;
    }
    rift_mux_send(&sender_mux, cid, send_data[i], STREAM_DATA_SIZE);

    rift_stream_t *rs = rift_mux_create_stream(&receiver_mux, cid);
    if (!rs) {
      RIFT_LOG_ERROR("Failed to create receiver stream %u", cid);
      return 1;
    }
    rs->recv_buf = recv_data[i];
    rs->recv_buf_size = STREAM_DATA_SIZE;
    rs->recv_offset = 0;
  }

  /* Start threads */
  volatile int running = 1;

  recv_thread_arg_t rta = {
      .mux = &receiver_mux, .sockfd = recv_fd, .running = &running};
  send_thread_arg_t sta = {.mux = &sender_mux, .running = &running};

  pthread_t recv_tid, send_tid;
  pthread_create(&recv_tid, NULL, mux_recv_thread, &rta);
  pthread_create(&send_tid, NULL, mux_send_thread, &sta);

  uint64_t start = rift_timestamp_us();

  /* Wait for all streams to finish sending (or timeout) */
  uint64_t timeout_us = 30 * 1000000ULL; /* 30 seconds */
  while (rift_timestamp_us() - start < timeout_us) {
    bool all_done = true;
    for (int i = 0; i < NUM_STREAMS; i++) {
      rift_stream_t *s = rift_mux_find_stream(&sender_mux, (uint16_t)(100 + i));
      if (s && s->send_offset < s->send_len) {
        all_done = false;
        break;
      }
    }
    if (all_done)
      break;
    usleep(100000); /* 100ms check interval */
  }

  /* Allow time for final ACKs */
  usleep(500000);
  running = 0;
  pthread_join(send_tid, NULL);
  pthread_join(recv_tid, NULL);

  uint64_t elapsed = rift_timestamp_us() - start;
  double elapsed_sec = (double)elapsed / 1000000.0;

  /* ── Results ──────────────────────────────────────────────────── */
  printf("\n═══════════════════════════════════════════════════════\n");
  printf("  Per-Stream Results:\n");
  printf("  ─────────────────────────────────────────────────────\n");

  int total_pass = 0;
  uint64_t total_bytes_sent = 0;

  for (int i = 0; i < NUM_STREAMS; i++) {
    uint16_t cid = (uint16_t)(100 + i);

    rift_stream_t *ss = rift_mux_find_stream(&sender_mux, cid);
    rift_stream_t *rs = rift_mux_find_stream(&receiver_mux, cid);

    uint64_t pkts_sent = ss ? RIFT_STAT_GET(&ss->stats, packets_sent) : 0;
    uint64_t pkts_recv = rs ? RIFT_STAT_GET(&rs->stats, packets_received) : 0;
    size_t recv_bytes = rs ? rs->recv_offset : 0;

    /* Verify data integrity for received portion */
    bool match = true;
    if (recv_bytes > 0) {
      match = (memcmp(send_data[i], recv_data[i], recv_bytes) == 0);
    }

    double stream_mbps =
        (double)(pkts_sent * RIFT_MAX_PAYLOAD * 8) / (elapsed_sec * 1000000.0);

    printf("  Stream %u: sent=%" PRIu64 " recv=%" PRIu64 " bytes=%zu/%d "
           "%.2f Mbps %s\n",
           cid, pkts_sent, pkts_recv, recv_bytes, STREAM_DATA_SIZE, stream_mbps,
           match ? "✅ PASS" : "❌ FAIL");

    if (match && recv_bytes > 0)
      total_pass++;
    total_bytes_sent += pkts_sent * RIFT_MAX_PAYLOAD;
  }

  double agg_mbps = (double)(total_bytes_sent * 8) / (elapsed_sec * 1000000.0);

  printf("  ─────────────────────────────────────────────────────\n");
  printf("  Aggregate:  %.2f Mbps  |  %d/%d streams verified\n", agg_mbps,
         total_pass, NUM_STREAMS);
  printf("  Duration:   %.2f seconds\n", elapsed_sec);
  printf("═══════════════════════════════════════════════════════\n\n");

  /* Cleanup */
  rift_mux_shutdown(&sender_mux);
  rift_mux_shutdown(&receiver_mux);
  close(send_fd);
  close(recv_fd);
  rift_log_shutdown();

  return (total_pass > 0) ? 0 : 1;
}
