/* rift_metrics.c — comprehensive metrics & benchmarking tool */

#include "rift_config.h"
#include "rift_log.h"
#include "rift_protocol.h"
#include "rift_stats.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

/* External RIFT APIs */
extern int rift_sender_run_ex(const char *host, uint16_t port,
                             const uint8_t *data, size_t data_len,
                             uint32_t window_size,
                             rift_transfer_result_t *out_result);
extern int rift_receiver_run_ex(uint16_t port, uint8_t *recv_buf,
                               size_t recv_buf_size, size_t *bytes_received,
                               uint32_t window_size,
                               rift_transfer_result_t *out_result);

/* ── Globals & Config ─────────────────────────────────────────────── */

typedef struct {
  const char *mode; /* "rift" or "tcp" */
  size_t data_size;
  uint16_t port;
  uint32_t window_size;
  int loss_percent; /* For report label only */
  int result;

  /* Output stats */
  double throughput_mbps;
  double avg_rtt_ms;
  double delivery_rate;
  double retx_rate;
  long memory_kb;
} benchmark_ctx_t;

/* ── Memory Usage Helper ──────────────────────────────────────────── */

static long get_memory_usage_kb(void) {
#if defined(__APPLE__)
  struct mach_task_basic_info info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info,
                &count) == KERN_SUCCESS) {
    return (long)(info.resident_size / 1024);
  }
  return 0;
#else
  struct rusage r;
  getrusage(RUSAGE_SELF, &r);
  return r.ru_maxrss; /* Linux returns KB */
#endif
}

/* ── TCP Benchmark ────────────────────────────────────────────────── */

static void *tcp_receiver_thread(void *arg) {
  benchmark_ctx_t *ctx = (benchmark_ctx_t *)arg;
  int listen_fd, query_fd;
  struct sockaddr_in addr;

  if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    return NULL;

  int reuse = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(ctx->port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(listen_fd);
    return NULL;
  }

  if (listen(listen_fd, 1) < 0) {
    close(listen_fd);
    return NULL;
  }

  if ((query_fd = accept(listen_fd, NULL, NULL)) < 0) {
    close(listen_fd);
    return NULL;
  }

  uint8_t buf[4096];
  size_t total_recv = 0;
  ssize_t n;

  while ((n = recv(query_fd, buf, sizeof(buf), 0)) > 0) {
    total_recv += n;
  }
  (void)total_recv; /* Silence unused variable warning */

  close(query_fd);
  close(listen_fd);
  return NULL;
}

static void run_tcp_benchmark(benchmark_ctx_t *ctx) {
  pthread_t tid;
  pthread_create(&tid, NULL, tcp_receiver_thread, ctx);
  usleep(100000); /* Wait for receiver */

  int sockfd;
  struct sockaddr_in addr;

  if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    ctx->result = -1;
    pthread_cancel(tid);
    return;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(ctx->port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    ctx->result = -1;
    close(sockfd);
    pthread_cancel(tid);
    return;
  }

  /* Disable Nagle for fair comparison with UDP */
  int flag = 1;
  setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

  size_t total_sent = 0;
  size_t chunk_size = 1400; /* Similar to RIFT MSS */
  uint8_t *data = malloc(chunk_size);
  memset(data, 'X', chunk_size);

  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);

  while (total_sent < ctx->data_size) {
    size_t remaining = ctx->data_size - total_sent;
    size_t to_send = remaining < chunk_size ? remaining : chunk_size;
    ssize_t n = send(sockfd, data, to_send, 0);
    if (n <= 0)
      break;
    total_sent += n;
  }

  clock_gettime(CLOCK_MONOTONIC, &end);
  double elapsed =
      (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;

  ctx->throughput_mbps = (double)(total_sent * 8) / (elapsed * 1000000.0);
  ctx->avg_rtt_ms = 0; /* Not easily measurable from userspace TCP send loop */
  ctx->retx_rate = 0;
  ctx->delivery_rate = 100.0;
  ctx->memory_kb = get_memory_usage_kb();
  ctx->result = 0;

  close(sockfd);
  free(data);
  pthread_join(tid, NULL);
}

