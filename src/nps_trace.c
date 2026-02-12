/* nps_trace.c — packet trace logger (JSON Lines + columnar text) */

#include "nps_trace.h"
#include "nps_log.h"
#include "nps_protocol.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* ── Global State ────────────────────────────────────────────────── */

static struct {
  FILE *json_fp;
  FILE *text_fp;
  nps_trace_format_t format;
  nps_trace_stats_t stats;
  pthread_mutex_t mutex;
  bool initialized;
  uint64_t base_ts; /* First timestamp, for relative output */
} g_trace = {
    .initialized = false,
};

/* ── Event Name Strings ──────────────────────────────────────────── */

static const char *event_names[] = {
    [NPS_TRACE_TX] = "TX",           [NPS_TRACE_RX] = "RX",
    [NPS_TRACE_ACK] = "ACK",         [NPS_TRACE_SACK] = "SACK",
    [NPS_TRACE_RETX] = "RETX",       [NPS_TRACE_DROP] = "DROP",
    [NPS_TRACE_CWND] = "CWND",       [NPS_TRACE_RTT] = "RTT",
    [NPS_TRACE_STATE] = "STATE",     [NPS_TRACE_LOSS] = "LOSS",
    [NPS_TRACE_TIMEOUT] = "TIMEOUT",
};

static const char *event_colors[] = {
    [NPS_TRACE_TX] = "\033[32m",      /* Green    */
    [NPS_TRACE_RX] = "\033[36m",      /* Cyan     */
    [NPS_TRACE_ACK] = "\033[34m",     /* Blue     */
    [NPS_TRACE_SACK] = "\033[35m",    /* Magenta  */
    [NPS_TRACE_RETX] = "\033[33m",    /* Yellow   */
    [NPS_TRACE_DROP] = "\033[31m",    /* Red      */
    [NPS_TRACE_CWND] = "\033[37m",    /* White    */
    [NPS_TRACE_RTT] = "\033[37m",     /* White    */
    [NPS_TRACE_STATE] = "\033[1m",    /* Bold     */
    [NPS_TRACE_LOSS] = "\033[31m",    /* Red      */
    [NPS_TRACE_TIMEOUT] = "\033[33m", /* Yellow */
};

#define COLOR_RESET "\033[0m"

const char *nps_trace_event_str(nps_trace_event_t event) {
  if (event <= NPS_TRACE_TIMEOUT)
    return event_names[event];
  return "UNKNOWN";
}

/* ── Initialization ──────────────────────────────────────────────── */

int nps_trace_init(const char *base_path, nps_trace_format_t format) {
  if (g_trace.initialized)
    nps_trace_shutdown();

  memset(&g_trace, 0, sizeof(g_trace));
  pthread_mutex_init(&g_trace.mutex, NULL);
  g_trace.format = format;
  g_trace.base_ts = nps_timestamp_us();

  if (base_path) {
    char path[512];

    if (format & NPS_TRACE_FMT_JSON) {
      snprintf(path, sizeof(path), "%s.jsonl", base_path);
      g_trace.json_fp = fopen(path, "w");
      if (!g_trace.json_fp) {
        NPS_LOG_ERROR("Failed to open trace file: %s", path);
        return -1;
      }
      NPS_LOG_INFO("Trace JSON output: %s", path);
    }

    if (format & NPS_TRACE_FMT_TEXT) {
      snprintf(path, sizeof(path), "%s.txt", base_path);
      g_trace.text_fp = fopen(path, "w");
      if (!g_trace.text_fp) {
        NPS_LOG_ERROR("Failed to open trace file: %s", path);
        return -1;
      }
      /* Header line */
      fprintf(g_trace.text_fp,
              "%-12s %-3s %-7s %-8s %-5s %-10s %-10s %-6s %-6s "
              "%-6s %-8s %-8s %s\n",
              "TIME_REL_MS", "DIR", "EVENT", "PKT_TYPE", "FLAGS", "SEQ", "ACK",
              "PLEN", "WIN", "CWND", "RTT_MS", "RTO_MS", "DETAIL");
      fprintf(g_trace.text_fp,
              "──────────── ─── ─────── ──────── ───── ────────── "
              "────────── ────── ────── ────── ──────── ──────── "
              "────────────────────\n");
      NPS_LOG_INFO("Trace TEXT output: %s", path);
    }
  }

  g_trace.initialized = true;
  return 0;
}

/* ── Log Entry: JSON ─────────────────────────────────────────────── */

