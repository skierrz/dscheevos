/* The console clock keeps local time; the launcher measures its offset from
 * UTC with the server's Date header, and the module and the journal convert
 * with it (dscm_console_to_utc). */
#include "clock.h"
#include "dscheevos_module.h"

#include <stdio.h>

static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
  uint32_t t = 0;
  int8_t q = 0;

  /* RFC 7231's example, and the lower-case form net.c sees */
  CHECK(clock_parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT", &t) && t == 784111777u);
  CHECK(clock_parse_http_date(" mon, 12 oct 2026 21:43:12 gmt", &t) && t == 1791841392u);
  CHECK(clock_parse_http_date("Thu, 29 Feb 2024 00:00:00 GMT", &t) && t == 1709164800u);
  CHECK(!clock_parse_http_date("12 Oct 2026 21:43:12", &t));
  CHECK(!clock_parse_http_date("Mon, 12 Foo 2026 21:43:12 GMT", &t));
  CHECK(!clock_parse_http_date("Mon, 12 Oct 2026 25:43:12 GMT", &t));

  /* EDT console (UTC-4), a few seconds of request time either way */
  CHECK(clock_offset_q(1791841392u - 4 * 3600 + 3, 1791841392u, &q) && q == -16);
  CHECK(clock_offset_q(1791841392u - 4 * 3600 - 20, 1791841392u, &q) && q == -16);
  /* India (UTC+5:30), Nepal (UTC+5:45), Kiribati (UTC+14), Baker Island (UTC-12), UTC */
  CHECK(clock_offset_q(1791841392u + 5 * 3600 + 1800, 1791841392u, &q) && q == 22);
  CHECK(clock_offset_q(1791841392u + 5 * 3600 + 2700 - 5, 1791841392u, &q) && q == 23);
  CHECK(clock_offset_q(1791841392u + 14 * 3600, 1791841392u, &q) && q == 56);
  CHECK(clock_offset_q(1791841392u - 12 * 3600, 1791841392u, &q) && q == -48);
  CHECK(clock_offset_q(1791841392u, 1791841392u, &q) && q == 0);
  /* a clock that's simply wrong (a day off): offset unknown */
  CHECK(!clock_offset_q(1791841392u + 86400, 1791841392u, &q));
  CHECK(!clock_offset_q(1791841392u - 15 * 3600, 1791841392u, &q));

  /* back to UTC */
  CHECK(dscm_console_to_utc(1791841392u - 4 * 3600, -16) == 1791841392u);
  CHECK(dscm_console_to_utc(1791841392u + 5 * 3600 + 1800, 22) == 1791841392u);
  CHECK(dscm_console_to_utc(1791841392u, 0) == 1791841392u);

  printf("%d checks, %d failed\n", checks, failed);
  return failed != 0;
}
