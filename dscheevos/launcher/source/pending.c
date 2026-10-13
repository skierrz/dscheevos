#include "pending.h"
#include "log.h"
#include "net.h"
#include "session.h"
#include "ui.h"
#include "dscheevos_module.h"

#include "rc_api_runtime.h"
#include "rc_error.h"

#include <dirent.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define PENDING_PATH DSC_DIR "pending.bin"
#define UNSENT_DIR DSC_DIR "unsent"
/* written when booting a spectator session: its journal is never sent */
#define SPECTATOR_MARK DSC_DIR "pending.spectator"

/* rcheevos' own rule for "try again later" (rc_client.c, not in a public header) */
int rc_client_should_retry(const rc_api_server_response_t* server_response);

static uint8_t g_file[DSCM_JOURNAL_FILE_SIZE];
static int g_stop;   /* the network is down: don't try the rest now */
static int g_told;   /* "Sending..." is on screen */

static int file_exists(const char* path)
{
  struct stat st;
  return stat(path, &st) == 0;
}

/* Reads a journal file; returns 0 if it holds none (cleared, short or bad). */
static int read_journal(const char* path)
{
  const dscm_journal_t* j = (const dscm_journal_t*)g_file;
  size_t n;
  FILE* f = fopen(path, "rb");
  if (!f)
    return 0;
  memset(g_file, 0, sizeof(g_file));
  n = fread(g_file, 1, sizeof(g_file), f);
  fclose(f);
  return n >= sizeof(dscm_journal_t) && j->magic == DSCM_JOURNAL_MAGIC && j->size == sizeof(dscm_journal_t)
      && j->count <= DSCM_JOURNAL_MAX && memchr(j->username, 0, sizeof(j->username))
      && memchr(j->game_hash, 0, sizeof(j->game_hash));
}

static int write_file(const char* path, const char* mode, const void* data)
{
  int err;
  FILE* f = fopen(path, mode);
  if (!f)
    return 0;
  fwrite(data, 1, DSCM_JOURNAL_FILE_SIZE, f);
  err = ferror(f);
  fclose(f);
  return !err;
}

/* Zeroes pending.bin in place (same size, same clusters). */
static void clear_pending(void)
{
  static const uint8_t zero[DSCM_JOURNAL_FILE_SIZE];
  if (!write_file(PENDING_PATH, "r+b", zero))
    write_file(PENDING_PATH, "wb", zero);
}

static unsigned unsent_entries(const dscm_journal_t* j)
{
  unsigned i, n = 0;
  for (i = 0; i < j->count; i++)
    if (!j->e[i].sent)
      n++;
  return n;
}

/* Keeps g_file's journal for a later start. */
static void keep_unsent(void)
{
  char path[64];
  unsigned i;
  mkdir(UNSENT_DIR, 0777);
  for (i = 0; i < 1000; i++) {
    snprintf(path, sizeof(path), UNSENT_DIR "/%03u.bin", i);
    if (!file_exists(path))
      break;
  }
  if (i == 1000 || !write_file(path, "wb", g_file))
    log_line("pending: couldn't keep unsent entries");
}

enum { ENTRY_KEEP, ENTRY_SENT, ENTRY_DROP };

/* Sends one entry. ENTRY_SENT when the server answered for good (accepted,
 * already unlocked, or refused for a reason a retry won't change),
 * ENTRY_DROP when it can't be sent at all, ENTRY_KEEP to retry later. */
