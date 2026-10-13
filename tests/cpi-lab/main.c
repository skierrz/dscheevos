/*
 * cpi-lab: ARM9 timing on the DSi, for checking melonDS's cache timing model
 * (the CPI our evaluation budgets rest on) against the console.
 *
 * DSi mode (16 MB RAM), the ARM9 at 67 MHz as in a DS-mode game. Each test
 * runs with IRQs off and is timed with timers 0-1 at the bus clock (one tick
 * is two ARM9 cycles); this is a standalone ROM, not a game, so the timers
 * are ours. Results go to the screen, the emulator log (@@ lines) and
 * sd:/dscheevos-lab/cpi.txt.
 *
 * 1. Memory patterns: cycles per access for sequential and line-stride
 *    reads with the D-cache on (cold and warm), the uncached mirror, and
 *    stores to lines not in the cache (the write buffer).
 * 2. The DScheevos core and rcheevos (the module's build flags) on the
 *    synthetic typical set, fake RAM, the same seed and per-frame changes as
 *    tests/perf/perf.c's first half: dsc_capture and dsc_idle cycles per
 *    frame, with both caches emptied before each frame (as the game leaves
 *    them) and without. tests/perf/run.sh counts the instructions of the
 *    same calls under qemu, so cycles / instructions is the console's CPI.
 */
#include "dscheevos.h"
#include "dscheevos_module.h"
#include "heap.h"

#include <nds.h>
#include <fat.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <sys/stat.h>

#include SET_HEADER

/* ---------------------------------------------------------------- log */

static char logbuf[16384];
static int loglen;

static void say(const char* fmt, ...)
{
  char b[200], m[208] = "@@";
  va_list ap;
  int n;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  iprintf("%s\n", b);
  n = strlen(b);
  if (loglen + n + 2 < (int)sizeof logbuf) {
    memcpy(logbuf + loglen, b, n);
    loglen += n;
    logbuf[loglen++] = '\n';
  }
  strcat(m, b);
  strcat(m, "\n");
  nocashMessage(m);
}

static void flush_log(void)
{
  FILE* f;
  mkdir("sd:/dscheevos-lab", 0777);
  f = fopen("sd:/dscheevos-lab/cpi.txt", "w");
  if (!f) { say("SD: can't write the log"); return; }
  fwrite(logbuf, 1, loglen, f);
  fclose(f);
}

/* ---------------------------------------------------------------- timing */

#define REG_SCFG_CLK9 (*(vu16*)0x04004004)

static inline u32 now(void) { return cpuGetTiming(); }   /* bus ticks, 33.5 MHz */

static int cmp_u32(const void* a, const void* b)
{
  u32 x = *(const u32*)a, y = *(const u32*)b;
  return x < y ? -1 : x > y;
}

/* ---------------------------------------------------------------- memory patterns */

#define BUF_BYTES (256 * 1024)
static u32* buf;     /* cached main RAM */

static u32 __attribute__((noinline, target("arm"))) read_seq(const volatile u32* p, u32 words)
{
  u32 s = 0, i;
  for (i = 0; i < words; i += 4)
    s += p[i] + p[i + 1] + p[i + 2] + p[i + 3];
  return s;
}

static u32 __attribute__((noinline, target("arm"))) read_stride(const volatile u32* p, u32 count, u32 stride_words)
{
  u32 s = 0, i;
  for (i = 0; i < count; i++)
    s += p[i * stride_words];
  return s;
}

static void __attribute__((noinline, target("arm"))) write_seq(volatile u32* p, u32 words)
{
  u32 i;
  for (i = 0; i < words; i += 4) {
    p[i] = i; p[i + 1] = i; p[i + 2] = i; p[i + 3] = i;
  }
}

/* isolated misses: one load per line at a given word offset, ~60 ALU
 * cycles apart (cache streaming: an early word arrives sooner than a late one) */
static u32 __attribute__((noinline, target("arm"))) read_isolated(const volatile u32* p, u32 count, u32 offset_words)
{
  u32 s = 0, i, k;
  for (i = 0; i < count; i++) {
    s += p[i * 8 + offset_words];
    for (k = 0; k < 20; k++) asm volatile("" ::: "memory");
  }
  return s;
}

/* bursts of 8 stores to lines not in the cache, then ~150 cycles of ALU
 * work (the write buffer would hide the stores) */
