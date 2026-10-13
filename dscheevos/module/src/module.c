/*
 * DScheevos ARM9 module: glue between the nds-bootstrap hooks and the core.
 * See dscheevos_module.h for the ABI and memory layout.
 */
#include "dscheevos_module.h"
#include "dscheevos.h"
#include "net/net.h"
#include "heap.h"
#include "util/rc_version.h"

#include <stddef.h>
#include <string.h>

#define REG_VCOUNT (*(volatile uint16_t*)0x04000006)
#define LINES_PER_FRAME 263

dscm_status_t dscm_g_status;   /* referenced by the header in start.s */
dscm_net_slot_t dscm_g_net[DSCM_NET_SLOTS];
dscm_notices_t dscm_g_notices;
/* Written only through the uncached mirror, so the ARM7 can copy it from
 * RAM. It has cache lines of its own: a cached write to a neighbour can
 * never write stale journal bytes back. */
union {
  dscm_journal_t j;
  uint8_t lines[(sizeof(dscm_journal_t) + 31) & ~31u];
} dscm_g_journal __attribute__((aligned(32)));
static void* g_net_handle[DSCM_NET_SLOTS];
static uint32_t g_net_sequence;
static dscm_boot_t g_boot;
static const char* g_session;        /* session file in memory (preloads) */
static uint32_t g_dtcm;
static int g_initialized;

extern uint32_t dscm_vblank_count_read(void);  /* start.s */
extern uint32_t dscm_dtcm_base(void);
extern void dscm_publish_notify(uint32_t count);
extern void dscm_publish_cues(uint32_t cues);
extern void dscm_publish_list(void* list);
extern void dscm_dc_clean(const void* addr, uint32_t len);
extern void dscm_dc_invalidate(const void* addr, uint32_t len);
extern uint32_t dscm_take(volatile uint32_t* p);
static uint32_t g_notices_seen;
static uint32_t g_cues_head;         /* last notice counted into the cues */
static uint32_t g_cues;
static volatile int g_reset_pending;
static int g_progress_stale;
static void update_progress(void);

/* Self-timing. REG_VCOUNT is read-only and runs whatever the game does; the
 * timers are the game's, so we never touch them. Whole lines; the caller
 * keeps the work under a frame (a longer one wraps and reads short). */
static inline uint32_t lines_between(uint32_t start, uint32_t end)
{
  int32_t d = (int32_t)end - (int32_t)start;
  return (uint32_t)(d < 0 ? d + LINES_PER_FRAME : d);
}
#define lines_since(start) lines_between((start), REG_VCOUNT)

/* What the VBlank IRQ (vblank_entry in start.s) reads and writes: two
 * cache lines in the module header, next to its code; each VBlank touches
 * only the first. publish_stats() copies the counters to the status in idle
 * time. idle_open says an idle pass is being timed: whoever clears it (the
 * pass at its end, or the VBlank IRQ that preempts it) counts its lines, so
 * none are counted twice. */
typedef struct dscm_irq_t {
  uint32_t last_vblank;          /* the ARM7's count at the last call */
  volatile uint32_t frames;      /* VBlanks since init */
  uint8_t on;                    /* initialized and active */
  volatile uint8_t in_vblank;    /* a copy (dsc_vblank_copy) is running */
  uint8_t reserved[2];
  volatile uint32_t idle_open;
  volatile uint32_t idle_start;
  uint32_t timed_frames;
  uint32_t irq_lines_total;
  uint16_t irq_lines_last;
  uint16_t irq_lines_max;
  /* second line: only when a pass is cut, or VBlanks were missed */
  volatile uint32_t idle_cut_lines;
  uint32_t idle_cut;
  uint32_t frames_missed;
  uint32_t reserved2[5];
} dscm_irq_t;
extern dscm_irq_t dscm_irq;
/* the offsets start.s uses */
_Static_assert(sizeof(dscm_irq_t) == 64, "dscm_irq: two cache lines");
_Static_assert(offsetof(dscm_irq_t, on) == 8 && offsetof(dscm_irq_t, in_vblank) == 9 && offsetof(dscm_irq_t, idle_open) == 12, "dscm_irq");
_Static_assert(offsetof(dscm_irq_t, idle_start) == 16 && offsetof(dscm_irq_t, timed_frames) == 20 && offsetof(dscm_irq_t, irq_lines_total) == 24, "dscm_irq");
_Static_assert(offsetof(dscm_irq_t, irq_lines_last) == 28 && offsetof(dscm_irq_t, irq_lines_max) == 30, "dscm_irq");
_Static_assert(offsetof(dscm_irq_t, idle_cut_lines) == 32 && offsetof(dscm_irq_t, idle_cut) == 36 && offsetof(dscm_irq_t, frames_missed) == 40, "dscm_irq");
#define g_frames (dscm_irq.frames)

