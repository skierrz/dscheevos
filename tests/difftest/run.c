/*
 * difftest-run: runs ONE rc_client over a memory trace and writes a canonical
 * per-frame log (docs/rcheevos-difftest.md). Built once per rcheevos tree:
 * difftest-run-stock (the pinned upstream commit) and difftest-run-patched
 * (our fork's pin, -DDIFFTEST_PARTITION). Same source otherwise.
 *
 *   difftest-run --set FILE --session FILE --trace FILE [--script FILE]
 *                [--mode single|partition] [--split SPEC] [--features LIST]
 *                (LIST: partition, changedriven, changes; joined with +)
 *                [--hardcore 0|1] [--thread] [--full FROM] [--frames N] --out LOG
 *
 * The log: a header (inputs and their hashes), then per frame f:
 *   F f
 *   E ...   every event rc_client raised during the frame, in order, all fields
 *   R ...   every server request issued during the frame (api + post data)
 *   S hash  SHA-256 of the state block after the frame (with --full F, from frame
 *           F on, the state block itself follows as T lines)
 * The state block: every achievement (id, state, bucket, unlocked, unlock time,
 * measured progress/percent), every leaderboard (id, state, tracker value), the
 * rich presence string, the user game summary and rc_client_serialize_progress.
 *
 * Partition mode (patched build only): the trigger evaluator claims achievements
 * by --split, evaluates the frame's work list in frame_begin in REVERSE order
 * (or, with --thread, in a second thread, concurrently with rc_client) and
 * hands the results back in result(): the ARM7 protocol, on the host.
 *
 * Core mode (difftest-run-core, -DDIFFTEST_CORE): the DScheevos core runs the
 * rc_client instead (dscheevos/core, its snapshot queue, the module's flags),
 * fed one capture and VBlank per trace frame, and evaluated by two simulated
 * CPUs that hand the rc_client over every random 1-8 frames (--handover SEED;
 * stalls of up to 150 frames, ending at 48 queued or half the arena, build a backlog;
 * past 96 KB or 48 frames both get extra turns until it is short again, as P1's
 * ARM9 take-over would). Resets and the hardcore switch
 * go through the core's queue (dsc_reset, dsc_hardcore_off); pauses and MENU
 * frames are a frozen window: the ARM9 takes the rc_client and drains the
 * queue (dsc_flush), then the paused frames run rc_client_idle. rc_client's
 * clock is the evaluated frame's own time. The log is the same format, so it
 * compares with the stock log as is. Not supported: SHORTREAD (the core's
 * snapshot serves reads) and UNLOAD/LOAD (the core loads once).
 *
 * changes (patched build with rc_client_set_changed_memory): before each
 * rc_client_do_frame, the exact memory that changed since the previous one (the
 * trace's ranges, short reads starting and ending), except on about one frame in
 * 13, which passes "unknown" (not with exact: measurement).
 */
#include "rc_client.h"
#include "rc_api_request.h"
#include "rc_consoles.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef DIFFTEST_THREADS
#include <pthread.h>
#include <stdatomic.h>
#endif

#include "sha256.h"

#ifdef DIFFTEST_CORE
#include "dscheevos.h"
#include "../../dscheevos/module/include/dscheevos_module.h"
#endif

/* internal state the public API doesn't show (has_hits is not in the progress
 * serialization): the same headers in both trees at the pin */
#include "client/rc_client_internal.h"
#include "runtime/rc_internal.h"

/* ------------------------------------------------------------------ stats (measurement builds) */

#ifdef DIFFTEST_STATS
/* counters the measurement copy of the tree increments (stats.sh): conditions evaluated in
 * groups, of them in the achievement phase, rc_test_condition calls, snapshot walk steps */
unsigned long g_st_eval, g_st_ach, g_st_ach0, g_st_test, g_st_walk, g_st_lb, g_st_lb0;
static FILE* g_stats;
#endif

/* ------------------------------------------------------------------ util */

static void die(const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "difftest-run: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(2);
}

static char* load_file(const char* path, size_t* size)
{
  FILE* f = fopen(path, "rb");
  char* b;
  long n;
  if (!f) die("can't open %s", path);
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  b = (char*)malloc((size_t)n + 1);
  if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) die("can't read %s", path);
  b[n] = 0;
  fclose(f);
  if (size) *size = (size_t)n;
  return b;
}

static void hash_hex(const void* data, size_t n, char out[65])
{
  uint8_t h[32];
  int i;
  sha256(data, n, h);
  for (i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", h[i]);
}

/* rc_client stamps unlocks with time(NULL): pin it to a fake epoch that moves
 * with the fake clock, so both sides stamp the same frames the same */
static uint64_t g_millis;
time_t time(time_t* t)
{
  time_t v = (time_t)(1700000000 + g_millis / 1000);
  if (t) *t = v;
  return v;
}

/* ------------------------------------------------------------------ log */

static FILE* g_log;

/* ------------------------------------------------------------------ memory (trace) */

/* --features changes: what changed since the last rc_client_do_frame */
static int g_changes, g_changes_exact;
#ifdef DIFFTEST_CHANGES
static rc_client_memory_range_t* g_chg;
static uint32_t g_chg_count, g_chg_size;
static rc_client_memory_range_t g_chg_none;
#endif
static void changed(uint32_t address, uint32_t num_bytes)
{
#ifdef DIFFTEST_CHANGES
  if (!g_changes) return;
  if (g_chg_count == g_chg_size) {
    g_chg_size = g_chg_size ? g_chg_size * 2 : 256;
    g_chg = (rc_client_memory_range_t*)realloc(g_chg, g_chg_size * sizeof(*g_chg));
    if (!g_chg) { fprintf(stderr, "out of memory\n"); exit(2); }
  }
  g_chg[g_chg_count].address = address;
  g_chg[g_chg_count].num_bytes = num_bytes;
  g_chg_count++;
#else
  (void)address; (void)num_bytes;
#endif
}

#define MAIN_BASE 0x00000000u
#define DTCM_BASE 0x01000000u
static uint8_t* g_main;
static uint32_t g_main_size;
static uint8_t g_dtcm[0x4000];
static uint32_t g_dtcm_size;

typedef struct { uint32_t addr, len, frames; } shortread_t;
#define MAX_SHORT 16
static shortread_t g_short[MAX_SHORT];
static int g_short_count;

static uint32_t RC_CCONV read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client)
{
  int i;
  (void)client;
  for (i = 0; i < g_short_count; i++) {
    const shortread_t* s = &g_short[i];
    if (s->frames && address < s->addr + s->len && address + num_bytes > s->addr)
      return address < s->addr ? s->addr - address : 0;   /* short: the bytes before the range */
  }
  if (address >= MAIN_BASE && address - MAIN_BASE < g_main_size) {
    uint32_t off = address - MAIN_BASE, n = num_bytes;
    if (off + n > g_main_size) n = g_main_size - off;
    memcpy(buffer, g_main + off, n);
    return n;
  }
  if (address >= DTCM_BASE && address - DTCM_BASE < g_dtcm_size) {
    uint32_t off = address - DTCM_BASE, n = num_bytes;
    if (off + n > g_dtcm_size) n = g_dtcm_size - off;
    memcpy(buffer, g_dtcm + off, n);
    return n;
  }
  return 0;
}

