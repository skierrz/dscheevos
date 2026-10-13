/*
 * Per-frame instruction counts for real RetroAchievements sets
 * (tests/perf/realsets.sh), built with the module's flags and run under
 * qemu-arm -cpu arm946 with a counting plugin. The plugin counts every guest
 * instruction between mark_begin and mark_end and prints one "PHASE n count"
 * line per pair, in order.
 *
 *   realsets.elf core|rc <set.json> [frames] [bytes changed per frame] [ram dump|-] [captures per frame]
 *
 * core: through the DScheevos core as module.c drives it. Per frame:
 *       dsc_capture (counted), dsc_vblank, dsc_idle (counted). With more
 *       captures per frame (the game's idle thread wakes many times a frame),
 *       the counted phase holds all of them, on the same memory.
 * rc:   rc_client_do_frame alone (counted), reading the fake RAM directly.
 *
 * The set file is an achievementsets reply as the server sends it. Without
 * a RAM dump, game RAM is random with one word in eight shaped like a DS
 * pointer (as perf.c); a dump is 4 MB of main RAM, optionally followed by
 * 16 KB of DTCM.
 *
 * Both modes also print an evaluation digest (every memref value, condition
 * hit count, trigger, leaderboard and rich presence state after each frame,
 * taken outside the counted calls) and newlib heap figures (mallinfo: bytes in
 * use after load and at the end, and the heap's high-water mark). With the
 * same set, frames and RAM, the core's digest must equal rc_client's own.
 */
#include "dscheevos.h"
#include "dscheevos_module.h"
#include "rc_client.h"
#include "client/rc_client_internal.h"
#include "runtime/rc_internal.h"

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- qemu-arm's Linux syscalls (devkitARM's newlib has no files or console) ---- */
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
/* Reads a whole file; returns its size, or -1. */
static int read_file(const char* path, char** data, int extra)
{
  int fd = sys(5, (int)path, 0, 0), size = 0, n;    /* open(path, O_RDONLY) */
  char* buf;
  if (fd < 0)
    return -1;
  size = sys(19, fd, 0, 2);    /* lseek(fd, 0, SEEK_END) */
  sys(19, fd, 0, 0);
  buf = (char*)malloc((size_t)size + (size_t)extra + 1);
  if (!buf)
    return -1;
  for (n = 0; n < size;) {
    int r = sys(3, fd, (int)(buf + n), size - n);    /* read */
    if (r <= 0)
      break;
    n += r;
  }
  sys(6, fd, 0, 0);    /* close */
  buf[n] = '\0';
  *data = buf;
  return n;
}

/* ---- markers: never inlined, each a separate address ---- */
volatile int marker_sink;
void __attribute__((noinline)) mark_begin(void) { marker_sink = 1; __asm__ volatile(""); }
void __attribute__((noinline)) mark_end(void) { marker_sink = 0; __asm__ volatile(""); }

/* ---- evaluation digest (FNV-1a), outside the counted calls ---- */
/* Linked with --wrap=rc_client_do_frame: remembers the client the core made. */
void __real_rc_client_do_frame(rc_client_t* client);
static rc_client_t* seen_client;
void __wrap_rc_client_do_frame(rc_client_t* client)
{
  seen_client = client;
  __real_rc_client_do_frame(client);
}

static uint32_t digest = 2166136261u;
static void mix(uint32_t v)
{
  int i;
  for (i = 0; i < 4; i++) {
    digest ^= (v >> (i * 8)) & 0xFF;
    digest *= 16777619u;
  }
}
static void mix_condsets(const rc_condset_t* cs)
{
  for (; cs; cs = cs->next) {
    const rc_condition_t* c;
    mix(cs->is_paused);
    for (c = cs->conditions; c; c = c->next) {
      mix(c->current_hits);
      mix(c->is_true);
    }
  }
}
static void mix_trigger(const rc_trigger_t* t)
{
  if (!t)
    return;
  mix(t->state);
  mix(t->measured_value);
  mix(t->has_hits);
  mix_condsets(t->requirement);
  mix_condsets(t->alternative);
}
static void digest_frame(rc_client_t* client)
{
  const rc_client_game_info_t* g = client ? client->game : NULL;
  const rc_client_subset_info_t* sub;
  const rc_memref_list_t* ml;
  const rc_modified_memref_list_t* mml;
  char rp[256];
  uint32_t i;
  if (!g)
    return;
  for (ml = &g->runtime.memrefs->memrefs; ml; ml = ml->next)
    for (i = 0; i < ml->count; i++) {
      mix(ml->items[i].value.value);
      mix(ml->items[i].value.prior);
      mix(ml->items[i].value.changed);
      mix(ml->items[i].value.type);
    }
  for (mml = &g->runtime.memrefs->modified_memrefs; mml; mml = mml->next)
    for (i = 0; i < mml->count; i++) {
      mix(mml->items[i].memref.value.value);
      mix(mml->items[i].memref.value.prior);
      mix(mml->items[i].memref.value.changed);
    }
  for (sub = g->subsets; sub; sub = sub->next) {
    for (i = 0; i < sub->public_.num_achievements; i++) {
      const rc_client_achievement_info_t* a = &sub->achievements[i];
      mix(a->public_.state);
      mix(a->public_.unlocked);
      mix_trigger(a->trigger);
    }
    for (i = 0; i < sub->public_.num_leaderboards; i++) {
      const rc_client_leaderboard_info_t* l = &sub->leaderboards[i];
      mix(l->public_.state);
      mix((uint32_t)l->value);
      if (l->lboard) {
        mix(l->lboard->state);
        mix_trigger(&l->lboard->start);
        mix_trigger(&l->lboard->submit);
        mix_trigger(&l->lboard->cancel);
      }
    }
  }
  rp[0] = '\0';
  rc_client_get_rich_presence_message(client, rp, sizeof(rp));
  for (i = 0; rp[i]; i++)
    mix((uint8_t)rp[i]);
}

