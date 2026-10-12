#ifndef DSC_INFLATE_H
#define DSC_INFLATE_H

#include <stddef.h>
#include <stdint.h>

#define GUNZIP_MAX (8u * 1024u * 1024u)   /* largest decoded reply */

/* Decodes a whole gzip member. Returns a malloc'd, NUL-terminated buffer
 * and its length (without the NUL), or NULL if the data is bad. */
char* gunzip(const uint8_t* in, size_t in_len, size_t* out_len);

#endif
