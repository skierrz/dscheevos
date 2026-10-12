/*
 * Host test of the module heap's load-time placement (src/heap.c): the core
 * and real rcheevos load a set from a session file the way module_init does,
 * with the snapshot arena, the lent replies and rc_api's parse of the set in
 * the session area. Then frames run, and the arena overwrites the replies.
 *
 *   make test                 the checks (a small set)
 *   SET=sets.json build/test_heap
 *                             a real set: heap use with and without the
 *                             placement, and that both evaluate the same
 *
 * Heap bytes are counted as newlib's malloc lays them out on the DS (8-byte
 * chunks, 4-byte header, 16 at least): build it 32-bit for DS numbers.
 */
#include "dscheevos.h"
#include "dscheevos_module.h"
#include "../src/heap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ---- the host's malloc, counted (heap.c calls these as __real_*) ---- */
static size_t chunk(size_t n) { size_t c = (n + 4 + 7) & ~(size_t)7; return c < 16 ? 16 : c; }
static size_t in_use, peak, set_blocks;   /* set_blocks: heap copies of the set (its size + 1) */
static size_t set_size = (size_t)-1;
static int counting = 1;
#define HDR 16
static void count(long d) { in_use += d; if (in_use > peak) peak = in_use; }
void* h_malloc(size_t n)
{
  size_t* p = (size_t*)malloc(n + HDR);
  if (!p) return NULL;
  p[0] = n;
  if (n == set_size + 1) set_blocks++;
  if (counting) count((long)chunk(n));
  return (char*)p + HDR;
}
void h_free(void* v)
{
  size_t* p;
  if (!v) return;
  p = (size_t*)((char*)v - HDR);
  if (counting) count(-(long)chunk(p[0]));
  free(p);
}
void* h_calloc(size_t a, size_t b)
{
  void* p = h_malloc(a * b);
  return p ? memset(p, 0, a * b) : NULL;
}
void* h_realloc(void* v, size_t n)
{
  void* q;
  if (!v) return h_malloc(n);
  q = h_malloc(n);
  if (q) {
    size_t old = ((size_t*)((char*)v - HDR))[0];
    memcpy(q, v, old < n ? old : n);
    h_free(v);
  }
  return q;
}

/* ---- the session area (DSCM_SESSION on the DS) ---- */
#define AREA (8u << 20)
char dscm_host_session[AREA] __attribute__((aligned(32)));
static dscm_boot_t* boot = (dscm_boot_t*)dscm_host_session;
static const char* g_session = dscm_host_session;

static const char login[] = "{\"Success\":true,\"User\":\"tester\",\"Token\":\"tok\",\"Score\":0,\"SoftcoreScore\":0,\"Messages\":0}";
static const char start[] = "{\"Success\":true,\"Unlocks\":[],\"HardcoreUnlocks\":[],\"ServerNow\":1760000000}";

/* As the launcher writes it; old_layout: 0 to 3 NULs after a reply, as before. */
static void make_session(const char* set, size_t set_len, int old_layout)
{
  const char* body[3] = { login, set, start };
  size_t len[3] = { sizeof(login) - 1, set_len, sizeof(start) - 1 };
  uint32_t off = (sizeof(dscm_boot_t) + 3) & ~3u;
  int i;
  memset(dscm_host_session, 0xA5, AREA);     /* garbage past the file, as in RAM */
  memset(boot, 0, sizeof(*boot));
  boot->magic = DSCM_SESSION_MAGIC;
  boot->size = sizeof(*boot);
  strcpy(boot->game_hash, "0123456789abcdef0123456789abcdef");
  boot->hardcore = 1;
  for (i = 0; i < 3; i++) {
    uint32_t next = old_layout ? ((uint32_t)len[i] + 3) & ~3u : DSCM_SESSION_PAD(len[i]);
    memcpy(dscm_host_session + off, body[i], len[i]);
    memset(dscm_host_session + off + len[i], 0, next - len[i]);
    boot->preload_offset[i] = off;
    boot->preload_length[i] = (uint32_t)len[i];
    off += next;
  }
  boot->file_size = off;
}

