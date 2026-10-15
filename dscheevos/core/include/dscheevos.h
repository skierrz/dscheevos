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
 *                 With config.handover, only while the ARM9 owns the
 *                 rc_client; dsc_arm7_step() is the ARM7's evaluator.
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
  /* Clock for journal entries (may be NULL: time()). */
  uint32_t (*unix_time)(void);
  /* Offline journal, written in place; journal_sync (below) makes each step visible
   * in RAM. May be NULL. */
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
  /* Makes journal writes so far visible in RAM, in order (on the DS: clean the
   * data cache lines when the mirror is cached). Called at every ordering
   * point of a journal update. May be NULL (writes are already in RAM). */
  void (*journal_sync)(const void* addr, uint32_t len);
  /* unix_time is UTC (else a console clock whose offset from UTC is unknown). */
  int clock_is_utc;
  /* DSCM_CLOCK_*: the speed-ups the session runs with, for the journal. */
  int clock;
  /* unix_time as it was frames_ago VBlanks ago: journal entries are stamped
   * with the time their frame was captured, not evaluated. May be NULL:
   * unix_time() less frames_ago / 60 s, rounded. */
  uint32_t (*unix_time_at)(uint32_t frames_ago);
  /* handover only, called on the ARM9 (the CPU with a data cache) around its
   * turns with the rc_client: acquire = 1 after the ARM7 granted it, before
   * the ARM9 reads any shared state (invalidate those lines by address);
   * acquire = 0 before the ARM9 gives it back (clean those lines by address,
   * drain the write buffer). May be NULL (no cache, or the host). */
  void (*ownership)(int acquire);
  /* With it, dsc_http_done keeps the platform's body buffer (no copy, no
   * allocation on the network side) until the owner has copied it, then
   * calls http_release(handle): the buffer is the platform's again. Needed
   * with hand-over (the owner may be the other CPU, and the heap is one).
   * May be NULL: dsc_http_done copies the body at once. */
  void (*http_release)(void* handle);
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
  int capture_at_halt;           /* 1: dsc_capture() takes the snapshot, dsc_vblank() queues it;
                                    DSC_CAPTURE_ONCE: see below */
  uint32_t idle_max_frames;      /* frames one dsc_idle() call evaluates at most; 0 = all queued when it starts */
  int handover;                  /* 1: two evaluators share the rc_client (see "Hand-over" below); 0: dsc_idle() owns it */
  /* dsc_handover_policy (P1, "ARM7 first, ARM9 when behind"): the ARM9 wants
   * the rc_client from hi frames OR hi bytes of backlog and gives it back at
   * lo frames AND lo bytes. 0 = the defaults: 6 / 1 frames, a quarter / a
   * sixteenth of snapshot_bytes. Busy frames of heavy sets are KBs each, so
   * the bytes, not the frames, decide for them. */
  uint32_t handover_hi_frames, handover_lo_frames;
  uint32_t handover_hi_bytes, handover_lo_bytes;
  /* Lag cap: a frame that would make the backlog more than cap frames or
   * cap bytes is not queued (a gap, counted in frames_dropped, as when the
   * ring or arena is full). 0 = no cap but the ring's and the arena's. */
  uint32_t lag_cap_frames, lag_cap_bytes;
} dsc_config_t;

