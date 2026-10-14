/*
 * Measurement only (research/optimize/copyonce.md). Replays the log of the melonDS runner's
 * --copyonce option: per VBlank interval the game's RAM at the module's snapshot copy (A, the last
 * one in the interval: today's method) and at other copy points. Runs stock rcheevos
 * (rc_client_do_frame) on A and on each candidate with the same real set, and reports how often the
 * watched bytes differ and whether any achievement or leaderboard event, or any evaluated state,
 * differs.
 *
 *   copyonce <set.json> <log> [warm-up frames evaluated on A's memory in every client and not counted, default 120]
 */
#include "rc_client.h"
#include "client/rc_client_internal.h"
#include "runtime/rc_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NMAIN 0x400000u
#define NIMG 0x404000u
#define NCAND 5          /* A (reference) and F1, F2, MID, VB */
static const char* cname[NCAND] = {"A (last capture, today)", "F1 first capture", "F2 second capture", "MID capture (prev count / 2)", "VB at VBlank entry"};

typedef struct {
  uint8_t* img;
  uint8_t* touched;
  rc_client_t* client;
  char events[4096];
  size_t nev;
  uint32_t digest, dm, dr;
  char* all;          /* every event, in order, without frame numbers */
  size_t nall, capall;
} ctx_t;

static char* set_json;
static uint32_t frameno;

static uint32_t read_mem(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client)
{
  ctx_t* c = (ctx_t*)rc_client_get_userdata(client);
  uint32_t i;
  if (address < NMAIN && address + num_bytes <= NMAIN) {
    memcpy(buffer, c->img + address, num_bytes);
    for (i = 0; i < num_bytes; i++) c->touched[address + i] = 1;
    return num_bytes;
  }
  if (address >= 0x1000000u && address + num_bytes <= 0x1004000u) {
    memcpy(buffer, c->img + NMAIN + (address - 0x1000000u), num_bytes);
    for (i = 0; i < num_bytes; i++) c->touched[NMAIN + address - 0x1000000u + i] = 1;
    return num_bytes;
  }
  return 0;
}

static const char* answer(const char* post)
{
  if (strstr(post, "r=login2"))
    return "{\"Success\":true,\"User\":\"User\",\"AvatarUrl\":\"http://server/UserPic/USER.png\",\"Token\":\"ApiToken\",\"Score\":1,\"Messages\":0}";
  if (strstr(post, "r=achievementsets")) return set_json;
  if (strstr(post, "r=startsession")) return "{\"Success\":true,\"Unlocks\":[],\"HardcoreUnlocks\":[]}";
  return "{\"Success\":true}";
}
static void server(const rc_api_request_t* r, rc_client_server_callback_t cb, void* data, rc_client_t* client)
{
  rc_api_server_response_t resp;
  (void)client;
  memset(&resp, 0, sizeof(resp));
  resp.body = answer(r->post_data ? r->post_data : "");
  resp.body_length = strlen(resp.body);
  resp.http_status_code = 200;
  cb(&resp, data);
}
static void done(int result, const char* message, rc_client_t* client, void* ud)
{
  (void)client; (void)ud;
  if (result != RC_OK) fprintf(stderr, "rc_client: %d %s\n", result, message ? message : "");
}
static rc_clock_t clk(const rc_client_t* c) { (void)c; return (rc_clock_t)(frameno * 1000u / 60u); }