/* ---- platform: fake game memory, the module's preload answers ---- */
static uint8_t ram[0x400000], dtcm[0x4000];
static uint32_t frames, rng;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static uint32_t read_memory(uint32_t a, uint8_t* b, uint32_t n)
{
  if (a < sizeof(ram) && n <= sizeof(ram) - a) { memcpy(b, ram + a, n); return n; }
  if (a >= 0x1000000 && a - 0x1000000 < sizeof(dtcm) && n <= sizeof(dtcm) - (a - 0x1000000)) { memcpy(b, dtcm + (a - 0x1000000), n); return n; }
  return 0;
}
static void memory_map(const uint8_t** m, const uint8_t** d) { *m = ram; *d = dtcm; }
static uint32_t millis(void) { return frames * 1000 / 60; }
static uint32_t frame(void) { return frames; }
static uint32_t unix_time(void) { return 1760000000u + frames / 60; }
static int no_interrupt(void) { return 0; }
static dscm_journal_t journal;
static dscm_notices_t notices;
static void publish_list(dscm_list_t* l) { (void)l; }

static uint32_t digest, unlocks, late_answers;
static void on_event(const dsc_event_t* e)
{
  digest = digest * 31 + e->type * 7 + e->id + frames * 13;
  if (e->type == DSC_EVENT_UNLOCK) unlocks++;
}

static int lend = 1;
/* module.c's http_send for the session-start requests */
static void http_send(const dsc_http_request_t* r)
{
  const char* p = r->post_data ? r->post_data : "";
  int k = !strncmp(p, "r=login2&", 9) ? 0 : !strncmp(p, "r=achievementsets&", 18) ? 1 : !strncmp(p, "r=startsession&", 15) ? 2 : -1;
  if (k >= 0 && boot->preload_length[k]) {
    uint32_t off = boot->preload_offset[k], len = boot->preload_length[k];
    const char* body = g_session + off;
    if (lend && off + len < boot->file_size && body[len] == '\0')
      heap_lend(body, len);
    dsc_http_done(r->handle, 200, body, len);
    heap_lend(NULL, 0);
    return;
  }
  if (k >= 0) late_answers++;
  {
    static const char ok[] = "{\"Success\":true}";
    dsc_http_done(r->handle, 200, ok, sizeof(ok) - 1);
  }
}

typedef struct result_t {
  int active, left;
  size_t set_blocks;               /* heap copies of the set */
  size_t init, load_peak, after_load, steady, peak;
  uint32_t digest, unlocks, achievements;
  dscm_heap_t heap;
} result_t;

/* module_init's sequence, then n frames. place: the heap placement on. */
static result_t run(int place, uint32_t arena_bytes, uint32_t n)
{
  dsc_platform_t pl;
  dsc_config_t c;
  result_t res;
  dsc_progress_t pr;
  uint32_t i;

  memset(&res, 0, sizeof(res));
  memset(&dscm_g_heap, 0, sizeof(dscm_g_heap));
  in_use = peak = set_blocks = 0;
  digest = unlocks = late_answers = frames = 0;
  rng = 12345;
  for (i = 0; i < sizeof(ram); i += 4) { uint32_t v = rnd(); if (!(v & 7)) v = 0x02000000u | (rnd() & 0x3FFFFCu); memcpy(ram + i, &v, 4); }

  if (place)
    heap_load_begin(boot->file_size);
  memset(&pl, 0, sizeof(pl));
  pl.read_memory = read_memory; pl.memory_map = memory_map; pl.interrupt_pending = no_interrupt;
  pl.millis = millis; pl.http_send = http_send; pl.event = on_event; pl.unix_time = unix_time;
  pl.journal = &journal; pl.notices = &notices; pl.publish_list = publish_list; pl.frame = frame;
  memset(&c, 0, sizeof(c));
  c.host = "https://retroachievements.org"; c.username = "tester"; c.token = "tok"; c.hardcore = 1;
  c.max_addresses = 4096; c.snapshot_slots = 1024; c.snapshot_bytes = arena_bytes;
  c.capture_at_halt = 1; c.idle_max_frames = 4;
  heap_arena_window(1);
  if (dsc_init(&pl, &c) != 0) { printf("  dsc_init failed\n"); return res; }
  heap_arena_window(0);
  res.init = in_use;
  dsc_begin_session(boot->game_hash);
  for (i = 0; i < 8 && !dsc_is_active(); i++)
    dsc_idle();
  for (i = 0; i < DSCM_PRELOAD_COUNT; i++)
    boot->preload_length[i] = 0;   /* as module_init */
  res.left = place ? heap_load_end() : 0;
  res.active = dsc_is_active();
  res.load_peak = peak;
  res.set_blocks = set_blocks;
  res.after_load = in_use;
  dsc_get_progress(&pr);
  res.achievements = pr.achievements;
  for (i = 0; i < n && res.active; i++) {
    int k;
    for (k = 0; k < 256; k++) ram[rnd() & 0x3FFFFF] = (uint8_t)rnd();
    frames++;
    while (dsc_capture() == 1) {}
    dsc_vblank();
    dsc_idle();
  }
  res.steady = in_use;
  res.peak = peak;
  res.digest = digest;
  res.unlocks = unlocks;
  res.heap = dscm_g_heap;
  dsc_shutdown();
  return res;
}