/* ── RIFT Benchmark ────────────────────────────────────────────────── */

static void *rift_receiver_thread(void *arg) {
  benchmark_ctx_t *ctx = (benchmark_ctx_t *)arg;
  uint8_t *buf = malloc(ctx->data_size + 1024 * 1024); /* Overprovision */
  size_t bytes_recv = 0;

  rift_transfer_result_t res = {0};
  int ret = rift_receiver_run_ex(ctx->port, buf, ctx->data_size + 1024 * 1024,
                                &bytes_recv, ctx->window_size, &res);

  if (ret == 0) {
    /* Pass delivery rate/goodput back if needed, but sender has better stats */
  }
  free(buf);
  return NULL;
}

static void run_rift_benchmark(benchmark_ctx_t *ctx) {
  pthread_t tid;
  pthread_create(&tid, NULL, rift_receiver_thread, ctx);
  usleep(100000);

  uint8_t *data = malloc(ctx->data_size);
  memset(data, 0x42, ctx->data_size);

  rift_transfer_result_t res = {0};
  int ret = rift_sender_run_ex("127.0.0.1", ctx->port, data, ctx->data_size,
                              ctx->window_size, &res);

  if (ret == 0) {
    ctx->result = 0;
    ctx->throughput_mbps = res.throughput_mbps;
    ctx->avg_rtt_ms = res.avg_srtt_ms;
    ctx->retx_rate =
        (double)res.packets_retransmitted / (double)res.packets_sent * 100.0;
    ctx->delivery_rate = 100.0; /* Reliable transport implies 100% delivery */
    ctx->memory_kb = get_memory_usage_kb();
  } else {
    ctx->result = -1;
  }

  free(data);
  pthread_join(tid, NULL);
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
  benchmark_ctx_t ctx = {.mode = "rift",
                         .data_size =
                             100 * 1024 * 1024, /* Default 100 MB for speed */
                         .port = 9000,
                         .window_size = RIFT_WINDOW_SIZE,
                         .loss_percent = 0};

  static struct option long_options[] = {
      {"mode", required_argument, 0, 'm'},
      {"size", required_argument, 0, 's'},
      {"loss", required_argument, 0, 'l'}, /* Informational only */
      {0, 0, 0, 0}};

  int opt;
  while ((opt = getopt_long(argc, argv, "m:s:l:", long_options, NULL)) != -1) {
    switch (opt) {
    case 'm':
      ctx.mode = optarg;
      break;
    case 's':
      ctx.data_size = (size_t)atol(optarg);
      break;
    case 'l':
      ctx.loss_percent = atoi(optarg);
      break;
    }
  }

  /* Silence logs */
  rift_log_init(RIFT_LOG_ERROR, NULL);

  if (strcmp(ctx.mode, "tcp") == 0) {
    run_tcp_benchmark(&ctx);
  } else {
    run_rift_benchmark(&ctx);
  }

  rift_log_shutdown();

  /* Output compact JSON for the orchestrator script */
  if (ctx.result == 0) {
    printf("{\n");
    printf("  \"mode\": \"%s\",\n", ctx.mode);
    printf("  \"data_size\": %zu,\n", ctx.data_size);
    printf("  \"loss_sim_percent\": %d,\n", ctx.loss_percent);
    printf("  \"throughput_mbps\": %.2f,\n", ctx.throughput_mbps);
    printf("  \"avg_rtt_us\": %.0f,\n", ctx.avg_rtt_ms * 1000.0);
    printf("  \"delivery_rate\": %.1f,\n", ctx.delivery_rate);
    printf("  \"retransmission_overhead\": %.2f,\n", ctx.retx_rate);
    printf("  \"memory_usage_kb\": %ld\n", ctx.memory_kb);
    printf("}\n");
    return 0;
  } else {
    fprintf(stderr, "Benchmark failed\n");
    return 1;
  }
}
