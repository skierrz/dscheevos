/* The console clock keeps local time with no time zone. These measure its
 * offset from UTC against a server's HTTP Date header. Plain C: host-tested
 * in ../tests. */
#ifndef DSC_CLOCK_H
#define DSC_CLOCK_H

#include <stdint.h>

/* An RFC 7231 date ("Sun, 06 Nov 1994 08:49:37 GMT", any case) as Unix
 * time. Returns 0 if it isn't one. */
int clock_parse_http_date(const char* s, uint32_t* utc);

/* console - utc, rounded to the nearest quarter hour, in quarter hours.
 * Returns 0 if it is more than 14 hours either way (no time zone is): the
 * clock is simply wrong, and its offset unknown. */
int clock_offset_q(uint32_t console, uint32_t utc, int8_t* offset_q);

#endif
