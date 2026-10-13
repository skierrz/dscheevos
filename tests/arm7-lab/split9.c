/*
 * arm7-lab split mode (arm7-split.nds): DSC_EVAL_CPU=split end to end in a
 * test ROM. The ARM9 runs the DScheevos core and rc_client (our rcheevos fork
 * with the trigger evaluator) over the cpi-lab set bench frames: same set,
 * RAM dump, seed and per-frame changes, dsc_capture + dsc_vblank + dsc_idle.
 * A share of the achievements (sd:/dscheevos-lab/split.txt: percent, default
 * 50; 0 = none claimed) is claimed; their triggers are evaluated by the ARM7
 * (payload in ARM7 WRAM, split7() in bench7.c) from the frame's memrefs while
 * the ARM9 evaluates the rest; rc_client takes the ARM7's results in its own
 * order.
 *
 * Coherency (the ARM7 has no cache; the ARM9's D-cache is write-back):
 * - claimed triggers are allocated on their own 32-byte lines;
 * - frame_begin: clean+invalidate BY LINE the memref storage
 *   (dsc_enum_memref_storage), the listed triggers and the mailbox's work
 *   list, then publish seq (its own line, cleaned);
 * - result(): poll the entry's result line, invalidating it each time;
 * - frame_end: wait for the ARM7's "frame done".
 * No global cache operation during frames (the lab still empties the caches
 * between frames for cold timing, as cpi-lab does).
 *
 * Per frame: ARM9 cycles in dsc_idle (total, of which waiting for the ARM7,
 * and in frame_begin's cache work), ARM7 cycles. Results on screen, @@ lines,
 * sd:/dscheevos-lab/split.txt.out.
 */
#include "dscheevos.h"
#include "dscheevos_module.h"
#include "rc_client.h"
#include "lab7.h"

#include <nds.h>
#include <fat.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <sys/stat.h>

extern const u8 payload_bin[], payload_bin_end[];

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

static char* load_file(const char* path, u32* size, u32 align, u32 min_alloc)
{
  FILE* f = fopen(path, "rb");
  char* b;
  long n;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  b = (char*)memalign(align, (u32)n + 1 > min_alloc ? (u32)n + 1 : min_alloc);
  if (b && fread(b, 1, n, f) == (size_t)n) b[n] = 0;
  else { free(b); b = NULL; }
  fclose(f);
  if (size) *size = (u32)n;
  return b;
}

static inline u32 now(void) { return cpuGetTiming(); }   /* bus ticks: 2 ARM9 cycles */

/* ---------------------------------------------------------------- the trigger evaluator (ARM9 glue) */

static lab7_split_t* M;
static u32 g_pct = 50, g_seq, g_next, g_claimed, g_claimed_bytes;
static u32 g_wait, g_begin;               /* this frame: ticks waiting for the ARM7, in frame_begin */

#define MAX_CLAIMED 4096
static struct { void* p; u32 size; } g_trig[MAX_CLAIMED];
static u32 g_ntrig;
#define MAX_RANGES 64
static struct { const void* p; u32 size; } g_mem[MAX_RANGES];
static u32 g_nmem, g_mem_bytes;

static u32 mix32(u32 x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }

/* the LAST pct% of the core set's achievements in rc_client's order: the ARM9
 * evaluates its share first while the ARM7 works on these, then takes their
 * results (sd:/dscheevos-lab/split.txt: "pct", or "pct hash" for a scattered
 * choice) */
static u32 g_index, g_total, g_hash;
static int claim(uint32_t id, void* ud)
{
  (void)ud;
  g_index++;
  if (g_hash ? (mix32(id) % 100 >= g_pct) : (g_index <= g_total - (g_total * g_pct + 50) / 100)) return 0;
  g_claimed++;
  return 1;
}

static void* alloc_trigger(uint32_t id, size_t size, void* ud)
{
  u32 n = ((u32)size + 31) & ~31u;
  void* p;
  (void)id; (void)ud;
  if (g_ntrig >= MAX_CLAIMED) return NULL;
  p = memalign(32, n);
  if (!p) return NULL;
  g_trig[g_ntrig].p = p;
  g_trig[g_ntrig].size = n;
  g_ntrig++;
  g_claimed_bytes += n;
  return p;
}

