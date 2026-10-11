/*
 * DScheevos ARM9 module: glue between the nds-bootstrap hooks and the core.
 * See dscheevos_module.h for the ABI and memory layout.
 */
#include "dscheevos_module.h"
#include "dscheevos.h"

#include <string.h>

#define REG_VCOUNT (*(volatile uint16_t*)0x04000006)
#define LINES_PER_FRAME 263

dscm_status_t dscm_g_status;   /* referenced by the header in start.s */
dscm_net_slot_t dscm_g_net[DSCM_NET_SLOTS];
static void* g_net_handle[DSCM_NET_SLOTS];
static uint32_t g_net_sequence;
static dscm_boot_t g_boot;
static const char* g_session;        /* session file in memory (preloads) */
static volatile uint32_t g_frames;
static uint32_t g_last_vblank;
static uint32_t g_dtcm;
static int g_initialized;

extern uint32_t dscm_vblank_count_read(void);  /* start.s */
extern uint32_t dscm_dtcm_base(void);

/* used by the stubs in syscalls.c */
uint32_t dscm_unix_time(void) { return g_boot.unix_time + g_frames / 60; }

/* ---- platform for the core ---- */

static uint32_t read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes)
{
  if (address < 0x400000u && address + num_bytes <= 0x400000u) {
    memcpy(buffer, (const void*)(0x02000000u + address), num_bytes);
    return num_bytes;
  }
  if (address >= 0x1000000u && address + num_bytes <= 0x1004000u && g_dtcm) {
    memcpy(buffer, (const void*)(g_dtcm + (address - 0x1000000u)), num_bytes);
    return num_bytes;
  }
  return 0;
}

static uint32_t millis(void)
{
  return (uint32_t)(((uint64_t)g_frames * 1000u) / 60u);
}

static const char* request_kind(const char* post)
{
  /* "r=<kind>&..." */
  return (post && post[0] == 'r' && post[1] == '=') ? post + 2 : "";
}

static int starts_with(const char* s, const char* prefix)
{
  return strncmp(s, prefix, strlen(prefix)) == 0 && (s[strlen(prefix)] == '&' || s[strlen(prefix)] == '\0');
}

static void http_send(const dsc_http_request_t* request)
{
  const char* kind = request_kind(request->post_data);
  int preload = -1;
  int i;

  /* Session-start requests were answered by the loader before the game booted. */
  if (starts_with(kind, "login2")) preload = DSCM_PRELOAD_LOGIN;
  else if (starts_with(kind, "achievementsets")) preload = DSCM_PRELOAD_SET;
  else if (starts_with(kind, "startsession")) preload = DSCM_PRELOAD_SESSION;

  if (preload >= 0) {
    if (g_boot.preload_length[preload]) {
      const char* body = g_session + g_boot.preload_offset[preload];
      dsc_http_done(request->handle, 200, body, g_boot.preload_length[preload]);
    }
    else {
      static const char missing[] = "{\"Success\":false,\"Error\":\"Not fetched at session start\"}";
      dsc_http_done(request->handle, 200, missing, sizeof(missing) - 1);
    }
    return;
  }

  /* Everything else (unlocks, leaderboards, pings) goes to the network layer. */
  for (i = 0; i < DSCM_NET_SLOTS; i++) {
    if (dscm_g_net[i].state == DSCM_NET_FREE) {
      dscm_net_slot_t* s = &dscm_g_net[i];
      if (strlen(request->url) >= DSCM_NET_URL_MAX || (request->post_data && strlen(request->post_data) >= DSCM_NET_POST_MAX))
        break;
      strcpy(s->url, request->url);
      strcpy(s->post, request->post_data ? request->post_data : "");
      s->sequence = ++g_net_sequence;
      s->http_status = 0;
      s->reply_length = 0;
      g_net_handle[i] = request->handle;
      dscm_g_status.pending_requests++;
      s->state = DSCM_NET_REQUEST;   /* publish last */
      return;
    }
  }
  /* queue full: rcheevos retries retryable errors later */
  dsc_http_done(request->handle, -2, "", 0);
}

static void poll_network(void)
{
  int i;
  for (i = 0; i < DSCM_NET_SLOTS; i++) {
    dscm_net_slot_t* s = &dscm_g_net[i];
    if (s->state == DSCM_NET_REPLIED) {
      uint32_t len = s->reply_length < DSCM_NET_REPLY_MAX ? s->reply_length : DSCM_NET_REPLY_MAX;
      dsc_http_done(g_net_handle[i], s->http_status, s->reply, len);
      g_net_handle[i] = NULL;
      dscm_g_status.pending_requests--;
      s->state = DSCM_NET_FREE;
    }
  }
}

static void copy_title(char* dst, const char* src, size_t n)
{
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, n - 1);
  dst[n - 1] = '\0';
}

