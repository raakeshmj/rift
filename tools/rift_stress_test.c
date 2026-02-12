/* rift_stress_test.c — congestion stress test with recovery measurement */

#include "rift_config.h"
#include "rift_congestion.h"
#include "rift_crc32.h"
#include "rift_log.h"
#include "rift_protocol.h"
#include "rift_rtt.h"
#include "rift_stats.h"
#include "rift_trace.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static volatile int g_running = 1;

static void signal_handler(int sig) {
  (void)sig;
  g_running = 0;
}

/* ── Phase Results ───────────────────────────────────────────────── */

typedef struct {
  double steady_pps;       /* Steady-state PPS before loss          */
  double peak_pps;         /* Peak PPS achieved                     */
  double recovery_ms;      /* Time to reach 90% of steady PPS       */
  double deg_pct;          /* Throughput degradation during loss     */
  uint32_t cwnd_at_loss;   /* cwnd when loss was injected           */
  uint32_t cwnd_min;       /* Minimum cwnd during recovery          */
  uint32_t cwnd_recovered; /* cwnd at recovery point              */
  double convergence_ms;   /* Time for cwnd to stabilize            */
} stress_result_t;

/* ── Stress Test Context ─────────────────────────────────────────── */

typedef struct {
  int send_fd;
  int recv_fd;
  struct sockaddr_in send_addr;
  struct sockaddr_in recv_addr;
  uint16_t port;

  rift_congestion_t congestion;
  rift_rtt_estimator_t rtt;
  rift_stats_t stats;

  /* Loss injection */
  double loss_rate;
  bool loss_active;

  /* Measurement */
  stress_result_t results[5]; /* Per-burst results */
  uint32_t burst_count;
} stress_ctx_t;

/* ── Receiver Thread ─────────────────────────────────────────────── */

static void *receiver_thread(void *arg) {
  stress_ctx_t *ctx = (stress_ctx_t *)arg;
  uint8_t buf[RIFT_MAX_SERIALIZED_SIZE];
  rift_packet_t pkt;

  while (g_running) {
    struct pollfd pfd = {.fd = ctx->recv_fd, .events = POLLIN};
    int ret = poll(&pfd, 1, 100);
    if (ret <= 0)
      continue;

    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    ssize_t n = recvfrom(ctx->recv_fd, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &from_len);
    if (n <= 0)
      continue;

    if (rift_packet_deserialize(buf, (size_t)n, &pkt) != 0)
      continue;

    RIFT_STAT_INC(&ctx->stats, packets_received);
    RIFT_STAT_ADD(&ctx->stats, bytes_received, (uint64_t)n);

    /* Send ACK back */
    rift_packet_t ack;
    rift_packet_build(&ack, RIFT_PKT_ACK, RIFT_FLAG_ACK, 0, pkt.header.seq_num + 1,
                     RIFT_WINDOW_SIZE, 0, NULL, 0, NULL, 0);
    ack.header.ts_send = rift_timestamp_us();
    ack.header.ts_echo = pkt.header.ts_send;

    uint8_t ack_buf[RIFT_MAX_SERIALIZED_SIZE];
    int ack_len = rift_packet_serialize(&ack, ack_buf, sizeof(ack_buf));
    if (ack_len > 0) {
      sendto(ctx->recv_fd, ack_buf, (size_t)ack_len, 0,
             (struct sockaddr *)&from, from_len);
    }
  }

  return NULL;
}

/* ── Send Burst ──────────────────────────────────────────────────── */

static double send_burst(stress_ctx_t *ctx, double duration_sec,
                         uint32_t *seq_out) {
  uint64_t start = rift_timestamp_us();
  uint64_t deadline = start + (uint64_t)(duration_sec * 1000000.0);
  uint32_t sent = 0;
  uint32_t seq = seq_out ? *seq_out : 1;
  uint8_t payload[RIFT_MAX_PAYLOAD];
  memset(payload, 0xAA, sizeof(payload));

  while (rift_timestamp_us() < deadline && g_running) {
    /* Simulate loss injection */
    if (ctx->loss_active) {
      double r = (double)rand() / (double)RAND_MAX;
      if (r < ctx->loss_rate) {
        RIFT_STAT_INC(&ctx->stats, packets_lost);
        rift_cc_on_loss(&ctx->congestion, false, rift_timestamp_us());
        seq++;
        continue;
      }
    }

    rift_packet_t pkt;
    rift_packet_build(&pkt, RIFT_PKT_DATA, 0, seq, 0, RIFT_WINDOW_SIZE, 0, NULL, 0,
                     payload, RIFT_MAX_PAYLOAD);
    pkt.header.ts_send = rift_timestamp_us();

    uint8_t wire[RIFT_MAX_SERIALIZED_SIZE];
    int len = rift_packet_serialize(&pkt, wire, sizeof(wire));
    if (len > 0) {
      sendto(ctx->send_fd, wire, (size_t)len, 0,
             (struct sockaddr *)&ctx->send_addr, sizeof(ctx->send_addr));
      RIFT_STAT_INC(&ctx->stats, packets_sent);
      RIFT_STAT_ADD(&ctx->stats, bytes_sent, (uint64_t)len);
      rift_cc_on_ack(&ctx->congestion, 1, rift_timestamp_us());
      sent++;
    }

    seq++;

    /* Pace: don't flood faster than cwnd allows */
    if (sent % (rift_cc_get_cwnd(&ctx->congestion) > 0
                    ? rift_cc_get_cwnd(&ctx->congestion)
                    : 1) ==
        0) {
      usleep(100);
    }
  }

  if (seq_out)
    *seq_out = seq;

  uint64_t elapsed = rift_timestamp_us() - start;
  return elapsed > 0 ? (double)sent * 1000000.0 / (double)elapsed : 0.0;
}

