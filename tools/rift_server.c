/* rift_server.c — test server (receiver) */

#include "rift_config.h"
#include "rift_log.h"
#include "rift_protocol.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declaration from rift_receiver.c */
extern int rift_receiver_run(uint16_t port, uint8_t *recv_buf,
                            size_t recv_buf_size, size_t *bytes_received,
                            uint32_t window_size);

static volatile int g_running = 1;

static void signal_handler(int sig) {
  (void)sig;
  g_running = 0;
}

static void print_usage(const char *prog) {
  printf("RIFT Test Server (Receiver)\n\n");
  printf("Usage: %s [options]\n\n", prog);
  printf("Options:\n");
  printf("  -p PORT         Listen port (default: %d)\n", RIFT_DEFAULT_PORT);
  printf("  -w WINDOW       Window size (default: %d)\n", RIFT_WINDOW_SIZE);
  printf("  -o FILE         Write received data to file\n");
  printf("  -v              Verbose logging (DEBUG level)\n");
  printf("  -q              Quiet logging (ERROR only)\n");
  printf("  -h              Show this help\n");
}

int main(int argc, char *argv[]) {
  uint16_t port = RIFT_DEFAULT_PORT;
  uint32_t window_size = RIFT_WINDOW_SIZE;
  const char *output_file = NULL;
  rift_log_level_t log_level = RIFT_LOG_INFO;
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
      log_level = RIFT_LOG_DEBUG;
      break;
    case 'q':
      log_level = RIFT_LOG_ERROR;
      break;
    case 'h':
      print_usage(argv[0]);
      return 0;
    default:
      print_usage(argv[0]);
      return 1;
    }
  }

  rift_log_init(log_level, NULL);
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  /* Allocate receive buffer (64 MB max) */
  size_t buf_size = 64 * 1024 * 1024;
  uint8_t *recv_buf = malloc(buf_size);
  if (!recv_buf) {
    RIFT_LOG_ERROR("Failed to allocate receive buffer");
    return 1;
  }

  RIFT_LOG_INFO("╔═══════════════════════════════════════════════╗");
  RIFT_LOG_INFO("║          RIFT Server Starting                 ║");
  RIFT_LOG_INFO("╠═══════════════════════════════════════════════╣");
  RIFT_LOG_INFO("║  Port:        %-6d                         ║", port);
  RIFT_LOG_INFO("║  Window:      %-6u                         ║", window_size);
  RIFT_LOG_INFO("║  Buffer:      %zu MB                       ║",
               buf_size / (1024 * 1024));
  RIFT_LOG_INFO("╚═══════════════════════════════════════════════╝");

  size_t bytes_received = 0;
  int result =
      rift_receiver_run(port, recv_buf, buf_size, &bytes_received, window_size);

  if (result == 0 && bytes_received > 0) {
    RIFT_LOG_INFO("Received %zu bytes successfully", bytes_received);

    if (output_file) {
      FILE *fp = fopen(output_file, "wb");
      if (fp) {
        fwrite(recv_buf, 1, bytes_received, fp);
        fclose(fp);
        RIFT_LOG_INFO("Data written to '%s'", output_file);
      } else {
        RIFT_LOG_ERROR("Cannot open output file '%s'", output_file);
      }
    }

    /* Verify by printing first/last few bytes */
    if (bytes_received > 0) {
      RIFT_LOG_INFO("First 16 bytes: ");
      char hex[64] = {0};
      size_t show = bytes_received < 16 ? bytes_received : 16;
      for (size_t i = 0; i < show; i++) {
        char tmp[4];
        snprintf(tmp, sizeof(tmp), "%02x ", recv_buf[i]);
        strcat(hex, tmp);
      }
      RIFT_LOG_INFO("  %s", hex);
    }
  }

  free(recv_buf);
  rift_log_shutdown();
  return result;
}
