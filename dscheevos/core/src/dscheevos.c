/*
 * DScheevos core. See dscheevos.h for the model.
 *
 * Snapshot: rcheevos reads game memory through read_memory(). While a
 * snapshot is being evaluated, reads are served from it; any address it
 * doesn't hold is read live once and added to the address table, so the next
 * VBlank captures it too (this covers pointers that just moved).
 *
 * Address tables are immutable once published. dsc_vblank() copies the bytes
 * listed in the current table into the next snapshot as a few runs of
 * contiguous memory. Each snapshot remembers which table it was taken with.
 *
 * Snapshots queue in a ring: dsc_vblank (IRQ) adds at the head, dsc_idle
 * evaluates and frees at the tail, oldest first. Their bytes live in one
 * arena, also used as a ring, so each frame costs only the bytes watched
 * and a long stretch without idle time (loading, save writes) queues many
 * frames instead of dropping them.
 */
#include "dscheevos.h"

#include "rc_client.h"
#include "rc_consoles.h"

#include <stdlib.h>
#include <string.h>

#define MAX_SLOTS 1024
#define TABLES 3              /* current, plus up to two still used by queued snapshots */
#define MAX_PENDING_HTTP 8

typedef struct run_t {
  uint32_t address;           /* RA address of the first byte */
  uint32_t offset;            /* where it lands in the snapshot */
  uint32_t length;
} run_t;

typedef struct table_t {
  uint32_t* addresses;        /* sorted, unique, one entry per byte */
  uint32_t count;
  run_t* runs;
  uint32_t run_count;
  uint32_t generation;
} table_t;

enum { SLOT_FREE, SLOT_FILLING, SLOT_READY, SLOT_EVAL };

typedef struct slot_t {
  volatile int state;
  uint8_t* data;
  uint32_t offset;            /* in the arena */
  uint32_t size;
  const table_t* table;
  uint32_t frame;
  volatile uint32_t repeats;  /* later frames whose bytes matched exactly; evaluated from this copy */
} slot_t;

typedef struct http_pending_t {
  int used;
  int done;
  rc_client_server_callback_t callback;
  void* callback_data;
  int status;
  char* body;
  size_t body_length;
} http_pending_t;

static struct {
  int initialized;
  dsc_platform_t platform;
  dsc_config_t config;
  rc_client_t* client;
  int active;

  table_t tables[TABLES];
  table_t* volatile current;   /* read by dsc_vblank */
  uint32_t* pending_new;       /* addresses missed during evaluation, merged after the frame */
  uint32_t pending_new_count;

  slot_t slots[MAX_SLOTS];     /* ring: [slot_tail, slot_head) are queued */
  uint32_t slot_count;         /* most frames that may wait */
  volatile uint32_t slot_head; /* written by dsc_vblank only */
  volatile uint32_t slot_tail; /* written by dsc_idle only */
  uint8_t* arena;
  uint32_t arena_size;
  uint32_t arena_head;         /* end of the newest snapshot's bytes */
  slot_t* evaluating;
  volatile uint32_t frame_counter;

  http_pending_t http[MAX_PENDING_HTTP];
  char game_hash[33];

  dsc_stats_t stats;
} S;

static void dsc_log(const char* message)
{
  if (S.platform.log)
    S.platform.log(message);
}

static void emit(dsc_event_type_t type, uint32_t id, uint32_t points, const char* title, const char* detail)
{
  dsc_event_t e;
  if (!S.platform.event)
    return;
  e.type = type;
  e.id = id;
  e.points = points;
  e.title = title;
  e.detail = detail;
  S.platform.event(&e);
}

/* ---- address tables ---- */

static int cmp_u32(const void* a, const void* b)
{
  uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
  return (x > y) - (x < y);
}

static void build_runs(table_t* t)
{
  uint32_t i;
  t->run_count = 0;
  for (i = 0; i < t->count; i++) {
    if (t->run_count && t->runs[t->run_count - 1].address + t->runs[t->run_count - 1].length == t->addresses[i]) {
      t->runs[t->run_count - 1].length++;
    }
    else {
      run_t* r = &t->runs[t->run_count++];
      r->address = t->addresses[i];
      r->offset = i;
      r->length = 1;
    }
  }
}

static int table_in_use(const table_t* t)
{
  int i;
  uint32_t n;
  (void)i;
  if (t == S.current)
    return 1;
  for (n = S.slot_tail; n != S.slot_head; n++)
    if (S.slots[n % MAX_SLOTS].table == t)
      return 1;
  return 0;
}

