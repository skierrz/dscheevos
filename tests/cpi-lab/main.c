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

/* Trace replay (sd:/dscheevos-lab/trace.dtrc, tests/difftest/gen.py format): the base image
 * replaces ram.bin, and each frame applies the trace's changed ranges instead of random bytes,
 * so evaluation sees real per-frame changes. Per-frame cycles go to the emulator log as
 * "@@T frame capture eval" lines. */
static u8* trace;                     /* the whole trace file, read once (no SD access while timing) */
static u32 trace_size, trace_pos;
static void report(const char* label, u32* v, int n);
static u32 trace_frames;
#define TRACE_MAX_FRAMES 4000

static u32 rd32(const u8* b) { return b[0] | b[1] << 8 | b[2] << 16 | (u32)b[3] << 24; }

static int trace_open(void)
{
  u32 main_size, dtcm_size, n;
  trace = (u8*)load_file("sd:/dscheevos-lab/trace.dtrc", &trace_size);
  if (!trace) return 0;
  if (trace_size < 24 || memcmp(trace, "DTRC1", 5)) { free(trace); trace = NULL; return 0; }
  main_size = rd32(trace + 8);
  dtcm_size = rd32(trace + 12);
  trace_frames = rd32(trace + 16);
  trace_pos = 24;
  if (!ram) ram = (u8*)memalign(32, 0x400000);
  memset(ram, 0, 0x400000);
  n = main_size < 0x400000 ? main_size : 0x400000;
  memcpy(ram, trace + trace_pos, n);
  trace_pos += main_size;
  n = dtcm_size < 0x4000 ? dtcm_size : 0x4000;
  memcpy(dtcm_fake, trace + trace_pos, n);
  trace_pos += dtcm_size;
  ram_dumped = 1;
  return 1;
}

/* applies the next frame's ranges; -1 at the end */
static int trace_step(u32* frame_no)
{
  u32 n, k, flags;
  if (trace_pos + 8 > trace_size) return -1;
  *frame_no = rd32(trace + trace_pos);
  flags = trace[trace_pos + 4];
  n = trace[trace_pos + 6] | trace[trace_pos + 7] << 8;
  trace_pos += 8;
  for (k = 0; k < n; k++) {
    u32 addr, len;
    if (trace_pos + 6 > trace_size) return -1;
    addr = rd32(trace + trace_pos);
    len = trace[trace_pos + 4] | trace[trace_pos + 5] << 8;
    trace_pos += 6;
    if (trace_pos + len > trace_size) return -1;
    if (addr < 0x400000u && addr + len <= 0x400000u) memcpy(ram + addr, trace + trace_pos, len);
    else if (addr >= 0x1000000u && addr + len <= 0x1004000u) memcpy(dtcm_fake + (addr - 0x1000000u), trace + trace_pos, len);
    trace_pos += len;
  }
  return (int)flags;
}

static void trace_run(void)
{
  static u32 cap_v[TRACE_MAX_FRAMES], eval_v[TRACE_MAX_FRAMES];
  u32 f, i = 0, cap, ev, t0, t1, ime, sum = 0;
  char m[64];
  dsc_stats_t s;
  while (i < TRACE_MAX_FRAMES && trace_step(&f) >= 0) {
    frames++;
    ime = REG_IME;
    REG_IME = 0;
    DC_FlushAll(); IC_InvalidateAll();
    t0 = now();
    dsc_capture();
    cap = (now() - t0) * 2;
    REG_IME = ime;
    dsc_vblank();
    answer_requests();
    ime = REG_IME;
    REG_IME = 0;
    DC_FlushAll(); IC_InvalidateAll();
    t0 = now();
    dsc_idle();
    t1 = now();
    REG_IME = ime;
    ev = (t1 - t0) * 2;
    cap_v[i] = cap; eval_v[i] = ev; sum += ev / 64;
    sprintf(m, "@@T %lu %lu %lu\n", (unsigned long)f, (unsigned long)cap, (unsigned long)ev);
    nocashMessage(m);
    i++;
  }
  say("trace: %lu frames (of %lu), eval mean %lu", (unsigned long)i, (unsigned long)trace_frames,
      (unsigned long)(i ? sum / i * 64 : 0));
  if (i >= 10) {
    report("capture, cold", cap_v, i);
    report("idle (eval), cold", eval_v, i);
  }
  dsc_get_stats(&s);
  say("evaluated %lu, dropped %lu", (unsigned long)s.frames_evaluated, (unsigned long)s.frames_dropped);
}


#ifdef LAB_PROFILE
/* Sampling profiler (build flag -DLAB_PROFILE): timer 2 fires every PROF_TICKS bus ticks (2x ARM9
 * cycles) only while dsc_idle runs in the cold frames; the IRQ vector is replaced for that time by a
 * handler that bumps a halfword-PC histogram. The BIOS IRQ stub pushed {r0-r3,r12,lr} before calling
 * the vector, so the interrupted PC + 4 is at [sp+20]. Dumped as "@@P addr count" lines. */
