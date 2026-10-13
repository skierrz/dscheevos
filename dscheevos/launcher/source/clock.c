#include "clock.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int month_of(const char* s)
{
  static const char names[] = "janfebmaraprmayjunjulaugsepoctnovdec";
  int i;
  for (i = 0; i < 12; i++)
    if (tolower((unsigned char)s[0]) == names[i * 3] && tolower((unsigned char)s[1]) == names[i * 3 + 1]
     && tolower((unsigned char)s[2]) == names[i * 3 + 2])
      return i;
  return -1;
}

/* days from 1970-01-01 to y-m-d (proleptic Gregorian, m 0-11) */
static int32_t days_from_civil(int32_t y, int m, int d)
{
  int32_t era, yoe, doy, doe;
  m += 1;
  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = y - era * 400;
  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

int clock_parse_http_date(const char* s, uint32_t* utc)
{
  char* end;
  long day, year, h, mi, sec;
  int mon;
  const char* comma = strchr(s, ',');
  if (!comma)
    return 0;
  s = comma + 1;
  day = strtol(s, &end, 10);
  if (end == s || *end != ' ')
    return 0;
  s = end + 1;
  if (strlen(s) < 4 || (mon = month_of(s)) < 0 || s[3] != ' ')
    return 0;
  s += 4;
  year = strtol(s, &end, 10);
  if (end == s || *end != ' ')
    return 0;
  s = end + 1;
  h = strtol(s, &end, 10);
  if (*end != ':') return 0;
  mi = strtol(end + 1, &end, 10);
  if (*end != ':') return 0;
  sec = strtol(end + 1, &end, 10);
  if (year < 1970 || year > 2105 || day < 1 || day > 31 || h > 23 || mi > 59 || sec > 60)
    return 0;
  *utc = (uint32_t)days_from_civil((int32_t)year, mon, (int)day) * 86400u + (uint32_t)(h * 3600 + mi * 60 + sec);
  return 1;
}

int clock_offset_q(uint32_t console, uint32_t utc, int8_t* offset_q)
{
  int32_t diff = (int32_t)(console - utc);
  int32_t q = (diff >= 0 ? diff + 450 : diff - 450) / 900;
  if (q > 14 * 4 || q < -14 * 4)
    return 0;
  *offset_q = (int8_t)q;
  return 1;
}
