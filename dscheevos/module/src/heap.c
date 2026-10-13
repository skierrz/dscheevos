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
#define SESSION   ((char*)DSCM_SESSION)
#endif
#define ARENA     (SESSION + HEAP_SESSION_HEAD)
#define ZONE_END  (ARENA + HEAP_ARENA_BYTES)
#define ADDR(p)   ((uint32_t)(uintptr_t)(p))

/* ---- sbrk: the low piece, then the high piece for good ---- */
#ifndef DSCM_HEAP_HOST_TEST   /* the host test (tests/heap) runs on the host's malloc */
#include <malloc.h>
#include <reent.h>
extern char __bss_end[];
#define LOW_BASE  ((char*)(((uint32_t)__bss_end + 7u) & ~7u))
#define LOW_END   ((char*)DSCM_SESSION)
#define HIGH_END  ((char*)DSCM_HEAP_END)
static char* high_base(void);   /* the main-RAM zone's top, from then on fixed */

static char* g_brk;
static int g_high;
static uint32_t g_gap;          /* newlib counts the gap as allocated */

void* _sbrk_r(struct _reent* r, ptrdiff_t incr)
{
  char* lo = g_high ? (char*)dscm_g_heap.high_base : LOW_BASE;
  char* hi = g_high ? HIGH_END : LOW_END;
  char* prev;
  if (!g_brk) {
    g_brk = LOW_BASE;
    dscm_g_heap.low_base = dscm_g_heap.low_top = (uint32_t)g_brk;
  }
  if (incr > hi - g_brk || incr < lo - g_brk) {
    char* hb;
    if (g_high || incr <= 0 || incr > HIGH_END - (hb = high_base())) {
      r->_errno = ENOMEM;
      return (void*)-1;
    }
    /* Doesn't fit below the session: go on above the arena (and the zone
     * there). newlib's malloc sees a gap ("foreign sbrk"), fences off the
     * old top and frees it. */
    g_high = 1;
    g_gap = (uint32_t)(hb - g_brk);
    g_brk = hb;
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
#endif

/* ---- load-time placement ---- */

void* __real_malloc(size_t n);
void __real_free(void* p);
void* __real_calloc(size_t n, size_t size);
void* __real_realloc(void* p, size_t n);

/* A set zone: blocks of the load that stay, in allocation order, in memory
 * of their own (heap.h). First fit over the freed blocks, else the next
 * fresh bytes; a block is an 8-byte header (its size, with the header, and
 * a tag) and its data, 8-aligned. Freed blocks are kept in address order
 * and merged with free neighbours; a free block at the top gives its bytes
 * back to the top. Blocks are only made while loading; freeing works always. */
#define ZONES      2               /* NWRAM (if granted), then main RAM */
#define ZONE_USED  0x55534544u     /* "DESU": a block in use */
#define ZONE_FREE  0x45455246u     /* "FREE" */
#define ZONE_MIN   16u
typedef struct zblock_t {
  uint32_t size;                   /* bytes, header included, a multiple of 8 */
  uint32_t tag;                    /* ZONE_USED, or ZONE_FREE */
  struct zblock_t* next;           /* free blocks only: the next one up */
} zblock_t;
typedef struct zone_t {
  char* base;
  char* end;
  char* top;                       /* fresh bytes start here */
  zblock_t* free;                  /* free blocks below top, in address order */
  int fixed;                       /* top can't move up any more (newlib's heap starts there) */
} zone_t;

#define LEND_MAX 4
static struct {
  int loading;                 /* between heap_load_begin and heap_load_end */
  int arena_open;              /* in dsc_init: the next HEAP_ARENA_BYTES malloc is the arena */
  int parsing;                 /* in rc_api's parse of the set */
  int shared;                  /* heap_shared: newlib's */
  uint32_t transient;          /* heap_transient: the next malloc of this size is newlib's */
  const char* lend;            /* the reply heap_lend offers */
  uint32_t lend_len;
  struct { char* p; uint32_t size; } lent[LEND_MAX];   /* lent and not freed yet */
  char* bump;                  /* the parsed set, after the session file */
  int live;                    /* bump blocks not freed yet */
  zone_t zone[ZONES];
} Z;

#ifdef DSCM_HEAP_HOST_TEST
extern char dscm_host_main_zone[];       /* the test's high heap piece */
extern uint32_t dscm_host_main_zone_size;
#define MAIN_ZONE      dscm_host_main_zone
#define MAIN_ZONE_END  (dscm_host_main_zone + dscm_host_main_zone_size)
#else
#define MAIN_ZONE      ((char*)HEAP_ZONE_END)
#define MAIN_ZONE_END  ((char*)DSCM_HEAP_END)
#endif

/* the session header, the replies and the arena: never newlib's */
static int in_zone(const void* p)
{
  return (const char*)p >= SESSION && (const char*)p < ZONE_END;
}

static zone_t* zone_of(const void* p)
{
  int i;
  for (i = 0; i < ZONES; i++)
    if ((const char*)p >= Z.zone[i].base && (const char*)p < Z.zone[i].end)
      return &Z.zone[i];
  return NULL;
}

static void zone_stats(zone_t* z)
{
  int i = (int)(z - Z.zone);
  if (ADDR(z->top) > dscm_g_heap.zone_top[i])
    dscm_g_heap.zone_top[i] = ADDR(z->top);
}

/* Lay out the zones (heap_load_begin). The main-RAM zone takes the bottom of
 * the high heap piece, until newlib asks for that piece (high_base). */
static void zones_begin(void)
{
  zone_t* m = &Z.zone[1];
  if (dscm_g_heap.high_base) {
    m->base = m->end = m->top = NULL;    /* newlib has it already */
    return;
  }
  m->base = m->top = MAIN_ZONE;
  m->end = MAIN_ZONE_END;
  m->free = NULL;
  m->fixed = 0;
  dscm_g_heap.zone_base[1] = dscm_g_heap.zone_top[1] = ADDR(m->base);
}

#ifndef DSCM_HEAP_HOST_TEST
static char* high_base(void)
{
  zone_t* m = &Z.zone[1];
  if (!m->base)
    return MAIN_ZONE;
  m->fixed = 1;
  m->end = m->top;               /* the zone ends where newlib's piece starts */
  return m->top;
}
#endif

static void* zone_alloc(zone_t* z, size_t n)
{
  zblock_t **link, *b;
  uint32_t need;
  if (!z->base || n > (size_t)(z->end - z->base))
    return NULL;
  need = ((uint32_t)n + 8u + 7u) & ~7u;
  if (need < ZONE_MIN)
    need = ZONE_MIN;
  for (link = &z->free; (b = *link) != NULL; link = &b->next) {
    if (b->size < need)
      continue;
    if (b->size - need >= ZONE_MIN) {
      zblock_t* rest = (zblock_t*)((char*)b + need);
      rest->size = b->size - need;
      rest->tag = ZONE_FREE;
      rest->next = b->next;
      *link = rest;
      b->size = need;
    }
    else {
      *link = b->next;
    }
    b->tag = ZONE_USED;
    dscm_g_heap.zone_blocks++;
    return (char*)b + 8;
  }
  if (z->fixed || need > (uint32_t)(z->end - z->top))
    return NULL;
  b = (zblock_t*)z->top;
  b->size = need;
  b->tag = ZONE_USED;
  z->top += need;
  zone_stats(z);
  dscm_g_heap.zone_blocks++;
  return (char*)b + 8;
}

/* Give a block's bytes back: merge with free neighbours, and with the top. */
static void zone_release(zone_t* z, zblock_t* b)
{
  zblock_t **link = &z->free, **plink = NULL, *prev = NULL, *next;
  while ((next = *link) != NULL && next < b) {
    plink = link;
    prev = next;
    link = &next->next;
  }
  b->tag = ZONE_FREE;
  b->next = next;
  if (next && (char*)b + b->size == (char*)next) {     /* merge the one above */
    b->size += next->size;
    b->next = next->next;
  }
  if (prev && (char*)prev + prev->size == (char*)b) {  /* merge into the one below */
    prev->size += b->size;
    prev->next = b->next;
    b = prev;
    link = plink;
  }
  else {
    *link = b;
  }
  /* now *link == b; the block at the top goes back to fresh bytes */
  if (!z->fixed && (char*)b + b->size == z->top) {
    *link = b->next;
    z->top = (char*)b;
  }
}

static void zone_free(zone_t* z, void* p)
{
  zblock_t* b = (zblock_t*)((char*)p - 8);
  if (b->tag != ZONE_USED)
    return;                      /* not a block of ours, or freed twice: leave it */
  dscm_g_heap.zone_blocks--;
  zone_release(z, b);
}

/* While loading: the zones in order, else newlib (counted). */
static void* place(size_t n)
{
  int i;
  for (i = 0; i < ZONES; i++) {
    void* p = zone_alloc(&Z.zone[i], n);
    if (p)
      return p;
  }
  dscm_g_heap.zone_missed += n;
  return NULL;
}

void heap_zone_nwram(void* base, uint32_t size)
{
  zone_t* z = &Z.zone[0];
  uintptr_t lo = ((uintptr_t)base + 31u) & ~(uintptr_t)31u;
  uintptr_t hi = ((uintptr_t)base + size) & ~(uintptr_t)31u;
  memset(z, 0, sizeof(*z));
  dscm_g_heap.zone_base[0] = dscm_g_heap.zone_top[0] = dscm_g_heap.zone_end[0] = 0;
  if (!base || hi < lo || hi - lo < 0x1000u)
    return;
  z->base = z->top = (char*)lo;
  z->end = (char*)hi;
  dscm_g_heap.zone_base[0] = dscm_g_heap.zone_top[0] = ADDR(lo);
  dscm_g_heap.zone_end[0] = ADDR(hi);
}

void heap_load_begin(uint32_t file_size)
{
  zone_t nw = Z.zone[0];
  memset(&Z, 0, sizeof(Z));
  Z.zone[0] = nw;
  Z.loading = 1;
  Z.bump = SESSION + ((file_size + 7u) & ~7u);
  dscm_g_heap.parse_base = dscm_g_heap.parse_top = ADDR(Z.bump);
  zones_begin();
}

void heap_arena_window(int open)
{
  Z.arena_open = open && Z.loading;
}

void heap_shared(int on)
{
  Z.shared = on;
}

void heap_transient(uint32_t n)
{
  Z.transient = n;
}

void heap_lend(const char* body, uint32_t len)
{
  /* only what free() will know as not newlib's */
  Z.lend = Z.loading && in_zone(body) && len < (uint32_t)(ZONE_END - body) ? body : NULL;
  Z.lend_len = len;
  if (body && !Z.lend)
    Z.transient = len + 1;       /* copied after all: short-lived */
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
    void* p;
    if (Z.arena_open && n == HEAP_ARENA_BYTES) {
      Z.arena_open = 0;
      dscm_g_heap.arena = ADDR(ARENA);
      return ARENA;
    }
    if (Z.lend && n == Z.lend_len + 1) {
      int i = lent_slot(NULL);
      if (i >= 0) {
        /* The caller copies the reply onto itself and writes the NUL after it. */
        p = (void*)Z.lend;
        Z.lent[i].p = (char*)p;
        Z.lent[i].size = (uint32_t)n;
        Z.lend = NULL;
        dscm_g_heap.lent += n - 1;
        return p;
      }
    }
    if (Z.parsing) {
      p = bump_alloc(n);
      if (p)
        return p;
    }
    else if (Z.transient && n == Z.transient) {
      Z.transient = 0;           /* a reply's copy: freed once parsed */
    }
    else if (!Z.shared) {
      p = place(n);
      if (p)
        return p;
    }
  }
  return __real_malloc(n);
}

void __wrap_free(void* p)
{
  zone_t* z;
  if (!in_zone(p)) {
    if ((z = zone_of(p)) != NULL)
      zone_free(z, p);
    else
      __real_free(p);
    return;
  }
  if (p == (void*)ARENA)
    return;                      /* the arena, at dsc_shutdown */
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
  if (Z.loading && (Z.parsing || !Z.shared)) {
    size_t n = count * size;
    void* p;
    if (size && n / size != count)
      return NULL;
    p = Z.parsing ? bump_alloc(n) : place(n);
    if (p)
      return memset(p, 0, n);
  }
  return __real_calloc(count, size);
}

void* __wrap_realloc(void* p, size_t n)
{
  uint32_t old;
  void* q;
  zone_t* z;
  int i;
  if (!p)
    return __wrap_malloc(n);
  if (!in_zone(p)) {
    if ((z = zone_of(p)) == NULL)
      return __real_realloc(p, n);
    {
      zblock_t* b = (zblock_t*)((char*)p - 8);
      uint32_t need = ((uint32_t)n + 8u + 7u) & ~7u;
      if (b->tag != ZONE_USED)
        return NULL;
      if (need < ZONE_MIN)
        need = ZONE_MIN;
      if (need <= b->size) {
        /* shrink in place; what's left over goes back */
        if (b->size - need >= ZONE_MIN) {
          zblock_t* rest = (zblock_t*)((char*)b + need);
          rest->size = b->size - need;
          b->size = need;
          zone_release(z, rest);
        }
        return p;
      }
      if (Z.loading && !z->fixed && (char*)b + b->size == z->top && need - b->size <= (uint32_t)(z->end - z->top)) {
        /* the last block: grow it */
        z->top += need - b->size;
        b->size = need;
        zone_stats(z);
        return p;
      }
      old = b->size - 8u;
    }
  }
  else {
    if (p == (void*)ARENA)
      return NULL;
    i = lent_slot(p);
    old = i >= 0 ? Z.lent[i].size : *((uint32_t*)p - 2);
  }
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
  for (i = 0; i < ZONES; i++) {
    zone_t* z = &Z.zone[i];
    zblock_t* b;
    uint32_t holes = 0;
    if (!z->base)
      continue;
    for (b = z->free; b; b = b->next)
      holes += b->size;
    dscm_g_heap.zone_used[i] = (uint32_t)(z->top - z->base) - holes;
    dscm_g_heap.zone_holes[i] = holes;
  }
#ifndef DSCM_HEAP_HOST_TEST
  /* newlib's high piece starts above what the zone holds now */
  if (!g_high && Z.zone[1].base) {
    Z.zone[1].fixed = 1;
    Z.zone[1].end = Z.zone[1].top;
  }
#endif
  dscm_g_heap.load_low_top = dscm_g_heap.low_top;
  dscm_g_heap.load_high_top = dscm_g_heap.high_top;
#ifndef DSCM_HEAP_HOST_TEST
  dscm_g_heap.load_in_use = (uint32_t)mallinfo().uordblks - g_gap;
#endif
  dscm_g_heap.load_left = (uint32_t)left;
  return left ? -1 : 0;
}