/* Merge addresses missed during the last evaluation into a new table and publish it. */
static void publish_pending_addresses(void)
{
  table_t* old = S.current;
  table_t* t = NULL;
  uint32_t i, n, max = S.config.max_addresses;

  if (!S.pending_new_count)
    return;

  for (i = 0; i < TABLES; i++) {
    if (!table_in_use(&S.tables[i])) {
      t = &S.tables[i];
      break;
    }
  }
  if (!t)
    return; /* every table is still referenced; try again after the next frame */

  n = old->count;
  memcpy(t->addresses, old->addresses, n * sizeof(uint32_t));
  for (i = 0; i < S.pending_new_count && n < max; i++)
    t->addresses[n++] = S.pending_new[i];
  qsort(t->addresses, n, sizeof(uint32_t), cmp_u32);

  /* drop duplicates */
  t->count = 0;
  for (i = 0; i < n; i++)
    if (t->count == 0 || t->addresses[t->count - 1] != t->addresses[i])
      t->addresses[t->count++] = t->addresses[i];

  build_runs(t);
  t->generation = old->generation + 1;
  S.pending_new_count = 0;
  S.current = t; /* single word store: dsc_vblank sees the old or the new table, never a mix */
  S.stats.addresses = t->count;
}

static int table_find(const table_t* t, uint32_t address)
{
  uint32_t lo = 0, hi = t->count;
  while (lo < hi) {
    uint32_t mid = (lo + hi) >> 1;
    if (t->addresses[mid] < address)
      lo = mid + 1;
    else
      hi = mid;
  }
  return (lo < t->count && t->addresses[lo] == address) ? (int)lo : -1;
}

static void note_missing(uint32_t address)
{
  uint32_t i;
  for (i = 0; i < S.pending_new_count; i++)
    if (S.pending_new[i] == address)
      return;
  if (S.pending_new_count < S.config.max_addresses)
    S.pending_new[S.pending_new_count++] = address;
}

/* ---- rcheevos callbacks ---- */

static uint32_t RC_CCONV read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client)
{
  slot_t* slot = S.evaluating;
  uint32_t i;
  (void)client;

  if (!slot)
    return S.platform.read_memory(address, buffer, num_bytes);

  {
    /* fast path: whole read inside one run of the snapshot's table */
    const table_t* t = slot->table;
    int idx = table_find(t, address);
    if (idx >= 0 && (uint32_t)idx + num_bytes <= t->count && t->addresses[idx + num_bytes - 1] == address + num_bytes - 1) {
      memcpy(buffer, slot->data + idx, num_bytes);
      return num_bytes;
    }
    for (i = 0; i < num_bytes; i++) {
      idx = table_find(t, address + i);
      if (idx >= 0) {
        buffer[i] = slot->data[idx];
      }
      else {
        if (S.platform.read_memory(address + i, buffer + i, 1) != 1)
          return i;
        note_missing(address + i);
        S.stats.live_reads++;
      }
    }
  }
  return num_bytes;
}

static void RC_CCONV server_call(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data, rc_client_t* client)
{
  dsc_http_request_t req;
  int i;
  (void)client;

  for (i = 0; i < MAX_PENDING_HTTP; i++)
    if (!S.http[i].used)
      break;
  if (i == MAX_PENDING_HTTP) {
    rc_api_server_response_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
    callback(&resp, callback_data);
    return;
  }

  memset(&S.http[i], 0, sizeof(S.http[i]));
  S.http[i].used = 1;
  S.http[i].callback = callback;
  S.http[i].callback_data = callback_data;
  S.stats.pending_requests++;

  req.url = request->url;
  req.post_data = request->post_data;
  req.content_type = request->content_type;
  req.handle = &S.http[i];
  S.platform.http_send(&req);
}

static rc_clock_t RC_CCONV get_time_ms(const rc_client_t* client)
{
  (void)client;
  return (rc_clock_t)S.platform.millis();
}

static void RC_CCONV log_message(const char* message, const rc_client_t* client)
{
  (void)client;
  dsc_log(message);
}

static void RC_CCONV event_handler(const rc_client_event_t* event, rc_client_t* client)
{
  (void)client;
  switch (event->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
      emit(DSC_EVENT_UNLOCK, event->achievement->id, event->achievement->points, event->achievement->title, event->achievement->description);
      break;
    case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
      emit(DSC_EVENT_LEADERBOARD_SUBMIT, event->leaderboard->id, 0, event->leaderboard->title, event->leaderboard->tracker_value);
      break;
    case RC_CLIENT_EVENT_GAME_COMPLETED:
      emit(DSC_EVENT_MASTERY, 0, 0, NULL, NULL);
      break;
    case RC_CLIENT_EVENT_SERVER_ERROR:
      emit(DSC_EVENT_SERVER_ERROR, 0, 0, event->server_error->api, event->server_error->error_message);
      break;
    default:
      break;
  }
}