static void event(const rc_client_event_t* e, rc_client_t* client)
{
  ctx_t* c = (ctx_t*)rc_client_get_userdata(client);
  char tmp[160];
  int n = 0;
  switch (e->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
      n = snprintf(tmp, sizeof tmp, "ach %u;", e->achievement->id); break;
    case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
    case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
    case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
      n = snprintf(tmp, sizeof tmp, "lb %d %u;", (int)e->type, e->leaderboard->id); break;
    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE:
      n = snprintf(tmp, sizeof tmp, "trk %d %u %s;", (int)e->type, e->leaderboard_tracker->id, e->leaderboard_tracker->display); break;
    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE:
      n = snprintf(tmp, sizeof tmp, "trkhide %u;", e->leaderboard_tracker->id); break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
      n = snprintf(tmp, sizeof tmp, "prog %u %s;", e->achievement->id, e->achievement->measured_progress); break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
    case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
      n = snprintf(tmp, sizeof tmp, "chal %d %u;", (int)e->type, e->achievement->id); break;
    default:
      n = snprintf(tmp, sizeof tmp, "ev %d;", (int)e->type); break;
  }
  if (n > 0 && c->nev + (size_t)n < sizeof c->events) { memcpy(c->events + c->nev, tmp, (size_t)n); c->nev += (size_t)n; }
  if (n > 0 && (e->type == RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED || e->type == RC_CLIENT_EVENT_LEADERBOARD_STARTED ||
      e->type == RC_CLIENT_EVENT_LEADERBOARD_FAILED || e->type == RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED)) {
    if (c->nall + (size_t)n + 1 > c->capall) { c->capall = c->capall * 2 + 4096; c->all = realloc(c->all, c->capall); }
    memcpy(c->all + c->nall, tmp, (size_t)n); c->nall += (size_t)n; c->all[c->nall] = 0;
  }
}

static uint32_t dg;
static void mix(uint32_t v) { int i; for (i = 0; i < 4; i++) { dg ^= (v >> (i * 8)) & 0xFF; dg *= 16777619u; } }
static void mix_trigger(const rc_trigger_t* t)
{
  const rc_condset_t* cs;
  const rc_condition_t* cd;
  if (!t) return;
  mix(t->state); mix(t->measured_value);
  for (cs = t->requirement; cs; cs = cs->next) for (cd = cs->conditions; cd; cd = cd->next) { mix(cd->current_hits); mix(cd->is_true); }
  for (cs = t->alternative; cs; cs = cs->next) for (cd = cs->conditions; cd; cd = cd->next) { mix(cd->current_hits); mix(cd->is_true); }
}
static uint32_t comp[4];
static uint32_t digest_of(rc_client_t* client)
{
  const rc_client_game_info_t* g = client->game;
  const rc_client_subset_info_t* sub;
  const rc_memref_list_t* ml;
  uint32_t i;
  dg = 2166136261u;
  for (ml = &g->runtime.memrefs->memrefs; ml; ml = ml->next)
    for (i = 0; i < ml->count; i++) mix(ml->items[i].value.value);
  comp[0] = dg; dg = 2166136261u;
  for (sub = g->subsets; sub; sub = sub->next) {
    for (i = 0; i < sub->public_.num_achievements; i++) {
      mix(sub->achievements[i].public_.state);
      comp[3] ^= 0; mix_trigger(sub->achievements[i].trigger);
    }
    for (i = 0; i < sub->public_.num_leaderboards; i++) {
      const rc_client_leaderboard_info_t* l = &sub->leaderboards[i];
      mix(l->public_.state); mix((uint32_t)l->value);
      if (l->lboard) { mix_trigger(&l->lboard->start); mix_trigger(&l->lboard->submit); mix_trigger(&l->lboard->cancel); }
    }
  }
  comp[1] = dg;
  return comp[0] * 31u + comp[1];
}


/* Which achievements and leaderboards differ between clients a and b (first few reports only). */
static uint32_t trig_digest(const rc_trigger_t* t) { dg = 2166136261u; mix_trigger(t); return dg; }
static void explain(const char* name, uint32_t fr, rc_client_t* a, rc_client_t* b)
{
  static int reports;
  const rc_client_subset_info_t *sa = a->game->subsets, *sb = b->game->subsets;
  uint32_t i;
  if (reports >= 6) return;
  reports++;
  for (; sa && sb; sa = sa->next, sb = sb->next) {
    for (i = 0; i < sa->public_.num_achievements; i++)
      if (trig_digest(sa->achievements[i].trigger) != trig_digest(sb->achievements[i].trigger)) {
        const rc_condition_t *ca = sa->achievements[i].trigger ? sa->achievements[i].trigger->requirement->conditions : NULL;
        const rc_condition_t *cb = sb->achievements[i].trigger ? sb->achievements[i].trigger->requirement->conditions : NULL;
        fprintf(stderr, "STATE DIFF %s frame %u: achievement %u state %d/%d measured %u/%u; first-core hits:", name, fr, sa->achievements[i].public_.id,
          (int)sa->achievements[i].public_.state, (int)sb->achievements[i].public_.state,
          sa->achievements[i].trigger ? sa->achievements[i].trigger->measured_value : 0, sb->achievements[i].trigger ? sb->achievements[i].trigger->measured_value : 0);
        for (; ca && cb; ca = ca->next, cb = cb->next) if (ca->current_hits != cb->current_hits || ca->is_true != cb->is_true) fprintf(stderr, " [%u/%u true %d/%d]", ca->current_hits, cb->current_hits, ca->is_true, cb->is_true);
        fprintf(stderr, "\n");
      }
    for (i = 0; i < sa->public_.num_leaderboards; i++)
      if (sa->leaderboards[i].public_.state != sb->leaderboards[i].public_.state || sa->leaderboards[i].value != sb->leaderboards[i].value)
        fprintf(stderr, "STATE DIFF %s frame %u: leaderboard %u state %d/%d value %d/%d\n", name, fr, sa->leaderboards[i].public_.id,
          (int)sa->leaderboards[i].public_.state, (int)sb->leaderboards[i].public_.state, (int)sa->leaderboards[i].value, (int)sb->leaderboards[i].value);
  }
}

