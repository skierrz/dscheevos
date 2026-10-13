/* Glue between rcheevos' requests and the wire (rcheevos stays unmodified). */
#ifndef DSC_REQUEST_H
#define DSC_REQUEST_H

#include <stddef.h>

/* The post data to send for an rcheevos request. Unchanged (the same
 * pointer) unless skip_unofficial is set and it is an achievementsets
 * request: then "&f=3" is appended in buf, so the server sends only the
 * promoted (core) achievements, the only ones rc_client loads anyway. */
const char* request_post(const char* post, int skip_unofficial, char* buf, size_t size);

#endif
