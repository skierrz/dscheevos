/*
 * Does a set fit the module's memory while it loads? (tests/perf/fitmem.sh)
 *
 * The core, rcheevos and the module's heap.c (load-time placement), built
 * with the module's own rules and flags, under qemu-arm. module_init's load
 * sequence runs on newlib's own malloc, whose sbrk is the module's (heap.c's
 * _sbrk_r): a low piece, then a high piece, both at their DS sizes, and the
 * high piece stops below a moved session (heap_load_begin) until heap.c gives
 * it back. So fragmentation and the layout are newlib's own, as on the DS.
 *
 *   fitmem.elf <set.json> [frames] [low piece bytes] [high piece bytes]
 *
 * High piece bytes 0: unlimited (reports what the load needs). Prints the
 * memory at each step of the load (after rc_api's parse, with the game data
 * built, after the load, after the frames) and an evaluation digest (the
 * same as realsets.c's): placement changes must not change it.
 */
#include "dscheevos.h"
#include "dscheevos_module.h"
#include "../../dscheevos/module/src/heap.h"
#include "rc_client.h"
#include "rc_api_runtime.h"
#include "rc_api_user.h"
#include "client/rc_client_internal.h"
#include "runtime/rc_internal.h"

#include <errno.h>
#include <malloc.h>
#include <reent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- qemu-arm's Linux syscalls (as realsets.c) ---- */
static int sys(int n, int a, int b, int c)
{
  register int r0 __asm__("r0") = a, r1 __asm__("r1") = b, r2 __asm__("r2") = c, r7 __asm__("r7") = n;
  __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
  return r0;
}
static void out(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char* fmt, ...)
{
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  sys(4, 1, (int)line, (int)strlen(line));
}

/* the set file: static, so reading it takes nothing from the heap */
#define FILE_MAX (2u << 20)
static char file_buf[FILE_MAX + 1];
static int read_file(const char* path)
{
  int fd = sys(5, (int)path, 0, 0), n = 0, r;
  if (fd < 0)
    return -1;
  while (n < (int)FILE_MAX && (r = sys(3, fd, (int)(file_buf + n), (int)FILE_MAX - n)) > 0)
    n += r;
  sys(6, fd, 0, 0);
  file_buf[n] = '\0';
  return n;
}

/* ---- the module's memory: session area, then the two heap pieces ---- */
char dscm_host_session[DSCM_HEAP_END - DSCM_SESSION + 64] __attribute__((aligned(32)));
#define LOW_DEFAULT  85784u      /* DSCM_SESSION - __heap_start (perf f88714a's map) */
#define HIGH_DS      (DSCM_HEAP_END - HEAP_ZONE_END)
#define HIGH_SIM     (6u << 20)
/* one array, the high piece above the low one as on the DS (newlib's malloc
 * takes memory below its old top for a failure) */
static char heap_mem[256 * 1024 + HEAP_SESSION_HEAD + HEAP_ARENA_BYTES + HIGH_SIM] __attribute__((aligned(8)));
#define low_mem  heap_mem
#define high_mem (heap_mem + 256 * 1024 + HEAP_SESSION_HEAD + HEAP_ARENA_BYTES)
static uint32_t low_size = LOW_DEFAULT, high_size = HIGH_DS;
static uint32_t reserved_top;    /* bytes at the high piece's top a moved session holds */
static char* g_brk;
static int g_high;
static uint32_t need_high, low_top, gap;   /* gap: newlib counts the bytes between the pieces as in use */  /* the most of the high piece in use incl. the moved session; low high-water */

static uint32_t used_high(void) { return g_high ? (uint32_t)(g_brk - high_mem) : 0; }
/* heap.c (host build): where its target build sets g_high_end */
void dscm_host_high_end(uint32_t reserved) { reserved_top = reserved; }

void* _sbrk_r(struct _reent* r, ptrdiff_t incr)
{
  char* lo = g_high ? high_mem : low_mem;
  char* hi = g_high ? high_mem + (high_size - reserved_top) : low_mem + low_size;
  char* prev;
  if (!g_brk)
    g_brk = low_mem;
  if (incr > hi - g_brk || incr < lo - g_brk) {
    if (g_high || incr <= 0 || incr > (ptrdiff_t)(high_size - reserved_top)) {
      out("SBRK_FAIL incr %d high %d brk_high %u\n", (int)incr, g_high, (unsigned)used_high());
      r->_errno = ENOMEM;
      return (void*)-1;
    }
    g_high = 1;
    gap = (uint32_t)(high_mem - g_brk);
    g_brk = high_mem;
  }
  prev = g_brk;
  g_brk += incr;
  if (g_high) {
    uint32_t n = (uint32_t)(g_brk - high_mem) + reserved_top;
    if (n > need_high)
      need_high = n;
  }
  else if ((uint32_t)(g_brk - low_mem) > low_top)
    low_top = (uint32_t)(g_brk - low_mem);
  return prev;
}
void* _sbrk(ptrdiff_t incr) { return _sbrk_r(_REENT, incr); }