/* Idle lines per frame, from the idle thread, with IRQs masked: at each
 * pass's start and end it closes the frames that ended since (VBlanks
 * counted by the IRQ). Lines a pass added belong to the frame it ran in,
 * and so does a cut (the IRQ counts it at that frame's end); frames since
 * then had no pass. "Ran": a pass started or ended in the frame. */
static struct {
  uint32_t frame;                /* the frame being added up (dscm_irq.frames then) */
  uint32_t lines;                /* its lines so far */
  uint32_t cut_seen;             /* dscm_irq.idle_cut_lines already counted */
  int ran;
  uint32_t last, max, total, none;
} g_idle;

static void idle_frames(uint32_t lines)
{
  uint32_t passed = dscm_irq.frames - g_idle.frame;
  if (passed) {
    uint32_t cut = dscm_irq.idle_cut_lines;
    uint32_t count = g_idle.lines + (cut - g_idle.cut_seen);
    g_idle.cut_seen = cut;
    g_idle.total += count;
    if (count > g_idle.max)
      g_idle.max = count;
    g_idle.last = passed > 1 ? 0 : count;
    g_idle.none += passed - 1 + !g_idle.ran;
    g_idle.frame = dscm_irq.frames;
    g_idle.lines = 0;
    g_idle.ran = 0;
  }
  g_idle.lines += lines;
  g_idle.ran = 1;
}

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

/* The same regions as read_memory, for the core's direct snapshot copy
 * (dsc_capture, or a copy in the VBlank IRQ), after g_dtcm is refreshed. */
static void memory_map(const uint8_t** main_ram, const uint8_t** dtcm)
{
  *main_ram = (const uint8_t*)0x02000000u;
  *dtcm = g_dtcm ? (const uint8_t*)g_dtcm : NULL;
}

extern uint32_t dscm_irq_off(void);
extern void dscm_irq_restore(uint32_t cpsr);
extern void dscm_wfi(void);

/* dsc_capture copies with IRQs masked and calls this every few words. IF
 * latches requests while CPSR.I masks them: a waiting IRQ runs here at
 * once, so the game sees at most a few words' delay, and the copy goes on.
 * If two lines or more went by, a game thread probably ran (the IRQ woke
 * it): start over. The core also starts over if the IRQ was a VBlank. */
#define REG_IE (*(volatile uint32_t*)0x04000210)
#define REG_IF (*(volatile uint32_t*)0x04000214)
static uint32_t g_capture_cpsr;   /* CPSR with IRQs on, from the caller of capture() */
static int interrupt_pending(void)
{
  uint32_t before;
  if (!(REG_IE & REG_IF))
    return 0;
  before = REG_VCOUNT;
  dscm_irq_restore(g_capture_cpsr);
  dscm_irq_off();
  return lines_between(before, REG_VCOUNT) >= 2;
}

/* A snapshot when the game halts. Returns with IRQs still masked, and the
 * CPSR to put back: the caller may halt first. */
/* The game sets its DTCM up before its first frame; the module starts before that. */
static void refresh_dtcm(void)
{
  if (!g_boot.dtcm_base)
    g_dtcm = dscm_dtcm_base();
}

static uint32_t capture(void)
{
  refresh_dtcm();
  g_capture_cpsr = dscm_irq_off();
  dsc_capture();
  return g_capture_cpsr;
}

/* g_frames * 1000 / 60, the same value mod 2^32 as the 64-bit product, in
 * 32-bit arithmetic: a 64-bit division costs ~340 instructions in libgcc,
 * and this runs several times every idle pass. */
static uint32_t millis(void)
{
  uint32_t f = g_frames, q = f / 3u;
  return q * 50u + (f - q * 3u) * 50u / 3u;
}
uint32_t dscm_millis(void) { return millis(); }

static uint32_t frame_count(void) { return g_frames; }

