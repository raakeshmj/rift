/* loader.c — eBPF program loader for NPS (XDP/TC attach + map pinning) */

#include <errno.h>
#include <getopt.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "../common.bpf.h"

static volatile int g_running = 1;

static void signal_handler(int sig) {
  (void)sig;
  g_running = 0;
}

static int ensure_pin_path(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0)
    return 0;
  return mkdir(path, 0755);
}

static void print_usage(const char *prog) {
  printf("NPS eBPF Loader\n\n");
  printf("Usage: %s [options]\n\n", prog);
  printf("Options:\n");
  printf("  -i, --iface IFACE    Network interface (required)\n");
  printf("  -a, --attach TYPE    Attach type: 'xdp' or 'tc' (default: xdp)\n");
  printf("  -d, --detach         Detach existing program\n");
  printf("  -p, --pin-path PATH  BPF map pin path (default: %s)\n",
         "/sys/fs/bpf/nps");
  printf("  -v, --verbose        Verbose output\n");
  printf("  -h, --help           Show this help\n");
}

int main(int argc, char *argv[]) {
  const char *iface = NULL;
  const char *attach_type = "xdp";
  const char *pin_path = "/sys/fs/bpf/nps";
  int detach = 0;
  int verbose = 0;

  static struct option long_options[] = {
      {"iface", required_argument, 0, 'i'},
      {"attach", required_argument, 0, 'a'},
      {"detach", no_argument, 0, 'd'},
      {"pin-path", required_argument, 0, 'p'},
      {"verbose", no_argument, 0, 'v'},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}};

  int opt;
  while ((opt = getopt_long(argc, argv, "i:a:dp:vh", long_options, NULL)) !=
         -1) {
    switch (opt) {
    case 'i':
      iface = optarg;
      break;
    case 'a':
      attach_type = optarg;
      break;
    case 'd':
      detach = 1;
      break;
    case 'p':
      pin_path = optarg;
      break;
    case 'v':
      verbose = 1;
      break;
    case 'h':
      print_usage(argv[0]);
      return 0;
    default:
      print_usage(argv[0]);
      return 1;
    }
  }

  if (!iface) {
    fprintf(stderr, "Error: --iface is required\n");
    print_usage(argv[0]);
    return 1;
  }

  unsigned int ifindex = if_nametoindex(iface);
  if (ifindex == 0) {
    fprintf(stderr, "Error: interface '%s' not found: %s\n", iface,
            strerror(errno));
    return 1;
  }

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  if (verbose) {
    printf("Interface: %s (index %u)\n", iface, ifindex);
    printf("Attach type: %s\n", attach_type);
    printf("Pin path: %s\n", pin_path);
  }

  /* ── Detach Mode ──────────────────────────────────────────────── */
  if (detach) {
    if (strcmp(attach_type, "xdp") == 0) {
      int err = bpf_xdp_detach(ifindex, 0, NULL);
      if (err) {
        fprintf(stderr, "Failed to detach XDP: %s\n", strerror(-err));
        return 1;
      }
      printf("XDP program detached from %s\n", iface);
    } else {
      printf("TC detach: use 'tc qdisc del dev %s clsact' manually\n", iface);
    }
    return 0;
  }

  /* ── Load eBPF Object ─────────────────────────────────────────── */
  const char *obj_file;
  if (strcmp(attach_type, "xdp") == 0) {
    obj_file = "xdp_filter.bpf.o";
  } else if (strcmp(attach_type, "tc") == 0) {
    obj_file = "tc_filter.bpf.o";
  } else {
    fprintf(stderr, "Unknown attach type: %s (use 'xdp' or 'tc')\n",
            attach_type);
    return 1;
  }

  struct bpf_object *obj = bpf_object__open(obj_file);
  if (!obj) {
    fprintf(stderr, "Failed to open %s: %s\n", obj_file, strerror(errno));
    return 1;
  }

  if (bpf_object__load(obj)) {
    fprintf(stderr, "Failed to load BPF object: %s\n", strerror(errno));
    bpf_object__close(obj);
    return 1;
  }

  printf("BPF object loaded: %s\n", obj_file);

  /* ── Pin Maps ─────────────────────────────────────────────────── */
  if (ensure_pin_path(pin_path) != 0 && errno != EEXIST) {
    fprintf(stderr, "Warning: cannot create pin path %s: %s\n", pin_path,
            strerror(errno));
  }

  struct bpf_map *map;
  bpf_object__for_each_map(map, obj) {
    const char *map_name = bpf_map__name(map);
    char pin_file[256];
    snprintf(pin_file, sizeof(pin_file), "%s/%s", pin_path, map_name);

    /* Unpin if exists */
    unlink(pin_file);

    int err = bpf_map__pin(map, pin_file);
    if (err) {
      fprintf(stderr, "Warning: failed to pin map '%s': %s\n", map_name,
              strerror(-err));
    } else if (verbose) {
      printf("  Pinned map: %s → %s\n", map_name, pin_file);
    }
  }

  /* ── Attach Program ───────────────────────────────────────────── */
  if (strcmp(attach_type, "xdp") == 0) {
    struct bpf_program *prog =
        bpf_object__find_program_by_name(obj, "nps_xdp_filter");
    if (!prog) {
      fprintf(stderr, "Cannot find XDP program 'nps_xdp_filter'\n");
      bpf_object__close(obj);
      return 1;
    }

    int prog_fd = bpf_program__fd(prog);
    int err = bpf_xdp_attach(ifindex, prog_fd, 0, NULL);
    if (err) {
      fprintf(stderr, "Failed to attach XDP: %s\n", strerror(-err));
      bpf_object__close(obj);
      return 1;
    }

    printf("XDP program attached to %s\n", iface);
  } else {
    /* TC attachment requires tc commands */
    printf("TC program loaded. Attach manually:\n");
    printf("  tc qdisc add dev %s clsact\n", iface);
    printf("  tc filter add dev %s egress bpf da obj %s sec tc\n", iface,
           obj_file);
  }

  /* ── Wait for Signal ──────────────────────────────────────────── */
  printf("\nPress Ctrl+C to detach and exit...\n");
  while (g_running) {
    sleep(1);
  }

  /* ── Cleanup ──────────────────────────────────────────────────── */
  if (strcmp(attach_type, "xdp") == 0) {
    bpf_xdp_detach(ifindex, 0, NULL);
    printf("\nXDP program detached from %s\n", iface);
  }

  bpf_object__close(obj);
  return 0;
}