static void __attribute__((noinline, target("arm"))) write_bursts(volatile u32* p, u32 bursts)
{
  u32 i, k;
  for (i = 0; i < bursts; i++) {
    volatile u32* q = p + i * 64;
    q[0] = i; q[8] = i; q[16] = i; q[24] = i; q[32] = i; q[40] = i; q[48] = i; q[56] = i;
    for (k = 0; k < 50; k++) asm volatile("" ::: "memory");
  }
}

static u32 __attribute__((noinline, target("arm"))) alu_only(u32 count)
{
  u32 i, k;
  for (i = 0; i < count; i++)
    for (k = 0; k < 20; k++) asm volatile("" ::: "memory");
  return count;
}

static void __attribute__((noinline, target("arm"))) alu_bursts(u32 bursts)
{
  u32 i, k;
  for (i = 0; i < bursts; i++)
    for (k = 0; k < 50; k++) asm volatile("" ::: "memory");
}

/* 64 independent ALU instructions per pass, in each state: cycles per
 * instruction with the code in the I-cache (an ARM9E issues one a cycle) */
#define ADDS8 "adds r0, #1\n adds r1, #1\n adds r2, #1\n adds r3, #1\n adds r0, #1\n adds r1, #1\n adds r2, #1\n adds r3, #1\n"
static void __attribute__((noinline, target("thumb"))) alu_thumb(u32 passes)
{
  asm volatile(".syntax unified\n1:\n" ADDS8 ADDS8 ADDS8 ADDS8 ADDS8 ADDS8 ADDS8 ADDS8 "subs %0, #1\n bne 1b\n .syntax divided\n"
               : "+l"(passes) :: "r0", "r1", "r2", "r3", "cc");
}
static void __attribute__((noinline, target("arm"))) alu_arm(u32 passes)
{
  asm volatile(".syntax unified\n1:\n" ADDS8 ADDS8 ADDS8 ADDS8 ADDS8 ADDS8 ADDS8 ADDS8 "subs %0, %0, #1\n bne 1b\n .syntax divided\n"
               : "+r"(passes) :: "r0", "r1", "r2", "r3", "cc");
}

static u32 sink;

/* cycles per access for fn over n accesses, IRQs off */
#define TIME(label, n, prep, call) do {                                   \
    u32 t0, t1, ime = REG_IME;                                            \
    REG_IME = 0;                                                          \
    prep;                                                                 \
    t0 = now(); call; t1 = now();                                         \
    REG_IME = ime;                                                        \
    say("%-26s %5lu.%02lu cyc/access (%lu accesses)", label,              \
        (unsigned long)((t1 - t0) * 2 / (n)),                             \
        (unsigned long)(((t1 - t0) * 200 / (n)) % 100), (unsigned long)(n)); \
  } while (0)

static void memory_tests(void)
{
  const u32 words = BUF_BYTES / 4;
  volatile u32* unc = (volatile u32*)((u32)buf - 0x02000000u + 0x0C000000u);   /* DSi uncached mirror */

  alu_thumb(4); alu_arm(4);
  TIME("Thumb ALU, cached code", 66 * 4096, (void)0, alu_thumb(4096));
  TIME("ARM ALU, cached code", 66 * 4096, (void)0, alu_arm(4096));
  memset(buf, 0x5A, BUF_BYTES);
  DC_FlushAll();
  TIME("seq read, cold", words, DC_FlushAll(), sink += read_seq(buf, words));
  TIME("seq read 2 KB, warm", 512 * 32, (DC_FlushAll(), sink += read_seq(buf, 512)),
       { int k; for (k = 0; k < 32; k++) sink += read_seq(buf, 512); });
  TIME("line stride (32 B), cold", words / 8, DC_FlushAll(), sink += read_stride(buf, words / 8, 8));
  TIME("4 KB + 32 B stride, cold", 60, DC_FlushAll(), sink += read_stride(buf, 60, 1032));
  TIME("seq read, uncached mirror", words / 4, DC_FlushAll(), sink += read_seq(unc, words / 4));
  TIME("isolated miss, word 0", words / 8, DC_FlushAll(), sink += read_isolated(buf, words / 8, 0));
  TIME("isolated miss, word 7", words / 8, DC_FlushAll(), sink += read_isolated(buf, words / 8, 7));
  TIME("  (their ALU loop alone)", words / 8, DC_FlushAll(), sink += alu_only(words / 8));
  TIME("store bursts of 8, uncached", 8 * (words / 64), DC_FlushAll(), write_bursts(buf, words / 64));
  TIME("  (their ALU loop alone)", 8 * (words / 64), DC_FlushAll(), alu_bursts(words / 64));
  TIME("seq write, lines not cached", words, DC_FlushAll(), write_seq(buf, words));
  TIME("seq write, lines cached", 512 * 32, (DC_FlushAll(), sink += read_seq(buf, 512)),
       { int k; for (k = 0; k < 32; k++) write_seq(buf, 512); });
}