static int g_net_started;
static uint32_t g_pass_frame;     /* g_frames at the last idle pass's once-a-frame work */
extern void dscm_publish_net_ring(void* ring);
extern void https_add_ca(const char* pem, uint32_t len);

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
      uint32_t offset = g_boot.preload_offset[preload], length = g_boot.preload_length[preload];
      const char* body = g_session + offset;
      /* With a spare NUL after it in the file, the core's copy of the reply
       * is the reply itself (heap.h): no heap for it. */
      if (offset + length < g_boot.file_size && body[length] == '\0')
        heap_lend(body, length);
      dsc_http_done(request->handle, 200, body, length);
      heap_lend(NULL, 0);
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

/* a request http_send() queued that the network layer hasn't taken yet */
static int request_waiting(void)
{
  int i;
  for (i = 0; i < DSCM_NET_SLOTS; i++)
    if (dscm_g_net[i].state == DSCM_NET_REQUEST)
      return 1;
  return 0;
}

static void poll_network(void)
{
  int i;
  for (i = 0; i < DSCM_NET_SLOTS; i++) {
    dscm_net_slot_t* s = &dscm_g_net[i];
    if (s->state == DSCM_NET_REPLIED) {
      uint32_t len = s->reply_length < DSCM_NET_REPLY_MAX ? s->reply_length : DSCM_NET_REPLY_MAX;
      if (s->http_status == 200 && !strncmp(s->post, "r=awardachievement&", 19))
        dscm_g_status.unlocks_sent++;
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
      dscm_publish_notify(dscm_g_status.unlock_count);
      g_progress_stale = 1;
      break;
    }
    case DSC_EVENT_GAME_LOADED:
      copy_title(dscm_g_status.game_title, e->title, sizeof(dscm_g_status.game_title));
      g_progress_stale = 1;
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

/* The list is complete: write it to RAM, then swap header->list. */
static void publish_list(dscm_list_t* list)
{
  if (list)
    dscm_dc_clean(list, sizeof(dscm_list_t) + list->count * sizeof(dscm_list_entry_t));
  dscm_publish_list(list);
}

/* Keep RAM current for readers past the cache, after notices change. */
static void clean_notices(void)
{
  uint32_t seen = dscm_g_notices.head + dscm_g_notices.indicators_generation;
  if (seen != g_notices_seen) {
    g_notices_seen = seen;
    dscm_dc_clean(&dscm_g_notices, sizeof(dscm_g_notices));
  }
}

/* Turn new notices into header->cues for the ARM7's LED: a counter per
 * progress or leaderboard notice, and a bit while a challenge shows. */
static void update_cues(void)
{
  uint32_t head = dscm_g_notices.head, cues = g_cues, seq;
  int i;
  if (head - g_cues_head > DSCM_NOTICE_RING)
    g_cues_head = head - DSCM_NOTICE_RING;   /* older ones were overwritten */
  for (seq = g_cues_head + 1; seq - 1 != head; seq++) {
    const dscm_notice_t* x = &dscm_g_notices.ring[seq % DSCM_NOTICE_RING];
    if (x->seq != seq)
      continue;
    if (x->kind == DSCM_NOTICE_PROGRESS)
      cues = (cues & ~DSCM_CUE_PROGRESS_MASK) | ((cues + 1) & DSCM_CUE_PROGRESS_MASK);
    else if (x->kind == DSCM_NOTICE_LEADERBOARD_STARTED || x->kind == DSCM_NOTICE_LEADERBOARD_FAILED
             || x->kind == DSCM_NOTICE_LEADERBOARD_SUBMITTED)
      cues = (cues & ~DSCM_CUE_LEADERBOARD_MASK) | ((cues + (1u << DSCM_CUE_LEADERBOARD_SHIFT)) & DSCM_CUE_LEADERBOARD_MASK);
  }
  g_cues_head = head;
  cues &= ~DSCM_CUE_CHALLENGE;
  for (i = 0; i < DSCM_INDICATOR_MAX; i++)
    if (dscm_g_notices.challenges[i].id)
      cues |= DSCM_CUE_CHALLENGE;
  if (cues != g_cues) {
    g_cues = cues;
    dscm_publish_cues(cues);
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
  /* from here on the journal is only written uncached: drop its cached lines */
  dscm_dc_clean(&dscm_g_journal, sizeof(dscm_g_journal));
  dscm_dc_invalidate(&dscm_g_journal, sizeof(dscm_g_journal));
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
  /* rc_client and the journal run on UTC, as the server does, when the
   * launcher could measure the console clock's offset */
  if (g_boot.flags & DSCM_BOOT_UTC_KNOWN)
    g_boot.unix_time = dscm_console_to_utc(g_boot.unix_time, g_boot.utc_offset_q);
  g_session = (const char*)boot;
  heap_load_begin(boot->file_size);   /* before the first malloc */
  g_boot.host[sizeof(g_boot.host) - 1] = '\0';
  g_boot.username[sizeof(g_boot.username) - 1] = '\0';
  g_boot.token[sizeof(g_boot.token) - 1] = '\0';
  g_boot.game_hash[sizeof(g_boot.game_hash) - 1] = '\0';
  g_dtcm = g_boot.dtcm_base;
  dscm_irq.last_vblank = dscm_vblank_count_read();
  dscm_g_status.hardcore = g_boot.hardcore;

  memset(&platform, 0, sizeof(platform));
  platform.read_memory = read_memory;
  platform.memory_map = memory_map;
  platform.interrupt_pending = interrupt_pending;
  platform.millis = millis;
  platform.http_send = http_send;
  platform.event = on_event;
  platform.unix_time = dscm_unix_time;
  platform.clock_is_utc = (g_boot.flags & DSCM_BOOT_UTC_KNOWN) != 0;
  platform.clock = g_boot.clock;
  dscm_g_status.clock = g_boot.clock;
  platform.journal = (dscm_journal_t*)DSCM_UNCACHED(&dscm_g_journal.j);
  platform.notices = &dscm_g_notices;
  platform.publish_list = publish_list;
  platform.frame = frame_count;

  memset(&config, 0, sizeof(config));
  config.host = g_boot.host;
  config.username = g_boot.username;
  config.token = g_boot.token;
  config.hardcore = g_boot.hardcore;
  config.spectator = (g_boot.flags & DSCM_BOOT_SPECTATOR) ? 1 : 0;
  config.max_addresses = 4096;
  config.snapshot_slots = 1024;          /* ~17 s of frames with no idle time (loading, save writes) */
  config.snapshot_bytes = HEAP_ARENA_BYTES;  /* shared by queued frames: a few hundred bytes each, typically */
  config.capture_at_halt = 1;            /* snapshot when the game halts, not in its VBlank interrupt */
  config.idle_max_frames = 4;            /* then the network and a fresh snapshot get their turn */

  heap_arena_window(1);                  /* the arena goes where the session's replies are */
  i = dsc_init(&platform, &config);
  heap_arena_window(0);
  if (i != 0) {
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
  /* The replies and rc_api's parse of the set are in the arena's place, which
   * the first frame overwrites: the load must be over, and nothing may
   * answer from the session again (later requests get "Not fetched"). An
   * unfinished load never resumes: module_idle runs only when active. */
  for (i = 0; i < DSCM_PRELOAD_COUNT; i++)
    g_boot.preload_length[i] = 0;
  if (heap_load_end() != 0 && dsc_is_active()) {
    dsc_shutdown();
    dscm_g_status.error = DSCM_ERROR_NO_MEMORY;
  }

  dscm_g_status.active = dsc_is_active();
  if (dscm_g_status.active)
    update_progress();
  clean_notices();
  update_cues();

  /* Keep using the launcher's Wi-Fi connection during play. */
  if (dscm_g_status.active && g_boot.net_valid) {
    static char user_agent[80];
    net_config_t nc;
    void* ring;
    memset(&nc, 0, sizeof(nc));
    memcpy(nc.mac, g_boot.mac, 6);
    nc.ip = g_boot.ip;
    nc.netmask = g_boot.netmask;
    nc.gateway = g_boot.gateway;
    nc.dns = g_boot.dns;
    memcpy(nc.ap_ssid, g_boot.ap_ssid, sizeof(nc.ap_ssid));
    nc.ap_ssid_len = g_boot.ap_ssid_len;
    memcpy(nc.ap_bssid, g_boot.ap_bssid, 6);
    nc.ap_channel = g_boot.ap_channel;
    nc.ap_security = g_boot.ap_security;
    nc.ap_pair_cipher = g_boot.ap_pair_cipher;
    nc.ap_group_cipher = g_boot.ap_group_cipher;
    nc.pmk_valid = g_boot.pmk_valid;
    memcpy(nc.pmk, g_boot.pmk, 32);
    nc.host = g_boot.host;
    strcpy(user_agent, "DScheevos/" DSC_VERSION " (Nintendo DSi) rcheevos/" RCHEEVOS_VERSION_STRING);
    nc.user_agent = user_agent;
    entropy_stir(&g_boot.unix_time, sizeof(g_boot.unix_time));
    ring = net_init(&nc);
    /* the link keeps the one copy of the PMK it needs */
    memset(nc.pmk, 0, sizeof(nc.pmk));
    memset(g_boot.pmk, 0, sizeof(g_boot.pmk));
    memset(((dscm_boot_t*)g_session)->pmk, 0, sizeof(g_boot.pmk));
    dscm_dc_clean(((dscm_boot_t*)g_session)->pmk, sizeof(g_boot.pmk));
    /* still in the session: nothing writes the arena before the first frame */
    if (g_boot.ca_length && g_boot.ca_offset <= g_boot.file_size && g_boot.ca_length <= g_boot.file_size - g_boot.ca_offset)
      https_add_ca(g_session + g_boot.ca_offset, g_boot.ca_length);
    dscm_publish_net_ring(ring);
    dscm_g_status.net_state = DSCM_NET_STATE_CONNECTING;
    g_net_started = 1;
  }
  g_initialized = 1;
  g_idle.frame = dscm_irq.frames;
  dscm_irq.on = dscm_g_status.active ? 1 : 0;
  return dscm_g_status.active ? 0 : -1;
}

/* The rest of a VBlank that needs a copy in the IRQ (vblank_entry in start.s
 * calls it on the module's IRQ stack). */
void module_vblank_copy(void)
{
  refresh_dtcm();
  dsc_vblank_copy();
}

/* Frames queued and not evaluated yet (dropped ones were never queued). */
static uint32_t backlog(void)
{
  dsc_stats_t s;
  dsc_get_stats(&s);
  return s.frames_snapshotted - s.frames_evaluated - s.frames_discarded;
}

/* The core's counters for the status (and the in-game menu). */
static void publish_stats(void)
{
  dsc_stats_t s;
  dsc_get_stats(&s);
  dscm_g_status.frames_snapshotted = s.frames_snapshotted;
  dscm_g_status.frames_dropped = s.frames_dropped;
  dscm_g_status.frames_evaluated = s.frames_evaluated;
  dscm_g_status.captures = s.captures;
  dscm_g_status.captures_stopped = s.captures_stopped;
  dscm_g_status.frames_stale = s.frames_stale;
  /* the IRQ's own counters */
  dscm_g_status.frames_missed = dscm_irq.frames_missed;
  dscm_g_status.timed_frames = dscm_irq.timed_frames;
  dscm_g_status.irq_lines_last = dscm_irq.irq_lines_last;
  dscm_g_status.irq_lines_max = dscm_irq.irq_lines_max;
  dscm_g_status.irq_lines_total = dscm_irq.irq_lines_total;
  dscm_g_status.idle_lines_last = g_idle.last;
  dscm_g_status.idle_lines_max = g_idle.max;
  dscm_g_status.idle_lines_total = g_idle.total;
  dscm_g_status.idle_none = g_idle.none;
  dscm_g_status.idle_cut = dscm_irq.idle_cut;
}

static void update_progress(void)
{
  dsc_progress_t p;
  dsc_get_progress(&p);
  dscm_g_status.achievements = p.achievements;
  dscm_g_status.achievements_unlocked = p.unlocked;
  dscm_g_status.points = p.points;
  dscm_g_status.points_unlocked = p.points_unlocked;
  if (p.hardcore_refused)
    dscm_g_status.hardcore = DSCM_HARDCORE_REFUSED;
  g_progress_stale = 0;
}

/* From reset_entry (start.s); IRQs are off. */
void module_reset(void)
{
  g_reset_pending = 1;
}

/* Called on the module's own stack from halt_entry, in the game's idle thread.
 * NitroSDK switches threads when an IRQ returns, so any game thread that wakes
 * (the main loop at VBlank, a loader thread at a card IRQ) preempts this work
 * at once; it resumes the next time the game is idle. eval_lines is wall time
 * between start and end, so it includes any time the game took meanwhile.
 * The idle lines in the status stop at the VBlank that preempts a pass (see
 * vblank_entry), so they leave out the game's frame but also the rest of
 * that pass; idle_cut counts such passes.
 * Returns nonzero when the pass took a scanline or more: an IRQ may have come
 * and gone meanwhile, so halt_entry returns to the game's loop instead of
 * waiting for the next one. */
int module_idle(int idle_loop)
{
  uint32_t start, end, used;
  int new_frame, evaluated;

  start = REG_VCOUNT;
  if (!g_initialized || !dscm_g_status.active)
    return 0;
  /* A game thread that halts until its IRQ comes (while (!flag) OS_Halt())
   * is waiting for its own frame: any work here would delay the game. Only
   * the idle thread's loop is free time. */
  if (!idle_loop) {
    dscm_g_status.halts_other++;
    return 0;
  }
  dscm_g_status.halts_idle++;
  {
    uint32_t cpsr = dscm_irq_off();
    idle_frames(0);
    dscm_irq.idle_start = start;
    dscm_irq.idle_open = 1;
    dscm_irq_restore(cpsr);
  }

  /* The idle thread wakes at every IRQ, often many times a frame (about 20
   * in Kirby Super Star Ultra). Work that only needs doing once a frame
   * (notices, LED cues, status, an idle network) waits for the first pass
   * after a VBlank, or a pass that evaluated something. */
  new_frame = g_frames != g_pass_frame;
  g_pass_frame = g_frames;

  if (g_reset_pending) {
    g_reset_pending = 0;
    dsc_reset();
  }
  if (dscm_g_status.pending_requests)
    poll_network();   /* answers that came in since the last pass */
  if (g_progress_stale)
    update_progress();

  if (backlog()) {
    /* Evaluation may take the rest of the frame: copy the memory the game
     * halted with first, so the next VBlank has it even if the copy before
     * the halt below comes too late. */
    dscm_irq_restore(capture());
    start = REG_VCOUNT;
    evaluated = dsc_idle();
    end = REG_VCOUNT;
    used = lines_between(start, end);
    dscm_g_status.eval_lines = (dscm_g_status.eval_lines * 7 + used * 16) / 8;
  }
  else {
    evaluated = dsc_idle();   /* answers, rcheevos' own timers: retries, pings */
  }
  if (new_frame || evaluated) {
    clean_notices();
    update_cues();
  }

  /* Then the network: achievements come first. Every pass while something
   * is in flight or waiting (so an unlock goes out at once), else once a
   * frame for lwIP's timers. */
  if (g_net_started && (new_frame || net_busy() || request_waiting())) {
    net_service(millis());
    dscm_g_status.net_state = net_state();
    dscm_g_status.net_detail = net_detail();
    dscm_g_status.tls_handshake_ms = net_handshake_ms();
    dscm_g_status.net_rejoins = link_rejoins();
    poll_network();
  }
  /* Last, the snapshot and the halt, with IRQs masked from one to the
   * other: the idle thread runs only when every game thread is blocked, so
   * this is the game's memory when it stopped, and no game code can run
   * between the copy and the halt. The CPU wakes from the halt when an IRQ
   * is requested (IE & IF), masked or not; the IRQ is taken when the mask
   * is lifted. The copy stops at once if an IRQ is already waiting, and the
   * halt then returns at once too.
   * It halts even with frames still waiting: the next IRQ brings the next
   * pass. In melonDS, an idle loop that never halted stalled Kirby Super
   * Star Ultra's streaming for a minute (tests/perf/AB.md). */
  {
    uint32_t cpsr = capture();
    start = dscm_irq.idle_start;
    /* else a VBlank came meanwhile and counted the pass */
    idle_frames(dscm_take(&dscm_irq.idle_open) ? lines_since(start) : 0);
    if (new_frame || evaluated)
      publish_stats();
    dscm_wfi();
    dscm_irq_restore(cpsr);
    return 1;   /* halted here */
  }
}

void module_shutdown(void)
{
  if (g_initialized)
    dsc_shutdown();
  g_initialized = 0;
  dscm_irq.on = 0;
  dscm_g_status.active = 0;
}

dscm_status_t* module_status(void) { return &dscm_g_status; }
dscm_net_slot_t* module_net(void) { return dscm_g_net; }
