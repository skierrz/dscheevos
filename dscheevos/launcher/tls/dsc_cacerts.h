/* The built-in root CAs (cacerts.c) and the extra CA in the session file:
 * DER certificates one after the other. Shared by the launcher and the
 * module. */
#ifndef DSC_CACERTS_H
#define DSC_CACERTS_H

#include "mbedtls/x509_crt.h"
#include <stddef.h>

extern const unsigned char dsc_cacerts[];
extern const size_t dsc_cacerts_size;

/* Adds each certificate of p[0..n) to chain. nocopy: chain points into p,
 * which must outlive it (the built-in ones: read-only data). 0 or -1. */
static inline int dsc_parse_der_list(mbedtls_x509_crt* chain, const unsigned char* p, size_t n, int nocopy)
{
  while (n) {
    size_t len;
    if (n < 4 || p[0] != 0x30 || p[1] != 0x82)
      return -1;
    len = 4 + ((size_t)p[2] << 8 | p[3]);
    if (len > n)
      return -1;
    if ((nocopy ? mbedtls_x509_crt_parse_der_nocopy(chain, p, len) : mbedtls_x509_crt_parse_der(chain, p, len)) != 0)
      return -1;
    p += len;
    n -= len;
  }
  return 0;
}

#endif
