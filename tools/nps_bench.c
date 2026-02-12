/* nps_bench.c — throughput/latency benchmarking tool */

#include "nps_config.h"
#include "nps_log.h"
#include "nps_protocol.h"
#include "nps_stats.h"

#include <getopt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Forward declarations */
extern int nps_sender_run(const char *host, uint16_t port, const uint8_t *data,
                          size_t data_len, uint32_t window_size);
extern int nps_receiver_run(uint16_t port, uint8_t *recv_buf,
                            size_t recv_buf_size, size_t *bytes_received,
                            uint32_t window_size);

typedef struct {
  uint16_t port;
  uint32_t window_size;
  size_t data_size;
  int result;
  uint64_t start_us;
  uint64_t end_us;
} bench_args_t;

static void *receiver_thread(void *arg) {
  bench_args_t *a = (bench_args_t *)arg;
  size_t buf_size = a->data_size + 1024 * 1024;
  uint8_t *buf = malloc(buf_size);
  if (!buf) {
    a->result = -1;
    return NULL;
  }

  size_t bytes_recv = 0;
  a->start_us = nps_timestamp_us();
  a->result =
      nps_receiver_run(a->port, buf, buf_size, &bytes_recv, a->window_size);
  a->end_us = nps_timestamp_us();

  /* Verify data integrity */
  if (a->result == 0 && bytes_recv == a->data_size) {
    bool valid = true;
    for (size_t i = 0; i < bytes_recv; i++) {
      if (buf[i] != (uint8_t)(i & 0xFF)) {
        fprintf(stderr,
                "  [FAIL] Data mismatch at offset %zu: "
                "expected 0x%02x, got 0x%02x\n",
                i, (uint8_t)(i & 0xFF), buf[i]);
        valid = false;
        break;
      }
    }
    if (valid) {
      printf("  [PASS] Data integrity verified (%zu bytes)\n", bytes_recv);
    }
  } else if (a->result == 0) {
    printf("  [WARN] Received %zu / %zu bytes\n", bytes_recv, a->data_size);
  }

  free(buf);
  return NULL;
}

static void *sender_thread(void *arg) {
  bench_args_t *a = (bench_args_t *)arg;

  /* Generate test data */
  uint8_t *data = malloc(a->data_size);
  if (!data) {
    a->result = -1;
    return NULL;
  }
  for (size_t i = 0; i < a->data_size; i++)
    data[i] = (uint8_t)(i & 0xFF);

  /* Small delay to let receiver start */
  usleep(100000); /* 100ms */

  a->start_us = nps_timestamp_us();
  a->result =
      nps_sender_run("127.0.0.1", a->port, data, a->data_size, a->window_size);
  a->end_us = nps_timestamp_us();

  free(data);
  return NULL;
}

static void run_benchmark(size_t data_size, uint32_t window_size, uint16_t port,
                          int run_num) {
  printf("\n══════════════════════════════════════════════════\n");
  printf("  Benchmark Run #%d\n", run_num);
  printf("  Data size:   %zu bytes (%.2f MB)\n", data_size,
         (double)data_size / (1024.0 * 1024.0));
  printf("  Window size: %u packets\n", window_size);
  printf("══════════════════════════════════════════════════\n");

  bench_args_t recv_args = {
      .port = port, .window_size = window_size, .data_size = data_size};
  bench_args_t send_args = {
      .port = port, .window_size = window_size, .data_size = data_size};

  pthread_t recv_tid, send_tid;
  pthread_create(&recv_tid, NULL, receiver_thread, &recv_args);
  pthread_create(&send_tid, NULL, sender_thread, &send_args);

  pthread_join(send_tid, NULL);
  pthread_join(recv_tid, NULL);

  /* Results */
  if (send_args.result == 0) {
    double elapsed_s =
        (double)(send_args.end_us - send_args.start_us) / 1000000.0;
    double throughput_mbps = (double)(data_size * 8) / (elapsed_s * 1000000.0);
    double goodput_mbps = throughput_mbps; /* Approximate */

    printf("\n  ── Results ──\n");
    printf("  Elapsed:     %.3f seconds\n", elapsed_s);
    printf("  Throughput:  %.2f Mbps\n", throughput_mbps);
    printf("  Goodput:     %.2f Mbps\n", goodput_mbps);
    printf("  Packets:     ~%zu\n", data_size / NPS_MAX_PAYLOAD + 1);
    printf("  PPS:         ~%.0f\n",
           (double)(data_size / NPS_MAX_PAYLOAD + 1) / elapsed_s);
  } else {
    printf("  [FAIL] Benchmark failed\n");
  }
}

static void print_usage(const char *prog) {
  printf("NPS Benchmark Tool\n\n");
  printf("Usage: %s [options]\n\n", prog);
  printf("Options:\n");
  printf("  --size N        Data size in bytes (default: 1048576)\n");
  printf("  --window N      Window size (default: %d)\n", NPS_WINDOW_SIZE);
  printf("  --port N        Port to use (default: %d)\n", NPS_DEFAULT_PORT);
  printf("  --runs N        Number of benchmark runs (default: 3)\n");
  printf("  -v              Verbose logging\n");
  printf("  -h              Show this help\n");
}

int main(int argc, char *argv[]) {
  size_t data_size = 1024 * 1024; /* 1 MB default */
  uint32_t window_size = NPS_WINDOW_SIZE;
  uint16_t port = NPS_DEFAULT_PORT;
  int runs = 3;
  nps_log_level_t log_level = NPS_LOG_WARN; /* Quiet by default for bench */

  static struct option long_options[] = {{"size", required_argument, 0, 's'},
                                         {"window", required_argument, 0, 'w'},
                                         {"port", required_argument, 0, 'p'},
                                         {"runs", required_argument, 0, 'r'},
                                         {"help", no_argument, 0, 'h'},
                                         {0, 0, 0, 0}};

  int opt;
  while ((opt = getopt_long(argc, argv, "s:w:p:r:vh", long_options, NULL)) !=
         -1) {
    switch (opt) {
    case 's':
      data_size = (size_t)atol(optarg);
      break;
    case 'w':
      window_size = (uint32_t)atoi(optarg);
      break;
    case 'p':
      port = (uint16_t)atoi(optarg);
      break;
    case 'r':
      runs = atoi(optarg);
      break;
    case 'v':
      log_level = NPS_LOG_DEBUG;
      break;
    case 'h':
      print_usage(argv[0]);
      return 0;
    default:
      print_usage(argv[0]);
      return 1;
    }
  }

  nps_log_init(log_level, NULL);

  printf("╔═══════════════════════════════════════════════════╗\n");
  printf("║         NPS Benchmark Suite                      ║\n");
  printf("╚═══════════════════════════════════════════════════╝\n");

  for (int i = 0; i < runs; i++) {
    run_benchmark(data_size, window_size, port + (uint16_t)i, i + 1);
  }

  printf("\n╔═══════════════════════════════════════════════════╗\n");
  printf("║         Benchmark Complete                       ║\n");
  printf("╚═══════════════════════════════════════════════════╝\n");

  nps_log_shutdown();
  return 0;
}
