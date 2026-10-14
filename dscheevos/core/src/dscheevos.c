/*
 * DScheevos core. See dscheevos.h for the model.
 *
 * Snapshot: rcheevos reads game memory through read_memory(). While a
 * snapshot is being evaluated, reads are served from it. When the game loads,
 * the address table gets every address the set reads directly, and the
 * pointer chains (AddAddress) are copied: each VBlank runs the copies on
 * live memory and keeps the bytes each pointer lands on in the snapshot
 * (common steps are computed by the core, the rest by rcheevos' own code;
 * see compile_chains). So a queued frame is evaluated with memory from its
 * own VBlank, even when a pointer moved. Anything still missing (a chain
 * that uses Remember/Recall) is read live and added to the table; that read
 * may be from a later frame, and live_reads counts it.
 *
 * Address tables are immutable once published, and sized to what they
 * hold (at most max_addresses). Publishing one also lays out its snapshot
 * (every aligned word holding a watched byte, see build_program), the list
 * of word copies dsc_vblank() runs, reading game memory directly through the
 * platform's memory_map, and where each of the set's direct reads lands in
 * the snapshot, in the order rcheevos makes them (see read_memory). Each
 * snapshot remembers which table it was taken with; a table no snapshot
 * uses any more is freed.
 *
 * When a set reads more addresses than max_addresses, the table holds the
 * first ones and the rest are read live at evaluation (live_reads), at the
 * cost of a plain read each.
 *
 * Snapshots queue in a ring: dsc_vblank (IRQ) adds at the head, dsc_idle
 * evaluates and frees at the tail, oldest first. With capture_at_halt the
 * IRQ only counts the VBlank (dsc_vblank_queue, see Q); idle time adds it
 * to the ring (drain). Their bytes live in one
 * arena, also used as a ring, so each frame costs only the bytes watched
 * and a long stretch without idle time (loading, save writes) queues many
 * frames instead of dropping them.
 *
 * Journal, notices and the achievement list (ABI v6) are filled from
 * rc_client's events and server replies, all in idle time.
 *
 * DSC_CHANGED_RANGES (needs rc_client_set_changed_memory, our rcheevos fork):
 * before each evaluated frame, rcheevos is told exactly which memory may read
 * differently than in the frame evaluated before (changes_before): the
 * snapshot words and chain targets that differ between the two snapshots. It
 * then skips the indirect reads whose pointer and target didn't change. The
 * snapshot evaluated last stays in the arena for this (S.last). Anything else
 * (no frame evaluated before, another table or memory map, a frame missed, a
 * live read, a reset or load) is "unknown": every indirect read is redone.
 */
#include "dscheevos.h"
#include "../../module/include/dscheevos_module.h"

#include "rc_client.h"
#include "rc_api_runtime.h"
#include "rc_consoles.h"
#include "client/rc_client_internal.h"
#include "runtime/rc_internal.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_SLOTS 1024
#define EVENT_RESERVE 16      /* ring slots only lifecycle changes may use: a reset is never lost to a full ring */
#define NOTICE_QUEUE 32       /* notices waiting for the ring (DSC_NOTICES_PER_FRAME a frame go in) */
#define NOTICE_HEADROOM 16    /* evaluation pauses while more than NOTICE_QUEUE - this are waiting */
#define TABLES 3              /* current, plus up to two still used by queued snapshots */
#define MAX_PENDING_HTTP 8
#define LIST_MIN_FRAMES 30    /* rebuild the achievement list at most this often */
#define WARNING_ID 101000001u /* the server's "unsupported emulator" achievement, and above */

/* The snapshot (dsc_vblank, in the game's interrupt) and the reads that
 * evaluation makes: ARM code, optimized for speed. The rest of the core is
 * built as the module Makefile says (Thumb, for size). */
#if defined(__arm__) && defined(__GNUC__) && !defined(__clang__)
#define HOT __attribute__((target("arm"), optimize("O2")))
/* dsc_vblank_queue and Q: the module's linker script puts them next to its
 * own VBlank code and data, so the IRQ touches few cache lines. */
#define IRQ_TEXT __attribute__((target("arm"), optimize("O2"), section(".text.irq")))
#define IRQ_DATA __attribute__((aligned(32), section(".data.irq")))
/* What every idle wake runs (the snapshot copy and its bookkeeping): one
 * section, which the module's linker script keeps contiguous and in a fixed
 * place, so a wake fetches few code lines and edits elsewhere don't move them. */
#define WAKE __attribute__((target("arm"), optimize("O2"), section(".text.wake.core")))
#define WAKE_SMALL __attribute__((section(".text.wake.core")))
/* Small helpers of a wake, compiled into their caller (dsc_capture): every
 * function has its own literal pool, a D-cache line that each wake reads
 * again after the game evicted it; one caller is one pool. */
#define WAKE_INL static inline __attribute__((always_inline))
/* Rare code (lifecycle changes, hand-over, quit): linked after everything
 * else (the module's .text_cold). */
#define COLD __attribute__((section(".text_cold"), noinline))
/* A pointer every function reads: kept with the code, in cached memory. */
#define CACHED_DATA __attribute__((section(".data.cached")))
#else
#define HOT
#define IRQ_TEXT
#define IRQ_DATA
#define WAKE
#define WAKE_SMALL
#define WAKE_INL static inline
#define COLD
#define CACHED_DATA
#endif

/* Readers (in-game menu, ARM7) may look at any time: a count or head is
 * stored only after what it covers. */
#define PUBLISH_BARRIER() __asm__ __volatile__("" ::: "memory")
/* Between a store and a load of words the other CPU writes (take_table and
 * the owner's table release, a Dekker-style pair): a compiler barrier on the
 * host; on the DS the ARM9 must also drain its write buffer here (step 1). */
#define CROSS_BARRIER() PUBLISH_BARRIER()

/* Test builds: points where the other CPU may run in the middle of a
 * cross-CPU step (tests/test_core.c simulates it there). */
#ifdef DSC_CHECK_FAST_STEPS
void (*dsc_test_interleave)(int point);
#define INTERLEAVE(point) do { if (dsc_test_interleave) dsc_test_interleave(point); } while (0)
static void check_failed(const char* what, uint32_t index);
#else
#define INTERLEAVE(point) ((void)0)
#endif

/* Word access to game memory and snapshots, which are byte arrays. */
typedef uint32_t __attribute__((may_alias)) word_t;
typedef uint16_t __attribute__((may_alias)) half_t;

typedef struct run_t {
  uint32_t address;           /* RA address of the first byte */
  uint32_t offset;            /* index of the first byte in the table */
  uint32_t length;
} run_t;

/* A byte copied on its own: in a word where some watched byte can't be read. */
typedef struct fix_t {
  uint32_t address;           /* RA address, or BAD_ADDRESS: always 0 */
  uint32_t at;                /* byte in the snapshot */
} fix_t;

typedef struct table_t {
  uint32_t* addresses;        /* sorted, unique, one entry per byte; NULL: unused */
  uint32_t count;
  run_t* runs;                /* only kept without a memory map (and in test builds) */
  uint32_t run_count;
  uint32_t generation;
  uint16_t* seq;              /* per direct read (S.seq): its byte in the snapshot, or SEQ_* */

  /* Snapshot layout and copy program, built when the table is published.
   * The snapshot holds every aligned word with a watched byte, in this
   * order: words copied singly (main RAM, then DTCM), words copied in
   * blocks (main, then DTCM; a block may include unwatched words), words
   * that are always 0, words copied byte by byte. */
  void* program;              /* one allocation for the arrays below */
  uint32_t* where;            /* per address: its byte in the snapshot */
  uint32_t* mask;             /* per snapshot word: its watched bytes, for the repeat check */
  uint32_t words;
  uint32_t* single;           /* region offset of each single word */
  uint32_t single_main, single_count;
  uint32_t* block;            /* (region offset, word count) per block */
  uint32_t block_main, block_count, block_words_main, block_words;
  uint32_t zero_words;
  fix_t* fix;
  uint32_t fix_count;
#ifdef DSC_CHANGED_RANGES
  uint32_t* word_address;     /* per snapshot word: the RA address of its first byte */
#endif
} table_t;

#define BAD_ADDRESS 0xFFFFFFFFu
#define SEQ_LIVE 0xFFFEu      /* no byte of it in the table (the table is full): read live */
#define SEQ_SLOW 0xFFFFu      /* anything else: the general path */
#define SEQ_LOOKAHEAD 4       /* direct reads rcheevos may skip (a memref it disabled) */
#define CURSOR_WINDOW 16      /* chain reads looked ahead for the one evaluation asks for */
#define BLOCK_MIN 4           /* watched words for a block copy to beat single words */

enum { SLOT_FREE, SLOT_FILLING, SLOT_READY, SLOT_EVAL };

/* Bytes a pointer chain landed on at VBlank, stored after the table's bytes. */
typedef struct target_t {
  uint32_t address;
  uint32_t length;
  uint8_t bytes[4];
} target_t;

/* A pointer chain's start: a value read at a fixed address. */
typedef struct root_t {
  uint32_t address;
  uint16_t copy;               /* index in chains.direct */
  uint8_t bytes;               /* 1, 2 or 4; 0: another size, read by rcheevos */
  uint8_t sync;                /* also keep the rcheevos copy (prior, changed) */
} root_t;

/* A chain step as run_chains computes it (see compile_chains). */
typedef struct cstep_t {
  uint32_t k;                  /* constant modifier */
  uint32_t mask;               /* on the parent's value: its size, and any AND folded in */
  uint16_t parent;             /* value index */
  uint16_t copy;               /* index in chains.mods */
  uint8_t fast;                /* 1, 2 or 4: a plain read of that many bytes; 0: run_step */
  uint8_t op;                  /* CS_* */
  uint8_t flags;               /* CF_* */
  uint8_t reserved;
} cstep_t;

/* The most common chain, run in one go: an aligned 32-bit pointer in main
 * RAM, masked, plus an offset, read once; nothing else uses the result. */
typedef struct leaf_t {
  uint32_t root;               /* RA address of the pointer */
  uint32_t mask;
  uint32_t k;
} leaf_t;

/* Private copies of the set's pointer chains, run at VBlank. */
typedef struct chains_t {
  rc_memref_t* direct;         /* pointers the chains start from */
  uint32_t direct_count;
  rc_modified_memref_t* mods;  /* chain steps, parents first */
  uint32_t mod_count;
  uint32_t target_count;       /* steps that read memory (one target_t each) */
  /* compiled (NULL in a reference copy) */
  root_t* roots;               /* directs a step needs: aligned 32-bit main RAM reads first */
  uint32_t root_count, plain_roots;
  leaf_t* leaves;              /* 1-byte reads, then 2, then 4 */
  uint32_t leaf_count[3];
  cstep_t* steps;              /* the other steps, in order; ANDs folded into the steps using them */
  uint32_t step_count;
  uint32_t* values;            /* current value of each root, then each step */
  rc_memref_value_t** vref;    /* the rcheevos copy behind each value */
  uint16_t* target_order;      /* target slot -> its read's place among the chain reads */
  uint16_t* target_slot;       /* the reverse */
} chains_t;

/* A queued frame (or lifecycle entry). Written by the producer only (the
 * ARM9's capture side); the consumer (the rc_client's owner) reads it and
 * keeps its own progress in S.done[]. A slot is the producer's again once
 * slot_tail has passed it. */
typedef struct slot_t {
  volatile int state;         /* SLOT_*, the producer's own bookkeeping */
  uint8_t* data;
  uint32_t offset;            /* in the arena */
  uint32_t size;
  const table_t* table;
  target_t* targets;          /* chains.target_count of them */
  uint32_t frame;
  /* Later frames whose bytes matched exactly; evaluated from this copy.
   * Only the newest slot grows, and the consumer frees a slot only once a
   * newer one exists (slot_head moved past it), so it never misses one. */
  volatile uint32_t repeats;
  uint32_t map;               /* how game memory was read (mem_t): only a snapshot taken the same way can repeat it */
  uint8_t carry;              /* capture_at_halt: no capture for this frame, evaluated with the last snapshot's memory */
  uint8_t event;              /* QEV_*: not a frame but a lifecycle change, applied in its place in the ring */
} slot_t;

/* Lifecycle changes queued in frame order (slot_t.event). */
enum { QEV_NONE, QEV_RESET, QEV_HARDCORE_OFF };

/* A request rc_client made (owner side). */
typedef struct http_pending_t {
  int used;
  uint32_t taken;             /* reply[].seq when the owner last took an answer */
  rc_client_server_callback_t callback;
  void* callback_data;
  uint8_t journal_kind;       /* DSCM_JOURNAL_* if the request is an award or a leaderboard entry */
  uint32_t journal_id;
  int32_t journal_score;      /* leaderboard entries only */
} http_pending_t;

/* Its answer (network side, dsc_http_done): seq != http[].taken means one waits. */
typedef struct http_reply_t {
  volatile uint32_t seq;
  int status;
  const char* body;           /* the platform's buffer (platform.http_release), else a copy */
  size_t body_length;
  int copied;                 /* body is our malloc'd copy */
} http_reply_t;

#define LINE __attribute__((aligned(32)))    /* a new cache line: the groups below have one writer each */
#define NO_KEEP 0xFFFFFFFFu

/* The core's state, in groups by writer (hand-over: the ARM9's data cache
 * writes back whole 32-byte lines, so two writers must never share one):
 *   set at init, then only read;
 *   P: the producer (the ARM9: capture, VBlank, drain, reset; its idle side);
 *   C: the owner of the rc_client (evaluation and everything rc_client does);
 *   N: the network side (dsc_http_done).
 * slots[] are P's, done[] are C's. */
static struct state_t {
  /* ---- set at init ---- */
  int initialized;
  dsc_platform_t platform;
  dsc_config_t config;
  rc_client_t* client;
  uint32_t slot_count;         /* most frames that may wait (the ring also keeps the newest evaluated one) */
  uint8_t* arena;
  uint32_t arena_size;
  char game_hash[33];

  /* ---- P: producer ---- */
  volatile uint32_t slot_head LINE; /* written by commit (drain), dsc_vblank_copy and push_event, never at once */
  volatile uint32_t items_in;  /* frames and lifecycle entries queued, all time (repeats count) */
  const table_t* volatile held;/* the table a capture or stage in slots[slot_head] uses (see take_table) */
  volatile int active;         /* set last: dsc_vblank uses the table and chains only after it */
  uint32_t arena_head;         /* end of the newest snapshot's bytes */
  volatile uint32_t frame_counter;
  int have_capture;            /* a frame was copied since dsc_init or the last dsc_reset */
  uint32_t idle_clock;         /* platform frame (or millis) when dsc_idle() last did its housekeeping */
  /* hand-over, the ARM9's own side (next to idle_clock: dsc_idle's early
   * return reads this line anyway) */
  int a9_want;                 /* dsc_want */
  int a9_acquired;             /* platform.ownership(1) done for this turn */
  /* capture_at_halt: slots[slot_head] holds a capture for the next VBlank (Q.stage) */
  int stage_same;              /* its bytes and targets equal the newest queued snapshot's */
  uint32_t stage_prev;         /* slot_head - 1 when it was taken */
  volatile int stage_failed;   /* the last capture found no room: the frame is dropped */
  int capturing;               /* in dsc_capture with interrupt_pending: poll it */
  uint32_t capture_frame;      /* Q.count when the copy began */
  uint8_t* scratch;            /* dsc_vblank without memory_map: the longest run */
  uint32_t scratch_size;
  /* Chain copies whose rcheevos state (prior, changed) advances with each
   * frame: saved before a capture, put back if it is replaced or stopped. */
  rc_memref_value_t** sync;
  rc_memref_value_t* sync_saved;
  uint32_t sync_count;
  int sync_dirty;              /* the values hold an uncommitted capture's state */
  chains_t chains;             /* compiled at load; the capture runs them */
  dsc_stats_t pstats;          /* the producer's counters (dsc_get_stats merges them) */
#ifdef DSC_CHECK_FAST_STEPS
  chains_t ref;                /* chains run only by rcheevos' own code */
  uint8_t* shadow[MAX_SLOTS];  /* per slot: the snapshot as the old code took it */
  uint32_t shadow_size[MAX_SLOTS];
#endif
  slot_t slots[MAX_SLOTS] LINE;/* ring: [slot_tail, slot_head) are queued */

  /* ---- C: the rc_client's owner ---- */
  volatile uint32_t slot_tail LINE; /* written by the owner only */
  volatile uint32_t items_out; /* items_in the owner has finished */
  /* The oldest arena offset the owner still needs besides the queued slots
   * (S.last's bytes), or NO_KEEP: the one word of S.last the producer reads. */
  volatile uint32_t keep;
  slot_t* evaluating;
  uint32_t eval_frame;         /* the frame rc_client_do_frame is running for (repeats counted) */
  /* The snapshot evaluated last: its bytes stay in the arena while a carry
   * slot may still need them. */
  slot_t last;
  int has_last;
  int hardcore_refused;
  table_t tables[TABLES];
  table_t* volatile current;   /* read by the producer */
  int tables_spare;            /* a table other than current may hold memory */
  int table_full_logged;
  uint32_t* pending_new;       /* addresses missed during evaluation, merged after the frame */
  uint32_t pending_new_count;
  uint32_t pending_new_size;   /* allocated */
  /* The direct reads rcheevos makes each frame, in its order (every memref
   * it reads, rc_client_update_memref_values): table_t.seq says where each
   * lands. seq_next is the one expected next while a frame is evaluated. */
  uint32_t* seq_address;
  uint8_t* seq_bytes;
  uint32_t seq_count;
  uint32_t seq_next;
  uint32_t target_cursor;      /* the chain read rcheevos likely asks for next (see read_memory) */
  http_pending_t http[MAX_PENDING_HTTP];
  dscm_list_t* list_buf[2];    /* double buffer; list_published is the one readers may use */
  dscm_list_t* list_published;
  uint32_t list_max;
  uint32_t list_generation;
  uint32_t list_frame;         /* frame_counter at the last build */
  int list_stale;
  /* progress for dsc_get_progress, published like the list (two copies) */
  dsc_progress_t progress[2];
  volatile uint32_t progress_index;
  int progress_stale;
  dsc_stats_t stats;           /* the owner's counters */
  uint32_t dropped_reported;   /* frames_dropped when DSC_EVENT_DEGRADED was last raised */
  /* notices raised, waiting for the ring (owner): DSC_NOTICES_PER_FRAME a
   * platform frame go in, so a reader that looks once a frame sees them all */
  dscm_notice_t* nq;
  uint32_t nq_head, nq_tail;   /* [nq_tail, nq_head) wait */
  uint32_t nq_frame, nq_sent;  /* platform frame, and notices put in the ring during it */
  int flushing;                /* dsc_flush: no notice pacing, no pause for it */
#ifdef DSC_CHANGED_RANGES
  /* The frame evaluated last (see changes_before): its snapshot, kept in
   * the arena while S.last or the slot being evaluated holds it. */
  const uint8_t* prev_data;
  const target_t* prev_targets;
  const table_t* prev_table;
  uint32_t prev_map;
  uint32_t prev_next_frame;    /* the frame after it */
  int prev_valid;
  int live_read;               /* read_memory read game memory live in this frame */
  rc_client_memory_range_t* ranges;
  uint32_t range_count, range_size;
#endif
  uint32_t done[MAX_SLOTS];    /* per slot: frames of it evaluated so far */

  volatile uint32_t http_taken;/* answers given to rcheevos, all time (http_answered: the network's count) */

  /* ---- N: the network side ---- */
  http_reply_t reply[MAX_PENDING_HTTP] LINE;
  volatile uint32_t http_answered;  /* answers stored, all time */
  uint32_t end LINE;           /* nothing after N shares its last line */
} S_storage;
/* The state through the view the platform picked (dsc_state_view): cached,
 * or uncached through the DS's 0x0C000000 mirror. Evaluation reads it for
 * every memory read rcheevos makes, so heavy sets want it cached; light sets
 * want it out of the cache (a capture at every idle wake touches it). */

