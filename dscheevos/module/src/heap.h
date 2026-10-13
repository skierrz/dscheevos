/*
 * The module's memory: one heap in two pieces around the session area, which
 * becomes the snapshot arena once the game's set is loaded.
 *
 *   __bss_end .. DSCM_SESSION          heap, low piece (what the image leaves)
 *   DSCM_SESSION .. +HEAP_SESSION_HEAD session header (dscm_boot_t; the PMK,
 *                                      the placard's flags): kept as is
 *   HEAP_ARENA .. HEAP_ZONE_END        snapshot arena. Until the set is
 *                                      loaded it holds the session file's
 *                                      replies, and rc_api's parsed copy of
 *                                      the set: the arena is not written
 *                                      before the first frame.
 *   HEAP_ZONE_END .. DSCM_HEAP_END     heap, high piece
 *
 * newlib's malloc takes the two heap pieces from _sbrk (heap.c), the second
 * as a "foreign sbrk" gap. heap.c also wraps malloc and free (link-time
 * --wrap) to put three load-time things in the arena's place:
 *   - the arena itself: the core's malloc of HEAP_ARENA_BYTES in dsc_init;
 *   - a preload reply's copy: the core copies every reply into a malloc'd
 *     buffer; a reply in the session is lent in place instead (heap_lend);
 *   - rc_api's parsed set (rc_api_process_fetch_game_sets_server_response):
 *     bump-allocated after the session file, freed before the load ends.
 * So the set's raw and parsed JSON cost no heap: load peak = steady state,
 * as long as the session file plus the parsed set fit in the arena.
 *
 * Every other allocation of the load goes to a set zone (heap.c), in
 * allocation order, which is the order rcheevos evaluates in: first DSi
 * NWRAM-B/C when nds-bootstrap grants it (heap_zone_nwram; its line fills
 * cost about half of main RAM's), then the bottom of the high heap piece,
 * whose rest newlib then gets. The parsed set's data is never interleaved
 * with newlib's headers or with later allocations. Blocks the load freed are
 * reused by the load; after it, nothing new goes to a zone.
 */
#ifndef DSCM_HEAP_H
#define DSCM_HEAP_H

#include "dscheevos_module.h"
#include <stdint.h>

#define HEAP_SESSION_HEAD 0x400u                         /* dscm_boot_t, rounded up */
#define HEAP_ARENA        (DSCM_SESSION + HEAP_SESSION_HEAD)
#define HEAP_ARENA_BYTES  (256u * 1024u)                 /* config.snapshot_bytes */
#define HEAP_ZONE_END     (HEAP_ARENA + HEAP_ARENA_BYTES)

/* Load-time placement. file_size: the session file's (checked by the caller). */
void heap_load_begin(uint32_t file_size);
/* Around dsc_init: its malloc of HEAP_ARENA_BYTES is the arena. */
void heap_arena_window(int open);
/* Lend body (in the session, body[len] a NUL the file has spare) to the
 * next malloc(len + 1): the core's copy of a reply. NULL: stop lending. */
void heap_lend(const char* body, uint32_t len);
/* Before heap_load_begin: memory of its own for the set (DSi NWRAM-B/C,
 * see dscm_boot_t), or size 0 for none. */
void heap_zone_nwram(void* base, uint32_t size);
/* The next malloc(n) is short-lived (a reply's copy that can't be lent):
 * newlib's, not a set zone's. 0: none. */
void heap_transient(uint32_t n);
/* Around allocations another party finds by address range (the achievement
 * list, which the in-game menu checks against the module's RAM): newlib's. */
void heap_shared(int on);
/* The load is over: nothing load-time may stay in the arena. Returns 0 if
 * nothing does, else -1 (the arena would overwrite it: don't start). */
int heap_load_end(void);

/* For tests and the RAM dump (tests/bootstrap/status.py reads it by name):
 * sbrk high-water marks per piece, the arena's use while loading. */
typedef struct dscm_heap_t {
  uint32_t low_base, low_top;         /* low piece: start, high-water */
  uint32_t high_base, high_top;       /* high piece: start (0 if unused), high-water */
  uint32_t load_low_top, load_high_top; /* the same at the end of the load */
  uint32_t load_in_use;               /* malloc'd bytes then (newlib mallinfo) */
  uint32_t arena;                     /* where the snapshot arena went */
  uint32_t lent;                      /* reply bytes lent in place (not copied) */
  uint32_t parse_base, parse_top;     /* rc_api's parsed set in the arena: start, high-water */
  uint32_t parse_heap;                /* bytes of it that didn't fit there (heap) */
  uint32_t load_left;                 /* load-time blocks still alive at the end (0) */
  /* set zones, [0] NWRAM (0s if none), [1] main RAM (bottom of the high piece) */
  uint32_t zone_base[2], zone_top[2];  /* start, high-water */
  uint32_t zone_end[2];               /* NWRAM: the grant's end; main RAM: 0 */
  uint32_t zone_used[2], zone_holes[2]; /* at the end of the load: bytes in blocks, bytes freed below the top */
  uint32_t zone_blocks;               /* blocks in use (now) */
  uint32_t zone_missed;               /* load-time bytes no zone had room for (newlib's) */
  uint32_t nwram_granted;             /* bytes nds-bootstrap granted (0: none); zone_end[0] 0 if they failed the check */
  uint32_t nwram_mpu;                 /* under the game's MPU, at the first frame: NWRAM_MPU_* */
} dscm_heap_t;
extern dscm_heap_t dscm_g_heap;
#define NWRAM_MPU_SEEN     0x01u      /* checked */
#define NWRAM_MPU_RW       0x02u      /* data read and write allowed */
#define NWRAM_MPU_DCACHE   0x04u
#define NWRAM_MPU_BUFFER   0x08u      /* write-back */
#define NWRAM_MPU_REGION_SHIFT 8      /* the region that maps it, 0xF00 if none */

#endif
