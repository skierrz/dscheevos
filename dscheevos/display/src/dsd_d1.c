/* D1: the all-NDMA bottom strip. See dsd_d1.h. */
#include "dsd_d1.h"

void dsd_d1_init(dsd_d1_t* s, const dsd_d1_hw_t* hw) {
  const dsd_d1_stats_t zero = { 0 };
  s->hw = hw;
  s->pixels = 0;
  s->rows = 0;
  s->state = DSD_D1_IDLE;
  s->armed_this_frame = 0;
  s->stats = zero;
}

void dsd_d1_set(dsd_d1_t* s, const uint16_t* pixels, int rows) {
  s->pixels = pixels;
  s->rows = (uint8_t)(rows < 0 ? 0 : rows > 64 ? 64 : rows);
}

void dsd_d1_wait_entry(dsd_d1_t* s) {
  if (s->state != DSD_D1_IDLE) return;
  if (!s->rows || !s->pixels) { s->stats.skip_nothing++; return; }
  const dsd_d1_hw_t* hw = s->hw;
  const uint32_t first = DSD_D1_LAST + 1 - s->rows;   /* first strip line */
  const uint32_t vc = hw->vcount();
  /* The table's first write lands at the H-Blank of line vc. Arm only with
   * at least two lines to spare before the strip's first line. */
  if (vc >= 192 || vc + 2 > first) { s->stats.skip_late++; return; }
  if (hw->blocked()) { s->stats.skip_blocked++; return; }

  /* The game is idle: its DISPCNT can't change until an IRQ, and every
   * game IRQ aborts first. So the captured value stays correct. */
  const uint32_t base = (hw->dispcnt_read() & ~0x30000u) | 0x10000u;
  const uint32_t n = DSD_D1_LAST + 1 - vc;            /* H-Blanks of lines vc..190 */
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t line = vc + i + 1;                 /* the line this write affects */
    s->table[i] = line >= first && line <= DSD_D1_LAST ? base | 0x30000u : base;
  }
  /* The FIFO starts at the H-Blank before the first strip line; 2 margin
   * rows are fed past the strip so a late switch shows our background. */
  hw->arm(s->table, n, first - 1 - vc, s->pixels, s->rows + 2u);
  s->state = DSD_D1_ARMED;
  s->armed_this_frame = 1;
  s->stats.armed++;
}

void dsd_d1_abort(dsd_d1_t* s) {
  if (s->state == DSD_D1_IDLE) return;                /* one compare when nothing is armed */
  const dsd_d1_hw_t* hw = s->hw;
  hw->stop();
  if ((hw->dispcnt_read() >> 16 & 3) == 3) hw->mode_write(1);
  s->state = DSD_D1_IDLE;
  if (hw->vcount() <= DSD_D1_LAST) s->stats.aborted++;   /* VBlank ends a finished strip: not an abort */
}

void dsd_d1_vblank(dsd_d1_t* s) {
  const dsd_d1_hw_t* hw = s->hw;
  if (s->state != DSD_D1_IDLE) {
    hw->stop();
    s->state = DSD_D1_IDLE;
  }
  if (s->armed_this_frame && (hw->dispcnt_read() >> 16 & 3) == 3) {
    hw->mode_write(1);
    s->stats.vblank_fixes++;
  }
  s->armed_this_frame = 0;
}
