/* rift_losssim.c — UDP loss simulator proxy */

#include "rift_config.h"
#include "rift_log.h"
#include "rift_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h> // Added for PRIu64
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static volatile int g_running = 1;

static void signal_handler(int sig) {
  (void)sig;
  g_running = 0;
}

typedef struct {
  uint16_t listen_port;
  uint16_t target_port;
  double loss_rate;   /* 0.0 - 1.0 */
  double delay_ms;    /* Added delay in ms */
  double jitter_ms;   /* Random jitter in ms */
  uint32_t burst_len; /* Burst loss length */

  /* Statistics */
  uint64_t total_forwarded;
  uint64_t total_dropped;
  uint64_t total_bytes;
  uint64_t client_to_server;
  uint64_t server_to_client;
} losssim_t;

static bool should_drop(losssim_t *sim) {
  double r = (double)rand() / (double)RAND_MAX;
  return r < sim->loss_rate;
}

static void add_delay(losssim_t *sim) {
  if (sim->delay_ms <= 0 && sim->jitter_ms <= 0)
    return;

  double delay = sim->delay_ms;
  if (sim->jitter_ms > 0) {
    double jitter = ((double)rand() / RAND_MAX - 0.5) * 2.0 * sim->jitter_ms;
    delay += jitter;
  }

  if (delay > 0) {
    usleep((useconds_t)(delay * 1000.0));
  }
}

static void print_usage(const char *prog) {
  printf("RIFT Packet Loss Simulator\n\n");
  printf("Usage: %s [options]\n\n", prog);
  printf("Options:\n");
  printf("  -L PORT     Listen port (proxy receives here)\n");
  printf("  -T PORT     Target port (proxy forwards to here)\n");
  printf("  -l RATE     Loss rate 0.0-1.0 (default: 0.05 = 5%%)\n");
  printf("  -d DELAY    Added delay in ms (default: 0)\n");
  printf("  -j JITTER   Random jitter in ms (default: 0)\n");
  printf("  -b LEN      Burst loss length (default: 1)\n");
  printf("  -v          Verbose logging\n");
  printf("  -h          Show this help\n");
  printf("\nExample:\n");
  printf("  %s -L 9998 -T 9999 -l 0.05 -d 10 -j 5\n", prog);
}

