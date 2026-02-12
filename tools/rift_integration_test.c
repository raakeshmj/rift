/* rift_integration_test.c — unit + integration test suite */

#include "rift_buffer.h"
#include "rift_config.h"
#include "rift_congestion.h"
#include "rift_crc32.h"
#include "rift_log.h"
#include "rift_protocol.h"
#include "rift_rtt.h"
#include "rift_stats.h"
#include "rift_window.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Test Framework ───────────────────────────────────────────────── */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START(name)                                                       \
  do {                                                                         \
    tests_run++;                                                               \
    printf("  [TEST] %s... ", name);                                           \
    fflush(stdout);                                                            \
  } while (0)

#define TEST_PASS()                                                            \
  do {                                                                         \
    tests_passed++;                                                            \
    printf("\033[32mPASS\033[0m\n");                                           \
  } while (0)

#define TEST_FAIL(msg)                                                         \
  do {                                                                         \
    tests_failed++;                                                            \
    printf("\033[31mFAIL: %s\033[0m\n", msg);                                  \
  } while (0)

#define ASSERT_EQ(a, b, msg)                                                   \
  do {                                                                         \
    if ((a) != (b)) {                                                          \
      char _buf[256];                                                          \
      snprintf(_buf, sizeof(_buf), "%s: expected %ld, got %ld", msg,           \
               (long)(b), (long)(a));                                          \
      TEST_FAIL(_buf);                                                         \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_TRUE(cond, msg)                                                 \
  do {                                                                         \
    if (!(cond)) {                                                             \
      TEST_FAIL(msg);                                                          \
      return;                                                                  \
    }                                                                          \
  } while (0)

/* ── CRC32 Tests ──────────────────────────────────────────────────── */

static void test_crc32_basic(void) {
  TEST_START("CRC32 basic computation");
  rift_crc32_init();

  const uint8_t data[] = "Hello, World!";
  uint32_t crc = rift_crc32(data, strlen((const char *)data));

  /* Known CRC32 for "Hello, World!" = 0xEC4AC3D0 */
  ASSERT_EQ(crc, 0xEC4AC3D0u, "CRC32 mismatch");
  TEST_PASS();
}

static void test_crc32_empty(void) {
  TEST_START("CRC32 empty buffer");
  uint32_t crc = rift_crc32(NULL, 0);
  /* CRC32 of empty data = 0x00000000 */
  ASSERT_EQ(crc, 0x00000000u, "CRC32 empty mismatch");
  TEST_PASS();
}

static void test_crc32_incremental(void) {
  TEST_START("CRC32 incremental");
  rift_crc32_init();

  const uint8_t data[] = "Hello, World!";
  size_t len = strlen((const char *)data);

  /* Compute in one shot */
  uint32_t crc_full = rift_crc32(data, len);

  /* Compute incrementally */
  uint32_t crc = RIFT_CRC32_INIT;
  crc = rift_crc32_update(crc, data, 5);           /* "Hello" */
  crc = rift_crc32_update(crc, data + 5, len - 5); /* ", World!" */
  crc = rift_crc32_finalize(crc);

  ASSERT_EQ(crc, crc_full, "Incremental CRC32 mismatch");
  TEST_PASS();
}

/* ── Buffer Tests ─────────────────────────────────────────────────── */

static void test_buffer_init_destroy(void) {
  TEST_START("Buffer init/destroy");
  rift_buffer_t buf;
  ASSERT_EQ(rift_buffer_init(&buf, 16), 0, "Init failed");
  ASSERT_EQ(buf.capacity, 16u, "Capacity wrong");
  ASSERT_TRUE(rift_buffer_empty(&buf), "Should be empty");
  rift_buffer_destroy(&buf);
  TEST_PASS();
}

static void test_buffer_insert_remove(void) {
  TEST_START("Buffer insert/remove");
  rift_buffer_t buf;
  rift_buffer_init(&buf, 16);

  rift_packet_t pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.header.seq_num = 42;

  ASSERT_EQ(rift_buffer_insert(&buf, &pkt, RIFT_SLOT_PENDING), 0,
            "Insert failed");
  ASSERT_EQ(buf.count, 1u, "Count wrong after insert");

  rift_buffer_slot_t *slot = rift_buffer_get(&buf, 42);
  ASSERT_TRUE(slot != NULL, "Get returned NULL");
  ASSERT_EQ(slot->state, RIFT_SLOT_PENDING, "State wrong");

  ASSERT_EQ(rift_buffer_remove(&buf, 42), 0, "Remove failed");
  ASSERT_TRUE(rift_buffer_empty(&buf), "Should be empty after remove");

  rift_buffer_destroy(&buf);
  TEST_PASS();
}

static void test_buffer_set_state(void) {
  TEST_START("Buffer set state");
  rift_buffer_t buf;
  rift_buffer_init(&buf, 16);

  rift_packet_t pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.header.seq_num = 10;
  rift_buffer_insert(&buf, &pkt, RIFT_SLOT_PENDING);

  ASSERT_EQ(rift_buffer_set_state(&buf, 10, RIFT_SLOT_ACKED), 0,
            "Set state failed");

  rift_buffer_slot_t *slot = rift_buffer_get(&buf, 10);
  ASSERT_EQ(slot->state, RIFT_SLOT_ACKED, "State not updated");

  rift_buffer_destroy(&buf);
  TEST_PASS();
}

/* ── Packet Serialization Tests ───────────────────────────────────── */

static void test_packet_serialize_deserialize(void) {
  TEST_START("Packet serialize/deserialize");
  rift_crc32_init();

  rift_packet_t orig;
  uint8_t payload[] = "Hello RIFT!";
  rift_packet_build(&orig, RIFT_PKT_DATA, RIFT_FLAG_ACK, 100, 50, 64, 1234, NULL,
                   0, payload, (uint16_t)strlen((char *)payload));

  uint8_t wire[RIFT_MAX_SERIALIZED_SIZE];
  int len = rift_packet_serialize(&orig, wire, sizeof(wire));
  ASSERT_TRUE(len > 0, "Serialize failed");

  rift_packet_t decoded;
  ASSERT_EQ(rift_packet_deserialize(wire, (size_t)len, &decoded), 0,
            "Deserialize failed");

  ASSERT_EQ(decoded.header.version, RIFT_VERSION, "Version mismatch");
  ASSERT_EQ(decoded.header.type, RIFT_PKT_DATA, "Type mismatch");
  ASSERT_EQ(decoded.header.seq_num, 100u, "Seq mismatch");
  ASSERT_EQ(decoded.header.ack_num, 50u, "Ack mismatch");
  ASSERT_EQ(decoded.header.conn_id, 1234u, "ConnID mismatch");
  ASSERT_EQ(decoded.header.payload_len, (uint16_t)strlen((char *)payload),
            "Payload len mismatch");
  ASSERT_TRUE(memcmp(decoded.payload, payload, decoded.header.payload_len) == 0,
              "Payload data mismatch");

  TEST_PASS();
}

static void test_packet_with_sack(void) {
  TEST_START("Packet with SACK blocks");
  rift_crc32_init();

  rift_sack_block_t blocks[2] = {
      {.start_seq = 10, .end_seq = 15},
      {.start_seq = 20, .end_seq = 25},
  };

  rift_packet_t orig;
  rift_packet_build(&orig, RIFT_PKT_SACK, RIFT_FLAG_ACK | RIFT_FLAG_SACK, 0, 5, 64,
                   4321, blocks, 2, NULL, 0);

  uint8_t wire[RIFT_MAX_SERIALIZED_SIZE];
  int len = rift_packet_serialize(&orig, wire, sizeof(wire));
  ASSERT_TRUE(len > 0, "Serialize with SACK failed");

  rift_packet_t decoded;
  ASSERT_EQ(rift_packet_deserialize(wire, (size_t)len, &decoded), 0,
            "Deserialize with SACK failed");

  ASSERT_EQ(decoded.header.sack_count, 2u, "SACK count mismatch");
  ASSERT_EQ(decoded.sack_blocks[0].start_seq, 10u, "SACK[0].start mismatch");
  ASSERT_EQ(decoded.sack_blocks[0].end_seq, 15u, "SACK[0].end mismatch");
  ASSERT_EQ(decoded.sack_blocks[1].start_seq, 20u, "SACK[1].start mismatch");
  ASSERT_EQ(decoded.sack_blocks[1].end_seq, 25u, "SACK[1].end mismatch");

  TEST_PASS();
}

static void test_packet_crc_corruption(void) {
  TEST_START("Packet CRC corruption detection");
  rift_crc32_init();

  rift_packet_t pkt;
  uint8_t payload[] = "Test data";
  rift_packet_build(&pkt, RIFT_PKT_DATA, 0, 1, 0, 64, 999, NULL, 0, payload,
                   (uint16_t)strlen((char *)payload));

  uint8_t wire[RIFT_MAX_SERIALIZED_SIZE];
  int len = rift_packet_serialize(&pkt, wire, sizeof(wire));
  ASSERT_TRUE(len > 0, "Serialize failed");

  /* Corrupt a byte in the payload area */
  wire[len - 1] ^= 0xFF;

  rift_packet_t decoded;
  ASSERT_EQ(rift_packet_deserialize(wire, (size_t)len, &decoded), -1,
            "Should detect corruption");

  TEST_PASS();
}

/* ── RTT Estimator Tests ──────────────────────────────────────────── */

static void test_rtt_basic(void) {
  TEST_START("RTT estimator basic");
  rift_rtt_estimator_t rtt;
  rift_rtt_init(&rtt);

  ASSERT_EQ((int)rtt.rto_ms, RIFT_RTO_INIT_MS, "Initial RTO wrong");

  /* First sample: SRTT = R, RTTVAR = R/2 */
  rift_rtt_update(&rtt, 100.0);
  ASSERT_TRUE(fabs(rtt.srtt_ms - 100.0) < 0.001, "SRTT wrong after first");
  ASSERT_TRUE(fabs(rtt.rttvar_ms - 50.0) < 0.001, "RTTVAR wrong after first");

  TEST_PASS();
}

static void test_rtt_convergence(void) {
  TEST_START("RTT estimator convergence");
  rift_rtt_estimator_t rtt;
  rift_rtt_init(&rtt);

  /* Feed stable RTT = 50ms */
  for (int i = 0; i < 100; i++) {
    rift_rtt_update(&rtt, 50.0);
  }

  /* SRTT should converge close to 50 */
  ASSERT_TRUE(fabs(rtt.srtt_ms - 50.0) < 1.0, "SRTT didn't converge");

  TEST_PASS();
}

static void test_rtt_backoff(void) {
  TEST_START("RTT backoff");
  rift_rtt_estimator_t rtt;
  rift_rtt_init(&rtt);
  rift_rtt_update(&rtt, 100.0);

  double rto_before = rtt.rto_ms;
  rift_rtt_backoff(&rtt);

  ASSERT_TRUE(rtt.rto_ms >= rto_before * 1.5, "RTO didn't increase");
  ASSERT_TRUE(rtt.rto_ms <= RIFT_RTO_MAX_MS, "RTO exceeds max");

  TEST_PASS();
}

/* ── Congestion Control Tests ─────────────────────────────────────── */

static void test_cc_slow_start(void) {
  TEST_START("Congestion control slow start");
  rift_congestion_t cc;
  rift_cc_init(&cc);

  ASSERT_EQ(cc.phase, RIFT_CC_SLOW_START, "Should start in slow start");
  ASSERT_EQ(rift_cc_get_cwnd(&cc), RIFT_CWND_INIT, "Initial cwnd wrong");

  /* Simulate ACKs - cwnd should grow exponentially */
  uint32_t prev_cwnd = rift_cc_get_cwnd(&cc);
  uint64_t now = 1000000;
  for (int i = 0; i < 5; i++) {
    rift_cc_on_ack(&cc, 1, now);
    now += 100000;
  }
  ASSERT_TRUE(rift_cc_get_cwnd(&cc) > prev_cwnd, "cwnd should grow");

  TEST_PASS();
}

static void test_cc_loss_response(void) {
  TEST_START("Congestion control loss response");
  rift_congestion_t cc;
  rift_cc_init(&cc);

  /* Grow window */
  uint64_t now = 1000000;
  for (int i = 0; i < 50; i++) {
    rift_cc_on_ack(&cc, 1, now);
    now += 100000;
  }

  uint32_t cwnd_before = rift_cc_get_cwnd(&cc);

  /* Timeout loss → reset to slow start */
  rift_cc_on_loss(&cc, true, now);
  ASSERT_EQ(cc.phase, RIFT_CC_SLOW_START, "Should be in slow start after TO");
  ASSERT_TRUE(rift_cc_get_cwnd(&cc) < cwnd_before,
              "cwnd should decrease after loss");

  TEST_PASS();
}

static void test_cc_fast_recovery(void) {
  TEST_START("Congestion control fast recovery");
  rift_congestion_t cc;
  rift_cc_init(&cc);

  /* Grow window */
  uint64_t now = 1000000;
  for (int i = 0; i < 50; i++) {
    rift_cc_on_ack(&cc, 1, now);
    now += 100000;
  }

  /* Fast retransmit (not timeout) */
  rift_cc_on_loss(&cc, false, now);
  ASSERT_EQ(cc.phase, RIFT_CC_FAST_RECOVERY, "Should be in fast recovery");

  TEST_PASS();
}

/* ── Window Tests ─────────────────────────────────────────────────── */

static void test_window_basic(void) {
  TEST_START("Window basic operations");
  rift_window_t win;
  ASSERT_EQ(rift_window_init(&win, 0, 4), 0, "Window init failed");

  ASSERT_TRUE(rift_window_can_send(&win), "Should be able to send");
  ASSERT_EQ(rift_window_in_flight(&win), 0u, "No packets in flight");

  rift_window_destroy(&win);
  TEST_PASS();
}

static void test_window_send_ack(void) {
  TEST_START("Window send and ACK");
  rift_window_t win;
  rift_window_init(&win, 0, 4);
  win.cwnd = 4;

  /* Send 4 packets */
  for (uint32_t i = 0; i < 4; i++) {
    rift_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.seq_num = i;
    ASSERT_EQ(rift_window_mark_sent(&win, &pkt, 1000000), 0, "Mark sent failed");
  }

  ASSERT_EQ(rift_window_in_flight(&win), 4u, "Should have 4 in flight");
  ASSERT_TRUE(!rift_window_can_send(&win), "Window should be full");

  /* ACK first 2 */
  uint32_t acked = rift_window_process_ack(&win, 2);
  ASSERT_EQ(acked, 2u, "Should ACK 2");
  ASSERT_EQ(rift_window_in_flight(&win), 2u, "Should have 2 in flight");
  ASSERT_TRUE(rift_window_can_send(&win), "Should be able to send again");

  rift_window_destroy(&win);
  TEST_PASS();
}

static void test_window_sack(void) {
  TEST_START("Window SACK processing");
  rift_window_t win;
  rift_window_init(&win, 0, 8);
  win.cwnd = 8;

  /* Send 6 packets: 0,1,2,3,4,5 */
  for (uint32_t i = 0; i < 6; i++) {
    rift_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.seq_num = i;
    rift_window_mark_sent(&win, &pkt, 1000000);
  }

  /* SACK: packets 2-3 and 5 received (gaps at 0-1 and 4) */
  rift_sack_block_t blocks[2] = {
      {.start_seq = 2, .end_seq = 4},
      {.start_seq = 5, .end_seq = 6},
  };

  uint32_t sacked = rift_window_process_sack(&win, blocks, 2);
  ASSERT_EQ(sacked, 3u, "Should SACK 3 packets");

  /* Verify states */
  rift_buffer_slot_t *s2 = rift_buffer_get(&win.buffer, 2);
  ASSERT_TRUE(s2 && s2->state == RIFT_SLOT_ACKED, "Seq 2 should be ACKED");

  rift_buffer_slot_t *s4 = rift_buffer_get(&win.buffer, 4);
  ASSERT_TRUE(s4 && s4->state == RIFT_SLOT_PENDING, "Seq 4 should be PENDING");

  rift_window_destroy(&win);
  TEST_PASS();
}

/* ── Stats Tests ──────────────────────────────────────────────────── */

static void test_stats_basic(void) {
  TEST_START("Stats counters");
  rift_stats_t stats;
  rift_stats_init(&stats);

  RIFT_STAT_INC(&stats, packets_sent);
  RIFT_STAT_INC(&stats, packets_sent);
  RIFT_STAT_ADD(&stats, bytes_sent, 1400);

  ASSERT_EQ(RIFT_STAT_GET(&stats, packets_sent), 2u, "Packet count wrong");
  ASSERT_EQ(RIFT_STAT_GET(&stats, bytes_sent), 1400u, "Byte count wrong");

  TEST_PASS();
}

/* ── String Helper Tests ──────────────────────────────────────────── */

static void test_type_strings(void) {
  TEST_START("Packet type strings");
  ASSERT_TRUE(strcmp(rift_pkt_type_str(RIFT_PKT_DATA), "DATA") == 0,
              "DATA string wrong");
  ASSERT_TRUE(strcmp(rift_pkt_type_str(RIFT_PKT_SYN), "SYN") == 0,
              "SYN string wrong");
  ASSERT_TRUE(
      strcmp(rift_conn_state_str(RIFT_STATE_ESTABLISHED), "ESTABLISHED") == 0,
      "ESTABLISHED string wrong");
  TEST_PASS();
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
  rift_log_init(RIFT_LOG_WARN, NULL);

  printf("\n");
  printf("╔═══════════════════════════════════════════════════════╗\n");
  printf("║          RIFT Integration Test Suite                  ║\n");
  printf("╚═══════════════════════════════════════════════════════╝\n\n");

  printf("── CRC32 ──────────────────────────────────────────────\n");
  test_crc32_basic();
  test_crc32_empty();
  test_crc32_incremental();

  printf("\n── Buffer ─────────────────────────────────────────────\n");
  test_buffer_init_destroy();
  test_buffer_insert_remove();
  test_buffer_set_state();

  printf("\n── Packet Serialization ────────────────────────────────\n");
  test_packet_serialize_deserialize();
  test_packet_with_sack();
  test_packet_crc_corruption();

  printf("\n── RTT Estimator ──────────────────────────────────────\n");
  test_rtt_basic();
  test_rtt_convergence();
  test_rtt_backoff();

  printf("\n── Congestion Control ─────────────────────────────────\n");
  test_cc_slow_start();
  test_cc_loss_response();
  test_cc_fast_recovery();

  printf("\n── Sliding Window ─────────────────────────────────────\n");
  test_window_basic();
  test_window_send_ack();
  test_window_sack();

  printf("\n── Statistics ─────────────────────────────────────────\n");
  test_stats_basic();

  printf("\n── String Helpers ─────────────────────────────────────\n");
  test_type_strings();

  printf("\n═══════════════════════════════════════════════════════\n");
  printf("  Results: %d/%d passed", tests_passed, tests_run);
  if (tests_failed > 0)
    printf(", \033[31m%d FAILED\033[0m", tests_failed);
  printf("\n");
  printf("═══════════════════════════════════════════════════════\n\n");

  rift_log_shutdown();
  return tests_failed > 0 ? 1 : 0;
}
