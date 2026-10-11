/*
 * Entropy for TLS. The DS has no random number generator; packet arrival
 * and other events land at times the ARM9 can't predict, so their timing
 * (VCOUNT, the hardware timers) is stirred into a SHA-256 pool, together
 * with the console's MAC and the time at boot.
 */
#include "net.h"

#include "mbedtls/sha256.h"

#include <string.h>

int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len, size_t* olen);

static mbedtls_sha256_context g_pool;
static int g_ready;

void entropy_stir(const void* data, uint32_t len)
{
  if (!g_ready) {
    mbedtls_sha256_init(&g_pool);
    mbedtls_sha256_starts(&g_pool, 0);
    g_ready = 1;
  }
  mbedtls_sha256_update(&g_pool, (const unsigned char*)data, len);
}

void entropy_bytes(uint8_t* out, uint32_t len)
{
  static uint32_t counter;
  size_t n;
  counter++;
  entropy_stir(&counter, sizeof(counter));
  mbedtls_hardware_poll(NULL, out, len, &n);
}

int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len, size_t* olen)
{
  mbedtls_sha256_context copy;
  unsigned char digest[32];
  (void)data;
  link_stir();
  mbedtls_sha256_clone(&copy, &g_pool);
  mbedtls_sha256_finish(&copy, digest);
  mbedtls_sha256_free(&copy);
  link_stir();
  if (len > sizeof(digest)) len = sizeof(digest);
  memcpy(output, digest, len);
  *olen = len;
  return 0;
}