/* ---- checks with a small set ---- */

static char* read_file(const char* path, size_t* len)
{
  FILE* f = fopen(path, "rb");
  char* b;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); *len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
  b = (char*)malloc(*len + 1);
  if (b && fread(b, 1, *len, f) != *len) { free(b); b = NULL; }
  fclose(f);
  return b;
}

/* a set whose achievements unlock after 300 frames and never */
static const char small_set[] =
  "{\"Success\":true,\"GameId\":1,\"Title\":\"Heap Test\",\"ConsoleId\":18,\"ImageIconUrl\":\"\",\"RichPresenceGameId\":1,"
  "\"RichPresencePatch\":\"Display:\\nHeap test\",\"Sets\":[{\"AchievementSetId\":1,\"GameId\":1,\"Title\":null,\"Type\":\"core\",\"ImageIconUrl\":\"\","
  "\"Achievements\":["
  "{\"ID\":1,\"Title\":\"300 frames\",\"Description\":\"d\",\"Flags\":3,\"Points\":1,\"MemAddr\":\"1=1.300.\",\"Author\":\"t\",\"BadgeName\":\"0\",\"Created\":1,\"Modified\":1},"
  "{\"ID\":2,\"Title\":\"Byte\",\"Description\":\"d\",\"Flags\":3,\"Points\":1,\"MemAddr\":\"0xH1000=5_0xH1001=6\",\"Author\":\"t\",\"BadgeName\":\"0\",\"Created\":1,\"Modified\":1},"
  "{\"ID\":3,\"Title\":\"Never\",\"Description\":\"d\",\"Flags\":3,\"Points\":1,\"MemAddr\":\"0=1\",\"Author\":\"t\",\"BadgeName\":\"0\",\"Created\":1,\"Modified\":1}],"
  "\"Leaderboards\":[{\"ID\":7,\"Mem\":\"STA:0xH2000=1::CAN:0=1::SUB:0xH2000=2::VAL:0xH2001\",\"Format\":\"VALUE\",\"LowerIsBetter\":false,\"Title\":\"L\",\"Description\":\"l\",\"Hidden\":false}]}]}";

static void test_placement(void)
{
  result_t a, b;
  size_t n = sizeof(small_set) - 1;
  uint32_t lent_want;
  printf("placement, small set\n");
  set_size = n;
  make_session(small_set, n, 0);
  lent_want = (uint32_t)(sizeof(login) - 1 + n + sizeof(start) - 1);
  b = run(0, HEAP_ARENA_BYTES, 600);
  make_session(small_set, n, 0);
  a = run(1, HEAP_ARENA_BYTES, 600);
  CHECK(a.active && b.active);
  CHECK(a.left == 0 && a.heap.load_left == 0);
  CHECK(a.heap.arena == (uint32_t)(uintptr_t)(dscm_host_session + HEAP_SESSION_HEAD));
  CHECK(a.heap.lent == lent_want);
  CHECK(a.heap.parse_top > a.heap.parse_base && a.heap.parse_heap == 0);
  CHECK(a.heap.parse_base == (uint32_t)(uintptr_t)(dscm_host_session + ((boot->file_size + 7) & ~7u)));
  CHECK(a.achievements == 3 && a.unlocks == 1);
  CHECK(a.digest == b.digest && a.unlocks == b.unlocks);
  /* the arena and the set's raw and parsed copies are off the heap */
  CHECK(b.init - a.init >= HEAP_ARENA_BYTES);
  printf("  heap before: init %zu load peak %zu steady %zu; after: init %zu load peak %zu steady %zu\n",
         b.init, b.load_peak, b.steady, a.init, a.load_peak, a.steady);
  CHECK(b.load_peak - a.load_peak >= HEAP_ARENA_BYTES);
  CHECK(b.set_blocks >= 1 && a.set_blocks == 0);   /* the copy of the set: heap before, not now */
  CHECK(late_answers == 0);
  set_size = (size_t)-1;
}