int main(int argc, char *argv[]) {
  losssim_t sim = {
      .listen_port = 0,
      .target_port = RIFT_DEFAULT_PORT,
      .loss_rate = 0.05,
      .delay_ms = 0,
      .jitter_ms = 0,
      .burst_len = 1,
  };

  rift_log_level_t log_level = RIFT_LOG_INFO;
  int opt;

  while ((opt = getopt(argc, argv, "L:T:l:d:j:b:vh")) != -1) {
    switch (opt) {
    case 'L':
      sim.listen_port = (uint16_t)atoi(optarg);
      break;
    case 'T':
      sim.target_port = (uint16_t)atoi(optarg);
      break;
    case 'l':
      sim.loss_rate = atof(optarg);
      break;
    case 'd':
      sim.delay_ms = atof(optarg);
      break;
    case 'j':
      sim.jitter_ms = atof(optarg);
      break;
    case 'b':
      sim.burst_len = (uint32_t)atoi(optarg);
      break;
    case 'v':
      log_level = RIFT_LOG_DEBUG;
      break;
    case 'h':
      print_usage(argv[0]);
      return 0;
    default:
      print_usage(argv[0]);
      return 1;
    }
  }

  if (sim.listen_port == 0) {
    fprintf(stderr, "Error: -L LISTEN_PORT is required\n");
    print_usage(argv[0]);
    return 1;
  }

  rift_log_init(log_level, NULL);
  srand((unsigned int)time(NULL));
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  /* Create listen socket */
  int listen_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (listen_fd < 0) {
    RIFT_LOG_ERROR("socket() failed: %s", strerror(errno));
    return 1;
  }

  int reuse = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in listen_addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = INADDR_ANY,
      .sin_port = htons(sim.listen_port),
  };

  if (bind(listen_fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) !=
      0) {
    RIFT_LOG_ERROR("bind() failed: %s", strerror(errno));
    close(listen_fd);
    return 1;
  }

  /* Target (server) address */
  struct sockaddr_in target_addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(0x7f000001), /* 127.0.0.1 */
      .sin_port = htons(sim.target_port),
  };

  RIFT_LOG_INFO("╔═══════════════════════════════════════════════╗");
  RIFT_LOG_INFO("║      RIFT Packet Loss Simulator               ║");
  RIFT_LOG_INFO("╠═══════════════════════════════════════════════╣");
  RIFT_LOG_INFO("║  Listen:   :%d → :%d                        ║",
               sim.listen_port, sim.target_port);
  RIFT_LOG_INFO("║  Loss:     %.1f%%                              ║",
               sim.loss_rate * 100.0);
  RIFT_LOG_INFO("║  Delay:    %.1f ms ± %.1f ms                  ║",
               sim.delay_ms, sim.jitter_ms);
  RIFT_LOG_INFO("╚═══════════════════════════════════════════════╝");

  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);
  bool has_client = false;

  uint8_t buf[RIFT_MAX_SERIALIZED_SIZE + 256];
  uint32_t burst_counter = 0;

  while (g_running) {
    struct pollfd pfd = {.fd = listen_fd, .events = POLLIN};
    int ret = poll(&pfd, 1, 1000);

    if (ret <= 0)
      continue;

    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);

    ssize_t n = recvfrom(listen_fd, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &from_len);
    if (n <= 0)
      continue;

    /* Determine direction: client→server or server→client */
    bool is_from_client = (from.sin_port != htons(sim.target_port));
    if (is_from_client && !has_client) {
      client_addr = from;
      client_len = from_len;
      has_client = true;
    }

    /* Decide whether to drop */
    bool drop = false;
    if (burst_counter > 0) {
      drop = true;
      burst_counter--;
    } else if (should_drop(&sim)) {
      drop = true;
      burst_counter = sim.burst_len - 1;
    }

    if (drop) {
      sim.total_dropped++;
      RIFT_LOG_DEBUG("DROPPED packet (%s, %zd bytes)",
                    is_from_client ? "C→S" : "S→C", n);
      continue;
    }

    /* Add delay/jitter */
    add_delay(&sim);

    /* Forward */
    struct sockaddr_in *dest;
    socklen_t dest_len;
    if (is_from_client) {
      dest = &target_addr;
      dest_len = sizeof(target_addr);
      sim.client_to_server++;
    } else {
      dest = &client_addr;
      dest_len = client_len;
      sim.server_to_client++;
    }

    sendto(listen_fd, buf, (size_t)n, 0, (struct sockaddr *)dest, dest_len);

    sim.total_forwarded++;
    sim.total_bytes += (uint64_t)n;

    RIFT_LOG_TRACE("Forwarded %zd bytes (%s)", n,
                  is_from_client ? "C→S" : "S→C");
  }

  RIFT_LOG_INFO("═══════════════════════════════════════════════");
  RIFT_LOG_INFO("  Loss Simulator Statistics");
  RIFT_LOG_INFO("  Forwarded:   %" PRIu64 " packets", sim.total_forwarded);
  RIFT_LOG_INFO("  Dropped:     %" PRIu64 " packets", sim.total_dropped);
  RIFT_LOG_INFO("  C→S:         %" PRIu64, sim.client_to_server);
  RIFT_LOG_INFO("  S→C:         %" PRIu64, sim.server_to_client);
  RIFT_LOG_INFO("  Total bytes: %" PRIu64, sim.total_bytes);
  double actual_loss =
      sim.total_forwarded + sim.total_dropped > 0
          ? (double)sim.total_dropped /
                (double)(sim.total_forwarded + sim.total_dropped) * 100.0
          : 0.0;
  RIFT_LOG_INFO("  Actual loss: %.2f%%", actual_loss);
  RIFT_LOG_INFO("═══════════════════════════════════════════════");

  close(listen_fd);
  rift_log_shutdown();
  return 0;
}
