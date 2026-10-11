/* sd:/_nds/dscheevos/launcher.log: what the last run did (server requests,
 * HTTP status, errors shown), for tests and bug reports. Never holds the
 * username, password or token. */
#ifndef DSC_LOG_H
#define DSC_LOG_H

void log_start(void);                  /* truncates the file */
void log_line(const char* fmt, ...);

#endif