static ctx_t* make(void)
{
  ctx_t* c = calloc(1, sizeof *c);
  c->img = calloc(1, NIMG); c->touched = calloc(1, NIMG);
  c->client = rc_client_create(read_mem, server);
  rc_client_set_userdata(c->client, c);
  rc_client_set_event_handler(c->client, event);
  rc_client_set_get_time_millisecs_function(c->client, clk);
  rc_client_set_hardcore_enabled(c->client, 1);
  rc_client_begin_login_with_token(c->client, "User", "ApiToken", done, NULL);
  rc_client_begin_load_game(c->client, "0123456789ABCDEF0123456789ABCDEF", done, NULL);
  if (!rc_client_is_game_loaded(c->client)) { fprintf(stderr, "game not loaded\n"); exit(1); }
  return c;
}

static uint32_t frames_total_v;
#define frames_total() frames_total_v
static uint32_t rd32(FILE* f) { uint32_t v; if (fread(&v, 4, 1, f) != 1) return 0xFFFFFFFFu; return v; }
static int apply(FILE* f, uint8_t* img)
{
  uint32_t n = rd32(f), i;
  for (i = 0; i < n; i++) {
    uint32_t off = rd32(f), len = rd32(f);
    if (off == 0xFFFFFFFFu || off + len > NIMG || fread(img + off, 1, len, f) != len) return -1;
  }
  return n == 0xFFFFFFFFu ? -1 : 0;
}