/* ── Measure Recovery ────────────────────────────────────────────── */

static void measure_recovery(stress_ctx_t *ctx, double steady_pps,
                             stress_result_t *result) {
  result->cwnd_at_loss = rift_cc_get_cwnd(&ctx->congestion);

  /* Induce loss burst */
  ctx->loss_active = true;
  ctx->loss_rate = 0.20; /* 20% loss */

  uint32_t seq = 10000;
  uint64_t loss_start = rift_timestamp_us();
  send_burst(ctx, 2.0, &seq); /* 2 seconds of loss */

  result->cwnd_min = rift_cc_get_cwnd(&ctx->congestion);

  /* Recovery phase */
  ctx->loss_active = false;
  uint64_t recovery_start = rift_timestamp_us();
  double target_pps = steady_pps * 0.9; /* 90% of steady-state */
  double current_pps = 0;
  bool recovered = false;

  for (int i = 0; i < 20 && g_running; i++) { /* Max 20 × 500ms = 10s */
    current_pps = send_burst(ctx, 0.5, &seq);

    if (current_pps >= target_pps && !recovered) {
      result->recovery_ms =
          (double)(rift_timestamp_us() - recovery_start) / 1000.0;
      result->cwnd_recovered = rift_cc_get_cwnd(&ctx->congestion);
      recovered = true;
      break;
    }
  }

  if (!recovered) {
    result->recovery_ms =
        (double)(rift_timestamp_us() - recovery_start) / 1000.0;
    result->cwnd_recovered = rift_cc_get_cwnd(&ctx->congestion);
  }

  uint64_t loss_elapsed = rift_timestamp_us() - loss_start;
  (void)loss_elapsed;
  result->deg_pct =
      steady_pps > 0 ? (1.0 - current_pps / steady_pps) * 100.0 : 0.0;
  result->convergence_ms = result->recovery_ms;
}

/* ── Print Results ───────────────────────────────────────────────── */

