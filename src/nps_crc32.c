/* nps_crc32.c — table-driven CRC32 (0xEDB88320, zlib-compatible) */

#include "nps_crc32.h"
#include <pthread.h>

static uint32_t crc32_table[256];
static pthread_once_t crc32_once = PTHREAD_ONCE_INIT;

static void crc32_generate_table(void) {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t crc = i;
    for (int j = 0; j < 8; j++) {
      if (crc & 1)
        crc = (crc >> 1) ^ 0xEDB88320;
      else
        crc >>= 1;
    }
    crc32_table[i] = crc;
  }
}

void nps_crc32_init(void) { pthread_once(&crc32_once, crc32_generate_table); }

uint32_t nps_crc32_update(uint32_t crc, const uint8_t *data, size_t length) {
  for (size_t i = 0; i < length; i++) {
    crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc;
}

uint32_t nps_crc32(const uint8_t *data, size_t length) {
  nps_crc32_init(); /* Ensure table is initialized */
  uint32_t crc = NPS_CRC32_INIT;
  crc = nps_crc32_update(crc, data, length);
  return nps_crc32_finalize(crc);
}