int main(int argc, char** argv)
{
  FILE* f;
  long sz;
  char magic[4];
  ctx_t* cx[NCAND];
  uint8_t* cleanA = malloc(NIMG);
  int k;
  uint32_t frames = 0, skip = argc > 3 ? (uint32_t)atoi(argv[3]) : 120, seen = 0;
  uint64_t watched_sum = 0, ncsum = 0;
  uint32_t ncmax = 0, ncone = 0;
  uint32_t present[NCAND] = {0}, snapdiff[NCAND] = {0}, watchdiff[NCAND] = {0}, statediff[NCAND] = {0}, evdiff[NCAND] = {0};
  uint32_t memdiff[NCAND] = {0}, trigdiff[NCAND] = {0};
  uint32_t maxw[NCAND] = {0}, maxfull[NCAND] = {0}, evcount[NCAND] = {0};
  uint64_t sumw[NCAND] = {0};
  if (argc < 3) { fprintf(stderr, "usage: copyonce set.json log [skip]\n"); return 2; }
  f = fopen(argv[1], "rb");
  if (!f) return 2;
  fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
  set_json = malloc((size_t)sz + 1); if (fread(set_json, 1, (size_t)sz, f) != (size_t)sz) return 2; set_json[sz] = 0; fclose(f);
  f = fopen(argv[2], "rb");
  if (!f || fread(magic, 1, 4, f) != 4 || memcmp(magic, "COV1", 4)) { fprintf(stderr, "bad log\n"); return 2; }
  for (k = 0; k < NCAND; k++) cx[k] = make();
  for (;;) {
    uint32_t fr = rd32(f), ncap, i;
    int have[NCAND];
    if (fr == 0xFFFFFFFFu) break;
    ncap = rd32(f); ncsum += ncap; if (ncap > ncmax) ncmax = ncap; if (ncap == 1) ncone++;
    if (apply(f, cx[0]->img)) break;
    memcpy(cleanA, cx[0]->img, NIMG);
    have[0] = 1;
    for (k = 1; k < NCAND; k++) {
      int h = fgetc(f);
      have[k] = h == 1;
      memcpy(cx[k]->img, cleanA, NIMG);
      if (h == 1 && apply(f, cx[k]->img)) { have[k] = -1; break; }
    }
    for (k = 1; k < NCAND; k++) if (have[k] < 0) goto end;
    frameno++;
    frames++; frames_total_v = frames;
    if (frames <= skip)    /* warm-up: every client sees A's memory, so the clients start in the same state */
      for (k = 1; k < NCAND; k++) memcpy(cx[k]->img, cleanA, NIMG);
    for (k = 0; k < NCAND; k++) {
      memset(cx[k]->touched, 0, NIMG);
      cx[k]->nev = 0;
      rc_client_do_frame(cx[k]->client);
      cx[k]->events[cx[k]->nev] = 0;
      cx[k]->digest = digest_of(cx[k]->client); cx[k]->dm = comp[0]; cx[k]->dr = comp[1];
    }
    if (frames <= skip) continue;
    seen++;
    for (i = 0, k = 0; i < NIMG; i++) watched_sum += cx[0]->touched[i];
    for (k = 1; k < NCAND; k++) {
      uint32_t w = 0, full = 0;
      present[k] += have[k] ? 1 : 0;
      for (i = 0; i < NIMG; i++) {
        int d = cx[k]->img[i] != cleanA[i];
        full += d;
        if (d && (cx[0]->touched[i] || cx[k]->touched[i])) w++;
      }
      if (full) snapdiff[k]++;
      if (w) watchdiff[k]++;
      sumw[k] += w;
      if (w > maxw[k]) maxw[k] = w;
      if (full > maxfull[k]) maxfull[k] = full;
      if (cx[k]->digest != cx[0]->digest) statediff[k]++;
      if (cx[k]->dm != cx[0]->dm) memdiff[k]++;
      if (cx[k]->dr != cx[0]->dr) { if (!trigdiff[k]) explain(cname[k], fr, cx[0]->client, cx[k]->client); trigdiff[k]++; }
      if (strcmp(cx[k]->events, cx[0]->events)) {
        evdiff[k]++;
        fprintf(stderr, "EVENT DIFF %s frame %u: A [%s] vs [%s]\n", cname[k], fr, cx[0]->events, cx[k]->events);
      }
    }
    if (cx[0]->nev) evcount[0] += 1;
  }
end:
  printf("frames replayed %u (counted %u), mean watched bytes per frame %.0f\n", frames, seen, seen ? (double)watched_sum / seen : 0.0);
  printf("captures per interval (intervals with at least one): mean %.1f, max %u, exactly one %u\n", frames_total() ? (double)ncsum / frames_total() : 0.0, ncmax, ncone);
  printf("events in A (achievement/leaderboard start, fail, submit): %s\n", cx[0]->all ? cx[0]->all : "(none)");
  for (k = 1; k < NCAND; k++)
    printf("%-30s copy exists %u  full-RAM-differs %u  watched-differs %u (%.2f%% of frames)  max watched bytes %u  mean %.3f  max full bytes %u  state-differs %u  event-frames-differ %u  all events equal at end: %s\n",
           cname[k], present[k], snapdiff[k], watchdiff[k], seen ? 100.0 * watchdiff[k] / seen : 0.0, maxw[k],
           seen ? (double)sumw[k] / seen : 0.0, maxfull[k], statediff[k], evdiff[k],
           strcmp(cx[k]->all ? cx[k]->all : "", cx[0]->all ? cx[0]->all : "") ? "NO" : "yes");
  for (k = 1; k < NCAND; k++) printf("  %-30s frames where memref values differ %u, achievement/leaderboard trigger state differs %u\n", cname[k], memdiff[k], trigdiff[k]);
  (void)evcount; (void)maxfull;
  return 0;
}
