/* map_reader.c — BPF map stats reader and rule manager */

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "../common.bpf.h"

static volatile int g_running = 1;
static const char *g_pin_path = "/sys/fs/bpf/nps";

static void signal_handler(int sig) {
  (void)sig;
  g_running = 0;
}

/* ── Open Pinned Map ──────────────────────────────────────────────── */

static int open_pinned_map(const char *name) {
  char path[256];
  snprintf(path, sizeof(path), "%s/%s", g_pin_path, name);
  int fd = bpf_obj_get(path);
  if (fd < 0) {
    fprintf(stderr, "Cannot open map '%s': %s\n", path, strerror(errno));
  }
  return fd;
}

/* ── IP Address Helper ────────────────────────────────────────────── */

static const char *ip_str(uint32_t ip_nbo) {
  static char buf[4][INET_ADDRSTRLEN];
  static int idx = 0;
  idx = (idx + 1) % 4;
  struct in_addr addr = {.s_addr = ip_nbo};
  inet_ntop(AF_INET, &addr, buf[idx], INET_ADDRSTRLEN);
  return buf[idx];
}

/* ── Show Global Stats ────────────────────────────────────────────── */

static void show_global_stats(void) {
  int fd = open_pinned_map(NPS_MAP_GLOBAL_STATS);
  if (fd < 0) {
    /* Try TC variant */
    fd = open_pinned_map("nps_tc_global_stats");
    if (fd < 0)
      return;
  }

  uint32_t key = 0;
  struct nps_global_stats stats;

  if (bpf_map_lookup_elem(fd, &key, &stats) != 0) {
    fprintf(stderr, "Failed to read global stats\n");
    close(fd);
    return;
  }

  printf("╔═══════════════════════════════════════════════════╗\n");
  printf("║          NPS eBPF Global Statistics              ║\n");
  printf("╠═══════════════════════════════════════════════════╣\n");
  printf("║  Total Packets:      %-15lu            ║\n", stats.total_packets);
  printf("║  Total Bytes:        %-15lu            ║\n", stats.total_bytes);
  printf("║  Passed:             %-15lu            ║\n", stats.total_passed);
  printf("║  Dropped:            %-15lu            ║\n", stats.total_dropped);
  printf("║  Rate Limited:       %-15lu            ║\n",
         stats.total_rate_limited);
  printf("║  NPS Protocol Pkts:  %-15lu            ║\n",
         stats.nps_protocol_packets);
  printf("╚═══════════════════════════════════════════════════╝\n");

  close(fd);
}

/* ── Show Connection Stats ────────────────────────────────────────── */

static void show_conn_stats(void) {
  int fd = open_pinned_map(NPS_MAP_CONN_STATS);
  if (fd < 0) {
    fd = open_pinned_map("nps_tc_conn_stats");
    if (fd < 0)
      return;
  }

  printf("\n── Per-Connection Statistics ──────────────────────────\n");
  printf("%-16s %-16s %-7s %-7s %-5s %-10s %-10s %-10s\n", "Source", "Dest",
         "SrcPort", "DstPort", "Proto", "Packets", "Passed", "Dropped");
  printf("─────────────────────────────────────────────────────────"
         "──────────────────────\n");

  struct nps_filter_key key = {0}, next_key;
  struct nps_conn_stats stats;
  int count = 0;

  while (bpf_map_get_next_key(fd, &key, &next_key) == 0 && count < 100) {
    if (bpf_map_lookup_elem(fd, &next_key, &stats) == 0) {
      printf("%-16s %-16s %-7u %-7u %-5u %-10lu %-10lu %-10lu\n",
             ip_str(next_key.src_ip), ip_str(next_key.dst_ip),
             ntohs(next_key.src_port), ntohs(next_key.dst_port),
             next_key.protocol, stats.packets, stats.packets_passed,
             stats.packets_dropped);
      count++;
    }
    key = next_key;
  }

  if (count == 0)
    printf("  (no connections tracked)\n");

  close(fd);
}

/* ── List Filter Rules ────────────────────────────────────────────── */

static void list_rules(void) {
  int fd = open_pinned_map(NPS_MAP_FILTER_RULES);
  if (fd < 0) {
    fd = open_pinned_map("nps_tc_filter_rules");
    if (fd < 0)
      return;
  }

  printf("\n── Filter Rules ──────────────────────────────────────\n");
  printf("%-16s %-16s %-7s %-7s %-5s %-8s %-12s\n", "Source", "Dest", "SrcPort",
         "DstPort", "Proto", "Action", "Rate(pps)");
  printf("─────────────────────────────────────────────────────────"
         "──────────────\n");

  struct nps_filter_key key = {0}, next_key;
  struct nps_filter_rule rule;
  int count = 0;

  const char *action_names[] = {"PASS", "DROP", "LIMIT"};

  while (bpf_map_get_next_key(fd, &key, &next_key) == 0 && count < 100) {
    if (bpf_map_lookup_elem(fd, &next_key, &rule) == 0) {
      const char *action =
          rule.action < 3 ? action_names[rule.action] : "UNKNOWN";
      printf("%-16s %-16s %-7u %-7u %-5u %-8s %-12lu\n",
             ip_str(next_key.src_ip), ip_str(next_key.dst_ip),
             ntohs(next_key.src_port), ntohs(next_key.dst_port),
             next_key.protocol, action, rule.rate_pps);
      count++;
    }
    key = next_key;
  }

  if (count == 0)
    printf("  (no rules configured)\n");

  close(fd);
}

