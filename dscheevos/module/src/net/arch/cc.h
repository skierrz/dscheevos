/* lwIP port: ARM9, little endian, newlib. */
#ifndef DSCM_LWIP_CC_H
#define DSCM_LWIP_CC_H

#include <stdint.h>
#include <stdlib.h>

#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_PLATFORM_DIAG(x)  do {} while (0)
#define LWIP_PLATFORM_ASSERT(x) do {} while (0)
#define LWIP_NO_STDINT_H 0
#define LWIP_TIMEVAL_PRIVATE 0

#endif