static void trace_write_json(const nps_trace_entry_t *e) {
  if (!g_trace.json_fp)
    return;

  double rel_ms = (double)(e->timestamp_us - g_trace.base_ts) / 1000.0;

  fprintf(g_trace.json_fp,
          "{\"t_ms\":%.3f,\"dir\":\"%c\",\"event\":\"%s\","
          "\"pkt_type\":\"%s\",\"flags\":%u,"
          "\"seq\":%u,\"ack\":%u,\"plen\":%u,\"win\":%u,"
          "\"conn_id\":%u,\"cwnd\":%u,\"ssthresh\":%u,"
          "\"rtt_ms\":%.3f,\"rto_ms\":%.3f",
          rel_ms, e->direction, nps_trace_event_str(e->event),
          nps_pkt_type_str(e->pkt_type), e->flags, e->seq_num, e->ack_num,
          e->payload_len, e->window_size, e->conn_id, e->cwnd, e->ssthresh,
          e->rtt_ms, e->rto_ms);

  if (e->event == NPS_TRACE_STATE) {
    fprintf(g_trace.json_fp, ",\"old_state\":\"%s\",\"new_state\":\"%s\"",
            nps_conn_state_str((nps_conn_state_t)e->old_state),
            nps_conn_state_str((nps_conn_state_t)e->new_state));
  }

  if (e->sack_count > 0) {
    fprintf(g_trace.json_fp, ",\"sack_blocks\":[");
    for (uint16_t i = 0; i < e->sack_count && i < 4; i++) {
      if (i > 0)
        fprintf(g_trace.json_fp, ",");
      fprintf(g_trace.json_fp, "[%u,%u]", e->sack_start[i], e->sack_end[i]);
    }
    fprintf(g_trace.json_fp, "]");
  }

  if (e->detail) {
    fprintf(g_trace.json_fp, ",\"detail\":\"%s\"", e->detail);
  }

  fprintf(g_trace.json_fp, "}\n");
  fflush(g_trace.json_fp);
}

/* ── Log Entry: Text ─────────────────────────────────────────────── */

static void trace_write_text(const nps_trace_entry_t *e) {
  if (!g_trace.text_fp)
    return;

  double rel_ms = (double)(e->timestamp_us - g_trace.base_ts) / 1000.0;

  /* Build flags string */
  char flags_str[16] = "";
  int fpos = 0;
  if (e->flags & 0x01)
    flags_str[fpos++] = 'S'; /* SYN   */
  if (e->flags & 0x02)
    flags_str[fpos++] = 'F'; /* FIN   */
  if (e->flags & 0x04)
    flags_str[fpos++] = 'A'; /* ACK   */
  if (e->flags & 0x08)
    flags_str[fpos++] = 'K'; /* SACK  */
  if (e->flags & 0x10)
    flags_str[fpos++] = 'N'; /* NACK  */
  if (e->flags & 0x20)
    flags_str[fpos++] = 'R'; /* RST   */
  if (e->flags & 0x80)
    flags_str[fpos++] = 'E'; /* ENC   */
  flags_str[fpos] = '\0';
  if (fpos == 0) {
    flags_str[0] = '-';
    flags_str[1] = '\0';
  }

  /* Detail or SACK info */
  char detail_buf[128] = "";
  if (e->event == NPS_TRACE_SACK && e->sack_count > 0) {
    int dpos = 0;
    dpos +=
        snprintf(detail_buf + dpos, sizeof(detail_buf) - (size_t)dpos, "SACK[");
    for (uint16_t i = 0; i < e->sack_count && i < 4; i++) {
      if (i > 0)
        dpos +=
            snprintf(detail_buf + dpos, sizeof(detail_buf) - (size_t)dpos, ",");
      dpos += snprintf(detail_buf + dpos, sizeof(detail_buf) - (size_t)dpos,
                       "%u-%u", e->sack_start[i], e->sack_end[i]);
    }
    snprintf(detail_buf + dpos, sizeof(detail_buf) - (size_t)dpos, "]");
  } else if (e->event == NPS_TRACE_STATE) {
    snprintf(detail_buf, sizeof(detail_buf), "%s→%s",
             nps_conn_state_str((nps_conn_state_t)e->old_state),
             nps_conn_state_str((nps_conn_state_t)e->new_state));
  } else if (e->detail) {
    snprintf(detail_buf, sizeof(detail_buf), "%s", e->detail);
  }

  fprintf(g_trace.text_fp,
          "%s%10.3f   %c   %-7s %-8s %-5s %10u %10u %6u %6u "
          "%6u %8.2f %8.2f %s%s\n",
          event_colors[e->event < 11 ? e->event : 0], rel_ms, e->direction,
          nps_trace_event_str(e->event), nps_pkt_type_str(e->pkt_type),
          flags_str, e->seq_num, e->ack_num, e->payload_len, e->window_size,
          e->cwnd, e->rtt_ms, e->rto_ms, detail_buf, COLOR_RESET);

  fflush(g_trace.text_fp);
}