/* ---- steps of the load ---- */
static uint32_t in_use(void) { return (uint32_t)mallinfo().uordblks - gap; }

static uint32_t peak_in_use;
static void step(const char* what)
{
  uint32_t u = in_use();
  if (u > peak_in_use)
    peak_in_use = u;
  out("STEP %-12s heap_in_use %7u low_brk %6u high_brk %7u reserved_top %7u need_high %7u\n",
      what, (unsigned)u, (unsigned)(g_high ? low_top : (uint32_t)(g_brk - low_mem)), (unsigned)used_high(),
      (unsigned)reserved_top, (unsigned)need_high);
}
/* right after rc_api's parse (the start-session request is the next thing rc_client does) */
int __real_rc_api_init_start_session_request_hosted(rc_api_request_t* request, const rc_api_start_session_request_t* p, const rc_api_host_t* host);
int __wrap_rc_api_init_start_session_request_hosted(rc_api_request_t* request, const rc_api_start_session_request_t* p, const rc_api_host_t* host)
{
  step("parsed");
  return __real_rc_api_init_start_session_request_hosted(request, p, host);
}
/* the game data built, the parse not freed yet */
void __real_rc_api_destroy_fetch_game_sets_response(rc_api_fetch_game_sets_response_t* response);
void __wrap_rc_api_destroy_fetch_game_sets_response(rc_api_fetch_game_sets_response_t* response)
{
  step("built");
  __real_rc_api_destroy_fetch_game_sets_response(response);
  step("parse_freed");
}

/* ---- evaluation digest (FNV-1a, as realsets.c) ---- */
void __real_rc_client_do_frame(rc_client_t* client);
static rc_client_t* seen_client;
void __wrap_rc_client_do_frame(rc_client_t* client) { seen_client = client; __real_rc_client_do_frame(client); }
static uint32_t digest = 2166136261u;
static void mix(uint32_t v) { int i; for (i = 0; i < 4; i++) { digest ^= (v >> (i * 8)) & 0xFF; digest *= 16777619u; } }
static void mix_condsets(const rc_condset_t* cs)
{
  for (; cs; cs = cs->next) {
    const rc_condition_t* c;
    mix(cs->is_paused);
    for (c = cs->conditions; c; c = c->next) { mix(c->current_hits); mix(c->is_true); }
  }
}
static void mix_trigger(const rc_trigger_t* t)
{
  if (!t) return;
  mix(t->state); mix(t->measured_value); mix(t->has_hits);
  mix_condsets(t->requirement); mix_condsets(t->alternative);
}
static void digest_frame(rc_client_t* client)
{
  const rc_client_game_info_t* g = client ? client->game : NULL;
  const rc_client_subset_info_t* sub;
  const rc_memref_list_t* ml;
  const rc_modified_memref_list_t* mml;
  char rp[256];
  uint32_t i;
  if (!g) return;
  for (ml = &g->runtime.memrefs->memrefs; ml; ml = ml->next)
    for (i = 0; i < ml->count; i++) {
      mix(ml->items[i].value.value); mix(ml->items[i].value.prior);
      mix(ml->items[i].value.changed); mix(ml->items[i].value.type);
    }
  for (mml = &g->runtime.memrefs->modified_memrefs; mml; mml = mml->next)
    for (i = 0; i < mml->count; i++) {
      mix(mml->items[i].memref.value.value); mix(mml->items[i].memref.value.prior);
      mix(mml->items[i].memref.value.changed);
    }
  for (sub = g->subsets; sub; sub = sub->next) {
    mix(sub->public_.num_achievements);
    for (i = 0; i < sub->public_.num_achievements; i++) {
      const rc_client_achievement_info_t* a = &sub->achievements[i];
      const char* s;
      mix(a->public_.id); mix(a->public_.state); mix(a->public_.unlocked); mix(a->public_.points);
      for (s = a->public_.title; s && *s; s++) mix((uint8_t)*s);
      for (s = a->public_.description; s && *s; s++) mix((uint8_t)*s);
      for (s = a->public_.badge_name; *s; s++) mix((uint8_t)*s);
      mix_trigger(a->trigger);
    }
    for (i = 0; i < sub->public_.num_leaderboards; i++) {
      const rc_client_leaderboard_info_t* l = &sub->leaderboards[i];
      mix(l->public_.id); mix(l->public_.state); mix((uint32_t)l->value);
      if (l->lboard) {
        mix(l->lboard->state);
        mix_trigger(&l->lboard->start); mix_trigger(&l->lboard->submit); mix_trigger(&l->lboard->cancel);
      }
    }
  }
  rp[0] = '\0';
  rc_client_get_rich_presence_message(client, rp, sizeof(rp));
  for (i = 0; rp[i]; i++) mix((uint8_t)rp[i]);
}