static int send_entry(const dsc_settings_t* settings, const dscm_journal_t* j, const dscm_journal_entry_t* e,
                      uint32_t now)
{
  rc_api_host_t host;
  rc_api_request_t request;
  rc_api_server_response_t response;
  http_response_t r;
  uint32_t ago = now > e->unix_time ? now - e->unix_time : 0;
  int result, done;

  memset(&host, 0, sizeof(host));
  host.host = settings->host;
  if (e->kind == DSCM_JOURNAL_AWARD) {
    rc_api_award_achievement_request_t params;
    memset(&params, 0, sizeof(params));
    params.username = settings->username;
    params.api_token = settings->token;
    params.achievement_id = e->id;
    params.hardcore = e->hardcore;
    params.game_hash = j->game_hash;
    params.seconds_since_unlock = ago;
    result = rc_api_init_award_achievement_request_hosted(&request, &params, &host);
  } else if (e->kind == DSCM_JOURNAL_LEADERBOARD) {
    rc_api_submit_lboard_entry_request_t params;
    memset(&params, 0, sizeof(params));
    params.username = settings->username;
    params.api_token = settings->token;
    params.leaderboard_id = e->id;
    params.score = e->score;
    params.game_hash = j->game_hash;
    params.seconds_since_completion = ago;
    result = rc_api_init_submit_lboard_entry_request_hosted(&request, &params, &host);
  } else {
    /* kind 0 is an entry the module was still writing */
    if (e->kind)
      log_line("pending: dropped entry %u of unknown kind %u", (unsigned)e->id, (unsigned)e->kind);
    return ENTRY_DROP;
  }
  if (result != RC_OK) {
    log_line("pending: dropped bad entry %u (%d)", (unsigned)e->id, result);
    return ENTRY_DROP;
  }

  http_request(request.url, request.post_data, request.content_type, &r);
  session_log_request(request.post_data, r.status);
  rc_api_destroy_request(&request);
  memset(&response, 0, sizeof(response));
  response.http_status_code = r.status > 0 ? r.status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
  response.body = r.body ? r.body : "";
  response.body_length = r.length;

  /* same rule as rc_client: success, or a server error that isn't retryable */
  if (e->kind == DSCM_JOURNAL_AWARD) {
    rc_api_award_achievement_response_t award;
    result = rc_api_process_award_achievement_server_response(&award, &response);
    done = (result == RC_OK && award.response.succeeded)
        || (award.response.error_message && !rc_client_should_retry(&response));
    if (award.response.error_message)
      log_line("achievement %u: %s", (unsigned)e->id, award.response.error_message);
    rc_api_destroy_award_achievement_response(&award);
  } else {
    rc_api_submit_lboard_entry_response_t entry;
    result = rc_api_process_submit_lboard_entry_server_response(&entry, &response);
    done = (result == RC_OK && entry.response.succeeded)
        || (entry.response.error_message && !rc_client_should_retry(&response));
    if (entry.response.error_message)
      log_line("leaderboard %u: %s", (unsigned)e->id, entry.response.error_message);
    rc_api_destroy_submit_lboard_entry_response(&entry);
  }
  if (r.status < 0)
    g_stop = 1;
  http_free(&r);
  return done ? ENTRY_SENT : ENTRY_KEEP;
}

/* Now, on the journal's time base. A UTC journal needs the console clock's
 * offset, measured on this start's server replies; without it the times are
 * unknown and the entries go as "just now" (0) rather than hours off. */
static uint32_t journal_now(const dscm_journal_t* j)
{
  uint32_t console = (uint32_t)time(NULL);
  int8_t q;
  if (j->time_base != DSCM_JOURNAL_TIME_UTC)
    return console;
  if (net_clock_offset_q(&q))
    return dscm_console_to_utc(console, q);
  log_line("pending: clock offset unknown, unlock times not sent");
  return 0;
}

/* Sends g_file's unsent entries; returns how many are still unsent. */
static unsigned send_journal(const dsc_settings_t* settings, const char* hash, pending_result_t* out)
{
  dscm_journal_t* j = (dscm_journal_t*)g_file;
  uint32_t now = journal_now(j);
  unsigned i, left = 0;
  int r;
  if (!g_told && unsent_entries(j)) {
    ui_status("Sending unlocks from last time...");
    g_told = 1;
  }
  for (i = 0; i < j->count; i++) {
    dscm_journal_entry_t* e = &j->e[i];
    if (e->sent)
      continue;
    r = g_stop ? ENTRY_KEEP : send_entry(settings, j, e, now);
    if (r == ENTRY_KEEP) {
      left++;
      continue;
    }
    e->sent = 1;
    if (r == ENTRY_DROP) {
      if (e->kind)
        out->dropped++;
    } else if (e->kind == DSCM_JOURNAL_LEADERBOARD) {
      out->sent_entries++;
    } else {
      out->sent_unlocks++;
      if (!strcasecmp(j->game_hash, hash))
        out->this_game++;
    }
  }
  out->failed += left;
  return left;
}

