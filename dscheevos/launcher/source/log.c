#include "log.h"
#include "config.h"
#include "dscheevos_module.h"

#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>

#define LOG_PATH DSC_DIR "launcher.log"

/* Hardware test kit: keeps the last run's launcher.log and the last game's
 * pending.bin (journal + status log in its tail) as hwlog/NNN.log and
 * hwlog/NNN.bin before this run overwrites them. At launch only, never in a game. */
#define HWLOG_DIR DSC_DIR "hwlog"
/* nds-bootstrap copies this many status bytes (DSCHEEVOS_STATUS_LOG_SIZE) */
_Static_assert(sizeof(dscm_status_t) == 668, "DSCHEEVOS_STATUS_LOG_SIZE in nds-bootstrap");

static int copy_file(const char* from, const char* to, int skip_if_zero)
{
  static unsigned char buf[4096];
  FILE* in = fopen(from, "rb");
  FILE* out;
  size_t n, i;
  int nonzero = 0;
  if (!in)
    return 0;
  n = fread(buf, 1, sizeof(buf), in);
  for (i = 0; i < n && !nonzero; i++)
    nonzero = buf[i] != 0;
  if (n == 0 || (skip_if_zero && !nonzero)) {   /* pending.bin is one buffer long */
    fclose(in);
    return 0;
  }
  out = fopen(to, "wb");
  if (!out) {
    fclose(in);
    return 0;
  }
  do {
    fwrite(buf, 1, n, out);
  } while ((n = fread(buf, 1, sizeof(buf), in)) > 0);
  fclose(in);
  fclose(out);
  return 1;
}

static void hwlog_archive(void)
{
  char log[64], bin[64];
  struct stat st;
  unsigned i;
  mkdir(HWLOG_DIR, 0777);
  for (i = 0; i < 1000; i++) {
    snprintf(log, sizeof(log), HWLOG_DIR "/%03u.log", i);
    snprintf(bin, sizeof(bin), HWLOG_DIR "/%03u.bin", i);
    if (stat(log, &st) != 0 && stat(bin, &st) != 0)
      break;
  }
  if (i == 1000)
    return;
  copy_file(LOG_PATH, log, 0);
  copy_file(DSC_DIR "pending.bin", bin, 1);
}

void log_start(void)
{
  FILE* f;
  hwlog_archive();
  f = fopen(LOG_PATH, "w");
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
