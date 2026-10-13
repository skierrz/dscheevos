/* D1: the all-NDMA bottom strip (docs/display-research/round2-synthesis.md).
 * Armed while the game waits for VBlank; then nothing of ours runs. An
 * H-Blank NDMA writes whole DISPCNT words from a table built here (the
 * game's own value captured at arming, with mode 3 on the strip's lines), and
 * the FIFO NDMA feeds the strip. The first thing any game IRQ does is the
 * abort (stop the DMAs, mode 1), so the game never sees mode 3 and never
 * loses a DISPCNT write. Hardware access goes through dsd_d1_hw_t: the lab
 * ROM and, later, the module implement it; tests run it on a simulator.
 * Not wired into the module before the owner's rulings. */
#ifndef DSD_D1_H
#define DSD_D1_H

#include <stdint.h>

#define DSD_D1_LAST 190               /* last strip line; line 191 is back to mode 1 */

typedef struct dsd_d1_hw_t {
  uint32_t (*vcount)(void);
  uint32_t (*dispcnt_read)(void);
  void (*mode_write)(uint8_t mode);   /* byte write to DISPCNT+2 keeping bits 18-23 */
  /* Program and start the DMAs: the table channel writes table[i] to DISPCNT
   * at the H-Blank ending line vcount_at_arm + i, for n entries; the FIFO
   * feed of `lines` rows from pixels starts at the H-Blank of entry
   * fifo_index (by a starter channel, or enabled now where the FIFO only
   * drains in mode 3). Called with IRQs masked. */
  void (*arm)(const uint32_t* table, uint32_t n, uint32_t fifo_index,
              const uint16_t* pixels, uint32_t lines);
  /* Stop every channel; return only once no table write can still land. */
  void (*stop)(void);
  int (*blocked)(void);               /* card read in flight, CE9 NDMA busy, ... */
} dsd_d1_hw_t;

enum { DSD_D1_IDLE = 0, DSD_D1_ARMED };

typedef struct dsd_d1_stats_t {
  uint32_t armed, aborted, vblank_fixes;
  uint32_t skip_late, skip_blocked, skip_nothing;
} dsd_d1_stats_t;

typedef struct dsd_d1_t {
  const dsd_d1_hw_t* hw;
  const uint16_t* pixels;             /* rows + 2 margin rows, BGR555, DMA-visible */
  uint8_t rows;
  uint8_t state;
  uint8_t armed_this_frame;
  uint32_t table[192];                /* DISPCNT per H-Blank from the arming line */
  dsd_d1_stats_t stats;
} dsd_d1_t;

void dsd_d1_init(dsd_d1_t* s, const dsd_d1_hw_t* hw);
void dsd_d1_set(dsd_d1_t* s, const uint16_t* pixels, int rows);   /* idle time */

/* The game has entered its VBlank wait (IRQs masked: the wait gate's entry). */
void dsd_d1_wait_entry(dsd_d1_t* s);

/* The first thing on any game IRQ while the game was waiting (the gate's
 * unpark path), before anything else runs. */
void dsd_d1_abort(dsd_d1_t* s);

/* VBlank fail-safe (the existing VBlank hook): never carry mode 3 into the
 * next frame. Only touches a mode 3 we set. */
void dsd_d1_vblank(dsd_d1_t* s);

#endif