/* ---------------------------------------------------------------- where the set goes */

/* sd:/dscheevos-lab/place.txt picks where the core and rcheevos put the
 * loaded set, through the module's own heap.c (wrapped malloc):
 *   newlib   (default) everything on newlib's heap, as before heap.c's zones
 *   session  rc_api's parse in the session area, the rest newlib's: the
 *            module before the set zones
 *   zone     the set zone in main RAM (the module without NWRAM)
 *   nwram    the set zone in NWRAM-B/C, mapped to the ARM9 at 0x03700000 as
 *            nds-bootstrap does for a DS-mode game, MPU region 3 narrowed
 *            to it and made cacheable and write-back as under the games'
 *            MPU (libnds leaves 0x03000000 uncached), restored after */
char dscm_host_session[HEAP_SESSION_HEAD + HEAP_ARENA_BYTES] __attribute__((aligned(32)));
char dscm_host_main_zone[1u << 20] __attribute__((aligned(32)));
uint32_t dscm_host_main_zone_size;
enum { PLACE_NEWLIB, PLACE_SESSION, PLACE_ZONE, PLACE_NWRAM };
static int place = PLACE_NEWLIB;
static const char* place_name[] = { "newlib", "session", "zone", "nwram" };
static u32 mpu3_saved, dc_saved, wb_saved;

#define MBK_REG(n) (*(vu32*)(0x04004040 + 4 * ((n) - 1)))
#define LAB_NWRAM 0x03700000u
#define LAB_NWRAM_SIZE 0x80000u

static u32 __attribute__((target("arm"), noinline)) cp15_read(int which)
{
  u32 v = 0;
  if (which == 0) __asm__ volatile("mrc p15, 0, %0, c6, c3, 0" : "=r"(v));
  if (which == 1) __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(v));
  if (which == 2) __asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(v));
  return v;
}
static void __attribute__((target("arm"), noinline)) cp15_region3(u32 region, u32 dc, u32 wb)
{
  DC_FlushAll();
  __asm__ volatile("mcr p15, 0, %0, c6, c3, 0\n mcr p15, 0, %1, c2, c0, 0\n mcr p15, 0, %2, c3, c0, 0"
                   :: "r"(region), "r"(dc), "r"(wb) : "memory");
  DC_InvalidateAll();
}

/* NWRAM-B/C to the ARM9 at 0x03700000-0x0377FFFF (C, then B), checked by
 * writing every 32 KB slot. Returns 0 if the console won't give it. */
static int nwram_take(void)
{
  u32 a;
  MBK_REG(2) = 0x9094989C; MBK_REG(3) = 0x8084888C;   /* B: slots 7..0, ARM9 */
  MBK_REG(4) = 0x9094989C; MBK_REG(5) = 0x8084888C;   /* C */
  MBK_REG(7) = 0x07C03740;                            /* B at 0x03740000 */
  MBK_REG(8) = 0x07403700;                            /* C at 0x03700000 */
  for (a = LAB_NWRAM; a < LAB_NWRAM + LAB_NWRAM_SIZE; a += 0x8000) *(vu32*)a = a ^ 0xA5A55A5A;
  for (a = LAB_NWRAM; a < LAB_NWRAM + LAB_NWRAM_SIZE; a += 0x8000)
    if (*(vu32*)a != (a ^ 0xA5A55A5A)) {
      say("NWRAM: not ours (MBK2 %08lx MBK7 %08lx MBK8 %08lx)", (unsigned long)MBK_REG(2),
          (unsigned long)MBK_REG(7), (unsigned long)MBK_REG(8));
      return 0;
    }
  mpu3_saved = cp15_read(0);
  dc_saved = cp15_read(1);
  wb_saved = cp15_read(2);
  cp15_region3(LAB_NWRAM | (18u << 1) | 1u, dc_saved | 0x08, wb_saved | 0x08);   /* 512 KB, cached, write-back */
  return 1;
}
static void nwram_give_back(void)
{
  cp15_region3(mpu3_saved, dc_saved, wb_saved);
}

