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

/* The TLS session of the last handshake with host (mbedtls_ssl_session_save
 * format) into buf: its length, 0 if none. Later connections to the same
 * host resume it; the module gets it in the session file for its first. */
size_t tls_session_save(const char* host, unsigned char* buf, size_t size);

#endif
