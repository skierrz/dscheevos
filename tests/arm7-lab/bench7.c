/*
 * arm7-lab payload: the DScheevos core and rcheevos (unmodified), built for
 * the ARM7 (armv4t), run on the ARM7 over the same frames as tests/cpi-lab's
 * set bench: the same set, RAM, seed and per-frame changes, dsc_capture +
 * dsc_vblank + dsc_idle every frame. dsc_capture and dsc_idle (evaluation)
 * are timed with ARM7 timers 0-1 (33.5 MHz, one tick = one ARM7 cycle), IRQs
 * off. A standalone ROM: the timers are ours.
 */
#include "dscheevos.h"
#include "dscheevos_module.h"
#include "lab7.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REG_IME     (*(volatile uint32_t*)0x04000208)
#define TM0_DATA    (*(volatile uint16_t*)0x04000100)
#define TM0_CR      (*(volatile uint16_t*)0x04000102)
#define TM1_DATA    (*(volatile uint16_t*)0x04000104)
#define TM1_CR      (*(volatile uint16_t*)0x04000106)

static lab7_params_t* P;
static int loglen;

static void say(const char* fmt, ...)
{
  va_list ap;
  int n;
  if (loglen >= LAB7_LOG_BYTES - 2) return;
  va_start(ap, fmt);
  n = vsnprintf(P->log + loglen, LAB7_LOG_BYTES - 1 - loglen, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  loglen += n;
  if (loglen > LAB7_LOG_BYTES - 2) loglen = LAB7_LOG_BYTES - 2;
  P->log[loglen++] = '\n';
  P->log[loglen] = 0;
}

static void timer_start(void)
{
  TM0_CR = 0; TM1_CR = 0;
  TM0_DATA = 0; TM1_DATA = 0;
  TM1_CR = 0x84;          /* enable, count-up (cascade) */
  TM0_CR = 0x80;          /* enable, F/1 = 33.5 MHz */
}
static inline uint32_t now(void)
{
  uint32_t hi, lo, hi2;
  do {
    hi = TM1_DATA;
    lo = TM0_DATA;
    hi2 = TM1_DATA;
  } while (hi != hi2);
  return (hi << 16) | lo;
}

/* ---------------------------------------------------------------- as tests/cpi-lab */

static uint8_t* ram;
static uint8_t* dtcm_fake;
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static uint32_t platform_read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes)
{
  if (address < 0x400000u && address + num_bytes <= 0x400000u) {
    memcpy(buffer, ram + address, num_bytes);
    return num_bytes;
  }
  if (address >= 0x1000000u && address + num_bytes <= 0x1004000u) {
    memcpy(buffer, dtcm_fake + (address - 0x1000000u), num_bytes);
    return num_bytes;
  }
  return 0;
}
static void memory_map(const uint8_t** main_ram, const uint8_t** dtcm_base) { *main_ram = ram; *dtcm_base = dtcm_fake; }
static uint32_t frames;
static uint32_t millis(void) { return frames * 1000u / 60u; }
static uint32_t frame(void) { return frames; }
static uint32_t unix_time(void) { return 1700000000u + frames / 60u; }
uint32_t dscm_unix_time(void) { return unix_time(); }

#define MAX_REQ 16
static struct { void* handle; const char* body; } req[MAX_REQ];
static int req_count;
static void http_send(const dsc_http_request_t* r)
{
  const char* p = r->post_data ? r->post_data : "";
  const char* body = "{\"Success\":true}";
  if (strstr(p, "r=login2"))
    body = "{\"Success\":true,\"User\":\"User\",\"AvatarUrl\":\"http://server/UserPic/USER.png\",\"Token\":\"ApiToken\",\"Score\":12345,\"Messages\":2}";
  else if (strstr(p, "r=achievementsets"))
    body = P->set_json;
  else if (strstr(p, "r=startsession"))
    body = "{\"Success\":true,\"Unlocks\":[],\"HardcoreUnlocks\":[]}";
  else if (strstr(p, "r=awardachievement"))
    body = "{\"Success\":true,\"Score\":5432,\"SoftcoreScore\":777,\"AchievementID\":5,\"AchievementsRemaining\":6}";
  if (req_count < MAX_REQ) {
    req[req_count].handle = r->handle;
    req[req_count].body = body;
    req_count++;
  }
}
static void answer_requests(void)
{
  int i, n = req_count;
  req_count = 0;
  for (i = 0; i < n; i++)
    dsc_http_done(req[i].handle, 200, req[i].body, strlen(req[i].body));
}
static void on_event(const dsc_event_t* e) { (void)e; }
static void publish_list(struct dscm_list_t* list) { (void)list; }
static int interrupt_pending(void) { return 0; }
static dscm_journal_t journal;
static dscm_notices_t notices;

static void change_memory(int n)
{
  int k;
  for (k = 0; k < n; k++)
    ram[rnd() & 0x3FFFFF] = (uint8_t)rnd();
  for (k = 0; k < n / 16; k++)
    dtcm_fake[rnd() & 0x3FFF] = (uint8_t)rnd();
}