static int is_mine(const dsc_settings_t* settings)
{
  const dscm_journal_t* j = (const dscm_journal_t*)g_file;
  return !strcasecmp(j->username, settings->username);
}

/* Reads only a kept journal's header: 1 for this user's, 0 for another
 * user's, -1 for a file that holds no journal. */
static int unsent_owner(const dsc_settings_t* settings, const char* path)
{
  dscm_journal_t j;
  size_t n;
  FILE* f = fopen(path, "rb");
  if (!f)
    return -1;
  n = fread(&j, 1, offsetof(dscm_journal_t, e), f);
  fclose(f);
  if (n != offsetof(dscm_journal_t, e) || j.magic != DSCM_JOURNAL_MAGIC || j.size != sizeof(dscm_journal_t)
      || !memchr(j.username, 0, sizeof(j.username)))
    return -1;
  return !strcasecmp(j.username, settings->username);
}

/* The journals kept from earlier starts: send this user's, delete the ones
 * done. Other users' files are skipped while scanning, so they never use up
 * the names read per start. */
static void send_unsent_dir(const dsc_settings_t* settings, const char* hash, pending_result_t* out)
{
  static char names[32][8];
  char path[64];
  struct dirent* d;
  int i, n = 0;
  DIR* dir = opendir(UNSENT_DIR);
  if (!dir)
    return;
  while ((d = readdir(dir)) != NULL && n < 32) {
    if (strlen(d->d_name) != 7 || strcmp(d->d_name + 3, ".bin"))
      continue;
    snprintf(path, sizeof(path), UNSENT_DIR "/%.7s", d->d_name);
    /* bad files are taken too, to be removed below */
    if (unsent_owner(settings, path) != 0)
      strcpy(names[n++], d->d_name);
  }
  closedir(dir);
  for (i = 0; i < n && !g_stop; i++) {
    snprintf(path, sizeof(path), UNSENT_DIR "/%.7s", names[i]);
    if (!read_journal(path)) {
      remove(path);
      continue;
    }
    if (send_journal(settings, hash, out) == 0)
      remove(path);
    else
      write_file(path, "r+b", g_file);
  }
}

void pending_send(const dsc_settings_t* settings, const char* hash, pending_result_t* out)
{
  memset(out, 0, sizeof(*out));
  g_stop = g_told = 0;

  /* a spectator session's journal is never sent */
  if (file_exists(SPECTATOR_MARK)) {
    if (read_journal(PENDING_PATH)) {
      log_line("pending: dropped a spectator session's entries");
      clear_pending();
    }
    remove(SPECTATOR_MARK);
  }
  if (settings->spectator) {
    /* keep what's waiting for a normal session */
    if (read_journal(PENDING_PATH) && unsent_entries((dscm_journal_t*)g_file)) {
      log_line("pending: kept for a normal session");
      keep_unsent();
      clear_pending();
    }
    return;
  }

  send_unsent_dir(settings, hash, out);
  if (read_journal(PENDING_PATH)) {
    dscm_journal_t* j = (dscm_journal_t*)g_file;
    if (!is_mine(settings)) {
      if (unsent_entries(j))
        keep_unsent();
    } else if (send_journal(settings, hash, out)) {
      keep_unsent();
    }
    clear_pending();
  }
  if (out->sent_unlocks || out->sent_entries || out->failed || out->dropped)
    log_line("pending: sent %u unlocks, %u leaderboard entries; %u kept, %u dropped",
             out->sent_unlocks, out->sent_entries, out->failed, out->dropped);
}

int pending_prepare(const dsc_settings_t* settings)
{
  static const uint8_t zero[DSCM_JOURNAL_FILE_SIZE];
  struct stat st;
  if (stat(PENDING_PATH, &st) != 0 || st.st_size != (off_t)DSCM_JOURNAL_FILE_SIZE) {
    remove(PENDING_PATH);
    if (!write_file(PENDING_PATH, "wb", zero))
      return 0;
  }
  if (settings->spectator) {
    FILE* f = fopen(SPECTATOR_MARK, "wb");
    if (!f)
      return 0;
    fclose(f);
  }
  return 1;
}
