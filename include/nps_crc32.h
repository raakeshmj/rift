/* nps_crc32.h — CRC32 checksum (polynomial 0xEDB88320, zlib-compatible) */

#ifndef NPS_CRC32_H
#define NPS_CRC32_H

#include <stddef.h>
#include <stdint.h>

/*
 * Initialize the CRC32 lookup table.
 * Must be called once before using nps_crc32().
 * Thread-safe: uses pthread_once internally.
 */
void nps_crc32_init(void);

/*
 * Compute CRC32 checksum over a buffer.
 * Returns the 32-bit CRC value.
 */
uint32_t nps_crc32(const uint8_t *data, size_t length);

/*
 * Incremental CRC32: update an existing CRC with more data.
 * Pass NPS_CRC32_INIT as `crc` for the first call.
 */
#define NPS_CRC32_INIT 0xFFFFFFFF
uint32_t nps_crc32_update(uint32_t crc, const uint8_t *data, size_t length);

/*
 * Finalize an incremental CRC32 computation.
 */
static inline uint32_t nps_crc32_finalize(uint32_t crc) {
  return crc ^ 0xFFFFFFFF;
}

#endif /* NPS_CRC32_H */
