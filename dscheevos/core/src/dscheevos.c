/*
 * DScheevos core. See dscheevos.h for the model.
 *
 * Snapshot: rcheevos reads game memory through read_memory(). While a
 * snapshot is being evaluated, reads are served from it. When the game loads,
 * the address table gets every address the set reads directly, and the
 * pointer chains (AddAddress) are copied: each VBlank runs the copies with
 * rcheevos' own code on live memory and keeps the bytes each pointer lands
 * on in the snapshot. So a queued frame is evaluated with memory from its
 * own VBlank, even when a pointer moved. Anything still missing (a chain
 * that uses Remember/Recall) is read live and added to the table; that read
 * may be from a later frame, and live_reads counts it.
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
#include "client/rc_client_internal.h"
#include "runtime/rc_internal.h"

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

/* Bytes a pointer chain landed on at VBlank, stored after the table's bytes. */
typedef struct target_t {
  uint32_t address;
  uint32_t length;
  uint8_t bytes[4];
} target_t;

/* Private copies of the set's pointer chains, run at VBlank. */
typedef struct chains_t {
  rc_memref_t* direct;         /* pointers the chains start from */
  uint32_t direct_count;
  rc_modified_memref_t* mods;  /* chain steps, parents first */
  uint8_t* kind;               /* per step: how run_chains computes it (STEP_*) */
  uint32_t mod_count;
  uint32_t target_count;       /* steps that read memory (one target_t each) */
} chains_t;

typedef struct slot_t {
  volatile int state;
  uint8_t* data;
  uint32_t offset;            /* in the arena */
  uint32_t size;
  const table_t* table;
  target_t* targets;          /* chains.target_count of them */
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
  volatile int active;         /* set last: dsc_vblank uses the table and chains only after it */
  chains_t chains;

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

/* ---- pointer chains ---- */

/* Most DS chains are "pointer & 0x1FFFFFF, then + offset and read", on plain
 * unsigned values. Those steps skip rcheevos' general operand code, which
 * costs several times more and runs in the VBlank IRQ; the result is the same. */
enum { STEP_GENERIC, STEP_AND, STEP_READ };

/* Reads a chain step's target live at VBlank and keeps the bytes in the snapshot. */
typedef struct capture_t {
  target_t* targets;
  uint32_t next;
} capture_t;

static uint32_t RC_CCONV capture_read(uint32_t address, uint8_t* buffer, uint32_t num_bytes, void* ud)
{
  capture_t* c = (capture_t*)ud;
  target_t* g = &c->targets[c->next++];
  uint32_t n = num_bytes > sizeof(g->bytes) ? 0 : S.platform.read_memory(address, buffer, num_bytes);
  g->address = address;
  g->length = n;
  memcpy(g->bytes, buffer, n);
  return n;
}

static uint32_t RC_CCONV live_read(uint32_t address, uint8_t* buffer, uint32_t num_bytes, void* ud)
{
  (void)ud;
  return S.platform.read_memory(address, buffer, num_bytes);
}

/* At VBlank, after the table's bytes are copied: advance the chain copies one
 * frame on live memory, as rc_update_memref_values does for the real ones. */
static void run_chains(target_t* targets)
{
  capture_t c;
  uint32_t i;
  c.targets = targets;
  c.next = 0;
  for (i = 0; i < S.chains.direct_count; i++) {
    rc_memref_t* m = &S.chains.direct[i];
    uint8_t b[4];
    if (m->value.size == RC_MEMSIZE_32_BITS && S.platform.read_memory(m->address, b, 4) == 4)
      rc_update_memref_value(&m->value, b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24));
    else
      rc_update_memref_value(&m->value, rc_read_memory(m->address, m->value.size, live_read, NULL));
  }
  for (i = 0; i < S.chains.mod_count; i++) {
    rc_modified_memref_t* m = &S.chains.mods[i];
    uint32_t before = c.next, value;
    switch (S.chains.kind[i]) {
      case STEP_AND:
        value = m->parent.value.memref->value.value & m->modifier.value.num;
        break;
      case STEP_READ: {
        target_t* g = &targets[c.next++];
        uint32_t n = m->memref.value.size == RC_MEMSIZE_8_BITS ? 1 : m->memref.value.size == RC_MEMSIZE_16_BITS ? 2 : 4;
        g->address = m->parent.value.memref->value.value + m->modifier.value.num;
        g->length = S.platform.read_memory(g->address, g->bytes, n);
        value = g->bytes[0] | (g->bytes[1] << 8) | (g->bytes[2] << 16) | ((uint32_t)g->bytes[3] << 24);  /* unread bytes are 0, as in rc_read_memory */
        break;
      }
      default:
        value = rc_get_modified_memref_value(m, capture_read, &c);
        break;
    }
#ifdef DSC_CHECK_FAST_STEPS  /* tests: the shortcut must match rcheevos' own code */
    if (S.chains.kind[i] != STEP_GENERIC && value != rc_get_modified_memref_value(m, live_read, NULL))
      abort();
#endif
    rc_update_memref_value(&m->memref.value, value);
    if (m->modifier_type == RC_OPERATOR_INDIRECT_READ && c.next == before) {
      targets[c.next].address = 0;    /* no read happened: keep the slot count fixed */
      targets[c.next++].length = 0;
    }
  }
}