static void test_old_layout(void)
{
  /* a set whose length is a multiple of 4, then the next reply at once: no NUL to lend */
  static char set[sizeof(small_set) + 4];
  size_t n = sizeof(small_set) - 1;
  result_t a;
  printf("old session layout: no NUL after a reply\n");
  memcpy(set, small_set, n);
  while (n & 3) set[n++] = ' ';
  make_session(set, n, 1);
  CHECK(dscm_host_session[boot->preload_offset[1] + n] == '{');
  a = run(1, HEAP_ARENA_BYTES, 400);
  CHECK(a.active && a.left == 0 && a.unlocks == 1);
  CHECK(a.heap.lent == (uint32_t)(sizeof(login) - 1 + sizeof(start) - 1));   /* the set was copied */
}

static void test_parse_overflow(void)
{
  /* a session file that leaves too little of the arena for the parsed set */
  size_t n = sizeof(small_set) - 1, big = HEAP_ARENA_BYTES - 256;
  char* set = (char*)malloc(big + 1);
  const char* cut = strstr(small_set, "\"Title\":\"Heap Test\"");
  size_t head = (size_t)(cut - small_set) + 9;
  result_t a;
  printf("parsed set doesn't fit after the session file\n");
  memcpy(set, small_set, head);                       /* ... "Title":"     */
  memset(set + head, 'x', big - n);                   /* a long title      */
  memcpy(set + head + (big - n), small_set + head, n - head);
  make_session(set, big, 0);
  a = run(1, HEAP_ARENA_BYTES, 400);
  CHECK(a.active && a.left == 0 && a.unlocks == 1);
  CHECK(a.heap.parse_heap > 0);
  free(set);
}

static void test_no_arena(void)
{
  result_t a;
  printf("arena of another size: stays on the heap\n");
  make_session(small_set, sizeof(small_set) - 1, 0);
  a = run(1, 128 * 1024, 400);
  CHECK(a.active && a.left == 0 && a.unlocks == 1);
  CHECK(a.heap.arena == 0);
}

int main(void)
{
  const char* path = getenv("SET");
  if (path) {
    size_t n;
    char* set = read_file(path, &n);
    result_t b, a;
    uint32_t frames_n = getenv("FRAMES") ? (uint32_t)atoi(getenv("FRAMES")) : 300;
    if (!set) { printf("can't read %s\n", path); return 1; }
    make_session(set, n, 0);
    b = run(0, HEAP_ARENA_BYTES, frames_n);
    make_session(set, n, 0);
    a = run(1, HEAP_ARENA_BYTES, frames_n);
    /* session area: today 256 KB (or the file), now the header + the arena,
     * or the file and the parsed set if they are bigger */
    printf("set=%zu file=%u ach=%u active=%d/%d same=%d unlocks=%u\n"
           "  before: heap init=%zu load_peak=%zu after_load=%zu steady=%zu peak=%zu\n"
           "  after:  heap init=%zu load_peak=%zu after_load=%zu steady=%zu peak=%zu"
           " lent=%u parsed_in_arena=%u parsed_on_heap=%u left=%d\n",
           n, boot->file_size, a.achievements, b.active, a.active, a.digest == b.digest && a.unlocks == b.unlocks, a.unlocks,
           b.init, b.load_peak, b.after_load, b.steady, b.peak,
           a.init, a.load_peak, a.after_load, a.steady, a.peak,
           a.heap.lent, a.heap.parse_top - a.heap.parse_base, a.heap.parse_heap, a.left);
    return 0;
  }
  test_placement();
  test_old_layout();
  test_parse_overflow();
  test_no_arena();
  printf("%d checks, %d failures\n", checks, failures);
  return failures != 0;
}
