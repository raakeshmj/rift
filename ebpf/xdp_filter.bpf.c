// SPDX-License-Identifier: GPL-2.0
/* xdp_filter.bpf.c — XDP ingress packet filter for RIFT */

#include "common.bpf.h"
#include "vmlinux.h"
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

/* ── BPF Maps ─────────────────────────────────────────────────────── */

/* Filter rules: key = {src_ip, dst_ip, src_port, dst_port, proto} */
struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, RIFT_MAX_FILTER_RULES);
  __type(key, struct rift_filter_key);
  __type(value, struct rift_filter_rule);
} rift_filter_rules SEC(".maps");

/* Per-connection statistics */
struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, RIFT_MAX_CONNECTIONS);
  __type(key, struct rift_filter_key);
  __type(value, struct rift_conn_stats);
} rift_conn_stats SEC(".maps");

/* Per-connection rate limiter state */
struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, RIFT_MAX_CONNECTIONS);
  __type(key, struct rift_filter_key);
  __type(value, struct rift_rate_state);
} rift_rate_state SEC(".maps");

/* Global statistics (per-CPU) */
struct {
  __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  __uint(max_entries, 1);
  __type(key, __u32);
  __type(value, struct rift_global_stats);
} rift_global_stats SEC(".maps");

/* ── Helpers ──────────────────────────────────────────────────────── */

static __always_inline void update_global_stats(struct rift_global_stats *g,
                                                __u64 bytes, int passed,
                                                int is_rift) {
  g->total_packets++;
  g->total_bytes += bytes;
  if (passed) {
    g->total_passed++;
  } else {
    g->total_dropped++;
  }
  if (is_rift) {
    g->rift_protocol_packets++;
  }
}

static __always_inline void update_conn_stats(struct rift_filter_key *key,
                                              __u64 bytes, int passed) {
  struct rift_conn_stats *stats;
  __u64 now = bpf_ktime_get_ns();

  stats = bpf_map_lookup_elem(&rift_conn_stats, key);
  if (stats) {
    stats->packets++;
    stats->bytes += bytes;
    stats->last_seen_ns = now;
    if (passed)
      stats->packets_passed++;
    else
      stats->packets_dropped++;
  } else {
    struct rift_conn_stats new_stats = {
        .packets = 1,
        .bytes = bytes,
        .packets_passed = passed ? 1 : 0,
        .packets_dropped = passed ? 0 : 1,
        .first_seen_ns = now,
        .last_seen_ns = now,
    };
    bpf_map_update_elem(&rift_conn_stats, key, &new_stats, BPF_ANY);
  }
}

static __always_inline int check_rate_limit(struct rift_filter_key *key,
                                            struct rift_filter_rule *rule) {
  struct rift_rate_state *state;
  __u64 now = bpf_ktime_get_ns();

  state = bpf_map_lookup_elem(&rift_rate_state, key);
  if (!state) {
    /* Initialize rate state */
    struct rift_rate_state new_state = {
        .tokens = rule->rate_pps > 0 ? rule->rate_pps : RIFT_TOKEN_BUCKET_SIZE,
        .last_update_ns = now,
        .max_tokens =
            rule->rate_pps > 0 ? rule->rate_pps : RIFT_TOKEN_BUCKET_SIZE,
        .refill_rate = rule->rate_pps > 0 ? rule->rate_pps : RIFT_TOKENS_PER_SEC,
    };
    bpf_map_update_elem(&rift_rate_state, key, &new_state, BPF_ANY);
    return 1; /* Allow first packet */
  }

  /* Refill tokens based on elapsed time */
  __u64 elapsed_ns = now - state->last_update_ns;
  __u64 new_tokens = (elapsed_ns * state->refill_rate) / RIFT_NS_PER_SEC;

  if (new_tokens > 0) {
    state->tokens += new_tokens;
    if (state->tokens > state->max_tokens)
      state->tokens = state->max_tokens;
    state->last_update_ns = now;
  }

  /* Consume a token */
  if (state->tokens > 0) {
    state->tokens--;
    return 1; /* Allow */
  }

  return 0; /* Rate limited → drop */
}