static void free_chains(void)
{
  free(S.chains.direct);
  free(S.chains.mods);
  free(S.chains.kind);
  memset(&S.chains, 0, sizeof(S.chains));
}

/* STEP_AND or STEP_READ when the step is the plain unsigned shape run_chains
 * computes itself: parent read as is (no size change, BCD, delta...), and a
 * constant modifier. */
static uint8_t step_kind(const rc_modified_memref_t* m)
{
  const rc_operand_t* p = &m->parent;
  if (p->type != RC_OPERAND_ADDRESS || !p->value.memref || p->size != p->value.memref->value.size ||
      p->value.memref->value.type != RC_VALUE_TYPE_UNSIGNED || m->modifier.type != RC_OPERAND_CONST ||
      m->memref.value.type != RC_VALUE_TYPE_UNSIGNED)
    return STEP_GENERIC;
  if (m->modifier_type == RC_OPERATOR_AND)
    return STEP_AND;
  if (m->modifier_type == RC_OPERATOR_INDIRECT_READ &&
      (m->memref.value.size == RC_MEMSIZE_8_BITS || m->memref.value.size == RC_MEMSIZE_16_BITS || m->memref.value.size == RC_MEMSIZE_32_BITS))
    return STEP_READ;
  return STEP_GENERIC;
}

/* The copy of a memref an operand points at, or NULL if it isn't copied. */
static rc_memref_t* chain_copy_of(const rc_memref_t* original, rc_memref_t* const* direct_src, rc_modified_memref_t* const* mod_src)
{
  uint32_t i;
  for (i = 0; i < S.chains.direct_count; i++)
    if (direct_src[i] == original)
      return &S.chains.direct[i];
  for (i = 0; i < S.chains.mod_count; i++)
    if (mod_src[i] == (const rc_modified_memref_t*)original)
      return &S.chains.mods[i].memref;
  return NULL;
}

/* Fill the address table with every address the set reads directly, and copy
 * the steps of every pointer chain. Runs once the game is loaded, before any
 * snapshot is taken. */
