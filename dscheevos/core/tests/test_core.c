/*
 * Host unit tests for the DScheevos core: real rcheevos, fake game memory,
 * fake server. Run: make -C dscheevos/core test
 */
#include "dscheevos.h"

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

static uint32_t read_memory(uint32_t a, uint8_t* buf, uint32_t n)
{
  uint32_t i;
  for (i = 0; i < n; i++) {
    uint32_t x = a + i;
    if (x < sizeof(ram)) buf[i] = ram[x];
    else if (x >= 0x1000000 && x < 0x1000000 + sizeof(dtcm)) buf[i] = dtcm[x - 0x1000000];
    else return i;
  }
  return n;
}
static void put32(uint32_t a, uint32_t v) { memcpy(&ram[a], &v, 4); }
static uint32_t millis(void) { return now_ms; }

/* ---- fake server ---- */
static const char* set_json =
  "{\"Success\":true,\"GameId\":77,\"Title\":\"Test Game\",\"ConsoleId\":18,\"ImageIconUrl\":\"\","
  "\"RichPresenceGameId\":77,\"RichPresencePatch\":\"\",\"Sets\":[{\"AchievementSetId\":7,\"GameId\":77,"
  "\"Title\":null,\"Type\":\"core\",\"ImageIconUrl\":\"\",\"Achievements\":["
  "{\"ID\":1001,\"Title\":\"Byte\",\"Description\":\"byte 0x10 is 5\",\"Flags\":3,\"Points\":5,\"MemAddr\":\"0xH000010=5\",\"Author\":\"t\",\"BadgeName\":\"1\",\"Created\":1,\"Modified\":1},"
  "{\"ID\":1002,\"Title\":\"Pointer\",\"Description\":\"pointer target +4 is 7\",\"Flags\":3,\"Points\":10,\"MemAddr\":\"I:0xX000100&33554431_0xH000004=7\",\"Author\":\"t\",\"BadgeName\":\"2\",\"Created\":1,\"Modified\":1},"
  "{\"ID\":1003,\"Title\":\"DTCM\",\"Description\":\"DTCM byte 0x10 is 9\",\"Flags\":3,\"Points\":25,\"MemAddr\":\"0xH1000010=9\",\"Author\":\"t\",\"BadgeName\":\"3\",\"Created\":1,\"Modified\":1},"
  "{\"ID\":1004,\"Title\":\"Steady\",\"Description\":\"byte 0x20 is 3 for 100 frames\",\"Flags\":3,\"Points\":1,\"MemAddr\":\"0xH000020=3.100.\",\"Author\":\"t\",\"BadgeName\":\"4\",\"Created\":1,\"Modified\":1}"
  "],\"Leaderboards\":[]}]}";

static int server_down;
static int awards[8], award_hardcore[8], award_count;
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
  char buf[256];
  const char* post = req->post_data ? req->post_data : "";

  if (server_down) {
    dsc_http_done(req->handle, 503, "Service Unavailable", 19);
    return;
  }
  param(post, "r", r, sizeof r);
  if (!strcmp(r, "login2")) body = "{\"Success\":true,\"User\":\"tester\",\"Token\":\"tok\",\"Score\":0,\"Messages\":0}";
  else if (!strcmp(r, "achievementsets")) body = set_json;
  else if (!strcmp(r, "startsession")) {
    hardcore_session = param(post, "h", v, sizeof v) && !strcmp(v, "1");
    body = "{\"Success\":true,\"Unlocks\":[],\"HardcoreUnlocks\":[]}";
  }
  else if (!strcmp(r, "awardachievement")) {
    if (award_count < 8) {
      awards[award_count] = param(post, "a", v, sizeof v) ? atoi(v) : 0;
      award_hardcore[award_count] = param(post, "h", v, sizeof v) && !strcmp(v, "1");
      award_count++;
    }
    snprintf(buf, sizeof buf, "{\"Success\":true,\"Score\":5,\"SoftcoreScore\":0,\"AchievementID\":%d,\"AchievementsRemaining\":2}", awards[award_count - 1]);
    body = buf;
  }
  dsc_http_done(req->handle, 200, body, strlen(body));
}

/* ---- events ---- */
static int unlocked[8], unlock_count, loaded, degraded;
static void on_event(const dsc_event_t* e)
{
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

/* ---- helpers ---- */
static void start_with(int hardcore, uint32_t slots, uint32_t bytes)
{
  dsc_platform_t p = { read_memory, millis, http_send, on_event, NULL };
  dsc_config_t c = { "http://mock", "tester", "tok", hardcore, 1024, slots, bytes };
  memset(ram, 0, sizeof ram);
  memset(dtcm, 0, sizeof dtcm);
  put32(0x100, 0x02000020);  /* pointer -> 0x20, target byte at 0x24 */
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
  start_with(1, 1024, 128);  /* a few frames' worth: the ring wraps many times */
  frames(3);
  dsc_get_stats(&s);
  per_frame = ((s.addresses + 3) & ~3u) + 12;  /* table bytes, then the pointer target */
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

int main(void)
{
  RUN(test_session_loads_hardcore);
  RUN(test_set_addresses_snapshotted_from_the_start);
  RUN(test_first_queued_frames_use_their_own_memory);
  RUN(test_queued_frames_follow_moved_pointers);
  RUN(test_queued_frames_ignore_later_memory);
  RUN(test_evaluates_frozen_snapshot);
  RUN(test_ignores_changes_after_vblank);
  RUN(test_pointer_chain_follows_moved_pointer);
  RUN(test_dtcm);
  RUN(test_unlock_sent_in_hardcore);
  RUN(test_softcore_session);
  RUN(test_dropped_frames_are_reported);
  RUN(test_long_stretch_without_idle_is_queued);
  RUN(test_snapshot_arena_wraps);
  RUN(test_unchanged_frames_queue_as_repeats);
  RUN(test_reset_and_progress);
  RUN(test_unlock_retried_when_server_down);
  printf("%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
