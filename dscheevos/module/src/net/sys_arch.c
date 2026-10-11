/* lwIP NO_SYS port: time from the module's frame counter, randomness from
 * the TLS entropy pool's timing samples. */
#include "lwip/sys.h"
#include "net.h"

#include "mbedtls/ctr_drbg.h"

extern uint32_t dscm_millis(void);

u32_t sys_now(void)
{
  return dscm_millis();
}

unsigned int dscm_lwip_rand(void)
{
  unsigned int r = *(volatile uint16_t*)0x04000006 ^ ((unsigned int)*(volatile uint16_t*)0x04000100 << 16);
  r ^= dscm_millis() * 2654435761u;
  link_stir();
  return r;
}