static void RC_CCONV game_loaded(int result, const char* error_message, rc_client_t* client, void* userdata)
{
  (void)client;
  (void)userdata;
  if (result == RC_OK) {
    const rc_client_game_t* game = rc_client_get_game_info(S.client);
    S.active = 1;
    emit(DSC_EVENT_GAME_LOADED, game ? game->id : 0, 0, game ? game->title : NULL, NULL);
  }
  else {
    emit(DSC_EVENT_GAME_LOAD_FAILED, (uint32_t)-result, 0, NULL, error_message);
  }
}

static void RC_CCONV logged_in(int result, const char* error_message, rc_client_t* client, void* userdata)
{
  (void)userdata;
  if (result != RC_OK) {
    emit(DSC_EVENT_LOGIN_FAILED, (uint32_t)-result, 0, NULL, error_message);
    return;
  }
  emit(DSC_EVENT_LOGIN_OK, 0, 0, S.config.username, NULL);
  rc_client_begin_load_game(client, S.game_hash, game_loaded, NULL);
}

/* ---- public API ---- */

int dsc_init(const dsc_platform_t* platform, const dsc_config_t* config)
{
  int i;
  uint32_t max;

  if (S.initialized)
    dsc_shutdown();
  if (!platform || !platform->read_memory || !platform->millis || !platform->http_send || !config)
    return -1;

  memset(&S, 0, sizeof(S));
  S.platform = *platform;
  S.config = *config;
  if (!S.config.max_addresses)
    S.config.max_addresses = 4096;
  max = S.config.max_addresses;
  S.slot_count = S.config.snapshot_slots ? S.config.snapshot_slots : 2;
  if (S.slot_count > MAX_SLOTS)
    S.slot_count = MAX_SLOTS;
  S.arena_size = S.config.snapshot_bytes ? S.config.snapshot_bytes : S.slot_count * max;

  for (i = 0; i < TABLES; i++) {
    S.tables[i].addresses = (uint32_t*)malloc(max * sizeof(uint32_t));
    S.tables[i].runs = (run_t*)malloc(max * sizeof(run_t));
    if (!S.tables[i].addresses || !S.tables[i].runs)
      return -2;
  }
  S.arena = (uint8_t*)malloc(S.arena_size);
  if (!S.arena)
    return -2;
  S.pending_new = (uint32_t*)malloc(max * sizeof(uint32_t));
  if (!S.pending_new)
    return -2;
  S.current = &S.tables[0];

  S.client = rc_client_create(read_memory, server_call);
  if (!S.client)
    return -2;
  rc_client_set_event_handler(S.client, event_handler);
  rc_client_set_get_time_millisecs_function(S.client, get_time_ms);
  if (S.platform.log)
    rc_client_enable_logging(S.client, RC_CLIENT_LOG_LEVEL_WARN, log_message);
  if (S.config.host)
    rc_client_set_host(S.client, S.config.host);
  rc_client_set_hardcore_enabled(S.client, S.config.hardcore ? 1 : 0);

  S.initialized = 1;
  return 0;
}

int dsc_begin_session(const char* game_hash)
{
  if (!S.initialized || !game_hash || strlen(game_hash) != 32)
    return -1;
  memcpy(S.game_hash, game_hash, 33);
  rc_client_begin_login_with_token(S.client, S.config.username, S.config.token, logged_in, NULL);
  return 0;
}

int dsc_is_active(void)
{
  return S.active;
}

/* Space for n bytes after the newest snapshot, or at the start of the
 * arena, without reaching the oldest queued one. -1 if there is none. */
static int32_t arena_alloc(uint32_t n)
{
  uint32_t head = S.arena_head, tail;
  if (S.slot_head == S.slot_tail)
    return n <= S.arena_size ? 0 : -1;    /* nothing queued: start over */
  tail = S.slots[S.slot_tail % MAX_SLOTS].offset;
  if (head >= tail) {
    if (S.arena_size - head >= n) return (int32_t)head;
    if (tail > n) return 0;
    return -1;
  }
  return tail - head > n ? (int32_t)head : -1;
}

void dsc_vblank(void)
{
  const table_t* t;
  slot_t* slot;
  int32_t offset;
  uint32_t i;

  if (!S.active)
    return;

  S.frame_counter++;
  t = S.current;
  offset = S.slot_head - S.slot_tail < S.slot_count ? arena_alloc(t->count) : -1;
  if (offset < 0) {
    S.stats.frames_dropped++;
    return;
  }

  slot = &S.slots[S.slot_head % MAX_SLOTS];
  slot->state = SLOT_FILLING;
  slot->offset = (uint32_t)offset;
  slot->size = t->count;
  slot->data = S.arena + offset;
  for (i = 0; i < t->run_count; i++) {
    const run_t* r = &t->runs[i];
    if (S.platform.read_memory(r->address, slot->data + r->offset, r->length) != r->length)
      memset(slot->data + r->offset, 0, r->length);
  }
  slot->table = t;
  slot->frame = S.frame_counter;
  S.stats.frames_snapshotted++;

  /* Same bytes as the newest queued frame, which idle hasn't started on:
   * count it there instead (cutscenes, loading, menus). Each repeat is still
   * evaluated as its own frame, so nothing is lost. */
  if (S.slot_head != S.slot_tail) {
    slot_t* prev = &S.slots[(S.slot_head - 1) % MAX_SLOTS];
    if (prev->state == SLOT_READY && prev->table == t && memcmp(prev->data, slot->data, t->count) == 0) {
      prev->repeats++;
      slot->state = SLOT_FREE;
      S.stats.frames_repeated++;
      return;
    }
  }

  slot->repeats = 0;
  S.arena_head = (uint32_t)offset + t->count;
  slot->state = SLOT_READY;
  S.slot_head++;    /* publish last */
}

