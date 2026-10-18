/* Glue between rcheevos' requests and the wire (rcheevos stays unmodified). */
#ifndef DSC_REQUEST_H
#define DSC_REQUEST_H

#include <stddef.h>

/* The post data to send for an rcheevos request. Unchanged (the same
 * pointer) except for an achievementsets request: then "&f=3" is appended in
 * buf, so the server sends only official (promoted) achievements, in the core
 * set and in official subsets alike. DScheevos supports official achievements
 * only, and rc_client never loads unofficial ones anyway: they would only
 * make the download and the session file bigger (Pokemon Platinum: 311 KB
 * with them, 197 KB without; the session file holds 256 KB). */
const char* request_post(const char* post, char* buf, size_t size);

#endif
