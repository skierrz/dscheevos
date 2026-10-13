/* Host tests for the bottom-edge strip (dsd_strip.c) on a line-by-line
 * model of the DS display and of a game that works, then waits for VBlank.
 * The invariants are the zero-lag argument of docs/display-library.md 11:
 * the strip is only live while the game waits, only on its own lines, and
 * never carried past VBlank. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd_strip.h"

static int checks, failures;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ---- the simulated machine */

static uint32_t line;                 /* VCOUNT */
static uint8_t mode = 1;              /* DISPCNT A display mode */
static int fifo_on, counter_left;     /* counter_left > 0: H-Blanks to go */
static uint32_t pending;              /* game IRQs raised but not yet taken */
static int blocked_now;
static int game_working;              /* the game's frame work is running */
static int hw_calls_while_working;    /* our hardware access during the game's work */

static void touch(void) { if (game_working) hw_calls_while_working++; }
static uint32_t hw_vcount(void) { return line; }
static uint8_t hw_mode_read(void) { touch(); return mode; }
static void hw_mode_write(uint8_t m) { touch(); mode = m; }
static void hw_fifo_start(const uint16_t* src, uint32_t lines) { (void)src; (void)lines; touch(); fifo_on = 1; }
static void hw_fifo_stop(void) { touch(); fifo_on = 0; }
static void hw_counter_start(uint32_t n) { touch(); counter_left = (int)n; }
static void hw_counter_stop(void) { counter_left = 0; }   /* disarm: allowed in the game's IRQ */
static uint32_t hw_pending(void) { return pending; }
static int hw_blocked(void) { return blocked_now; }

static const dsd_strip_hw_t HW = { hw_vcount, hw_mode_read, hw_mode_write, hw_fifo_start, hw_fifo_stop,
                                   hw_counter_start, hw_counter_stop, hw_pending, hw_blocked };

static const uint16_t pixels[256 * 64];

/* ---- one frame */

typedef struct {
  int rows;                           /* strip rows wanted (0 = none) */
  int wait_at;                        /* line the game enters its wait (-1: never this frame) */
  int game_irq_at;                    /* a game IRQ at the start of this line (-1: none) */
  int irq_wakes_game;                 /* that IRQ wakes a game thread (it works again) */
  int pending_at_entry;               /* a game IRQ is pending when the entry IRQ comes */
  int blocked;                        /* a guard says no */
  int counter_late;                   /* deliver the entry IRQ this many lines late */
  int exit_late;                      /* deliver the exit IRQ this many lines late */
} frame_t;

typedef struct {
  int mode3_lines[192];               /* lines drawn in mode 3 */
  int mode3_while_working;
  int fifo_while_working;
  int mode3_outside;                  /* mode 3 on a line outside the strip's band */
} result_t;

static dsd_strip_t S;

/* A fresh machine and strip for each scenario. */
static void fresh(void) {
  mode = 1;
  fifo_on = counter_left = 0;
  pending = 0;
  dsd_strip_init(&S, &HW);
}

