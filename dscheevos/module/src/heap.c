/*
 * The module heap: _sbrk over the two heap pieces, and the load-time
 * placement in the arena's place (see heap.h). The wraps cost one or two
 * compares per malloc and free; nothing allocates per frame.
 */
#include "heap.h"
#include "rc_api_runtime.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

dscm_heap_t dscm_g_heap;

#ifdef DSCM_HEAP_HOST_TEST
/* the host test's session area, HEAP_ZONE_END - DSCM_SESSION bytes (or more) */
extern char dscm_host_session[];
#define SESSION   dscm_host_session
#else
#define SESSION   ((char*)DSCM_DATA_ADDR(DSCM_SESSION))
#endif
#define ARENA     (SESSION + HEAP_SESSION_HEAD)
#define ZONE_END  (ARENA + HEAP_ARENA_BYTES)
#define ADDR(p)   ((uint32_t)(uintptr_t)(p))
#define IS_ARENA(p) (((ADDR(p) ^ ADDR(ARENA)) & 0x00FFFFFFu) == 0)   /* in either view */

/* ---- sbrk: the low piece, then the high piece for good ---- */
#ifndef DSCM_HEAP_HOST_TEST   /* the host test (tests/heap) runs on the host's malloc */
#include <malloc.h>
#include <reent.h>
extern char __heap_start[];
/* newlib's heap (rcheevos' game data, the core's tables) in the view
 * heap_view() picked: the same RAM through the cache or the mirror */
static uint32_t g_heap_hi = DSCM_DATA_HI;
#define HV(a)     ((char*)((((uint32_t)(a)) & 0x00FFFFFFu) | g_heap_hi))
#define LOW_BASE  HV((((uint32_t)__heap_start + 7u) & ~7u))
#define LOW_END   HV(DSCM_SESSION)
#define HIGH_BASE HV(HEAP_ZONE_END)
#define HIGH_END  HV(DSCM_HEAP_END)


static char* g_brk;
static int g_high;
static uint32_t g_gap;          /* newlib counts the gap as allocated */

void heap_view(int uncached)
{
  if (!g_brk)   /* before the first allocation only */
    g_heap_hi = uncached ? DSCM_DATA_HI : 0x02000000u;
}

void* _sbrk_r(struct _reent* r, ptrdiff_t incr)
{
  char* lo = g_high ? HIGH_BASE : LOW_BASE;
  char* hi = g_high ? HIGH_END : LOW_END;
  char* prev;
  if (!g_brk) {
    g_brk = LOW_BASE;
    dscm_g_heap.low_base = dscm_g_heap.low_top = (uint32_t)g_brk;
  }
  if (incr > hi - g_brk || incr < lo - g_brk) {
    if (g_high || incr <= 0 || incr > HIGH_END - HIGH_BASE) {
      r->_errno = ENOMEM;
      return (void*)-1;
    }
    /* Doesn't fit below the session: go on above the arena. newlib's malloc
     * sees a gap ("foreign sbrk"), fences off the old top and frees it. */
    g_high = 1;
    g_gap = (uint32_t)(HIGH_BASE - g_brk);
    g_brk = HIGH_BASE;
    dscm_g_heap.high_base = (uint32_t)g_brk;
  }
  prev = g_brk;
  g_brk += incr;
  if (g_high) {
    if ((uint32_t)g_brk > dscm_g_heap.high_top)
      dscm_g_heap.high_top = (uint32_t)g_brk;
  }
  else if ((uint32_t)g_brk > dscm_g_heap.low_top) {
    dscm_g_heap.low_top = (uint32_t)g_brk;
  }
  return prev;
}
void* _sbrk(ptrdiff_t incr) { return _sbrk_r(_REENT, incr); }
#define ARENA_VIEW HV(ARENA)
#else
#define ARENA_VIEW ARENA
#endif

/* ---- load-time placement ---- */

void* __real_malloc(size_t n);
void __real_free(void* p);
void* __real_calloc(size_t n, size_t size);
void* __real_realloc(void* p, size_t n);

#define LEND_MAX 4
static struct {
  int loading;                 /* between heap_load_begin and heap_load_end */
  int arena_open;              /* in dsc_init: the next HEAP_ARENA_BYTES malloc is the arena */
  int parsing;                 /* in rc_api's parse of the set */
  const char* lend;            /* the reply heap_lend offers */
  uint32_t lend_len;
  struct { char* p; uint32_t size; } lent[LEND_MAX];   /* lent and not freed yet */
  char* bump;                  /* the parsed set, after the session file */
  int live;                    /* bump blocks not freed yet */
} Z;

/* the session header, the replies and the arena: never newlib's */
static int in_zone(const void* p)
{
  return (const char*)p >= SESSION && (const char*)p < ZONE_END;
}

void heap_load_begin(uint32_t file_size)
{
  memset(&Z, 0, sizeof(Z));
  Z.loading = 1;
  Z.bump = SESSION + ((file_size + 7u) & ~7u);
  dscm_g_heap.parse_base = dscm_g_heap.parse_top = ADDR(Z.bump);
}

