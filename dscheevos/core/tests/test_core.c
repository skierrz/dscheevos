/*
 * Host unit tests for the DScheevos core: real rcheevos, fake game memory,
 * fake server. Run: make -C dscheevos/core test
 */
#include "dscheevos.h"
#include "../../module/include/dscheevos_module.h"

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
  else if (!strcmp(r, "achievementsets")) body = serve_v6 ? set_json_v6 : serve_warning ? set_json_warning : set_json;
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
static int spectator;
static uint32_t (*platform_frame)(void);
static void start_with(int hardcore, uint32_t slots, uint32_t bytes)
{
  dsc_platform_t p = { read_memory, millis, http_send, on_event, NULL, unix_time, &journal, &notices, publish_list, platform_frame };
  dsc_config_t c = { "http://mock", "tester", "tok", hardcore, 1024, slots, bytes, spectator };
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

static void test_journal_unlock_marked_sent(void)
{
  start(1);
  CHECK(journal.magic == DSCM_JOURNAL_MAGIC && journal.size == sizeof(dscm_journal_t));
  CHECK(journal.game_id == 77 && journal.count == 0);
  CHECK(!strcmp(journal.username, "tester"));
  CHECK(!strcmp(journal.game_hash, "0123456789abcdef0123456789abcdef"));
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
  dsc_platform_t p = { read_memory, millis, http_send, on_event, NULL, NULL, NULL, NULL, NULL };
  dsc_config_t c = { "http://mock", "tester", "tok", 1, 1024, 0, 0, 0 };
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
  RUN(test_unsupported_client_warning);
  RUN(test_reset_and_progress);
  RUN(test_unlock_retried_when_server_down);
  RUN(test_journal_unlock_marked_sent);
  RUN(test_journal_unsent_until_server_answers);
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
  RUN(test_notice_uses_platform_frame);
  RUN(test_notice_ring_wraps);
  RUN(test_no_outputs_is_fine);
  printf("%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