/* ── Add Filter Rule ──────────────────────────────────────────────── */

static int add_rule(const char *src_ip, const char *dst_ip, uint16_t src_port,
                    uint16_t dst_port, uint8_t proto, int action,
                    uint64_t rate_pps) {
  int fd = open_pinned_map(NPS_MAP_FILTER_RULES);
  if (fd < 0)
    return -1;

  struct nps_filter_key key = {
      .src_port = htons(src_port),
      .dst_port = htons(dst_port),
      .protocol = proto,
  };

  if (src_ip && strcmp(src_ip, "0") != 0)
    inet_pton(AF_INET, src_ip, &key.src_ip);
  if (dst_ip && strcmp(dst_ip, "0") != 0)
    inet_pton(AF_INET, dst_ip, &key.dst_ip);

  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);

  struct nps_filter_rule rule = {
      .action = action,
      .rate_pps = rate_pps,
      .rate_bps = 0,
      .created_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec,
  };

  if (bpf_map_update_elem(fd, &key, &rule, BPF_ANY) != 0) {
    fprintf(stderr, "Failed to add rule: %s\n", strerror(errno));
    close(fd);
    return -1;
  }

  printf("Rule added: %s:%u → %s:%u proto=%u action=%d rate=%lu pps\n",
         src_ip ? src_ip : "*", src_port, dst_ip ? dst_ip : "*", dst_port,
         proto, action, rate_pps);

  close(fd);
  return 0;
}

/* ── Watch Mode ───────────────────────────────────────────────────── */

static void watch_stats(int interval_ms) {
  printf("Live stats (Ctrl+C to stop)...\n\n");

  while (g_running) {
    printf("\033[2J\033[H"); /* Clear screen */
    show_global_stats();
    show_conn_stats();

    time_t now = time(NULL);
    printf("\nLast update: %s", ctime(&now));

    usleep(interval_ms * 1000);
  }
}

/* ── Main ─────────────────────────────────────────────────────────── */

static void print_usage(const char *prog) {
  printf("NPS BPF Map Reader\n\n");
  printf("Usage: %s <command> [options]\n\n", prog);
  printf("Commands:\n");
  printf("  stats                          Show global and connection stats\n");
  printf("  watch                          Live-updating stats display\n");
  printf("  list-rules                     List all filter rules\n");
  printf("  add-rule SRC DST SPORT DPORT PROTO ACTION [RATE]\n");
  printf("                                 Add a filter rule\n");
  printf("    ACTION: 0=PASS, 1=DROP, 2=LIMIT\n");
  printf("    RATE: packets/sec (for LIMIT action)\n");
  printf("  del-rule SRC DST SPORT DPORT PROTO\n");
  printf("                                 Delete a filter rule\n");
  printf("\nOptions:\n");
  printf("  -p PATH   BPF pin path (default: /sys/fs/bpf/nps)\n");
}

int main(int argc, char *argv[]) {
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  /* Check for -p option */
  for (int i = 1; i < argc - 1; i++) {
    if (strcmp(argv[i], "-p") == 0) {
      g_pin_path = argv[i + 1];
      break;
    }
  }

  const char *cmd = argv[1];

  if (strcmp(cmd, "stats") == 0) {
    show_global_stats();
    show_conn_stats();
  } else if (strcmp(cmd, "watch") == 0) {
    watch_stats(1000);
  } else if (strcmp(cmd, "list-rules") == 0) {
    list_rules();
  } else if (strcmp(cmd, "add-rule") == 0) {
    if (argc < 8) {
      fprintf(stderr,
              "Usage: %s add-rule SRC DST SPORT DPORT PROTO "
              "ACTION [RATE]\n",
              argv[0]);
      return 1;
    }
    uint64_t rate = argc > 8 ? (uint64_t)atol(argv[8]) : 0;
    return add_rule(argv[2], argv[3], (uint16_t)atoi(argv[4]),
                    (uint16_t)atoi(argv[5]), (uint8_t)atoi(argv[6]),
                    atoi(argv[7]), rate);
  } else if (strcmp(cmd, "del-rule") == 0) {
    if (argc < 7) {
      fprintf(stderr, "Usage: %s del-rule SRC DST SPORT DPORT PROTO\n",
              argv[0]);
      return 1;
    }
    int fd = open_pinned_map(NPS_MAP_FILTER_RULES);
    if (fd < 0)
      return 1;

    struct nps_filter_key key = {
        .src_port = htons((uint16_t)atoi(argv[4])),
        .dst_port = htons((uint16_t)atoi(argv[5])),
        .protocol = (uint8_t)atoi(argv[6]),
    };
    if (strcmp(argv[2], "0") != 0)
      inet_pton(AF_INET, argv[2], &key.src_ip);
    if (strcmp(argv[3], "0") != 0)
      inet_pton(AF_INET, argv[3], &key.dst_ip);

    if (bpf_map_delete_elem(fd, &key) != 0) {
      fprintf(stderr, "Failed to delete rule: %s\n", strerror(errno));
      close(fd);
      return 1;
    }
    printf("Rule deleted.\n");
    close(fd);
  } else {
    print_usage(argv[0]);
    return 1;
  }

  return 0;
}
