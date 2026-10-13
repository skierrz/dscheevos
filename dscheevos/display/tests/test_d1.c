/* Host tests for D1, the all-NDMA bottom strip (dsd_d1.c), on a line-by-line
 * model: the display, an H-Blank DMA writing DISPCNT from a table, the FIFO,
 * and a game that works, then waits for VBlank, with IRQs whose first act is
 * the abort (the wait gate's unpark path). The invariants are the round-2
 * design's: mode 3 only on the strip's lines and only while the game waits,
 * never at the next frame's top, and no game DISPCNT write ever lost. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd_d1.h"

static int checks, failures;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ---- the simulated machine */
static uint32_t line, dispcnt = 0x00010F00;  /* mode 1, BG0-3 on */
static const uint32_t* tab;
static uint32_t tab_n, tab_k, tab_base, fifo_index;
static int tab_on, fifo_on, game_working, hw_while_working;

static void touch(void) { if (game_working) hw_while_working++; }
static uint32_t hw_vcount(void) { return line; }
static uint32_t hw_dispcnt(void) { return dispcnt; }
static void hw_mode(uint8_t m) { dispcnt = (dispcnt & ~0x30000u) | (uint32_t)m << 16; }
static void hw_arm(const uint32_t* t, uint32_t n, uint32_t fi, const uint16_t* px, uint32_t lines) {
  (void)px; (void)lines; touch();
  tab = t; tab_n = n; tab_k = 0; tab_base = line; fifo_index = fi; tab_on = 1; fifo_on = 0;
}
static void hw_stop(void) { tab_on = 0; fifo_on = 0; }   /* the abort: allowed in a game IRQ */
static int blocked_now;
static int hw_blocked(void) { return blocked_now; }
static const dsd_d1_hw_t HW = { hw_vcount, hw_dispcnt, hw_mode, hw_arm, hw_stop, hw_blocked };

static const uint16_t pixels[256 * 40];
static dsd_d1_t S;

static void fresh(void) {
  dispcnt = 0x00010F00;
  tab_on = fifo_on = 0;
  blocked_now = 0;
  dsd_d1_init(&S, &HW);
}

typedef struct {
  int rows;
  int wait_at;                        /* line the game enters its wait (-1: never) */
  int irq_at;                         /* a game IRQ at the start of this line (-1: none) */
  int irq_writes_dispcnt;             /* its handler toggles BG1 (a game DISPCNT write) */
  int irq_wakes;                      /* it wakes a game thread, which then works... */
  int rewait_at;                      /* ...until it waits again at this line (-1: not this frame) */
  int blocked;
} frame_t;

typedef struct {
  int mode3[192];
  int mode3_working, mode3_outside, top_mode3, lost_write;
} result_t;

static void run_frame(const frame_t* f, result_t* r) {
  memset(r, 0, sizeof(*r));
  dsd_d1_set(&S, pixels, f->rows);
  blocked_now = f->blocked;
  uint32_t game_bits = dispcnt & ~0x30000u;     /* what the game last wrote, minus mode */
  int waiting = 0;
  for (int step = 0; step < 263; step++) {
    line = (uint32_t)((192 + step) % 263);
    if (step == 0) {                  /* VBlank IRQ: unpark (abort first), fail-safe, game works */
      if (waiting) dsd_d1_abort(&S);
      dsd_d1_vblank(&S);
      game_working = 1;
      waiting = 0;
    }
    if (f->wait_at >= 0 && (int)line == f->wait_at && game_working) {
      game_working = 0; waiting = 1;
      dsd_d1_wait_entry(&S);
    }
    if (f->irq_at >= 0 && (int)line == f->irq_at) {
      if (waiting) dsd_d1_abort(&S);  /* the gate's unpark path: our abort first */
      if (f->irq_writes_dispcnt) {    /* the game's handler: RMW of DISPCNT */
        dispcnt ^= 0x200;
        game_bits = dispcnt & ~0x30000u;
      }
      if (f->irq_wakes) { game_working = 1; waiting = 0; }
    }
    if (f->rewait_at >= 0 && (int)line == f->rewait_at && game_working) {
      game_working = 0; waiting = 1;
      dsd_d1_wait_entry(&S);
    }
    if (line < 192) {                 /* the line is drawn */
      const int m3 = (dispcnt >> 16 & 3) == 3;
      r->mode3[line] = m3;
      if (m3 && game_working) r->mode3_working++;
      if (m3 && (line < (uint32_t)(DSD_D1_LAST + 1 - 64) || line > DSD_D1_LAST)) r->mode3_outside++;
      if (m3 && line == 0) r->top_mode3++;
      if (fifo_on && game_working) r->mode3_working++;
      if ((dispcnt & ~0x30000u) != game_bits) r->lost_write++;
      /* its H-Blank: the table DMA writes one whole word */
      if (tab_on) {
        if (tab_k == fifo_index) fifo_on = 1;
        dispcnt = tab[tab_k++];
        if (tab_k == tab_n) tab_on = 0;
      }
    }
  }
  game_working = 0;
}

