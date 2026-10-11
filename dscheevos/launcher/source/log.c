#include "log.h"
#include "config.h"

#include <stdarg.h>
#include <stdio.h>

#define LOG_PATH DSC_DIR "launcher.log"

void log_start(void)
{
  FILE* f = fopen(LOG_PATH, "w");
  if (f) {
    fprintf(f, "DScheevos launcher " DSC_VERSION "\n");
    fclose(f);
  }
}

void log_line(const char* fmt, ...)
{
  va_list ap;
  FILE* f = fopen(LOG_PATH, "a");
  if (!f)
    return;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fclose(f);
}
