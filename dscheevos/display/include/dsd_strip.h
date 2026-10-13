/* The bottom-edge notice strip (P2b, docs/display-library.md 11.1 and 11.4):
 * engine A's main-memory display mode on lines S..E at the bottom of the
 * screen, fed by a DSi NDMA from the strip buffer, shown only while the game
 * waits for VBlank and ended by any game IRQ.
 *
 * All hardware access goes through dsd_strip_hw_t, so the same logic runs
 * in the host simulator (tests) and on the DSi (lab ROM, later the module).
 * Not game-facing yet: nothing calls this from the module before the owner
 * rules on the residuals. */
#ifndef DSD_STRIP_H
#define DSD_STRIP_H

#include <stdint.h>

#define DSD_STRIP_K 2                 /* lines kept free before VBlank (exit at 191-K) */
#define DSD_STRIP_LAST (191 - DSD_STRIP_K)   /* last strip line */

typedef struct dsd_strip_hw_t {
  uint32_t (*vcount)(void);
  uint8_t (*mode_read)(void);         /* DISPCNT A bits 16-17 */
  void (*mode_write)(uint8_t mode);   /* a byte write to DISPCNT+2 keeping bits 18-23 */
  void (*fifo_start)(const uint16_t* src, uint32_t lines);   /* NDMA, startup 09h */
  void (*fifo_stop)(void);
  void (*counter_start)(uint32_t hblanks);   /* NDMA H-Blank counter, IRQ at the end */
  void (*counter_stop)(void);
  uint32_t (*game_irqs_pending)(void);       /* IE & IF & ~ours */
  int (*blocked)(void);               /* guards: card read, game DMA, GX FIFO busy, ARM7 save/wireless */
} dsd_strip_hw_t;

enum { DSD_STRIP_IDLE = 0, DSD_STRIP_ARMED, DSD_STRIP_LIVE };

typedef struct dsd_strip_stats_t {
  uint32_t armed, shown, aborted;
  uint32_t skip_not_waiting, skip_late, skip_pending, skip_blocked, skip_too_late_to_arm;
  uint32_t vblank_fixes;              /* mode 3 still on at VBlank, put back */
  uint32_t late_exits;                /* exit IRQ after the band's last line: margin rows shown */
} dsd_strip_stats_t;

typedef struct dsd_strip_t {
  const dsd_strip_hw_t* hw;
  const uint16_t* pixels;             /* 256 x rows BGR555, uncached or flushed */
  uint8_t rows;                       /* rows to show this frame, 0 = none */
  uint8_t state;
  uint8_t live_this_frame;            /* we set mode 3 this frame */
  uint8_t first;                      /* first strip line this frame */
  dsd_strip_stats_t stats;
} dsd_strip_t;

void dsd_strip_init(dsd_strip_t* s, const dsd_strip_hw_t* hw);

/* What to show from now on (rows = dsd_visible_rows; 0 hides). Idle time.
 * pixels holds rows + DSD_STRIP_K rows: the last K are margin (the strip's
 * background), fed past the band so a late exit shows our border, never a
 * FIFO underrun. */
void dsd_strip_set(dsd_strip_t* s, const uint16_t* pixels, int rows);

/* The game has just entered its VBlank wait (idle time). Arms the counter
 * so its IRQ comes at the end of the line before the strip. */
void dsd_strip_wait_entry(dsd_strip_t* s);

/* The counter's IRQ. in_wait: the interrupted code was the game's VBlank
 * wait (decided by the caller from the interrupted PC, not a flag). */
void dsd_strip_counter_irq(dsd_strip_t* s, int in_wait);

/* Any game IRQ (from the patched dispatcher), before the game's handler. */
void dsd_strip_game_irq(dsd_strip_t* s);

/* VBlank (the existing hook): never carry mode 3 into the next frame. */
void dsd_strip_vblank(dsd_strip_t* s);

#endif
