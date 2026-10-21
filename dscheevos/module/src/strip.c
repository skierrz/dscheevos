/* LAB ONLY (branch strip-lab, never in a kit): the bottom notice strip in the
 * module, to see it in real games in melonDS. Built with STRIP=1.
 *
 * The display library (dscheevos/display) picks what to show and draws it;
 * this file moves the pixels with the D1 scheme (round2-synthesis.md), with
 * one change: no code in the game's IRQ path. The strip is armed only in the
 * module's last halt of an idle pass, which runs with IRQs masked (CPSR.I),
 * and stopped the moment the CPU wakes, still masked, before the game's
 * handler can run. So game code never runs while DISPCNT holds our mode.
 *
 *   NDMA3: on each H-Blank from the arming line, a whole DISPCNT word from
 *          dtab (the game's word; mode 3 on the strip's lines E..190).
 *   NDMA1: on each H-Blank up to line E-1, a word into NDMA2's control from
 *          btab (0, then the FIFO enable at E-1): starts the feed with no CPU.
 *   NDMA2: the main-memory display FIFO feed (startup 09h).
 * NDMA0 is nds-bootstrap's card engine: never touched, and the strip isn't
 * armed while NDMA0 or NDMA1 is busy.
 *
 * The tables are built in idle passes (IRQs on), so arming is a few register
 * writes with IRQs masked. A wake while armed stops all three channels and
 * puts the game's mode back. */
#include <stdint.h>
#include <string.h>
#include "dscheevos_module.h"
#include "dsd.h"
#include "dsd_badge.h"
#include "strip.h"

#define REG32(a) (*(volatile uint32_t*)(a))
#define REG16(a) (*(volatile uint16_t*)(a))
#define REG8(a)  (*(volatile uint8_t*)(a))
#define REG_VCOUNT    REG16(0x04000006)
#define DISPCNT_A     REG32(0x04000000)
#define DISPCNT_MODE  REG8(0x04000002)
#define MMEM_FIFO     0x04000068u
#define NDMA_SAD(x)   REG32(0x04004104 + (x) * 0x1C)
#define NDMA_DAD(x)   REG32(0x04004108 + (x) * 0x1C)
#define NDMA_TCNT(x)  REG32(0x0400410C + (x) * 0x1C)
#define NDMA_WCNT(x)  REG32(0x04004110 + (x) * 0x1C)
#define NDMA_BCNT(x)  REG32(0x04004114 + (x) * 0x1C)
#define NDMA_CNT(x)   REG32(0x0400411C + (x) * 0x1C)
#define NDMA_ON       (1u << 31)
#define NDMA_START(m) ((uint32_t)(m) << 24)
#define NDMA_DFIX     (2u << 10)
#define NDMA_PHYS(n)  ((uint32_t)(n) << 16)

#define CH_FIFO 2
#define CH_TABLE 3
#define CH_START 1
#define ROWS_MAX 32
#define MARGIN 2                      /* rows fed past the strip: a late switch shows our background */
#define LAST 190                      /* last strip line; 191 is the game's again */

extern const dsd_font_t dsd_font_latin;
extern dscm_notices_t dscm_g_notices;
extern void dscm_dc_clean(const void* addr, uint32_t len);

static dsd_t g_dsd;
static uint16_t g_px[(ROWS_MAX + MARGIN) * 256] __attribute__((aligned(32)));
static uint32_t g_dtab[192] __attribute__((aligned(32)));
static uint32_t g_btab[192] __attribute__((aligned(32)));

/* Read in the masked halt: kept in one place. */
static struct {
  uint32_t base;                      /* the game's DISPCNT the tables were built for (mode 1) */
  uint8_t rows;                       /* strip lines now, 0 = nothing to show */
  uint8_t first;                      /* first strip line E = 191 - rows */
  uint8_t armed;
  uint8_t want_base;                  /* the game's DISPCNT changed: rebuild in the next pass */
  uint32_t frame_flags;               /* this frame: 1 armed, 2 full, 4 cut */
  uint32_t arm_vblanks;               /* VBlanks counted when armed */
  uint32_t miss_run;                  /* frames in a row with something to show and no full strip */
  uint32_t down_until;                /* stood down until this frame */
  uint32_t down_len;                  /* the next stand-down's length: doubles while probes fail */
} S;

dscm_strip_stats_t dscm_strip_stats;
static uint32_t g_render_gen;         /* scene drawn into g_px */
static uint8_t g_rendered;

