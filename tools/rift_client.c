/* rift_client.c — test client (sender) */

#include "rift_config.h"
#include "rift_log.h"
#include "rift_protocol.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declaration from rift_sender.c */
extern int rift_sender_run(const char *host, uint16_t port, const uint8_t *data,
                          size_t data_len, uint32_t window_size);

static void print_usage(const char *prog) {
  printf("RIFT Test Client (Sender)\n\n");
  printf("Usage: %s -h HOST [options]\n\n", prog);
  printf("Options:\n");
  printf("  -h HOST         Target host (required)\n");
  printf("  -p PORT         Target port (default: %d)\n", RIFT_DEFAULT_PORT);
  printf("  -s SIZE         Generate and send SIZE bytes of test data\n");
  printf("  -f FILE         Send contents of FILE\n");
  printf("  -w WINDOW       Window size (default: %d)\n", RIFT_WINDOW_SIZE);
  printf("  -v              Verbose logging (DEBUG level)\n");
  printf("  -q              Quiet logging (ERROR only)\n");
  printf("  --help          Show this help\n");
}

/* Generate deterministic test data for verification */
static void generate_test_data(uint8_t *buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    buf[i] = (uint8_t)(i & 0xFF);
  }
}

int main(int argc, char *argv[]) {
  const char *host = NULL;
  uint16_t port = RIFT_DEFAULT_PORT;
  size_t data_size = 0;
  const char *input_file = NULL;
  uint32_t window_size = RIFT_WINDOW_SIZE;
  rift_log_level_t log_level = RIFT_LOG_INFO;
  int opt;

  while ((opt = getopt(argc, argv, "H:h:p:s:f:w:vq")) != -1) {
    switch (opt) {
    case 'H':
    case 'h':
      host = optarg;
      break;
    case 'p':
      port = (uint16_t)atoi(optarg);
      break;
    case 's':
      data_size = (size_t)atol(optarg);
      break;
    case 'f':
      input_file = optarg;
      break;
    case 'w':
      window_size = (uint32_t)atoi(optarg);
      break;
    case 'v':
      log_level = RIFT_LOG_DEBUG;
      break;
    case 'q':
      log_level = RIFT_LOG_ERROR;
      break;
    default:
      print_usage(argv[0]);
      return 1;
    }
  }

  if (!host) {
    fprintf(stderr, "Error: -h HOST is required\n\n");
    print_usage(argv[0]);
    return 1;
  }

  if (data_size == 0 && !input_file) {
    data_size = 1024 * 1024; /* Default: 1 MB */
  }

  rift_log_init(log_level, NULL);

  uint8_t *data = NULL;
  size_t data_len = 0;

  if (input_file) {
    /* Read from file */
    FILE *fp = fopen(input_file, "rb");
    if (!fp) {
      RIFT_LOG_ERROR("Cannot open input file '%s'", input_file);
      return 1;
    }
    fseek(fp, 0, SEEK_END);
    data_len = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);

    data = malloc(data_len);
    if (!data) {
      RIFT_LOG_ERROR("Failed to allocate %zu bytes", data_len);
      fclose(fp);
      return 1;
    }
    fread(data, 1, data_len, fp);
    fclose(fp);
    RIFT_LOG_INFO("Loaded %zu bytes from '%s'", data_len, input_file);
  } else {
    /* Generate test data */
    data_len = data_size;
    data = malloc(data_len);
    if (!data) {
      RIFT_LOG_ERROR("Failed to allocate %zu bytes", data_len);
      return 1;
    }
    generate_test_data(data, data_len);
    RIFT_LOG_INFO("Generated %zu bytes of test data", data_len);
  }

  RIFT_LOG_INFO("╔═══════════════════════════════════════════════╗");
  RIFT_LOG_INFO("║          RIFT Client Starting                 ║");
  RIFT_LOG_INFO("╠═══════════════════════════════════════════════╣");
  RIFT_LOG_INFO("║  Target:      %s:%-5d                      ║", host, port);
  RIFT_LOG_INFO("║  Data size:   %zu bytes                     ║", data_len);
  RIFT_LOG_INFO("║  Window:      %-6u                         ║", window_size);
  RIFT_LOG_INFO("╚═══════════════════════════════════════════════╝");

  int result = rift_sender_run(host, port, data, data_len, window_size);

  free(data);
  rift_log_shutdown();
  return result;
}
