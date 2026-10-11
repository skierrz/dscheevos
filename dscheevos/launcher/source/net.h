/* Wi-Fi (the console's saved access points) and a small HTTP/1.1 client. */
#ifndef DSC_NET_H
#define DSC_NET_H

#include <stddef.h>

/* Connects with the access points saved in the console's settings. Uses the
 * DSi's own Wi-Fi (WPA2) when available. Returns 0 when online. */
int net_connect(int timeout_frames);
const char* net_error(void);

typedef struct http_response_t {
  int status;           /* HTTP status, or < 0 on a network error */
  char* body;           /* NUL-terminated, malloc'd; free with http_free */
  size_t length;
} http_response_t;

void http_set_user_agent(const char* user_agent);
/* POST (or GET when post is NULL). http:// and https:// URLs. */
void http_request(const char* url, const char* post, const char* content_type, http_response_t* out);
void http_free(http_response_t* r);

#endif