typedef struct dsc_stats_t {
  uint32_t frames_snapshotted;
  uint32_t frames_evaluated;
  uint32_t frames_dropped;       /* snapshots lost because evaluation was behind */
  uint32_t frames_discarded;     /* always 0: queued frames are evaluated before a reset now (kept for the layout) */
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
  uint32_t notices_lost;         /* notices that found the waiting queue full (one frame raised too many) */
  uint32_t notices_waiting;      /* raised, not in the ring yet (at most DSC_NOTICES_PER_FRAME go in per frame) */
  uint32_t handovers;            /* times the ARM9 took or gave back the rc_client (handover) */
  uint32_t lifecycle_lost;       /* resets or hardcore switches that found the ring full (never, in practice) */
  uint32_t arena_used;           /* snapshot bytes held now (queued frames and the one evaluated last), of config.snapshot_bytes */
  uint32_t frame_bytes;          /* arena bytes one distinct frame takes with the current table (repeats take none) */
  uint32_t frames_capped;        /* of frames_dropped: refused by the lag cap */
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
 * interrupt (call again at the next halt), -1 if nothing was taken.
 * capture_at_halt = DSC_CAPTURE_ONCE: one snapshot a frame, the first
 * dsc_capture() after a VBlank that completes; it is that VBlank's frame
 * (taken after the game's work that followed the VBlank, as a frontend
 * that evaluates after each emulated frame sees it) and is queued at once.
 * Later calls in the frame take nothing (-1). A frame with no completed
 * capture before the next VBlank is evaluated with the last snapshot. */
#define DSC_CAPTURE_ONCE 2
int dsc_capture(void);

/* Idle time. Returns the number of frames evaluated. */
int dsc_idle(void);
int dsc_idle_max(uint32_t max_frames);   /* LAB (coverage) */

/* Answer for a request from dsc_platform_t.http_send. */
void dsc_http_done(void* handle, int http_status, const char* body, size_t body_length);

/* Idle context (it queues the frames dsc_vblank() counted first; with
 * hand-over it only reads, since it may be called on either CPU: call
 * dsc_drain() on the ARM9 first for the latest frame counts). */
void dsc_get_stats(dsc_stats_t* stats);

/* Nonzero if dsc_idle() has work besides the once-a-frame kind: frames queued
 * (VBlanks not committed yet, or snapshots not evaluated) or answers waiting.
 * Cheap (a few words, no copy): for a caller that wakes many times a frame
 * and wants to know whether this wake needs dsc_idle() at all; a new frame
 * (rcheevos' timers) always does. */
int dsc_work_pending(void);

/* Commit the VBlanks queued so far (dsc_vblank_queue) into the frame ring,
 * as dsc_capture and dsc_idle do first. Lets the platform do that work with
 * interrupts enabled before a capture that runs with them masked. */
void dsc_drain(void);

/* DS: before dsc_init, which view of RAM the core's own state is used
 * through: 0x02000000 (cached) or 0x0C000000 (the mirror). No-op elsewhere. */
void dsc_state_view(uint32_t high_byte);

/* The game was reset: reset achievement state, as an emulator does on
 * reset. Queued in frame order: every frame captured before the reset is
 * still evaluated first, then rcheevos is reset, then the frames after it.
 * Call from idle context (the ARM9), not from an IRQ. */
void dsc_reset(void);

/* Hardcore off (never on again), queued in frame order like dsc_reset. */
void dsc_hardcore_off(void);

/* Evaluate every frame queued now, lifecycle changes included, ignoring
 * idle_max_frames and notice pacing: for a window where the game is frozen
 * (in-game menu, reset, quit). The caller must own the rc_client (always
 * without handover). Returns the frames evaluated, -1 if it doesn't own it. */
int dsc_flush(void);

/* Frames (and queued lifecycle changes) captured and not evaluated yet. */
uint32_t dsc_backlog(void);

/* Snapshot arena bytes held now (queued frames and the one evaluated last),
 * of config.snapshot_bytes. Producer side, cheap. */
uint32_t dsc_arena_used(void);

/* ---- Hand-over (config.handover) ----
 * One rc_client, evaluated by whichever CPU owns it, one whole frame at a
 * time, oldest queued frame first; ownership changes only between frames.
 * Two flags, each written by one CPU only: req9 (ARM9) and grant7 (ARM7).
 * The ARM9 owns it iff req9 && grant7, else the ARM7 does:
 *   (0,0) ARM7 owns -> ARM9 asks: (1,0), ARM7 still owns until its frame ends
 *   -> ARM7 grants between frames: (1,1) ARM9 owns -> ARM9 gives it back
 *   between frames: (0,1) nobody evaluates -> ARM7 takes it: (0,0).
 * The ARM9 asks again only from (0,0), so the two never evaluate at once.
 * The ARM9 never waits: it asks and goes on; dsc_idle() evaluates only once
 * the grant has come. Everything that touches the rc_client (frames, server
 * replies, rcheevos' timers, the list, notices) runs on the owner.
 * dsc_vblank/dsc_capture/dsc_reset/dsc_hardcore_off stay on the ARM9.
 * dsc_init() starts with the ARM9 owning it (1,1), for the set load and the
 * first frame; dsc_want(0) then hands it to the ARM7. */
#define DSC_OWNER_ARM9 0
#define DSC_OWNER_ARM7 1
#define DSC_NOTICES_PER_FRAME 8    /* notices that go into the ring per platform frame at most */

/* ARM9, idle context: want the rc_client (1) or give it back (0). Giving it
 * back happens at once (the ARM9 is between frames whenever it can call
 * this); taking it happens in a later dsc_idle(), once the ARM7 granted it. */
void dsc_want(int want);

/* ARM9, idle context: the P1 policy. Wants the rc_client when the game is
 * frozen (menu, reset, quit) or the backlog reaches hi frames or hi bytes,
 * and gives it back once it is down to lo frames and lo bytes (config).
 * Calls dsc_want; returns what it wants. */
int dsc_handover_policy(int frozen);

/* Who owns the rc_client now: DSC_OWNER_*. Without handover, the ARM9. */
int dsc_owner(void);

/* ARM7: when the ARM7 owns the rc_client, evaluate up to max_frames queued
 * frames (0: all queued when it starts), stopping early between frames if
 * the ARM9 asked for it; grant it to the ARM9 between frames when asked.
 * Returns the frames evaluated. */
int dsc_arm7_step(uint32_t max_frames);

void dsc_get_progress(dsc_progress_t* progress);

/* Tear down: evaluate the frames still queued (when the caller owns the
 * rc_client, see dsc_flush), then unload the game and free everything. */
void dsc_shutdown(void);

#ifdef DSC_CHECK_FAST_STEPS
/* Host tests only: called at points where the other CPU may run in the
 * middle of a cross-CPU step (1: take_table, 2: before the producer adds a
 * frame to the ring, 3: before the owner evaluates a frame, 4: before the
 * owner frees a sealed slot). */
extern void (*dsc_test_interleave)(int point);   /* 5: after take_table, before the copy */
void dsc_test_republish(uint32_t address);
int dsc_test_layout(void);
#endif

#ifdef DSC_TEST_HOOKS
/* Host tests only (tests/difftest core mode): see the frames and rcheevos'
 * events as rc_client makes them. */
typedef struct dsc_test_hooks_t {
  void (*rc_event)(const void* event, void* client);   /* every rc_client_event_t, before the core handles it */
  void (*frame_begin)(uint32_t frame);                  /* before a queued frame's rc_client_do_frame (core frame number) */
  void (*frame_end)(void* client, uint32_t frame);      /* after it */
  void (*lifecycle)(int what);                          /* a queued reset (1) or hardcore off (2), before it is applied */
  int sync_replies;                                     /* an answer given inside http_send goes to rc_client at once, as a synchronous server would */
} dsc_test_hooks_t;
void dsc_test_set_hooks(const dsc_test_hooks_t* hooks);
void* dsc_test_client(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