static void place_begin(void)
{
  dscm_host_main_zone_size = place == PLACE_ZONE ? sizeof(dscm_host_main_zone) : 0;
  if (place == PLACE_NWRAM) {
    if (nwram_take()) {
      heap_zone_nwram((void*)LAB_NWRAM, LAB_NWRAM_SIZE);
      dscm_host_main_zone_size = sizeof(dscm_host_main_zone);   /* what NWRAM can't hold */
    }
    else {
      place = PLACE_ZONE;
      dscm_host_main_zone_size = sizeof(dscm_host_main_zone);
    }
  }
  if (place != PLACE_NEWLIB)
    heap_load_begin(0);
}

static void place_end(void)
{
  if (place == PLACE_NEWLIB)
    return;
  heap_load_end();
  say("placed (%s): parse %lu B in the session area, %lu on the heap; zones: nwram %lu B (+%lu holes), main %lu B (+%lu), missed %lu",
      place_name[place], (unsigned long)(dscm_g_heap.parse_top - dscm_g_heap.parse_base), (unsigned long)dscm_g_heap.parse_heap,
      (unsigned long)dscm_g_heap.zone_used[0], (unsigned long)dscm_g_heap.zone_holes[0],
      (unsigned long)dscm_g_heap.zone_used[1], (unsigned long)dscm_g_heap.zone_holes[1], (unsigned long)dscm_g_heap.zone_missed);
}

/* ---------------------------------------------------------------- the core on the typical set */

static u8* ram;                       /* 4 MB fake game RAM */
static const char* set_json;          /* the set: sd:/dscheevos-lab/set.json, else the typical one */
static int ram_dumped;                /* ram (and the DTCM) came from sd:/dscheevos-lab/ram.bin */

/* a whole file from the SD card, NUL-terminated; NULL if missing */
static char* load_file(const char* path, u32* size)
{
  FILE* f = fopen(path, "rb");
  char* b;
  long n;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  b = (char*)malloc(n + 1);
  if (b && fread(b, 1, n, f) == (size_t)n) b[n] = 0;
  else { free(b); b = NULL; }
  fclose(f);
  if (size) *size = (u32)n;
  return b;
}
static u8 dtcm_fake[0x4000];
static u32 rng = 12345;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static u32 platform_read_memory(u32 address, u8* buffer, u32 num_bytes)
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
static void memory_map(const u8** main_ram, const u8** dtcm_base) { *main_ram = ram; *dtcm_base = dtcm_fake; }
static u32 frames;
static u32 millis(void) { return frames * 1000u / 60u; }
static u32 frame(void) { return frames; }
static u32 unix_time(void) { return 1700000000u + frames / 60u; }

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
    body = set_json;
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
  for (i = 0; i < n; i++) {
    u32 len = strlen(req[i].body);
    heap_transient(len + 1);             /* the core's copy of the reply: as the module, not set data */
    dsc_http_done(req[i].handle, 200, req[i].body, len);
    heap_transient(0);
  }
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
    ram[rnd() & 0x3FFFFF] = (u8)rnd();
  for (k = 0; k < n / 16; k++)
    dtcm_fake[rnd() & 0x3FFF] = (u8)rnd();
}

#define FRAMES 60
static u32 cap_cold[FRAMES], idle_cold[FRAMES], cap_warm[FRAMES], idle_warm[FRAMES];

static void report(const char* label, u32* v, int n)
{
  u32 sum = 0;
  int i;
  for (i = 0; i < n; i++) sum += v[i];
  qsort(v, n, sizeof(u32), cmp_u32);
  say("%-18s median %7lu  p10 %7lu  max %7lu  mean %7lu cycles", label, (unsigned long)v[n / 2],
      (unsigned long)v[n / 10], (unsigned long)v[n - 1], (unsigned long)(sum / n));
}

