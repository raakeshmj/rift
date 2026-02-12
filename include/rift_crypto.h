/* rift_crypto.h — ChaCha20 encryption (RFC 8439, 256-bit key, 96-bit nonce) */

#ifndef RIFT_CRYPTO_H
#define RIFT_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── Key / Nonce Sizes ───────────────────────────────────────────── */
#define RIFT_CHACHA20_KEY_SIZE 32   /* 256 bits */
#define RIFT_CHACHA20_NONCE_SIZE 12 /* 96 bits  */
#define RIFT_CHACHA20_BLOCK_SIZE 64 /* 512 bits */

/* ── Crypto Context ──────────────────────────────────────────────── */
typedef struct {
  uint8_t key[RIFT_CHACHA20_KEY_SIZE];
  bool enabled;
} rift_crypto_ctx_t;

/* ── API ─────────────────────────────────────────────────────────── */

/**
 * Initialize the crypto context with a pre-shared key.
 * @param ctx   Crypto context to initialize.
 * @param key   256-bit key (32 bytes). Pass NULL to disable encryption.
 */
void rift_crypto_init(rift_crypto_ctx_t *ctx, const uint8_t *key);

/**
 * Encrypt data in-place using ChaCha20.
 * Nonce is derived from conn_id and seq_num for per-packet uniqueness.
 *
 * @param ctx       Initialized crypto context.
 * @param data      Data to encrypt (modified in-place).
 * @param len       Length of data.
 * @param conn_id   Connection identifier (for nonce derivation).
 * @param seq_num   Sequence number (for nonce derivation).
 * @return 0 on success, -1 if crypto disabled or error.
 */
int rift_crypto_encrypt(const rift_crypto_ctx_t *ctx, uint8_t *data, size_t len,
                       uint16_t conn_id, uint32_t seq_num);

/**
 * Decrypt data in-place using ChaCha20.
 * ChaCha20 is symmetric — encryption and decryption are the same XOR operation.
 *
 * @param ctx       Initialized crypto context.
 * @param data      Data to decrypt (modified in-place).
 * @param len       Length of data.
 * @param conn_id   Connection identifier (for nonce derivation).
 * @param seq_num   Sequence number (for nonce derivation).
 * @return 0 on success, -1 if crypto disabled or error.
 */
int rift_crypto_decrypt(const rift_crypto_ctx_t *ctx, uint8_t *data, size_t len,
                       uint16_t conn_id, uint32_t seq_num);

/**
 * Generate a random 256-bit key.
 * Uses /dev/urandom on POSIX systems.
 *
 * @param key_out  Buffer to write 32 bytes of key material.
 * @return 0 on success, -1 on error.
 */
int rift_crypto_generate_key(uint8_t key_out[RIFT_CHACHA20_KEY_SIZE]);

/**
 * Derive a nonce from conn_id and seq_num.
 * Layout: [conn_id (2 bytes) | padding (2 bytes) | seq_num (4 bytes) |
 *          seq_num XOR conn_id (4 bytes)]
 *
 * @param conn_id    Connection identifier.
 * @param seq_num    Sequence number.
 * @param nonce_out  Buffer to write 12 bytes of nonce.
 */
void rift_crypto_derive_nonce(uint16_t conn_id, uint32_t seq_num,
                             uint8_t nonce_out[RIFT_CHACHA20_NONCE_SIZE]);

/**
 * Parse a hex string into a key buffer.
 * @param hex  64-character hex string.
 * @param key  Output buffer (32 bytes).
 * @return 0 on success, -1 on invalid input.
 */
int rift_crypto_parse_hex_key(const char *hex,
                             uint8_t key[RIFT_CHACHA20_KEY_SIZE]);

#endif /* RIFT_CRYPTO_H */