static void print_results(stress_ctx_t *ctx) {
  printf("\n");
  printf("╔═══════════════════════════════════════════════════════════╗\n");
  printf("║           RIFT Congestion Stress Test Results             ║\n");
  printf("╠═══════════════════════════════════════════════════════════╣\n");
  printf("║  Steady-state PPS:   %10.0f                          ║\n",
         ctx->results[0].steady_pps);
  printf("║  Peak PPS:           %10.0f                          ║\n",
         ctx->results[0].peak_pps);
  printf("╠═══════════════════════════════════════════════════════════╣\n");
  printf("║  Burst │ RecoveryMS │ Degrad%% │ CWND@Loss │ CWND Min   ║\n");
  printf("║  ──────┼────────────┼─────────┼───────────┼──────────── ║\n");

  for (uint32_t i = 0; i < ctx->burst_count; i++) {
    stress_result_t *r = &ctx->results[i];
    printf("║  %5u │ %10.1f │ %7.1f │ %9u │ %10u ║\n", i + 1, r->recovery_ms,
           r->deg_pct, r->cwnd_at_loss, r->cwnd_min);
  }

  printf("╠═══════════════════════════════════════════════════════════╣\n");

  /* Average recovery time */
  double avg_recovery = 0;
  for (uint32_t i = 0; i < ctx->burst_count; i++)
    avg_recovery += ctx->results[i].recovery_ms;
  avg_recovery /= ctx->burst_count;

  printf("║  Avg recovery time:  %8.1f ms                        ║\n",
         avg_recovery);
  printf("║  Final CWND:         %8u                            ║\n",
         rift_cc_get_cwnd(&ctx->congestion));
  printf("╚═══════════════════════════════════════════════════════════╝\n");
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
  uint16_t port = RIFT_DEFAULT_PORT + 10; /* Avoid conflict */
  int opt;

  while ((opt = getopt(argc, argv, "p:h")) != -1) {
    switch (opt) {
    case 'p':
      port = (uint16_t)atoi(optarg);
      break;
    case 'h':
      printf("RIFT Congestion Stress Test\n");
      printf("  -p PORT  Port (default: %d)\n", RIFT_DEFAULT_PORT + 10);
      return 0;
    default:
      return 1;
    }
  }

  rift_crc32_init();
  rift_log_init(RIFT_LOG_INFO, NULL);
  rift_trace_init("/tmp/rift_stress_trace", RIFT_TRACE_FMT_BOTH);
  srand((unsigned)rift_timestamp_us());
  signal(SIGINT, signal_handler);

  stress_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.port = port;
  ctx.burst_count = 3;

  rift_cc_init(&ctx.congestion);
  rift_rtt_init(&ctx.rtt);
  rift_stats_init(&ctx.stats);

  /* Create UDP sockets */
  ctx.send_fd = socket(AF_INET, SOCK_DGRAM, 0);
  ctx.recv_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (ctx.send_fd < 0 || ctx.recv_fd < 0) {
    RIFT_LOG_ERROR("socket() failed: %s", strerror(errno));
    return 1;
  }

  int reuse = 1;
  setsockopt(ctx.recv_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  ctx.recv_addr.sin_family = AF_INET;
  ctx.recv_addr.sin_addr.s_addr = INADDR_ANY;
  ctx.recv_addr.sin_port = htons(port);

  if (bind(ctx.recv_fd, (struct sockaddr *)&ctx.recv_addr,
           sizeof(ctx.recv_addr)) != 0) {
    RIFT_LOG_ERROR("bind() failed: %s", strerror(errno));
    return 1;
  }

  ctx.send_addr.sin_family = AF_INET;
  ctx.send_addr.sin_addr.s_addr = htonl(0x7f000001);
  ctx.send_addr.sin_port = htons(port);

  /* Start receiver thread */
  pthread_t recv_tid;
  pthread_create(&recv_tid, NULL, receiver_thread, &ctx);

  RIFT_LOG_INFO("╔═══════════════════════════════════════════════════╗");
  RIFT_LOG_INFO("║          RIFT Congestion Stress Test              ║");
  RIFT_LOG_INFO("╚═══════════════════════════════════════════════════╝");

  /* ── Phase 1: Ramp Up ─────────────────────────────────────────── */
  RIFT_LOG_INFO("Phase 1: Ramp up (3 seconds)...");
  uint32_t seq = 1;
  double ramp_pps = send_burst(&ctx, 3.0, &seq);
  ctx.results[0].steady_pps = ramp_pps;
  RIFT_LOG_INFO("  Steady-state PPS: %.0f", ramp_pps);

  /* ── Phase 2: Saturate ────────────────────────────────────────── */
  RIFT_LOG_INFO("Phase 2: Saturate (5 seconds)...");
  double peak_pps = send_burst(&ctx, 5.0, &seq);
  ctx.results[0].peak_pps = peak_pps;
  ctx.results[0].steady_pps = peak_pps; /* Update with better estimate */
  RIFT_LOG_INFO("  Peak PPS: %.0f  CWND: %u", peak_pps,
               rift_cc_get_cwnd(&ctx.congestion));

  /* ── Phase 3-5: Loss bursts + recovery ────────────────────────── */
  for (uint32_t burst = 0; burst < ctx.burst_count && g_running; burst++) {
    RIFT_LOG_INFO("Phase %u: Loss burst #%u (20%% loss for 2s)...", burst + 3,
                 burst + 1);
    measure_recovery(&ctx, peak_pps, &ctx.results[burst]);
    RIFT_LOG_INFO("  Recovery: %.1f ms  CWND: %u→%u→%u",
                 ctx.results[burst].recovery_ms,
                 ctx.results[burst].cwnd_at_loss, ctx.results[burst].cwnd_min,
                 ctx.results[burst].cwnd_recovered);

    /* Short cooldown between bursts */
    if (burst + 1 < ctx.burst_count) {
      RIFT_LOG_INFO("  Cooldown (2 seconds)...");
      send_burst(&ctx, 2.0, &seq);
    }
  }

  /* ── Results ──────────────────────────────────────────────────── */
  g_running = 0;
  pthread_join(recv_tid, NULL);

  print_results(&ctx);
  rift_stats_report(&ctx.stats);
  rift_trace_dump_summary();

  close(ctx.send_fd);
  close(ctx.recv_fd);
  rift_trace_shutdown();
  rift_log_shutdown();

  return 0;
}
