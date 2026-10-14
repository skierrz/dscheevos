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
#define NFILE 5          /* A (reference) and F1, F2, MID, VB: what the log holds */
#define NCAND 6          /* plus CTRL: A's memory again (a second client; in step mode also resynced like the others) */
static const char* cname[NCAND] = {"A (last capture, today)", "F1 first capture", "F2 second capture", "MID capture (prev count / 2)", "VB at VBlank entry", "CTRL (A again)"};

typedef struct {
  uint8_t* img;
  uint8_t* touched;
  uint32_t* tlist;       /* offsets set in touched this frame */
  uint32_t ntl;
  rc_client_t* client;
  char events[4096];     /* this frame's achievement unlocks and leaderboard start/fail/submit (with values) */
  size_t nev;
  char events2[4096];    /* this frame's tracker, progress, challenge and other indicator events */
  size_t nev2;
  uint32_t digest, dm, dr;
  char rp[256];       /* rich presence message after this frame's do_frame */
  char* all;          /* every achievement/leaderboard start, fail, submit event, in order, with frame number */
  size_t nall, capall;
} ctx_t;

static char* set_json;
static uint32_t frameno;
static int quiet;      /* ignore events raised by a progress restore */

static uint32_t read_mem(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client)
{
  ctx_t* c = (ctx_t*)rc_client_get_userdata(client);
  uint32_t i;
  if (address < NMAIN && address + num_bytes <= NMAIN) {
    memcpy(buffer, c->img + address, num_bytes);
    for (i = 0; i < num_bytes; i++) if (!c->touched[address + i]) { c->touched[address + i] = 1; c->tlist[c->ntl++] = address + i; }
    return num_bytes;
  }
  if (address >= 0x1000000u && address + num_bytes <= 0x1004000u) {
    memcpy(buffer, c->img + NMAIN + (address - 0x1000000u), num_bytes);
    for (i = 0; i < num_bytes; i++) { uint32_t o = NMAIN + address - 0x1000000u + i; if (!c->touched[o]) { c->touched[o] = 1; c->tlist[c->ntl++] = o; } }
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
  if (quiet) return;
  switch (e->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
      n = snprintf(tmp, sizeof tmp, "ach %u;", e->achievement->id); break;
    case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
    case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
    case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
      n = snprintf(tmp, sizeof tmp, "lb %d %u %s;", (int)e->type, e->leaderboard->id, e->leaderboard->tracker_value ? e->leaderboard->tracker_value : ""); break;
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
  if (n > 0 && (e->type == RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED || e->type == RC_CLIENT_EVENT_LEADERBOARD_STARTED ||
      e->type == RC_CLIENT_EVENT_LEADERBOARD_FAILED || e->type == RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED)) {
    if (c->nev + (size_t)n < sizeof c->events) { memcpy(c->events + c->nev, tmp, (size_t)n); c->nev += (size_t)n; }
    char fb[24];
    int fn = snprintf(fb, sizeof fb, "@%u ", frameno);
    if (c->nall + (size_t)n + (size_t)fn + 1 > c->capall) { c->capall = c->capall * 2 + 4096; c->all = realloc(c->all, c->capall); }
    memcpy(c->all + c->nall, fb, (size_t)fn); c->nall += (size_t)fn;
    memcpy(c->all + c->nall, tmp, (size_t)n); c->nall += (size_t)n; c->all[c->nall] = 0;
  }
  else if (n > 0 && c->nev2 + (size_t)n < sizeof c->events2) { memcpy(c->events2 + c->nev2, tmp, (size_t)n); c->nev2 += (size_t)n; }
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
  int lines = 0;
  if (reports >= 2) return;
  reports++;
  for (; sa && sb; sa = sa->next, sb = sb->next) {
    for (i = 0; i < sa->public_.num_achievements; i++)
      if (lines < 4 && trig_digest(sa->achievements[i].trigger) != trig_digest(sb->achievements[i].trigger)) {
        lines++;
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

/* the running value of every leaderboard (measured/tracked), as rc_client holds it */
static uint32_t lbvalue_digest(rc_client_t* client)
{
  const rc_client_subset_info_t* sub;
  uint32_t i, h = 2166136261u;
  for (sub = client->game->subsets; sub; sub = sub->next)
    for (i = 0; i < sub->public_.num_leaderboards; i++) {
      h = (h ^ (uint32_t)sub->leaderboards[i].value) * 16777619u;
      h = (h ^ (uint32_t)sub->leaderboards[i].public_.state) * 16777619u;
    }
  return h;
}

static ctx_t* make(void)
{
  ctx_t* c = calloc(1, sizeof *c);
  c->img = calloc(1, NIMG); c->touched = calloc(1, NIMG); c->tlist = malloc(NIMG * sizeof(uint32_t));
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
typedef struct { uint32_t off, len, at; } run_t;
typedef struct { run_t* r; uint32_t n, cap; uint8_t* data; size_t dn, dcap; } runs_t;
/* read one run list from the log into rs (offsets, lengths and bytes); -1 at end/short file */
static int readruns(FILE* f, runs_t* rs)
{
  uint32_t n = rd32(f), i;
  if (n == 0xFFFFFFFFu) return -1;
  rs->n = 0; rs->dn = 0;
  for (i = 0; i < n; i++) {
    uint32_t off = rd32(f), len = rd32(f);
    if (off == 0xFFFFFFFFu || off + len > NIMG) return -1;
    if (rs->n == rs->cap) { rs->cap = rs->cap * 2 + 1024; rs->r = realloc(rs->r, rs->cap * sizeof(run_t)); }
    if (rs->dn + len > rs->dcap) { rs->dcap = (rs->dn + len) * 2 + 65536; rs->data = realloc(rs->data, rs->dcap); }
    if (fread(rs->data + rs->dn, 1, len, f) != len) return -1;
    rs->r[rs->n].off = off; rs->r[rs->n].len = len; rs->r[rs->n].at = (uint32_t)rs->dn; rs->n++; rs->dn += len;
  }
  return 0;
}
static void applyruns(const runs_t* rs, uint8_t* img)
{
  uint32_t i;
  for (i = 0; i < rs->n; i++) memcpy(img + rs->r[i].off, rs->data + rs->r[i].at, rs->r[i].len);
}

int main(int argc, char** argv)
{
  FILE* f;
  long sz;
  char magic[4];
  ctx_t* cx[NCAND];
  static runs_t rs[NFILE];
  int k;
  uint32_t frames = 0, skip = argc > 3 ? (uint32_t)atoi(argv[3]) : 120, seen = 0;
  int step = argc > 4 && !strcmp(argv[4], "step");
  size_t pcap = 0;
  uint8_t* pbuf = NULL;
  uint64_t watched_sum = 0, ncsum = 0;
  uint32_t ncmax = 0, ncone = 0, nocap = 0;
  uint32_t rpdiff[NCAND] = {0}, lbvdiff[NCAND] = {0}, firstev[NCAND] = {0}, ev2diff[NCAND] = {0}, firststate[NCAND] = {0};
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
  pcap = rc_client_progress_size(cx[0]->client) + 65536; pbuf = malloc(pcap);
  printf("mode: %s\n", step ? "one-step (every candidate is resynced to A's evaluated state before each frame)" : "free-running (each candidate keeps its own evaluated state)");
  for (;;) {
    uint32_t fr = rd32(f), ncap, i;
    int have[NCAND];
    if (fr == 0xFFFFFFFFu) break;
    ncap = rd32(f);
    if (ncap == 0xFFFFFFFFu) break;
    if (ncap == 0) { nocap++; continue; }   /* no capture in this interval: the module evaluates nothing new */
    ncsum += ncap; if (ncap > ncmax) ncmax = ncap; if (ncap == 1) ncone++;
    if (readruns(f, &rs[0])) break;
    frames_total_v++;
    for (k = 0; k < NCAND; k++) applyruns(&rs[0], cx[k]->img);   /* every image is A's memory now */
    have[0] = 1; have[5] = 1;
    for (k = 1; k < NFILE; k++) {
      int h = fgetc(f);
      have[k] = h == 1;
      rs[k].n = 0;
      if (h == 1 && readruns(f, &rs[k])) { have[k] = -1; break; }
    }
    for (k = 1; k < NFILE; k++) if (have[k] < 0) goto end;
    frameno++;
    frames++;
    if (frames <= skip) for (k = 1; k < NFILE; k++) rs[k].n = 0;   /* warm-up: every client sees A's memory, so they start in the same state */
    for (k = 1; k < NFILE; k++) applyruns(&rs[k], cx[k]->img);
    if (step && frames > 1) {   /* every candidate starts the frame from A's evaluated state (one-step comparison) */
      int r = rc_client_serialize_progress_sized(cx[0]->client, pbuf, pcap);
      if (r != RC_OK) { fprintf(stderr, "serialize %d\n", r); return 3; }
      for (k = 1; k < NCAND; k++) {
        quiet = 1;
        r = rc_client_deserialize_progress_sized(cx[k]->client, pbuf, pcap);
        quiet = 0;
        if (r != RC_OK) { fprintf(stderr, "deserialize %d\n", r); return 3; }
      }
    }
    for (k = 0; k < NCAND; k++) {
      for (i = 0; i < cx[k]->ntl; i++) cx[k]->touched[cx[k]->tlist[i]] = 0;
      cx[k]->ntl = 0;
      cx[k]->nev = 0; cx[k]->nev2 = 0;
      rc_client_do_frame(cx[k]->client);
      cx[k]->events[cx[k]->nev] = 0; cx[k]->events2[cx[k]->nev2] = 0;
      cx[k]->digest = digest_of(cx[k]->client); cx[k]->dm = comp[0]; cx[k]->dr = comp[1];
      cx[k]->rp[0] = 0;
      if (rc_client_has_rich_presence(cx[k]->client)) rc_client_get_rich_presence_message(cx[k]->client, cx[k]->rp, sizeof cx[k]->rp);
    }
    if (frames <= skip) continue;
    seen++;
    watched_sum += cx[0]->ntl;
    for (k = 1; k < NCAND; k++) {
      uint32_t w = 0, full = 0, ri;
      present[k] += have[k] ? 1 : 0;
      if (k < NFILE) for (ri = 0; ri < rs[k].n; ri++) {
        uint32_t o = rs[k].r[ri].off, e = o + rs[k].r[ri].len;
        for (; o < e; o++) {
          int d = cx[k]->img[o] != cx[0]->img[o];
          full += d;
          if (d && (cx[0]->touched[o] || cx[k]->touched[o])) w++;
        }
      }
      if (full) snapdiff[k]++;
      if (w) watchdiff[k]++;
      sumw[k] += w;
      if (w > maxw[k]) maxw[k] = w;
      if (full > maxfull[k]) maxfull[k] = full;
      if (cx[k]->digest != cx[0]->digest) { statediff[k]++; if (!firststate[k]) firststate[k] = fr; }
      if (cx[k]->dm != cx[0]->dm) memdiff[k]++;
      if (cx[k]->dr != cx[0]->dr) { if (!trigdiff[k]) explain(cname[k], fr, cx[0]->client, cx[k]->client); trigdiff[k]++; }
      if (strcmp(cx[k]->rp, cx[0]->rp)) rpdiff[k]++;
      if (strcmp(cx[k]->events2, cx[0]->events2)) ev2diff[k]++;
      if (lbvalue_digest(cx[k]->client) != lbvalue_digest(cx[0]->client)) lbvdiff[k]++;
      if (strcmp(cx[k]->events, cx[0]->events)) {
        evdiff[k]++;
        if (evdiff[k] <= 3) fprintf(stderr, "EVENT DIFF %s frame %u: A [%.240s] vs [%.240s]\n", cname[k], fr, cx[0]->events, cx[k]->events);
        if (!firstev[k]) firstev[k] = fr;
      }
    }
    for (k = 1; k < NFILE; k++) {   /* undo the candidates' runs: every image is A's memory again */
      uint32_t ri;
      for (ri = 0; ri < rs[k].n; ri++) memcpy(cx[k]->img + rs[k].r[ri].off, cx[0]->img + rs[k].r[ri].off, rs[k].r[ri].len);
    }
    if (cx[0]->nev) evcount[0] += 1;
  }
  /* (the frame loop continues above; candidate runs are undone at the top of the next frame) */
end:
  if (getenv("CO_EVENTS")) {   /* every client's full event list, one file each, for copyonce_report.py */
    static const char* nm[NCAND] = {"A", "F1", "F2", "MID", "VB", "CTRL"};
    for (k = 0; k < NCAND; k++) {
      char fn[1024]; FILE* o;
      snprintf(fn, sizeof fn, "%s.%s.txt", getenv("CO_EVENTS"), nm[k]);
      o = fopen(fn, "w");
      if (o) { fputs(cx[k]->all ? cx[k]->all : "", o); fclose(o); }
    }
  }
  printf("frames replayed %u (counted %u), mean watched bytes per frame %.0f\n", frames, seen, seen ? (double)watched_sum / seen : 0.0);
  printf("intervals with no capture (module evaluates nothing new, not replayed): %u; intervals with at least one capture: %u\n", nocap, frames_total());
  printf("captures per interval (intervals with at least one): mean %.1f, max %u, exactly one %u\n", frames_total() ? (double)ncsum / frames_total() : 0.0, ncmax, ncone);
  for (k = 0; k < NCAND; k++) {
    const char* a = cx[k]->all ? cx[k]->all : "";
    size_t ne = 0; const char* q;
    for (q = a; *q; q++) if (*q == ';') ne++;
    printf("%s: %zu achievement/leaderboard events (start, fail, submit, unlock); first: %.700s%s\n", cname[k], ne, ne ? a : "(none)", strlen(a) > 700 ? " ..." : "");
  }
  for (k = 1; k < NCAND; k++)
    printf("%-30s copy exists %u  full-RAM-differs %u  watched-differs %u (%.2f%% of frames)  max watched bytes %u  mean %.3f  max full bytes %u  state-differs %u  event-frames-differ %u (first at %u; first state difference at %u)  all events equal at end: %s\n",
           cname[k], present[k], snapdiff[k], watchdiff[k], seen ? 100.0 * watchdiff[k] / seen : 0.0, maxw[k],
           seen ? (double)sumw[k] / seen : 0.0, maxfull[k], statediff[k], evdiff[k], firstev[k], firststate[k],
           strcmp(cx[k]->all ? cx[k]->all : "", cx[0]->all ? cx[0]->all : "") ? "NO" : "yes");
  for (k = 1; k < NCAND; k++) printf("  %-30s rich presence differs in %u frames, leaderboard running value/state differs in %u frames, tracker/progress/challenge events differ in %u frames\n", cname[k], rpdiff[k], lbvdiff[k], ev2diff[k]);
  for (k = 1; k < NCAND; k++) printf("  %-30s frames where memref values differ %u, achievement/leaderboard trigger state differs %u\n", cname[k], memdiff[k], trigdiff[k]);
  (void)evcount; (void)maxfull;
  return 0;
}
