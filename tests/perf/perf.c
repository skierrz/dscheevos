/*
 * Instruction-count harness for the DScheevos core (tests/perf/run.sh).
 *
 * Runs the real core and rcheevos, built with the module's flags, against a
 * fake game RAM and a synthetic set, under qemu-arm. Calls around which the
 * count is taken are bracketed by the mark_* functions below; run.sh finds
 * them in qemu's execution trace.
 *
 *   perf.elf <frames> <bytes changed per frame>
 *
 * First half: dsc_vblank then dsc_idle every frame (the queue is empty at
 * VBlank). Second half: dsc_idle only every 4th frame, so dsc_vblank finds
 * the frame before it still queued and compares against it (repeat check).
 */
#include "dscheevos.h"
#include "dscheevos_module.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include SET_HEADER

/* ---- output through qemu-arm's Linux syscalls (devkitARM's newlib has no console) ---- */
static int sys(int n, int a, int b, int c)
{
  register int r0 __asm__("r0") = a, r1 __asm__("r1") = b, r2 __asm__("r2") = c, r7 __asm__("r7") = n;
  __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
  return r0;
}
static void out(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char* fmt, ...)
{
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  sys(4, 1, (int)line, (int)strlen(line));    /* write(1, ...) */
}
static void quit(int code)
{
  sys(248, code, 0, 0);    /* exit_group */
}

/* ---- markers: never inlined, each a separate address ---- */
volatile int marker_sink;
void __attribute__((noinline)) mark_vblank(void) { marker_sink = 1; __asm__ volatile(""); }
void __attribute__((noinline)) mark_vblank_queued(void) { marker_sink = 2; __asm__ volatile(""); }
void __attribute__((noinline)) mark_idle(void) { marker_sink = 3; __asm__ volatile(""); }
void __attribute__((noinline)) mark_end(void) { marker_sink = 0; __asm__ volatile(""); }

/* ---- fake game memory: 4 MB main RAM + 16 KB DTCM ---- */
static uint8_t ram[0x400000] __attribute__((aligned(4))), dtcm[0x4000];
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* Same shape as module.c's read_memory. */
static uint32_t platform_read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes)
{
  if (address < 0x400000u && address + num_bytes <= 0x400000u) {
    memcpy(buffer, ram + address, num_bytes);
    return num_bytes;
  }
  if (address >= 0x1000000u && address + num_bytes <= 0x1004000u) {
    memcpy(buffer, dtcm + (address - 0x1000000u), num_bytes);
    return num_bytes;
  }
  return 0;
}

/* As module.c gives it: the snapshot reads game memory directly. */
static void memory_map(const uint8_t** main_ram, const uint8_t** dtcm_base)
{
  *main_ram = ram;
  *dtcm_base = dtcm;
}

static uint32_t frames;
static uint32_t millis(void) { return frames * 1000u / 60u; }
static uint32_t frame(void) { return frames; }
static uint32_t unix_time(void) { return 1700000000u + frames / 60u; }

/* ---- fake server: answers are queued and handed back in idle time ---- */
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
    body = patch_json;
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

static uint32_t unlocks;
static void on_event(const dsc_event_t* e)
{
  if (e->type == DSC_EVENT_UNLOCK)
    unlocks++;
}
static void publish_list(struct dscm_list_t* list) { (void)list; }

static dscm_journal_t journal;
static dscm_notices_t notices;

static void change_memory(int n)
{
  int k;
  for (k = 0; k < n; k++)
    ram[rnd() & 0x3FFFFF] = (uint8_t)rnd();
  for (k = 0; k < n / 16; k++)
    dtcm[rnd() & 0x3FFF] = (uint8_t)rnd();
}

int main(int argc, char** argv)
{
  int n = argc > 1 ? atoi(argv[1]) : 300;
  int mutate = argc > 2 ? atoi(argv[2]) : 512;
  dsc_platform_t platform;
  dsc_config_t config;
  dsc_stats_t s;
  int i;

  /* random RAM; one word in eight looks like a DS pointer (0x02xxxxxx).
   * Filled 8 words per loop pass with an inlined xorshift: qemu logs every
   * block it runs, and the fill would otherwise be most of the log. */
  {
    uint32_t* w = (uint32_t*)ram;
    uint32_t x = rng;
#define NEXT (x ^= x << 13, x ^= x >> 17, x ^= x << 5, (x & 7) == 0 ? 0x02000000u | (x & 0x3FFFFCu) : x)
    for (i = 0; i < (int)(sizeof(ram) / 4); i += 8) {
      w[i] = NEXT; w[i + 1] = NEXT; w[i + 2] = NEXT; w[i + 3] = NEXT;
      w[i + 4] = NEXT; w[i + 5] = NEXT; w[i + 6] = NEXT; w[i + 7] = NEXT;
    }
#undef NEXT
    rng = x;
  }

  /* as module.c sets it up */
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
  memset(&config, 0, sizeof(config));
  config.host = "http://server";
  config.username = "User";
  config.token = "ApiToken";
  config.hardcore = 1;
  config.max_addresses = 4096;
  config.snapshot_slots = 1024;
  config.snapshot_bytes = 256 * 1024;

  if (dsc_init(&platform, &config) != 0 || dsc_begin_session("0123456789ABCDEF0123456789ABCDEF") != 0) {
    out("FAIL init\n");
    quit(1);
  }
  for (i = 0; i < 16 && !dsc_is_active(); i++) {
    answer_requests();
    dsc_idle();
  }
  if (!dsc_is_active()) {
    out("FAIL game not loaded\n");
    quit(1);
  }
  dsc_get_stats(&s);
  out("INFO addresses %u, stack near 0x%08x\n", (unsigned)s.addresses, (unsigned)(uintptr_t)&s);

  for (i = 0; i < n; i++) {
    int queued = i >= n / 2;
    frames++;
    change_memory(mutate);
    if (queued && (i % 4) != 0)
      mark_vblank_queued();
    else
      mark_vblank();
    dsc_vblank();
    mark_end();
    answer_requests();
    if (!queued) {
      mark_idle();
      dsc_idle();
      mark_end();
    }
    else if ((i % 4) == 3) {
      dsc_idle();
    }
  }
  dsc_idle();

  dsc_get_stats(&s);
  out("INFO snapshotted %u evaluated %u repeated %u dropped %u live_reads %u unlocks %u\n",
    (unsigned)s.frames_snapshotted, (unsigned)s.frames_evaluated, (unsigned)s.frames_repeated,
    (unsigned)s.frames_dropped, (unsigned)s.live_reads, (unsigned)unlocks);
  dsc_shutdown();
  out("DONE\n");
  quit(0);
  return 0;
}
