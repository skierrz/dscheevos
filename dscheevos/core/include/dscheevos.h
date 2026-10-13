/*
 * DScheevos core: runs rcheevos inside a DS game.
 *
 * The platform layer (nds-bootstrap hooks on hardware, or a test harness)
 * calls three entry points:
 *
 *   dsc_vblank()  at VBlank, in interrupt context: copies the game RAM the
 *                 achievement set needs into a snapshot. Short and bounded.
 *                 With capture_at_halt, the copy is made by dsc_capture()
 *                 when the game halts, and dsc_vblank() only counts the
 *                 VBlank; idle time queues the frame.
 *   dsc_capture() (capture_at_halt only) when the game has finished its
 *                 frame and halts, with interrupts disabled: copies the game
 *                 RAM for the next dsc_vblank(). Stops early if
 *                 interrupt_pending() says an interrupt is waiting.
 *   dsc_idle()    when the game is idle: evaluates waiting snapshots with
 *                 rcheevos and finishes server replies. Can be interrupted.
 *   dsc_http_done()  when a request the core asked for has an answer.
 *
 * Everything else (memory map, clock, network, display) comes through
 * dsc_platform_t, so the same code runs on the DS and in host tests.
 *
 * What a display layer and the launcher read (ABI v6 in dscheevos_module.h)
 * is written by the core in idle time, never in dsc_vblank():
 *   journal   every unlock and leaderboard entry, and whether the server answered
 *   notices   one-shot notices and the challenge/tracker indicators
 *   list      the core set, rebuilt when stale and handed to publish_list()
 */
#ifndef DSCHEEVOS_H
#define DSCHEEVOS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dscm_journal_t;            /* dscheevos_module.h */
struct dscm_notices_t;
struct dscm_list_t;

/* Unlock and status notifications for the display layer. */
typedef enum dsc_event_type_t {
  DSC_EVENT_LOGIN_OK = 1,
  DSC_EVENT_LOGIN_FAILED,
  DSC_EVENT_GAME_LOADED,
  DSC_EVENT_GAME_LOAD_FAILED,
  DSC_EVENT_UNLOCK,              /* achievement earned (id, title, points) */
  DSC_EVENT_UNLOCK_SENT,         /* server accepted it */
  DSC_EVENT_LEADERBOARD_SUBMIT,
  DSC_EVENT_MASTERY,
  DSC_EVENT_DEGRADED,            /* evaluation fell behind and dropped frames */
  DSC_EVENT_SERVER_ERROR
} dsc_event_type_t;

typedef struct dsc_event_t {
  dsc_event_type_t type;
  uint32_t id;
  uint32_t points;
  const char* title;             /* valid only during the callback */
  const char* detail;
} dsc_event_t;

/* Request the platform must send. Body is form-encoded POST data. */
typedef struct dsc_http_request_t {
  const char* url;
  const char* post_data;         /* NULL for GET */
  const char* content_type;
  void* handle;                  /* pass back to dsc_http_done */
} dsc_http_request_t;

typedef struct dsc_platform_t {
  /* Read live game memory using RetroAchievements DS addresses
   * (0x000000-0x3FFFFF main RAM, 0x1000000-0x1003FFF DTCM). Returns bytes read. */
  uint32_t (*read_memory)(uint32_t address, uint8_t* buffer, uint32_t num_bytes);
  /* Monotonic milliseconds. */
  uint32_t (*millis)(void);
  /* Start an HTTP request; call dsc_http_done() with the result later, from idle context. */
  void (*http_send)(const dsc_http_request_t* request);
  /* Notifications for the display layer (may be NULL). */
  void (*event)(const dsc_event_t* event);
  /* Diagnostics (may be NULL). */
  void (*log)(const char* message);
  /* Console clock, for journal entries (may be NULL: time()). */
  uint32_t (*unix_time)(void);
  /* Offline journal, written in place (on the DS through the uncached mirror). May be NULL. */
  struct dscm_journal_t* journal;
  /* Notices for a display layer, written in place. May be NULL. */
  struct dscm_notices_t* notices;
  /* A complete achievement list to make visible; the core owns both buffers
   * and never writes the one published last. May be NULL. */
  void (*publish_list)(struct dscm_list_t* list);
  /* Platform frame count, stamped on notices (may be NULL: the core's own count). */
  uint32_t (*frame)(void);
  /* Where game memory is right now, so dsc_vblank() can read it directly:
   * main RAM (RetroAchievements address 0) and DTCM (0x1000000), NULL for a
   * region that can't be read. Both must be word-aligned (the snapshot
   * loads whole words). Called once per dsc_vblank(). read_memory()
   * must agree: the same bytes, and a read not entirely inside one region
   * fails. May be NULL: the snapshot then reads through read_memory(),
   * which costs more. */
  void (*memory_map)(const uint8_t** main_ram, const uint8_t** dtcm);
  /* capture_at_halt: polled every few words while dsc_capture() copies with
   * interrupts masked. Lets any waiting interrupt run (dsc_vblank() may run
   * then) and returns nonzero if the copy must start over: game code may
   * have run meanwhile. May be NULL. */
  int (*interrupt_pending)(void);
  /* Brackets allocations that another party finds by address (the
   * achievement list): on(1) before, on(0) after, so the platform can keep
   * them where that party looks. May be NULL. */
  void (*shared_alloc)(int on);
} dsc_platform_t;