/* trace: "DTRC1\0\0\0", u32 main size, u32 dtcm size, u32 frame count, u32 seed,
 * main image, dtcm image, then per frame: u32 frame, u8 flags, u8 pad, u16 ranges,
 * per range u32 address (rcheevos address space), u16 length, bytes */
#define FLAG_LAG  1
#define FLAG_MENU 2
static const uint8_t* g_trace;
static size_t g_trace_size, g_trace_pos;
static uint32_t g_trace_frames, g_trace_seed;
static uint16_t g_trace_ranges;     /* ranges the last frame changed */

static uint32_t rd32(void) { uint32_t v; if (g_trace_pos + 4 > g_trace_size) die("trace truncated"); memcpy(&v, g_trace + g_trace_pos, 4); g_trace_pos += 4; return v; }
static uint16_t rd16(void) { uint16_t v; if (g_trace_pos + 2 > g_trace_size) die("trace truncated"); memcpy(&v, g_trace + g_trace_pos, 2); g_trace_pos += 2; return v; }

static void trace_open(const char* path)
{
  g_trace = (const uint8_t*)load_file(path, &g_trace_size);
  if (g_trace_size < 24 || memcmp(g_trace, "DTRC1\0\0\0", 8)) die("%s: not a trace", path);
  g_trace_pos = 8;
  g_main_size = rd32();
  g_dtcm_size = rd32();
  g_trace_frames = rd32();
  g_trace_seed = rd32();
  if (g_dtcm_size > sizeof(g_dtcm) || g_trace_pos + g_main_size + g_dtcm_size > g_trace_size) die("bad trace header");
  g_main = (uint8_t*)malloc(g_main_size);
  memcpy(g_main, g_trace + g_trace_pos, g_main_size);
  g_trace_pos += g_main_size;
  memcpy(g_dtcm, g_trace + g_trace_pos, g_dtcm_size);
  g_trace_pos += g_dtcm_size;
}

/* applies the next frame's changes; returns its flags, -1 at the end */
static int trace_next(uint32_t* frame)
{
  uint8_t flags;
  uint16_t n, i;
  if (g_trace_pos >= g_trace_size) return -1;
  *frame = rd32();
  flags = g_trace[g_trace_pos];
  g_trace_pos += 2;
  n = rd16();
  g_trace_ranges = n;
  for (i = 0; i < n; i++) {
    uint32_t a = rd32();
    uint16_t len = rd16();
    if (g_trace_pos + len > g_trace_size) die("trace truncated");
    changed(a, len);
    if (a >= DTCM_BASE && a - DTCM_BASE + len <= g_dtcm_size) memcpy(g_dtcm + (a - DTCM_BASE), g_trace + g_trace_pos, len);
    else if (a + len <= g_main_size) memcpy(g_main + a, g_trace + g_trace_pos, len);
    else die("trace range %08x+%u outside memory", a, len);
    g_trace_pos += len;
  }
  return flags;
}

/* ------------------------------------------------------------------ fake server and clock */

static const char* g_set_json;
static const char* g_session_json;
static uint32_t g_total_achievements, g_awarded;
static struct { char kind[32]; int count; } g_fail[8];
static int g_fail_count;

static rc_clock_t RC_CCONV get_millis(const rc_client_t* client) { (void)client; return (rc_clock_t)g_millis; }

static int fail_next(const char* api)
{
  int i;
  for (i = 0; i < g_fail_count; i++) {
    if (g_fail[i].count > 0 && strcmp(g_fail[i].kind, api) == 0) {
      g_fail[i].count--;
      return 1;
    }
  }
  return 0;
}

static void api_name(const char* post, char* out, size_t n)
{
  const char* r = post ? strstr(post, "r=") : NULL;
  size_t i = 0;
  if (!r) { snprintf(out, n, "?"); return; }
  r += 2;
  while (r[i] && r[i] != '&' && i + 1 < n) { out[i] = r[i]; i++; }
  out[i] = 0;
}

static void RC_CCONV server_call(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data, rc_client_t* client)
{
  rc_api_server_response_t resp;
  char api[32], body[256];
  const char* post = request->post_data ? request->post_data : "";
  (void)client;
  api_name(post, api, sizeof(api));
  fprintf(g_log, "R %s %s %s\n", api, request->url ? request->url : "", post);

  memset(&resp, 0, sizeof(resp));
  resp.http_status_code = 200;
  if (fail_next(strcmp(api, "awardachievement") == 0 ? "award" : strcmp(api, "submitlbentry") == 0 ? "submit" : api)) {
    resp.body = "";
    resp.body_length = 0;
    resp.http_status_code = 503;
    callback(&resp, callback_data);
    return;
  }
  if (strcmp(api, "login2") == 0)
    resp.body = "{\"Success\":true,\"User\":\"User\",\"Token\":\"ApiToken\",\"Score\":12345,\"SoftcoreScore\":123,\"Messages\":0,\"Permissions\":1,\"AccountType\":\"Registered\"}";
  else if (strcmp(api, "achievementsets") == 0 || strcmp(api, "patch") == 0)
    resp.body = g_set_json;
  else if (strcmp(api, "startsession") == 0)
    resp.body = g_session_json;
  else if (strcmp(api, "awardachievement") == 0) {
    uint32_t remaining = g_total_achievements > g_awarded + 1 ? g_total_achievements - g_awarded - 1 : 0;
    const char* a = strstr(post, "&a=");
    g_awarded++;
    snprintf(body, sizeof(body), "{\"Success\":true,\"Score\":%u,\"SoftcoreScore\":777,\"AchievementID\":%u,\"AchievementsRemaining\":%u}",
             12345u + g_awarded * 10u, a ? (unsigned)strtoul(a + 3, NULL, 10) : 0u, remaining);
    resp.body = body;
  }
  else if (strcmp(api, "submitlbentry") == 0) {
    const char* i = strstr(post, "&i=");
    const char* s = strstr(post, "&s=");
    snprintf(body, sizeof(body), "{\"Success\":true,\"Response\":{\"Score\":%ld,\"BestScore\":%ld,\"RankInfo\":{\"Rank\":1,\"NumEntries\":1},\"TopEntries\":[],\"LBData\":{\"LeaderboardID\":%u}}}",
             s ? strtol(s + 3, NULL, 10) : 0L, s ? strtol(s + 3, NULL, 10) : 0L, i ? (unsigned)strtoul(i + 3, NULL, 10) : 0u);
    resp.body = body;
  }
  else
    resp.body = "{\"Success\":true}";
  resp.body_length = strlen(resp.body);
  callback(&resp, callback_data);
}

/* ------------------------------------------------------------------ events */

static void put_str(const char* label, const char* s)
{
  fprintf(g_log, " %s=\"", label);
  if (s) {
    for (; *s; s++) {
      if (*s == '"' || *s == '\\') fputc('\\', g_log);
      if (*s == '\n') { fputs("\\n", g_log); continue; }
      fputc(*s, g_log);
    }
  }
  fputc('"', g_log);
}

static void put_achievement(FILE* f, const rc_client_achievement_t* a)
{
  fprintf(f, " ach=%u state=%u bucket=%u unlocked=%u unlock_time=%ld points=%u category=%u type=%u measured_percent=%.9g",
          a->id, a->state, a->bucket, a->unlocked, (long)a->unlock_time, a->points, a->category, a->type, (double)a->measured_percent);
  if (f == g_log) {
    put_str("measured_progress", a->measured_progress);
    put_str("title", a->title);
    put_str("description", a->description);
    put_str("badge", a->badge_name);
  }
  else
    fprintf(f, " measured_progress=\"%s\"", a->measured_progress);
}