static int count3(const result_t* r, int* a, int* b) {
  int n = 0; *a = *b = -1;
  for (int l = 0; l < 192; l++) if (r->mode3[l]) { n++; if (*a < 0) *a = l; *b = l; }
  return n;
}

static void base(frame_t* f) {
  memset(f, 0, sizeof(*f));
  f->rows = 32; f->wait_at = 100; f->irq_at = -1; f->rewait_at = -1;
}

int main(void) {
  frame_t f; result_t r; int a, b;

  /* 1. The game waits from line 100: the strip is on 159-190 exactly, every frame. */
  fresh(); base(&f);
  for (int i = 0; i < 3; i++) {
    run_frame(&f, &r);
    CHECK(count3(&r, &a, &b) == 32 && a == 159 && b == 190);
    CHECK(!r.mode3_working && !r.mode3_outside && !r.top_mode3 && !r.lost_write);
  }
  CHECK(S.stats.armed == 3 && S.stats.aborted == 0 && S.stats.vblank_fixes == 0);
  CHECK(hw_while_working == 0);

  /* 2. Still working at the strip: no arming, no strip. */
  fresh(); base(&f); f.wait_at = 170;
  run_frame(&f, &r);
  CHECK(count3(&r, &a, &b) == 0 && S.stats.skip_late == 1);

  /* 3. A game IRQ mid-strip that writes DISPCNT and wakes the game: the strip
   * ends at that line, and the game's write is never lost. */
  fresh(); base(&f); f.irq_at = 170; f.irq_writes_dispcnt = 1; f.irq_wakes = 1;
  run_frame(&f, &r);
  CHECK(count3(&r, &a, &b) == 170 - 159 && b == 169);
  CHECK(!r.mode3_working && !r.lost_write && S.stats.aborted == 1);
  run_frame(&f, &r);                  /* and the next frame starts clean */
  CHECK(!r.top_mode3 && !r.lost_write);

  /* 4. A game IRQ while armed, before the strip: disarmed; the game re-waits
   * at 120 and the strip is re-armed for this frame. */
  fresh(); base(&f); f.irq_at = 110; f.irq_writes_dispcnt = 1; f.irq_wakes = 1; f.rewait_at = 120;
  run_frame(&f, &r);
  CHECK(count3(&r, &a, &b) == 32 && a == 159);
  CHECK(!r.lost_write && !r.mode3_working);

  /* 5. A guard says no (card read in flight): nothing armed. */
  fresh(); base(&f); f.blocked = 1;
  run_frame(&f, &r);
  CHECK(count3(&r, &a, &b) == 0 && S.stats.skip_blocked == 1);

  /* 6. Short notices (the in/out animation): 8 rows sit at 183-190. */
  fresh(); base(&f); f.rows = 8;
  run_frame(&f, &r);
  CHECK(count3(&r, &a, &b) == 8 && a == 183 && b == 190);

  /* 7. The game never waits this frame: nothing. */
  fresh(); base(&f); f.wait_at = -1;
  run_frame(&f, &r);
  CHECK(count3(&r, &a, &b) == 0);

  /* Fuzz: random frames, the invariants hold every time. */
  fresh();
  srand(4321);
  long bad_work = 0, bad_out = 0, bad_top = 0, lost = 0, shown = 0;
  for (int i = 0; i < 20000; i++) {
    base(&f);
    f.rows = rand() % 5 == 0 ? 0 : 1 + rand() % 32;
    f.wait_at = rand() % 8 == 0 ? -1 : rand() % 263;
    if (rand() % 3 == 0) {
      f.irq_at = rand() % 263;
      f.irq_writes_dispcnt = rand() % 2;
      f.irq_wakes = rand() % 2;
      f.rewait_at = f.irq_wakes && rand() % 2 ? (f.irq_at + 1 + rand() % 40) % 263 : -1;
    }
    f.blocked = rand() % 25 == 0;
    run_frame(&f, &r);
    bad_work += r.mode3_working; bad_out += r.mode3_outside; bad_top += r.top_mode3; lost += r.lost_write;
    for (int l = 0; l < 192; l++) if (r.mode3[l]) { shown++; break; }
  }
  printf("  fuzz: strip on %ld of 20000 frames; aborts %lu, vblank fixes %lu\n",
         shown, (unsigned long)S.stats.aborted, (unsigned long)S.stats.vblank_fixes);
  CHECK(bad_work == 0);               /* never live while the game works */
  CHECK(bad_out == 0);                /* only on the strip's lines */
  CHECK(bad_top == 0);                /* never carried into the next frame */
  CHECK(lost == 0);                   /* no game DISPCNT write lost */
  CHECK(shown > 3000);
  CHECK(hw_while_working == 0);       /* no hardware work of ours in the game's time but the abort */

  printf("d1: %d checks, %d failed\n", checks, failures);
  return failures != 0;
}