static void frame_begin(const rc_client_claimed_trigger_t* work, uint32_t count, void* ud)
{
  u32 t0 = now(), i;
  (void)ud;
  if (count > LAB7_SPLIT_MAX) count = LAB7_SPLIT_MAX;   /* the bench's sets fit */
  for (i = 0; i < count; i++) {
    M->work[i].id = work[i].achievement_id;
    M->work[i].trigger = work[i].trigger;
  }
  /* claimed triggers: the ARM9 only ever READS them during frames (rc_client
   * writes them at lifecycle points, cleaned there), so no per-frame flush;
   * result() drops the header line it read before the ARM7 wrote it */
  M->count = count;
  for (i = 0; i < g_nmem; i++)
    DC_FlushRange(g_mem[i].p, g_mem[i].size);           /* the frame's memref values */
  DC_FlushRange((void*)&M->count, 32);
  DC_FlushRange(M->work, ((count * sizeof(lab7_work_t)) + 31) & ~31u);
  g_seq++;
  M->seq = g_seq;
  DC_FlushRange((void*)&M->seq, 32);                     /* publish (drains the write buffer) */
  g_next = 0;
  g_begin += now() - t0;
}

static int result(uint32_t id, void* trigger, void* ud)
{
  u32 i = g_next++, t0 = now();
  (void)id; (void)ud;
  for (;;) {
    DC_InvalidateRange((void*)&M->result[i], 32);
    if (M->result[i].seq == g_seq) break;
  }
  g_wait += now() - t0;
  /* rc_client reads state and measured values from the trigger's header next */
  DC_InvalidateRange(trigger, 32);
  return M->result[i].state;
}

static void frame_end(void* ud)
{
  u32 t0 = now();
  (void)ud;
  for (;;) {
    DC_InvalidateRange((void*)&M->count, 32);
    if (M->arm7_done == g_seq) break;
  }
  g_wait += now() - t0;
}

static rc_client_trigger_evaluator_t g_evaluator = { claim, alloc_trigger, frame_begin, result, frame_end, NULL };

static void note_memrefs(const void* p, size_t size, void* ud)
{
  (void)ud;
  if (g_nmem < MAX_RANGES) {
    g_mem[g_nmem].p = p;
    g_mem[g_nmem].size = (u32)size;
    g_nmem++;
    g_mem_bytes += (u32)size;
  }
}

/* ---------------------------------------------------------------- the platform, as tests/cpi-lab */

static u8* ram;
static u8 dtcm_fake[0x4000];
static const char* set_json;
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
static void on_event(const dsc_event_t* e)
{
  if (e->type == DSC_EVENT_UNLOCK) say("event: unlock %lu at frame %lu", (unsigned long)e->id, (unsigned long)frames);
}
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

static int cmp_u32(const void* a, const void* b)
{
  u32 x = *(const u32*)a, y = *(const u32*)b;
  return x < y ? -1 : x > y;
}
#define FRAMES 60
static u32 r_idle[FRAMES], r_wait[FRAMES], r_begin[FRAMES], r_arm7[FRAMES], r_cap[FRAMES];
static void report(const char* label, u32* v, int n)
{
  u32 sum = 0;
  int i;
  for (i = 0; i < n; i++) sum += v[i];
  qsort(v, n, sizeof(u32), cmp_u32);
  say("%-22s median %8lu  p10 %8lu  max %8lu  mean %8lu", label, (unsigned long)v[n / 2],
      (unsigned long)v[n / 10], (unsigned long)v[n - 1], (unsigned long)(sum / n));
}

static lab7_params_t params __attribute__((aligned(32)));

