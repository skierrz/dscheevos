/* LAB ONLY: the bottom notice strip in the module (strip.c, STRIP=1). */
#ifndef DSCM_STRIP_H
#define DSCM_STRIP_H

#include <stdint.h>

typedef struct dscm_strip_stats_t {
  uint32_t arms, wakes_done, wakes_cut, wakes_before, mode_fixed;
  uint32_t skip_late, skip_dispcnt, skip_busy, skip_mode;
  uint32_t frames_armed, frames_full, frames_cut, frames_none, frames_unarmed;
  uint32_t renders, arms_vblank, stand_downs, frames_down, skip_gamedma;
  uint32_t frames_learning, learn_frames, picks, pick_first, pick_score, no_place;
  uint32_t resumes, gap_lines, frames_resumed;
} dscm_strip_stats_t;

extern dscm_strip_stats_t dscm_strip_stats;

/* edge: 0 auto, 1 top half, 2 bottom half (the player's NOTICE_EDGE) */
#ifndef STRIP_EDGE_DEFAULT
#define STRIP_EDGE_DEFAULT 0
#endif
void dscm_strip_init(int hardcore, int edge);
void dscm_strip_frame(uint32_t frame);   /* once a frame, idle pass, IRQs on */
int dscm_strip_active(void);
void dscm_strip_arm(uint32_t vblanks);   /* last halt, IRQs masked, before WFI */
void dscm_strip_wake(uint32_t vblanks);  /* after WFI, IRQs masked */
/* LAB: the strip's counters in the status' IRQ fields (unused, always 0 in this module), so the
 * hardware kit's status log (pending.bin) carries them: irq_lines_total = frames with a full strip,
 * irq_lines_max = frames armed, irq_lines_last = frames cut. */
struct dscm_status_t;
void dscm_strip_publish(struct dscm_status_t* st);

#endif