/* What the VBlank IRQ reads and writes with capture_at_halt, once something
 * is captured: nothing else of the core. It counts the VBlank and hands the
 * stage to it; drain() (idle time) then adds the frames to the ring, as
 * commit(). Only the IRQ writes count, frames and sealed; only idle time
 * writes taken. */
enum { MODE_OFF, MODE_COPY, MODE_QUEUE };
static struct {
  volatile uint32_t mode;      /* MODE_COPY: dsc_vblank_copy() checks (and may switch to MODE_QUEUE) */
  volatile uint32_t stage;     /* slots[slot_head] holds a capture for the next VBlank */
  volatile uint32_t count;     /* every VBlank: did one come during a copy? */
  volatile uint32_t frames;    /* VBlanks queued, all time */
  volatile uint32_t sealed;    /* the one of those the stage went to */
  volatile uint32_t taken;     /* frames drain() has committed */
} Q_storage IRQ_DATA;

/* The two pointers every wake follows, in one cached line (.data.cached):
 * the state's view and Q. The module build gives their fixed address
 * (DSC_VIEWS_ADDR, module.ld asserts it): the code then builds it from an
 * immediate and an offset, so wake code reads no literal pool for them (a
 * pool is a cache line of ours that the game evicts and every wake reads
 * again, evicting one of the game's). */
struct dsc_views_t {
  struct state_t* s;
  __typeof__(Q_storage)* q;
};
struct dsc_views_t dsc_views CACHED_DATA = { &S_storage, &Q_storage };
#ifdef DSC_VIEWS_ADDR
/* 0x02D80000 (the module's base) in two instructions that are the same in ARM
 * and Thumb code, no literal; not volatile, so computed once per function. */
static inline __attribute__((always_inline)) uintptr_t views_base(void)
{
  uintptr_t b;
  __asm__(".syntax unified\n\tmovs %0, #0xB6\n\tlsls %0, %0, #18" : "=l"(b) : : "cc");
  return b;
}
_Static_assert((DSC_VIEWS_ADDR & 0xFFFF0000u) == 0x02D80000u, "views_base()");
#define VIEWS (*(struct dsc_views_t*)(views_base() + (DSC_VIEWS_ADDR & 0xFFFFu)))
#else
#define VIEWS dsc_views
#endif
#define S_view (VIEWS.s)
#define S (*VIEWS.s)
#define Q (*VIEWS.q)

/* Hand-over (config.handover): who owns the rc_client. Each word has one
 * writer; on the DS both must be read and written past the ARM9's data
 * cache (step 1 places them uncached). */
static struct {
  volatile uint32_t req9 LINE; /* ARM9 only: it wants the rc_client, or has it */
  volatile uint32_t grant7 LINE; /* ARM7 only: it stopped and let the ARM9 have it */
} O;

#ifdef DSC_TEST_HOOKS
static dsc_test_hooks_t T;
#endif

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

/* Returns 0, or -1 if out of memory. */
static int build_runs(table_t* t)
{
  uint32_t i, n = 0;
  for (i = 0; i < t->count; i++)
    n += !i || t->addresses[i - 1] + 1 != t->addresses[i];
  free(t->runs);
  t->runs = (run_t*)malloc((n ? n : 1) * sizeof(run_t));
  if (!t->runs)
    return -1;
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
  return 0;
}

/* Memory regions, as the platform's read_memory has them: a read must be
 * entirely inside one, or it fails and the snapshot keeps zeros. */
enum { REGION_MAIN, REGION_DTCM, REGION_NONE, REGION_MIXED };
#define MAIN_SIZE 0x400000u
#define DTCM_START 0x1000000u
#define DTCM_SIZE 0x4000u

static HOT int region_of(uint32_t address, uint32_t length)
{
  if (address < MAIN_SIZE && length <= MAIN_SIZE - address)
    return REGION_MAIN;
  if (address - DTCM_START < DTCM_SIZE && length <= DTCM_SIZE - (address - DTCM_START))
    return REGION_DTCM;
  return REGION_NONE;
}

static uint32_t region_offset(uint32_t address)
{
  return address < MAIN_SIZE ? address : address - DTCM_START;
}

/* Build time only: a snapshot word and how it is copied. */
typedef struct word_info_t {
  uint32_t address;           /* aligned */
  uint32_t mask;              /* watched bytes */
  uint32_t first;             /* its first address in the table */
  uint8_t region;             /* REGION_*: of every watched byte's run */
  uint8_t block;              /* 1: copied in a block */
  uint8_t gap;                /* blocks: unwatched words before this one */
  uint8_t reserved;
} word_info_t;

/* Lay out the snapshot of a table and compile the copies dsc_vblank runs
 * (see table_t). Single words cost a load and a store each; a cluster of
 * watched words at most one word apart is copied as one block when it has
 * BLOCK_MIN or more. A watched byte whose run can't be read whole stays 0,
 * as read_memory would leave it; words with both kinds go byte by byte.
 * Then where each of the set's direct reads (S.seq) lands (seq_of).
 * Returns 0, or -1 if out of memory (the caller frees what it allocated). */
static uint16_t seq_of(const table_t* t, uint32_t address, uint32_t n);

/* dsc_capture copies with interrupts masked, polling every POLL_WORDS words
 * or chain steps (stop_now) */
#define POLL_WORDS 8

static int build_program(table_t* t)
{
  word_info_t* w;
  uint8_t* run_region;
  uint32_t i, j, r, n = 0, at, single_main = 0, single = 0, block_main = 0, blocks = 0;
  uint32_t block_words_main = 0, block_words = 0, zero = 0, mixed = 0, fix_count = 0;
  uint32_t pos[6], words, longest = 0;
  size_t size;
  uint8_t* p;

  if (build_runs(t) != 0)
    return -1;
  /* without a memory map, copy_snapshot reads a run at a time */
  for (r = 0; r < t->run_count; r++)
    longest = t->runs[r].length > longest ? t->runs[r].length : longest;
  if (!S.platform.memory_map && longest > S.scratch_size) {
    uint8_t* b = (uint8_t*)realloc(S.scratch, longest);
    if (!b)
      return -1;
    S.scratch = b;
    S.scratch_size = longest;
  }
  w = (word_info_t*)malloc((t->count ? t->count : 1) * sizeof(*w));
  run_region = (uint8_t*)malloc(t->run_count ? t->run_count : 1);
  if (!w || !run_region) {
    free(w);
    free(run_region);
    return -1;
  }
  for (r = 0; r < t->run_count; r++)
    run_region[r] = (uint8_t)region_of(t->runs[r].address, t->runs[r].length);

  /* one entry per word, addresses ascending */
  for (i = 0, r = 0; i < t->count; i++) {
    uint32_t a = t->addresses[i], region;
    while (i >= t->runs[r].offset + t->runs[r].length)
      r++;
    region = run_region[r];
    if (!n || w[n - 1].address != (a & ~3u)) {
      memset(&w[n], 0, sizeof(w[n]));
      w[n].address = a & ~3u;
      w[n].first = i;
      w[n].region = (uint8_t)region;
      n++;
    }
    else if (w[n - 1].region != region) {
      w[n - 1].region = REGION_MIXED;
    }
    w[n - 1].mask |= 0xFFu << ((a & 3) * 8);
  }

  /* blocks: clusters of words in one readable region, at most one unwatched word apart */
  for (i = 0; i < n; i = j) {
    uint32_t span;
    j = i + 1;
    if (w[i].region > REGION_DTCM)
      continue;
    while (j < n && w[j].region == w[i].region && w[j].address - w[j - 1].address <= 8)
      j++;
    if (j - i < BLOCK_MIN)
      continue;
    span = (w[j - 1].address - w[i].address) / 4 + 1;
    for (r = i; r < j; r++) {
      w[r].block = 1;
      w[r].gap = (uint8_t)(r == i ? 0 : (w[r].address - w[r - 1].address) / 4 - 1);
    }
    if (w[i].region == REGION_MAIN) {
      block_main++;
      block_words_main += span;
    }
    blocks++;
    block_words += span;
  }
  for (i = 0; i < n; i++) {
    if (w[i].region == REGION_NONE)
      zero++;
    else if (w[i].region == REGION_MIXED) {
      mixed++;
      for (j = 0; j < 4; j++)
        fix_count += (w[i].mask >> (j * 8)) & 1;
    }
    else if (!w[i].block) {
      single++;
      single_main += w[i].region == REGION_MAIN;
    }
  }
  words = single + block_words + zero + mixed;

  size = (t->count + words + single + 2 * blocks) * sizeof(uint32_t) + fix_count * sizeof(fix_t) +
         S.seq_count * sizeof(uint16_t);
#ifdef DSC_CHANGED_RANGES
  size += words * sizeof(uint32_t);
#endif
  p = (uint8_t*)malloc(size ? size : 1);
  if (!p) {
    free(w);
    free(run_region);
    return -1;
  }
  free(t->program);
  t->program = p;
  t->where = (uint32_t*)p;
  t->mask = t->where + t->count;
  t->single = t->mask + words;
  t->block = t->single + single;
#ifdef DSC_CHANGED_RANGES
  t->word_address = t->block + 2 * blocks;
  memset(t->word_address, 0, words * sizeof(uint32_t));
  t->fix = (fix_t*)(t->word_address + words);
#else
  t->fix = (fix_t*)(t->block + 2 * blocks);
#endif
  t->seq = (uint16_t*)(t->fix + fix_count);
  t->words = words;
  t->single_main = single_main;
  t->single_count = single;
  t->block_main = block_main;
  t->block_count = blocks;
  t->block_words_main = block_words_main;
  t->block_words = block_words;
  t->zero_words = zero;
  t->fix_count = 0;

  /* next word in each part of the snapshot */
  pos[0] = 0;                                      /* single, main */
  pos[1] = single_main;                            /* single, DTCM */
  pos[2] = single;                                 /* blocks, main */
  pos[3] = single + block_words_main;              /* blocks, DTCM */
  pos[4] = single + block_words;                   /* zero */
  pos[5] = pos[4] + zero;                          /* byte by byte */
  for (i = 0, r = 0, blocks = 0; i < n; i++) {
    int part;
    if (w[i].region == REGION_NONE)
      part = 4;
    else if (w[i].region == REGION_MIXED)
      part = 5;
    else
      part = (w[i].block ? 2 : 0) + (w[i].region == REGION_DTCM);
    if (w[i].block) {
      if (i == 0 || !w[i - 1].block || w[i - 1].region != w[i].region || w[i].address - w[i - 1].address > 8) {
        r = blocks++;
        t->block[r * 2] = region_offset(w[i].address);
        t->block[r * 2 + 1] = 1;
      }
      else {
        for (j = 0; j < w[i].gap; j++)
          t->mask[pos[part]++] = 0;
        t->block[r * 2 + 1] += w[i].gap + 1;
      }
    }
    else if (part < 2) {
      t->single[pos[part]] = region_offset(w[i].address);
    }
    at = pos[part]++;
    t->mask[at] = w[i].mask;
#ifdef DSC_CHANGED_RANGES
    t->word_address[at] = w[i].address;
#endif
    for (j = w[i].first; j < t->count && (t->addresses[j] & ~3u) == w[i].address; j++) {
      t->where[j] = at * 4 + (t->addresses[j] & 3);
      if (part == 5) {
        fix_t* f = &t->fix[t->fix_count++];
        uint32_t k;
        for (k = 0; j >= t->runs[k].offset + t->runs[k].length; k++)
          ;
        f->address = run_region[k] == REGION_NONE ? BAD_ADDRESS : t->addresses[j];
        f->at = t->where[j];
      }
    }
  }
  /* blocks are numbered main first: they came in address order, main RAM before DTCM */
  free(w);
  free(run_region);
  for (i = 0; i < S.seq_count; i++)
    t->seq[i] = seq_of(t, S.seq_address[i], S.seq_bytes[i]);
#ifndef DSC_CHECK_FAST_STEPS
  if (S.platform.memory_map) {
    free(t->runs);    /* only copy_snapshot without a map reads them */
    t->runs = NULL;
  }
#endif
  return 0;
}

static void table_free(table_t* t)
{
  free(t->addresses);
  free(t->runs);
  free(t->program);
  memset(t, 0, sizeof(*t));
}

static int table_in_use(const table_t* t)
{
  int i;
  uint32_t n;
  (void)i;
  if (t == S.current)
    return 1;
  /* the producer's capture or stage (take_table): read after current was
   * stored (CROSS_BARRIER in the caller's publish), so either it saw the new
   * current or this sees its table */
  CROSS_BARRIER();
  if (S.held == t)
    return 1;
  if (S.has_last && S.last.table == t)
    return 1;
  for (n = S.slot_tail; n != S.slot_head; n++)
    if (S.slots[n % MAX_SLOTS].table == t)
      return 1;
  return 0;
}

/* Merge addresses missed during the last evaluation into a new table, sized
 * to what it holds, and publish it. */
static void publish_pending_addresses(void)
{
  table_t* old = S.current;
  table_t* t = NULL;
  table_t next;
  uint32_t i, n;

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

  /* note_missing keeps old->count + pending_new_count within max_addresses */
  memset(&next, 0, sizeof(next));
  n = old->count + S.pending_new_count;
  next.addresses = (uint32_t*)malloc(n * sizeof(uint32_t));
  if (!next.addresses)
    return;    /* no memory now; the addresses stay pending */
  if (old->count)
    memcpy(next.addresses, old->addresses, old->count * sizeof(uint32_t));
  memcpy(next.addresses + old->count, S.pending_new, S.pending_new_count * sizeof(uint32_t));
  qsort(next.addresses, n, sizeof(uint32_t), cmp_u32);

  /* drop duplicates */
  for (i = 0; i < n; i++)
    if (next.count == 0 || next.addresses[next.count - 1] != next.addresses[i])
      next.addresses[next.count++] = next.addresses[i];
  if (next.count < n) {
    uint32_t* fit = (uint32_t*)realloc(next.addresses, next.count * sizeof(uint32_t));
    if (fit)
      next.addresses = fit;
  }

  if (build_program(&next) != 0) {
    table_free(&next);
    return;    /* no memory now; the addresses stay pending */
  }
  next.generation = old->generation + 1;
  table_free(t);    /* nothing uses it (table_in_use) */
  *t = next;
  S.pending_new_count = 0;
  if (S.pending_new_size > 64) {
    /* the load's list (every address the set reads) is not needed again */
    free(S.pending_new);
    S.pending_new = NULL;
    S.pending_new_size = 0;
  }
  PUBLISH_BARRIER();
  S.current = t; /* single word store: dsc_vblank sees the old or the new table, never a mix */
  CROSS_BARRIER();
  S.tables_spare = t != old;
  S.stats.addresses = t->count;
  if (t->count >= S.config.max_addresses && !S.table_full_logged) {
    S.table_full_logged = 1;
    dsc_log("snapshot table full (max_addresses): further addresses are read live");
  }
}

/* Idle time: free the tables no snapshot uses any more. */
static void release_tables(void)
{
  uint32_t i;
  int spare = 0;
  if (!S.tables_spare)
    return;
  for (i = 0; i < TABLES; i++) {
    table_t* t = &S.tables[i];
    if (t == S.current || !t->addresses)
      continue;
    if (table_in_use(t))
      spare = 1;
    else
      table_free(t);
  }
  S.tables_spare = spare;
}

