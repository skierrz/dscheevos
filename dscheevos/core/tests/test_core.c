/*
 * Host unit tests for the DScheevos core: real rcheevos, fake game memory,
 * fake server. Run: make -C dscheevos/core test
 */
#include "dscheevos.h"
#include "../../module/include/dscheevos_module.h"

#include "rc_runtime.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- tiny test framework ---- */
static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define RUN(t) do { printf("%s\n", #t); t(); } while (0)

/* ---- fake game memory: RA DS addresses ---- */
static uint8_t ram[0x400000];
static uint8_t dtcm[0x4000];
static uint32_t now_ms;

static int dtcm_known = 1;   /* 0: the platform can't find the game's DTCM */

/* irq_in_idle: the next live read made during dsc_idle (evaluation) first
 * runs a VBlank, as if the IRQ came in the middle of idle work. Cleared
 * when it fires, so idle can catch up. */
static int irq_in_idle, in_idle, in_irq, irq_count;
static void irq_vblank(void);

/* As the module reads: a read must be entirely inside one region. */
static uint32_t read_memory(uint32_t a, uint8_t* buf, uint32_t n)
{
  if (irq_in_idle && in_idle && !in_irq) {
    irq_in_idle = 0;
    irq_vblank();
  }
  if (a < sizeof(ram) && n <= sizeof(ram) - a) {
    memcpy(buf, ram + a, n);
    return n;
  }
  if (a >= 0x1000000 && a - 0x1000000 < sizeof(dtcm) && n <= sizeof(dtcm) - (a - 0x1000000) && dtcm_known) {
    memcpy(buf, dtcm + (a - 0x1000000), n);
    return n;
  }
  return 0;
}

/* The platform's memory map, or none (use_map 0): dsc_vblank then reads
 * through read_memory. Tests of the snapshot run both ways. */
static int use_map = 1;
static void memory_map(const uint8_t** main_ram, const uint8_t** dtcm_base)
{
  *main_ram = ram;
  *dtcm_base = dtcm_known ? dtcm : NULL;
}
static void put32(uint32_t a, uint32_t v) { memcpy(&ram[a], &v, 4); }
static uint32_t millis(void) { return now_ms; }

/* ---- capture at halt (config.capture_at_halt) ----
 * halt_mode: every dsc_vblank() in the tests is the module's pair: a
 * dsc_capture() when the game halts, then the VBlank. With chaos (the
 * tests that compare with rcheevos), a frame also gets, at random: a
 * capture of other memory first (the game ran again before halting), a
 * capture stopped part-way for an interrupt, or no capture at all when
 * memory is the same as at the last one (a lag frame). None of these may
 * change any result. */
static int halt_mode, chaos;
static uint32_t idle_budget;   /* config.idle_max_frames */
static int stop_in = -1;      /* polls until the fake interrupt; -1: none */
static int vblank_in = -1;    /* polls until a VBlank comes in the middle of a copy; -1: none */
static int vblank_fired;
static uint32_t chaos_rng = 1;
static uint32_t chaos_rnd(void) { chaos_rng ^= chaos_rng << 13; chaos_rng ^= chaos_rng >> 17; chaos_rng ^= chaos_rng << 5; return chaos_rng; }
static int interrupt_pending(void)
{
  if (vblank_in >= 0 && vblank_in-- == 0) {
    (dsc_vblank)();   /* the IRQ the copy lets run is this frame's VBlank */
    vblank_fired = 1;
    return 0;
  }
  if (stop_in < 0)
    return 0;
  return stop_in-- == 0;
}
static uint8_t seen_ram[0x400000], seen_dtcm[0x4000];
static int seen_valid, chaos_stale, chaos_stops, chaos_extra, chaos_vblank_in_copy;

/* bytes the chaos capture scrambles and puts back */
static void scramble(uint8_t* saved, uint32_t* where, int n)
{
  int i;
  for (i = 0; i < n; i++) {
    uint32_t r = chaos_rnd();
    where[i] = r % 3 == 0 ? 0x3fffc0 + r % 0x40 : r % 3 == 1 ? 0x1000000 + (r >> 8) % 0x200 : (r >> 8) % 0x1200;
    saved[i] = where[i] >= 0x1000000 ? dtcm[where[i] - 0x1000000] : ram[where[i]];
    if (where[i] >= 0x1000000) dtcm[where[i] - 0x1000000] = (uint8_t)(r >> 16);
    else ram[where[i]] = (uint8_t)(r >> 16);
  }
}
static void unscramble(const uint8_t* saved, const uint32_t* where, int n)
{
  while (n--) {
    if (where[n] >= 0x1000000) dtcm[where[n] - 0x1000000] = saved[n];
    else ram[where[n]] = saved[n];
  }
}

static void test_vblank(void)
{
  if (halt_mode) {
    if (chaos && seen_valid && chaos_rnd() % 3 == 0 && !memcmp(ram, seen_ram, sizeof ram) && !memcmp(dtcm, seen_dtcm, sizeof dtcm)) {
      if (chaos_rnd() % 2) {
        chaos_stale++;    /* the game didn't halt this frame */
      }
      else {
        /* the VBlank comes while a copy lets an IRQ run */
        vblank_fired = 0;
        vblank_in = (int)(chaos_rnd() % 12);
        dsc_capture();
        vblank_in = -1;
        if (vblank_fired) {
          chaos_vblank_in_copy++;
          return;
        }
      }
    }
    else {
      if (chaos && chaos_rnd() % 3 == 0) {
        uint8_t saved[24];
        uint32_t where[24];
        scramble(saved, where, 24);
        dsc_capture();
        unscramble(saved, where, 24);
        chaos_extra++;
      }
      if (chaos && chaos_rnd() % 3 == 0) {
        stop_in = (int)(chaos_rnd() % 12);
        if (dsc_capture() == 1)
          chaos_stops++;
        stop_in = -1;
      }
      while (dsc_capture() == 1)
        ;
      if (chaos) {
        memcpy(seen_ram, ram, sizeof ram);
        memcpy(seen_dtcm, dtcm, sizeof dtcm);
        seen_valid = 1;
      }
    }
  }
  (dsc_vblank)();
}
#define dsc_vblank() test_vblank()

/* ---- fake server ---- */
#define SET_HEAD \
  "{\"Success\":true,\"GameId\":77,\"Title\":\"Test Game\",\"ConsoleId\":18,\"ImageIconUrl\":\"\"," \
  "\"RichPresenceGameId\":77,\"RichPresencePatch\":\"\",\"Sets\":[{\"AchievementSetId\":7,\"GameId\":77," \
  "\"Title\":null,\"Type\":\"core\",\"ImageIconUrl\":\"\",\"Achievements\":["
#define SET_ACHIEVEMENTS \
"{\"ID\":1001,\"Title\":\"Byte\",\"Description\":\"byte 0x10 is 5\",\"Flags\":3,\"Points\":5,\"MemAddr\":\"0xH000010=5\",\"Author\":\"t\",\"BadgeName\":\"1\",\"Created\":1,\"Modified\":1}," \
  "{\"ID\":1002,\"Title\":\"Pointer\",\"Description\":\"pointer target +4 is 7\",\"Flags\":3,\"Points\":10,\"MemAddr\":\"I:0xX000100&33554431_0xH000004=7\",\"Author\":\"t\",\"BadgeName\":\"2\",\"Created\":1,\"Modified\":1}," \
  "{\"ID\":1003,\"Title\":\"DTCM\",\"Description\":\"DTCM byte 0x10 is 9\",\"Flags\":3,\"Points\":25,\"MemAddr\":\"0xH1000010=9\",\"Author\":\"t\",\"BadgeName\":\"3\",\"Created\":1,\"Modified\":1}," \
  "{\"ID\":1004,\"Title\":\"Steady\",\"Description\":\"byte 0x20 is 3 for 100 frames\",\"Flags\":3,\"Points\":1,\"MemAddr\":\"0xH000020=3.100.\",\"Author\":\"t\",\"BadgeName\":\"4\",\"Created\":1,\"Modified\":1}"
#define SET_TAIL "],\"Leaderboards\":[]}]}"
/* ABI v6 tests: measured, challenge, typed, long-titled and unsupported achievements, and a leaderboard */
#define LONG_TITLE "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\xc3\xa9"   /* 46 bytes, then a 2-byte character */
#define SET_V6 \
  ",{\"ID\":1005,\"Title\":\"Measured\",\"Description\":\"collect 10\",\"Flags\":3,\"Points\":2,\"MemAddr\":\"M:0xH000030>=10\",\"Type\":\"missable\",\"Author\":\"t\",\"BadgeName\":\"5\",\"Created\":1,\"Modified\":1}" \
  ",{\"ID\":1006,\"Title\":\"Challenge\",\"Description\":\"0x41 while 0x40\",\"Flags\":3,\"Points\":3,\"MemAddr\":\"0xH000040=1_T:0xH000041=1\",\"Type\":\"progression\",\"Author\":\"t\",\"BadgeName\":\"6\",\"Created\":1,\"Modified\":1}" \
  ",{\"ID\":1007,\"Title\":\"" LONG_TITLE "\",\"Description\":\"never\",\"Flags\":3,\"Points\":4,\"MemAddr\":\"0xH000050=99\",\"Type\":\"win_condition\",\"Author\":\"t\",\"BadgeName\":\"7\",\"Created\":1,\"Modified\":1}" \
  ",{\"ID\":1008,\"Title\":\"Unsupported\",\"Description\":\"bad address\",\"Flags\":3,\"Points\":5,\"MemAddr\":\"0xH800000=1\",\"Author\":\"t\",\"BadgeName\":\"8\",\"Created\":1,\"Modified\":1}"
#define SET_V6_TAIL "],\"Leaderboards\":[{\"ID\":2001,\"Title\":\"Fast\",\"Description\":\"d\",\"Mem\":\"STA:0xH000060=1::CAN:0xH000060=2::SUB:0xH000060=3::VAL:0xH000061\",\"Format\":\"VALUE\",\"LowerIsBetter\":false,\"Hidden\":false}]}]}"
/* what retroachievements.org adds for a client it doesn't recognise */
#define SET_WARNING ",{\"ID\":101000001,\"Title\":\"Warning: Unsupported Emulator\",\"Description\":\"Hardcore unlocks cannot be earned using this emulator.\",\"Flags\":3,\"Points\":0,\"MemAddr\":\"1=1.300.\",\"Author\":\"\",\"BadgeName\":\"00000\",\"Created\":1,\"Modified\":1}"
static const char* set_json = SET_HEAD SET_ACHIEVEMENTS SET_TAIL;
static const char* set_json_warning = SET_HEAD SET_ACHIEVEMENTS SET_WARNING SET_TAIL;
static const char* set_json_v6 = SET_HEAD SET_ACHIEVEMENTS SET_V6 SET_V6_TAIL;
static int serve_warning, serve_v6;
static const char* serve_set;             /* a test's own set, if set */
static const char* session_unlocks = "";  /* startsession "Unlocks" (softcore) entries */

enum { AWARD_OK, AWARD_ALREADY, AWARD_REFUSED };
static int award_reply;
static int lb_ids[8], lb_scores[8], lb_count;
static int lb_defer;                      /* hold submitlbentry replies until answered by hand */
static void* lb_handles[8];

static int server_down;
static int awards[16], award_hardcore[16], award_count;
static int hardcore_session;

static const char* param(const char* post, const char* key, char* out, size_t outlen)
{
  char k[16];
  const char* p;
  snprintf(k, sizeof k, "%s=", key);
  for (p = post; p && *p; ) {
    if (strncmp(p, k, strlen(k)) == 0) {
      size_t n = strcspn(p + strlen(k), "&");
      if (n >= outlen) n = outlen - 1;
      memcpy(out, p + strlen(k), n);
      out[n] = 0;
      return out;
    }
    p = strchr(p, '&');
    if (p) p++;
  }
  return NULL;
}

static void http_send(const dsc_http_request_t* req)
{
  char r[32] = "", v[32];
  const char* body = "{\"Success\":true}";
  char buf[320];
  int status = 200;
  const char* post = req->post_data ? req->post_data : "";

  if (server_down) {
    dsc_http_done(req->handle, 503, "Service Unavailable", 19);
    return;
  }
  param(post, "r", r, sizeof r);
  if (!strcmp(r, "login2")) body = "{\"Success\":true,\"User\":\"tester\",\"Token\":\"tok\",\"Score\":0,\"Messages\":0}";
  else if (!strcmp(r, "achievementsets")) body = serve_set ? serve_set : serve_v6 ? set_json_v6 : serve_warning ? set_json_warning : set_json;
  else if (!strcmp(r, "startsession")) {
    hardcore_session = param(post, "h", v, sizeof v) && !strcmp(v, "1");
    snprintf(buf, sizeof buf, "{\"Success\":true,\"Unlocks\":[%s],\"HardcoreUnlocks\":[]}", session_unlocks);
    body = buf;
  }
  else if (!strcmp(r, "awardachievement")) {
    int id = param(post, "a", v, sizeof v) ? atoi(v) : 0;
    if (award_count < 16) {
      awards[award_count] = id;
      award_hardcore[award_count] = param(post, "h", v, sizeof v) && !strcmp(v, "1");
      award_count++;
    }
    if (award_reply == AWARD_REFUSED) {
      status = 404;
      body = "{\"Success\":false,\"Error\":\"Achievement not found\",\"Code\":\"not_found\"}";
    }
    else {
      /* real replies; an already unlocked one is a success with an error message */
      snprintf(buf, sizeof buf, "{\"Success\":true,\"Score\":5,\"SoftcoreScore\":0,\"AchievementID\":%d,\"AchievementsRemaining\":%d%s}",
        id, award_count >= 4 ? 0 : 4 - award_count,
        award_reply == AWARD_ALREADY ? ",\"Error\":\"User already has this achievement unlocked in hardcore mode.\"" : "");
      body = buf;
    }
  }
  else if (!strcmp(r, "submitlbentry")) {
    int score = param(post, "s", v, sizeof v) ? atoi(v) : 0;
    if (lb_count < 8) {
      lb_ids[lb_count] = param(post, "i", v, sizeof v) ? atoi(v) : 0;
      lb_scores[lb_count] = score;
      lb_handles[lb_count++] = req->handle;
    }
    if (lb_defer)
      return;
    snprintf(buf, sizeof buf, "{\"Success\":true,\"Response\":{\"Score\":%d,\"BestScore\":%d,\"RankInfo\":{\"Rank\":2,\"NumEntries\":\"7\"},\"TopEntries\":[]}}", score, score);
    body = buf;
  }
  dsc_http_done(req->handle, status, body, strlen(body));
}

/* ---- events ---- */
static int unlocked[8], unlock_count, loaded, degraded;
#define DIFF_MAX 256               /* achievements in a comparison with rcheevos (see compare_with_rcheevos) */
#define DIFF_BASE 5000
static uint32_t core_unlock_frame[DIFF_MAX];
static void on_event(const dsc_event_t* e)
{
  if (e->type == DSC_EVENT_UNLOCK && e->id >= DIFF_BASE && e->id < DIFF_BASE + DIFF_MAX) {
    dsc_stats_t s;
    dsc_get_stats(&s);    /* frames are evaluated in order: this is the next one */
    core_unlock_frame[e->id - DIFF_BASE] = s.frames_evaluated + 1;
  }
  if (e->type == DSC_EVENT_UNLOCK && unlock_count < 8) unlocked[unlock_count++] = (int)e->id;
  if (e->type == DSC_EVENT_GAME_LOADED) loaded = 1;
  if (e->type == DSC_EVENT_DEGRADED) degraded++;
}
static int was_unlocked(int id)
{
  int i;
  for (i = 0; i < unlock_count; i++) if (unlocked[i] == id) return 1;
  return 0;
}

/* ---- ABI v6 outputs ---- */
static dscm_journal_t journal;
static dscm_notices_t notices;
static uint32_t fake_time;
static uint32_t unix_time(void) { return fake_time; }

/* Every published list must be complete, and the one published before must
 * not change while the next is built. */
static dscm_list_t* published;
static int publish_count, publish_bad;
static uint8_t published_copy[sizeof(dscm_list_t) + 16 * sizeof(dscm_list_entry_t)];
static size_t published_size;
static void publish_list(dscm_list_t* l)
{
  uint32_t i;
  if (published && memcmp(published, published_copy, published_size)) publish_bad++;
  if (l && (l->magic != DSCM_LIST_MAGIC || l->entry_size != sizeof(dscm_list_entry_t) || l == published)) publish_bad++;
  for (i = 0; l && i < l->count; i++)
    if (!l->e[i].id || !l->e[i].title[0]) publish_bad++;
  published = l;
  publish_count += l != NULL;
  published_size = l ? sizeof(dscm_list_t) + l->count * sizeof(dscm_list_entry_t) : 0;
  if (published_size > sizeof(published_copy)) { publish_bad++; published_size = 0; }
  if (l) memcpy(published_copy, l, published_size);
}
static const dscm_list_entry_t* list_entry(uint32_t id)
{
  uint32_t i;
  for (i = 0; published && i < published->count; i++)
    if (published->e[i].id == id) return &published->e[i];
  return NULL;
}
/* newest notice of a kind still in the ring, or NULL */
static const dscm_notice_t* find_notice(int kind)
{
  uint32_t seq;
  for (seq = notices.head; seq > 0 && seq + DSCM_NOTICE_RING > notices.head; seq--) {
    const dscm_notice_t* n = &notices.ring[seq % DSCM_NOTICE_RING];
    if (n->seq == seq && n->kind == kind) return n;
  }
  return NULL;
}
static uint32_t journal_unsent(void)
{
  uint32_t i, n = 0;
  for (i = 0; i < journal.count; i++) if (journal.e[i].kind && !journal.e[i].sent) n++;
  return n;
}

/* ---- helpers ---- */
static int table_full_logs;
static void dbg_log(const char* m)
{
  if (strstr(m, "snapshot table full"))
    table_full_logs++;
  if (getenv("DSC_LOG"))
    printf("LOG %s\n", m);
}
static uint32_t max_addresses = 1024;    /* config.max_addresses for start_with */
static int spectator;
static int clock_utc;                     /* platform.clock_is_utc for start_with */
static int clock_bits;                    /* platform.clock (DSCM_CLOCK_*) for start_with */
static uint32_t (*platform_frame)(void);
static void (*platform_journal_sync)(const void*, uint32_t);
static void start_with(int hardcore, uint32_t slots, uint32_t bytes)
{
  dsc_platform_t p = { read_memory, millis, http_send, on_event, dbg_log, unix_time, &journal, &notices, publish_list, platform_frame,
                       use_map ? memory_map : NULL, interrupt_pending, platform_journal_sync, clock_utc, clock_bits };
  dsc_config_t c = { "http://mock", "tester", "tok", hardcore, max_addresses, slots, bytes, spectator, halt_mode, idle_budget };
  seen_valid = 0;
  table_full_logs = 0;
  memset(ram, 0, sizeof ram);
  memset(dtcm, 0, sizeof dtcm);
  memset(&journal, 0xEE, sizeof journal);   /* the core must clear it */
  memset(&notices, 0xEE, sizeof notices);
  put32(0x100, 0x02000020);  /* pointer -> 0x20, target byte at 0x24 */
  published = NULL;
  published_size = 0;
  publish_count = publish_bad = lb_count = 0;
  fake_time = 1700000000;
  unlock_count = award_count = loaded = degraded = server_down = hardcore_session = 0;
  now_ms = 1000;
  CHECK(dsc_init(&p, &c) == 0);
  CHECK(dsc_begin_session("0123456789abcdef0123456789abcdef") == 0);
  dsc_idle(); /* finishes login, set and session requests */
  dsc_idle();
  CHECK(loaded);
  CHECK(dsc_is_active());
}
static void start(int hardcore) { start_with(hardcore, 0, 0); }

/* one game frame: VBlank snapshot, then the game changes memory, then idle time */
static void frame(void (*after_vblank)(void))
{
  dsc_vblank();
  if (after_vblank) after_vblank();
  now_ms += 17;
  dsc_idle();
}
static void frames(int n) { while (n--) frame(NULL); }

/* ---- tests ---- */
static void set_byte_5(void) { ram[0x10] = 5; }
static void clear_byte(void) { ram[0x10] = 0; }

static void test_session_loads_hardcore(void)
{
  start(1);
  CHECK(hardcore_session == 1);
  dsc_shutdown();
}

static void test_set_addresses_snapshotted_from_the_start(void)
{
  dsc_stats_t s;
  start(1);
  dsc_get_stats(&s);
  CHECK(s.addresses == 1 + 1 + 4 + 1); /* bytes 0x10 and 0x20, pointer (4), DTCM byte: known at load */
  frames(8);
  dsc_get_stats(&s);
  CHECK(s.live_reads == 0);            /* everything comes from snapshots */
  CHECK(s.frames_evaluated == 8);
  CHECK(s.frames_dropped == 0);
  dsc_shutdown();
}

static void test_first_queued_frames_use_their_own_memory(void)
{
  int i;
  start_with(1, 1024, 0);
  for (i = 0; i < 60; i++) {   /* the game loads right after the session starts: no idle time */
    ram[0x10] = (i == 20) ? 5 : 0;
    dsc_vblank();
  }
  dsc_idle();
  CHECK(was_unlocked(1001));   /* true only at VBlank 20, before any frame was evaluated */
  dsc_shutdown();
}

static void test_queued_frames_follow_moved_pointers(void)
{
  dsc_stats_t s;
  int i;
  start_with(1, 1024, 0);
  frames(3);
  put32(0x100, 0x02000040);    /* pointer moves; its target 0x44 is 7 at this VBlank only */
  ram[0x44] = 7;
  dsc_vblank();
  ram[0x44] = 0;
  for (i = 0; i < 30; i++)     /* still no idle time */
    dsc_vblank();
  dsc_idle();
  CHECK(was_unlocked(1002));
  dsc_get_stats(&s);
  CHECK(s.live_reads == 0);
  dsc_shutdown();
}

static void test_queued_frames_ignore_later_memory(void)
{
  int i;
  start_with(1, 1024, 0);
  frames(3);
  put32(0x100, 0x02000040);    /* pointer moves to 0x40 (target 0x44 is 0) ... */
  dsc_vblank();
  put32(0x100, 0x02000020);    /* ... and back before the next VBlank */
  ram[0x44] = 7;               /* 0x44 becomes 7 once nothing points at it */
  for (i = 0; i < 30; i++)
    dsc_vblank();
  dsc_idle();
  CHECK(!was_unlocked(1002));  /* a later value must never leak into an earlier frame */
  dsc_shutdown();
}

static void test_evaluates_frozen_snapshot(void)
{
  start(1);
  frames(3);                 /* all conditions false */
  ram[0x10] = 5;             /* true at VBlank ... */
  frame(clear_byte);         /* ... but the game clears it before idle time */
  CHECK(was_unlocked(1001)); /* evaluation saw the VBlank value */
  dsc_shutdown();
}

static void test_ignores_changes_after_vblank(void)
{
  start(1);
  frames(3);
  frame(set_byte_5);         /* becomes true only after VBlank */
  CHECK(!was_unlocked(1001));
  frame(NULL);               /* next VBlank captures it */
  CHECK(was_unlocked(1001));
  dsc_shutdown();
}

static void test_pointer_chain_follows_moved_pointer(void)
{
  dsc_stats_t s;
  start(1);
  frames(3);
  put32(0x100, 0x02000040);  /* pointer moves to 0x40; target +4 = 0x44 */
  ram[0x44] = 7;
  frame(NULL);               /* the new target is captured at VBlank */
  dsc_get_stats(&s);
  CHECK(s.live_reads == 0);
  CHECK(was_unlocked(1002));
  dsc_shutdown();
}

static void test_dtcm(void)
{
  start(1);
  frames(3);
  dtcm[0x10] = 9;
  frame(NULL);
  CHECK(was_unlocked(1003));
  dsc_shutdown();
}

static void test_unlock_sent_in_hardcore(void)
{
  start(1);
  frames(3);
  ram[0x10] = 5;
  frames(2);
  CHECK(award_count == 1);
  CHECK(awards[0] == 1001);
  CHECK(award_hardcore[0] == 1);
  dsc_shutdown();
}

static void test_softcore_session(void)
{
  start(0);
  CHECK(hardcore_session == 0);
  frames(3);
  ram[0x10] = 5;
  frames(2);
  CHECK(award_count == 1 && award_hardcore[0] == 0);
  dsc_shutdown();
}

static void test_dropped_frames_are_reported(void)
{
  dsc_stats_t s;
  start(1);
  frames(3);
  ram[0x20] = 1; dsc_vblank();
  ram[0x20] = 2; dsc_vblank();
  ram[0x20] = 4; dsc_vblank();  /* no idle time for three changing VBlanks: one must be dropped */
  dsc_idle();
  dsc_get_stats(&s);
  CHECK(s.frames_dropped == 1);
  CHECK(degraded >= 1);
  dsc_shutdown();
}

static void test_long_stretch_without_idle_is_queued(void)
{
  dsc_stats_t s;
  int i;
  start_with(1, 1024, 0);
  frames(3);
  for (i = 0; i < 600; i++) {   /* ten seconds of VBlanks with no idle time */
    ram[0x10] = (i == 300) ? 5 : 0;
    dsc_vblank();
  }
  CHECK(!was_unlocked(1001));
  dsc_idle();                /* catch up, in order */
  dsc_get_stats(&s);
  CHECK(s.frames_dropped == 0);
  CHECK(s.frames_evaluated == s.frames_snapshotted);
  CHECK(was_unlocked(1001)); /* the one-frame value was seen */
  dsc_shutdown();
}

static void test_snapshot_arena_wraps(void)
{
  dsc_stats_t s;
  uint32_t per_frame;
  int i;
  start_with(1, 1024, halt_mode ? 160 : 128);  /* a few frames' worth: the ring wraps many times
                                                  (at halt the last snapshot is kept too) */
  frames(3);
  dsc_get_stats(&s);
  per_frame = 4 * 4 + 12;    /* 4 words (0x10, 0x20, the pointer, DTCM 0x10), then the pointer target */
  CHECK(per_frame * 4 < 128);
  for (i = 0; i < 400; i++) {
    ram[0x10] = (i == 333) ? 5 : 0;
    dsc_vblank();
    if (i % 3 == 2) dsc_idle(); /* three frames queue up each time */
  }
  dsc_idle();
  dsc_get_stats(&s);
  CHECK(s.frames_dropped == 0);
  CHECK(was_unlocked(1001));
  for (i = 0; i < 20; i++) {  /* more than 128 bytes of changing frames queued: drops, reported */
    ram[0x20] = (uint8_t)(i + 10);
    dsc_vblank();
  }
  dsc_idle();
  dsc_get_stats(&s);
  CHECK(s.frames_dropped > 0);
  CHECK(degraded >= 1);
  dsc_shutdown();
}

static void test_unchanged_frames_queue_as_repeats(void)
{
  dsc_stats_t s;
  int i;
  start_with(1, 4, 128);     /* room for only a few distinct frames */
  frames(3);
  ram[0x20] = 3;
  for (i = 0; i < 99; i++)   /* a cutscene: no idle time, nothing watched changes */
    dsc_vblank();
  dsc_idle();
  dsc_get_stats(&s);
  CHECK(s.frames_dropped == 0);
  CHECK(s.frames_repeated >= 97);
  CHECK(s.frames_evaluated == s.frames_snapshotted);
  CHECK(!was_unlocked(1004)); /* 99 of 100 frames: every repeat counted once, no more */
  dsc_vblank();
  dsc_idle();
  CHECK(was_unlocked(1004));
  dsc_shutdown();
}

static void test_unsupported_client_warning(void)
{
  dsc_progress_t p;
  start(1);
  dsc_get_progress(&p);
  CHECK(!p.hardcore_refused);    /* no warning in the set */
  dsc_shutdown();

  serve_warning = 1;
  start(1);
  dsc_get_progress(&p);
  CHECK(p.hardcore_refused);     /* the server will file hardcore unlocks as softcore */
  CHECK(p.achievements == 4);    /* the warning isn't one of the game's achievements */
  frames(310);                   /* its condition is true for 300 frames */
  CHECK(unlock_count == 0);      /* never shown as an unlock ... */
  CHECK(award_count == 0);       /* ... nor sent */
  dsc_shutdown();

  start(0);
  dsc_get_progress(&p);
  CHECK(!p.hardcore_refused);    /* softcore: nothing to refuse */
  serve_warning = 0;
  dsc_shutdown();
}

static void test_reset_and_progress(void)
{
  dsc_progress_t p;
  start(1);
  dsc_get_progress(&p);
  CHECK(p.achievements == 4 && p.unlocked == 0 && p.points == 41);
  frames(3);
  ram[0x10] = 5;
  frames(2);
  CHECK(was_unlocked(1001));
  dsc_get_progress(&p);
  CHECK(p.unlocked == 1 && p.points_unlocked == 5);
  dsc_vblank();
  dsc_vblank();
  dsc_reset();               /* queued frames dropped, state reset, unlocks kept */
  frames(2);
  dsc_get_progress(&p);
  CHECK(p.unlocked == 1);
  CHECK(dsc_is_active());
  dsc_shutdown();
}

static void test_unlock_retried_when_server_down(void)
{
  int i;
  start(1);
  frames(3);
  server_down = 1;
  ram[0x10] = 5;
  frames(2);
  CHECK(was_unlocked(1001));   /* shown to the player right away */
  CHECK(award_count == 0);
  server_down = 0;
  for (i = 0; i < 120 && award_count == 0; i++) { now_ms += 1000; frames(1); }
  CHECK(award_count == 1 && awards[0] == 1001);
  dsc_shutdown();
}

/* ---- ABI v6: journal ---- */

static void unlock_1001(void)
{
  frames(3);
  ram[0x10] = 5;
  frames(2);
}

/* The module runs the clock on UTC when the launcher measured the console
 * clock's offset: the journal says so, and its times are the platform's. */
static void test_journal_time_base_utc(void)
{
  clock_utc = 1;
  clock_bits = DSCM_CLOCK_CPU_133;    /* the session's speed-up is recorded */
  start(1);
  clock_utc = 0;
  clock_bits = 0;
  CHECK(journal.time_base == DSCM_JOURNAL_TIME_UTC);
  CHECK(journal.clock == DSCM_CLOCK_CPU_133);
  fake_time = 1700000123;
  unlock_1001();
  CHECK(journal.count == 1 && journal.e[0].unix_time == 1700000123);
}

static void test_journal_unlock_marked_sent(void)
{
  start(1);
  CHECK(journal.magic == DSCM_JOURNAL_MAGIC && journal.size == sizeof(dscm_journal_t));
  CHECK(journal.game_id == 77 && journal.count == 0);
  CHECK(!strcmp(journal.username, "tester"));
  CHECK(!strcmp(journal.game_hash, "0123456789abcdef0123456789abcdef"));
  CHECK(journal.time_base == DSCM_JOURNAL_TIME_CONSOLE);   /* clock offset unknown */
  CHECK(journal.clock == 0);
  fake_time = 1700000123;
  unlock_1001();
  CHECK(award_count == 1);
  CHECK(journal.count == 1);
  CHECK(journal.e[0].kind == DSCM_JOURNAL_AWARD && journal.e[0].id == 1001);
  CHECK(journal.e[0].hardcore == 1 && journal.e[0].unix_time == 1700000123);
  CHECK(journal.e[0].sent == 1);       /* the awardachievement reply was a success */
  dsc_shutdown();

  start(0);
  unlock_1001();
  CHECK(journal.count == 1 && journal.e[0].hardcore == 0 && journal.e[0].sent == 1);
  dsc_shutdown();
}

/* journal_sync models a cached journal: RAM (what the ARM7 copies) only
 * changes at sync points. At every sync, every counted entry is complete. */
static dscm_journal_t journal_ram;
static int journal_syncs, journal_torn;
static void sync_journal_ram(const void* addr, uint32_t len)
{
  uint32_t i;
  CHECK(addr == &journal && len == sizeof journal);
  memcpy(&journal_ram, &journal, sizeof journal);
  journal_syncs++;
  if (journal_ram.magic == DSCM_JOURNAL_MAGIC)
    for (i = 0; i < journal_ram.count && i < DSCM_JOURNAL_MAX; i++)
      if (journal_ram.e[i].kind == 0)
        journal_torn++;
}

static void test_journal_sync_points(void)
{
  int i;
  journal_syncs = journal_torn = 0;
  platform_journal_sync = sync_journal_ram;
  start(1);
  CHECK(journal_syncs > 0 && journal_ram.magic == DSCM_JOURNAL_MAGIC && journal_ram.game_id == 77);
  frames(3);
  server_down = 1;
  ram[0x10] = 5;
  frames(2);
  /* the unsent unlock is in RAM before anything else happens */
  CHECK(journal_ram.count == 1 && journal_ram.e[0].kind == DSCM_JOURNAL_AWARD && journal_ram.e[0].sent == 0);
  server_down = 0;
  for (i = 0; i < 120 && award_count == 0; i++) { now_ms += 1000; frames(1); }
  frames(1);
  CHECK(journal_ram.count == 1 && journal_ram.e[0].sent == 1);   /* the answer reached RAM too */
  CHECK(memcmp(&journal_ram, &journal, sizeof journal) == 0);
  CHECK(journal_torn == 0);
  dsc_shutdown();
  platform_journal_sync = NULL;
}

static void test_journal_unsent_until_server_answers(void)
{
  int i;
  start(1);
  frames(3);
  server_down = 1;                     /* 503: rc_client retries */
  ram[0x10] = 5;
  frames(2);
  CHECK(journal.count == 1 && journal.e[0].sent == 0);
  CHECK(find_notice(DSCM_NOTICE_OFFLINE) != NULL);
  server_down = 0;
  for (i = 0; i < 120 && award_count == 0; i++) { now_ms += 1000; frames(1); }
  frames(1);
  CHECK(award_count == 1);
  CHECK(journal.count == 1 && journal.e[0].sent == 1);
  CHECK(find_notice(DSCM_NOTICE_ONLINE) != NULL);
  dsc_shutdown();
}

static void test_journal_answers_that_are_final(void)
{
  start(1);
  award_reply = AWARD_ALREADY;         /* success with an error message: counts as sent */
  unlock_1001();
  CHECK(award_count == 1 && journal.count == 1 && journal.e[0].sent == 1);
  dsc_shutdown();

  start(1);
  award_reply = AWARD_REFUSED;         /* 404 with an error: rc_client won't retry, so it's final */
  unlock_1001();
  CHECK(award_count == 1 && journal.count == 1 && journal.e[0].sent == 1);
  award_reply = AWARD_OK;
  dsc_shutdown();
}

static void test_journal_full(void)
{
  uint32_t i;
  start(1);
  frames(3);
  /* a full journal, all answered; entry 7 is the oldest */
  journal.count = DSCM_JOURNAL_MAX;
  for (i = 0; i < DSCM_JOURNAL_MAX; i++) {
    journal.e[i].kind = DSCM_JOURNAL_AWARD;
    journal.e[i].id = 500 + i;
    journal.e[i].sent = 1;
    journal.e[i].unix_time = 1600000000 + (i == 7 ? 0 : 10 + i);
  }
  ram[0x10] = 5;
  frames(2);
  CHECK(journal.count == DSCM_JOURNAL_MAX);
  CHECK(journal.e[7].id == 1001 && journal.e[7].sent == 1);
  dsc_stats_t st;
  dsc_get_stats(&st);
  CHECK(st.journal_lost == 0);

  /* nothing answered: the oldest unsent entry is lost, and counted */
  for (i = 0; i < DSCM_JOURNAL_MAX; i++) {
    journal.e[i].sent = 0;
    journal.e[i].unix_time = 1600000000 + (i == 30 ? 0 : 10 + i);
  }
  server_down = 1;
  dtcm[0x10] = 9;
  frames(2);
  CHECK(was_unlocked(1003));
  CHECK(journal.count == DSCM_JOURNAL_MAX);
  CHECK(journal.e[30].id == 1003 && journal.e[30].sent == 0 && journal.e[30].kind == DSCM_JOURNAL_AWARD);
  dsc_get_stats(&st);
  CHECK(st.journal_lost == 1);
  CHECK(journal_unsent() == DSCM_JOURNAL_MAX);
  dsc_shutdown();
}

static void test_journal_leaderboard(void)
{
  const dscm_notice_t* n;
  int i;
  serve_v6 = 1;
  start(1);
  frames(3);
  ram[0x61] = 42;
  ram[0x60] = 1;                       /* start */
  frames(2);
  CHECK(find_notice(DSCM_NOTICE_LEADERBOARD_STARTED) && find_notice(DSCM_NOTICE_LEADERBOARD_STARTED)->id == 2001);
  for (i = 0; i < DSCM_INDICATOR_MAX && !notices.trackers[i].id; i++) {}
  CHECK(i < DSCM_INDICATOR_MAX && !strcmp(notices.trackers[i].text, "42"));
  ram[0x61] = 43;
  frames(2);
  CHECK(i < DSCM_INDICATOR_MAX && !strcmp(notices.trackers[i].text, "43"));
  ram[0x60] = 3;                       /* submit */
  frames(2);
  CHECK(lb_count == 1 && lb_ids[0] == 2001 && lb_scores[0] == 43);
  CHECK(journal.count == 1 && journal.e[0].kind == DSCM_JOURNAL_LEADERBOARD);
  CHECK(journal.e[0].id == 2001 && journal.e[0].score == 43 && journal.e[0].sent == 1);
  n = find_notice(DSCM_NOTICE_LEADERBOARD_SUBMITTED);
  CHECK(n && n->id == 2001 && n->value == 43 && !strcmp(n->text, "Fast") && !strcmp(n->detail, "43"));
  n = find_notice(DSCM_NOTICE_LEADERBOARD_RANK);
  CHECK(n && n->id == 2001 && n->value == 2 && !strcmp(n->detail, "2/7"));
  for (i = 0; i < DSCM_INDICATOR_MAX; i++) CHECK(notices.trackers[i].id == 0);

  ram[0x60] = 0;
  frames(2);
  ram[0x60] = 1;                       /* start again, then cancel */
  frames(2);
  ram[0x60] = 2;
  frames(2);
  n = find_notice(DSCM_NOTICE_LEADERBOARD_FAILED);
  CHECK(n && n->id == 2001 && !strcmp(n->text, "Fast"));
  CHECK(journal.count == 1);
  serve_v6 = 0;
  dsc_shutdown();
}

/* Two entries for one board in flight, answered out of order. */
static void lb_reply(int n)
{
  char buf[200];
  snprintf(buf, sizeof buf, "{\"Success\":true,\"Response\":{\"Score\":%d,\"BestScore\":%d,\"RankInfo\":{\"Rank\":2,\"NumEntries\":\"7\"},\"TopEntries\":[]}}",
    lb_scores[n], lb_scores[n]);
  dsc_http_done(lb_handles[n], 200, buf, strlen(buf));
}
static void test_journal_leaderboard_out_of_order(void)
{
  serve_v6 = 1;
  lb_defer = 1;
  start(1);
  frames(3);
  ram[0x61] = 150; ram[0x60] = 1; frames(2);
  ram[0x60] = 3; frames(2);
  ram[0x60] = 0; frames(2);
  ram[0x61] = 140; ram[0x60] = 1; frames(2);
  ram[0x60] = 3; frames(2);
  CHECK(lb_count == 2 && lb_scores[0] == 150 && lb_scores[1] == 140);
  CHECK(journal.count == 2 && journal_unsent() == 2);
  lb_reply(1);                         /* the second one answers first */
  frames(1);
  CHECK(journal.e[0].score == 150 && journal.e[0].sent == 0);
  CHECK(journal.e[1].score == 140 && journal.e[1].sent == 1);
  lb_reply(0);
  frames(1);
  CHECK(journal_unsent() == 0);
  lb_defer = 0;
  serve_v6 = 0;
  dsc_shutdown();
}

/* ---- ABI v6: spectator ---- */

static void test_spectator_sends_nothing(void)
{
  spectator = 1;
  serve_v6 = 1;
  start(1);
  unlock_1001();
  ram[0x61] = 5; ram[0x60] = 1; frames(2);
  ram[0x60] = 3; frames(2);
  frames(40);
  CHECK(was_unlocked(1001));           /* shown ... */
  CHECK(award_count == 0 && lb_count == 0);   /* ... but nothing sent */
  CHECK(journal.count == 0);           /* and nothing left for the launcher to send */
  CHECK(find_notice(DSCM_NOTICE_UNLOCK) != NULL);
  CHECK(list_entry(1001) && list_entry(1001)->state == DSCM_ACH_UNLOCKED);
  spectator = 0;
  serve_v6 = 0;
  dsc_shutdown();
}

/* ---- ABI v6: achievement list ---- */

static void test_list_states(void)
{
  const dscm_list_entry_t* e;
  serve_v6 = 1;
  session_unlocks = "{\"ID\":1002,\"When\":1600000000}";
  start(1);                            /* hardcore; 1002 unlocked earlier in softcore */
  CHECK(publish_count == 1 && published && !publish_bad);
  CHECK(published->count == 8 && published->generation == 1);
  CHECK(published->e[0].id == 1001 && published->e[7].id == 1008);   /* the set's order */
  e = list_entry(1001);
  CHECK(e && e->state == DSCM_ACH_LOCKED && e->points == 5 && e->unlock_time == 0);
  CHECK(e && !strcmp(e->title, "Byte") && !strcmp(e->description, "byte 0x10 is 5"));
  e = list_entry(1002);
  CHECK(e && e->state == DSCM_ACH_UNLOCKED_SOFTCORE && e->unlock_time == 1600000000);
  CHECK(list_entry(1005) && list_entry(1005)->flags == DSCM_ACH_FLAG_MISSABLE);
  CHECK(list_entry(1006) && list_entry(1006)->flags == DSCM_ACH_FLAG_PROGRESSION);
  e = list_entry(1007);
  CHECK(e && e->flags == DSCM_ACH_FLAG_WIN && strlen(e->title) == 46);   /* cut before the 2-byte character */
  CHECK(list_entry(1008) && list_entry(1008)->state == DSCM_ACH_DISABLED);
  {
    const dscm_notice_t* n = find_notice(DSCM_NOTICE_SESSION_START);
    CHECK(n && n->seq == 1 && n->value == 1 && n->id == 77);
    CHECK(n && !strcmp(n->text, "Test Game") && !strcmp(n->detail, "0/8 achievements"));
  }
  unlock_1001();
  frames(40);
  e = list_entry(1001);
  CHECK(e && e->state == DSCM_ACH_UNLOCKED && e->unlock_time != 0);
  CHECK(!publish_bad);
  dsc_shutdown();
  CHECK(published == NULL);            /* unpublished before the buffers are freed */

  start(0);                            /* softcore: the softcore unlock counts */
  CHECK(list_entry(1002) && list_entry(1002)->state == DSCM_ACH_UNLOCKED);
  CHECK(find_notice(DSCM_NOTICE_SESSION_START) && find_notice(DSCM_NOTICE_SESSION_START)->value == 0);
  CHECK(!strcmp(find_notice(DSCM_NOTICE_SESSION_START)->detail, "1/8 achievements"));
  session_unlocks = "";
  serve_v6 = 0;
  dsc_shutdown();
}

static void test_list_measured_and_progress(void)
{
  const dscm_list_entry_t* e;
  const dscm_notice_t* n;
  serve_v6 = 1;
  start(1);
  frames(3);
  ram[0x30] = 3;
  frames(40);
  e = list_entry(1005);
  CHECK(e && (e->flags & DSCM_ACH_FLAG_MEASURED) && !strcmp(e->measured, "3/10"));
  n = find_notice(DSCM_NOTICE_PROGRESS);
  CHECK(n && n->id == 1005 && !strcmp(n->text, "Measured") && !strcmp(n->detail, "3/10"));
  ram[0x30] = 10;
  frames(40);
  e = list_entry(1005);
  CHECK(e && e->state == DSCM_ACH_UNLOCKED && !(e->flags & DSCM_ACH_FLAG_MEASURED));
  CHECK(!publish_bad);
  serve_v6 = 0;
  dsc_shutdown();
}

static void test_list_rebuild_throttle(void)
{
  dsc_stats_t st;
  int i, first = 0;
  serve_v6 = 1;
  start(1);
  frames(100);                         /* nothing changes: no rebuild */
  dsc_get_stats(&st);
  CHECK(st.list_builds == 1 && publish_count == 1);
  ram[0x10] = 5;
  frame(NULL);                         /* unlock: stale, but built 101 frames ago, so now */
  CHECK(publish_count == 2);
  ram[0x30] = 4;                       /* progress: stale again ... */
  frame(NULL);
  for (i = 0; i < 40 && publish_count == 2; i++) { frame(NULL); first++; }
  CHECK(publish_count == 3);
  CHECK(first >= 28 && first <= 29);   /* ... rebuilt once 30 frames have passed */
  frames(60);
  CHECK(publish_count == 3);
  ram[0x10] = 0;
  dsc_reset();                         /* reset: stale */
  frames(31);
  CHECK(publish_count == 4);
  CHECK(!publish_bad);
  CHECK(published && published->generation == 4);
  serve_v6 = 0;
  dsc_shutdown();
}

/* ---- ABI v6: notices ---- */

static int challenge_shown(uint32_t id, const char* title)
{
  int i;
  for (i = 0; i < DSCM_INDICATOR_MAX; i++)
    if (notices.challenges[i].id == id && (!title || !strcmp(notices.challenges[i].text, title))) return 1;
  return 0;
}

static void test_challenge_indicator(void)
{
  uint32_t gen;
  serve_v6 = 1;
  start(1);
  CHECK(notices.magic == DSCM_NOTICES_MAGIC);
  frames(3);
  gen = notices.indicators_generation;
  ram[0x40] = 1;                       /* primed: everything but the trigger is true */
  frames(40);
  CHECK(challenge_shown(1006, "Challenge"));
  CHECK(notices.indicators_generation != gen);
  CHECK(list_entry(1006) && (list_entry(1006)->flags & DSCM_ACH_FLAG_CHALLENGE));
  gen = notices.indicators_generation;
  ram[0x41] = 1;                       /* trigger */
  frames(40);
  CHECK(was_unlocked(1006));
  CHECK(!challenge_shown(1006, NULL));
  CHECK(notices.indicators_generation != gen);
  CHECK(list_entry(1006) && !(list_entry(1006)->flags & DSCM_ACH_FLAG_CHALLENGE));
  CHECK(list_entry(1006)->state == DSCM_ACH_UNLOCKED);

  /* a reset hides a showing indicator */
  dsc_shutdown();
  start(1);
  frames(3);
  ram[0x40] = 1;
  frames(3);
  CHECK(challenge_shown(1006, NULL));
  dsc_reset();
  ram[0x40] = 0;
  frames(3);
  CHECK(!challenge_shown(1006, NULL));
  serve_v6 = 0;
  dsc_shutdown();
}

/* The game is quit right after the server's answer to the last unlock:
 * rcheevos still has the mastery event pending (raised on the next frame)
 * and crashed raising it while unloading. */
static void test_shutdown_right_after_mastery(void)
{
  int f;
  start(1);
  frames(3);    /* true from the start, rcheevos waits for them to be false first */
  ram[0x10] = 5;
  ram[0x24] = 7;
  dtcm[0x10] = 9;
  ram[0x20] = 3;
  for (f = 0; f < 300 && award_count < 4; f++)
    frame(NULL);
  CHECK(award_count == 4);
  dsc_shutdown();
}

static void test_notice_unlock_and_mastery(void)
{
  const dscm_notice_t* n;
  start(1);
  unlock_1001();
  n = find_notice(DSCM_NOTICE_UNLOCK);
  CHECK(n && n->id == 1001 && n->value == 5 && !strcmp(n->text, "Byte") && !strcmp(n->detail, "byte 0x10 is 5"));
  CHECK(n && n->frame >= 4 && n->frame <= 5);   /* the frame it unlocked in */
  ram[0x24] = 7;                       /* pointer target */
  dtcm[0x10] = 9;
  ram[0x20] = 3;
  frames(110);
  CHECK(unlock_count == 4);
  n = find_notice(DSCM_NOTICE_MASTERY);
  CHECK(n && n->id == 77 && !strcmp(n->text, "Test Game"));
  dsc_shutdown();
}

static uint32_t module_frames(void) { return 1000; }
static void test_notice_uses_platform_frame(void)
{
  const dscm_notice_t* n;
  platform_frame = module_frames;
  start(1);
  unlock_1001();
  n = find_notice(DSCM_NOTICE_UNLOCK);
  CHECK(n && n->frame == 1000);
  platform_frame = NULL;
  dsc_shutdown();
}

static void test_notice_ring_wraps(void)
{
  uint32_t seq, ok = 1;
  int i;
  serve_v6 = 1;
  start(1);
  frames(3);
  for (i = 0; i < 10; i++) {           /* 20 notices: start, cancel */
    ram[0x60] = 1; frames(2);
    ram[0x60] = 2; frames(2);
    ram[0x60] = 0; frames(2);
  }
  CHECK(notices.head == 21);           /* session start + 20 */
  for (seq = notices.head - DSCM_NOTICE_RING + 1; seq <= notices.head; seq++) {
    const dscm_notice_t* n = &notices.ring[seq % DSCM_NOTICE_RING];
    int want = (seq % 2) ? DSCM_NOTICE_LEADERBOARD_FAILED : DSCM_NOTICE_LEADERBOARD_STARTED;
    if (n->seq != seq || n->kind != want || n->id != 2001) ok = 0;
  }
  CHECK(ok);
  serve_v6 = 0;
  dsc_shutdown();
}

static void test_no_outputs_is_fine(void)
{
  dsc_platform_t p = { read_memory, millis, http_send, on_event, NULL, NULL, NULL, NULL, NULL, NULL, NULL, interrupt_pending };
  dsc_config_t c = { "http://mock", "tester", "tok", 1, 1024, 0, 0, 0, halt_mode };
  memset(ram, 0, sizeof ram);
  unlock_count = award_count = loaded = 0;
  CHECK(dsc_init(&p, &c) == 0);
  CHECK(dsc_begin_session("0123456789abcdef0123456789abcdef") == 0);
  dsc_idle();
  CHECK(loaded && dsc_is_active());
  unlock_1001();
  CHECK(was_unlocked(1001) && award_count == 1);
  dsc_shutdown();
}


/* ---- snapshot copy and pointer chains, against rcheevos reading live memory ---- */

static uint32_t rng = 1;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* A set of achievements DIFF_BASE, DIFF_BASE + 1, ... with these triggers. */
static char* make_set(const char* const* triggers, int n)
{
  size_t size = 4096;
  char* out;
  int i;
  for (i = 0; i < n; i++)
    size += strlen(triggers[i]) + 160;
  out = (char*)malloc(size);
  strcpy(out, SET_HEAD);
  for (i = 0; i < n; i++)
    sprintf(out + strlen(out), "%s{\"ID\":%d,\"Title\":\"T%d\",\"Description\":\"d\",\"Flags\":3,\"Points\":1,"
      "\"MemAddr\":\"%s\",\"Author\":\"t\",\"BadgeName\":\"1\",\"Created\":1,\"Modified\":1}",
      i ? "," : "", DIFF_BASE + i, i, triggers[i]);
  strcat(out, SET_TAIL);
  return out;
}

static uint32_t ref_unlock_frame[DIFF_MAX], ref_frame;
static uint32_t RC_CCONV ref_peek(uint32_t address, uint8_t* buffer, uint32_t num_bytes, void* ud)
{
  (void)ud;
  return read_memory(address, buffer, num_bytes);
}
static void RC_CCONV ref_event(const rc_runtime_event_t* e)
{
  if (e->type == RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED && e->id >= DIFF_BASE && e->id < DIFF_BASE + DIFF_MAX &&
      !ref_unlock_frame[e->id - DIFF_BASE])
    ref_unlock_frame[e->id - DIFF_BASE] = ref_frame;
}

/* Run the set through the core (idle time every idle_every frames) and
 * through rcheevos' own runtime reading live memory at each VBlank; every
 * achievement must unlock on the same frame in both. mutate(frame, when)
 * changes memory: when 0 before the VBlank, 1 right after it (the snapshot
 * must not see that). Returns how many unlocked. In this test build the
 * core also checks each snapshot against the old copy code and rcheevos
 * (DSC_CHECK_FAST_STEPS), and aborts on any difference. */
static int compare_with_rcheevos(const char* const* triggers, int n, int frames_to_run, int idle_every,
                                 void (*mutate)(int frame, int when), dsc_stats_t* stats)
{
  rc_runtime_t ref;
  char* json = make_set(triggers, n);
  int i, f, unlocks = 0, same = 1, parsed = 0;

  memset(core_unlock_frame, 0, sizeof(core_unlock_frame));
  memset(ref_unlock_frame, 0, sizeof(ref_unlock_frame));
  serve_set = json;
  start_with(1, 1024, 0);
  serve_set = NULL;
  chaos = halt_mode;
  rc_runtime_init(&ref);
  for (i = 0; i < n; i++)
    parsed += rc_runtime_activate_achievement(&ref, DIFF_BASE + i, triggers[i], NULL, 0) == RC_OK;
  CHECK(parsed == n);
  for (f = 1; f <= frames_to_run; f++) {
    mutate(f, 0);
    dsc_vblank();
    ref_frame = f;
    rc_runtime_do_frame(&ref, ref_event, ref_peek, NULL, NULL);
    mutate(f, 1);
    if (f % idle_every == 0)
      dsc_idle();
  }
  while (dsc_idle())    /* catch up (several calls with idle_max_frames) */
    ;
  dsc_get_stats(stats);
  CHECK(stats->frames_dropped == 0);
  CHECK(stats->frames_evaluated == (uint32_t)frames_to_run);
  for (i = 0; i < n; i++) {
    /* A direct read that crosses the end of main RAM or DTCM fails, and
     * rc_client (as in RetroArch) then disables the achievement; rcheevos'
     * bare runtime used as the reference here doesn't. */
    unlocks += ref_unlock_frame[i] != 0;
    if (!strncmp(triggers[i], "0xX3ffffe", 9) || !strncmp(triggers[i], "0xX1003ffe", 10)) {
      CHECK(core_unlock_frame[i] == 0);
      continue;
    }
    if (core_unlock_frame[i] != ref_unlock_frame[i]) {
      if (same)
        printf("  achievement %d (%s): core frame %u, rcheevos frame %u\n", DIFF_BASE + i, triggers[i],
          (unsigned)core_unlock_frame[i], (unsigned)ref_unlock_frame[i]);
      same = 0;
    }
  }
  CHECK(same);
  chaos = 0;
  rc_runtime_destroy(&ref);
  dsc_shutdown();
  free(json);
  return unlocks;
}

/* Words copied singly and in blocks, unaligned values, DTCM, a block with
 * an unwatched word in it, and runs at the ends of main RAM and DTCM that
 * can't be read whole (they read as 0, as read_memory leaves them). */
static const char* const layout_triggers[] = {
  "0xH000010=1_0xH000011=2",                       /* two bytes, one word */
  "0x 000013=258",                                 /* 16-bit across a word boundary */
  "0xX000021=50462976",                            /* 32-bit, unaligned: 00 01 02 03 */
  "0xX000200=1_0xX000204=2_0xH00020c=3_0x 000210=4_0xX000214=5",  /* block, 0x208 not watched */
  "0xH1000010=3",                                  /* DTCM single */
  "0xX1000100=1_0xX1000104=2_0xX1000108=3_0xX100010c=4",  /* DTCM block */
  "0xX3ffffe=0_0xH000030=1",                       /* crosses the end of main RAM: disabled */
  "0xH3ffffc=2",                                   /* same word as the run above, readable */
  "0xX1003ffe=0_0xH000031=1",                      /* crosses the end of DTCM: disabled */
  "d0xH000040=1_0xH000040=2",                      /* delta */
};

static void layout_mutate(int frame, int when)
{
  static const uint32_t hot[] = { 0x10, 0x11, 0x13, 0x14, 0x21, 0x22, 0x23, 0x24, 0x200, 0x204, 0x208, 0x20c, 0x20d,
    0x210, 0x214, 0x3ffffc, 0x3ffffd, 0x3ffffe, 0x3fffff, 0x30, 0x31, 0x40, 0x12, 0x20 };
  static const uint32_t hot_dtcm[] = { 0x10, 0x11, 0x100, 0x104, 0x108, 0x10c, 0x10d, 0x3ffe, 0x3fff };
  int k;
  if (when == 1 && frame % 3) {    /* after the VBlank: must not be seen until the next one */
    ram[0x10] ^= 1;
    return;
  }
  if (frame > 300 && frame < 340)    /* quiet: queued frames repeat */
    return;
  for (k = 0; k < 4; k++) {
    uint32_t a = hot[rnd() % (sizeof(hot) / sizeof(hot[0]))];
    ram[a] = (uint8_t)(rnd() % 6);
  }
  dtcm[hot_dtcm[rnd() % (sizeof(hot_dtcm) / sizeof(hot_dtcm[0]))]] = (uint8_t)(rnd() % 5);
  if (frame % 50 == 7) {    /* the exact values some triggers want */
    put32(0x21, 0x03020100);
    ram[0x13] = 2; ram[0x14] = 1;
    put32(0x200, 1); put32(0x204, 2); ram[0x20c] = 3; ram[0x210] = 4; ram[0x211] = 0; put32(0x214, 5);
    memcpy(&dtcm[0x100], "\1\0\0\0\2\0\0\0\3\0\0\0\4\0\0\0", 16);
  }
}

static void test_snapshot_layouts_match_rcheevos(void)
{
  dsc_stats_t s;
  int idle, unlocks;
  for (idle = 1; idle <= 7; idle += 6) {
    rng = 99;
    unlocks = compare_with_rcheevos(layout_triggers, (int)(sizeof(layout_triggers) / sizeof(layout_triggers[0])),
      400, idle, layout_mutate, &s);
    CHECK(unlocks >= 9);
    CHECK(s.live_reads == 0);
    if (idle > 1)
      CHECK(s.frames_repeated > 0);
  }
}

/* DTCM can't be read after a while (at load rc_client would disable the
 * achievements that use it): those bytes are 0, as read_memory leaves them.
 * It goes during a quiet, queued stretch: that frame is not a repeat. */
static void lose_dtcm_mutate(int frame, int when)
{
  if (frame == 320)
    dtcm_known = 0;
  layout_mutate(frame, when);
}

static void test_snapshot_without_dtcm(void)
{
  dsc_stats_t s;
  rng = 5;
  compare_with_rcheevos(layout_triggers, (int)(sizeof(layout_triggers) / sizeof(layout_triggers[0])),
    400, 7, lose_dtcm_mutate, &s);
  dtcm_known = 1;
}

/* A frame read differently (DTCM no longer readable) is stored, not
 * counted as a repeat, even when the bytes come out the same. */
static void test_new_memory_map_is_not_a_repeat(void)
{
  dsc_stats_t s;
  start_with(1, 1024, 0);
  frames(3);
  dsc_vblank();
  dsc_vblank();                /* DTCM is all 0: a repeat */
  dtcm_known = 0;
  dsc_vblank();                /* DTCM reads 0 now too, but it wasn't read */
  dtcm_known = 1;
  dsc_get_stats(&s);
  CHECK(s.frames_repeated == (use_map ? 1u : 2u));    /* without a map, the core can't tell */
  dsc_idle();
  dsc_shutdown();
}

/* Twenty single words; while frames queue, one changes per frame, each in
 * turn: none of them is a repeat. Then nothing changes: all are. */
static void test_any_word_breaks_a_repeat(void)
{
  static char text[20][32];
  const char* triggers[20];
  char* json;
  dsc_stats_t s;
  int i;
  for (i = 0; i < 20; i++) {
    snprintf(text[i], sizeof text[i], "0xH%06x=200", 0x800 + 16 * i);
    triggers[i] = text[i];
  }
  json = make_set(triggers, 20);
  serve_set = json;
  start_with(1, 1024, 0);
  serve_set = NULL;
  frames(3);
  for (i = 0; i < 40; i++) {
    ram[0x800 + 16 * (i % 20)]++;
    dsc_vblank();
  }
  dsc_get_stats(&s);
  CHECK(s.frames_repeated == 0);
  for (i = 0; i < 10; i++)
    dsc_vblank();
  dsc_idle();
  dsc_get_stats(&s);
  CHECK(s.frames_repeated == 10);
  CHECK(s.frames_evaluated == s.frames_snapshotted);
  dsc_shutdown();
  free(json);
}

/* Pointer chains of every shape run_chains computes itself, and some it
 * leaves to rcheevos, into main RAM, DTCM, unmapped memory and across the
 * end of main RAM; pointers move between frames. */
#define SLOTS 0x1000       /* 16 pointer slots */
#define NEST 0x5000        /* pointers a slot can point at */
static const uint32_t pointers[] = {
  0x02002000, 0x02003001, 0x01000100, 0x023ffff0, 0x00500000,      /* DS pointers: & 0x1FFFFFF */
  0x00002000, 0x00003001, 0x01000101, 0x003ffff4,                  /* plain offsets */
  0x02005000, 0x02005010, 0x00000000
};
static const char* const chain_shapes[] = {
  "I:0xX%06x&33554431_0xH%04x=%u",
  "I:0xX%06x_0xH%04x=%u",
  "I:0xX%06x&4194303_0x %04x=%u",
  "I:0xX%06x&33554431_0xX%04x=%u",
  "I:0xX%06x&33554431_I:0xX%04x&33554431_0xH%04x=%u",
  "I:d0xX%06x&33554431_0xH%04x=%u",
  "I:p0xX%06x&33554431_0xH%04x=%u",
  "I:0xX%06x*1_0xH%04x=%u",
  "I:0xX%06x+16_0xH%04x=%u",
  "I:0xX%06x-33554432_0xH%04x=%u",
  "I:0xX%06x^33554432_0xH%04x=%u",
  "I:0xW%06x_0xH%04x=%u",
  "I:0x %06x_0xH%04x=%u",
  "I:0xX%06x/2_0xH%04x=%u",
  "I:0xX%06x&33554431_0xL%04x=%u",
  "I:0xX%06x&33554431_0xM%04x=1",
  "I:0xX%06x&33554431_d0xH%04x!=0xH%04x",
  "I:0xH%06x_0xH%04x=%u",
  "I:0xX%06x&33554431_I:0x %04x_0xH%04x=%u",
  "I:0xX%06x&33554431_0x %04x>%u_I:0xX%06x&33554431_0xH%04x=%u",
  "I:0xX%06x&33554431_I:d0xX%04x&33554431_0xH%04x=%u",     /* delta of a step */
  "I:0xX%06x&33554431_I:0xX%04x/1_0xH%04x=%u",             /* a step rcheevos computes, from a step */
};

static void chain_mutate(int frame, int when)
{
  int k;
  if (when == 1) {
    if (frame % 4 == 1)    /* moves right after the VBlank: the snapshot keeps the old target */
      put32(SLOTS + 4 * (rnd() % 16), pointers[rnd() % (sizeof(pointers) / sizeof(pointers[0]))]);
    return;
  }
  if (frame > 200 && frame < 260)
    return;
  for (k = 0; k < 8; k++) {
    static const uint32_t areas[] = { 0x2000, 0x3001, 0x3fffdc, 0x10, 0x5000, 0x40 };
    ram[areas[rnd() % 6] + rnd() % 0x24] = (uint8_t)(rnd() % 4);
  }
  dtcm[0x100 + rnd() % 0x24] = (uint8_t)(rnd() % 4);
  if (frame % 3 == 0)
    put32(SLOTS + 4 * (rnd() % 16), pointers[rnd() % (sizeof(pointers) / sizeof(pointers[0]))]);
  if (frame % 5 == 0)
    put32(NEST + 4 * (rnd() % 8), pointers[rnd() % 9]);
}

static void test_pointer_chains_match_rcheevos(void)
{
  enum { N = 160 };
  static char text[N][160];
  const char* triggers[N];
  dsc_stats_t s;
  int i, idle, unlocks;
  rng = 7;
  for (i = 0; i < N; i++) {
    const char* shape = chain_shapes[i % (sizeof(chain_shapes) / sizeof(chain_shapes[0]))];
    uint32_t slot = SLOTS + 4 * (rnd() % 16), off = rnd() % 0x14, off2 = rnd() % 0x10, v = rnd() % 4;
    if (strstr(shape, "0xH%06x_"))
      slot = 0x40 + rnd() % 4;    /* 8-bit pointer: a small address */
    if (strstr(shape, "%04x&") || strstr(shape, "%04x/"))  /* nested: the second pointer is in NEST */
      snprintf(text[i], sizeof text[i], shape, slot, off & ~3u, off2, v);
    else if (strstr(shape, "I:0x %04x"))
      snprintf(text[i], sizeof text[i], shape, slot, 0x10u, off2, v);
    else if (strstr(shape, "d0xH%04x!="))
      snprintf(text[i], sizeof text[i], shape, slot, off, off);
    else if (strstr(shape, ">%u_I:"))
      snprintf(text[i], sizeof text[i], shape, slot, off, v, SLOTS + 4 * (rnd() % 16), off2, v);
    else
      snprintf(text[i], sizeof text[i], shape, slot, off, v);
    if (i % 3 == 1)
      strcat(text[i], ".3.");
    triggers[i] = text[i];
  }
  for (idle = 1; idle <= 9; idle += 4) {
    rng = 11;
    use_map = idle != 5;          /* and once through read_memory */
    memset(ram + NEST, 0, 0x20);
    unlocks = compare_with_rcheevos(triggers, N, 500, idle, chain_mutate, &s);
    CHECK(unlocks > N / 2);
    /* at halt: a frame with no capture reuses the last targets, and a chain
     * on a pointer's delta then lands elsewhere and is read live */
    CHECK(s.live_reads == 0 || (halt_mode && s.frames_stale));
    if (idle > 1)
      CHECK(s.frames_repeated > 0);
  }
  use_map = 1;
}

/* Remember/Recall chains aren't copied: read live, and counted. */
static void test_recall_chain_reads_live(void)
{
  static const char* const triggers[] = { "K:0xX001000&33554431_I:{recall}_0xH000004=1" };
  char* json = make_set(triggers, 1);
  dsc_stats_t s;
  serve_set = json;
  start(1);
  serve_set = NULL;
  put32(0x1000, 0x02000020);
  frames(3);
  ram[0x24] = 1;
  frames(2);
  dsc_get_stats(&s);
  CHECK(s.live_reads > 0);
  CHECK(was_unlocked(DIFF_BASE));
  dsc_shutdown();
  free(json);
}

/* A change to an unwatched byte in a watched word is not a change. */
static void test_unwatched_bytes_still_repeat(void)
{
  dsc_stats_t s;
  int i;
  start_with(1, 1024, 0);
  frames(3);
  for (i = 0; i < 20; i++) {
    ram[0x11] = (uint8_t)i;    /* same word as 0x10, not watched */
    ram[0x103 + 0x20] = (uint8_t)i;
    dsc_vblank();
  }
  dsc_idle();
  dsc_get_stats(&s);
  CHECK(s.frames_repeated == 19);
  dsc_shutdown();
}

/* ---- review: gaps found against the code before the copy program ---- */

/* A chain's 32-bit read at 0x3ffffe fails at VBlank (it crosses the end of
 * main RAM), but a 1-byte read of 0x3ffffe is fine. A Recall chain (read
 * live at evaluation) reading that one byte must get it, as rcheevos and
 * the old code do. */
static void test_live_read_next_to_failed_chain_read(void)
{
  static const char* const triggers[] = {
    "K:0xX001000_I:{recall}_0xH000000=7",   /* 1 byte at 0x3ffffe, read live */
    "I:0xX001004_0xX000000=1",              /* 4 bytes at 0x3ffffe: fails */
  };
  char* json = make_set(triggers, 2);
  serve_set = json;
  start(1);
  serve_set = NULL;
  put32(0x1000, 0x3ffffe);
  put32(0x1004, 0x3ffffe);
  frames(3);
  ram[0x3ffffe] = 7;
  frames(3);
  CHECK(was_unlocked(DIFF_BASE));
  dsc_shutdown();
  free(json);
}

/* Default arena (snapshot_bytes 0 = slots * max_addresses): a set of
 * scattered single bytes must still fit, as it did with one byte per
 * address. */
static void test_scattered_set_fits_default_arena(void)
{
  enum { T = 6, PER = 100 };
  static char text[T][PER * 16];
  const char* triggers[T];
  char* json;
  dsc_stats_t s;
  int i, k;
  for (i = 0; i < T; i++) {
    text[i][0] = 0;
    for (k = 0; k < PER; k++)
      sprintf(text[i] + strlen(text[i]), "%s0xH%06x=9", k ? "_" : "", 0x10000 + 4 * (i * PER + k));
    triggers[i] = text[i];
  }
  json = make_set(triggers, T);
  serve_set = json;
  start_with(1, 2, 0);       /* 2 slots, 2 * 1024 bytes */
  serve_set = NULL;
  frames(10);
  dsc_get_stats(&s);
  CHECK(s.addresses == T * PER);
  CHECK(s.frames_dropped == 0);
  CHECK(s.frames_evaluated == s.frames_snapshotted);
  dsc_shutdown();
  free(json);
}

/* Tables are sized to the set: one far bigger than the old fixed tables
 * (1024 addresses here) is snapshotted whole, with no live reads. */
static void test_table_sized_to_set(void)
{
  enum { T = 30, PER = 100 };
  static char text[T][PER * 16];
  const char* triggers[T];
  char* json;
  dsc_stats_t s;
  int i, k;
  for (i = 0; i < T; i++) {
    text[i][0] = 0;
    for (k = 0; k < PER; k++)
      sprintf(text[i] + strlen(text[i]), "%s0xH%06x=9", k ? "_" : "", 0x10000 + 3 * (i * PER + k));
    triggers[i] = text[i];
  }
  json = make_set(triggers, T);
  max_addresses = 4096;
  serve_set = json;
  start_with(1, 64, 64 * 1024);
  serve_set = NULL;
  ram[0x10000] = 9;
  frames(5);
  dsc_get_stats(&s);
  CHECK(s.addresses == T * PER);
  CHECK(s.live_reads == 0);
  CHECK(s.frames_dropped == 0);
  CHECK(table_full_logs == 0);
  dsc_shutdown();
  max_addresses = 1024;
  free(json);
}

/* A set that reads more addresses than max_addresses: the table holds the
 * first ones, the rest are read live at evaluation (logged once, counted).
 * With memory changed only before each VBlank, every achievement unlocks
 * on the same frame as in rcheevos reading live memory, chains included. */
static void overflow_mutate(int frame, int when)
{
  int k;
  if (when == 1)
    return;
  for (k = 0; k < 6; k++)
    ram[0x2000 + rnd() % 0x40] = (uint8_t)(rnd() % 3);
  if (frame % 4 == 0)
    put32(0x1000 + 4 * (rnd() % 2), 0x02002000 + 4 * (rnd() % 8));
}
static void test_table_overflow_reads_live(void)
{
  enum { N = 24 };
  static char text[N][96];
  const char* triggers[N];
  dsc_stats_t s;
  int i, unlocks, pass;
  rng = 5;
  for (i = 0; i < N; i++) {
    uint32_t a = 0x2000 + (rnd() % 0x40);
    switch (i % 4) {
      case 0: snprintf(text[i], sizeof text[i], "0xH%06x=1_0xH%06x=2", a, 0x2000 + (rnd() % 0x40)); break;
      case 1: snprintf(text[i], sizeof text[i], "0x %06x=257", a & ~1u); break;
      case 2: snprintf(text[i], sizeof text[i], "I:0xX%06x&33554431_0xH%04x=2", 0x1000 + 4 * (rnd() % 2), rnd() % 0x10); break;
      default: snprintf(text[i], sizeof text[i], "0xH%06x=2.2._d0xH%06x=1", a, a); break;
    }
    triggers[i] = text[i];
  }
  for (pass = 0; pass < 2; pass++) {
    use_map = pass;
    max_addresses = 12;
    rng = 9;
    unlocks = compare_with_rcheevos(triggers, N, 300, 1, overflow_mutate, &s);
    CHECK(unlocks > N / 2);
    CHECK(s.addresses == 12);
    CHECK(s.live_reads > 0);
    CHECK(table_full_logs == 1);
  }
  max_addresses = 1024;
  use_map = 1;
}

/* Tables republished while frames are queued: Recall chains read live, so
 * each evaluation adds addresses (until the table is full); pointers move
 * between region ends, DTCM comes and goes. The self-check
 * (DSC_CHECK_FAST_STEPS) aborts on any difference from the old copy. */
static void test_republish_while_queued(void)
{
  static const uint32_t spots[] = { 0x10, 0x3ffffc, 0x3ffffd, 0x3ffffe, 0x3fffff, 0x1000000, 0x1003ffd, 0x1003ffe,
                                    0x1003fff, 0x400000, 0xfffffe, 0x2001, 0x2002, 0x2003 };
  static const char* const triggers[] = {
    "K:0xX001000_I:{recall}_0xH000000=7",
    "K:0xX001004_I:{recall}_0x 000000=7",
    "K:0xX001008_I:{recall}_0xX000000=7",
    "K:0xX00100c_I:{recall}_0xH000001=7",
    "I:0xX001000_0xX000000=1",
    "I:0xX001004_0x 000000=1",
    "I:0xX001008_0xH000000=1",
    "I:0xX00100c&33554431_0xH000002=1",
    "0xH1000010=3_0xX000020=1",
  };
  char* json = make_set(triggers, (int)(sizeof(triggers) / sizeof(triggers[0])));
  dsc_stats_t s;
  int idle, f, pass;
  for (pass = 0; pass < 2; pass++) {
    use_map = pass;
    for (idle = 1; idle <= 9; idle += 4) {
      rng = 1234 + idle;
      serve_set = json;
      start_with(1, 1024, 0);
      serve_set = NULL;
      for (f = 0; f < 600; f++) {
        uint32_t a = (f % 7 == 0) ? spots[rnd() % (sizeof(spots) / sizeof(spots[0]))] : rnd() % 0x400000;
        if (rnd() % 3 == 0 && (a & 0xFF000000u) == 0)
          a = (rnd() & 1) ? 0x1000000 + rnd() % 0x4000 : a;
        put32(0x1000 + 4 * (rnd() % 4), a);
        if (a < 0x400000)
          ram[a] = (uint8_t)(rnd() % 8);
        if (f % 37 == 0)
          dtcm_known = !dtcm_known;
        if (f > 300 && f < 330)
          put32(0x1000, 0x10);    /* quiet: repeats */
        dsc_vblank();
        if (f % idle == 0)
          dsc_idle();
      }
      dtcm_known = 1;
      dsc_idle();
      dsc_get_stats(&s);
      CHECK(s.frames_evaluated == s.frames_snapshotted);
      CHECK(s.addresses > 100);
      dsc_shutdown();
    }
  }
  use_map = 1;
  free(json);
}

/* A VBlank that comes while idle evaluates a frame (here: in the middle of
 * a live read), with memory changed: it queues a frame, or counts a repeat
 * on the newest one, never on the one being evaluated. The self-check
 * compares every snapshot with the old copy. */
static void irq_vblank(void)
{
  uint32_t a = rnd() % 0x400000;
  in_irq = 1;
  if (rnd() % 4)
    ram[a] = (uint8_t)rnd();
  put32(0x1000 + 4 * (rnd() % 4), rnd() % 2 ? rnd() % 0x400000 : 0x10);
  dsc_vblank();
  irq_count++;
  in_irq = 0;
}
static void test_vblank_during_evaluation(void)
{
  static const char* const triggers[] = {
    "K:0xX001000_I:{recall}_0xH000000=7",
    "K:0xX001004_I:{recall}_0x 000000=7",
    "I:0xX001008&33554431_0xH000000=1",
    "I:0xX00100c_0xX000000=1",
    "0xH000020=3",
  };
  char* json = make_set(triggers, (int)(sizeof(triggers) / sizeof(triggers[0])));
  dsc_stats_t s;
  int f, pass;
  for (pass = 0; pass < 2; pass++) {
    use_map = pass;
    rng = 77 + pass;
    serve_set = json;
    start_with(1, 1024, 0);
    serve_set = NULL;
    irq_count = 0;
    for (f = 0; f < 400; f++) {
      put32(0x1000 + 4 * (rnd() % 4), rnd() % 0x400000);
      ram[0x20] = (uint8_t)(rnd() % 4);
      dsc_vblank();
      in_idle = 1;
      irq_in_idle = 1;
      if (f % 3 == 0)
        dsc_idle();
      in_idle = 0;
    }
    irq_in_idle = 0;
    in_idle = 1;
    while (dsc_idle())
      ;
    in_idle = 0;
    dsc_get_stats(&s);
    CHECK(irq_count > 100);
    CHECK(s.frames_dropped == 0);
    CHECK(s.frames_evaluated == s.frames_snapshotted);
    dsc_shutdown();
  }
  use_map = 1;
  free(json);
}

/* Random address sets (clusters, gaps, region ends, every size) with frames
 * queued and DTCM coming and going; the self-check compares each snapshot
 * with the old copy. */
static void test_random_layouts(void)
{
  static const char* const sizes[] = { "H", " ", "W", "X", "L", "U", "M", "I", "J", "G", "K" };
  static const uint32_t bases[] = { 0x0, 0x1000, 0x3fff00, 0x1000000, 0x1003f00, 0x3ffff8, 0x1003ff8 };
  enum { T = 40 };
  static char text[T][64];
  const char* triggers[T];
  dsc_stats_t s;
  int seed, i, f;
  for (seed = 1; seed <= 40; seed++) {
    char* json;
    rng = 777 + seed;
    for (i = 0; i < T; i++) {
      uint32_t a = bases[rnd() % 7] + rnd() % (seed % 3 ? 0x40 : 0x200);
      snprintf(text[i], sizeof text[i], "0x%s%06x=%u", sizes[rnd() % 11], a, (unsigned)(rnd() % 3));
      triggers[i] = text[i];
    }
    json = make_set(triggers, T);
    use_map = seed & 1;
    serve_set = json;
    start_with(1, 1024, 0);
    serve_set = NULL;
    for (f = 0; f < 120; f++) {
      int k;
      for (k = 0; k < 6; k++) {
        uint32_t a = bases[rnd() % 7] + rnd() % 0x200;
        if (a < 0x400000) ram[a] = (uint8_t)rnd();
        else if (a - 0x1000000 < 0x4000) dtcm[a - 0x1000000] = (uint8_t)rnd();
      }
      if (f % 29 == 0)
        dtcm_known = !dtcm_known;
      dsc_vblank();
      if (f % (1 + seed % 5) == 0)
        dsc_idle();
    }
    dtcm_known = 1;
    dsc_idle();
    dsc_get_stats(&s);
    CHECK(s.frames_evaluated == s.frames_snapshotted);
    dsc_shutdown();
    free(json);
  }
  use_map = 1;
}

/* More chain shapes: roots in DTCM and unaligned, 16-bit roots with a
 * mask, three levels, two ANDs, MUL and ADD before a read, shared prefixes. */
static void more_chain_mutate(int frame, int when)
{
  static const uint32_t ptrs[] = { 0x02002000, 0x02003001, 0x01000100, 0x023ffff4, 0x00500000, 0x00002000,
                                   0x01003ff0, 0x3ffff1, 0x00002002, 0x00001000 };
  int k;
  if (when == 1) {
    if (frame % 4 == 1)
      put32(0x1000 + 4 * (rnd() % 8), ptrs[rnd() % 10]);
    return;
  }
  if (frame > 200 && frame < 240)
    return;
  for (k = 0; k < 8; k++) {
    static const uint32_t areas[] = { 0x2000, 0x3000, 0x3fffe0, 0x1000, 0x500 };
    ram[areas[rnd() % 5] + rnd() % 0x20] = (uint8_t)(rnd() % 4);
  }
  dtcm[0x100 + rnd() % 0x10] = (uint8_t)(rnd() % 4);
  dtcm[0x3ff0 + rnd() % 0x10] = (uint8_t)(rnd() % 4);
  if (frame % 3 == 0)
    put32(0x1000 + 4 * (rnd() % 8), ptrs[rnd() % 10]);
  if (frame % 7 == 0) {
    uint32_t v = ptrs[rnd() % 10];
    memcpy(&dtcm[0x200 + 4 * (rnd() % 4)], &v, 4);
  }
  if (frame % 5 == 0)
    put32(0x1001 + 4 * (rnd() % 4), ptrs[rnd() % 10]);
}

static const char* const more_chain_triggers[] = {
    "I:0xX1000200&33554431_0xH0004=1",
    "I:0xX1000204_0xH0000=2",
    "I:0xX001001&33554431_0xH0002=1",
    "I:0x 001000&65535_0xH0003=1",
    "I:0x 001004_0x 0001=3",
    "I:0xX001008&33554431_I:0xX0000&33554431_I:0xX0000&33554431_0xH0001=2",
    "I:0xX00100c&33554431&4194303_0xH0002=1",
    "I:0xX001010*2_0xH0000=1",
    "I:0xX001014+4_0xX0000=1",
    "I:0xX001018&33554431_0xH0004=1_I:0xX001018&33554431_0xH0008=2",
    "I:0xX00101c&33554431_0x 0001=1_I:0xX00101c&33554431_0xX0004=0",
    "I:0xW001000&16777215_0xH0005=1",
    "I:0xX1003ffc_0xH0000=1",
    "I:0xX001000&33554431_d0xH0004=1",
    "I:0xX001004&33554431_p0x 0006=2",
    "I:0xX001008&33554431_I:d0xX0000_0xH0002=3",
};

static void test_more_chain_shapes_match_rcheevos(void)
{
  static const char* const* const triggers = more_chain_triggers;
  dsc_stats_t s;
  int idle, pass;
  for (pass = 0; pass < 2; pass++) {
    for (idle = 1; idle <= 9; idle += 8) {
      rng = 31 + idle;
      use_map = pass;
      CHECK(compare_with_rcheevos(triggers, (int)(sizeof(more_chain_triggers) / sizeof(more_chain_triggers[0])), 400, idle, more_chain_mutate, &s) >= 8);
      CHECK(s.live_reads == 0);
    }
  }
  use_map = 1;
}

/* Idle time handed out a frame at a time (idle_max_frames 1), at halt with
 * chaos: a slot's repeats are split over several dsc_idle calls while
 * VBlanks keep queueing frames behind it. */
static void test_budgeted_idle_matches_rcheevos(void)
{
  dsc_stats_t s;
  int idle, saved = halt_mode;
  halt_mode = 1;
  idle_budget = 1;
  for (idle = 1; idle <= 9; idle += 8) {
    rng = 31 + idle;
    CHECK(compare_with_rcheevos(layout_triggers, (int)(sizeof(layout_triggers) / sizeof(layout_triggers[0])), 400, idle, layout_mutate, &s) >= 8);
    rng = 31 + idle;
    CHECK(compare_with_rcheevos(more_chain_triggers, (int)(sizeof(more_chain_triggers) / sizeof(more_chain_triggers[0])), 400, idle, more_chain_mutate, &s) >= 8);
  }
  idle_budget = 0;
  halt_mode = saved;
}

static void run_all(void)
{
  for (use_map = 0; use_map < 2; use_map++) {    /* the snapshot tests, without and with a memory map */
    printf("-- memory map %s\n", use_map ? "on" : "off");
    RUN(test_set_addresses_snapshotted_from_the_start);
    RUN(test_first_queued_frames_use_their_own_memory);
    RUN(test_queued_frames_follow_moved_pointers);
    RUN(test_queued_frames_ignore_later_memory);
    RUN(test_evaluates_frozen_snapshot);
    RUN(test_ignores_changes_after_vblank);
    RUN(test_pointer_chain_follows_moved_pointer);
    RUN(test_dtcm);
    RUN(test_dropped_frames_are_reported);
    RUN(test_long_stretch_without_idle_is_queued);
    RUN(test_snapshot_arena_wraps);
    RUN(test_unchanged_frames_queue_as_repeats);
    RUN(test_unwatched_bytes_still_repeat);
    RUN(test_snapshot_layouts_match_rcheevos);
    RUN(test_snapshot_without_dtcm);
    RUN(test_any_word_breaks_a_repeat);
    RUN(test_new_memory_map_is_not_a_repeat);
    RUN(test_scattered_set_fits_default_arena);
  }
  use_map = 1;
  RUN(test_pointer_chains_match_rcheevos);
  RUN(test_recall_chain_reads_live);
  RUN(test_live_read_next_to_failed_chain_read);
  RUN(test_republish_while_queued);
  RUN(test_table_sized_to_set);
  RUN(test_table_overflow_reads_live);
  RUN(test_vblank_during_evaluation);
  RUN(test_random_layouts);
  RUN(test_more_chain_shapes_match_rcheevos);
  RUN(test_session_loads_hardcore);
  RUN(test_unlock_sent_in_hardcore);
  RUN(test_softcore_session);
  RUN(test_unsupported_client_warning);
  RUN(test_reset_and_progress);
  RUN(test_unlock_retried_when_server_down);
  RUN(test_journal_unlock_marked_sent);
  RUN(test_journal_time_base_utc);
  RUN(test_journal_unsent_until_server_answers);
  RUN(test_journal_sync_points);
  RUN(test_journal_answers_that_are_final);
  RUN(test_journal_full);
  RUN(test_journal_leaderboard);
  RUN(test_journal_leaderboard_out_of_order);
  RUN(test_spectator_sends_nothing);
  RUN(test_list_states);
  RUN(test_list_measured_and_progress);
  RUN(test_list_rebuild_throttle);
  RUN(test_challenge_indicator);
  RUN(test_notice_unlock_and_mastery);
  RUN(test_shutdown_right_after_mastery);
  RUN(test_budgeted_idle_matches_rcheevos);
  RUN(test_notice_uses_platform_frame);
  RUN(test_notice_ring_wraps);
  RUN(test_no_outputs_is_fine);
}

int main(void)
{
  run_all();
  /* again, with the snapshot taken when the game halts */
  printf("-- capture at halt\n");
  halt_mode = 1;
  run_all();
  printf("capture at halt: %d stale frames, %d stopped captures, %d replaced captures, %d VBlanks during a copy\n",
    chaos_stale, chaos_stops, chaos_extra, chaos_vblank_in_copy);
  CHECK(chaos_stale > 50 && chaos_stops > 100 && chaos_extra > 100 && chaos_vblank_in_copy > 20);
  printf("%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