/* ── XDP Program ──────────────────────────────────────────────────── */

SEC("xdp")
int rift_xdp_filter(struct xdp_md *ctx) {
  void *data = (void *)(long)ctx->data;
  void *data_end = (void *)(long)ctx->data_end;

  /* ── Parse Ethernet Header ──────────────────────────────────── */
  struct ethhdr *eth = data;
  if ((void *)(eth + 1) > data_end)
    return XDP_PASS;

  /* Only process IPv4 */
  if (eth->h_proto != bpf_htons(ETH_P_IP))
    return XDP_PASS;

  /* ── Parse IP Header ────────────────────────────────────────── */
  struct iphdr *ip = (void *)(eth + 1);
  if ((void *)(ip + 1) > data_end)
    return XDP_PASS;

  /* Minimum IP header length check */
  if (ip->ihl < 5)
    return XDP_PASS;

  __u64 pkt_len = data_end - data;

  /* ── Parse Transport Header ─────────────────────────────────── */
  __u16 src_port = 0, dst_port = 0;
  void *transport = (void *)ip + (ip->ihl * 4);

  if (ip->protocol == IPPROTO_UDP) {
    struct udphdr *udp = transport;
    if ((void *)(udp + 1) > data_end)
      return XDP_PASS;
    src_port = udp->source;
    dst_port = udp->dest;
  } else if (ip->protocol == IPPROTO_TCP) {
    struct tcphdr *tcp = transport;
    if ((void *)(tcp + 1) > data_end)
      return XDP_PASS;
    src_port = tcp->source;
    dst_port = tcp->dest;
  } else {
    return XDP_PASS; /* Not TCP/UDP, pass through */
  }

  /* ── Build Filter Key ───────────────────────────────────────── */
  struct rift_filter_key key = {
      .src_ip = ip->saddr,
      .dst_ip = ip->daddr,
      .src_port = src_port,
      .dst_port = dst_port,
      .protocol = ip->protocol,
  };

  /* Check if this is RIFT protocol traffic */
  int is_rift = (dst_port == bpf_htons(9999) || src_port == bpf_htons(9999));

  /* ── Global Stats ───────────────────────────────────────────── */
  __u32 stats_key = 0;
  struct rift_global_stats *gstats;
  gstats = bpf_map_lookup_elem(&rift_global_stats, &stats_key);

  /* ── Lookup Filter Rule ─────────────────────────────────────── */
  struct rift_filter_rule *rule;
  int action = RIFT_ACTION_PASS;

  /* Try exact match first */
  rule = bpf_map_lookup_elem(&rift_filter_rules, &key);

  if (!rule) {
    /* Try wildcard: match on dst_ip + dst_port only */
    struct rift_filter_key wild_key = {
        .src_ip = 0,
        .dst_ip = ip->daddr,
        .src_port = 0,
        .dst_port = dst_port,
        .protocol = ip->protocol,
    };
    rule = bpf_map_lookup_elem(&rift_filter_rules, &wild_key);
  }

  if (rule) {
    action = rule->action;
  }

  /* ── Apply Action ───────────────────────────────────────────── */
  int passed = 1;

  switch (action) {
  case RIFT_ACTION_DROP:
    passed = 0;
    break;

  case RIFT_ACTION_LIMIT:
    if (rule && !check_rate_limit(&key, rule)) {
      passed = 0;
      if (gstats)
        gstats->total_rate_limited++;
    }
    break;

  case RIFT_ACTION_PASS:
  default:
    break;
  }

  /* ── Update Stats ───────────────────────────────────────────── */
  if (gstats)
    update_global_stats(gstats, pkt_len, passed, is_rift);

  update_conn_stats(&key, pkt_len, passed);

  return passed ? XDP_PASS : XDP_DROP;
}

char _license[] SEC("license") = "GPL";
