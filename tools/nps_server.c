/* nps_server.c — test server (receiver) */

#include "nps_config.h"
#include "nps_log.h"
#include "nps_protocol.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declaration from nps_receiver.c */
extern int nps_receiver_run(uint16_t port, uint8_t *recv_buf,
                            size_t recv_buf_size, size_t *bytes_received,
                            uint32_t window_size);

static volatile int g_running = 1;

static void signal_handler(int sig) {
  (void)sig;
  g_running = 0;
}

static void print_usage(const char *prog) {
  printf("NPS Test Server (Receiver)\n\n");
  printf("Usage: %s [options]\n\n", prog);
  printf("Options:\n");
  printf("  -p PORT         Listen port (default: %d)\n", NPS_DEFAULT_PORT);
  printf("  -w WINDOW       Window size (default: %d)\n", NPS_WINDOW_SIZE);
  printf("  -o FILE         Write received data to file\n");
  printf("  -v              Verbose logging (DEBUG level)\n");
  printf("  -q              Quiet logging (ERROR only)\n");
  printf("  -h              Show this help\n");
}

int main(int argc, char *argv[]) {
  uint16_t port = NPS_DEFAULT_PORT;
  uint32_t window_size = NPS_WINDOW_SIZE;
  const char *output_file = NULL;
  nps_log_level_t log_level = NPS_LOG_INFO;
  int opt;

  while ((opt = getopt(argc, argv, "p:w:o:vqh")) != -1) {
    switch (opt) {
    case 'p':
      port = (uint16_t)atoi(optarg);
      break;
    case 'w':
      window_size = (uint32_t)atoi(optarg);
      break;
    case 'o':
      output_file = optarg;
      break;
    case 'v':
      log_level = NPS_LOG_DEBUG;
      break;
    case 'q':
      log_level = NPS_LOG_ERROR;
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
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  /* Allocate receive buffer (64 MB max) */
  size_t buf_size = 64 * 1024 * 1024;
  uint8_t *recv_buf = malloc(buf_size);
  if (!recv_buf) {
    NPS_LOG_ERROR("Failed to allocate receive buffer");
    return 1;
  }

  NPS_LOG_INFO("╔═══════════════════════════════════════════════╗");
  NPS_LOG_INFO("║          NPS Server Starting                 ║");
  NPS_LOG_INFO("╠═══════════════════════════════════════════════╣");
  NPS_LOG_INFO("║  Port:        %-6d                         ║", port);
  NPS_LOG_INFO("║  Window:      %-6u                         ║", window_size);
  NPS_LOG_INFO("║  Buffer:      %zu MB                       ║",
               buf_size / (1024 * 1024));
  NPS_LOG_INFO("╚═══════════════════════════════════════════════╝");

  size_t bytes_received = 0;
  int result =
      nps_receiver_run(port, recv_buf, buf_size, &bytes_received, window_size);

  if (result == 0 && bytes_received > 0) {
    NPS_LOG_INFO("Received %zu bytes successfully", bytes_received);

    if (output_file) {
      FILE *fp = fopen(output_file, "wb");
      if (fp) {
        fwrite(recv_buf, 1, bytes_received, fp);
        fclose(fp);
        NPS_LOG_INFO("Data written to '%s'", output_file);
      } else {
        NPS_LOG_ERROR("Cannot open output file '%s'", output_file);
      }
    }

    /* Verify by printing first/last few bytes */
    if (bytes_received > 0) {
      NPS_LOG_INFO("First 16 bytes: ");
      char hex[64] = {0};
      size_t show = bytes_received < 16 ? bytes_received : 16;
      for (size_t i = 0; i < show; i++) {
        char tmp[4];
        snprintf(tmp, sizeof(tmp), "%02x ", recv_buf[i]);
        strcat(hex, tmp);
      }
      NPS_LOG_INFO("  %s", hex);
    }
  }

  free(recv_buf);
  nps_log_shutdown();
  return result;
}
