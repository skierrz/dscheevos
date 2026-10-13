/* The bottom-edge notice strip. See dsd_strip.h. Every path but
 * dsd_strip_game_irq's disarm runs while the game waits for VBlank. */
#include "dsd_strip.h"

void dsd_strip_init(dsd_strip_t* s, const dsd_strip_hw_t* hw) {
  dsd_strip_stats_t zero = { 0 };
  s->hw = hw;
  s->pixels = 0;
  s->rows = 0;
  s->state = DSD_STRIP_IDLE;
  s->live_this_frame = 0;
  s->first = 0;
  s->stats = zero;
}

void dsd_strip_set(dsd_strip_t* s, const uint16_t* pixels, int rows) {
  s->pixels = pixels;
  s->rows = (uint8_t)(rows < 0 ? 0 : rows > 64 ? 64 : rows);
}

void dsd_strip_wait_entry(dsd_strip_t* s) {
  if (s->state != DSD_STRIP_IDLE || !s->rows || !s->pixels) return;
  const uint32_t first = DSD_STRIP_LAST + 1 - s->rows;
  const uint32_t vc = s->hw->vcount();
  /* The counter must fire at the end of line first-1, so arm before that line. */
  if (vc + 2 > first) {
    s->stats.skip_too_late_to_arm++;
    return;
  }
  s->first = (uint8_t)first;
  s->state = DSD_STRIP_ARMED;
  s->stats.armed++;
  /* H-Blanks from line vc (not yet passed) to first-1: first - vc of them. */
  s->hw->counter_start(first - vc);
}

void dsd_strip_counter_irq(dsd_strip_t* s, int in_wait) {
  const dsd_strip_hw_t* hw = s->hw;
  if (s->state == DSD_STRIP_ARMED) {
    const uint32_t vc = hw->vcount();
    s->state = DSD_STRIP_IDLE;
    /* Entry: only while the game waits, on time, with nothing pending. */
    if (!in_wait) { s->stats.skip_not_waiting++; return; }
    /* On time means still in the H-Blank before the first strip line; once
     * that line is drawing, a switch would tear it. */
    if (vc + 1 != s->first) { s->stats.skip_late++; return; }
    if (hw->game_irqs_pending()) { s->stats.skip_pending++; return; }
    if (hw->blocked()) { s->stats.skip_blocked++; return; }
    const uint32_t rows = s->rows;
    hw->fifo_start(s->pixels, rows + DSD_STRIP_K);   /* + margin rows */
    hw->mode_write(3);
    s->live_this_frame = 1;
    s->state = DSD_STRIP_LIVE;
    s->stats.shown++;
    hw->counter_start(rows);          /* IRQ at the end of the last strip line */
  } else if (s->state == DSD_STRIP_LIVE) {
    if (hw->mode_read() == 3) hw->mode_write(1);
    hw->fifo_stop();
    if (hw->vcount() > DSD_STRIP_LAST) s->stats.late_exits++;
    s->state = DSD_STRIP_IDLE;
  }
}

void dsd_strip_game_irq(dsd_strip_t* s) {
  if (s->state == DSD_STRIP_IDLE) return;     /* the common case: one compare */
  const dsd_strip_hw_t* hw = s->hw;
  if (s->state == DSD_STRIP_LIVE) {
    if (hw->mode_read() == 3) hw->mode_write(1);
    hw->fifo_stop();
    s->stats.aborted++;
  }
  hw->counter_stop();
  s->state = DSD_STRIP_IDLE;
}

void dsd_strip_vblank(dsd_strip_t* s) {
  const dsd_strip_hw_t* hw = s->hw;
  if (s->live_this_frame && hw->mode_read() == 3) {
    hw->mode_write(1);
    hw->fifo_stop();
    s->stats.vblank_fixes++;
  }
  if (s->state != DSD_STRIP_IDLE) {
    hw->counter_stop();
    s->state = DSD_STRIP_IDLE;
  }
  s->live_this_frame = 0;
}