typedef struct dsc_config_t {
  const char* host;              /* e.g. "https://retroachievements.org" */
  const char* username;
  const char* token;             /* login token, never a password */
  int hardcore;                  /* 1: hardcore session */
  uint32_t max_addresses;        /* snapshot capacity, e.g. 4096 */
  uint32_t snapshot_slots;       /* most frames that can wait for evaluation; 0 = 2, max 1024 */
  uint32_t snapshot_bytes;       /* memory for queued snapshots; 0 = snapshot_slots * max_addresses * 4 */
  int spectator;                 /* 1: rc_client spectator mode, nothing is sent or journaled */
  int capture_at_halt;           /* 1: dsc_capture() takes the snapshot, dsc_vblank() queues it */
  uint32_t idle_max_frames;      /* frames one dsc_idle() call evaluates at most; 0 = all queued when it starts */
} dsc_config_t;

typedef struct dsc_stats_t {
  uint32_t frames_snapshotted;
  uint32_t frames_evaluated;
  uint32_t frames_dropped;       /* snapshots lost because evaluation was behind */
  uint32_t frames_discarded;     /* queued snapshots from before a game reset */
  uint32_t frames_repeated;      /* snapshotted frames queued as a repeat of the one before */
  uint32_t live_reads;           /* reads not covered by the snapshot (new or moved addresses) */
  uint32_t addresses;            /* addresses captured each VBlank */
  uint32_t pending_requests;
  uint32_t journal_lost;         /* unsent journal entries overwritten because it was full */
  uint32_t list_builds;          /* achievement lists published */
  /* capture_at_halt */
  uint32_t captures;             /* dsc_capture() calls that finished a copy */
  uint32_t captures_stopped;     /* ... that stopped for an interrupt */
  uint32_t frames_stale;         /* VBlanks with no capture since the one before: evaluated with the last copy */
} dsc_stats_t;

/* Progress in the loaded game's core set (for menus). */
typedef struct dsc_progress_t {
  uint32_t achievements;
  uint32_t unlocked;
  uint32_t points;
  uint32_t points_unlocked;
  int hardcore_refused;          /* the server sent its "unsupported emulator" warning: it files hardcore unlocks as softcore */
} dsc_progress_t;

/* Set up the core. Returns 0 on success. */
int dsc_init(const dsc_platform_t* platform, const dsc_config_t* config);

/* Log in with the token, then identify and load the game by its RetroAchievements
 * hash (rcheevos DS hash). Completes over later dsc_idle()/dsc_http_done() calls. */
int dsc_begin_session(const char* game_hash);

/* 1 once the game's set is loaded and frames are being evaluated. */
int dsc_is_active(void);

/* VBlank, interrupt context. */
void dsc_vblank(void);

/* dsc_vblank() in two parts, for a handler that keeps the common case off
 * its own stack: dsc_vblank_queue() uses no stack and calls nothing, and
 * returns nonzero when this VBlank also needs dsc_vblank_copy() (a copy in
 * the interrupt: capture_at_halt off, or nothing captured yet). */
int dsc_vblank_queue(void);
void dsc_vblank_copy(void);

/* capture_at_halt: the game halted, interrupts disabled. Returns 0 if the
 * snapshot is ready for the next dsc_vblank(), 1 if it stopped for an
 * interrupt (call again at the next halt), -1 if nothing was taken. */
int dsc_capture(void);

/* Idle time. Returns the number of frames evaluated. */
int dsc_idle(void);

/* Answer for a request from dsc_platform_t.http_send. */
void dsc_http_done(void* handle, int http_status, const char* body, size_t body_length);

/* Idle context (it queues the frames dsc_vblank() counted first). */
void dsc_get_stats(dsc_stats_t* stats);

/* The game was reset: drop queued frames and reset achievement state, as
 * an emulator does on reset. Call from idle context, not from an IRQ. */
void dsc_reset(void);

void dsc_get_progress(dsc_progress_t* progress);

/* Tear down: unload the game and free everything. */
void dsc_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
