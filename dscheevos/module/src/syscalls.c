/*
 * Minimal C library support for the module: stubs for the system calls
 * newlib may reference (the heap is heap.c). The module has no files,
 * no console and never exits.
 */
#include "dscheevos_module.h"

#include <errno.h>
#include <reent.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>

extern uint32_t dscm_unix_time(void);

/* _sbrk is in heap.c */

int _gettimeofday_r(struct _reent* r, struct timeval* tv, void* tz)
{
  (void)r; (void)tz;
  if (tv) { tv->tv_sec = (time_t)dscm_unix_time(); tv->tv_usec = 0; }
  return 0;
}
int _gettimeofday(struct timeval* tv, void* tz) { return _gettimeofday_r(_REENT, tv, tz); }
clock_t _times_r(struct _reent* r, struct tms* t) { (void)r; if (t) { t->tms_utime = t->tms_stime = t->tms_cutime = t->tms_cstime = 0; } return 0; }
clock_t _times(struct tms* t) { return _times_r(_REENT, t); }

int _write_r(struct _reent* r, int fd, const void* buf, size_t n) { (void)r; (void)fd; (void)buf; return (int)n; }
int _read_r(struct _reent* r, int fd, void* buf, size_t n) { (void)fd; (void)buf; (void)n; r->_errno = EBADF; return -1; }
int _close_r(struct _reent* r, int fd) { (void)fd; r->_errno = EBADF; return -1; }
_off_t _lseek_r(struct _reent* r, int fd, _off_t o, int w) { (void)fd; (void)o; (void)w; r->_errno = EBADF; return -1; }
int _fstat_r(struct _reent* r, int fd, struct stat* st) { (void)r; (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty_r(struct _reent* r, int fd) { (void)r; (void)fd; return 1; }
int _open_r(struct _reent* r, const char* p, int f, int m) { (void)p; (void)f; (void)m; r->_errno = ENOENT; return -1; }
int _kill_r(struct _reent* r, int pid, int sig) { (void)pid; (void)sig; r->_errno = EINVAL; return -1; }
int _getpid_r(struct _reent* r) { (void)r; return 1; }
int _write(int fd, const void* buf, size_t n) { return _write_r(_REENT, fd, buf, n); }
int _read(int fd, void* buf, size_t n) { return _read_r(_REENT, fd, buf, n); }
int _close(int fd) { return _close_r(_REENT, fd); }
_off_t _lseek(int fd, _off_t o, int w) { return _lseek_r(_REENT, fd, o, w); }
int _fstat(int fd, struct stat* st) { return _fstat_r(_REENT, fd, st); }
int _isatty(int fd) { return _isatty_r(_REENT, fd); }
int _kill(int pid, int sig) { return _kill_r(_REENT, pid, sig); }
int _getpid(void) { return 1; }
void _exit(int code) { (void)code; for (;;) {} }