static void put_leaderboard(FILE* f, const rc_client_leaderboard_t* l)
{
  fprintf(f, " lb=%u state=%u format=%u lower=%u", l->id, l->state, l->format, l->lower_is_better);
  if (f == g_log) {
    put_str("tracker", l->tracker_value);
    put_str("title", l->title);
  }
  else
    fprintf(f, " tracker=\"%s\"", l->tracker_value ? l->tracker_value : "");
}

static void RC_CCONV event_handler(const rc_client_event_t* e, rc_client_t* client)
{
  (void)client;
  fprintf(g_log, "E %u", e->type);
  if (e->achievement) put_achievement(g_log, e->achievement);
  if (e->leaderboard) put_leaderboard(g_log, e->leaderboard);
  if (e->leaderboard_tracker) { fprintf(g_log, " tracker=%u", e->leaderboard_tracker->id); put_str("display", e->leaderboard_tracker->display); }
  if (e->leaderboard_scoreboard) {
    const rc_client_leaderboard_scoreboard_t* s = e->leaderboard_scoreboard;
    fprintf(g_log, " scoreboard=%u rank=%u entries=%u", s->leaderboard_id, s->new_rank, s->num_entries);
    put_str("submitted", s->submitted_score);
    put_str("best", s->best_score);
  }
  if (e->server_error) {
    fprintf(g_log, " error_result=%d related=%u", e->server_error->result, e->server_error->related_id);
    put_str("api", e->server_error->api);
    put_str("message", e->server_error->error_message);
  }
  if (e->subset) { fprintf(g_log, " subset=%u", e->subset->id); put_str("subset_title", e->subset->title); }
  fputc('\n', g_log);
}

/* ------------------------------------------------------------------ state block */

static char* g_state;
static size_t g_state_len, g_state_cap;

static void state_printf(const char* fmt, ...)
{
  va_list ap;
  int n;
  for (;;) {
    va_start(ap, fmt);
    n = vsnprintf(g_state + g_state_len, g_state_cap - g_state_len, fmt, ap);
    va_end(ap);
    if (n >= 0 && (size_t)n < g_state_cap - g_state_len) { g_state_len += (size_t)n; return; }
    g_state_cap = g_state_cap * 2 + 4096;
    g_state = (char*)realloc(g_state, g_state_cap);
  }
}

static void trigger_state(const char* what, uint32_t id, const rc_trigger_t* t)
{
  const rc_condset_t* cs;
  int k = 0;
  if (!t) return;
  state_printf(" %s=%u tstate=%u has_hits=%u mv=%u mt=%u pct=%u", what, id, t->state, t->has_hits,
               t->measured_value, t->measured_target, t->measured_as_percent);
  for (cs = t->requirement; cs || k == 0; cs = (k == 0) ? t->alternative : cs->next, k++) {
    const rc_condition_t* c;
    if (!cs) { if (k == 0) continue; break; }
    state_printf(" [%u", cs->is_paused);
    for (c = cs->conditions; c; c = c->next)
      state_printf(" %u:%u", c->current_hits, c->is_true);
    state_printf("]");
  }
  state_printf("\n");
}

/* every trigger's internal state, every leaderboard's, every memref */
static void internal_state(rc_client_t* client)
{
  const rc_client_subset_info_t* subset;
  if (!client->game) return;
  for (subset = client->game->subsets; subset; subset = subset->next) {
    uint32_t i;
    state_printf("subset %u active=%u mastery=%u\n", subset->public_.id, subset->active, subset->mastery);
    for (i = 0; i < subset->public_.num_achievements; i++)
      trigger_state("t", subset->achievements[i].public_.id, subset->achievements[i].trigger);
    for (i = 0; i < subset->public_.num_leaderboards; i++) {
      const rc_client_leaderboard_info_t* l = &subset->leaderboards[i];
      state_printf(" lbi=%u value=%d\n", l->public_.id, l->value);
      if (l->lboard) {
        state_printf(" lbstate=%u\n", l->lboard->state);
        trigger_state("start", l->public_.id, &l->lboard->start);
        trigger_state("submit", l->public_.id, &l->lboard->submit);
        trigger_state("cancel", l->public_.id, &l->lboard->cancel);
      }
    }
  }
  if (client->game->runtime.memrefs) {
    const rc_memref_list_t* ml;
    const rc_modified_memref_list_t* mml;
    for (ml = &client->game->runtime.memrefs->memrefs; ml; ml = ml->next) {
      uint32_t i;
      for (i = 0; i < ml->count; i++) {
        const rc_memref_t* m = &ml->items[i];
        state_printf(" m %x %u %u %u %u %u\n", m->address, m->value.value, m->value.prior, m->value.changed, m->value.type, m->value.size);
      }
    }
    for (mml = &client->game->runtime.memrefs->modified_memrefs; mml; mml = mml->next) {
      uint32_t i;
      for (i = 0; i < mml->count; i++) {
        const rc_memref_value_t* v = &mml->items[i].memref.value;
        state_printf(" mm %u %u %u %u %u\n", i, v->value, v->prior, v->changed, v->type);
      }
    }
  }
}