void heap_arena_window(int open)
{
  Z.arena_open = open && Z.loading;
}

void heap_lend(const char* body, uint32_t len)
{
  /* only what free() will know as not newlib's */
  Z.lend = Z.loading && in_zone(body) && len < (uint32_t)(ZONE_END - body) ? body : NULL;
  Z.lend_len = len;
}

/* A bump block: its size, then the data (8-aligned). */
static void* bump_alloc(size_t n)
{
  char* p = Z.bump;
  size_t avail = p < ZONE_END ? (size_t)(ZONE_END - p) : 0;
  if (n > avail || 8 + ((n + 7) & ~(size_t)7) > avail) {
    dscm_g_heap.parse_heap += n;
    return NULL;
  }
  *(uint32_t*)p = (uint32_t)n;
  Z.bump = p + 8 + ((n + 7) & ~(size_t)7);
  if (ADDR(Z.bump) > dscm_g_heap.parse_top)
    dscm_g_heap.parse_top = ADDR(Z.bump);
  Z.live++;
  return p + 8;
}

static int lent_slot(const void* p)
{
  int i;
  for (i = 0; i < LEND_MAX; i++)
    if (Z.lent[i].p == p)
      return i;
  return -1;
}

void* __wrap_malloc(size_t n)
{
  if (Z.loading) {
    if (Z.arena_open && n == HEAP_ARENA_BYTES) {
      Z.arena_open = 0;
      /* the snapshot arena: in the heap's view (heap_view), so a set whose
       * evaluation runs cached also reads its snapshots cached */
      dscm_g_heap.arena = ADDR(ARENA_VIEW);
      return ARENA_VIEW;
    }
    if (Z.lend && n == Z.lend_len + 1) {
      int i = lent_slot(NULL);
      if (i >= 0) {
        /* The caller copies the reply onto itself and writes the NUL after it. */
        char* p = (char*)Z.lend;
        Z.lent[i].p = p;
        Z.lent[i].size = (uint32_t)n;
        Z.lend = NULL;
        dscm_g_heap.lent += n - 1;
        return p;
      }
    }
    if (Z.parsing) {
      void* p = bump_alloc(n);
      if (p)
        return p;
    }
  }
  return __real_malloc(n);
}

void __wrap_free(void* p)
{
  if (IS_ARENA(p))
    return;                      /* the arena (either view), at dsc_shutdown */
  if (!in_zone(p)) {
    __real_free(p);
    return;
  }
  {
    int i = lent_slot(p);
    if (i >= 0)
      Z.lent[i].p = NULL;
    else
      Z.live--;                  /* a bump block: its bytes go with the load */
  }
}

void* __wrap_calloc(size_t count, size_t size)
{
  if (Z.loading && Z.parsing) {
    size_t n = count * size;
    void* p;
    if (size && n / size != count)
      return NULL;
    p = bump_alloc(n);
    if (p)
      return memset(p, 0, n);
  }
  return __real_calloc(count, size);
}

void* __wrap_realloc(void* p, size_t n)
{
  uint32_t old;
  void* q;
  int i;
  if (!p)
    return __wrap_malloc(n);
  if (IS_ARENA(p))
    return NULL;
  if (!in_zone(p))
    return __real_realloc(p, n);
  i = lent_slot(p);
  old = i >= 0 ? Z.lent[i].size : *((uint32_t*)p - 2);
  q = __wrap_malloc(n);
  if (q) {
    memcpy(q, p, old < n ? old : n);
    __wrap_free(p);
  }
  return q;
}

/* rc_client's only parse of the set: every allocation in it is the parsed
 * response, which rc_client frees (rc_api_destroy_fetch_game_sets_response)
 * once it has built its own game data from it. */
int __real_rc_api_process_fetch_game_sets_server_response(rc_api_fetch_game_sets_response_t* response,
    const rc_api_server_response_t* server_response);
int __wrap_rc_api_process_fetch_game_sets_server_response(rc_api_fetch_game_sets_response_t* response,
    const rc_api_server_response_t* server_response)
{
  int result;
  Z.parsing = Z.loading;
  result = __real_rc_api_process_fetch_game_sets_server_response(response, server_response);
  Z.parsing = 0;
  return result;
}

int heap_load_end(void)
{
  int i, left = Z.live;
  for (i = 0; i < LEND_MAX; i++)
    if (Z.lent[i].p)
      left++;
  Z.loading = Z.arena_open = Z.parsing = 0;
  Z.lend = NULL;
  dscm_g_heap.load_low_top = dscm_g_heap.low_top;
  dscm_g_heap.load_high_top = dscm_g_heap.high_top;
#ifndef DSCM_HEAP_HOST_TEST
  dscm_g_heap.load_in_use = (uint32_t)mallinfo().uordblks - g_gap;
#endif
  dscm_g_heap.load_left = (uint32_t)left;
  return left ? -1 : 0;
}