static void run_frame(const frame_t* f, result_t* r) {
  memset(r, 0, sizeof(*r));
  dsd_strip_set(&S, pixels, f->rows);
  int in_wait = 0;
  int late_irq_at = -1;               /* a counter IRQ held back by f->counter_late */
  pending = 0;
  blocked_now = f->blocked;
  /* The frame starts at VBlank (line 192): the game works until wait_at. */
  for (int step = 0; step < 263; step++) {
    line = (uint32_t)((192 + step) % 263);
    if (step == 0) {
      dsd_strip_vblank(&S);
      game_working = 1;
      in_wait = 0;
    }
    /* the game enters its wait at the start of this line */
    if (f->wait_at >= 0 && (int)line == f->wait_at && game_working) {
      game_working = 0;
      in_wait = 1;
      dsd_strip_wait_entry(&S);
    }
    /* a game IRQ at the start of the line */
    if (f->game_irq_at >= 0 && (int)line == f->game_irq_at) {
      dsd_strip_game_irq(&S);
      if (f->irq_wakes_game) { game_working = 1; in_wait = 0; }
    }
    /* a late counter IRQ arrives now */
    if (late_irq_at >= 0 && step == late_irq_at) {
      pending = f->pending_at_entry && S.state == DSD_STRIP_ARMED;
      dsd_strip_counter_irq(&S, in_wait && !game_working);
      pending = 0;
      late_irq_at = -1;
    }
    /* the line is drawn */
    if (line < 192) {
      r->mode3_lines[line] = mode == 3;
      if (mode == 3 && game_working) r->mode3_while_working++;
      /* the band, plus K margin lines a late exit may show (our background) */
      if (mode == 3 && (line < DSD_STRIP_LAST + 1 - 64 || line > DSD_STRIP_LAST + DSD_STRIP_K)) r->mode3_outside++;
    }
    if (fifo_on && game_working) r->fifo_while_working++;
    /* its H-Blank: the counter counts visible lines only */
    if (line < 192 && counter_left > 0 && --counter_left == 0) {
      const int late = S.state == DSD_STRIP_ARMED ? f->counter_late : f->exit_late;
      if (late) {
        late_irq_at = step + late;
      } else {
        pending = f->pending_at_entry && S.state == DSD_STRIP_ARMED;
        dsd_strip_counter_irq(&S, in_wait && !game_working);
        pending = 0;
      }
    }
  }
  game_working = 0;
}

static int count_mode3(const result_t* r, int* first, int* last) {
  int n = 0;
  *first = -1;
  *last = -1;
  for (int l = 0; l < 192; l++)
    if (r->mode3_lines[l]) { n++; if (*first < 0) *first = l; *last = l; }
  return n;
}

static void base(frame_t* f) {
  memset(f, 0, sizeof(*f));
  f->rows = 32;
  f->wait_at = 100;
  f->game_irq_at = -1;
}

