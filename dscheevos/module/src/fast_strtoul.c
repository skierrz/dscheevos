/*
 * strtoul for the module: newlib's algorithm and results exactly
 * (libc/stdlib/strtoul.c), without its two software divisions per call.
 * newlib computes cutoff = ULONG_MAX / base and cutlim = ULONG_MAX % base on
 * every call; the ARM946E-S has no divide instruction, so __udivsi3 was
 * ~20% of loading a set (rcheevos parses every number with strtoul). Bases
 * 10 and 16 (all rcheevos uses) take constants; any other base goes to
 * newlib's own strtoul. tests/module checks it against newlib's on the DS.
 */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

unsigned long __real_strtoul(const char* nptr, char** endptr, int base);

unsigned long dsc_strtoul(const char* nptr, char** endptr, int base)
{
  const unsigned char* s = (const unsigned char*)nptr;
  unsigned long acc, cutoff;
  int c, neg = 0, any, cutlim;

  if (base != 10 && base != 16)
    return __real_strtoul(nptr, endptr, base);

  /* newlib: skip isspace() (C locale), then an optional sign */
  do {
    c = *s++;
  } while (c == ' ' || (c >= '\t' && c <= '\r'));
  if (c == '-') {
    neg = 1;
    c = *s++;
  }
  else if (c == '+')
    c = *s++;
  if (base == 16 && c == '0' && (*s == 'x' || *s == 'X')) {
    c = s[1];
    s += 2;
  }
  if (base == 10) {
    cutoff = ULONG_MAX / 10;    /* constants: no division at run time */
    cutlim = (int)(ULONG_MAX % 10);
  }
  else {
    cutoff = ULONG_MAX / 16;
    cutlim = (int)(ULONG_MAX % 16);
  }
  for (acc = 0, any = 0;; c = *s++) {
    if (c >= '0' && c <= '9')
      c -= '0';
    else if (c >= 'A' && c <= 'Z')
      c -= 'A' - 10;
    else if (c >= 'a' && c <= 'z')
      c -= 'a' - 10;
    else
      break;
    if (c >= base)
      break;
    if (any < 0 || acc > cutoff || (acc == cutoff && c > cutlim))
      any = -1;
    else {
      any = 1;
      acc = acc * (unsigned long)base + (unsigned long)c;
    }
  }
  if (any < 0) {
    acc = ULONG_MAX;
    errno = ERANGE;
  }
  else if (neg)
    acc = -acc;
  if (endptr)
    *endptr = (char*)(any ? (const char*)s - 1 : nptr);
  return acc;
}

#ifndef DSC_FAST_STRTOUL_TEST
unsigned long __wrap_strtoul(const char* nptr, char** endptr, int base)
{
  return dsc_strtoul(nptr, endptr, base);
}
#endif