static void finish_http(void)
{
  int i, progress = 1;
  /* a reply can start the next request (login -> set -> session), so loop until settled */
  while (progress) {
   progress = 0;
   for (i = 0; i < MAX_PENDING_HTTP; i++) {
    http_pending_t* p = &S.http[i];
    if (p->used && p->done) {
      rc_api_server_response_t resp;
      rc_client_server_callback_t cb = p->callback;
      void* data = p->callback_data;
      char* body = p->body;

      resp.body = body;
      resp.body_length = p->body_length;
      resp.http_status_code = p->status;
      p->used = 0;
      S.stats.pending_requests--;
      cb(&resp, data);
      free(body);
      progress = 1;
    }
   }
  }
}

int dsc_idle(void)
{
  int evaluated = 0;
  static uint32_t dropped_reported;

  if (!S.initialized)
    return 0;

  finish_http();

  while (S.slot_tail != S.slot_head) {
    slot_t* next = &S.slots[S.slot_tail % MAX_SLOTS];
    if (next->state != SLOT_READY)
      break;

    next->state = SLOT_EVAL;          /* from here on, dsc_vblank leaves repeats alone */
    {
      uint32_t n = 1 + next->repeats, k;
      S.evaluating = next;
      for (k = 0; k < n; k++) {
        rc_client_do_frame(S.client);
        S.stats.frames_evaluated++;
        evaluated++;
      }
      S.evaluating = NULL;
    }
    next->state = SLOT_FREE;
    S.slot_tail++;

    publish_pending_addresses();
    finish_http();
  }

  if (!evaluated && S.active)
    rc_client_idle(S.client);

  if (S.stats.frames_dropped != dropped_reported) {
    dropped_reported = S.stats.frames_dropped;
    emit(DSC_EVENT_DEGRADED, dropped_reported, 0, NULL, "evaluation fell behind; frames were dropped");
  }
  return evaluated;
}

void dsc_http_done(void* handle, int http_status, const char* body, size_t body_length)
{
  http_pending_t* p = (http_pending_t*)handle;
  if (!p || !p->used || p->done)
    return;
  p->body = (char*)malloc(body_length + 1);
  if (p->body) {
    memcpy(p->body, body, body_length);
    p->body[body_length] = '\0';
    p->body_length = body_length;
    p->status = http_status;
  }
  else {
    p->body_length = 0;
    p->status = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
  }
  p->done = 1;
}

void dsc_get_stats(dsc_stats_t* stats)
{
  *stats = S.stats;
}

void dsc_reset(void)
{
  uint32_t head = S.slot_head, n;
  if (!S.initialized)
    return;
  S.evaluating = NULL;
  for (n = S.slot_tail; n != head; n++)
    S.stats.frames_discarded += 1 + S.slots[n % MAX_SLOTS].repeats;
  S.slot_tail = head;          /* frames from before the reset */
  if (S.active)
    rc_client_reset(S.client);
}

void dsc_get_progress(dsc_progress_t* progress)
{
  rc_client_user_game_summary_t summary;
  memset(progress, 0, sizeof(*progress));
  if (!S.initialized || !S.active)
    return;
  rc_client_get_user_game_summary(S.client, &summary);
  progress->achievements = summary.num_promoted_achievements;
  progress->unlocked = summary.num_unlocked_achievements;
  progress->points = summary.points_available;
  progress->points_unlocked = summary.points_unlocked;
}

void dsc_shutdown(void)
{
  int i;
  if (!S.initialized)
    return;
  S.active = 0;
  if (S.client) {
    rc_client_unload_game(S.client);
    rc_client_destroy(S.client);
  }
  for (i = 0; i < TABLES; i++) {
    free(S.tables[i].addresses);
    free(S.tables[i].runs);
  }
  free(S.arena);
  for (i = 0; i < MAX_PENDING_HTTP; i++)
    if (S.http[i].used)
      free(S.http[i].body);
  free(S.pending_new);
  memset(&S, 0, sizeof(S));
}