/* newlib heap: bytes in use (mallinfo), and the heap's high-water mark
 * (newlib's own record of the most memory malloc took from sbrk). */
extern unsigned long __malloc_max_sbrked_mem;
static void heap_line(const char* what)
{
  struct mallinfo mi = mallinfo();
  out("INFO heap %s used %u top %u max %u\n", what, (unsigned)mi.uordblks, (unsigned)mi.arena,
    (unsigned)__malloc_max_sbrked_mem);
}

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
static uint32_t fake_read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client)
{
  (void)client;
  return platform_read_memory(address, buffer, num_bytes);
}

static void memory_map(const uint8_t** main_ram, const uint8_t** dtcm_base)
{
  *main_ram = ram;
  *dtcm_base = dtcm;
}

static uint32_t frames;
static uint32_t millis(void) { return frames * 1000u / 60u; }
static uint32_t frame(void) { return frames; }
static uint32_t unix_time(void) { return 1700000000u + frames / 60u; }

/* ---- fake server ---- */
static char* set_json;
static const char* answer(const char* post_data)
{
  const char* p = post_data ? post_data : "";
  if (strstr(p, "r=login2"))
    return "{\"Success\":true,\"User\":\"User\",\"AvatarUrl\":\"http://server/UserPic/USER.png\",\"Token\":\"ApiToken\",\"Score\":12345,\"Messages\":2}";
  if (strstr(p, "r=achievementsets"))
    return set_json;
  if (strstr(p, "r=startsession"))
    return "{\"Success\":true,\"Unlocks\":[],\"HardcoreUnlocks\":[]}";
  if (strstr(p, "r=awardachievement"))
    return "{\"Success\":true,\"Score\":5432,\"SoftcoreScore\":777,\"AchievementID\":5,\"AchievementsRemaining\":6}";
  return "{\"Success\":true}";
}