#define PROF_TICKS 2000
#define PROF_BASE 0x02000000u
#define PROF_BINS (0x400000u / 2)
u16* prof_hist;
u32 prof_n, prof_oor, prof_oor_v[256];
void prof_irq(void);
__asm__(".arm\n.global prof_irq\n.type prof_irq,%function\nprof_irq:\n"
        "  ldr r0, =0x04000214\n"
        "  mov r1, #0x20\n"                   /* IF bit 5 = timer 2 */
        "  str r1, [r0]\n"
        "  ldr r2, [sp, #20]\n"
        "  sub r2, r2, #4\n"
        "  ldr r3, =prof_n\n  ldr r0, [r3]\n  add r0, r0, #1\n  str r0, [r3]\n"
        "  sub r1, r2, #0x02000000\n"
        "  cmp r1, #0x400000\n"
        "  bhs 1f\n"
        "  ldr r3, =prof_hist\n  ldr r3, [r3]\n"
        "  mov r1, r1, lsr #1\n  mov r1, r1, lsl #1\n"
        "  ldrh r0, [r3, r1]\n  add r0, r0, #1\n  strh r0, [r3, r1]\n"
        "  bx lr\n"
        "1: ldr r3, =prof_oor\n  ldr r0, [r3]\n  add r1, r0, #1\n  str r1, [r3]\n"
        "  and r0, r0, #255\n  ldr r3, =prof_oor_v\n  str r2, [r3, r0, lsl #2]\n"
        "  bx lr\n  .ltorg\n");
static VoidFn prof_saved;
static u32 prof_ie;
static void prof_on(void)
{
  prof_saved = IRQ_HANDLER;
  prof_ie = REG_IE;
  IRQ_HANDLER = prof_irq;
  REG_IE = IRQ_TIMER2;
  REG_IF = ~0;
  TIMER_DATA(2) = (u16)(65536 - PROF_TICKS);
  TIMER_CR(2) = TIMER_ENABLE | TIMER_IRQ_REQ | TIMER_DIV_1;
  REG_IME = 1;
}
static void prof_off(void)
{
  REG_IME = 0;
  TIMER_CR(2) = 0;
  REG_IF = ~0;
  REG_IE = prof_ie;
  IRQ_HANDLER = prof_saved;
}
static void prof_dump(void)
{
  u32 i;
  char m[64];
  sprintf(m, "@@P samples %lu oor %lu ticks %u\n", (unsigned long)prof_n, (unsigned long)prof_oor, PROF_TICKS);
  nocashMessage(m);
  for (i = 0; i < prof_oor && i < 256; i++) {
    sprintf(m, "@@O %08lx\n", (unsigned long)prof_oor_v[i]);
    nocashMessage(m);
  }
  for (i = 0; i < PROF_BINS; i++)
    if (prof_hist[i]) {
      sprintf(m, "@@P %08lx %u\n", (unsigned long)(PROF_BASE + i * 2), prof_hist[i]);
      nocashMessage(m);
    }
}
#define PROF_ON() prof_on()
#define PROF_OFF() prof_off()
#else
#define PROF_ON() ((void)0)
#define PROF_OFF() ((void)0)
#endif

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
#ifdef LAB_PROFILE
  prof_hist = (u16*)calloc(PROF_BINS, 2);
  if (!prof_hist) { say("FAIL no memory for the profile"); return -1; }
#endif
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
  if (dsc_init(&platform, &config) != 0 || dsc_begin_session("0123456789ABCDEF0123456789ABCDEF") != 0) {
    say("FAIL init");
    return -1;
  }
  {
    u32 t0 = now(), ime;
    for (i = 0; i < 16 && !dsc_is_active(); i++) {
      answer_requests();
      ime = REG_IME;
      REG_IME = 0;
      dsc_idle();
      REG_IME = ime;
    }
    say("load: %llu cycles", (unsigned long long)(u32)(now() - t0) * 2u);
  }
  if (!dsc_is_active()) { say("FAIL set not loaded"); return -1; }
  dsc_get_stats(&s);
  say("%s set, %s RAM: %lu addresses", set_json == patch_json ? "typical" : "SD", ram_dumped ? "dumped" : "random",
      (unsigned long)s.addresses);

  frames++;
  dsc_vblank();
  dsc_idle();
  if (trace) { trace_run(); return 0; }
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
    if (cold) PROF_ON();
    dsc_idle();
    t2 = now();
    if (cold) PROF_OFF();
    REG_IME = ime;
    if (cold) { cap_cold[i] = cap; idle_cold[i] = (t2 - t1) * 2; }
    else { cap_warm[i - FRAMES] = cap; idle_warm[i - FRAMES] = (t2 - t1) * 2; }
  }
#ifdef LAB_PROFILE
  prof_dump();
#endif
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
  set_json = patch_json;
  if (ok) {
    u32 n;
    char* j = load_file("sd:/dscheevos-lab/set.json", NULL);
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
    trace_open();
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