static int core_tests(void)
{
  dsc_platform_t platform;
  dsc_config_t config;
  dsc_stats_t s;
  int i;

  if (!ram) ram = (u8*)memalign(32, 0x400000);
  if (!ram) { say("FAIL no memory for the fake RAM"); return -1; }
  if (!ram_dumped) {
    u32* w = (u32*)ram;
    u32 x = rng;
#define NEXT (x ^= x << 13, x ^= x >> 17, x ^= x << 5, (x & 7) == 0 ? 0x02000000u | (x & 0x3FFFFCu) : x)
    for (i = 0; i < 0x400000 / 4; i += 8) {
      w[i] = NEXT; w[i + 1] = NEXT; w[i + 2] = NEXT; w[i + 3] = NEXT;
      w[i + 4] = NEXT; w[i + 5] = NEXT; w[i + 6] = NEXT; w[i + 7] = NEXT;
    }
#undef NEXT
    rng = x;
  }
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
  config.snapshot_bytes = 256 * 1024;
  config.capture_at_halt = 1;
  platform.shared_alloc = heap_shared;
  place_begin();
  heap_arena_window(1);                  /* as the module: the snapshot arena in the session area */
  i = dsc_init(&platform, &config);
  heap_arena_window(0);
  if (i != 0 || dsc_begin_session("0123456789ABCDEF0123456789ABCDEF") != 0) {
    say("FAIL init");
    return -1;
  }
  for (i = 0; i < 16 && !dsc_is_active(); i++) {
    answer_requests();
    dsc_idle();
  }
  place_end();
  if (!dsc_is_active()) { say("FAIL set not loaded"); return -1; }
  dsc_get_stats(&s);
  say("%s set, %s RAM: %lu addresses", set_json == patch_json ? "typical" : "SD", ram_dumped ? "dumped" : "random",
      (unsigned long)s.addresses);

  frames++;
  dsc_vblank();
  dsc_idle();
  /* as perf.c's first half: capture, VBlank, idle every frame */
  for (i = 0; i < 2 * FRAMES; i++) {
    int cold = i < FRAMES;
    u32 t0, t1, t2, cap, ime;
    frames++;
    change_memory(512);
    ime = REG_IME;
    REG_IME = 0;
    if (cold) { DC_FlushAll(); IC_InvalidateAll(); }
    t0 = now();
    dsc_capture();
    cap = (now() - t0) * 2;
    REG_IME = ime;
    dsc_vblank();
    answer_requests();
    ime = REG_IME;
    REG_IME = 0;
    if (cold) { DC_FlushAll(); IC_InvalidateAll(); }
    t1 = now();
    dsc_idle();
    t2 = now();
    REG_IME = ime;
    if (cold) { cap_cold[i] = cap; idle_cold[i] = (t2 - t1) * 2; }
    else { cap_warm[i - FRAMES] = cap; idle_warm[i - FRAMES] = (t2 - t1) * 2; }
  }
  report("capture, cold", cap_cold, FRAMES);
  report("idle (eval), cold", idle_cold, FRAMES);
  report("capture, warm", cap_warm, FRAMES);
  report("idle (eval), warm", idle_warm, FRAMES);
  dsc_get_stats(&s);
  say("evaluated %lu, dropped %lu", (unsigned long)s.frames_evaluated, (unsigned long)s.frames_dropped);
  if (place == PLACE_NWRAM)
    nwram_give_back();
  return 0;
}

int main(void)
{
  int ok;
  consoleDemoInit();
  if (!isDSiMode()) {
    iprintf("cpi-lab needs DSi mode:\nstart it from TWiLight Menu++\nin DSi mode.\n");
    for (;;) swiWaitForVBlank();
  }
  ok = fatInitDefault();
  set_json = patch_json;
  if (ok) {
    u32 n;
    char* j = load_file("sd:/dscheevos-lab/set.json", NULL);
    char* pl = load_file("sd:/dscheevos-lab/place.txt", NULL);
    if (pl) {
      int k;
      for (k = 0; k < 4; k++)
        if (!strncmp(pl, place_name[k], strlen(place_name[k]))) place = k;
      free(pl);
    }
    char* r = load_file("sd:/dscheevos-lab/ram.bin", &n);
    if (j) set_json = j;
    if (r && n >= 0x400000) {
      ram = (u8*)memalign(32, 0x400000);
      if (ram) {
        memcpy(ram, r, 0x400000);
        if (n >= 0x404000) memcpy(dtcm_fake, r + 0x400000, 0x4000);
        ram_dumped = 1;
      }
    }
    free(r);
  }
  say("cpi-lab: ARM9 timing (DSi mode, ARM9 at 67 MHz)");
  REG_SCFG_CLK9 &= ~1;                     /* 67 MHz, as a DS-mode game */
  { int k; for (k = 0; k < 8; k++) asm volatile("nop"); }
  say("SCFG_CLK %04x", REG_SCFG_CLK9);
  cpuStartTiming(0);
  buf = (u32*)memalign(32, BUF_BYTES);
  if (!buf) say("FAIL no memory for the buffer");
  else if (set_json == patch_json) memory_tests();   /* a set from the SD: the core only */
  core_tests();
  say("DONE");
  if (ok) flush_log();
  iprintf("%s\n", ok ? "log written" : "no SD: log not written");
  for (;;) swiWaitForVBlank();
}
