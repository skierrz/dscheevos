/* TLS for https:// requests (mbedTLS). */
#ifndef DSC_TLS_H
#define DSC_TLS_H

#include <stddef.h>

typedef struct tls_t tls_t;

/* Mixes the time of a network event into the entropy pool. */
void tls_stir(void);

tls_t* tls_open(int fd, const char* host, const char** error);
int tls_write(tls_t* t, const char* data, size_t len);
int tls_read(tls_t* t, char* buf, size_t len);
void tls_close(tls_t* t);

#endif