/* ---- the session file, as the launcher writes it ---- */
static dscm_boot_t* const boot = (dscm_boot_t*)dscm_host_session;
static const char* g_session = dscm_host_session;
static const char login[] = "{\"Success\":true,\"User\":\"tester\",\"Token\":\"tok\",\"Score\":0,\"SoftcoreScore\":0,\"Messages\":0}";
static const char start[] = "{\"Success\":true,\"Unlocks\":[],\"HardcoreUnlocks\":[],\"ServerNow\":1760000000}";
static const char ca[4096];   /* stands in for the CA the launcher appends (read at module_init's end) */
static int make_session(const char* set, uint32_t set_len)
{
  const char* body[3] = { login, set, start };
  uint32_t len[3] = { sizeof(login) - 1, set_len, sizeof(start) - 1 };
  uint32_t off = (sizeof(dscm_boot_t) + 3) & ~3u;
  int i;
  memset(boot, 0, sizeof(*boot));
  boot->magic = DSCM_SESSION_MAGIC;
  boot->size = sizeof(*boot);
  strcpy(boot->game_hash, "0123456789abcdef0123456789abcdef");
  boot->hardcore = 1;
  for (i = 0; i < 3; i++) {
    uint32_t next = DSCM_SESSION_PAD(len[i]);
    if (off + next > sizeof(dscm_host_session) - sizeof(ca))
      return -1;
    memcpy(dscm_host_session + off, body[i], len[i]);
    memset(dscm_host_session + off + len[i], 0, next - len[i]);
    boot->preload_offset[i] = off;
    boot->preload_length[i] = len[i];
    off += next;
  }
  boot->ca_offset = off;
  boot->ca_length = sizeof(ca);
  memset(dscm_host_session + off, 0x30, sizeof(ca));
  off += sizeof(ca);
  boot->file_size = off;
  return 0;
}

/* ---- platform ---- */
static uint8_t ram[0x400000] __attribute__((aligned(4))), dtcm[0x4000];
static uint32_t frames, rng = 12345;
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
static uint32_t unlocks;
static void on_event(const dsc_event_t* e)
{
  mix(e->type); mix(e->id); mix(frames);
  if (e->type == DSC_EVENT_UNLOCK) unlocks++;
}
/* module.c's http_send for the session-start requests */
static void http_send(const dsc_http_request_t* r)
{
  const char* p = r->post_data ? r->post_data : "";
  int k = !strncmp(p, "r=login2&", 9) ? 0 : !strncmp(p, "r=achievementsets&", 18) ? 1 : !strncmp(p, "r=startsession&", 15) ? 2 : -1;
  if (k >= 0 && boot->preload_length[k]) {
    uint32_t off = boot->preload_offset[k], len = boot->preload_length[k];
    const char* body = g_session + off;
    if (off + len < boot->file_size && body[len] == '\0')
      heap_lend(body, len);
    dsc_http_done(r->handle, 200, body, len);
    heap_lend(NULL, 0);
    return;
  }
  {
    static const char ok[] = "{\"Success\":true}";
    dsc_http_done(r->handle, 200, ok, sizeof(ok) - 1);
  }
}