int main(void)
{
  dsc_platform_t platform;
  dsc_config_t config;
  dsc_stats_t s;
  int ok, i;
  u32 n = 0;
  char* r;

  consoleDemoInit();
  if (!isDSiMode()) { iprintf("needs DSi mode\n"); for (;;) swiWaitForVBlank(); }
  ok = fatInitDefault();
  if (!ok) { say("FAIL no SD"); for (;;) swiWaitForVBlank(); }
  set_json = load_file("sd:/dscheevos-lab/set.json", NULL, 4, 0);
  r = load_file("sd:/dscheevos-lab/ram.bin", &n, 32, 0x404000);
  if (!set_json || !r || n < 0x400000) { say("FAIL set.json + ram.bin needed"); for (;;) swiWaitForVBlank(); }
  ram = (u8*)r;
  if (n >= 0x404000) memcpy(dtcm_fake, r + 0x400000, 0x4000);
  {
    FILE* f = fopen("sd:/dscheevos-lab/split.txt", "r");
    if (f) { int k, h = 0; if (fscanf(f, "%d %d", &k, &h) >= 1 && k >= 0 && k <= 100) g_pct = (u32)k; g_hash = (u32)h; fclose(f); }
  }
  { const char* q = set_json; while ((q = strstr(q, "\"Flags\":3")) != NULL) { g_total++; q += 9; } }
  say("arm7-lab split: %lu%% of achievements claimed for the ARM7", (unsigned long)g_pct);
  REG_SCFG_CLK &= ~1;                     /* ARM9 at 67 MHz, as a DS-mode game */
  { int k; for (k = 0; k < 8; k++) asm volatile("nop"); }
  cpuStartTiming(0);

  /* the ARM7 serves the mailbox */
  M = (lab7_split_t*)memalign(32, sizeof(lab7_split_t));
  if (!M || (u32)M + sizeof(*M) > LAB7_PAYLOAD_BASE) { say("FAIL mailbox"); for (;;) swiWaitForVBlank(); }
  memset(M, 0, sizeof(*M));
  memset(&params, 0, sizeof(params));
  params.split = M;
  params.heap_end = LAB7_HEAP_END;
  memcpy((void*)LAB7_PAYLOAD_BASE, payload_bin, payload_bin_end - payload_bin);
  DC_FlushAll();
  fifoSendValue32(FIFO_USER_01, (u32)&params);

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
  platform.trigger_evaluator = &g_evaluator;
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
    say("FAIL init"); for (;;) swiWaitForVBlank();
  }
  for (i = 0; i < 16 && !dsc_is_active(); i++) { answer_requests(); dsc_idle(); }
  if (!dsc_is_active()) { say("FAIL set not loaded"); for (;;) swiWaitForVBlank(); }
  dsc_enum_memref_storage(note_memrefs, NULL);
  for (i = 0; i < (int)g_ntrig; i++) DC_FlushRange(g_trig[i].p, g_trig[i].size);
  dsc_get_stats(&s);
  say("%lu addresses; claimed %lu achievements, %lu KB of triggers; memrefs %lu KB in %lu ranges",
      (unsigned long)s.addresses, (unsigned long)g_claimed, (unsigned long)(g_claimed_bytes >> 10),
      (unsigned long)(g_mem_bytes >> 10), (unsigned long)g_nmem);

  frames++;
  dsc_vblank();
  dsc_idle();
  for (i = 0; i < FRAMES; i++) {
    u32 t0, t1, t2, ime;
    frames++;
    change_memory(512);
    ime = REG_IME; REG_IME = 0;
    DC_FlushAll(); IC_InvalidateAll();     /* cold, as the game leaves the caches (lab only) */
    t0 = now();
    dsc_capture();
    r_cap[i] = (now() - t0) * 2;
    REG_IME = ime;
    dsc_vblank();
    answer_requests();
    ime = REG_IME; REG_IME = 0;
    DC_FlushAll(); IC_InvalidateAll();
    g_wait = g_begin = 0;
    t1 = now();
    dsc_idle();
    t2 = now();
    REG_IME = ime;
    r_idle[i] = (t2 - t1) * 2;
    r_wait[i] = g_wait * 2;
    r_begin[i] = g_begin * 2;
    DC_InvalidateRange((void*)&M->count, 32);
    r_arm7[i] = M->arm7_cycles;
  }
  {
    static u32 busy[FRAMES];
    for (i = 0; i < FRAMES; i++) busy[i] = r_idle[i] - r_wait[i];
    report("ARM9 idle total (cyc)", r_idle, FRAMES);
    report("ARM9 busy (cyc)", busy, FRAMES);
    report("ARM9 waiting (cyc)", r_wait, FRAMES);
    report("ARM9 frame_begin (cyc)", r_begin, FRAMES);
    report("ARM7 eval (ARM7 cyc)", r_arm7, FRAMES);
    report("ARM9 capture (cyc)", r_cap, FRAMES);
  }
  dsc_get_stats(&s);
  say("evaluated %lu, dropped %lu", (unsigned long)s.frames_evaluated, (unsigned long)s.frames_dropped);
  M->quit = 1;
  DC_FlushRange((void*)&M->seq, 32);
  say("DONE");
  {
    FILE* f;
    mkdir("sd:/dscheevos-lab", 0777);
    f = fopen("sd:/dscheevos-lab/split.txt.out", "w");
    if (f) { fwrite(logbuf, 1, loglen, f); fclose(f); }
  }
  for (;;) swiWaitForVBlank();
}