static void prepare_snapshots(void)
{
  rc_memrefs_t* memrefs = S.client->game->runtime.memrefs;
  rc_memref_list_t* ml;
  rc_modified_memref_list_t* mml;
  rc_modified_memref_t** mods = NULL;
  rc_memref_t** direct_src = NULL;
  uint8_t* need = NULL;
  uint32_t mod_total = 0, i, j, n;

  free_chains();
  if (!memrefs)
    return;

  /* every byte read directly, merged into the table (rcheevos has already
   * disabled achievements whose addresses can't be read) */
  for (ml = &memrefs->memrefs; ml; ml = ml->next) {
    for (i = 0; i < ml->count; i++) {
      const rc_memref_t* m = &ml->items[i];
      uint32_t bytes = m->value.type == RC_VALUE_TYPE_NONE ? 0 : rc_memref_bytes(m->value.size);
      for (j = 0; j < bytes && S.pending_new_count < S.config.max_addresses; j++)
        S.pending_new[S.pending_new_count++] = m->address + j;
    }
  }
  publish_pending_addresses();

  /* chain steps in order, parents first */
  for (mml = &memrefs->modified_memrefs; mml; mml = mml->next)
    mod_total += mml->count;
  if (!mod_total)
    return;
  mods = (rc_modified_memref_t**)malloc(mod_total * sizeof(*mods));
  need = (uint8_t*)calloc(mod_total, 1);
  direct_src = (rc_memref_t**)malloc(mod_total * 2 * sizeof(*direct_src));
  S.chains.mods = (rc_modified_memref_t*)malloc(mod_total * sizeof(rc_modified_memref_t));
  S.chains.kind = (uint8_t*)malloc(mod_total);
  S.chains.direct = (rc_memref_t*)malloc(mod_total * 2 * sizeof(rc_memref_t));
  if (!mods || !need || !direct_src || !S.chains.mods || !S.chains.kind || !S.chains.direct)
    goto fail;
  n = 0;
  for (mml = &memrefs->modified_memrefs; mml; mml = mml->next)
    for (i = 0; i < mml->count; i++)
      mods[n++] = &mml->items[i];

  /* a step is needed if it reads memory, or a needed step uses it; walk back from the reads */
  for (i = mod_total; i-- > 0;) {
    const rc_operand_t* ops[2];
    if (mods[i]->modifier_type == RC_OPERATOR_INDIRECT_READ)
      need[i] = 1;
    if (!need[i])
      continue;
    ops[0] = &mods[i]->parent;
    ops[1] = &mods[i]->modifier;
    for (j = 0; j < 2; j++) {
      const rc_operand_t* op = ops[j];
      uint32_t k;
      if (!rc_operand_type_is_memref(op->type) || !op->value.memref)
        continue;
      if (op->value.memref->value.memref_type == RC_MEMREF_TYPE_MODIFIED_MEMREF) {
        for (k = 0; k < i; k++)
          if (mods[k] == (rc_modified_memref_t*)op->value.memref)
            need[k] = 1;
      }
      else if (op->value.memref->value.memref_type == RC_MEMREF_TYPE_MEMREF) {
        for (k = 0; k < S.chains.direct_count; k++)
          if (direct_src[k] == op->value.memref)
            break;
        if (k == S.chains.direct_count) {
          direct_src[k] = op->value.memref;
          S.chains.direct[k] = *op->value.memref;
          S.chains.direct_count++;
        }
      }
    }
  }

  /* copy the needed steps, pointing their operands at the copies; a step that
   * depends on something not copied (Remember/Recall) is left to live reads */
  for (i = 0; i < mod_total; i++) {
    rc_modified_memref_t* copy;
    rc_operand_t* ops[2];
    int ok = 1;
    if (!need[i])
      continue;
    copy = &S.chains.mods[S.chains.mod_count];
    *copy = *mods[i];
    ops[0] = &copy->parent;
    ops[1] = &copy->modifier;
    for (j = 0; j < 2; j++) {
      if (ops[j]->type == RC_OPERAND_RECALL) {
        ok = 0;
      }
      else if (rc_operand_type_is_memref(ops[j]->type) && ops[j]->value.memref) {
        rc_memref_t* c = chain_copy_of(ops[j]->value.memref, direct_src, mods);
        if (c)
          ops[j]->value.memref = c;
        else
          ok = 0;
      }
    }
    if (!ok)
      continue;
    mods[S.chains.mod_count] = mods[i];  /* reused as "source of copy k" from here on */
    S.chains.kind[S.chains.mod_count] = step_kind(copy);
    S.chains.mod_count++;
    if (copy->modifier_type == RC_OPERATOR_INDIRECT_READ)
      S.chains.target_count++;
  }
  free(mods);
  free(need);
  free(direct_src);
  return;

fail:
  free(mods);
  free(need);
  free(direct_src);
  free_chains();
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
      uint32_t a = address + i, k;
      idx = table_find(t, a);
      if (idx >= 0) {
        buffer[i] = slot->data[idx];
        continue;
      }
      for (k = 0; k < S.chains.target_count; k++) {
        const target_t* g = &slot->targets[k];
        if (a - g->address < g->length)
          break;
      }
      if (k < S.chains.target_count) {
        buffer[i] = slot->targets[k].bytes[a - slot->targets[k].address];
      }
      else {
        if (S.platform.read_memory(a, buffer + i, 1) != 1)
          return i;
        note_missing(a);
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
    prepare_snapshots();
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
  uint32_t i, size;

  if (!S.active)
    return;

  S.frame_counter++;
  t = S.current;
  size = ((t->count + 3) & ~3u) + S.chains.target_count * sizeof(target_t);
  offset = S.slot_head - S.slot_tail < S.slot_count ? arena_alloc(size) : -1;
  if (offset < 0) {
    S.stats.frames_dropped++;
    return;
  }

  slot = &S.slots[S.slot_head % MAX_SLOTS];
  slot->state = SLOT_FILLING;
  slot->offset = (uint32_t)offset;
  slot->size = size;
  slot->data = S.arena + offset;
  slot->targets = (target_t*)(slot->data + ((t->count + 3) & ~3u));
  for (i = 0; i < t->run_count; i++) {
    const run_t* r = &t->runs[i];
    if (S.platform.read_memory(r->address, slot->data + r->offset, r->length) != r->length)
      memset(slot->data + r->offset, 0, r->length);
  }
  memset(slot->data + t->count, 0, size - t->count);  /* padding and targets: compared for repeats */
  run_chains(slot->targets);
  slot->table = t;
  slot->frame = S.frame_counter;
  S.stats.frames_snapshotted++;

  /* Same bytes as the newest queued frame, which idle hasn't started on:
   * count it there instead (cutscenes, loading, menus). Each repeat is still
   * evaluated as its own frame, so nothing is lost. */
  if (S.slot_head != S.slot_tail) {
    slot_t* prev = &S.slots[(S.slot_head - 1) % MAX_SLOTS];
    if (prev->state == SLOT_READY && prev->table == t && memcmp(prev->data, slot->data, size) == 0) {
      prev->repeats++;
      slot->state = SLOT_FREE;
      S.stats.frames_repeated++;
      return;
    }
  }

  slot->repeats = 0;
  S.arena_head = (uint32_t)offset + size;
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
  free_chains();
  memset(&S, 0, sizeof(S));
}
