/* arm7-lab payload: the heap (main RAM, from the image's end) and newlib stubs. */
#include "lab7.h"

#include <errno.h>
#include <reent.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>

extern char __heap_start[];
extern uint32_t dscm_unix_time(void);
uint32_t heap_high;

void* _sbrk_r(struct _reent* r, ptrdiff_t incr)
{
  static char* brk;
  char* old;
  if (!brk) { brk = __heap_start; heap_high = (uint32_t)brk; }
  if ((uint32_t)(brk + incr) > LAB7_HEAP_END) { r->_errno = ENOMEM; return (void*)-1; }
  old = brk;
  brk += incr;
  if ((uint32_t)brk > heap_high) heap_high = (uint32_t)brk;
  return old;
}
void* _sbrk(ptrdiff_t incr) { return _sbrk_r(_REENT, incr); }

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
void __assert_func(const char* file, int line, const char* func, const char* expr)
{
  (void)file; (void)line; (void)func; (void)expr;
  abort();
}

#ifdef LAB7_NWRAM
/* Data placement test: allocations up to NW_MAX bytes (rcheevos' and the
 * core's per-set data, the snapshot arena) are bump-allocated in DSi
 * NWRAM-B/C mapped to the ARM7 (512 KB at 0x03100000), larger ones (the
 * set's JSON, parse buffers) from the main RAM heap. Frees in NWRAM are
 * dropped (measurement only). */
extern char __bss_end[];
#define NW_BASE (((uint32_t)__bss_end + 31) & ~31u)   /* NWRAM builds: code, rodata, bss, then this heap */
#define NW_END  0x03180000u
#define NW_MAX  (32u * 1024u)
static uint32_t nw_next;
uint32_t nw_used, nw_spilled;
void* __real_malloc(size_t n);
void __real_free(void* p);
void* __real_realloc(void* p, size_t n);
static int in_nw(const void* p) { return (uint32_t)p >= 0x03100000u && (uint32_t)p < NW_END; }
void* __wrap_malloc(size_t n)
{
  if (!nw_next) nw_next = NW_BASE;
  if (n <= NW_MAX) {
    uint32_t a = (nw_next + 8 + 7) & ~7u;
    if (a + n <= NW_END) {
      ((uint32_t*)a)[-1] = n;
      nw_next = a + n;
      nw_used = nw_next - NW_BASE;
      return (void*)a;
    }
    nw_spilled += n;
  }
  return __real_malloc(n);
}
void __wrap_free(void* p) { if (!in_nw(p)) __real_free(p); }
void* __wrap_calloc(size_t c, size_t s)
{
  void* p = __wrap_malloc(c * s);
  if (p) { char* q = p; size_t i; for (i = 0; i < c * s; i++) q[i] = 0; }
  return p;
}
void* __wrap_realloc(void* p, size_t n)
{
  void* q;
  uint32_t old, i;
  if (!p) return __wrap_malloc(n);
  if (!in_nw(p)) {
    if (n > NW_MAX) return __real_realloc(p, n);
    q = __wrap_malloc(n);
    if (!q) return NULL;
    /* size of a newlib block: its header's size field minus overhead (upper bound is fine: n) */
    for (i = 0; i < n; i++) ((char*)q)[i] = ((char*)p)[i];
    __real_free(p);
    return q;
  }
  old = ((uint32_t*)p)[-1];
  if (n <= old) return p;
  q = __wrap_malloc(n);
  if (!q) return NULL;
  for (i = 0; i < old; i++) ((char*)q)[i] = ((char*)p)[i];
  return q;
}
void nwram_map7(void)
{
  /* ARM7 windows (ARM7's MBK7/MBK8): B at 0x03100000, C at 0x03140000, 256 KB each */
  *(volatile uint32_t*)0x04004058 = (0x20u << 3) | (3u << 12) | (0x28u << 19);
  *(volatile uint32_t*)0x0400405C = (0x28u << 3) | (3u << 12) | (0x30u << 19);
}
#endif
