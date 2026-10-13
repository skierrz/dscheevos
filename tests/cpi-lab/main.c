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

/* ---------------------------------------------------------------- the core on the typical set */

static u8* ram;                       /* 4 MB fake game RAM */
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

  ram = (u8*)memalign(32, 0x400000);
  if (!ram) { say("FAIL no memory for the fake RAM"); return -1; }
  {
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
  if (dsc_init(&platform, &config) != 0 || dsc_begin_session("0123456789ABCDEF0123456789ABCDEF") != 0) {
    say("FAIL init");
    return -1;
  }
  for (i = 0; i < 16 && !dsc_is_active(); i++) {
    answer_requests();
    dsc_idle();
  }
  if (!dsc_is_active()) { say("FAIL set not loaded"); return -1; }
  dsc_get_stats(&s);
  say("typical set: %lu addresses", (unsigned long)s.addresses);

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
  say("cpi-lab: ARM9 timing (DSi mode, ARM9 at 67 MHz)");
  REG_SCFG_CLK9 &= ~1;                     /* 67 MHz, as a DS-mode game */
  { int k; for (k = 0; k < 8; k++) asm volatile("nop"); }
  say("SCFG_CLK %04x", REG_SCFG_CLK9);
  cpuStartTiming(0);
  buf = (u32*)memalign(32, BUF_BYTES);
  if (!buf) say("FAIL no memory for the buffer");
  else memory_tests();
  core_tests();
  say("DONE");
  if (ok) flush_log();
  iprintf("%s\n", ok ? "log written" : "no SD: log not written");
  for (;;) swiWaitForVBlank();
}