static void on_event(const dsc_event_t* e)
{
  switch (e->type) {
    case DSC_EVENT_UNLOCK: {
      dscm_unlock_t* u = &dscm_g_status.recent[dscm_g_status.unlock_count % DSCM_RECENT_MAX];
      u->id = e->id;
      u->points = e->points;
      u->frame = g_frames;
      copy_title(u->title, e->title, sizeof(u->title));
      dscm_g_status.unlock_count++;
      break;
    }
    case DSC_EVENT_GAME_LOADED:
      copy_title(dscm_g_status.game_title, e->title, sizeof(dscm_g_status.game_title));
      break;
    case DSC_EVENT_LOGIN_FAILED:
      dscm_g_status.error = DSCM_ERROR_LOGIN;
      break;
    case DSC_EVENT_GAME_LOAD_FAILED:
      dscm_g_status.error = DSCM_ERROR_GAME_LOAD;
      break;
    default:
      break;
  }
}

/* ---- entry points ---- */

extern char __bss_start[], __bss_end[];

int module_init(const dscm_boot_t* boot)
{
  dsc_platform_t platform;
  dsc_config_t config;
  int i;

  memset(__bss_start, 0, (size_t)(__bss_end - __bss_start));
  if (!boot || boot->magic != DSCM_SESSION_MAGIC || boot->size != sizeof(dscm_boot_t)
      || boot->file_size > DSCM_SESSION_MAX) {
    dscm_g_status.error = DSCM_ERROR_BAD_BOOT;
    return -1;
  }
  for (i = 0; i < DSCM_PRELOAD_COUNT; i++) {
    if (boot->preload_offset[i] > boot->file_size || boot->preload_length[i] > boot->file_size - boot->preload_offset[i]) {
      dscm_g_status.error = DSCM_ERROR_BAD_BOOT;
      return -1;
    }
  }
  g_boot = *boot;
  g_session = (const char*)boot;
  g_boot.host[sizeof(g_boot.host) - 1] = '\0';
  g_boot.username[sizeof(g_boot.username) - 1] = '\0';
  g_boot.token[sizeof(g_boot.token) - 1] = '\0';
  g_boot.game_hash[sizeof(g_boot.game_hash) - 1] = '\0';
  g_dtcm = g_boot.dtcm_base;
  g_last_vblank = dscm_vblank_count_read();
  dscm_g_status.hardcore = g_boot.hardcore;

  memset(&platform, 0, sizeof(platform));
  platform.read_memory = read_memory;
  platform.millis = millis;
  platform.http_send = http_send;
  platform.event = on_event;

  memset(&config, 0, sizeof(config));
  config.host = g_boot.host;
  config.username = g_boot.username;
  config.token = g_boot.token;
  config.hardcore = g_boot.hardcore;
  config.max_addresses = 4096;
  config.snapshot_slots = 60;   /* ride out a second of frames with no idle time (loading) */

  if (dsc_init(&platform, &config) != 0) {
    dscm_g_status.error = DSCM_ERROR_NO_MEMORY;
    return -1;
  }
  if (dsc_begin_session(g_boot.game_hash) != 0) {
    dscm_g_status.error = DSCM_ERROR_BAD_BOOT;
    return -1;
  }
  /* session-start replies are preloaded, so this settles in a few passes */
  for (i = 0; i < 8 && !dsc_is_active() && !dscm_g_status.error; i++)
    dsc_idle();

  dscm_g_status.active = dsc_is_active();
  g_initialized = 1;
  return dscm_g_status.active ? 0 : -1;
}

/* Called on every ARM9 IPC sync IRQ; acts once per ARM7 VBlank. */
void module_vblank(void)
{
  dsc_stats_t s;
  uint32_t count = dscm_vblank_count_read();
  uint32_t passed = count - g_last_vblank;
  if (passed == 0)
    return;   /* another IPC message, not a VBlank */
  g_last_vblank = count;
  g_frames += passed;
  if (!g_initialized || !dscm_g_status.active)
    return;
  if (passed > 1)
    dscm_g_status.frames_missed += passed - 1;
  if (!g_boot.dtcm_base)
    g_dtcm = dscm_dtcm_base();
  dsc_vblank();
  dsc_get_stats(&s);
  dscm_g_status.frames_snapshotted = s.frames_snapshotted;
  dscm_g_status.frames_dropped = s.frames_dropped;
}

/* Called on the module's own stack from halt_entry, in the game's idle thread.
 * NitroSDK switches threads when an IRQ returns, so any game thread that wakes
 * (the main loop at VBlank, a loader thread at a card IRQ) preempts this work
 * at once; it resumes the next time the game is idle. eval_lines is wall time
 * between start and end, so it includes any time the game took meanwhile. */
void module_idle(void)
{
  dsc_stats_t s;
  uint32_t start, end, used;

  if (!g_initialized || !dscm_g_status.active)
    return;

  poll_network();
  dsc_get_stats(&s);
  if (s.frames_snapshotted == s.frames_evaluated + s.frames_dropped)
    return; /* nothing waiting */

  start = REG_VCOUNT;
  dsc_idle();
  end = REG_VCOUNT;
  used = (end + LINES_PER_FRAME - start) % LINES_PER_FRAME;
  dscm_g_status.eval_lines = (dscm_g_status.eval_lines * 7 + used * 16) / 8;

  dsc_get_stats(&s);
  dscm_g_status.frames_evaluated = s.frames_evaluated;
}

void module_shutdown(void)
{
  if (g_initialized)
    dsc_shutdown();
  g_initialized = 0;
  dscm_g_status.active = 0;
}

dscm_status_t* module_status(void) { return &dscm_g_status; }
dscm_net_slot_t* module_net(void) { return dscm_g_net; }