int main(void) {
  frame_t f;
  result_t r;
  int a, b;
  fresh();

  /* 1. The game waits early: the strip is on lines 158-189 exactly. */
  base(&f);
  run_frame(&f, &r);
  CHECK(count_mode3(&r, &a, &b) == 32 && a == DSD_STRIP_LAST - 31 && b == DSD_STRIP_LAST);
  CHECK(!r.mode3_while_working && !r.fifo_while_working && !r.mode3_outside);
  CHECK(S.stats.shown == 1 && mode == 1 && !fifo_on && S.state == DSD_STRIP_IDLE);

  /* 2. The game works past the strip's start: nothing armed, nothing shown. */
  fresh();
  base(&f);
  f.wait_at = 170;
  run_frame(&f, &r);
  CHECK(count_mode3(&r, &a, &b) == 0);
  CHECK(S.stats.skip_too_late_to_arm == 1 && S.stats.armed == 0);

  /* 3. Waits just in time (line 155 for a strip from 158). */
  fresh();
  base(&f);
  f.wait_at = 155;
  run_frame(&f, &r);
  CHECK(count_mode3(&r, &a, &b) == 32 && a == 158);

  /* 4. A game IRQ during the strip ends it at once. */
  fresh();
  base(&f);
  f.game_irq_at = 170;
  f.irq_wakes_game = 1;
  run_frame(&f, &r);
  CHECK(count_mode3(&r, &a, &b) == 170 - 158 && a == 158 && b == 169);
  CHECK(S.stats.aborted == 1 && !r.mode3_while_working && !r.fifo_while_working);

  /* 5. A game IRQ while armed (before the strip): disarmed, nothing shown. */
  fresh();
  base(&f);
  f.game_irq_at = 120;
  run_frame(&f, &r);
  CHECK(count_mode3(&r, &a, &b) == 0 && S.stats.armed == 1 && S.stats.shown == 0);
  CHECK(counter_left == 0);

  /* 6. A game IRQ pending at entry: skipped. 7. A guard says no: skipped. */
  fresh();
  base(&f);
  f.pending_at_entry = 1;
  run_frame(&f, &r);
  CHECK(count_mode3(&r, &a, &b) == 0 && S.stats.skip_pending == 1);
  base(&f);
  f.blocked = 1;
  run_frame(&f, &r);
  CHECK(count_mode3(&r, &a, &b) == 0 && S.stats.skip_blocked == 1);

  /* 8. The game woke up (working) when the entry IRQ came: skipped. */
  fresh();
  base(&f);
  f.game_irq_at = 140;
  f.irq_wakes_game = 1;               /* wakes it while armed: disarmed here already */
  run_frame(&f, &r);
  CHECK(count_mode3(&r, &a, &b) == 0 && !r.mode3_while_working);

  /* 9. An animating notice: 8 rows sit at the bottom of the band. */
  fresh();
  base(&f);
  f.rows = 8;
  run_frame(&f, &r);
  CHECK(count_mode3(&r, &a, &b) == 8 && a == DSD_STRIP_LAST - 7 && b == DSD_STRIP_LAST);

  /* 10. The entry IRQ comes once the first strip line is drawing: a switch
   * would tear it, so the frame is skipped. */
  fresh();
  base(&f);
  f.counter_late = 1;
  run_frame(&f, &r);
  CHECK(count_mode3(&r, &a, &b) == 0 && S.stats.skip_late == 1);

  /* 11. The exit IRQ is late: the margin rows show (still before VBlank). */
  fresh();
  base(&f);
  f.exit_late = 2;
  run_frame(&f, &r);
  /* IRQ 2 lines late arrives at the start of line LAST+2: one margin line shown */
  CHECK(count_mode3(&r, &a, &b) == 33 && b == DSD_STRIP_LAST + 1 && S.stats.late_exits == 1);
  CHECK(!r.mode3_outside);

  /* 12. Exit held back past VBlank: the VBlank hook puts mode 1 back. */
  fresh();
  base(&f);
  f.exit_late = 6;
  run_frame(&f, &r);
  run_frame(&f, &r);                  /* starts with the VBlank hook */
  CHECK(S.stats.vblank_fixes >= 1);
  CHECK(!r.mode3_lines[0] && !r.mode3_while_working);

  /* Fuzz: random frames; the invariants must hold every time. */
  fresh();
  srand(12345);
  int bad_working = 0, bad_outside = 0, bad_carry = 0, shown = 0;
  for (int i = 0; i < 20000; i++) {
    base(&f);
    f.rows = (rand() % 5 == 0) ? 0 : 1 + rand() % 32;
    const int w = rand() % 263;
    f.wait_at = rand() % 10 == 0 ? -1 : w;
    f.game_irq_at = rand() % 3 == 0 ? rand() % 263 : -1;
    f.irq_wakes_game = rand() % 2;
    f.pending_at_entry = rand() % 20 == 0;
    f.blocked = rand() % 20 == 0;
    f.counter_late = rand() % 10 == 0 ? rand() % 3 : 0;
    f.exit_late = rand() % 10 == 0 ? rand() % 3 : 0;   /* up to K lines */
    run_frame(&f, &r);
    if ((r.mode3_while_working || r.fifo_while_working || r.mode3_outside || r.mode3_lines[0]) && !getenv("QUIET")) {
      static int shown_bad;
      if (shown_bad++ < 3)
        printf("  bad frame %d: rows %d wait %d irq %d wakes %d pend %d blk %d late %d/%d: working %d fifo %d outside %d carry %d\n",
               i, f.rows, f.wait_at, f.game_irq_at, f.irq_wakes_game, f.pending_at_entry, f.blocked,
               f.counter_late, f.exit_late, r.mode3_while_working, r.fifo_while_working, r.mode3_outside, r.mode3_lines[0]);
    }
    bad_working += r.mode3_while_working + r.fifo_while_working;
    bad_outside += r.mode3_outside;
    bad_carry += r.mode3_lines[0];
    for (int l = 0; l < 192; l++) if (r.mode3_lines[l]) { shown++; break; }
  }
  printf("  fuzz: strip shown on %d of 20000 frames; aborts %lu, vblank fixes %lu\n",
         shown, (unsigned long)S.stats.aborted, (unsigned long)S.stats.vblank_fixes);
  CHECK(bad_working == 0);            /* never live while the game works */
  CHECK(bad_outside == 0);            /* never outside the bottom band */
  CHECK(bad_carry == 0);              /* never carried into the next frame's top */
  CHECK(shown > 2000);
  CHECK(hw_calls_while_working == 0); /* our hardware work never runs in the game's time */

  printf("strip: %d checks, %d failed\n", checks, failures);
  return failures != 0;
}
