// SPDX-License-Identifier: GPL-2.0
/* tc_filter.bpf.c — TC egress packet filter for RIFT */

#include "common.bpf.h"
#include "vmlinux.h"
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

#define TC_ACT_OK 0
#define TC_ACT_SHOT 2

/* ── BPF Maps (shared naming with XDP program) ───────────────────── */

struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, RIFT_MAX_FILTER_RULES);
  __type(key, struct rift_filter_key);
  __type(value, struct rift_filter_rule);
} rift_tc_filter_rules SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, RIFT_MAX_CONNECTIONS);
  __type(key, struct rift_filter_key);
  __type(value, struct rift_conn_stats);
} rift_tc_conn_stats SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, RIFT_MAX_CONNECTIONS);
  __type(key, struct rift_filter_key);
  __type(value, struct rift_rate_state);
} rift_tc_rate_state SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  __uint(max_entries, 1);
  __type(key, __u32);
  __type(value, struct rift_global_stats);
} rift_tc_global_stats SEC(".maps");

/* ── Rate Limit Check ─────────────────────────────────────────────── */

static __always_inline int tc_check_rate_limit(struct rift_filter_key *key,
                                               struct rift_filter_rule *rule) {
  struct rift_rate_state *state;
  __u64 now = bpf_ktime_get_ns();

  state = bpf_map_lookup_elem(&rift_tc_rate_state, key);
  if (!state) {
    struct rift_rate_state new_state = {
        .tokens = rule->rate_pps > 0 ? rule->rate_pps : RIFT_TOKEN_BUCKET_SIZE,
        .last_update_ns = now,
        .max_tokens =
            rule->rate_pps > 0 ? rule->rate_pps : RIFT_TOKEN_BUCKET_SIZE,
        .refill_rate = rule->rate_pps > 0 ? rule->rate_pps : RIFT_TOKENS_PER_SEC,
    };
    bpf_map_update_elem(&rift_tc_rate_state, key, &new_state, BPF_ANY);
    return 1;
  }

  __u64 elapsed_ns = now - state->last_update_ns;
  __u64 new_tokens = (elapsed_ns * state->refill_rate) / RIFT_NS_PER_SEC;

  if (new_tokens > 0) {
    state->tokens += new_tokens;
    if (state->tokens > state->max_tokens)
      state->tokens = state->max_tokens;
    state->last_update_ns = now;
  }

  if (state->tokens > 0) {
    state->tokens--;
    return 1;
  }

  return 0;
}

/* ── TC Program ───────────────────────────────────────────────────── */

SEC("tc")
int rift_tc_filter(struct __sk_buff *skb) {
  void *data = (void *)(long)skb->data;
  void *data_end = (void *)(long)skb->data_end;

  /* Parse Ethernet */
  struct ethhdr *eth = data;
  if ((void *)(eth + 1) > data_end)
    return TC_ACT_OK;

  if (eth->h_proto != bpf_htons(ETH_P_IP))
    return TC_ACT_OK;

  /* Parse IP */
  struct iphdr *ip = (void *)(eth + 1);
  if ((void *)(ip + 1) > data_end)
    return TC_ACT_OK;

  if (ip->ihl < 5)
    return TC_ACT_OK;

  __u64 pkt_len = data_end - data;

  /* Parse Transport */
  __u16 src_port = 0, dst_port = 0;
  void *transport = (void *)ip + (ip->ihl * 4);

  if (ip->protocol == IPPROTO_UDP) {
    struct udphdr *udp = transport;
    if ((void *)(udp + 1) > data_end)
      return TC_ACT_OK;
    src_port = udp->source;
    dst_port = udp->dest;
  } else if (ip->protocol == IPPROTO_TCP) {
    struct tcphdr *tcp = transport;
    if ((void *)(tcp + 1) > data_end)
      return TC_ACT_OK;
    src_port = tcp->source;
    dst_port = tcp->dest;
  } else {
    return TC_ACT_OK;
  }

  /* Build key */
  struct rift_filter_key key = {
      .src_ip = ip->saddr,
      .dst_ip = ip->daddr,
      .src_port = src_port,
      .dst_port = dst_port,
      .protocol = ip->protocol,
  };

  int is_rift = (dst_port == bpf_htons(9999) || src_port == bpf_htons(9999));

  /* Global stats */
  __u32 stats_key = 0;
  struct rift_global_stats *gstats;
  gstats = bpf_map_lookup_elem(&rift_tc_global_stats, &stats_key);

  /* Filter rule lookup */
  struct rift_filter_rule *rule;
  rule = bpf_map_lookup_elem(&rift_tc_filter_rules, &key);

  if (!rule) {
    struct rift_filter_key wild_key = {
        .src_ip = ip->saddr,
        .dst_ip = 0,
        .src_port = src_port,
        .dst_port = 0,
        .protocol = ip->protocol,
    };
    rule = bpf_map_lookup_elem(&rift_tc_filter_rules, &wild_key);
  }

  int passed = 1;

  if (rule) {
    switch (rule->action) {
    case RIFT_ACTION_DROP:
      passed = 0;
      break;
    case RIFT_ACTION_LIMIT:
      if (!tc_check_rate_limit(&key, rule)) {
        passed = 0;
        if (gstats)
          gstats->total_rate_limited++;
      }
      break;
    default:
      break;
    }
  }

  /* Update stats */
  if (gstats) {
    gstats->total_packets++;
    gstats->total_bytes += pkt_len;
    if (passed)
      gstats->total_passed++;
    else
      gstats->total_dropped++;
    if (is_rift)
      gstats->rift_protocol_packets++;
  }

  /* Connection stats */
  __u64 now = bpf_ktime_get_ns();
  struct rift_conn_stats *cstats;
  cstats = bpf_map_lookup_elem(&rift_tc_conn_stats, &key);
  if (cstats) {
    cstats->packets++;
    cstats->bytes += pkt_len;
    cstats->last_seen_ns = now;
    if (passed)
      cstats->packets_passed++;
    else
      cstats->packets_dropped++;
  } else {
    struct rift_conn_stats new_stats = {
        .packets = 1,
        .bytes = pkt_len,
        .packets_passed = passed ? 1 : 0,
        .packets_dropped = passed ? 0 : 1,
        .first_seen_ns = now,
        .last_seen_ns = now,
    };
    bpf_map_update_elem(&rift_tc_conn_stats, &key, &new_stats, BPF_ANY);
  }

  return passed ? TC_ACT_OK : TC_ACT_SHOT;
}

char _license[] SEC("license") = "GPL";
