/* nps_crypto.c — ChaCha20 stream cipher (RFC 8439) */

#include "nps_crypto.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ── ChaCha20 Constants ──────────────────────────────────────────── */

/* "expand 32-byte k" in little-endian */
static const uint32_t CHACHA20_CONSTANTS[4] = {
    0x61707865, /* expa */
    0x3320646e, /* nd 3 */
    0x79622d32, /* 2-by */
    0x6b206574, /* te k */
};

/* ── Quarter Round ───────────────────────────────────────────────── */

#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

#define QR(a, b, c, d)                                                         \
  do {                                                                         \
    a += b;                                                                    \
    d ^= a;                                                                    \
    d = ROTL32(d, 16);                                                         \
    c += d;                                                                    \
    b ^= c;                                                                    \
    b = ROTL32(b, 12);                                                         \
    a += b;                                                                    \
    d ^= a;                                                                    \
    d = ROTL32(d, 8);                                                          \
    c += d;                                                                    \
    b ^= c;                                                                    \
    b = ROTL32(b, 7);                                                          \
  } while (0)

/* ── Read/Write Little-Endian ────────────────────────────────────── */

static inline uint32_t le32_read(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static inline void le32_write(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v);
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/* ── ChaCha20 Block Function ────────────────────────────────────── */

static void chacha20_block(const uint32_t state[16], uint8_t out[64]) {
  uint32_t x[16];
  memcpy(x, state, 64);

  /* 20 rounds = 10 double-rounds */
  for (int i = 0; i < 10; i++) {
    /* Column rounds */
    QR(x[0], x[4], x[8], x[12]);
    QR(x[1], x[5], x[9], x[13]);
    QR(x[2], x[6], x[10], x[14]);
    QR(x[3], x[7], x[11], x[15]);
    /* Diagonal rounds */
    QR(x[0], x[5], x[10], x[15]);
    QR(x[1], x[6], x[11], x[12]);
    QR(x[2], x[7], x[8], x[13]);
    QR(x[3], x[4], x[9], x[14]);
  }

  /* Add original state */
  for (int i = 0; i < 16; i++) {
    le32_write(out + i * 4, x[i] + state[i]);
  }
}

/* ── ChaCha20 Encrypt/Decrypt ────────────────────────────────────── */

static void chacha20_xor(const uint8_t key[32], const uint8_t nonce[12],
                         uint32_t counter, uint8_t *data, size_t len) {
  /* Build state matrix:
   * [const0] [const1] [const2] [const3]
   * [key0  ] [key1  ] [key2  ] [key3  ]
   * [key4  ] [key5  ] [key6  ] [key7  ]
   * [count ] [nonce0] [nonce1] [nonce2]
   */
  uint32_t state[16];
  state[0] = CHACHA20_CONSTANTS[0];
  state[1] = CHACHA20_CONSTANTS[1];
  state[2] = CHACHA20_CONSTANTS[2];
  state[3] = CHACHA20_CONSTANTS[3];

  for (int i = 0; i < 8; i++) {
    state[4 + i] = le32_read(key + i * 4);
  }

  state[12] = counter;
  state[13] = le32_read(nonce);
  state[14] = le32_read(nonce + 4);
  state[15] = le32_read(nonce + 8);

  size_t offset = 0;
  while (offset < len) {
    uint8_t keystream[64];
    chacha20_block(state, keystream);

    size_t chunk = len - offset;
    if (chunk > 64)
      chunk = 64;

    for (size_t i = 0; i < chunk; i++) {
      data[offset + i] ^= keystream[i];
    }

    offset += chunk;
    state[12]++; /* Increment block counter */
  }
}

/* ── Public API ──────────────────────────────────────────────────── */

void nps_crypto_init(nps_crypto_ctx_t *ctx, const uint8_t *key) {
  memset(ctx, 0, sizeof(*ctx));
  if (key) {
    memcpy(ctx->key, key, NPS_CHACHA20_KEY_SIZE);
    ctx->enabled = true;
  }
}

int nps_crypto_encrypt(const nps_crypto_ctx_t *ctx, uint8_t *data, size_t len,
                       uint16_t conn_id, uint32_t seq_num) {
  if (!ctx->enabled || len == 0)
    return -1;

  uint8_t nonce[NPS_CHACHA20_NONCE_SIZE];
  nps_crypto_derive_nonce(conn_id, seq_num, nonce);
  chacha20_xor(ctx->key, nonce, 1, data, len);
  return 0;
}

int nps_crypto_decrypt(const nps_crypto_ctx_t *ctx, uint8_t *data, size_t len,
                       uint16_t conn_id, uint32_t seq_num) {
  /* ChaCha20 is symmetric: decrypt == encrypt (XOR with same keystream) */
  return nps_crypto_encrypt(ctx, data, len, conn_id, seq_num);
}

void nps_crypto_derive_nonce(uint16_t conn_id, uint32_t seq_num,
                             uint8_t nonce_out[NPS_CHACHA20_NONCE_SIZE]) {
  memset(nonce_out, 0, NPS_CHACHA20_NONCE_SIZE);

  /* Bytes 0-1: conn_id (little-endian) */
  nonce_out[0] = (uint8_t)(conn_id);
  nonce_out[1] = (uint8_t)(conn_id >> 8);

  /* Bytes 2-3: padding (zero) */

  /* Bytes 4-7: seq_num (little-endian) */
  le32_write(nonce_out + 4, seq_num);

  /* Bytes 8-11: seq_num XOR conn_id (for extra entropy) */
  le32_write(nonce_out + 8, seq_num ^ (uint32_t)conn_id);
}

int nps_crypto_generate_key(uint8_t key_out[NPS_CHACHA20_KEY_SIZE]) {
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0)
    return -1;

  ssize_t n = read(fd, key_out, NPS_CHACHA20_KEY_SIZE);
  close(fd);

  return (n == NPS_CHACHA20_KEY_SIZE) ? 0 : -1;
}

int nps_crypto_parse_hex_key(const char *hex,
                             uint8_t key[NPS_CHACHA20_KEY_SIZE]) {
  size_t hex_len = strlen(hex);
  if (hex_len != NPS_CHACHA20_KEY_SIZE * 2)
    return -1;

  for (int i = 0; i < NPS_CHACHA20_KEY_SIZE; i++) {
    unsigned int byte;
    if (sscanf(hex + i * 2, "%2x", &byte) != 1)
      return -1;
    key[i] = (uint8_t)byte;
  }

  return 0;
}