/* The first index whose address is at least address. */
static HOT uint32_t table_lower(const table_t* t, uint32_t address)
{
  uint32_t lo = 0, hi = t->count;
  while (lo < hi) {
    uint32_t mid = (lo + hi) >> 1;
    if (t->addresses[mid] < address)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

/* A whole read inside one run of the table, stored in order in the
 * snapshot and inside one region (lo: table_lower). */
static inline HOT int table_run_read(const table_t* t, uint32_t lo, uint32_t address, uint32_t num_bytes)
{
  return lo < t->count && t->addresses[lo] == address && lo + num_bytes <= t->count &&
      t->addresses[lo + num_bytes - 1] == address + num_bytes - 1 &&
      t->where[lo + num_bytes - 1] == t->where[lo] + num_bytes - 1 &&
      (address + num_bytes <= MAIN_SIZE || address - DTCM_START <= DTCM_SIZE - num_bytes);
}

/* Where a direct read of the set lands in a table's snapshot (table_t.seq):
 * read_memory serves it from there as the general path would. SEQ_LIVE: no
 * byte of it is in the table and the table is full, so the general path
 * would read it live (it only looks among the chain reads first, which
 * hold the same bytes unless memory changed since the snapshot). */
static uint16_t seq_of(const table_t* t, uint32_t address, uint32_t n)
{
  uint32_t lo = table_lower(t, address);
  if (table_run_read(t, lo, address, n))
    return t->where[lo] < SEQ_LIVE ? (uint16_t)t->where[lo] : SEQ_SLOW;
  if ((lo == t->count || t->addresses[lo] - address >= n) && t->count >= S.config.max_addresses &&
      region_of(address, n) != REGION_NONE)
    return SEQ_LIVE;
  return SEQ_SLOW;
}

/* Only as many as the next table has room for: publish_pending_addresses
 * would drop the rest (and a full table then costs nothing per read). */
static void note_missing(uint32_t address)
{
  uint32_t i;
  if (S.current->count + S.pending_new_count >= S.config.max_addresses)
    return;
  for (i = 0; i < S.pending_new_count; i++)
    if (S.pending_new[i] == address)
      return;
  if (S.pending_new_count == S.pending_new_size) {
    uint32_t size = S.pending_new_size ? S.pending_new_size * 2 : 16;
    uint32_t* p = (uint32_t*)realloc(S.pending_new, size * sizeof(uint32_t));
    if (!p)
      return;    /* stays a live read */
    S.pending_new = p;
    S.pending_new_size = size;
  }
  S.pending_new[S.pending_new_count++] = address;
}

static int hardcore_refused(rc_client_t* client);

/* ---- game memory at VBlank ---- */

/* Where dsc_vblank reads game memory: straight from the platform's memory
 * map, or through read_memory() when the platform has none. */
typedef struct mem_t {
  const uint8_t* base[2];      /* REGION_MAIN, REGION_DTCM; NULL: can't be read */
  int direct;                  /* the platform gave a memory map */
} mem_t;

/* Returns which regions are mapped (bits 0, 1) and whether there is a map (bit 2). */
WAKE_INL uint32_t mem_open(mem_t* m)
{
  m->base[0] = m->base[1] = NULL;
  m->direct = S.platform.memory_map != NULL;
  if (m->direct)
    S.platform.memory_map(&m->base[0], &m->base[1]);
  return (m->base[0] ? 1 : 0) | (m->base[1] ? 2 : 0) | (m->direct ? 4 : 0);
}

/* 2 or 4 bytes at an unaligned address, little-endian. Out of line: inlined,
 * GCC merges it with the aligned load and uses bytes for both. */
static HOT __attribute__((noinline)) uint32_t load_unaligned(const uint8_t* p, uint32_t n)
{
  return n == 2 ? p[0] | (p[1] << 8) : p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 1, 2 or 4 bytes at p, little-endian. */
static inline HOT uint32_t load(const uint8_t* p, uint32_t n)
{
  if (n == 1)
    return *p;
  if ((uintptr_t)p & (n - 1))
    return load_unaligned(p, n);
  return n == 2 ? *(const half_t*)p : *(const word_t*)p;
}

/* n (1, 2 or 4) bytes at an RA address, little-endian, as read_memory reads
 * them into a zeroed buffer. Returns how many bytes were read. */
static inline HOT uint32_t peek(const mem_t* m, uint32_t address, uint32_t n, uint32_t* value)
{
  const uint8_t* p;
  uint32_t o;
  if (!m->direct) {
    uint8_t b[4] = { 0, 0, 0, 0 };
    o = S.platform.read_memory(address, b, n);
    *value = b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
    return o;
  }
  if (address < MAIN_SIZE && n <= MAIN_SIZE - address)
    p = m->base[REGION_MAIN] ? m->base[REGION_MAIN] + address : NULL;
  else if ((o = address - DTCM_START) < DTCM_SIZE && n <= DTCM_SIZE - o)
    p = m->base[REGION_DTCM] ? m->base[REGION_DTCM] + o : NULL;
  else
    p = NULL;
  if (!p) {
    *value = 0;
    return 0;
  }
  *value = load(p, n);
  return n;
}

/* ---- pointer chains ---- */

/* dsc_capture runs with interrupts masked: every POLL_WORDS words or chain
 * steps the platform lets any waiting interrupt run. It stops if the
 * platform says game code may have run, or a VBlank came (the copy would be
 * of two frames). */
static inline HOT int stop_now(void)
{
  return S.capturing && (S.platform.interrupt_pending() || Q.count != S.capture_frame);
}

/* How run_chains computes a step. Reads and the common arithmetic on
 * plain unsigned values are computed here (the value is what rcheevos'
 * operand code gives for them); anything else goes through rcheevos. */
enum {
  CS_READ1 = 1, CS_READ2 = 2, CS_READ4 = 4,   /* indirect read of that many bytes */
  CS_AND, CS_XOR, CS_ADD, CS_SUB, CS_MUL,
  CS_GENERIC                                  /* rc_get_modified_memref_value */
};
#define CF_DELTA 1    /* the parent is read as its delta */
#define CF_PRIOR 2    /* ... or its prior */
#define CF_SYNC  4    /* keep the rcheevos copy up to date (prior, changed) */
#define CF_READS 8    /* rcheevos computes it, and it reads memory (one target) */
#define FAST_STATE 0x80    /* cstep_t.fast: the parent is read as its delta or prior */

/* Reads a chain step's target live at VBlank and keeps the bytes in the snapshot. */
typedef struct capture_t {
  target_t* targets;
  uint32_t next;
} capture_t;

static HOT uint32_t RC_CCONV capture_read(uint32_t address, uint8_t* buffer, uint32_t num_bytes, void* ud)
{
  capture_t* c = (capture_t*)ud;
  target_t* g = &c->targets[c->next++];
  uint32_t n = num_bytes > sizeof(g->bytes) ? 0 : S.platform.read_memory(address, buffer, num_bytes);
  g->address = address;
  g->length = n;
  *(word_t*)g->bytes = 0;
  memcpy(g->bytes, buffer, n);
  return n;
}

static HOT uint32_t RC_CCONV live_read(uint32_t address, uint8_t* buffer, uint32_t num_bytes, void* ud)
{
  (void)ud;
  return S.platform.read_memory(address, buffer, num_bytes);
}

/* A chain step that isn't a plain read: computed here or by rcheevos.
 * Writes its value, and its target if it reads; returns the next target. */
static HOT __attribute__((noinline)) target_t* run_step(const cstep_t* s, target_t* g, const mem_t* m, uint32_t* out)
{
  const chains_t* c = &S.chains;
  uint32_t p = c->values[s->parent], v;
  if (s->op == CS_GENERIC) {
    rc_modified_memref_t* mm = &c->mods[s->copy];
    capture_t cap;
    cap.targets = g;
    cap.next = 0;
    v = rc_get_modified_memref_value(mm, capture_read, &cap);
    if ((s->flags & CF_READS) && !cap.next) {
      g->address = 0;    /* no read happened: keep the slot count fixed */
      g->length = 0;
      *(word_t*)g->bytes = 0;
    }
    if (s->flags & CF_READS)
      g++;
  }
  else {
    if (s->flags & (CF_DELTA | CF_PRIOR)) {
      const rc_memref_value_t* pv = c->vref[s->parent];
      p = ((s->flags & CF_PRIOR) || pv->changed) ? pv->prior : pv->value;
    }
    p &= s->mask;
    switch (s->op) {
      case CS_READ1:
      case CS_READ2:
      case CS_READ4:
        g->address = p + s->k;
        g->length = peek(m, g->address, s->op, &v);
        *(word_t*)g->bytes = v;    /* unread bytes are 0, as in rc_read_memory */
        g++;
        break;
      case CS_AND: v = p & s->k; break;
      case CS_XOR: v = p ^ s->k; break;
      case CS_ADD: v = p + s->k; break;
      case CS_SUB: v = p - s->k; break;
      default: v = p * s->k; break;    /* CS_MUL */
    }
  }
  *out = v;
  if (s->flags & CF_SYNC)
    rc_update_memref_value(&c->mods[s->copy].memref.value, v);
  return g;
}

/* One leaf (see leaf_t) of size n, main RAM mapped at ram. */
static inline HOT __attribute__((always_inline)) void run_leaf(const leaf_t* l, uint32_t n, target_t* g, const mem_t* m, const uint8_t* ram)
{
  uint32_t a = (*(const word_t*)(ram + l->root) & l->mask) + l->k, v, length = n;
  if (a <= MAIN_SIZE - n)
    v = load(ram + a, n);
  else
    length = peek(m, a, n, &v);    /* DTCM, or nothing */
  g->address = a;
  g->length = length;
  *(word_t*)g->bytes = v;
}

/* Leaves of size n. With *prev (the newest queued snapshot's targets),
 * compares each target with it until one differs, then sets *prev to NULL
 * and just runs the rest. */
static inline HOT __attribute__((always_inline)) target_t* run_leaves(const leaf_t* l, uint32_t count, uint32_t n, target_t* g,
                                                                  const mem_t* m, const uint8_t* ram, const target_t** prev)
{
  const leaf_t* end = l + count;
  const target_t* p = *prev;
  if (p) {
    for (; l < end; l++, g++, p++) {
      run_leaf(l, n, g, m, ram);
      /* the length follows from the address, with the same regions readable (slot_t.map) */
      if ((p->address ^ g->address) | (*(const word_t*)p->bytes ^ *(const word_t*)g->bytes)) {
        p = NULL;
        l++;
        g++;
        break;
      }
    }
    *prev = p;
  }
  for (; l < end; l++, g++)
    run_leaf(l, n, g, m, ram);
  return g;
}

/* Leaves without a memory map (or main RAM): through peek. */
static HOT target_t* run_leaves_slow(const leaf_t* l, const uint32_t* count, target_t* g, const mem_t* m, const target_t** prev)
{
  const target_t* p = *prev;
  uint32_t k, n;
  for (k = 0; k < 3; k++) {
    for (n = count[k]; n; n--, l++, g++) {
      uint32_t a, v;
      peek(m, l->root, 4, &a);
      a = (a & l->mask) + l->k;
      g->address = a;
      g->length = peek(m, a, 1u << k, &v);
      *(word_t*)g->bytes = v;
      if (p && memcmp(p++, g, sizeof(*g)) != 0)
        p = NULL;
    }
  }
  *prev = p;
  return g;
}

/* At VBlank, after the table's bytes are copied: advance the chain copies one
 * frame on live memory, as rc_update_memref_values does for the real ones.
 * Every target slot is written: leaves first, then the other steps'. With
 * prev (the newest queued snapshot's targets), returns 1 if every target
 * is the same as there, else 0; -1 if it stopped for an interrupt. */
static WAKE __attribute__((noinline)) int run_chains(target_t* targets, const mem_t* m, const target_t* prev)
{
  const chains_t* c = &S.chains;
  const uint8_t* ram = m->direct ? m->base[REGION_MAIN] : NULL;
  uint32_t* values = c->values;
  uint32_t* out = values + c->root_count;
  const root_t* r = c->roots;
  const leaf_t* l = c->leaves;
  const cstep_t* s = c->steps;
  const cstep_t* end = s + c->step_count;
  target_t* g = targets;
  uint32_t i = 0, n = c->root_count, plain = ram ? c->plain_roots : 0, left, k;

  /* roots: aligned 32-bit pointers in main RAM, then any others */
  for (; i < plain; i++)
    values[i] = *(const word_t*)(ram + r[i].address);
  for (; i < n; i++) {
    rc_memref_t* d = &c->direct[r[i].copy];
    uint32_t v;
    if (r[i].bytes)
      peek(m, r[i].address, r[i].bytes, &v);
    else
      v = rc_read_memory(d->address, d->value.size, live_read, NULL);
    values[i] = v;
    if (r[i].sync)
      rc_update_memref_value(&d->value, v);
  }
  if (stop_now())
    return -1;

  /* leaves, by size: compared as they are read while the targets match */
  if (!ram) {
    g = run_leaves_slow(l, c->leaf_count, g, m, &prev);
    if (stop_now())
      return -1;
  }
  else {
    /* a call per size, so each is built for its size */
#define LEAVES(index, size) \
    for (left = c->leaf_count[index]; left; left -= k, l += k) { \
      k = left < POLL_WORDS ? left : POLL_WORDS; \
      g = run_leaves(l, k, size, g, m, ram, &prev); \
      if (stop_now()) \
        return -1; \
    }
    LEAVES(0, 1)
    LEAVES(1, 2)
    LEAVES(2, 4)
#undef LEAVES
  }

  /* other steps: plain reads here, anything else in run_step */
  if (!ram) {
    for (; s < end; s++, out++) {
      if (!((end - s) & (POLL_WORDS - 1)) && stop_now())
        return -1;
      g = run_step(s, g, m, out);
    }
  }
  else {
    for (; s < end; s++, out++) {
      uint32_t a, v, p;
      if (!((end - s) & (POLL_WORDS - 1)) && stop_now())
        return -1;
      n = s->fast;
      if (!n) {
        g = run_step(s, g, m, out);
        continue;
      }
      p = values[s->parent];
      if (n & FAST_STATE) {
        /* the parent's delta or prior, as run_step reads it */
        const rc_memref_value_t* pv = c->vref[s->parent];
        p = ((s->flags & CF_PRIOR) || pv->changed) ? pv->prior : pv->value;
        n &= ~FAST_STATE;
      }
      a = (p & s->mask) + s->k;
      g->address = a;
      if (a <= MAIN_SIZE - n) {
        v = load(ram + a, n);
        g->length = n;
      }
      else {
        g->length = peek(m, a, n, &v);    /* DTCM, or nothing */
      }
      *(word_t*)g->bytes = v;
      *out = v;
      g++;
    }
  }

  /* the other steps' targets, if the leaves all matched */
  if (!prev)
    return 0;
  l = c->leaves + c->leaf_count[0] + c->leaf_count[1] + c->leaf_count[2];
  g = targets + (l - c->leaves);
  return memcmp(g, prev, (c->target_count - (l - c->leaves)) * sizeof(target_t)) == 0;
}

static void free_chain_copies(chains_t* c)
{
  free(c->direct);
  free(c->mods);
  free(c->roots);
  free(c->steps);
  free(c->values);
  free(c->vref);
  free(c->leaves);
  free(c->target_order);
  free(c->target_slot);
  memset(c, 0, sizeof(*c));
}

static void free_chains(void)
{
  free_chain_copies(&S.chains);
#ifdef DSC_CHECK_FAST_STEPS
  free_chain_copies(&S.ref);
#endif
  free(S.sync);
  free(S.sync_saved);
  S.sync = NULL;
  S.sync_saved = NULL;
  S.sync_count = 0;
  S.sync_dirty = 0;
}

/* List the chain copies whose rcheevos state run_chains advances (and, in
 * test builds, every reference copy check_snapshot advances). */
static int list_sync(void)
{
  const chains_t* c = &S.chains;
  uint32_t i, n = 0, total = c->root_count + c->step_count;
#ifdef DSC_CHECK_FAST_STEPS
  total += S.ref.direct_count + S.ref.mod_count;
#endif
  S.sync = (rc_memref_value_t**)malloc((total ? total : 1) * sizeof(*S.sync));
  S.sync_saved = (rc_memref_value_t*)malloc((total ? total : 1) * sizeof(*S.sync_saved));
  if (!S.sync || !S.sync_saved)
    return -1;
  for (i = 0; i < c->root_count; i++)
    if (c->roots[i].sync)
      S.sync[n++] = &c->direct[c->roots[i].copy].value;
  for (i = 0; i < c->step_count; i++)
    if (c->steps[i].flags & CF_SYNC)
      S.sync[n++] = &c->mods[c->steps[i].copy].memref.value;
#ifdef DSC_CHECK_FAST_STEPS
  for (i = 0; i < S.ref.direct_count; i++)
    S.sync[n++] = &S.ref.direct[i].value;
  for (i = 0; i < S.ref.mod_count; i++)
    S.sync[n++] = &S.ref.mods[i].memref.value;
#endif
  S.sync_count = n;
  return 0;
}

WAKE_INL void sync_save(void)
{
  uint32_t i;
  for (i = 0; i < S.sync_count; i++)
    S.sync_saved[i] = *S.sync[i];
  S.sync_dirty = 1;
}

/* Undo an uncommitted capture's chain state. */
WAKE_INL void sync_restore(void)
{
  uint32_t i;
  if (!S.sync_dirty)
    return;
  for (i = 0; i < S.sync_count; i++)
    *S.sync[i] = S.sync_saved[i];
  S.sync_dirty = 0;
}

/* A frame evaluated with the last snapshot's memory: as if every chain
 * copy were read again with the same value (rc_update_memref_value). In
 * the middle of a copy (a VBlank while dsc_capture let an IRQ run) the
 * committed values are the saved ones: the copy puts them back. */
static WAKE void sync_unchanged(void)
{
  uint32_t i;
  if (S.sync_dirty) {
    for (i = 0; i < S.sync_count; i++)
      S.sync_saved[i].changed = 0;
    return;
  }
  for (i = 0; i < S.sync_count; i++)
    S.sync[i]->changed = 0;
}

/* Index of a memref in rcheevos' lists of direct memrefs (or of modified
 * ones), in list order; -1 if it is in neither. */
static int32_t direct_index(const rc_memrefs_t* memrefs, const rc_memref_t* m)
{
  const rc_memref_list_t* l;
  uint32_t base = 0;
  for (l = &memrefs->memrefs; l; l = l->next) {
    if (m >= l->items && m < l->items + l->count)
      return (int32_t)(base + (uint32_t)(m - l->items));
    base += l->count;
  }
  return -1;
}

static int32_t mod_index(const rc_memrefs_t* memrefs, const rc_memref_t* m)
{
  const rc_modified_memref_list_t* l;
  const rc_modified_memref_t* p = (const rc_modified_memref_t*)m;
  uint32_t base = 0;
  for (l = &memrefs->modified_memrefs; l; l = l->next) {
    if (p >= l->items && p < l->items + l->count)
      return (int32_t)(base + (uint32_t)(p - l->items));
    base += l->count;
  }
  return -1;
}

/* Copy the steps of every pointer chain into c, operands pointing at the
 * copies; each array sized to what it holds. Returns 0, or -1 if out of
 * memory. */
static int copy_chains(chains_t* c, rc_memrefs_t* memrefs)
{
  rc_memref_list_t* ml;
  rc_modified_memref_list_t* mml;
  rc_modified_memref_t** mods = NULL;
  rc_memref_t** direct_src = NULL;  /* source of direct copy k */
  uint32_t* direct_of = NULL;       /* direct memref -> its copy + 1, or 0 */
  uint32_t* copy_of = NULL;         /* step -> its copy + 1, 0: not copied; 1 in the first pass: needed */
  uint32_t mod_total = 0, direct_total = 0, i, j, n;
  int result = -1;

  memset(c, 0, sizeof(*c));
  /* chain steps in order, parents first */
  for (mml = &memrefs->modified_memrefs; mml; mml = mml->next)
    mod_total += mml->count;
  if (!mod_total)
    return 0;
  for (ml = &memrefs->memrefs; ml; ml = ml->next)
    direct_total += ml->count;
  mods = (rc_modified_memref_t**)malloc(mod_total * sizeof(*mods));
  copy_of = (uint32_t*)calloc(mod_total, sizeof(*copy_of));
  direct_of = (uint32_t*)calloc(direct_total + 1, sizeof(*direct_of));
  direct_src = (rc_memref_t**)malloc(mod_total * 2 * sizeof(*direct_src));
  if (!mods || !copy_of || !direct_of || !direct_src)
    goto done;
  n = 0;
  for (mml = &memrefs->modified_memrefs; mml; mml = mml->next)
    for (i = 0; i < mml->count; i++)
      mods[n++] = &mml->items[i];

  /* a step is needed if it reads memory, or a needed step uses it; walk back
   * from the reads. Directs a needed step uses get a copy, numbered as found. */
  for (i = mod_total; i-- > 0;) {
    const rc_operand_t* ops[2];
    if (mods[i]->modifier_type == RC_OPERATOR_INDIRECT_READ)
      copy_of[i] = 1;
    if (!copy_of[i])
      continue;
    ops[0] = &mods[i]->parent;
    ops[1] = &mods[i]->modifier;
    for (j = 0; j < 2; j++) {
      const rc_operand_t* op = ops[j];
      int32_t k;
      if (!rc_operand_type_is_memref(op->type) || !op->value.memref)
        continue;
      if (op->value.memref->value.memref_type == RC_MEMREF_TYPE_MODIFIED_MEMREF) {
        k = mod_index(memrefs, op->value.memref);
        if (k >= 0 && (uint32_t)k < i)
          copy_of[k] = 1;
      }
      else if (op->value.memref->value.memref_type == RC_MEMREF_TYPE_MEMREF) {
        k = direct_index(memrefs, op->value.memref);
        if (k >= 0 && direct_of[k])
          continue;
        if (k < 0) {    /* not in the set's list: look among the copies */
          uint32_t x;
          for (x = 0; x < c->direct_count && direct_src[x] != op->value.memref; x++)
            ;
          if (x < c->direct_count)
            continue;
        }
        direct_src[c->direct_count++] = op->value.memref;
        if (k >= 0)
          direct_of[k] = c->direct_count;
      }
    }
  }

  /* which needed steps are copied: those whose operands are all copies; a
   * step that depends on something not copied (Remember/Recall) is left to
   * live reads */
  for (i = 0; i < mod_total; i++) {
    const rc_operand_t* ops[2];
    int ok = copy_of[i] != 0;
    copy_of[i] = 0;
    if (!ok)
      continue;
    ops[0] = &mods[i]->parent;
    ops[1] = &mods[i]->modifier;
    for (j = 0; j < 2 && ok; j++) {
      const rc_operand_t* op = ops[j];
      if (op->type == RC_OPERAND_RECALL) {
        ok = 0;
      }
      else if (rc_operand_type_is_memref(op->type) && op->value.memref) {
        int32_t k;
        if (op->value.memref->value.memref_type == RC_MEMREF_TYPE_MODIFIED_MEMREF) {
          k = mod_index(memrefs, op->value.memref);
          ok = k >= 0 && (uint32_t)k < i && copy_of[k];
        }
        else if (op->value.memref->value.memref_type == RC_MEMREF_TYPE_MEMREF) {
          k = direct_index(memrefs, op->value.memref);
          if (k < 0) {
            uint32_t x;
            for (x = 0; x < c->direct_count && direct_src[x] != op->value.memref; x++)
              ;
            ok = x < c->direct_count;
          }
          else {
            ok = direct_of[k] != 0;
          }
        }
        else {
          ok = 0;
        }
      }
    }
    if (ok)
      copy_of[i] = ++c->mod_count;
  }

  c->direct = (rc_memref_t*)malloc((c->direct_count ? c->direct_count : 1) * sizeof(rc_memref_t));
  c->mods = (rc_modified_memref_t*)malloc((c->mod_count ? c->mod_count : 1) * sizeof(rc_modified_memref_t));
  if (!c->direct || !c->mods)
    goto done;
  for (i = 0; i < c->direct_count; i++)
    c->direct[i] = *direct_src[i];

  /* copy them, operands pointing at the copies */
  for (i = 0; i < mod_total; i++) {
    rc_modified_memref_t* copy;
    rc_operand_t* ops[2];
    if (!copy_of[i])
      continue;
    copy = &c->mods[copy_of[i] - 1];
    *copy = *mods[i];
    ops[0] = &copy->parent;
    ops[1] = &copy->modifier;
    for (j = 0; j < 2; j++) {
      rc_operand_t* op = ops[j];
      int32_t k;
      if (!rc_operand_type_is_memref(op->type) || !op->value.memref)
        continue;
      if (op->value.memref->value.memref_type == RC_MEMREF_TYPE_MODIFIED_MEMREF) {
        k = mod_index(memrefs, op->value.memref);
        op->value.memref = &c->mods[copy_of[k] - 1].memref;
      }
      else {
        uint32_t x;
        k = direct_index(memrefs, op->value.memref);
        if (k >= 0) {
          x = direct_of[k] - 1;
        }
        else {
          for (x = 0; direct_src[x] != op->value.memref; x++)
            ;
        }
        op->value.memref = &c->direct[x];
      }
    }
    if (copy->modifier_type == RC_OPERATOR_INDIRECT_READ)
      c->target_count++;
  }
  result = 0;

done:
  free(mods);
  free(copy_of);
  free(direct_of);
  free(direct_src);
  if (result != 0)
    free_chain_copies(c);
  return result;
}

/* The value index of a copy an operand points at. */
static uint32_t chain_index(const chains_t* c, const rc_memref_t* m)
{
  if (m >= c->direct && m < c->direct + c->direct_count)
    return (uint32_t)(m - c->direct);
  return c->direct_count + (uint32_t)((const rc_modified_memref_t*)m - c->mods);
}

/* An operand run_chains reads itself: a copy's plain unsigned value (or its
 * delta or prior) with the operand's size applied as a mask, which is what
 * rc_evaluate_operand gives for it. */
static int fast_operand(const chains_t* c, const rc_operand_t* op, uint32_t* index, uint32_t* mask, uint8_t* flags)
{
  if ((op->type != RC_OPERAND_ADDRESS && op->type != RC_OPERAND_DELTA && op->type != RC_OPERAND_PRIOR) ||
      !op->value.memref || op->value.memref->value.type != RC_VALUE_TYPE_UNSIGNED)
    return 0;
  switch (op->size) {
    case RC_MEMSIZE_8_BITS: *mask = 0xFF; break;
    case RC_MEMSIZE_16_BITS: *mask = 0xFFFF; break;
    case RC_MEMSIZE_24_BITS: *mask = 0xFFFFFF; break;
    case RC_MEMSIZE_32_BITS: *mask = 0xFFFFFFFF; break;
    default: return 0;
  }
  *index = chain_index(c, op->value.memref);
  *flags = op->type == RC_OPERAND_DELTA ? CF_DELTA : op->type == RC_OPERAND_PRIOR ? CF_PRIOR : 0;
  return 1;
}

/* How a step with a constant modifier is computed on plain unsigned values,
 * or CS_GENERIC. Matches rc_get_modified_memref_value: e.g. SUB negates the
 * modifier to signed and adds it back as unsigned, which is u32 subtraction.
 * (AddSource/SubSource steps combine a condition's value; they never feed a
 * pointer, so chain copies don't have them.) */
static uint8_t fast_op(const rc_modified_memref_t* m)
{
  if (m->modifier.type != RC_OPERAND_CONST || m->memref.value.type != RC_VALUE_TYPE_UNSIGNED)
    return CS_GENERIC;
  switch (m->modifier_type) {
    case RC_OPERATOR_INDIRECT_READ:
      return m->memref.value.size == RC_MEMSIZE_8_BITS ? CS_READ1 :
             m->memref.value.size == RC_MEMSIZE_16_BITS ? CS_READ2 :
             m->memref.value.size == RC_MEMSIZE_32_BITS ? CS_READ4 : CS_GENERIC;
    case RC_OPERATOR_AND: return CS_AND;
    case RC_OPERATOR_XOR: return CS_XOR;
    case RC_OPERATOR_ADD: return CS_ADD;
    case RC_OPERATOR_SUB: return CS_SUB;
    case RC_OPERATOR_MULT: return CS_MUL;
    default: return CS_GENERIC;
  }
}

static int plain_root(const rc_memref_t* m, int sync)
{
  return m->value.size == RC_MEMSIZE_32_BITS && !(m->address & 3) && m->address < MAIN_SIZE && !sync;
}

/* After compile_chains: keep only what run_chains uses. The steps it runs
 * through rcheevos (CS_GENERIC), and those whose rcheevos state is kept
 * (CF_SYNC: read as delta or prior, or by rcheevos' code), keep their copy;
 * the rest live in steps[] and values[] alone. The other arrays shrink to
 * their counts. Returns 0, or -1 if out of memory. */
static int compact_chains(chains_t* c, uint32_t total)
{
  rc_modified_memref_t* kept = NULL;
  uint32_t* index = NULL;    /* old copy -> kept copy + 1, or 0 */
  uint32_t i, n = 0, values = c->root_count + c->step_count, leaves;
  void* p;

  for (i = 0; i < c->step_count; i++)
    if (c->steps[i].op == CS_GENERIC || (c->steps[i].flags & CF_SYNC))
      n++;
  index = (uint32_t*)calloc(c->mod_count + 1, sizeof(*index));
  if (n)
    kept = (rc_modified_memref_t*)malloc(n * sizeof(*kept));
  if (!index || (n && !kept)) {
    free(index);
    free(kept);
    return -1;
  }
  n = 0;
  for (i = 0; i < c->step_count; i++) {
    cstep_t* s = &c->steps[i];
    if (s->op == CS_GENERIC || (s->flags & CF_SYNC)) {
      kept[n] = c->mods[s->copy];
      index[s->copy] = ++n;
      s->copy = (uint16_t)(n - 1);
    }
    else {
      s->copy = 0;    /* not used */
    }
  }
  /* Operands of a copy rcheevos' code computes point at kept copies (it
   * syncs what it reads); the other kept copies only hold their value. */
  for (i = 0; i < c->step_count; i++) {
    const cstep_t* s = &c->steps[i];
    rc_operand_t* ops[2];
    uint32_t j;
    if (s->op != CS_GENERIC && !(s->flags & CF_SYNC))
      continue;
    ops[0] = &kept[s->copy].parent;
    ops[1] = &kept[s->copy].modifier;
    for (j = 0; j < 2; j++) {
      const rc_modified_memref_t* m = (const rc_modified_memref_t*)ops[j]->value.memref;
      if (!rc_operand_type_is_memref(ops[j]->type) || !m || m < c->mods || m >= c->mods + c->mod_count)
        continue;
      if (index[m - c->mods])
        ops[j]->value.memref = &kept[index[m - c->mods] - 1].memref;
      else if (s->op != CS_GENERIC)
        ops[j]->value.memref = NULL;    /* never read */
      else {
        free(index);
        free(kept);
        return -1;    /* can't happen: compile_chains syncs what rcheevos' code reads */
      }
    }
  }
  for (i = 0; i < total + 1 && i < values; i++) {
    const rc_modified_memref_t* m = (const rc_modified_memref_t*)c->vref[i];
    if ((const void*)m >= (const void*)c->mods && (const void*)m < (const void*)(c->mods + c->mod_count))
      c->vref[i] = index[m - c->mods] ? &kept[index[m - c->mods] - 1].memref.value : NULL;
  }
  free(index);
  free(c->mods);
  c->mods = kept;
  c->mod_count = n;

  /* shrinking can't fail; a NULL from realloc keeps the old block */
  leaves = c->leaf_count[0] + c->leaf_count[1] + c->leaf_count[2];
  if ((p = realloc(c->values, (values + 1) * sizeof(*c->values))) != NULL)
    c->values = (uint32_t*)p;
  if ((p = realloc(c->vref, (values + 1) * sizeof(*c->vref))) != NULL)
    c->vref = (rc_memref_value_t**)p;
  if ((p = realloc(c->steps, (c->step_count + 1) * sizeof(*c->steps))) != NULL)
    c->steps = (cstep_t*)p;
  if ((p = realloc(c->leaves, (leaves + 1) * sizeof(*c->leaves))) != NULL)
    c->leaves = (leaf_t*)p;
  if ((p = realloc(c->roots, (c->root_count + 1) * sizeof(*c->roots))) != NULL)
    c->roots = (root_t*)p;
  return 0;
}

/* Turn the copies into the program run_chains runs.
 *  - An AND whose result only feeds later steps' plain reads of it is folded
 *    into their parent mask (the "pointer & 0x1FFFFFF" of most DS chains).
 *  - A plain read of an aligned 32-bit pointer in main RAM that nothing else
 *    uses becomes a leaf, run in one go with its pointer.
 *  - Only copies something reads as delta or prior, or through rcheevos'
 *    code, keep their rcheevos state; the others live in values[] alone.
 * Values are numbered roots first (plain ones first), then steps in order.
 * Returns 0, or -1 if out of memory or too big. */
static int compile_chains(chains_t* c)
{
  uint32_t d = c->direct_count, total = d + c->mod_count, i, k, pass, t;
  uint32_t* parent = NULL;
  uint32_t* mask = NULL;
  uint32_t* number = NULL;     /* index (directs, then mods) -> value index */
  uint16_t* target = NULL;     /* mod -> its read's place among the chain reads */
  uint8_t* op = NULL;
  uint8_t* flags = NULL;
  uint8_t* sync = NULL;
  uint8_t* fold = NULL;
  uint8_t* used = NULL;        /* a step that isn't a leaf reads it */
  uint8_t* leaf = NULL;        /* 0, or the leaf's size */
  int result = -1;

  if (total > 0xFFFF)
    return -1;
  c->roots = (root_t*)calloc(d + 1, sizeof(root_t));
  c->leaves = (leaf_t*)calloc(c->mod_count + 1, sizeof(leaf_t));
  c->steps = (cstep_t*)calloc(c->mod_count + 1, sizeof(cstep_t));
  c->values = (uint32_t*)calloc(total + 1, sizeof(uint32_t));
  c->vref = (rc_memref_value_t**)calloc(total + 1, sizeof(rc_memref_value_t*));
  c->target_order = (uint16_t*)calloc(c->target_count + 1, sizeof(uint16_t));
  c->target_slot = (uint16_t*)calloc(c->target_count + 1, sizeof(uint16_t));
  parent = (uint32_t*)calloc(c->mod_count + 1, sizeof(uint32_t));
  mask = (uint32_t*)calloc(c->mod_count + 1, sizeof(uint32_t));
  number = (uint32_t*)calloc(total + 1, sizeof(uint32_t));
  target = (uint16_t*)calloc(c->mod_count + 1, sizeof(uint16_t));
  op = (uint8_t*)calloc(c->mod_count + 1, 1);
  flags = (uint8_t*)calloc(c->mod_count + 1, 1);
  fold = (uint8_t*)calloc(c->mod_count + 1, 1);
  leaf = (uint8_t*)calloc(c->mod_count + 1, 1);
  sync = (uint8_t*)calloc(total + 1, 1);
  used = (uint8_t*)calloc(total + 1, 1);
  if (!c->roots || !c->leaves || !c->steps || !c->values || !c->vref || !c->target_order || !c->target_slot || !parent || !mask ||
      !number || !target || !op || !flags || !fold || !leaf || !sync || !used)
    goto done;

  /* what each step needs, and which copies must keep their rcheevos state */
  for (i = 0, t = 0; i < c->mod_count; i++) {
    const rc_modified_memref_t* m = &c->mods[i];
    if (m->modifier_type == RC_OPERATOR_INDIRECT_READ)
      target[i] = (uint16_t)t++;
    op[i] = fast_op(m);
    if (op[i] != CS_GENERIC && !fast_operand(c, &m->parent, &parent[i], &mask[i], &flags[i]))
      op[i] = CS_GENERIC;
    if (op[i] == CS_GENERIC) {
      if (rc_operand_type_is_memref(m->parent.type) && m->parent.value.memref)
        sync[chain_index(c, m->parent.value.memref)] = 1;
      if (rc_operand_type_is_memref(m->modifier.type) && m->modifier.value.memref)
        sync[chain_index(c, m->modifier.value.memref)] = 1;
    }
    else if (flags[i]) {
      sync[parent[i]] = 1;
    }
  }

  /* fold ANDs: nothing needs their own state, their parent comes first, and
   * every step using them comes after them */
  for (i = 0; i < c->mod_count; i++)
    fold[i] = op[i] == CS_AND && !sync[d + i] && parent[i] < d + i;
  for (i = 0; i < c->mod_count; i++)
    if (op[i] != CS_GENERIC && parent[i] >= d + i)
      fold[parent[i] - d] = 0;
  for (i = 0; i < c->mod_count; i++) {
    if (op[i] != CS_GENERIC && parent[i] >= d && fold[parent[i] - d]) {
      uint32_t a = parent[i] - d;    /* already resolved: a < i */
      mask[i] &= mask[a] & c->mods[a].modifier.value.num;
      flags[i] = flags[a];
      parent[i] = parent[a];
    }
  }

  /* leaves: plain reads of a plain root that nothing reads */
  for (i = 0; i < c->mod_count; i++)
    if (!fold[i] && op[i] != CS_GENERIC)
      used[parent[i]] = 1;
  for (i = 0; i < c->mod_count; i++)
    if (!fold[i] && op[i] <= CS_READ4 && !flags[i] && !sync[d + i] && !used[d + i] &&
        parent[i] < d && plain_root(&c->direct[parent[i]], sync[parent[i]]))
      leaf[i] = op[i];
  memset(used, 0, total);
  for (i = 0; i < c->mod_count; i++)
    if (!fold[i] && !leaf[i] && op[i] != CS_GENERIC)
      used[parent[i]] = 1;

  /* roots a step reads, plain ones first */
  for (k = 0, pass = 0; pass < 2; pass++) {
    for (i = 0; i < d; i++) {
      root_t* r;
      if (!used[i] && !sync[i])
        continue;
      if (plain_root(&c->direct[i], sync[i]) != !pass)
        continue;
      number[i] = k;
      r = &c->roots[k++];
      r->address = c->direct[i].address;
      r->copy = (uint16_t)i;
      r->bytes = c->direct[i].value.size == RC_MEMSIZE_8_BITS ? 1 : c->direct[i].value.size == RC_MEMSIZE_16_BITS ? 2 :
                 c->direct[i].value.size == RC_MEMSIZE_32_BITS ? 4 : 0;
      r->sync = sync[i];
      c->values[number[i]] = c->direct[i].value.value;
      c->vref[number[i]] = &c->direct[i].value;
    }
    if (!pass)
      c->plain_roots = k;
  }
  c->root_count = k;

  /* leaves by size, then the other steps in order; targets in that order */
  t = 0;
  for (pass = 0; pass < 3; pass++) {
    for (i = 0; i < c->mod_count; i++) {
      leaf_t* l;
      if (leaf[i] != (1u << pass))
        continue;
      l = &c->leaves[c->leaf_count[0] + c->leaf_count[1] + c->leaf_count[2]];
      c->leaf_count[pass]++;
      l->root = c->direct[parent[i]].address;
      l->mask = mask[i];
      l->k = c->mods[i].modifier.value.num;
      c->target_order[t++] = target[i];
    }
  }
  for (i = 0; i < c->mod_count; i++)
    if (!fold[i] && !leaf[i])
      number[d + i] = k++;
  for (i = 0; i < c->mod_count; i++) {
    const rc_modified_memref_t* m = &c->mods[i];
    cstep_t* s;
    if (fold[i] || leaf[i])
      continue;
    s = &c->steps[c->step_count++];
    s->op = op[i];
    s->copy = (uint16_t)i;
    if (op[i] != CS_GENERIC) {
      s->k = m->modifier.value.num;
      s->mask = mask[i];
      s->parent = (uint16_t)number[parent[i]];
      s->flags = flags[i];
    }
    else if (m->modifier_type == RC_OPERATOR_INDIRECT_READ) {
      s->flags = CF_READS;
    }
    if (sync[d + i] || op[i] == CS_GENERIC)
      s->flags |= CF_SYNC;
    /* plain reads, also of a parent's delta or prior, run in run_chains */
    s->fast = (op[i] <= CS_READ4 && !(s->flags & ~(CF_DELTA | CF_PRIOR))) ?
              (uint8_t)(op[i] | (s->flags ? FAST_STATE : 0)) : 0;
    if (m->modifier_type == RC_OPERATOR_INDIRECT_READ)
      c->target_order[t++] = target[i];
    c->values[number[d + i]] = m->memref.value.value;
    c->vref[number[d + i]] = (rc_memref_value_t*)&m->memref.value;
  }
  for (i = 0; i < t; i++)
    c->target_slot[c->target_order[i]] = (uint16_t)i;
  result = t == c->target_count ? 0 : -1;
  if (result == 0)
    result = compact_chains(c, d + c->mod_count);

done:
  free(parent);
  free(mask);
  free(number);
  free(target);
  free(op);
  free(flags);
  free(fold);
  free(leaf);
  free(sync);
  free(used);
  return result;
}

/* Fill the address table with every address the set reads directly, and copy
 * the steps of every pointer chain. Runs once the game is loaded, before any
 * snapshot is taken. */
static void prepare_snapshots(void)
{
  rc_memrefs_t* memrefs = S.client->game->runtime.memrefs;
  rc_memref_list_t* ml;
  uint32_t i, j, n = 0, bytes_total = 0;

  Q.stage = 0;
#ifdef DSC_CHANGED_RANGES
  S.prev_valid = 0;
#endif
  free_chains();
  free(S.seq_address);
  free(S.seq_bytes);
  S.seq_address = NULL;
  S.seq_bytes = NULL;
  S.seq_count = 0;
  if (!memrefs)
    return;

  /* the direct reads, in rcheevos' order (memrefs it reads, of a known size) */
  for (ml = &memrefs->memrefs; ml; ml = ml->next) {
    for (i = 0; i < ml->count; i++) {
      const rc_memref_t* m = &ml->items[i];
      uint32_t bytes = m->value.type == RC_VALUE_TYPE_NONE ? 0 : rc_memref_bytes(m->value.size);
      n += bytes != 0;
      bytes_total += bytes;
    }
  }
  S.seq_address = (uint32_t*)malloc((n ? n : 1) * sizeof(uint32_t));
  S.seq_bytes = (uint8_t*)malloc(n ? n : 1);
  if (bytes_total > S.config.max_addresses - S.current->count)
    bytes_total = S.config.max_addresses - S.current->count;
  if (S.pending_new_size < S.pending_new_count + bytes_total) {
    uint32_t* p = (uint32_t*)realloc(S.pending_new, (S.pending_new_count + bytes_total + 1) * sizeof(uint32_t));
    if (p) {
      S.pending_new = p;
      S.pending_new_size = S.pending_new_count + bytes_total + 1;
    }
  }
  if (S.seq_address && S.seq_bytes) {
    for (ml = &memrefs->memrefs; ml; ml = ml->next) {
      for (i = 0; i < ml->count; i++) {
        const rc_memref_t* m = &ml->items[i];
        uint32_t bytes = m->value.type == RC_VALUE_TYPE_NONE ? 0 : rc_memref_bytes(m->value.size);
        if (bytes) {
          S.seq_address[S.seq_count] = m->address;
          S.seq_bytes[S.seq_count++] = (uint8_t)bytes;
        }
      }
    }
  }

  /* every byte read directly, merged into the table (rcheevos has already
   * disabled achievements whose addresses can't be read), as many as fit */
  for (ml = &memrefs->memrefs; ml; ml = ml->next) {
    for (i = 0; i < ml->count; i++) {
      const rc_memref_t* m = &ml->items[i];
      uint32_t bytes = m->value.type == RC_VALUE_TYPE_NONE ? 0 : rc_memref_bytes(m->value.size);
      for (j = 0; j < bytes && S.current->count + S.pending_new_count < S.config.max_addresses &&
                  S.pending_new_count < S.pending_new_size; j++)
        S.pending_new[S.pending_new_count++] = m->address + j;
    }
  }
  publish_pending_addresses();

  S.target_cursor = 0;
  if (copy_chains(&S.chains, memrefs) != 0 || compile_chains(&S.chains) != 0)
    free_chains();    /* chains then fall back to live reads */
#ifdef DSC_CHECK_FAST_STEPS
  else if (copy_chains(&S.ref, memrefs) != 0 || list_sync() != 0)
    abort();
#else
  else if (list_sync() != 0)
    free_chains();
#endif
}

/* ---- journal, notices, achievement list ---- */

/* Copy UTF-8 text into a fixed field, cut on a character boundary. */
static void copy_text(char* dst, const char* src, size_t n)
{
  size_t len = src ? strlen(src) : 0;
  if (len >= n) {
    len = n - 1;
    while (len > 0 && ((uint8_t)src[len] & 0xC0) == 0x80)
      len--;
  }
  if (len)
    memcpy(dst, src, len);
  dst[len] = '\0';
}

static uint32_t unix_now(void)
{
  return S.platform.unix_time ? S.platform.unix_time() : (uint32_t)time(NULL);
}

/* The frame the event belongs to: the queued one being evaluated, if any. */
static uint32_t current_frame(void)
{
  return S.evaluating ? S.eval_frame : S.frame_counter;
}

/* The unix time when the frame being evaluated was captured: an unlock is
 * journalled at the time it happened in the game, however long the frame
 * waited in the queue. */
static uint32_t unix_captured(void)
{
  uint32_t ago = 0;
  if (S.evaluating)
    ago = S.frame_counter + (Q.frames - Q.taken) - S.eval_frame;
  if (S.platform.unix_time_at)
    return S.platform.unix_time_at(ago);
  return unix_now() - (ago + 30) / 60;
}

/* An ordering point of a journal update: what's written so far reaches RAM
 * before anything written after (the ARM7 reads the journal from RAM). */
static void journal_sync(const dscm_journal_t* j)
{
  PUBLISH_BARRIER();
  if (S.platform.journal_sync)
    S.platform.journal_sync(j, sizeof(dscm_journal_t));
}

/* Header fields, once the game is known; magic last. */
static void journal_start(uint32_t game_id)
{
  dscm_journal_t* j = S.platform.journal;
  if (!j)
    return;
  j->size = sizeof(dscm_journal_t);
  j->game_id = game_id;
  j->time_base = S.platform.clock_is_utc ? DSCM_JOURNAL_TIME_UTC : DSCM_JOURNAL_TIME_CONSOLE;
  j->clock = (uint8_t)S.platform.clock;
  copy_text(j->username, S.config.username, sizeof(j->username));
  copy_text(j->game_hash, S.game_hash, sizeof(j->game_hash));
  journal_sync(j);
  j->magic = DSCM_JOURNAL_MAGIC;
  journal_sync(j);
}

/* The oldest entry with the given sent flag, or DSCM_JOURNAL_MAX. */
static uint32_t journal_oldest(const dscm_journal_t* j, int sent)
{
  uint32_t i, pick = DSCM_JOURNAL_MAX;
  for (i = 0; i < j->count; i++)
    if (j->e[i].sent == sent && (pick == DSCM_JOURNAL_MAX || j->e[i].unix_time < j->e[pick].unix_time))
      pick = i;
  return pick;
}

/* An entry is written with kind 0, so a reader skips it until it is complete. */
static void journal_add(uint8_t kind, uint32_t id, int32_t score)
{
  dscm_journal_t* j = S.platform.journal;
  dscm_journal_entry_t* e;
  uint32_t pick;
  if (!j || S.config.spectator)
    return;
  if (j->count < DSCM_JOURNAL_MAX) {
    pick = j->count;
  }
  else {
    /* full: reuse the oldest answered entry, or lose the oldest unsent one */
    pick = journal_oldest(j, 1);
    if (pick == DSCM_JOURNAL_MAX) {
      pick = journal_oldest(j, 0);
      S.stats.journal_lost++;
    }
  }
  e = &j->e[pick];
  e->kind = 0;
  journal_sync(j);
  e->hardcore = rc_client_get_hardcore_enabled(S.client) ? 1 : 0;
  e->sent = 0;
  e->reserved = 0;
  e->id = id;
  e->score = score;
  e->unix_time = unix_captured();
  journal_sync(j);
  e->kind = kind;
  journal_sync(j);
  if (pick == j->count) {
    j->count = pick + 1;
    journal_sync(j);
  }
}

/* Leaderboard entries also match on score: two for one board can be in flight. */
static void journal_answered(uint8_t kind, uint32_t id, int32_t score)
{
  dscm_journal_t* j = S.platform.journal;
  uint32_t i, pick = DSCM_JOURNAL_MAX;
  if (!j)
    return;
  for (i = 0; i < j->count; i++)
    if (j->e[i].kind == kind && j->e[i].id == id && !j->e[i].sent &&
        (kind != DSCM_JOURNAL_LEADERBOARD || j->e[i].score == score) &&
        (pick == DSCM_JOURNAL_MAX || j->e[i].unix_time < j->e[pick].unix_time))
      pick = i;
  if (pick < DSCM_JOURNAL_MAX) {
    j->e[pick].sent = 1;
    journal_sync(j);
  }
}

/* Number after "key=" in form data, 0 if missing. */
static uint32_t post_param(const char* post, const char* key)
{
  size_t n = strlen(key);
  const char* p = post;
  while (p && *p) {
    if (strncmp(p, key, n) == 0 && p[n] == '=')
      return (uint32_t)strtoul(p + n + 1, NULL, 10);
    p = strchr(p, '&');
    if (p)
      p++;
  }
  return 0;
}

static void request_journal_kind(http_pending_t* p, const char* post)
{
  if (!post)
    return;
  if (strncmp(post, "r=awardachievement&", 19) == 0) {
    p->journal_kind = DSCM_JOURNAL_AWARD;
    p->journal_id = post_param(post, "a");
  }
  else if (strncmp(post, "r=submitlbentry&", 16) == 0) {
    p->journal_kind = DSCM_JOURNAL_LEADERBOARD;
    p->journal_id = post_param(post, "i");
    p->journal_score = (int32_t)post_param(post, "s");
  }
}

/* 1 if the server answered for good, as rc_client decides: accepted (or
 * "already unlocked", which comes as a success), or refused with an error
 * it won't retry. */
static int answered(int result, const rc_api_response_t* r, const rc_api_server_response_t* resp)
{
  if (result == RC_OK && r->succeeded)
    return 1;
  return r->error_message && !rc_client_should_retry(resp);
}

static int request_answered(uint8_t kind, const rc_api_server_response_t* resp)
{
  int result, ok;
  if (kind == DSCM_JOURNAL_AWARD) {
    rc_api_award_achievement_response_t a;
    result = rc_api_process_award_achievement_server_response(&a, resp);
    ok = answered(result, &a.response, resp);
    rc_api_destroy_award_achievement_response(&a);
  }
  else {
    rc_api_submit_lboard_entry_response_t l;
    result = rc_api_process_submit_lboard_entry_server_response(&l, resp);
    ok = answered(result, &l.response, resp);
    rc_api_destroy_submit_lboard_entry_response(&l);
  }
  return ok;
}

/* Waiting notices into the ring, oldest first, at most DSC_NOTICES_PER_FRAME
 * per platform frame: the ring holds 16, so a reader that looks at least once
 * a frame misses none (the ring has no read index to wait on). Each slot is
 * complete before head moves to it. */
static inline __attribute__((always_inline)) void notices_send(void)
{
  dscm_notices_t* n = S.platform.notices;
  uint32_t frame;
  if (!n || !S.nq)
    return;
  frame = S.platform.frame ? S.platform.frame() : S.frame_counter;
  if (frame != S.nq_frame) {
    S.nq_frame = frame;
    S.nq_sent = 0;
  }
  while (S.nq_tail != S.nq_head && S.nq_sent < DSC_NOTICES_PER_FRAME) {
    uint32_t seq = n->head + 1;
    dscm_notice_t* x = &n->ring[seq % DSCM_NOTICE_RING];
    *x = S.nq[S.nq_tail % NOTICE_QUEUE];
    x->seq = seq;
    PUBLISH_BARRIER();
    n->head = seq;
    S.nq_tail++;
    S.nq_sent++;
  }
  S.stats.notices_waiting = S.nq_head - S.nq_tail;
}

/* One-shot notice, queued for the ring (notices_send). Evaluation pauses
 * while the queue is nearly full (NOTICE_HEADROOM), so only a single frame
 * raising more than that can find it full: then a newer progress update
 * replaces a waiting one for the same achievement, else it is counted lost. */
static void notice(uint16_t kind, uint32_t id, int32_t value, const char* text, const char* detail)
{
  dscm_notices_t* n = S.platform.notices;
  dscm_notice_t* x = NULL;
  if (!n)
    return;
  if (!S.nq) {
    /* no queue (out of memory at init): straight into the ring */
    uint32_t seq = n->head + 1;
    x = &n->ring[seq % DSCM_NOTICE_RING];
    x->seq = seq;
  }
  else if (S.nq_head - S.nq_tail < NOTICE_QUEUE) {
    x = &S.nq[S.nq_head % NOTICE_QUEUE];
  }
  else if (kind == DSCM_NOTICE_PROGRESS) {
    uint32_t i;
    for (i = S.nq_tail; i != S.nq_head && !x; i++)
      if (S.nq[i % NOTICE_QUEUE].kind == DSCM_NOTICE_PROGRESS && S.nq[i % NOTICE_QUEUE].id == id)
        x = &S.nq[i % NOTICE_QUEUE];
  }
  if (!x) {
    S.stats.notices_lost++;
    return;
  }
  x->kind = kind;
  x->reserved = 0;
  x->id = id;
  x->value = value;
  x->frame = S.platform.frame ? S.platform.frame() : current_frame();
  copy_text(x->text, text, sizeof(x->text));
  copy_text(x->detail, detail, sizeof(x->detail));
  if (!S.nq) {
    PUBLISH_BARRIER();
    n->head = x->seq;
    return;
  }
  if (x == &S.nq[S.nq_head % NOTICE_QUEUE] && S.nq_head - S.nq_tail < NOTICE_QUEUE)
    S.nq_head++;
  notices_send();
}

/* Show or update an indicator. More than DSCM_INDICATOR_MAX at once are not shown. */
static void indicator_set(dscm_indicator_t* list, uint32_t id, const char* text)
{
  dscm_notices_t* n = S.platform.notices;
  int i, free_slot = -1;
  if (!n || !id)
    return;
  for (i = 0; i < DSCM_INDICATOR_MAX; i++) {
    if (list[i].id == id) {
      copy_text(list[i].text, text, sizeof(list[i].text));
      n->indicators_generation++;
      return;
    }
    if (!list[i].id && free_slot < 0)
      free_slot = i;
  }
  if (free_slot < 0)
    return;
  copy_text(list[free_slot].text, text, sizeof(list[free_slot].text));
  PUBLISH_BARRIER();
  list[free_slot].id = id;
  n->indicators_generation++;
}

static void indicator_clear(dscm_indicator_t* list, uint32_t id)
{
  dscm_notices_t* n = S.platform.notices;
  int i;
  if (!n)
    return;
  for (i = 0; i < DSCM_INDICATOR_MAX; i++)
    if (list[i].id == id)
      list[i].id = 0;
  n->indicators_generation++;
}

/* Owner: publish the core set's progress for dsc_get_progress (which the
 * ARM9 may call while the ARM7 owns the rc_client). */
COLD static void progress_publish(void)
{
  rc_client_user_game_summary_t summary;
  uint32_t i = (S.progress_index + 1) & 1;
  dsc_progress_t* p = &S.progress[i];
  memset(p, 0, sizeof(*p));
  rc_client_get_user_game_summary(S.client, &summary);
  p->achievements = summary.num_promoted_achievements;
  p->unlocked = summary.num_unlocked_achievements;
  p->points = summary.points_available;
  p->points_unlocked = summary.points_unlocked;
  p->hardcore_refused = S.hardcore_refused;
  PUBLISH_BARRIER();
  S.progress_index = i;
  S.progress_stale = 0;
}

static void list_free(void)
{
  if (S.list_published && S.platform.publish_list)
    S.platform.publish_list(NULL);
  free(S.list_buf[0]);
  free(S.list_buf[1]);
  S.list_buf[0] = S.list_buf[1] = S.list_published = NULL;
}

/* Two buffers big enough for the core set, allocated once at load. */
static void list_alloc(void)
{
  const rc_client_subset_info_t* core = S.client->game ? S.client->game->subsets : NULL;
  size_t size;
  list_free();
  if (!S.platform.publish_list)
    return;
  S.list_max = core ? core->public_.num_achievements : 0;
  size = sizeof(dscm_list_t) + S.list_max * sizeof(dscm_list_entry_t);
  S.list_buf[0] = (dscm_list_t*)malloc(size);
  S.list_buf[1] = (dscm_list_t*)malloc(size);
  if (!S.list_buf[0] || !S.list_buf[1]) {
    dsc_log("no memory for the achievement list");
    list_free();
  }
  S.list_stale = 1;
}

static uint8_t list_state(const rc_client_achievement_info_t* a, int hardcore)
{
  if (hardcore && (a->public_.unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE))
    return DSCM_ACH_UNLOCKED;
  if (!hardcore && a->public_.unlocked)
    return DSCM_ACH_UNLOCKED;
  if (a->public_.unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_CASUAL)
    return DSCM_ACH_UNLOCKED_SOFTCORE;
  if (a->public_.state == RC_CLIENT_ACHIEVEMENT_STATE_DISABLED || a->public_.bucket == RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED)
    return DSCM_ACH_DISABLED;
  return DSCM_ACH_LOCKED;
}

/* Build the core set into the buffer readers aren't using, then publish it. */
static void list_build(void)
{
  const rc_client_subset_info_t* core = S.client->game ? S.client->game->subsets : NULL;
  rc_client_achievement_list_t* refresh;
  dscm_list_t* out;
  int hardcore = rc_client_get_hardcore_enabled(S.client);
  uint32_t i, n = 0;

  if (!S.list_buf[0] || !core)
    return;
  /* rc_client's list refreshes each achievement's bucket and measured progress */
  refresh = rc_client_create_achievement_list(S.client, RC_CLIENT_ACHIEVEMENT_CATEGORY_PROMOTED,
    RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
  if (!refresh)
    return;
  out = S.list_buf[S.list_published == S.list_buf[0] ? 1 : 0];

  /* the set's own order is its display order */
  for (i = 0; i < core->public_.num_achievements && n < S.list_max; i++) {
    const rc_client_achievement_info_t* a = &core->achievements[i];
    dscm_list_entry_t* e;
    if (a->public_.id >= WARNING_ID || a->public_.category != RC_CLIENT_ACHIEVEMENT_CATEGORY_PROMOTED)
      continue;
    e = &out->e[n++];
    memset(e, 0, sizeof(*e));
    e->id = a->public_.id;
    e->points = (uint16_t)a->public_.points;
    e->state = list_state(a, hardcore);
    if (e->state == DSCM_ACH_UNLOCKED)
      e->unlock_time = (uint32_t)(hardcore ? a->unlock_time_hardcore : a->public_.unlock_time);
    else if (e->state == DSCM_ACH_UNLOCKED_SOFTCORE)
      e->unlock_time = (uint32_t)a->unlock_time_casual;
    if (a->public_.measured_progress[0]) {
      e->flags |= DSCM_ACH_FLAG_MEASURED;
      copy_text(e->measured, a->public_.measured_progress, sizeof(e->measured));
    }
    if (a->public_.bucket == RC_CLIENT_ACHIEVEMENT_BUCKET_ACTIVE_CHALLENGE)
      e->flags |= DSCM_ACH_FLAG_CHALLENGE;
    if (a->public_.type == RC_CLIENT_ACHIEVEMENT_TYPE_MISSABLE)
      e->flags |= DSCM_ACH_FLAG_MISSABLE;
    else if (a->public_.type == RC_CLIENT_ACHIEVEMENT_TYPE_PROGRESSION)
      e->flags |= DSCM_ACH_FLAG_PROGRESSION;
    else if (a->public_.type == RC_CLIENT_ACHIEVEMENT_TYPE_WIN)
      e->flags |= DSCM_ACH_FLAG_WIN;
    copy_text(e->title, a->public_.title, sizeof(e->title));
    copy_text(e->description, a->public_.description, sizeof(e->description));
  }
  rc_client_destroy_achievement_list(refresh);

  out->magic = DSCM_LIST_MAGIC;
  out->generation = ++S.list_generation;
  out->count = n;
  out->entry_size = sizeof(dscm_list_entry_t);
  S.list_published = out;
  S.list_frame = S.frame_counter;
  S.stats.list_builds++;
  S.platform.publish_list(out);
}

/* Idle time: rebuild a stale list, at most once every LIST_MIN_FRAMES frames. */
static void list_update(void)
{
  if (!S.active || !S.list_stale || !S.list_buf[0])
    return;
  if (S.list_published && S.frame_counter - S.list_frame < LIST_MIN_FRAMES)
    return;
  S.list_stale = 0;
  list_build();
}

/* ---- rcheevos callbacks ---- */

static HOT uint32_t RC_CCONV read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client)
{
  slot_t* slot = S.evaluating;
  const table_t* t;
  uint32_t i, j, lo, failed;
  (void)client;

  if (!slot)
    return S.platform.read_memory(address, buffer, num_bytes);

  if (!num_bytes)
    return 0;
  t = slot->table;

  /* The set's direct reads come first, in the same order every frame:
   * where each lands was worked out with the table (seq_of). A memref
   * rcheevos stopped reading is skipped. */
  if (S.seq_next < S.seq_count && t->seq) {
    uint32_t k = S.seq_next, end = k + SEQ_LOOKAHEAD < S.seq_count ? k + SEQ_LOOKAHEAD : S.seq_count;
    for (; k < end; k++) {
      uint32_t w;
      if (S.seq_address[k] != address || S.seq_bytes[k] != num_bytes)
        continue;
      S.seq_next = k + 1;
      w = t->seq[k];
      if (w < SEQ_LIVE) {
        const uint8_t* p = slot->data + w;
        buffer[0] = p[0];
        for (i = 1; i < num_bytes; i++)
          buffer[i] = p[i];
        return num_bytes;
      }
      if (w == SEQ_LIVE) {
        /* the table is full: read live, as the general path would */
#ifdef DSC_CHANGED_RANGES
        S.live_read = 1;
#endif
        if (S.platform.read_memory(address, buffer, num_bytes) != num_bytes) {
          memset(buffer, 0, num_bytes);
          return 0;
        }
        S.stats.live_reads += num_bytes;
#ifdef DSC_CHANGED_RANGES
        S.live_read = 1;
#endif
        return num_bytes;
      }
      break;
    }
  }

  /* Then the chain reads: most likely one of the next few in rcheevos'
   * order. Any target with these bytes holds the same values as the table,
   * if it has them too: both are copied in the same snapshot. */
  failed = 0;
  {
    uint32_t n = S.chains.target_count, k = S.target_cursor;
#ifdef DSC_CHANGED_RANGES
    uint32_t missed;
#endif
    for (i = 0; i < n && i < CURSOR_WINDOW; i++, k = k + 1 == n ? 0 : k + 1) {
      const target_t* g = &slot->targets[S.chains.target_slot[k]];
      if (g->address != address)
        continue;
      S.target_cursor = k + 1 == n ? 0 : k + 1;
      if (g->length >= num_bytes) {
        memcpy(buffer, g->bytes, num_bytes);
        return num_bytes;
      }
      failed = !g->length;
      break;
    }
#ifdef DSC_CHANGED_RANGES
    missed = i == CURSOR_WINDOW && i < n;
#endif

    /* whole read inside one run of the snapshot's table, stored in order */
    lo = table_lower(t, address);
    if (table_run_read(t, lo, address, num_bytes)) {
      memcpy(buffer, slot->data + t->where[lo], num_bytes);
      return num_bytes;
    }

#ifdef DSC_CHANGED_RANGES
    /* rcheevos skips the chain reads whose pointer and target didn't change
     * (changes_before), so the one it asks for may be further on: look on,
     * in its order (the reads of a frame come in that order, so a frame's
     * look-ahead adds up to about one pass over the targets). */
    for (; missed && i < n; i++, k = k + 1 == n ? 0 : k + 1) {
      const target_t* g = &slot->targets[S.chains.target_slot[k]];
      if (g->address != address)
        continue;
      S.target_cursor = k + 1 == n ? 0 : k + 1;
      if (g->length >= num_bytes) {
        memcpy(buffer, g->bytes, num_bytes);
        return num_bytes;
      }
      failed = !g->length;
      break;
    }
#endif
  }

  /* A chain read that read nothing at VBlank, no byte in the table, first
   * byte in no region: no target has that byte either. It fails, as below. */
  if (failed && (lo == t->count || t->addresses[lo] - address >= num_bytes) && region_of(address, 1) == REGION_NONE)
    return 0;

  /* As the platform reads: a read not entirely inside one region fails
   * whole, and rcheevos then sees 0 (rc_read_memory uses the buffer, not
   * the count, so partial bytes would make a value rcheevos never sees).
   * The paths above only return reads inside one region. */
  if (region_of(address, num_bytes) == REGION_NONE)
    return 0;
  for (i = 0, j = lo; i < num_bytes; i++) {
    uint32_t a = address + i, k;
    while (j < t->count && t->addresses[j] < a)
      j++;
    if (j < t->count && t->addresses[j] == a) {
      buffer[i] = slot->data[t->where[j]];
      continue;
    }
    for (k = 0, failed = 0; k < S.chains.target_count; k++) {
      const target_t* g = &slot->targets[k];
      if (a - g->address < g->length)
        break;
      failed |= g->address == a && !g->length;
    }
    if (k < S.chains.target_count) {
      buffer[i] = slot->targets[k].bytes[a - slot->targets[k].address];
    }
    else if (i == 0 && failed && region_of(address, num_bytes) == REGION_NONE) {
      return 0;    /* the chain's read failed at VBlank (e.g. across the end of main RAM): fail it again */
    }
    else {
#ifdef DSC_CHANGED_RANGES
      S.live_read = 1;
#endif
      if (S.platform.read_memory(a, buffer + i, 1) != 1) {
        memset(buffer, 0, num_bytes);    /* fails whole, as the platform's read */
        return 0;
      }
      note_missing(a);
      S.stats.live_reads++;
    }
  }
  return num_bytes;
}

static void finish_http(void);

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
  S.http[i].taken = S.reply[i].seq;
  S.http[i].used = 1;
  S.http[i].callback = callback;
  S.http[i].callback_data = callback_data;
  request_journal_kind(&S.http[i], request->post_data);
  S.stats.pending_requests++;

  req.url = request->url;
  req.post_data = request->post_data;
  req.content_type = request->content_type;
  req.handle = &S.http[i];
  S.platform.http_send(&req);
#ifdef DSC_TEST_HOOKS
  if (T.sync_replies && S.http[i].used && S.reply[i].seq != S.http[i].taken)
    finish_http();
#endif
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
  dscm_notices_t* n = S.platform.notices;
  const rc_client_achievement_t* a = event->achievement;
  const rc_client_leaderboard_t* l = event->leaderboard;
  (void)client;
#ifdef DSC_TEST_HOOKS
  if (T.rc_event)
    T.rc_event(event, client);
#endif
  switch (event->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
      if (a->id >= WARNING_ID)
        break;    /* the server's "unsupported emulator" warning, not an unlock (see hardcore_refused) */
      emit(DSC_EVENT_UNLOCK, a->id, a->points, a->title, a->description);
      journal_add(DSCM_JOURNAL_AWARD, a->id, 0);
      notice(DSCM_NOTICE_UNLOCK, a->id, (int32_t)a->points, a->title, a->description);
      S.list_stale = 1;
      S.progress_stale = 1;
      break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
      notice(DSCM_NOTICE_PROGRESS, a->id, 0, a->title, a->measured_progress);
      S.list_stale = 1;
      break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
      if (n)
        indicator_set(n->challenges, a->id, a->title);
      S.list_stale = 1;
      break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
      if (n)
        indicator_clear(n->challenges, a->id);
      S.list_stale = 1;
      break;
    case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
      notice(DSCM_NOTICE_LEADERBOARD_STARTED, l->id, 0, l->title, NULL);
      break;
    case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
      notice(DSCM_NOTICE_LEADERBOARD_FAILED, l->id, 0, l->title, NULL);
      break;
    case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED: {
      int32_t score = ((const rc_client_leaderboard_info_t*)l)->value;
      emit(DSC_EVENT_LEADERBOARD_SUBMIT, l->id, 0, l->title, l->tracker_value);
      journal_add(DSCM_JOURNAL_LEADERBOARD, l->id, score);
      notice(DSCM_NOTICE_LEADERBOARD_SUBMITTED, l->id, score, l->title, l->tracker_value);
      break;
    }
    case RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD: {
      char rank[24];
      snprintf(rank, sizeof(rank), "%u/%u", (unsigned)event->leaderboard_scoreboard->new_rank,
        (unsigned)event->leaderboard_scoreboard->num_entries);
      notice(DSCM_NOTICE_LEADERBOARD_RANK, event->leaderboard_scoreboard->leaderboard_id,
        (int32_t)event->leaderboard_scoreboard->new_rank, l ? l->title : NULL, rank);
      break;
    }
    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE:
      if (n)
        indicator_set(n->trackers, event->leaderboard_tracker->id, event->leaderboard_tracker->display);
      break;
    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE:
      if (n)
        indicator_clear(n->trackers, event->leaderboard_tracker->id);
      break;
    case RC_CLIENT_EVENT_RESET:
      S.list_stale = 1;
      break;
    case RC_CLIENT_EVENT_GAME_COMPLETED: {
      const rc_client_game_t* game = rc_client_get_game_info(S.client);
      emit(DSC_EVENT_MASTERY, 0, 0, NULL, NULL);
      notice(DSCM_NOTICE_MASTERY, game ? game->id : 0, 0, game ? game->title : NULL, NULL);
      break;
    }
    case RC_CLIENT_EVENT_DISCONNECTED:
      notice(DSCM_NOTICE_OFFLINE, 0, 0, NULL, NULL);
      break;
    case RC_CLIENT_EVENT_RECONNECTED:
      notice(DSCM_NOTICE_ONLINE, 0, 0, NULL, NULL);
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
    rc_client_user_game_summary_t summary;
    char count[48];
    prepare_snapshots();
    S.hardcore_refused = S.config.hardcore && hardcore_refused(S.client);
    journal_start(game ? game->id : 0);
    list_alloc();
    rc_client_get_user_game_summary(S.client, &summary);
    snprintf(count, sizeof(count), "%u/%u achievements", (unsigned)summary.num_unlocked_achievements,
      (unsigned)summary.num_promoted_achievements);
    notice(DSCM_NOTICE_SESSION_START, game ? game->id : 0, rc_client_get_hardcore_enabled(S.client) ? 1 : 0,
      game ? game->title : NULL, count);
    PUBLISH_BARRIER();
    S.active = 1;
    progress_publish();
    Q.mode = MODE_COPY;
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
  uint32_t max;

  if (S.initialized)
    dsc_shutdown();
  if (!platform || !platform->read_memory || !platform->millis || !platform->http_send || !config)
    return -1;

  memset(&S, 0, sizeof(S));
  memset((void*)&Q, 0, sizeof(Q));
  memset((void*)&O, 0, sizeof(O));
  S.keep = NO_KEEP;
  S.platform = *platform;
  S.config = *config;
  if (!S.config.max_addresses)
    S.config.max_addresses = 4096;
  max = S.config.max_addresses;
  S.slot_count = S.config.snapshot_slots ? S.config.snapshot_slots : 2;
  if (S.slot_count > MAX_SLOTS - EVENT_RESERVE)
    S.slot_count = MAX_SLOTS - EVENT_RESERVE;
  S.arena_size = S.config.snapshot_bytes ? S.config.snapshot_bytes : S.slot_count * max * 4;   /* worst case: a word per address */

  /* tables are allocated as they are published, sized to what they hold */
  S.arena = (uint8_t*)malloc(S.arena_size);
  if (!S.arena)
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
#ifdef DSC_CHANGE_DRIVEN
  /* skip achievements and leaderboards whose inputs didn't change (rcheevos patch on our
   * fork, results identical to stock: docs/rcheevos-pin.md); off unless built with it */
  rc_client_set_change_driven_enabled(S.client, 1);
#endif
  /* before login: rc_client can't turn it on once a game is loaded */
  if (S.config.spectator)
    rc_client_set_spectator_mode_enabled(S.client, 1);

  if (S.platform.journal) {
    memset(S.platform.journal, 0, sizeof(dscm_journal_t));    /* magic is set once the game loads */
    journal_sync(S.platform.journal);
  }
  if (S.platform.notices) {
    memset(S.platform.notices, 0, sizeof(dscm_notices_t));
    S.platform.notices->magic = DSCM_NOTICES_MAGIC;
    S.nq = (dscm_notice_t*)calloc(NOTICE_QUEUE, sizeof(dscm_notice_t));   /* NULL: notices go straight to the ring */
  }

  if (S.config.handover) {
    /* the ARM9 loads the set (and should take the first frame, which builds
     * rcheevos' indexes), then gives it to the ARM7 with dsc_want(0) */
    O.req9 = 1;
    O.grant7 = 1;
    S.a9_want = 1;
    S.a9_acquired = 1;
  }
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

/* Producer: the oldest arena byte still needed, or NO_KEEP: the owner's
 * keep word (S.last's bytes), else the oldest queued slot. slot_tail is read
 * before keep: the owner stores keep before it moves slot_tail, so a stale
 * pair only ever protects more. */
WAKE_INL uint32_t arena_oldest(void)
{
  uint32_t tail = S.slot_tail, keep;
  PUBLISH_BARRIER();
  keep = S.keep;
  if (keep != NO_KEEP)
    return keep;
  return tail != S.slot_head ? S.slots[tail % MAX_SLOTS].offset : NO_KEEP;
}

/* The table the capture uses: S.current, announced in S.held first and read
 * again after (the owner frees a table only if held isn't it, see
 * table_in_use). Producer side. */
WAKE_INL const table_t* take_table(void)
{
  const table_t* t;
  do {
    t = S.current;
    S.held = t;
    CROSS_BARRIER();
    INTERLEAVE(1);
  } while (S.current != t);
  return t;
}

/* Space for n bytes after the newest snapshot, or at the start of the
 * arena, without reaching the oldest queued one. -1 if there is none. */
WAKE_INL int32_t arena_alloc(uint32_t n)
{
  uint32_t head = S.arena_head, tail = arena_oldest();
  if (tail == NO_KEEP)
    return n <= S.arena_size ? 0 : -1;    /* nothing queued or kept: start over */
  if (head >= tail) {
    if (S.arena_size - head >= n) return (int32_t)head;
    if (tail > n) return 0;
    return -1;
  }
  return tail - head > n ? (int32_t)head : -1;
}

/* Producer: the lag cap (config.lag_cap_*): queueing n more arena bytes as
 * a new frame would pass it. Repeats take no bytes and are always queued. */
COLD static int lag_capped(uint32_t n)
{
  uint32_t frames = S.items_in - S.items_out;
  if (S.config.lag_cap_frames && frames >= S.config.lag_cap_frames)
    return 1;
  if (S.config.lag_cap_bytes) {
    uint32_t used = 0, tail = arena_oldest();
    if (tail != NO_KEEP)
      used = S.arena_head >= tail ? S.arena_head - tail : S.arena_size - tail + S.arena_head;
    if (used + n > S.config.lag_cap_bytes)
      return 1;
  }
  return 0;
}

/* Words a..b of two snapshots hold the same watched bytes. */
static WAKE int same_words(const uint8_t* x, const uint8_t* y, const uint32_t* mask, uint32_t a, uint32_t b)
{
  const word_t* p = (const word_t*)x;
  const word_t* q = (const word_t*)y;
  for (; a < b; a++)
    if ((p[a] ^ q[a]) & mask[a])
      return 0;
  return 1;
}

/* Single words: a load and a store each. */
static inline HOT void copy_words(word_t* d, const uint8_t* base, const uint32_t* src, uint32_t n)
{
  for (; n >= 4; n -= 4, src += 4, d += 4) {
    uint32_t o0 = src[0], o1 = src[1], o2 = src[2], o3 = src[3];
    uint32_t a = *(const word_t*)(base + o0), b = *(const word_t*)(base + o1);
    uint32_t c = *(const word_t*)(base + o2), e = *(const word_t*)(base + o3);
    d[0] = a;
    d[1] = b;
    d[2] = c;
    d[3] = e;
  }
  while (n--)
    *d++ = *(const word_t*)(base + *src++);
}

/* Copy the table's words into a snapshot (see build_program). Returns 0,
 * or -1 if it stopped for an interrupt (dsc_capture). Nothing of ours is
 * read but the table: the repeat check (snapshot_same) waits for the frame's
 * commit, once a frame instead of at every capture. */
#if defined(__arm__) && defined(__GNUC__) && !defined(__clang__)
/* Compiled into dsc_capture (one frame and one literal pool for a wake, not
 * two); dsc_vblank_copy, which runs only until the first capture, calls the
 * out-of-line copy below. */
#define COPY_INL static inline __attribute__((always_inline, target("arm"), optimize("O2")))
#define COPY_OOL static __attribute__((noinline, target("arm"), optimize("O2")))
#else
#define COPY_INL static inline
#define COPY_OOL static
#endif
COPY_INL int copy_snapshot_inl(const table_t* t, uint8_t* data, const mem_t* m)
{
  word_t* d = (word_t*)data;
  uint32_t i, at = 0;

  if (!m->direct) {
    /* through read_memory, a run at a time, as it fails or succeeds whole */
    for (i = 0; i < t->run_count; i++) {
      const run_t* r = &t->runs[i];
      const uint32_t* where = &t->where[r->offset];
      uint8_t* out = data + where[0];
      uint32_t j;
      if (where[r->length - 1] == where[0] + r->length - 1) {
        if (S.platform.read_memory(r->address, out, r->length) != r->length)
          memset(out, 0, r->length);
        continue;
      }
      if (S.platform.read_memory(r->address, S.scratch, r->length) != r->length)
        memset(S.scratch, 0, r->length);
      for (j = 0; j < r->length; j++)
        data[where[j]] = S.scratch[j];
    }
    return 0;
  }

  /* single words */
  for (i = 0; i < 2; i++) {
    const uint8_t* base = m->base[i];
    const uint32_t* src = t->single + (i ? t->single_main : 0);
    uint32_t n = i ? t->single_count - t->single_main : t->single_main;
    if (!base) {
      memset((void*)(d + at), 0, n * 4);
    }
    else {
      uint32_t k, c;
      for (k = 0; k < n; k += c) {
        c = n - k < POLL_WORDS ? n - k : POLL_WORDS;
        copy_words(d + at + k, base, src + k, c);
        if (stop_now())
          return -1;
      }
    }
    at += n;
  }

  /* blocks */
  d += at;
  for (i = 0; i < t->block_count; i++) {
    const uint8_t* base = m->base[i < t->block_main ? REGION_MAIN : REGION_DTCM];
    uint32_t n = t->block[i * 2 + 1];
    const word_t* s;
    if (!base) {
      memset((void*)d, 0, n * 4);
      d += n;
      continue;
    }
    s = (const word_t*)(base + t->block[i * 2]);
    while (n) {
      uint32_t k = n < POLL_WORDS ? n : POLL_WORDS;
      n -= k;
      for (; k >= 4; k -= 4, s += 4, d += 4) {
        uint32_t a = s[0], b = s[1], c = s[2], e = s[3];
        d[0] = a;
        d[1] = b;
        d[2] = c;
        d[3] = e;
      }
      while (k--)
        *d++ = *s++;
      if (stop_now())
        return -1;
    }
  }

  /* words read_memory would not read, then bytes copied one by one */
  memset((void*)d, 0, t->zero_words * 4);
  for (i = 0; i < t->fix_count; i++) {
    uint32_t v;
    peek(m, t->fix[i].address, 1, &v);
    data[t->fix[i].at] = (uint8_t)v;
  }
  (void)at;
  return 0;
}

COPY_OOL int copy_snapshot(const table_t* t, uint8_t* data, const mem_t* m)
{
  return copy_snapshot_inl(t, data, m);
}

/* The repeat check: snapshot a holds the same watched bytes and chain
 * targets as b (same table and memory map, as the callers check). */
static WAKE_SMALL int snapshot_same(const slot_t* a, const slot_t* b)
{
  const table_t* t = a->table;
  uint32_t words = t->words * 4;
  return same_words(a->data, b->data, t->mask, 0, t->words)
         && memcmp(a->data + words, b->data + words, a->size - words) == 0;
}

/* Chain targets: only when the set has pointer chains (else 0, as
 * run_chains returns for no prev). */
#define HAS_CHAINS() (S.chains.root_count || S.chains.target_count)

#ifdef DSC_CHECK_FAST_STEPS
static void check_failed(const char* what, uint32_t index)
{
  fprintf(stderr, "dscheevos: snapshot check failed: %s %u (frame %u)\n", what, (unsigned)index, (unsigned)S.frame_counter);
  abort();
}

/* Tests: take the snapshot again as the code before the copy program did
 * (read_memory per run, every chain step through rcheevos), check the fast
 * snapshot holds the same bytes and targets, and keep it to check the
 * repeat decision. */
static void check_snapshot(const slot_t* slot, const table_t* t)
{
  uint32_t dense = (t->count + 3) & ~3u, size = dense + S.ref.target_count * sizeof(target_t), i, n = S.slot_head % MAX_SLOTS;
  uint8_t* shadow = (uint8_t*)realloc(S.shadow[n], size ? size : 1);
  target_t* targets = (target_t*)(shadow + dense);
  capture_t c;
  if (!shadow)
    check_failed("no memory", 0);
  S.shadow[n] = shadow;
  S.shadow_size[n] = size;
  memset(shadow, 0, size);
  for (i = 0; i < t->run_count; i++) {
    const run_t* r = &t->runs[i];
    if (S.platform.read_memory(r->address, shadow + r->offset, r->length) != r->length)
      memset(shadow + r->offset, 0, r->length);
  }
  for (i = 0; i < S.ref.direct_count; i++) {
    rc_memref_t* m = &S.ref.direct[i];
    rc_update_memref_value(&m->value, rc_read_memory(m->address, m->value.size, live_read, NULL));
  }
  c.targets = targets;
  c.next = 0;
  for (i = 0; i < S.ref.mod_count; i++) {
    rc_modified_memref_t* m = &S.ref.mods[i];
    uint32_t before = c.next;
    rc_update_memref_value(&m->memref.value, rc_get_modified_memref_value(m, capture_read, &c));
    if (m->modifier_type == RC_OPERATOR_INDIRECT_READ && c.next == before) {
      targets[c.next].address = 0;
      targets[c.next++].length = 0;
    }
  }
  if (S.ref.target_count != S.chains.target_count || c.next != S.ref.target_count)
    check_failed("target count", c.next);
  for (i = 0; i < t->count; i++)
    if (slot->data[t->where[i]] != shadow[i])
      check_failed("byte at address", t->addresses[i]);
  for (i = 0; i < S.ref.target_count; i++)
    if (memcmp(&targets[S.chains.target_order[i]], &slot->targets[i], sizeof(target_t)) != 0)
      check_failed("chain target", S.chains.target_order[i]);
}

/* The repeat decision the old code made: the whole old snapshot equal.
 * (Also only for a snapshot read the same way: when the platform starts or
 * stops being able to read a region, that frame is stored, not repeated.) */
static int check_repeat(const slot_t* prev, const slot_t* slot, const table_t* t)
{
  uint32_t a = (S.slot_head - 1) % MAX_SLOTS, b = S.slot_head % MAX_SLOTS;
  return prev->state == SLOT_READY && prev->table == t && prev->map == slot->map && S.shadow_size[a] == S.shadow_size[b] &&
         memcmp(S.shadow[a], S.shadow[b], S.shadow_size[b]) == 0;
}
#endif

/* Queue the stage (dsc_capture's snapshot) for evaluation, or count the
 * frame against the newest queued one. Idle time (drain). */
static WAKE_SMALL void commit(int staged)
{
  slot_t* slot = &S.slots[S.slot_head % MAX_SLOTS];
  slot_t* prev = S.slot_head != S.slot_tail ? &S.slots[(S.slot_head - 1) % MAX_SLOTS] : NULL;

  S.frame_counter++;
  /* frames waiting, repeats included: past MAX_SLOTS (~17 s), drop */
  if (S.items_in - S.items_out >= MAX_SLOTS) {
    if (staged) {
      sync_restore();
      slot->state = SLOT_FREE;
    }
    S.pstats.frames_dropped++;
    return;
  }
  if (prev && prev->event)
    prev = NULL;                 /* a lifecycle change: the frames before it are not repeated after it */
  if (staged) {
    int repeat = S.stage_same && prev && S.stage_prev == S.slot_head - 1 && prev->state == SLOT_READY
                 && snapshot_same(slot, prev);
    if ((S.config.lag_cap_frames || S.config.lag_cap_bytes) && lag_capped(repeat ? 0 : slot->size)) {
      sync_restore();
      slot->state = SLOT_FREE;
      S.pstats.frames_dropped++;
      S.pstats.frames_capped++;
      return;
    }
    S.sync_dirty = 0;            /* the chain copies' state is this frame's now */
    S.pstats.frames_snapshotted++;
    S.have_capture = 1;
    if (repeat) {
      /* prev is the newest slot: the owner may be evaluating it, but it
       * frees it only once a newer slot exists, so this frame is seen */
      INTERLEAVE(2);
      prev->repeats++;
      PUBLISH_BARRIER();
      S.items_in++;
      slot->state = SLOT_FREE;
      S.pstats.frames_repeated++;
      return;
    }
    slot->repeats = 0;
    slot->carry = 0;
    slot->event = QEV_NONE;
    slot->frame = S.frame_counter;
    S.arena_head = slot->offset + slot->size;
    slot->state = SLOT_READY;
    INTERLEAVE(2);
    PUBLISH_BARRIER();
    S.slot_head++;    /* publish last */
    S.items_in++;
    return;
  }

  /* No capture since the last VBlank: the game didn't halt (a lag frame),
   * or every capture stopped for an interrupt. Evaluate this frame with the
   * memory captured last, as a repeat of it. */
  if (S.stage_failed || (!prev && S.keep == NO_KEEP && !S.have_capture) || (!(prev && prev->state == SLOT_READY) && S.slot_head - S.slot_tail > S.slot_count)) {
    S.pstats.frames_dropped++;
    return;
  }
  if ((S.config.lag_cap_frames || S.config.lag_cap_bytes) && lag_capped(0)) {
    S.pstats.frames_dropped++;
    S.pstats.frames_capped++;
    return;
  }
  sync_unchanged();
  S.pstats.frames_snapshotted++;
  S.pstats.frames_stale++;
  if (prev && prev->state == SLOT_READY) {
    INTERLEAVE(2);
    prev->repeats++;
    PUBLISH_BARRIER();
    S.items_in++;
    return;
  }
  /* the newest snapshot is being evaluated, or done, or a lifecycle change
   * came after it: a slot that uses the memory evaluated last before it */
  slot->carry = 1;
  slot->event = QEV_NONE;
  slot->repeats = 0;
  slot->size = 0;
  slot->offset = S.arena_head;
  slot->data = NULL;
  slot->targets = NULL;
  slot->table = NULL;
  slot->frame = S.frame_counter;
  slot->state = SLOT_READY;
  PUBLISH_BARRIER();
  S.slot_head++;
  S.items_in++;
}

/* The VBlanks the IRQ queued, oldest first, into the ring. Idle time:
 * called before anything that looks at the ring or the stage. */
WAKE_INL void drain(void)
{
  uint32_t n = Q.frames;
  while (Q.taken != n) {
    uint32_t f = Q.taken + 1;
    commit(Q.sealed == f);
    PUBLISH_BARRIER();
    Q.taken = f;
  }
}

static inline __attribute__((always_inline)) void queue_frame(void)
{
  uint32_t f = Q.frames + 1;
  if (Q.stage) {
    Q.stage = 0;
    Q.sealed = f;
  }
  Q.frames = f;      /* publish last */
}

/* No stack, no calls: the module runs it on the game's IRQ stack. A copy
 * (nothing captured yet, or capture_at_halt off) needs dsc_vblank_copy().
 * While drain() still has frames to commit, this one waits behind them:
 * only one side moves the ring at a time. */
IRQ_TEXT int dsc_vblank_queue(void)
{
  uint32_t mode = Q.mode;
  Q.count++;
  if (mode == MODE_QUEUE || Q.frames != Q.taken) {
    queue_frame();
    return 0;
  }
  return mode == MODE_COPY;
}

void dsc_vblank(void)
{
  if (dsc_vblank_queue())
    dsc_vblank_copy();
}

HOT void dsc_vblank_copy(void)
{
  const table_t* t;
  slot_t* slot;
  slot_t* prev = NULL;
  mem_t m;
  int32_t offset;
  uint32_t size;
  int same;

  if (!S.active)
    return;
  /* At halt, but nothing captured yet (session start, or after a reset):
   * this one frame is copied here, so the first frames aren't lost. Once
   * there is a capture, VBlanks only queue. */
  if (S.config.capture_at_halt && (Q.stage || S.have_capture || S.stage_failed)) {
    Q.mode = MODE_QUEUE;
    queue_frame();
    return;
  }

  S.frame_counter++;
  t = take_table();
#ifdef DSC_CHECK_FAST_STEPS
  {
    /* the owner may publish and free tables now (the other CPU): the one
     * taken must stay as it is through the copy */
    uint32_t generation = t->generation;
    const uint32_t* addresses = t->addresses;
    INTERLEAVE(5);
    if (t->generation != generation || t->addresses != addresses)
      check_failed("table freed under a capture", generation);
  }
#endif
  size = t->words * 4 + S.chains.target_count * sizeof(target_t);
  S.pstats.frame_bytes = size;
  offset = S.slot_head - S.slot_tail <= S.slot_count ? arena_alloc(size) : -1;
  if (offset < 0) {
    S.pstats.frames_dropped++;
    return;
  }

  /* Same watched bytes and targets as the newest queued frame (which may be
   * being evaluated: it is freed only once a newer slot exists): count it
   * there instead (cutscenes, loading, menus).
   * Each repeat is still evaluated as its own frame, so nothing is lost.
   * The copy compares as it goes, until the first difference. */
  slot = &S.slots[S.slot_head % MAX_SLOTS];
  slot->map = mem_open(&m);
  if (S.slot_head != S.slot_tail) {
    prev = &S.slots[(S.slot_head - 1) % MAX_SLOTS];
    if (prev->state != SLOT_READY || prev->event || prev->table != t || prev->map != slot->map)
      prev = NULL;
  }

  slot->state = SLOT_FILLING;
  slot->event = QEV_NONE;
  slot->offset = (uint32_t)offset;
  slot->size = size;
  slot->data = S.arena + offset;
  slot->targets = (target_t*)(slot->data + t->words * 4);
  slot->table = t;
  copy_snapshot(t, slot->data, &m);
  if (HAS_CHAINS())
    run_chains(slot->targets, &m, NULL);
  same = prev && snapshot_same(slot, prev);
  if ((S.config.lag_cap_frames || S.config.lag_cap_bytes) && lag_capped(same ? 0 : size)) {
    slot->state = SLOT_FREE;
    S.pstats.frames_dropped++;
    S.pstats.frames_capped++;
    return;
  }
  slot->frame = S.frame_counter;
  S.pstats.frames_snapshotted++;
  S.have_capture = 1;
#ifdef DSC_CHECK_FAST_STEPS
  check_snapshot(slot, t);
  if (S.slot_head != S.slot_tail && same != check_repeat(&S.slots[(S.slot_head - 1) % MAX_SLOTS], slot, t))
    check_failed("repeat decision", same);
#endif

  if (same) {
    INTERLEAVE(2);
    prev->repeats++;
    PUBLISH_BARRIER();
    S.items_in++;
    slot->state = SLOT_FREE;
    S.pstats.frames_repeated++;
    return;
  }

  slot->repeats = 0;
  slot->carry = 0;
  S.arena_head = (uint32_t)offset + size;
  slot->state = SLOT_READY;
  PUBLISH_BARRIER();
  S.slot_head++;    /* publish last */
  S.items_in++;
}

WAKE int dsc_capture(void)
{
  const table_t* t;
  slot_t* slot;
  slot_t* prev = NULL;
  mem_t m;
  int32_t offset;
  uint32_t size, head;
  int same, chains;

  if (!S.active || !S.config.capture_at_halt)
    return -1;
  drain();
  /* replaces a capture taken earlier in this frame: the game ran since */
  Q.stage = 0;
  sync_restore();

  t = take_table();
#ifdef DSC_CHECK_FAST_STEPS
  {
    /* the owner may publish and free tables now (the other CPU): the one
     * taken must stay as it is through the copy */
    uint32_t generation = t->generation;
    const uint32_t* addresses = t->addresses;
    INTERLEAVE(5);
    if (t->generation != generation || t->addresses != addresses)
      check_failed("table freed under a capture", generation);
  }
#endif
  size = t->words * 4 + S.chains.target_count * sizeof(target_t);
  S.pstats.frame_bytes = size;
  offset = S.slot_head - S.slot_tail <= S.slot_count ? arena_alloc(size) : -1;
  if (offset < 0) {
    S.stage_failed = 1;
    return -1;
  }
  S.stage_failed = 0;

  slot = &S.slots[S.slot_head % MAX_SLOTS];
  slot->map = mem_open(&m);
  if (S.slot_head != S.slot_tail) {
    prev = &S.slots[(S.slot_head - 1) % MAX_SLOTS];
    if (prev->state != SLOT_READY || prev->carry || prev->event || prev->table != t || prev->map != slot->map)
      prev = NULL;
  }
  slot->state = SLOT_FILLING;
  slot->event = QEV_NONE;
  slot->offset = (uint32_t)offset;
  slot->size = size;
  slot->data = S.arena + offset;
  slot->targets = (target_t*)(slot->data + t->words * 4);
  slot->table = t;
  sync_save();
  head = S.slot_head;
  S.capture_frame = Q.count;
  S.capturing = S.platform.interrupt_pending != NULL;
  same = copy_snapshot_inl(t, slot->data, &m);
  chains = same < 0 ? -1 : HAS_CHAINS() ? run_chains(slot->targets, &m, NULL) : 0;
  S.capturing = 0;
  if (chains < 0) {
    sync_restore();
    if (S.slot_head == head)
      slot->state = SLOT_FREE;    /* else a VBlank copied into it (nothing captured before) */
    S.pstats.captures_stopped++;
    return 1;
  }
  /* a repeat of prev? decided at the commit (snapshot_same), once a frame */
  same = prev != NULL;
#ifdef DSC_CHECK_FAST_STEPS
  check_snapshot(slot, t);
  if (prev && snapshot_same(slot, prev) != check_repeat(prev, slot, t))
    check_failed("repeat decision", same);
#endif
  S.pstats.captures++;
  S.stage_same = same;
  S.stage_prev = S.slot_head - 1;
  PUBLISH_BARRIER();
  Q.stage = 1;
  return 0;
}

static void finish_http(void)
{
  int i, progress = 1;
  /* a reply can start the next request (login -> set -> session), so loop until settled */
  while (progress) {
   progress = 0;
   for (i = 0; i < MAX_PENDING_HTTP; i++) {
    http_pending_t* p = &S.http[i];
    const http_reply_t* r = &S.reply[i];
    uint32_t seq = r->seq;
    if (p->used && seq != p->taken) {
      rc_api_server_response_t resp;
      rc_client_server_callback_t cb = p->callback;
      void* data = p->callback_data;
      char* body;
      size_t length = r->body_length;
      int status = r->status;

      PUBLISH_BARRIER();      /* the answer is complete before seq moves */
      if (r->copied) {
        body = (char*)r->body;
      }
      else {
        /* the platform's buffer: copied here, on the owner, which is the
         * only side that allocates (one allocator, two CPUs), then given back */
        body = (char*)malloc(length + 1);
        if (body) {
          memcpy(body, r->body, length);
          body[length] = '\0';
        }
        else {
          length = 0;
          status = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
        }
        S.platform.http_release(p);
      }
      resp.body = body ? body : "";
      resp.body_length = length;
      resp.http_status_code = status;
      p->taken = seq;
      p->used = 0;
      S.http_taken++;
      S.stats.pending_requests--;
      if (p->journal_kind && request_answered(p->journal_kind, &resp)) {
        journal_answered(p->journal_kind, p->journal_id, p->journal_score);
        if (p->journal_kind == DSCM_JOURNAL_AWARD)
          emit(DSC_EVENT_UNLOCK_SENT, p->journal_id, 0, NULL, NULL);
      }
      cb(&resp, data);
      free(body);
      progress = 1;
    }
   }
  }
}

#ifdef DSC_CHANGED_RANGES
/* Adds [address, address + n) to the frame's changed memory. Returns 0, or
 * -1 if out of memory. */
static int add_range(uint32_t address, uint32_t n)
{
  rc_client_memory_range_t* r;
  if (S.range_count) {
    r = &S.ranges[S.range_count - 1];
    if (r->address + r->num_bytes == address) {
      r->num_bytes += n;
      return 0;
    }
  }
  if (S.range_count == S.range_size) {
    uint32_t size = S.range_size ? S.range_size * 2 : 64;
    r = (rc_client_memory_range_t*)realloc(S.ranges, size * sizeof(*r));
    if (!r)
      return -1;
    S.ranges = r;
    S.range_size = size;
  }
  r = &S.ranges[S.range_count++];
  r->address = address;
  r->num_bytes = n;
  return 0;
}

/* Tells rcheevos which memory may read differently in this frame (slot s,
 * frame number frame) than in the frame evaluated before: every byte
 * read_memory serves from either snapshot that differs, with the same table
 * (the same words) and chains (the same targets). A target that moved or
 * changed counts at its old and its new address. Otherwise "unknown". */
static void changes_before(const slot_t* s, uint32_t frame)
{
  static const rc_client_memory_range_t none;
  const table_t* t = s->table;
  uint32_t a;

  S.range_count = 0;
  if (!S.prev_valid || frame != S.prev_next_frame || t != S.prev_table || s->map != S.prev_map ||
      !s->data || !S.prev_data || (s->data == S.prev_data) != (s->targets == S.prev_targets)) {
    rc_client_set_changed_memory(S.client, NULL, 0);
    return;
  }
  if (s->data != S.prev_data) {
    const word_t* p = (const word_t*)s->data;
    const word_t* q = (const word_t*)S.prev_data;
    for (a = 0; a < t->words; a++) {
      if (((p[a] ^ q[a]) & t->mask[a]) && add_range(t->word_address[a], 4) != 0) {
        rc_client_set_changed_memory(S.client, NULL, 0);
        return;
      }
    }
    for (a = 0; a < S.chains.target_count; a++) {
      const target_t* g = &s->targets[a];
      const target_t* h = &S.prev_targets[a];
      if (g->address == h->address && g->length == h->length && !memcmp(g->bytes, h->bytes, g->length))
        continue;
      if (add_range(h->address, h->length ? h->length : 1) != 0 ||
          add_range(g->address, g->length ? g->length : 1) != 0) {
        rc_client_set_changed_memory(S.client, NULL, 0);
        return;
      }
    }
  }
  rc_client_set_changed_memory(S.client, S.range_count ? S.ranges : &none, S.range_count);
}

#ifdef DSC_CHECK_FAST_STEPS
static uint32_t RC_CCONV check_read(uint32_t address, uint8_t* buffer, uint32_t num_bytes, void* ud)
{
  /* as rc_client_read_modified_memory_helper */
  if (address > S.client->game->max_valid_address)
    return 0;
  return read_memory(address, buffer, num_bytes, (rc_client_t*)ud);
}

/* Every indirect read rcheevos skipped holds what reading it now gives. */
static void check_indirect_reads(void)
{
  rc_modified_memref_list_t* ml;
  uint32_t i, index = 0;
  if (!S.client->game || S.client->game->waiting_for_reset || !S.client->game->runtime.memrefs)
    return;
  for (ml = &S.client->game->runtime.memrefs->modified_memrefs; ml; ml = ml->next) {
    for (i = 0; i < ml->count; i++, index++) {
      const rc_modified_memref_t* m = &ml->items[i];
      if (m->modifier_type == RC_OPERATOR_INDIRECT_READ &&
          rc_get_modified_memref_value(m, check_read, S.client) != m->memref.value.value)
        check_failed("indirect read after changes_before", index);
    }
  }
}
#endif

/* After the frame: it is the one evaluated before the next, unless it read
 * game memory live (which may have changed since). */
static void changes_after(const slot_t* s, uint32_t frame)
{
#ifdef DSC_CHECK_FAST_STEPS
  check_indirect_reads();
#endif
  S.prev_valid = !S.live_read && s->data != NULL;
  S.live_read = 0;
  S.prev_data = s->data;
  S.prev_targets = s->targets;
  S.prev_table = s->table;
  S.prev_map = s->map;
  S.prev_next_frame = frame + 1;
}
#endif

/* ---- the owner's work, and who owns the rc_client ---- */

#ifdef DSC_CHECK_FAST_STEPS
/* Test builds: the CPU running the owner's work owns the rc_client by the
 * flags, at every frame boundary. */
static void check_owner(int cpu)
{
  int owner = !S.config.handover ? DSC_OWNER_ARM9 : (O.req9 && O.grant7) ? DSC_OWNER_ARM9 : DSC_OWNER_ARM7;
  if (S.config.handover && cpu == DSC_OWNER_ARM7 && O.grant7)
    owner = -1;
  if (owner != cpu) {
    fprintf(stderr, "dscheevos: evaluation by a CPU that doesn't own the rc_client (cpu %d, req9 %u, grant7 %u)\n",
            cpu, (unsigned)O.req9, (unsigned)O.grant7);
    abort();
  }
}
#define CHECK_OWNER(cpu) check_owner(cpu)
#else
#define CHECK_OWNER(cpu) ((void)0)
#endif

/* Stop before the next frame: the ARM9 asked for the rc_client (the ARM7
 * gives it at this boundary), or notices are waiting for readers. */
static inline __attribute__((always_inline)) int must_stop(int cpu)
{
  if (cpu == DSC_OWNER_ARM7 && O.req9)
    return 1;
  return !S.flushing && S.nq && S.nq_head - S.nq_tail > NOTICE_QUEUE - NOTICE_HEADROOM;
}

/* A lifecycle change, in its place among the frames. */
COLD static void apply_event(uint8_t ev)
{
#ifdef DSC_TEST_HOOKS
  if (T.lifecycle)
    T.lifecycle(ev);
#endif
  S.progress_stale = 1;
  if (ev == QEV_RESET) {
    if (S.active) {
      rc_client_reset(S.client);
      S.list_stale = 1;
    }
  }
  else if (ev == QEV_HARDCORE_OFF) {
    rc_client_set_hardcore_enabled(S.client, 0);
    S.list_stale = 1;
  }
#ifdef DSC_CHANGED_RANGES
  S.prev_valid = 0;    /* the next frame's changed memory is "unknown" */
#endif
}

/* Idle-side writes to the ring (push_event): in MODE_COPY the VBlank IRQ
 * writes slot_head too (dsc_vblank_copy), so until ring_unlock it only
 * counts frames, which drain() then commits behind what idle wrote. */
COLD static uint32_t ring_lock(void)
{
  uint32_t mode = Q.mode;
  if (mode == MODE_COPY)
    Q.mode = MODE_QUEUE;
  return mode;
}

COLD static void ring_unlock(uint32_t mode)
{
  if (mode == MODE_COPY)
    Q.mode = MODE_COPY;
}

/* Queue a lifecycle change behind the frames already queued (producer side,
 * idle context, between ring_lock and ring_unlock). EVENT_RESERVE slots are
 * kept for these. */
COLD static void push_event(uint8_t ev)
{
  slot_t* slot;
  if (S.slot_head - S.slot_tail >= MAX_SLOTS) {
    S.pstats.lifecycle_lost++;
    return;
  }
  slot = &S.slots[S.slot_head % MAX_SLOTS];
  slot->event = ev;
  slot->carry = 0;
  slot->repeats = 0;
  slot->size = 0;
  slot->offset = S.arena_head;
  slot->data = NULL;
  slot->targets = NULL;
  slot->table = NULL;
  slot->map = 0;
  slot->frame = S.frame_counter;
  slot->state = SLOT_READY;
  PUBLISH_BARRIER();
  S.slot_head++;    /* publish last */
  S.items_in++;
}

/* The owner's pass (cpu: DSC_OWNER_*): server answers, then the frames and
 * lifecycle changes queued when it starts, oldest first, at most budget
 * frames; rcheevos' timers, the list and the notices. Ownership can change
 * only between two frames (must_stop), never inside one. */
/* inline: its loop stays inside dsc_idle, a listed hot function (hot.ld) */
static inline __attribute__((always_inline)) int owner_pass(uint32_t budget, int cpu)
{
  int evaluated = 0, applied = 0;
  uint32_t stop;

  CHECK_OWNER(cpu);
  finish_http();

  /* Only what is queued now: frames that arrive meanwhile wait for the next
   * call, so the caller gets back to the network and the next capture even
   * when evaluation is slower than the game. */
  stop = S.items_in;
  while (budget && S.items_out != stop) {
    uint32_t tail = S.slot_tail, n, k, k0;
    slot_t* next;
    if (tail == S.slot_head || must_stop(cpu))
      break;
    next = &S.slots[tail % MAX_SLOTS];
    if (next->event) {
      apply_event(next->event);
      applied++;
      S.items_out++;
      PUBLISH_BARRIER();
      S.slot_tail = tail + 1;
      continue;
    }
    n = 1 + next->repeats;
    k0 = k = S.done[tail % MAX_SLOTS];
    if (k < n) {
      slot_t carried;
      if (next->carry) {
        /* frames with no capture: the last snapshot's memory */
        carried = S.last;
        carried.frame = next->frame;
        S.evaluating = &carried;
      }
      else {
        S.evaluating = next;
      }
      for (; k < n && budget && S.items_out != stop; k++, budget--) {
        if (k != k0 && must_stop(cpu))
          break;
        CHECK_OWNER(cpu);
        INTERLEAVE(3);
        S.seq_next = 0;
        S.eval_frame = next->frame + k;
#ifdef DSC_TEST_HOOKS
        if (T.frame_begin)
          T.frame_begin(S.eval_frame);
#endif
#ifdef DSC_CHANGED_RANGES
        changes_before(S.evaluating, next->frame + k);
        rc_client_do_frame(S.client);
        changes_after(S.evaluating, next->frame + k);
#else
        rc_client_do_frame(S.client);
#endif
#ifdef DSC_TEST_HOOKS
        if (T.frame_end)
          T.frame_end(S.client, S.eval_frame);
#endif
        S.stats.frames_evaluated++;
        S.items_out++;
        evaluated++;
#ifdef DSC_TEST_HOOKS
        if (T.sync_replies)
          finish_http();
#endif
      }
      S.done[tail % MAX_SLOTS] = k;
      S.evaluating = NULL;
      publish_pending_addresses();
      finish_http();
      notices_send();
      if (k < n)
        break;    /* out of budget, or ownership/notices: the rest of this slot next time */
      continue;   /* look again: the producer may have added repeats meanwhile */
    }
    /* Every frame of it so far is evaluated. The newest slot may still get
     * repeats (the producer adds them only there), so it stays queued; once
     * a newer slot exists its repeats are final (stored before slot_head
     * moved): read them again, then free it. */
    INTERLEAVE(4);       /* (n was read above: the producer may add to it, then seal it) */
    if (S.slot_head == tail + 1)
      break;
    PUBLISH_BARRIER();
    if (1 + next->repeats != k)
      continue;
#ifdef DSC_CHANGED_RANGES
    if (!next->carry) {
      /* keep its bytes: a carry slot after it reads them, changes_before compares with them */
#else
    if (S.config.capture_at_halt && !next->carry) {
      /* keep its bytes: a carry slot after it reads them */
#endif
      S.last = *next;
      S.has_last = 1;
      S.keep = next->offset;     /* before slot_tail moves (see arena_oldest) */
    }
    S.done[tail % MAX_SLOTS] = 0;
    PUBLISH_BARRIER();
    S.slot_tail = tail + 1;
  }

  release_tables();
  /* rcheevos' timers when no frame ran them (rc_client_do_frame does). Not
   * right after a lifecycle change with its frame still to come: an emulator
   * applies it and runs that frame at once (a ping due then would find no
   * frame since the last one and be put off) */
  if (!evaluated && !applied && S.active)
    rc_client_idle(S.client);
  list_update();
  notices_send();
  if (S.progress_stale)
    progress_publish();

  if (S.pstats.frames_dropped != S.dropped_reported) {
    S.dropped_reported = S.pstats.frames_dropped;
    emit(DSC_EVENT_DEGRADED, S.dropped_reported, 0, NULL, "evaluation fell behind; frames were dropped");
  }
  return evaluated;
}

/* ARM9, between frames: give the rc_client back. Everything the ARM9 wrote
 * into shared state must reach RAM first; nothing after it may write there. */
COLD static void arm9_release(void)
{
  if (S.a9_acquired) {
    S.stats.handovers++;
    if (S.platform.ownership)
      S.platform.ownership(0);
  }
  S.a9_acquired = 0;
  PUBLISH_BARRIER();
  O.req9 = 0;
}

/* ARM9, between frames: 1 if it owns the rc_client now (acquiring it if
 * the ARM7 just granted it). Asks for it when wanted; never waits. */
static inline __attribute__((always_inline)) int arm9_turn(void)
{
  if (!S.config.handover)
    return 1;
  if (O.req9 && O.grant7) {
    if (!S.a9_want) {
      arm9_release();
      return 0;
    }
    if (!S.a9_acquired) {
      if (S.platform.ownership)
        S.platform.ownership(1);
      S.a9_acquired = 1;
      S.stats.handovers++;
    }
    return 1;
  }
  if (S.a9_want && !O.req9 && !O.grant7)
    O.req9 = 1;    /* the ARM7 grants it between two of its frames */
  return 0;
}

/* LAB (coverage): dsc_idle evaluating at most max_frames queued frames. */
int dsc_idle_max(uint32_t max_frames)
{
  if (!S.initialized)
    return 0;
  drain();
  if (!arm9_turn())
    return 0;
  return owner_pass(max_frames, DSC_OWNER_ARM9);
}

int dsc_idle(void)
{
  if (!S.initialized)
    return 0;

  /* The game's idle thread wakes at every IRQ, often many times a frame.
   * In the same frame as the last call, with no VBlank to commit, nothing
   * queued and no answer waiting, there is nothing to do: return before
   * touching more memory (rcheevos' own timers run once a frame below). */
  {
    uint32_t clock = S.platform.frame ? S.platform.frame() : S.platform.millis();
    if (clock == S.idle_clock && Q.frames == Q.taken && S.items_in == S.items_out && S.http_answered == S.http_taken
        && S.a9_want == S.a9_acquired)    /* hand-over: wanted and not held yet, ask (arm9_turn) */
      return 0;
    S.idle_clock = clock;
  }

  drain();
  if (!arm9_turn())
    return 0;
  return owner_pass(S.config.idle_max_frames ? S.config.idle_max_frames : 0xFFFFFFFFu, DSC_OWNER_ARM9);
}

COLD void dsc_want(int want)
{
  if (!S.initialized || !S.config.handover)
    return;
  S.a9_want = want != 0;
  if (!S.a9_want && S.config.handover && O.req9)
    arm9_release();    /* between frames: give it back (or stop asking) now */
}

COLD int dsc_handover_policy(int frozen)
{
  uint32_t frames, bytes, hi_f, lo_f, hi_b, lo_b;
  int want;
  if (!S.initialized || !S.config.handover)
    return 0;
  frames = dsc_backlog();
  /* bytes behind: what the arena holds less the frame evaluated last (kept
   * for a repeat or carry), so a caught-up queue counts 0 */
  bytes = dsc_arena_used();
  bytes = !frames ? 0 : bytes > S.pstats.frame_bytes ? bytes - S.pstats.frame_bytes : 0;
  hi_f = S.config.handover_hi_frames ? S.config.handover_hi_frames : 6;
  lo_f = S.config.handover_lo_frames ? S.config.handover_lo_frames : 1;
  hi_b = S.config.handover_hi_bytes ? S.config.handover_hi_bytes : S.arena_size / 4;
  lo_b = S.config.handover_lo_bytes ? S.config.handover_lo_bytes : S.arena_size / 16;
  if (frozen || frames >= hi_f || bytes >= hi_b)
    want = 1;
  else if (S.a9_want && (frames > lo_f || bytes > lo_b))
    want = 1;      /* behind: keep it until the backlog is short in frames and in bytes */
  else
    want = 0;
  if (want != S.a9_want)
    dsc_want(want);
  return want;
}

COLD int dsc_owner(void)
{
  if (!S.config.handover)
    return DSC_OWNER_ARM9;
  return O.req9 && O.grant7 ? DSC_OWNER_ARM9 : DSC_OWNER_ARM7;
}

COLD int dsc_arm7_step(uint32_t max_frames)
{
  int n;
  if (!S.initialized || !S.config.handover)
    return 0;
  if (O.grant7) {
    if (O.req9)
      return 0;          /* the ARM9 has it */
    O.grant7 = 0;        /* given back: the ARM7's again */
  }
  if (O.req9) {
    O.grant7 = 1;        /* asked: between frames, so grant it now */
    return 0;
  }
  n = owner_pass(max_frames ? max_frames : 0xFFFFFFFFu, DSC_OWNER_ARM7);
  if (O.req9) {
    PUBLISH_BARRIER();
    O.grant7 = 1;        /* stopped for it at a frame boundary */
  }
  return n;
}

COLD uint32_t dsc_backlog(void)
{
  if (!S.initialized)
    return 0;
  /* lifecycle entries count too: one each */
  return (Q.frames - Q.taken) + (S.items_in - S.items_out);
}

COLD int dsc_flush(void)
{
  int n;
  if (!S.initialized)
    return 0;
  drain();
  if (!arm9_turn())
    return -1;
  S.flushing = 1;
  n = owner_pass(0xFFFFFFFFu, DSC_OWNER_ARM9);
  S.flushing = 0;
  return n;
}

void dsc_http_done(void* handle, int http_status, const char* body, size_t body_length)
{
  http_pending_t* p = (http_pending_t*)handle;
  http_reply_t* r;
  uint32_t i;
  if (!p || p < S.http || p >= S.http + MAX_PENDING_HTTP)
    return;
  i = (uint32_t)(p - S.http);
  r = &S.reply[i];
  if (!p->used || r->seq != p->taken)
    return;              /* not asked, or answered already */
  if (S.platform.http_release) {
    /* the platform keeps the bytes until http_release: nothing allocated here */
    r->body = body;
    r->copied = 0;
    r->body_length = body_length;
    r->status = http_status;
  }
  else {
    char* copy = (char*)malloc(body_length + 1);
    r->copied = 1;
    if (copy) {
      memcpy(copy, body, body_length);
      copy[body_length] = '\0';
      r->body = copy;
      r->body_length = body_length;
      r->status = http_status;
    }
    else {
      r->body = NULL;
      r->body_length = 0;
      r->status = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
    }
  }
  PUBLISH_BARRIER();
  r->seq = p->taken + 1;
  S.http_answered++;
}

int dsc_work_pending(void)
{
  return S.initialized && (Q.frames != Q.taken || S.items_in != S.items_out || S.http_answered != S.http_taken);
}

void dsc_state_view(uint32_t high_byte)
{
#if defined(__arm__)
  S_view = (struct state_t*)(((uint32_t)&S_storage & 0x00FFFFFFu) | high_byte);
#else
  (void)high_byte;
#endif
}

WAKE void dsc_drain(void)
{
  if (S.initialized)
    drain();
}

COLD uint32_t dsc_arena_used(void)
{
  uint32_t tail;
  if (!S.initialized)
    return 0;
  /* as arena_alloc sees it: from the oldest bytes still needed to the newest */
  tail = arena_oldest();
  if (tail == NO_KEEP)
    return 0;
  return S.arena_head >= tail ? S.arena_head - tail : S.arena_size - tail + S.arena_head;
}

/* The owner's counters, with the producer's merged in (one writer each). */
void dsc_get_stats(dsc_stats_t* stats)
{
  /* drain() is the producer's (the ARM9's): with hand-over this may be
   * called on the owner, e.g. from an event callback, so it only reads */
  if (S.initialized && !S.config.handover)
    drain();
  *stats = S.stats;
  stats->frames_snapshotted = S.pstats.frames_snapshotted;
  stats->frames_dropped = S.pstats.frames_dropped;
  stats->frames_repeated = S.pstats.frames_repeated;
  stats->frames_stale = S.pstats.frames_stale;
  stats->captures = S.pstats.captures;
  stats->captures_stopped = S.pstats.captures_stopped;
  stats->lifecycle_lost = S.pstats.lifecycle_lost;
  stats->frame_bytes = S.pstats.frame_bytes;
  stats->frames_capped = S.pstats.frames_capped;
  stats->arena_used = dsc_arena_used();
}

COLD void dsc_reset(void)
{
  uint32_t mode;
  if (!S.initialized)
    return;
  mode = ring_lock();
  /* no stage for the next VBlank (it holds memory from before the reset),
   * then the frames already counted: they come before the reset */
  Q.stage = 0;
  drain();
  sync_restore();
  if (S.active)
    push_event(QEV_RESET);     /* rc_client_reset once the frames before it are evaluated */
  S.have_capture = 0;
  ring_unlock(S.active ? MODE_COPY : mode);    /* the next frame is copied in the IRQ again, as at the start */
}

COLD void dsc_hardcore_off(void)
{
  uint32_t mode;
  if (!S.initialized)
    return;
  mode = ring_lock();
  drain();
  push_event(QEV_HARDCORE_OFF);
  ring_unlock(mode);
}

/* RetroAchievements adds a warning achievement (ID 101000001 and up) to the
 * set when it doesn't recognise the client; it then files hardcore unlocks
 * as softcore. rc_client never submits it and leaves it out of the totals. */
static int hardcore_refused(rc_client_t* client)
{
  rc_client_achievement_list_t* list = rc_client_create_achievement_list(client,
    RC_CLIENT_ACHIEVEMENT_CATEGORY_PROMOTED, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
  uint32_t b, i;
  int found = 0;
  if (!list)
    return 0;
  for (b = 0; b < list->num_buckets && !found; b++)
    for (i = 0; i < list->buckets[b].num_achievements; i++)
      if (list->buckets[b].achievements[i]->id >= 101000001u)
        found = 1;
  rc_client_destroy_achievement_list(list);
  return found;
}

COLD void dsc_get_progress(dsc_progress_t* progress)
{
  int tries;
  memset(progress, 0, sizeof(*progress));
  if (!S.initialized || !S.active)
    return;
  /* the owner's last published copy (progress_publish): two buffers, so a
   * copy that saw the index move is taken again, once */
  for (tries = 0; tries < 2; tries++) {
    uint32_t i = S.progress_index;
    PUBLISH_BARRIER();
    *progress = S.progress[i & 1];
    PUBLISH_BARRIER();
    if (S.progress_index == i)
      break;
  }
}

void dsc_shutdown(void)
{
  int i;
  if (!S.initialized)
    return;
  /* the frames still queued: an unlock in the last moments before a quit
   * is evaluated (and journalled) like any other */
  Q.mode = MODE_OFF;           /* no more frames: VBlanks from here on are not queued */
  if (S.active)
    dsc_flush();    /* -1 without the rc_client (hand-over): the platform hands it over first */
  S.active = 0;
  if (S.client) {
    /* rcheevos (v12.5.0) raises a pending mastery event while unloading,
     * after it has cleared client->game, and crashes. Mastery is pending
     * from the server's answer to the last unlock until the next frame: a
     * game quit right then just shows no mastery notice. */
    if (S.client->game) {
      rc_client_subset_info_t* sub;
      for (sub = S.client->game->subsets; sub; sub = sub->next)
        if (sub->mastery == RC_CLIENT_MASTERY_STATE_PENDING)
          sub->mastery = RC_CLIENT_MASTERY_STATE_SHOWN;
    }
    rc_client_unload_game(S.client);
    rc_client_destroy(S.client);
  }
  for (i = 0; i < TABLES; i++)
    table_free(&S.tables[i]);
  free(S.arena);
  free(S.scratch);
  free(S.seq_address);
  free(S.seq_bytes);
#ifdef DSC_CHECK_FAST_STEPS
  for (i = 0; i < MAX_SLOTS; i++)
    free(S.shadow[i]);
#endif
  for (i = 0; i < MAX_PENDING_HTTP; i++)
    if (S.http[i].used && S.reply[i].seq != S.http[i].taken && S.reply[i].copied)
      free((void*)S.reply[i].body);
  free(S.pending_new);
  free(S.nq);
#ifdef DSC_CHANGED_RANGES
  free(S.ranges);
#endif
  free_chains();
  list_free();
  memset(&S, 0, sizeof(S));
  memset((void*)&Q, 0, sizeof(Q));
  memset((void*)&O, 0, sizeof(O));
}

#ifdef DSC_CHECK_FAST_STEPS
/* Tests: each writer's group starts a cache line, and the groups are in
 * the order the struct declares (P, slots, C, N): 0 if so. */
int dsc_test_layout(void)
{
#define OFF(f) ((uint32_t)offsetof(struct state_t, f))
  if (OFF(slot_head) % 32 || OFF(slots) % 32 || OFF(slot_tail) % 32 || OFF(reply) % 32 || OFF(end) % 32)
    return 1;
  if (!(OFF(game_hash) < OFF(slot_head) && OFF(pstats) < OFF(slots) && OFF(slots) < OFF(slot_tail) &&
        OFF(done) < OFF(reply) && OFF(http_taken) < OFF(reply) && OFF(http_answered) < OFF(end)))
    return 2;
  if (((uintptr_t)&S_storage) % 32 || ((uintptr_t)&O.grant7 - (uintptr_t)&O.req9) < 32)
    return 3;
  return 0;
#undef OFF
}

/* Tests: the owner adds an address to the table now (a new table published,
 * unused ones freed), as a live read during evaluation would. */
void dsc_test_republish(uint32_t address)
{
  note_missing(address);
  publish_pending_addresses();
  release_tables();
}
#endif

#ifdef DSC_TEST_HOOKS
void dsc_test_set_hooks(const dsc_test_hooks_t* hooks)
{
  if (hooks)
    T = *hooks;
  else
    memset(&T, 0, sizeof(T));
}

void* dsc_test_client(void)
{
  return S.client;
}
#endif