/* ── Update Stats ────────────────────────────────────────────────── */

static void trace_update_stats(const nps_trace_entry_t *e) {
  nps_trace_stats_t *s = &g_trace.stats;

  switch (e->event) {
  case NPS_TRACE_TX:
    s->count_tx++;
    break;
  case NPS_TRACE_RX:
    s->count_rx++;
    break;
  case NPS_TRACE_ACK:
    s->count_ack++;
    break;
  case NPS_TRACE_SACK:
    s->count_sack++;
    break;
  case NPS_TRACE_RETX:
    s->count_retx++;
    break;
  case NPS_TRACE_DROP:
    s->count_drop++;
    break;
  case NPS_TRACE_CWND:
    s->count_cwnd++;
    break;
  case NPS_TRACE_RTT:
    s->count_rtt++;
    break;
  case NPS_TRACE_STATE:
    s->count_state++;
    break;
  case NPS_TRACE_LOSS:
    s->count_loss++;
    break;
  case NPS_TRACE_TIMEOUT:
    s->count_timeout++;
    break;
  }

  s->total_events++;

  if (s->first_ts == 0)
    s->first_ts = e->timestamp_us;
  s->last_ts = e->timestamp_us;
}

/* ── Public: Log Entry ───────────────────────────────────────────── */

void nps_trace_log(const nps_trace_entry_t *entry) {
  if (!g_trace.initialized)
    return;

  pthread_mutex_lock(&g_trace.mutex);

  trace_write_json(entry);
  trace_write_text(entry);
  trace_update_stats(entry);

  pthread_mutex_unlock(&g_trace.mutex);
}

/* ── Public: Get Stats ───────────────────────────────────────────── */

nps_trace_stats_t nps_trace_get_stats(void) {
  nps_trace_stats_t s;
  pthread_mutex_lock(&g_trace.mutex);
  s = g_trace.stats;
  pthread_mutex_unlock(&g_trace.mutex);
  return s;
}

/* ── Public: Dump Summary ────────────────────────────────────────── */

void nps_trace_dump_summary(void) {
  nps_trace_stats_t s = nps_trace_get_stats();
  double dur_ms = (s.last_ts > s.first_ts)
                      ? (double)(s.last_ts - s.first_ts) / 1000.0
                      : 0.0;

  NPS_LOG_INFO("╔═══════════════════ Trace Summary ═══════════════════╗");
  NPS_LOG_INFO("║  Duration:     %.2f ms", dur_ms);
  NPS_LOG_INFO("║  Total events: %" PRIu64, s.total_events);
  NPS_LOG_INFO("║  ── Breakdown ──");
  NPS_LOG_INFO("║  TX:       %" PRIu64, s.count_tx);
  NPS_LOG_INFO("║  RX:       %" PRIu64, s.count_rx);
  NPS_LOG_INFO("║  ACK:      %" PRIu64, s.count_ack);
  NPS_LOG_INFO("║  SACK:     %" PRIu64, s.count_sack);
  NPS_LOG_INFO("║  RETX:     %" PRIu64, s.count_retx);
  NPS_LOG_INFO("║  DROP:     %" PRIu64, s.count_drop);
  NPS_LOG_INFO("║  CWND chg: %" PRIu64, s.count_cwnd);
  NPS_LOG_INFO("║  RTT meas: %" PRIu64, s.count_rtt);
  NPS_LOG_INFO("║  STATE:    %" PRIu64, s.count_state);
  NPS_LOG_INFO("║  LOSS det: %" PRIu64, s.count_loss);
  NPS_LOG_INFO("║  TIMEOUT:  %" PRIu64, s.count_timeout);
  if (s.count_tx > 0) {
    double retx_pct = (double)s.count_retx / (double)s.count_tx * 100.0;
    NPS_LOG_INFO("║  Retransmit ratio: %.2f%%", retx_pct);
  }
  NPS_LOG_INFO("╚════════════════════════════════════════════════════╝");
}

/* ── Public: Shutdown ────────────────────────────────────────────── */

void nps_trace_shutdown(void) {
  if (!g_trace.initialized)
    return;

  pthread_mutex_lock(&g_trace.mutex);

  if (g_trace.json_fp) {
    fclose(g_trace.json_fp);
    g_trace.json_fp = NULL;
  }
  if (g_trace.text_fp) {
    fclose(g_trace.text_fp);
    g_trace.text_fp = NULL;
  }

  g_trace.initialized = false;
  pthread_mutex_unlock(&g_trace.mutex);
  pthread_mutex_destroy(&g_trace.mutex);
}