#define MAX_REQ 16
static struct { void* handle; const char* body; } req[MAX_REQ];
static int req_count;
static void http_send(const dsc_http_request_t* r)
{
  if (req_count < MAX_REQ) {
    req[req_count].handle = r->handle;
    req[req_count].body = answer(r->post_data);
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

static void rc_server(const rc_api_request_t* r, rc_client_server_callback_t cb, void* data, rc_client_t* client)
{
  rc_api_server_response_t resp;
  (void)client;
  memset(&resp, 0, sizeof(resp));
  resp.body = answer(r->post_data);
  resp.body_length = strlen(resp.body);
  resp.http_status_code = 200;
  cb(&resp, data);
}
static void rc_done(int result, const char* message, rc_client_t* client, void* userdata)
{
  (void)client; (void)userdata;
  if (result != RC_OK)
    out("INFO rc_client callback %d %s\n", result, message ? message : "");
}
static void rc_event(const rc_client_event_t* e, rc_client_t* client) { (void)e; (void)client; }
static rc_clock_t rc_clock(const rc_client_t* client) { (void)client; return (rc_clock_t)millis(); }

static uint32_t unlocks;
static void on_event(const dsc_event_t* e)
{
  if (e->type == DSC_EVENT_UNLOCK)
    unlocks++;
}
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
    dtcm[rnd() & 0x3FFF] = (uint8_t)rnd();
}

static void fill_memory(const char* dump)
{
  uint32_t* w = (uint32_t*)ram;
  uint32_t x = rng;
  char* data;
  int i, size;
#define NEXT (x ^= x << 13, x ^= x >> 17, x ^= x << 5, (x & 7) == 0 ? 0x02000000u | (x & 0x3FFFFCu) : x)
  for (i = 0; i < (int)(sizeof(ram) / 4); i += 8) {
    w[i] = NEXT; w[i + 1] = NEXT; w[i + 2] = NEXT; w[i + 3] = NEXT;
    w[i + 4] = NEXT; w[i + 5] = NEXT; w[i + 6] = NEXT; w[i + 7] = NEXT;
  }
#undef NEXT
  rng = x;
  if (!dump)
    return;
  size = read_file(dump, &data, 0);
  if (size < (int)sizeof(ram)) {
    out("FAIL RAM dump %s\n", dump);
    quit(1);
  }
  memcpy(ram, data, sizeof(ram));
  if (size >= (int)(sizeof(ram) + sizeof(dtcm)))
    memcpy(dtcm, data + sizeof(ram), sizeof(dtcm));
  free(data);
}

static int captures = 1;
static void run_core(int n, int mutate)
{
  dsc_platform_t platform;
  dsc_config_t config;
  dsc_stats_t s;
  int i;

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
  config.idle_max_frames = 4;

  heap_line("start");
  if (dsc_init(&platform, &config) != 0 || dsc_begin_session("0123456789ABCDEF0123456789ABCDEF") != 0) {
    out("FAIL init\n");
    quit(1);
  }
  for (i = 0; i < 64 && !dsc_is_active(); i++) {
    answer_requests();
    dsc_idle();
  }
  if (!dsc_is_active()) {
    out("FAIL game not loaded\n");
    quit(1);
  }
  dsc_get_stats(&s);
  out("INFO core addresses %u\n", (unsigned)s.addresses);
  heap_line("load");

  /* the first frame is copied in the VBlank (nothing captured yet) */
  frames++;
  dsc_vblank();
  dsc_idle();
  digest_frame(seen_client);

  for (i = 0; i < n; i++) {
    frames++;
    change_memory(mutate);
    mark_begin();
    {
      int k;
      for (k = 0; k < captures; k++)
        dsc_capture();    /* the game halts at the end of its frame: the snapshot */
    }
    mark_end();
    dsc_vblank();
    answer_requests();
    mark_begin();
    dsc_idle();       /* one frame evaluated */
    mark_end();
    digest_frame(seen_client);
  }
  heap_line("end");
  out("INFO digest %08x\n", (unsigned)digest);
  dsc_get_stats(&s);
  out("INFO snapshotted %u evaluated %u repeated %u dropped %u live_reads %u unlocks %u\n",
    (unsigned)s.frames_snapshotted, (unsigned)s.frames_evaluated, (unsigned)s.frames_repeated,
    (unsigned)s.frames_dropped, (unsigned)s.live_reads, (unsigned)unlocks);
  dsc_shutdown();
}

static void run_rc(int n, int mutate)
{
  rc_client_t* c;
  int i;
  heap_line("start");
  c = rc_client_create(fake_read_memory, rc_server);
  rc_client_set_event_handler(c, rc_event);
  rc_client_set_get_time_millisecs_function(c, rc_clock);
  rc_client_set_hardcore_enabled(c, 1);
  rc_client_begin_login_with_token(c, "User", "ApiToken", rc_done, NULL);
  rc_client_begin_load_game(c, "0123456789ABCDEF0123456789ABCDEF", rc_done, NULL);
  if (!rc_client_is_game_loaded(c)) {
    out("FAIL game not loaded\n");
    quit(1);
  }
  heap_line("load");
  /* as the core's first frame (copied in the VBlank) */
  frames++;
  rc_client_do_frame(c);
  digest_frame(c);
  for (i = 0; i < n; i++) {
    frames++;
    change_memory(mutate);
    mark_begin();
    rc_client_do_frame(c);
    mark_end();
    digest_frame(c);
  }
  heap_line("end");
  out("INFO digest %08x\n", (unsigned)digest);
  rc_client_destroy(c);
}

int main(int argc, char** argv)
{
  int n = argc > 3 ? atoi(argv[3]) : 60;
  int mutate = argc > 4 ? atoi(argv[4]) : 512;
  if (argc < 3 || read_file(argv[2], &set_json, 0) < 0) {
    out("usage: realsets.elf core|rc <set.json> [frames] [bytes] [ram dump]\n");
    quit(2);
  }
  if (argc > 6)
    captures = atoi(argv[6]) > 0 ? atoi(argv[6]) : 1;
  fill_memory(argc > 5 && strcmp(argv[5], "-") ? argv[5] : NULL);
  if (!strcmp(argv[1], "rc"))
    run_rc(n, mutate);
  else
    run_core(n, mutate);
  out("DONE\n");
  quit(0);
  return 0;
}