int main(int argc, char** argv)
{
  dsc_platform_t pl;
  dsc_config_t c;
  dsc_progress_t pr;
  uint32_t i, n_frames;
  int n, left, active;

  if (argc < 2 || (n = read_file(argv[1])) <= 0) {
    out("usage: fitmem.elf <set.json> [frames] [low bytes] [high bytes, 0 unlimited]\n");
    return 2;
  }
  n_frames = argc > 2 ? (uint32_t)atoi(argv[2]) : 300;
  if (argc > 3) low_size = (uint32_t)atoi(argv[3]);
  if (argc > 4) high_size = (uint32_t)atoi(argv[4]);
  if (!high_size) high_size = HIGH_SIM;
  if (low_size > 256 * 1024 || high_size > HIGH_SIM || make_session(file_buf, (uint32_t)n) != 0) {
    out("bad sizes or set too big\n");
    return 2;
  }
  out("SET bytes %d file %u low %u high %u (DS high piece %u)\n", n, (unsigned)boot->file_size,
      (unsigned)low_size, (unsigned)high_size, (unsigned)HIGH_DS);
  for (i = 0; i < sizeof(ram); i += 4) { uint32_t v = rnd(); if (!(v & 7)) v = 0x02000000u | (rnd() & 0x3FFFFCu); memcpy(ram + i, &v, 4); }

  g_session = heap_load_begin(boot->file_size);
  memset(&pl, 0, sizeof(pl));
  pl.read_memory = read_memory; pl.memory_map = memory_map; pl.interrupt_pending = no_interrupt;
  pl.millis = millis; pl.http_send = http_send; pl.event = on_event; pl.unix_time = unix_time;
  pl.journal = &journal; pl.notices = &notices; pl.publish_list = publish_list; pl.frame = frame;
  memset(&c, 0, sizeof(c));
  c.host = "https://retroachievements.org"; c.username = "tester"; c.token = "tok"; c.hardcore = 1;
  c.max_addresses = 4096; c.snapshot_slots = 1024; c.snapshot_bytes = HEAP_ARENA_BYTES;
  c.capture_at_halt = 1; c.idle_max_frames = 4;
  heap_arena_window(1);
  if (dsc_init(&pl, &c) != 0) { out("FAIL dsc_init\n"); return 1; }
  heap_arena_window(0);
  step("init");
  dsc_begin_session(boot->game_hash);
  for (i = 0; i < 8 && !dsc_is_active(); i++)
    dsc_idle();
  for (i = 0; i < DSCM_PRELOAD_COUNT; i++)
    boot->preload_length[i] = 0;
  left = heap_load_end();
  active = dsc_is_active();
  step("loaded");
  heap_release_session();
  step("released");
  dsc_get_progress(&pr);
  out("LOAD active %d left %d oom %u achievements %u lent %u parse_arena %u parse_heap %u moved %u\n",
      active, left, (unsigned)dscm_g_heap.oom, (unsigned)pr.achievements, (unsigned)dscm_g_heap.lent,
      (unsigned)(dscm_g_heap.parse_top - dscm_g_heap.parse_base), (unsigned)dscm_g_heap.parse_heap,
      (unsigned)(dscm_g_heap.moved ? 1 : 0));
  for (i = 0; i < n_frames && active; i++) {
    int k;
    for (k = 0; k < 512; k++) ram[rnd() & 0x3FFFFF] = (uint8_t)rnd();
    frames++;
    while (dsc_capture() == 1) {}
    dsc_vblank();
    dsc_idle();
    digest_frame(seen_client);
  }
  step("steady");
  if (seen_client && seen_client->game) {
    /* rc_client's game data: its buffer (triggers, strings) and the shared memrefs */
    const rc_client_game_info_t* g = seen_client->game;
    const rc_buffer_chunk_t* ch;
    const rc_memref_list_t* ml;
    const rc_modified_memref_list_t* mml;
    uint32_t chunks = 0, size = 0, unused = 0, memrefs = 0, mods = 0, conds = 0;
    const rc_client_subset_info_t* sub;
    for (ch = &g->buffer.chunk; ch; ch = ch->next) {
      chunks++;
      size += (uint32_t)(ch->end - ch->start);
      unused += (uint32_t)(ch->end - ch->write);
    }
    for (ml = &g->runtime.memrefs->memrefs; ml; ml = ml->next) memrefs += ml->count;
    for (mml = &g->runtime.memrefs->modified_memrefs; mml; mml = mml->next) mods += mml->count;
    for (sub = g->subsets; sub; sub = sub->next)
      for (i = 0; i < sub->public_.num_achievements; i++) {
        const rc_trigger_t* t = sub->achievements[i].trigger;
        const rc_condset_t* cs;
        const rc_condition_t* c;
        if (!t) continue;
        for (cs = t->requirement; cs; cs = cs->next) for (c = cs->conditions; c; c = c->next) conds++;
        for (cs = t->alternative; cs; cs = cs->next) for (c = cs->conditions; c; c = c->next) conds++;
      }
    out("GAME buffer %u bytes in %u chunks (%u unused); memrefs %u (%u B each), modified %u (%u B), achievement conditions %u (%u B)\n",
        (unsigned)size, (unsigned)chunks, (unsigned)unused, (unsigned)memrefs, (unsigned)sizeof(rc_memref_t),
        (unsigned)mods, (unsigned)sizeof(rc_modified_memref_t), (unsigned)conds, (unsigned)sizeof(rc_condition_t));
  }
  out("RESULT fits %d need_high %u high_ds %u low_top %u low %u peak_in_use %u steady_in_use %u digest %08x unlocks %u frames %u\n",
      active && !dscm_g_heap.oom && need_high <= HIGH_DS && low_top <= low_size,
      (unsigned)need_high, (unsigned)HIGH_DS, (unsigned)low_top, (unsigned)low_size,
      (unsigned)peak_in_use, (unsigned)in_use(), (unsigned)digest, (unsigned)unlocks, (unsigned)n_frames);
  out("DONE\n");
  sys(248, 0, 0, 0);
  return 0;
}