static int cmp_u32(const void* a, const void* b)
{
  uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
  return x < y ? -1 : x > y;
}
static void report(const char* label, const uint32_t* src, int n)
{
  static uint32_t v[LAB7_MAX_FRAMES];
  uint32_t sum = 0;
  int i;
  for (i = 0; i < n; i++) { v[i] = src[i]; sum += v[i]; }
  qsort(v, n, sizeof(uint32_t), cmp_u32);
  say("%-18s median %8lu  p10 %8lu  max %8lu  mean %8lu ARM7 cycles", label, (unsigned long)v[n / 2],
      (unsigned long)v[n / 10], (unsigned long)v[n - 1], (unsigned long)(sum / n));
}

extern char __heap_start[];
extern uint32_t heap_high;

void bench7_main(lab7_params_t* p)
{
  dsc_platform_t platform;
  dsc_config_t config;
  dsc_stats_t s;
  int i, n;

  P = p;
  loglen = 0;
  ram = p->ram;
  dtcm_fake = p->dtcm;
  n = p->frames > LAB7_MAX_FRAMES ? LAB7_MAX_FRAMES : (int)p->frames;
  timer_start();
#ifdef LAB7_NWRAM
  {
    extern void nwram_map7(void);
    volatile uint32_t* t = (volatile uint32_t*)0x0317FFF8;   /* the heap's far end: not used yet */
    volatile uint32_t* u = (volatile uint32_t*)0x0317FFFC;
    nwram_map7();
    *t = 0x12345678u; *u = 0x9ABCDEF0u;
    if (*t != 0x12345678u || *u != 0x9ABCDEF0u) { say("FAIL NWRAM not mapped to the ARM7"); p->status = 2; return; }
    say("NWRAM-B/C mapped at 0x03100000 (512 KB)");
  }
#endif

  memset(&platform, 0, sizeof(platform));
  platform.read_memory = platform_read_memory;
  platform.millis = millis;
  platform.http_send = http_send;
  platform.event = on_event;
  platform.unix_time = unix_time;
  platform.journal = &journal;
  platform.notices = &notices;
  platform.publish_list = publish_list;
  platform.frame = frame;
  platform.memory_map = memory_map;
  platform.interrupt_pending = interrupt_pending;
  memset(&config, 0, sizeof(config));
  config.host = "http://server";
  config.username = "User";
  config.token = "ApiToken";
  config.hardcore = 1;
  config.max_addresses = 4096;
  config.snapshot_slots = 1024;
#ifdef LAB7_NWRAM
  config.snapshot_bytes = 32 * 1024;    /* the arena in NWRAM too (one frame in flight here) */
#else
  config.snapshot_bytes = 256 * 1024;
#endif
  config.capture_at_halt = 1;
  {
    uint32_t t0 = now();
    if (dsc_init(&platform, &config) != 0 || dsc_begin_session("0123456789ABCDEF0123456789ABCDEF") != 0) {
      say("FAIL init");
      p->status = 2;
      return;
    }
    for (i = 0; i < 16 && !dsc_is_active(); i++) {
      answer_requests();
      dsc_idle();
    }
    if (!dsc_is_active()) { say("FAIL set not loaded"); p->status = 2; return; }
    say("set load %lu ARM7 cycles", (unsigned long)(now() - t0));
  }
  dsc_get_stats(&s);
  p->addresses = s.addresses;
  say("%s RAM: %lu addresses", p->ram_dumped ? "dumped" : "random", (unsigned long)s.addresses);

  frames++;
  dsc_vblank();
  dsc_idle();
  for (i = 0; i < n; i++) {
    uint32_t t0, t1, t2, cap, ime;
    frames++;
    change_memory(512);
    ime = REG_IME;
    REG_IME = 0;
    t0 = now();
    dsc_capture();
    cap = now() - t0;
    REG_IME = ime;
    dsc_vblank();
    answer_requests();
    ime = REG_IME;
    REG_IME = 0;
    t1 = now();
    dsc_idle();
    t2 = now();
    REG_IME = ime;
    p->cap[i] = cap;
    p->idle[i] = t2 - t1;
  }
  report("capture", p->cap, n);
  report("idle (eval)", p->idle, n);
  dsc_get_stats(&s);
  p->evaluated = s.frames_evaluated;
  p->dropped = s.frames_dropped;
  p->heap_used = heap_high - (uint32_t)__heap_start;
  say("evaluated %lu, dropped %lu, heap %lu KB", (unsigned long)s.frames_evaluated, (unsigned long)s.frames_dropped,
      (unsigned long)(p->heap_used >> 10));
#ifdef LAB7_NWRAM
  {
    extern uint32_t nw_used, nw_spilled;
    say("NWRAM used %lu KB, small allocations spilled to main RAM %lu KB", (unsigned long)(nw_used >> 10),
        (unsigned long)(nw_spilled >> 10));
  }
#endif
  p->status = 1;
}