static void state_block(rc_client_t* client)
{
  rc_client_achievement_list_t* al;
  rc_client_leaderboard_list_t* ll;
  rc_client_user_game_summary_t sum;
  char rp[512];
  size_t n;
  uint32_t i, j;

  uint8_t* progress = NULL;
  int progress_result = 0;
  size_t progress_size;

  g_state_len = 0;
  if (!g_state) { g_state_cap = 65536; g_state = (char*)malloc(g_state_cap); }
  g_state[0] = 0;

  /* serialize the progress first: with change-driven evaluation, what is owed to skipped
   * achievements' hit counts is settled by it (internal state is only exact once observed through
   * the API); stock serializing changes nothing, so the order is the same for both */
  progress_size = rc_client_progress_size(client);
  if (progress_size) {
    progress = (uint8_t*)malloc(progress_size);
    progress_result = rc_client_serialize_progress_sized(client, progress, progress_size);
  }

  al = rc_client_create_achievement_list(client, RC_CLIENT_ACHIEVEMENT_CATEGORY_PROMOTED_AND_UNPROMOTED, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
  if (al) {
    for (i = 0; i < al->num_buckets; i++) {
      const rc_client_achievement_bucket_t* b = &al->buckets[i];
      state_printf("bucket %u subset=%u n=%u\n", b->bucket_type, b->subset_id, b->num_achievements);
      for (j = 0; j < b->num_achievements; j++) {
        const rc_client_achievement_t* a = b->achievements[j];
        state_printf(" ach=%u state=%u bucket=%u unlocked=%u unlock_time=%ld measured_percent=%.9g measured_progress=\"%s\"\n",
                     a->id, a->state, a->bucket, a->unlocked, (long)a->unlock_time, (double)a->measured_percent, a->measured_progress);
      }
    }
    rc_client_destroy_achievement_list(al);
  }
  ll = rc_client_create_leaderboard_list(client, RC_CLIENT_LEADERBOARD_LIST_GROUPING_NONE);
  if (ll) {
    for (i = 0; i < ll->num_buckets; i++) {
      const rc_client_leaderboard_bucket_t* b = &ll->buckets[i];
      for (j = 0; j < b->num_leaderboards; j++) {
        const rc_client_leaderboard_t* l = b->leaderboards[j];
        state_printf(" lb=%u state=%u tracker=\"%s\"\n", l->id, l->state, l->tracker_value ? l->tracker_value : "");
      }
    }
    rc_client_destroy_leaderboard_list(ll);
  }
  internal_state(client);
  n = rc_client_get_rich_presence_message(client, rp, sizeof(rp));
  (void)n;
  state_printf("rp \"%s\"\n", rp);
  memset(&sum, 0, sizeof(sum));
  rc_client_get_user_game_summary(client, &sum);
  state_printf("summary %u %u %u %u %u %u %ld %ld\n", sum.num_promoted_achievements, sum.num_unpromoted_achievements,
               sum.num_unlocked_achievements, sum.num_unsupported_achievements, sum.points_available, sum.points_unlocked,
               (long)sum.beaten_time, (long)sum.completed_time);
  if (progress) {
    char h[65];
    hash_hex(progress, progress_size, h);
    state_printf("progress %d %zu %s\n", progress_result, progress_size, h);
    free(progress);
  }
}

/* ------------------------------------------------------------------ partition (patched build) */

#ifdef DIFFTEST_PARTITION
typedef struct {
  enum { SPLIT_NONE, SPLIT_ALL, SPLIT_MOD, SPLIT_RANDOM, SPLIT_LIST } kind;
  uint32_t mod, rem, seed, pct;
  uint32_t* ids;
  uint32_t nids;
} split_t;
static split_t g_split;
static int g_threaded;
static uint32_t g_claimed, g_work_max;

static uint32_t mix32(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }

static int RC_CCONV ev_claim(uint32_t id, void* ud)
{
  int c = 0;
  uint32_t i;
  (void)ud;
  switch (g_split.kind) {
    case SPLIT_NONE: c = 0; break;
    case SPLIT_ALL: c = 1; break;
    case SPLIT_MOD: c = id % g_split.mod == g_split.rem; break;
    case SPLIT_RANDOM: c = mix32(id ^ g_split.seed) % 100 < g_split.pct; break;
    case SPLIT_LIST: for (i = 0; i < g_split.nids; i++) if (g_split.ids[i] == id) c = 1; break;
  }
  g_claimed += (uint32_t)c;
  return c;
}

/* claimed triggers on their own 32-byte lines, as the console glue places them */
static void* RC_CCONV ev_alloc(uint32_t id, size_t size, void* ud)
{
  void* p = NULL;
  (void)id; (void)ud;
  if (posix_memalign(&p, 32, (size + 31) & ~(size_t)31)) return NULL;
  memset(p, 0xA5, (size + 31) & ~(size_t)31);   /* rcheevos must initialize all it uses */
  return p;   /* leaked at unload: a test tool */
}

static const rc_client_claimed_trigger_t* g_work;
static uint32_t g_work_count;
static int g_results[4096];
#ifdef DIFFTEST_THREADS
static atomic_int g_done[4096];
static pthread_t g_thread;
static int g_thread_live;
static void* worker(void* arg)
{
  uint32_t i;
  (void)arg;
  for (i = g_work_count; i > 0; i--) {
    g_results[i - 1] = rc_client_evaluate_claimed_trigger(g_work[i - 1].trigger);
    atomic_store_explicit(&g_done[i - 1], 1, memory_order_release);
  }
  return NULL;
}
#endif

static void RC_CCONV ev_frame_begin(const rc_client_claimed_trigger_t* work, uint32_t count, void* ud)
{
  uint32_t i;
  (void)ud;
  if (count > sizeof(g_results) / sizeof(g_results[0])) die("work list too long");
  g_work = work;
  g_work_count = count;
  if (count > g_work_max) g_work_max = count;
#ifdef DIFFTEST_THREADS
  if (g_threaded) {
    for (i = 0; i < count; i++) atomic_store(&g_done[i], 0);
    if (pthread_create(&g_thread, NULL, worker, NULL)) die("pthread_create");
    g_thread_live = 1;
    return;
  }
#endif
  for (i = count; i > 0; i--)
    g_results[i - 1] = rc_client_evaluate_claimed_trigger(work[i - 1].trigger);
}

static uint32_t g_next;
static int g_fault;        /* --fault: self-test, hide the first remote unlock (the harness must see it) */
static int RC_CCONV ev_result(uint32_t id, void* trigger, void* ud)
{
  uint32_t i = g_next;
  (void)ud;
  /* rc_client takes results in work-list order */
  if (i >= g_work_count || g_work[i].achievement_id != id || g_work[i].trigger != trigger)
    die("result() for achievement %u out of work-list order (entry %u of %u)", id, i, g_work_count);
  g_next++;
#ifdef DIFFTEST_THREADS
  if (g_threaded)
    while (!atomic_load_explicit(&g_done[i], memory_order_acquire)) {}
#endif
  if (g_fault && g_results[i] == RC_TRIGGER_STATE_TRIGGERED) {
    g_fault = 0;
    return RC_TRIGGER_STATE_ACTIVE;
  }
  return g_results[i];
}

static void RC_CCONV ev_frame_end(void* ud)
{
  (void)ud;
  if (g_next != g_work_count) die("frame_end with %u of %u results taken", g_next, g_work_count);
  g_next = 0;
#ifdef DIFFTEST_THREADS
  if (g_thread_live) { pthread_join(g_thread, NULL); g_thread_live = 0; }
#endif
}

static void parse_split(const char* spec)
{
  memset(&g_split, 0, sizeof(g_split));
  if (!spec || !strcmp(spec, "none")) g_split.kind = SPLIT_NONE;
  else if (!strcmp(spec, "all")) g_split.kind = SPLIT_ALL;
  else if (sscanf(spec, "mod:%u:%u", &g_split.mod, &g_split.rem) == 2 && g_split.mod) g_split.kind = SPLIT_MOD;
  else if (sscanf(spec, "random:%u:%u", &g_split.seed, &g_split.pct) == 2) g_split.kind = SPLIT_RANDOM;
  else if (!strncmp(spec, "list:", 5)) {
    char* t = load_file(spec + 5, NULL);
    char* p = t;
    g_split.kind = SPLIT_LIST;
    g_split.ids = (uint32_t*)malloc(sizeof(uint32_t) * 65536);
    while (*p) {
      char* e;
      unsigned long v = strtoul(p, &e, 10);
      if (e == p) { p++; continue; }
      if (g_split.nids < 65536) g_split.ids[g_split.nids++] = (uint32_t)v;
      p = e;
    }
  }
  else die("bad --split %s", spec);
}
#endif

/* ------------------------------------------------------------------ hit counts near 2^32 */

/* HITS seed: round-trips the progress through serialize/deserialize (so a change-driven index is
 * dropped), then sets every non-zero hit count without a target in the active achievements to just
 * below 2^32 or just below 2^32 - 0x10000, so they wrap (or near a skip limit) within the run.
 * The same on both sides. */
static void hits_near_wrap(rc_client_t* client, uint32_t seed)
{
  const rc_client_subset_info_t* subset;
  size_t n;
  uint8_t* p;
  if (!client->game) return;
  n = rc_client_progress_size(client);
  if (!n) return;
  p = (uint8_t*)malloc(n);
  if (rc_client_serialize_progress_sized(client, p, n) == RC_OK)
    rc_client_deserialize_progress_sized(client, p, n);
  free(p);
  for (subset = client->game->subsets; subset; subset = subset->next) {
    uint32_t i;
    for (i = 0; i < subset->public_.num_achievements; i++) {
      const rc_trigger_t* t = subset->achievements[i].trigger;
      const rc_condset_t* cs;
      int k = 0;
      if (!t || !rc_trigger_state_active(t->state)) continue;
      for (cs = t->requirement; cs || k == 0; cs = (k == 0) ? t->alternative : cs->next, k++) {
        rc_condition_t* c;
        if (!cs) { if (k == 0) continue; break; }
        for (c = cs->conditions; c; c = c->next) {
          if (c->current_hits && !c->required_hits) {
            seed = seed * 1103515245u + 12345u;
            c->current_hits = ((seed >> 16) & 1 ? 0xFFFFFFFFu : 0xFFFEFFFFu) - ((seed >> 17) % 400u);
          }
        }
      }
    }
  }
}

/* ------------------------------------------------------------------ script */

typedef struct { uint32_t frame; char cmd[16]; char a[32]; uint32_t b, c; } command_t;
static command_t* g_cmds;
static int g_ncmds;

static void load_script(const char* path)
{
  char* t = load_file(path, NULL);
  char* line = strtok(t, "\n");
  g_cmds = (command_t*)calloc(4096, sizeof(command_t));
  for (; line; line = strtok(NULL, "\n")) {
    command_t c;
    int n;
    memset(&c, 0, sizeof(c));
    if (line[0] == '#' || !line[0]) continue;
    n = sscanf(line, "%u %15s %31s %u %u", &c.frame, c.cmd, c.a, &c.b, &c.c);
    if (n < 2) die("bad script line: %s", line);
    if (g_ncmds < 4096) g_cmds[g_ncmds++] = c;
  }
}


/* ------------------------------------------------------------------ main */

static const char* g_hash = "0123456789ABCDEF0123456789ABCDEF";
static int g_load_result = -99;
static void RC_CCONV load_done(int result, const char* error, rc_client_t* client, void* ud)
{
  (void)client; (void)ud;
  g_load_result = result;
  fprintf(g_log, "L %d", result);
  put_str("error", error);
  fputc('\n', g_log);
}
static void RC_CCONV login_done(int result, const char* error, rc_client_t* client, void* ud)
{
  (void)client; (void)ud;
  if (result != RC_OK) die("login failed: %s", error ? error : "?");
}

static rc_client_t* new_client(int hardcore, const char* features)
{
  rc_client_t* client = rc_client_create(read_memory, server_call);
  rc_client_set_event_handler(client, event_handler);
  rc_client_set_get_time_millisecs_function(client, get_millis);
  rc_client_set_hardcore_enabled(client, hardcore);
#ifdef DIFFTEST_PARTITION
  if (strstr(features, "partition")) {
    rc_client_trigger_evaluator_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.claim = ev_claim;
    ev.alloc = ev_alloc;
    ev.frame_begin = ev_frame_begin;
    ev.result = ev_result;
    ev.frame_end = ev_frame_end;
    rc_client_set_trigger_evaluator(client, &ev);
  }
#endif
#ifdef DIFFTEST_CHANGE_DRIVEN
  /* the tree has rcheevos' change-driven patch (the Makefile checks rc_client.h) */
  if (strstr(features, "changedriven"))
    rc_client_set_change_driven_enabled(client, 1);
#else
  if (strstr(features, "changedriven"))
    die("this build: no change-driven patch");
#endif
#if !defined(DIFFTEST_PARTITION)
  if (strstr(features, "partition"))
    die("this build: no partition patch");
#endif
#ifndef DIFFTEST_CHANGES
  if (strstr(features, "changes"))
    die("this build: no changed-memory patch");
#endif
  g_changes = strstr(features, "changes") != NULL;
  g_changes_exact = strstr(features, "exact") != NULL;
  rc_client_begin_login_with_token(client, "User", "ApiToken", login_done, NULL);
  return client;
}

static uint32_t count_achievements(const char* json)
{
  uint32_t n = 0;
  const char* p = json;
  /* achievementsets reply: every achievement object has a "MemAddr" */
  while ((p = strstr(p, "\"MemAddr\"")) != NULL) { n++; p += 9; }
  return n;
}


/* ------------------------------------------------------------------ core mode */

#ifdef DIFFTEST_CORE
static uint32_t g_platform_frame;        /* the trace frame now (the platform's frame count) */
static uint32_t* g_core_map;             /* core frame number -> trace frame */
static uint32_t g_core_map_size, g_vblanks;
static long g_last_F = -1;
static long g_full_from, g_state_sparse;
static uint32_t g_frames_done;
static int g_core_ci;                    /* next harness-only command (FAIL, HITS) to apply */
static command_t g_life[256];            /* lifecycle commands queued in the core, oldest first */
static int g_life_head, g_life_tail;
static dscm_journal_t g_journal;
static dscm_notices_t g_notices;
static uint32_t g_ho_rng;
static int g_ho_want, g_ho_switch_in, g_ho_stall, g_handover;
static uint32_t g_ho_switches, g_ho_stalls, g_max_backlog, g_ho_catchups, g_max_arena;
static int g_cpu;                        /* which simulated CPU is calling the core */
static uint32_t g_by_cpu[2];             /* frames evaluated by each */
static int g_core_fault;                 /* --fault: self-test, resets and the hardcore switch one frame late */
static int g_changed_since_capture = 1;
static uint32_t g_lag_frames;

static uint32_t ho_rnd(void) { g_ho_rng ^= g_ho_rng << 13; g_ho_rng ^= g_ho_rng >> 17; g_ho_rng ^= g_ho_rng << 5; return g_ho_rng; }

/* as the module reads: a read must be entirely inside one region */
static uint32_t core_read(uint32_t address, uint8_t* buffer, uint32_t n)
{
  uint32_t main_size = g_main_size < 0x400000u ? g_main_size : 0x400000u;
  if (address < main_size && n <= main_size - address) { memcpy(buffer, g_main + address, n); return n; }
  if (address >= DTCM_BASE && address - DTCM_BASE < g_dtcm_size && n <= g_dtcm_size - (address - DTCM_BASE)) {
    memcpy(buffer, g_dtcm + (address - DTCM_BASE), n);
    return n;
  }
  return 0;
}
static void core_map(const uint8_t** main_ram, const uint8_t** dtcm)
{
  *main_ram = g_main_size >= 0x400000u ? g_main : NULL;
  *dtcm = g_dtcm_size == sizeof(g_dtcm) ? g_dtcm : NULL;
}
static uint32_t core_millis(void) { return (uint32_t)g_millis; }
static uint32_t core_unix(void) { return (uint32_t)time(NULL); }
static uint32_t core_frame(void) { return g_platform_frame; }

/* the stock harness's server, answering into the core */
static void RC_CCONV core_answer(const rc_api_server_response_t* r, void* handle)
{
  dsc_http_done(handle, r->http_status_code, r->body ? r->body : "", r->body_length);
}
static void core_http_send(const dsc_http_request_t* req)
{
  rc_api_request_t r;
  memset(&r, 0, sizeof(r));
  r.url = req->url;
  r.post_data = req->post_data;
  r.content_type = req->content_type;
  server_call(&r, core_answer, req->handle, NULL);
}
static void core_event(const dsc_event_t* e)
{
  if (e->type == DSC_EVENT_LOGIN_OK)
    fprintf(g_log, "F load\n");
  else if (e->type == DSC_EVENT_GAME_LOADED || e->type == DSC_EVENT_GAME_LOAD_FAILED) {
    g_load_result = e->type == DSC_EVENT_GAME_LOADED ? 0 : -(int)e->id;
    fprintf(g_log, "L %d", g_load_result);
    put_str("error", e->type == DSC_EVENT_GAME_LOADED ? NULL : e->detail);
    fputc('\n', g_log);
  }
}

static void write_F(uint32_t f)
{
  if (g_last_F != (long)f) {
    fprintf(g_log, "F %u\n", f);
    g_last_F = f;
  }
}

static void write_C(const command_t* c)
{
  fprintf(g_log, "C %s %s %u %u\n", c->cmd, c->a, c->b, c->c);
}

/* harness-only commands (the fake server's failures, the hit-count seed): in
 * evaluation order, just before their frame */
static void core_commands_up_to(uint32_t f, rc_client_t* client)
{
  for (; g_core_ci < g_ncmds && g_cmds[g_core_ci].frame <= f; g_core_ci++) {
    const command_t* c = &g_cmds[g_core_ci];
    if (!strcmp(c->cmd, "FAIL")) {
      write_C(c);
      if (g_fail_count < 8) {
        snprintf(g_fail[g_fail_count].kind, sizeof(g_fail[0].kind), "%s", c->a);
        g_fail[g_fail_count].count = (int)c->b;
        g_fail_count++;
      }
    }
    else if (!strcmp(c->cmd, "HITS")) {
      write_C(c);
      hits_near_wrap(client, (uint32_t)strtoul(c->a, NULL, 0));
    }
  }
}

static void hook_rc_event(const void* e, void* client) { event_handler((const rc_client_event_t*)e, (rc_client_t*)client); }

static void hook_frame_begin(uint32_t core_frame)
{
  uint32_t f;
  if (core_frame >= g_core_map_size) die("core frame %u out of range", core_frame);
  f = g_core_map[core_frame];
  g_by_cpu[g_cpu]++;
  g_millis = (uint64_t)f * 1001u / 60u;
  write_F(f);
  core_commands_up_to(f, (rc_client_t*)dsc_test_client());
}

static void state_record(rc_client_t* client, uint32_t f)
{
  char h[65];
  g_frames_done++;
  if (g_state_sparse > 1 && ((f * 2654435761u) >> 8) % (uint32_t)g_state_sparse != 0)
    return;
  state_block(client);
  hash_hex(g_state, g_state_len, h);
  fprintf(g_log, "S %s\n", h);
  if (g_full_from >= 0 && (long)f >= g_full_from) {
    char* p = g_state;
    while (*p) {
      char* e = strchr(p, '\n');
      if (e) *e = 0;
      fprintf(g_log, "T %s\n", p);
      if (!e) break;
      p = e + 1;
    }
  }
}

static void hook_frame_end(void* client, uint32_t core_frame)
{
  state_record((rc_client_t*)client, g_core_map[core_frame]);
}

static void hook_lifecycle(int what)
{
  const command_t* c;
  if (g_life_tail == g_life_head) die("lifecycle change %d with none queued", what);
  c = &g_life[g_life_tail++ % 256];
  g_millis = (uint64_t)c->frame * 1001u / 60u;
  write_F(c->frame);
  write_C(c);
}

/* The two evaluators: the "ARM9" wants the rc_client for a random 1-8 frames,
 * then gives it back for 1-8; each frame both get a few turns in random
 * order. Now and then (1 frame in 200) nobody evaluates for up to 150 frames,
 * so frames queue deep (the ring and arena hold them; nothing may drop). */
static void core_tick(void)
{
  int k;
  if (!g_handover) {
    dsc_idle();
    return;
  }
  /* a stall ends early at 48 queued frames (under the design's 60-frame lag
   * cap) or half the 256 KB arena: busy fuzz frames fill it at about 60-75
   * frames for Platinum or Digimon, far fewer for SoulSilver (chain targets);
   * then frames drop, which is the dropped-frames gap, not hand-over */
  if (g_ho_stall > 0 && dsc_backlog() < 48) {
    dsc_stats_t st;
    dsc_get_stats(&st);
    if (st.arena_used < 128u * 1024u) { g_ho_stall--; return; }
  }
  g_ho_stall = 0;
  if (ho_rnd() % 200 == 0) { g_ho_stall = 1 + (int)(ho_rnd() % 150); g_ho_stalls++; }
  /* behind (P1's "ARM9 helps when the backlog passes Hi"): more turns for
   * both, through the same protocol, until the queue is short again, so no
   * frame drops for capacity (busy SoulSilver frames are many KB each) */
  {
    dsc_stats_t st;
    int guard = 0;
    dsc_get_stats(&st);
    if (st.arena_used > 96u * 1024u || dsc_backlog() > 48) {
      g_ho_catchups++;
      while ((st.arena_used > 64u * 1024u || dsc_backlog() > 24) && guard++ < 1000) {
        g_cpu = DSC_OWNER_ARM9;
        dsc_idle();
        g_cpu = DSC_OWNER_ARM7;
        dsc_arm7_step(1 + ho_rnd() % 3);
        g_cpu = DSC_OWNER_ARM9;
        dsc_get_stats(&st);
      }
    }
  }
  if (--g_ho_switch_in <= 0) {
    g_ho_want = !g_ho_want;
    dsc_want(g_ho_want);
    g_ho_switch_in = 1 + (int)(ho_rnd() % 8);
    g_ho_switches++;
  }
  for (k = 1 + (int)(ho_rnd() % 4); k > 0; k--) {
    if (ho_rnd() % 2) {
      g_cpu = DSC_OWNER_ARM9;
      dsc_idle();
    }
    else {
      g_cpu = DSC_OWNER_ARM7;
      dsc_arm7_step(ho_rnd() % 4);
      g_cpu = DSC_OWNER_ARM9;
    }
  }
}

/* frozen window (pause, in-game menu): the ARM9 takes the rc_client and drains */
static void core_freeze(void)
{
  int guard = 0;
  if (g_handover) {
    dsc_want(1);
    while (dsc_owner() != DSC_OWNER_ARM9 && guard++ < 16) {
      dsc_idle();          /* asks */
      g_cpu = DSC_OWNER_ARM7;
      dsc_arm7_step(1);    /* grants at its next boundary */
      g_cpu = DSC_OWNER_ARM9;
    }
    g_ho_want = 1;
    g_ho_stall = 0;
  }
  if (dsc_flush() < 0) die("flush without the rc_client");
}

static int core_main(const char* features, int hardcore, long full_from, long max_frames, long state_sparse, uint32_t ho_seed)
{
  dsc_platform_t p;
  dsc_config_t c;
  dsc_test_hooks_t hooks;
  dsc_stats_t st;
  uint32_t frame;
  int flags, paused = 0, ci = 0;
  long frames_seen = 0;

  if (features && features[0]) die("core mode: no --features (the core's own flags are built in)");
  g_full_from = full_from;
  g_state_sparse = state_sparse;
  g_handover = ho_seed != 0;
  g_ho_rng = ho_seed ? ho_seed : 1;
  g_core_map_size = g_trace_frames + 16;
  g_core_map = (uint32_t*)calloc(g_core_map_size, sizeof(uint32_t));
  for (ci = 0; ci < g_ncmds; ci++)
    if (!strcmp(g_cmds[ci].cmd, "SHORTREAD") || !strcmp(g_cmds[ci].cmd, "UNLOAD") || !strcmp(g_cmds[ci].cmd, "LOAD"))
      die("core mode: %s is not supported", g_cmds[ci].cmd);
  ci = 0;

  memset(&hooks, 0, sizeof(hooks));
  hooks.rc_event = hook_rc_event;
  hooks.frame_begin = hook_frame_begin;
  hooks.frame_end = hook_frame_end;
  hooks.lifecycle = hook_lifecycle;
  hooks.sync_replies = 1;
  dsc_test_set_hooks(&hooks);

  memset(&p, 0, sizeof(p));
  p.read_memory = core_read;
  p.millis = core_millis;
  p.http_send = core_http_send;
  p.event = core_event;
  p.unix_time = core_unix;
  p.journal = &g_journal;
  p.notices = &g_notices;
  p.frame = core_frame;
  p.memory_map = core_map;
  memset(&c, 0, sizeof(c));
  c.username = "User";
  c.token = "ApiToken";
  c.hardcore = hardcore;
  c.max_addresses = 4096;                /* the module's settings */
  c.snapshot_slots = 1024;
  c.snapshot_bytes = 256u * 1024u;
  c.capture_at_halt = 1;
  c.idle_max_frames = 4;
  c.handover = g_handover;
  if (dsc_init(&p, &c) != 0) die("dsc_init");
  if (dsc_begin_session(g_hash) != 0) die("dsc_begin_session");
  dsc_idle();
  if (!dsc_is_active()) die("core mode: the game didn't load (%d)", g_load_result);
  if (g_handover) { g_ho_want = 1; g_ho_switch_in = 1 + (int)(ho_rnd() % 8); }

  while ((flags = trace_next(&frame)) >= 0) {
    int late = 0;
    if (max_frames >= 0 && frames_seen >= max_frames) break;
    frames_seen++;
    g_platform_frame = frame;
    for (; ci < g_ncmds && g_cmds[ci].frame <= frame; ci++) {
      command_t* cmd = &g_cmds[ci];
      if (!strcmp(cmd->cmd, "RESET") || !strcmp(cmd->cmd, "HARDCORE")) {
        if (!strcmp(cmd->cmd, "HARDCORE") && atoi(cmd->a) != 0) die("core mode: hardcore only goes off");
        if (g_life_head - g_life_tail >= 256) die("too many lifecycle changes queued");
        g_life[g_life_head % 256] = *cmd;
        g_life[g_life_head % 256].frame = frame + (g_core_fault ? 1 : 0);
        g_life_head++;
        if (g_core_fault)
          late |= !strcmp(cmd->cmd, "RESET") ? 1 : 2;
        else if (!strcmp(cmd->cmd, "RESET")) dsc_reset(); else dsc_hardcore_off();
      }
      else if (!strcmp(cmd->cmd, "PAUSE")) {
        core_freeze();
        g_millis = (uint64_t)frame * 1001u / 60u;
        write_F(frame);
        write_C(cmd);
        paused = (int)strtol(cmd->a, NULL, 0);
      }
    }
    if (g_trace_ranges)
      g_changed_since_capture = 1;
    if (paused > 0 || (flags & FLAG_MENU)) {
      /* the game is frozen: nothing captured; rcheevos' timers only */
      if (paused > 0) paused--;
      g_millis = (uint64_t)frame * 1001u / 60u;
      write_F(frame);
      core_commands_up_to(frame, (rc_client_t*)dsc_test_client());
      core_freeze();       /* nothing queued: rc_client_idle, as the stock run */
      state_record((rc_client_t*)dsc_test_client(), frame);
      continue;
    }
    /* the game halts with this frame's memory, then its VBlank; with
     * hand-over, a frame whose memory didn't change since the last capture
     * sometimes has none (a lag frame: evaluated with the last capture) */
    if (g_handover && !g_trace_ranges && !g_changed_since_capture && ho_rnd() % 2)
      g_lag_frames++;
    else {
      while (dsc_capture() == 1) ;
      g_changed_since_capture = 0;
    }
    g_vblanks++;
    if (g_vblanks >= g_core_map_size) die("too many frames");
    g_core_map[g_vblanks] = frame;
    dsc_vblank();
    if (late & 1) dsc_reset();
    if (late & 2) dsc_hardcore_off();
    {
      uint32_t b = dsc_backlog(), a = dsc_arena_used();
      if (b > g_max_backlog) g_max_backlog = b;
      if (a > g_max_arena) g_max_arena = a;
    }
    core_tick();
  }
  core_freeze();     /* the end: everything evaluated */
  dsc_get_stats(&st);
  fprintf(g_log, "# core: frames %u evaluated %u (arm9 %u arm7 %u) dropped %u repeated %u stale %u live_reads %u handovers %u switches %u stalls %u max_backlog %u notices_lost %u lag %u catchups %u frame_bytes %u max_arena %u\n",
          st.frames_snapshotted, st.frames_evaluated, g_by_cpu[0], g_by_cpu[1], st.frames_dropped, st.frames_repeated, st.frames_stale, st.live_reads,
          st.handovers, g_ho_switches, g_ho_stalls, g_max_backlog, st.notices_lost, g_lag_frames, g_ho_catchups, st.frame_bytes, g_max_arena);
  if (st.frames_dropped) fprintf(stderr, "difftest-run-core: %u frames dropped\n", st.frames_dropped);
  fprintf(g_log, "END %u\n", g_frames_done);
  dsc_shutdown();
  fclose(g_log);
  return 0;
}
#endif

int main(int argc, char** argv)
{
  const char *set = NULL, *session = NULL, *trace = NULL, *script = NULL, *out = NULL;
  const char *mode = "single", *split = "none", *features = "";
  int hardcore = 1, paused = 0, i, flags, ci = 0;
  long full_from = -1, max_frames = -1, state_sparse = 0;
  uint32_t ho_seed = 0;
  uint32_t frame, frames_done = 0;
  rc_client_t* client;
  char h[65];

  for (i = 1; i < argc; i++) {
    const char* a = argv[i];
#define ARG(name) (!strcmp(a, name) && i + 1 < argc)
    if (ARG("--set")) set = argv[++i];
    else if (ARG("--session")) session = argv[++i];
    else if (ARG("--trace")) trace = argv[++i];
    else if (ARG("--script")) script = argv[++i];
    else if (ARG("--mode")) mode = argv[++i];
    else if (ARG("--split")) split = argv[++i];
    else if (ARG("--features")) features = argv[++i];
    else if (ARG("--hardcore")) hardcore = atoi(argv[++i]);
    else if (ARG("--full")) full_from = atol(argv[++i]);
    else if (ARG("--frames")) max_frames = atol(argv[++i]);
    else if (ARG("--state-sparse")) state_sparse = atol(argv[++i]);
    else if (ARG("--out")) out = argv[++i];
    else if (ARG("--handover")) ho_seed = (uint32_t)strtoul(argv[++i], NULL, 0);
#ifdef DIFFTEST_STATS
    else if (ARG("--stats")) { g_stats = fopen(argv[++i], "w"); if (!g_stats) die("can't write stats"); }
#endif
    else if (!strcmp(a, "--fault")) {
#ifdef DIFFTEST_PARTITION
      g_fault = 1;
#endif
#ifdef DIFFTEST_CORE
      g_core_fault = 1;
#endif
    }
    else if (!strcmp(a, "--thread")) {
#if defined(DIFFTEST_PARTITION) && defined(DIFFTEST_THREADS)
      g_threaded = 1;
#else
      die("--thread needs the patched build with DIFFTEST_THREADS");
#endif
    }
    else die("unknown argument %s", a);
  }
  if (!set || !session || !trace || !out) die("usage: --set F --session F --trace F --out LOG [...]");
  if (strcmp(mode, "single") && strcmp(mode, "partition")) die("bad --mode");
  if (!strcmp(mode, "partition") && !strstr(features, "partition")) die("--mode partition needs --features partition");
#ifdef DIFFTEST_PARTITION
  parse_split(strcmp(mode, "partition") ? "none" : split);
#endif

  g_log = fopen(out, "w");
  if (!g_log) die("can't write %s", out);
  {
    size_t n;
    char* s = load_file(set, &n);
    g_set_json = s;
    hash_hex(s, n, h);
    fprintf(g_log, "H set %s\n", h);
    s = load_file(session, &n);
    g_session_json = s;
    hash_hex(s, n, h);
    fprintf(g_log, "H session %s\n", h);
    s = load_file(trace, &n);
    hash_hex(s, n, h);
    fprintf(g_log, "H trace %s\n", h);
    free(s);
    if (script) { s = load_file(script, &n); hash_hex(s, n, h); free(s); }
    fprintf(g_log, "H script %s\n", script ? h : "none");
  }
  fprintf(g_log, "H hardcore %d\n", hardcore);
  /* mode/split/features differ by design between the compared runs: logged as comments (#), not compared */
  fprintf(g_log, "# mode %s split %s features %s threaded %d\n", mode, split, features[0] ? features : "none",
#ifdef DIFFTEST_PARTITION
          g_threaded
#else
          0
#endif
          );
  g_total_achievements = count_achievements(g_set_json);
  trace_open(trace);
  fprintf(g_log, "H trace_seed %u frames %u\n", g_trace_seed, g_trace_frames);
  if (script) load_script(script);

#ifdef DIFFTEST_CORE
  return core_main(features, hardcore, full_from, max_frames, state_sparse, ho_seed);
#else
  (void)ho_seed;
#endif
  client = new_client(hardcore, features);
  fprintf(g_log, "F load\n");
  rc_client_begin_load_game(client, g_hash, load_done, NULL);

  while ((flags = trace_next(&frame)) >= 0) {
    if (max_frames >= 0 && (long)frames_done >= max_frames) break;
    g_millis = (uint64_t)frame * 1001u / 60u;
    fprintf(g_log, "F %u\n", frame);
    for (; ci < g_ncmds && g_cmds[ci].frame <= frame; ci++) {
      const command_t* c = &g_cmds[ci];
      fprintf(g_log, "C %s %s %u %u\n", c->cmd, c->a, c->b, c->c);
      if (!strcmp(c->cmd, "LOAD")) rc_client_begin_load_game(client, g_hash, load_done, NULL);
      else if (!strcmp(c->cmd, "UNLOAD")) rc_client_unload_game(client);
      else if (!strcmp(c->cmd, "RESET")) rc_client_reset(client);
      else if (!strcmp(c->cmd, "HARDCORE")) rc_client_set_hardcore_enabled(client, atoi(c->a));
      else if (!strcmp(c->cmd, "PAUSE")) paused = (int)strtol(c->a, NULL, 0);
      else if (!strcmp(c->cmd, "HITS")) hits_near_wrap(client, (uint32_t)strtoul(c->a, NULL, 0));
      else if (!strcmp(c->cmd, "SHORTREAD") && g_short_count < MAX_SHORT) {
        g_short[g_short_count].addr = (uint32_t)strtoul(c->a, NULL, 0);
        g_short[g_short_count].len = c->b;
        g_short[g_short_count].frames = c->c;
        g_short_count++;
        if (c->c) changed(g_short[g_short_count - 1].addr, c->b);
      }
      else if (!strcmp(c->cmd, "FAIL") && g_fail_count < 8) {
        snprintf(g_fail[g_fail_count].kind, sizeof(g_fail[0].kind), "%s", c->a);
        g_fail[g_fail_count].count = (int)c->b;
        g_fail_count++;
      }
      else die("unknown script command %s", c->cmd);
    }
    if (paused > 0 || (flags & FLAG_MENU)) {
      if (paused > 0) paused--;
      rc_client_idle(client);
    }
    else {
#ifdef DIFFTEST_CHANGES
      if (g_changes) {
        /* about one frame in 13: unknown */
        if (!g_changes_exact && ((frame * 2654435761u) >> 12) % 13 == 5)
          rc_client_set_changed_memory(client, NULL, 0);
        else
          rc_client_set_changed_memory(client, g_chg ? g_chg : &g_chg_none, g_chg_count);
      }
#endif
      rc_client_do_frame(client);
#ifdef DIFFTEST_CHANGES
      g_chg_count = 0;
#endif
    }
    for (i = 0; i < g_short_count; i++)
      if (g_short[i].frames && !--g_short[i].frames) changed(g_short[i].addr, g_short[i].len);
#ifdef DIFFTEST_STATS
    if (g_stats) {
      /* measurement: no state block (it serializes, which would settle owed tallies every frame) */
      fprintf(g_stats, "%u %lu %lu %lu %lu %lu\n", frame, g_st_eval, g_st_ach, g_st_test, g_st_walk, g_st_lb);
      g_st_eval = g_st_ach = g_st_test = g_st_walk = g_st_lb = 0;
      frames_done++;
      continue;
    }
#endif
    /* --state-sparse K: the state block (which serializes the progress) on about 1 frame in K,
     * so what change-driven evaluation owes skipped achievements builds up between them */
    if (state_sparse > 1 && ((frame * 2654435761u) >> 8) % (uint32_t)state_sparse != 0) {
      frames_done++;
      continue;
    }
    state_block(client);
    hash_hex(g_state, g_state_len, h);
    fprintf(g_log, "S %s\n", h);
    if (full_from >= 0 && (long)frame >= full_from) {
      char* p = g_state;
      while (*p) {
        char* e = strchr(p, '\n');
        if (e) *e = 0;
        fprintf(g_log, "T %s\n", p);
        if (!e) break;
        p = e + 1;
      }
    }
    frames_done++;
  }
#ifdef DIFFTEST_PARTITION
  fprintf(g_log, "# claimed %u, longest work list %u\n", g_claimed, g_work_max);
#endif
  fprintf(g_log, "END %u\n", frames_done);
  rc_client_destroy(client);
  fclose(g_log);
  return 0;
}
