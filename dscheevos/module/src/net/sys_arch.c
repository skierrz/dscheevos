/* lwIP NO_SYS port: time from the module's frame counter, randomness from
 * the TLS entropy pool. */
#include "lwip/sys.h"
#include "net.h"

#include "mbedtls/ctr_drbg.h"

extern uint32_t dscm_millis(void);

u32_t sys_now(void)
{
  return dscm_millis();
}

/* lwIP's ports and sequence numbers: from the entropy pool (never the
 * game's VCOUNT or timers) */
unsigned int dscm_lwip_rand(void)
{
  unsigned int r;
  entropy_bytes((uint8_t*)&r, sizeof(r));
  return r;
}