static const char* text_fn(void* ctx, const dsd_item_t* it, int which) {
  (void)ctx;
  if (it->kind == DSD_ITEM_OFFLINE) return which == DSD_TEXT_TITLE ? "Unlocks are kept and sent later" : "";
  if (it->kind == DSD_ITEM_ONLINE) return which == DSD_TEXT_TITLE ? "Everything waiting was sent" : "";
  if (it->seq) {
    const dscm_notice_t* e = &dscm_g_notices.ring[it->seq % DSCM_NOTICE_RING];
    if (e->seq == it->seq) return which == DSD_TEXT_TITLE ? e->text : e->detail;
    return "";
  }
  for (int i = 0; i < DSCM_INDICATOR_MAX; i++) {
    if (it->kind == DSD_ITEM_CHALLENGE && dscm_g_notices.challenges[i].id == it->id)
      return which == DSD_TEXT_TITLE ? dscm_g_notices.challenges[i].text : "";
    if (it->kind == DSD_ITEM_TRACKER && dscm_g_notices.trackers[i].id == it->id)
      return which == DSD_TEXT_TITLE ? "Leaderboard" : dscm_g_notices.trackers[i].text;
  }
  return "";
}

void dscm_strip_init(int hardcore) {
  dsd_config_t cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.font = &dsd_font_latin;
  cfg.text = text_fn;
  cfg.hardcore = (uint8_t)hardcore;
  dsd_init(&g_dsd, &cfg);
  memset(&S, 0, sizeof S);
  memset(&dscm_strip_stats, 0, sizeof dscm_strip_stats);
  g_rendered = 0;
}

static uint32_t fifo_cnt(void) { return NDMA_ON | NDMA_START(0x09) | NDMA_PHYS(2) | NDMA_DFIX; }

/* Tables by H-Blank line l (the write at the end of line l affects line l+1). */
static void build_tables(uint32_t base, int rows) {
  const uint32_t first = 191u - (uint32_t)rows;
  for (uint32_t l = 0; l < 192; l++) {
    const uint32_t line = l + 1;
    g_dtab[l] = line >= first && line <= LAST ? (base | 0x30000u) : base;
    g_btab[l] = line == first ? fifo_cnt() : 0;
  }
  dscm_dc_clean(g_dtab, sizeof g_dtab);
  dscm_dc_clean(g_btab, sizeof g_btab);
}

/* Once a frame, in an idle pass (IRQs on). */
void dscm_strip_frame(uint32_t frame) {
  dsd_caps_t caps = { ROWS_MAX, 0 };
  int rows = 0;

  if (S.rows) {
    if (S.frame_flags & 2) {
      S.miss_run = 0;
      S.down_len = 0;
    } else {
      S.miss_run++;
    }
  }
  /* Stand down: 10 frames in a row with no full strip (the game is busy at
   * the strip's lines, or IRQs keep waking it there): stop arming for 2 s,
   * then 4, 8, 16 while probes keep failing, so the module doesn't take
   * every wake's full path for nothing. The scheduler holds the notice
   * meanwhile (caps 0) and lists it as missed after 10 s. */
  if (S.miss_run >= 10) {
    S.miss_run = 0;
    S.down_len = S.down_len ? (S.down_len < 960 ? S.down_len * 2 : 960) : 120;
    S.down_until = frame + S.down_len;
    dscm_strip_stats.stand_downs++;
  }
  if ((int32_t)(S.down_until - frame) > 0) {
    caps.strip_lines = 0;
    dscm_strip_stats.frames_down++;
  }

  if (S.frame_flags & 1) {
    dscm_strip_stats.frames_armed++;
    if (S.frame_flags & 2) dscm_strip_stats.frames_full++;
    else if (S.frame_flags & 4) dscm_strip_stats.frames_cut++;
    else dscm_strip_stats.frames_none++;
  } else if (S.rows) {
    dscm_strip_stats.frames_unarmed++;
  }
  S.frame_flags = 0;

  dsd_ingest(&g_dsd, &dscm_g_notices);
  dsd_tick(&g_dsd, frame, &caps);
  if (!caps.strip_lines) {
    S.rows = 0;
    return;
  }
  if (dsd_current(&g_dsd)->kind != DSD_ITEM_NONE) {
    if (!g_rendered || g_render_gen != dsd_scene_gen(&g_dsd)) {
      dsd_surface_t s = { g_px, 256, ROWS_MAX, 256 };
      S.rows = 0;                     /* the buffer changes: nothing armed from it meanwhile */
      g_render_gen = dsd_scene_gen(&g_dsd);
      while (!dsd_render(&g_dsd, &s, ROWS_MAX)) {}
      for (int i = ROWS_MAX * 256; i < (ROWS_MAX + MARGIN) * 256; i++)
        g_px[i] = g_px[(ROWS_MAX - 1) * 256 + (i & 255)];
      dscm_dc_clean(g_px, sizeof g_px);
      g_rendered = 1;
      dscm_strip_stats.renders++;
    }
    rows = dsd_visible_rows(&g_dsd, ROWS_MAX);
  } else {
    g_rendered = 0;
  }
  if (rows > 0) {
    const uint32_t d = DISPCNT_A;
    if ((d >> 16 & 3) != 1) {         /* only a game showing its normal picture */
      dscm_strip_stats.skip_mode++;
      S.rows = 0;
      return;
    }
    if (rows != S.rows || d != S.base) {
      S.rows = 0;
      build_tables(d, rows);
      S.base = d;
      S.first = (uint8_t)(191 - rows);
    }
    S.rows = (uint8_t)rows;
  } else {
    S.rows = 0;
  }
}

int dscm_strip_active(void) { return S.rows != 0; }

/* The module's last halt, IRQs masked, right before WFI. */
void dscm_strip_arm(uint32_t vblanks) {
  uint32_t vc;
  if (!S.rows || S.armed) return;
  vc = REG_VCOUNT;
  /* In VBlank (the game finished its frame early and waits for the next):
   * H-Blank DMAs don't run during VBlank, so the table starts at line 0. */
  if (vc >= 192) {
    vc = 0;
    dscm_strip_stats.arms_vblank++;
  } else if (vc + 2 > S.first) {
    dscm_strip_stats.skip_late++;
    return;
  }
  if (DISPCNT_A != S.base) { dscm_strip_stats.skip_dispcnt++; return; }
  /* The game's own DMAs (their destinations can't be read back): one that
   * starts on H-Blank (raster effects; it could target DISPCNT), on the
   * display FIFO, or on the geometry FIFO (3D work the strip's bus use could
   * slow) keeps the strip off. */
  for (int x = 0; x < 4; x++) {
    const uint32_t c = REG32(0x040000B8 + x * 12);
    const uint32_t t = c >> 27 & 7;
    if ((c & (1u << 31)) && (t == 2 || t == 4 || t == 7)) { dscm_strip_stats.skip_gamedma++; return; }
  }
  if ((NDMA_CNT(0) | NDMA_CNT(CH_START) | NDMA_CNT(CH_FIFO) | NDMA_CNT(CH_TABLE)) & NDMA_ON) {
    dscm_strip_stats.skip_busy++;
    return;
  }
  NDMA_SAD(CH_FIFO) = DSCM_PHYS(g_px);
  NDMA_DAD(CH_FIFO) = MMEM_FIFO;
  NDMA_TCNT(CH_FIFO) = (uint32_t)(S.rows + MARGIN) * 128u;
  NDMA_WCNT(CH_FIFO) = 4;
  NDMA_BCNT(CH_FIFO) = 0;
  NDMA_SAD(CH_TABLE) = DSCM_PHYS(&g_dtab[vc]);
  NDMA_DAD(CH_TABLE) = 0x04000000u;
  NDMA_TCNT(CH_TABLE) = LAST + 1 - vc;          /* H-Blanks of lines vc..190 */
  NDMA_WCNT(CH_TABLE) = 1;
  NDMA_BCNT(CH_TABLE) = 0;
  NDMA_SAD(CH_START) = DSCM_PHYS(&g_btab[vc]);
  NDMA_DAD(CH_START) = 0x0400411Cu + CH_FIFO * 0x1C;
  NDMA_TCNT(CH_START) = S.first - vc;           /* stops once it has started the feed */
  NDMA_WCNT(CH_START) = 1;
  NDMA_BCNT(CH_START) = 0;
  NDMA_CNT(CH_TABLE) = NDMA_ON | NDMA_START(0x07) | NDMA_DFIX;
  NDMA_CNT(CH_START) = NDMA_ON | NDMA_START(0x07) | NDMA_DFIX;
  S.armed = 1;
  S.arm_vblanks = vblanks;
  S.frame_flags |= 1;
  dscm_strip_stats.arms++;
}

/* The first thing after WFI returns, IRQs still masked. */
void dscm_strip_wake(uint32_t vblanks) {
  uint32_t vc;
  int done;
  (void)vblanks;
  if (!S.armed) return;
  done = !(NDMA_CNT(CH_TABLE) & NDMA_ON);       /* the table ran out: every strip line was shown */
  NDMA_CNT(CH_TABLE) = 0;
  NDMA_CNT(CH_START) = 0;
  NDMA_CNT(CH_FIFO) = 0;
  while (NDMA_CNT(CH_TABLE) & NDMA_ON) {}       /* no table write may land after the restore */
  if ((DISPCNT_MODE & 3) == 3) {
    DISPCNT_MODE = (uint8_t)((DISPCNT_MODE & ~3) | 1);
    dscm_strip_stats.mode_fixed++;
  }
  S.armed = 0;
  vc = REG_VCOUNT;
  if (done) {
    S.frame_flags |= 2;
    dscm_strip_stats.wakes_done++;
  } else if (vc >= S.first && vc <= LAST) {     /* cut inside the strip */
    S.frame_flags |= 4;
    dscm_strip_stats.wakes_cut++;
  } else {
    dscm_strip_stats.wakes_before++;  /* woke before the strip: armed again at the next halt */
  }
}

void dscm_strip_publish(struct dscm_status_t* st) {
  st->irq_lines_total = dscm_strip_stats.frames_full;
  st->irq_lines_max = dscm_strip_stats.frames_armed;
  st->irq_lines_last = dscm_strip_stats.frames_cut;
}
