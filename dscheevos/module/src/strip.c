/* LAB ONLY (branch strip-lab, never in a kit): the notice bar in the module,
 * to see it in real games in melonDS. Built with STRIP=1.
 *
 * The display library (dscheevos/display) picks what to show and draws it;
 * this file moves the pixels with the D1 scheme (round2-synthesis.md), with
 * one change: no code in the game's IRQ path. The bar is armed only in the
 * module's last halt of an idle pass, which runs with IRQs masked (CPSR.I),
 * and stopped the moment the CPU wakes, still masked, before the game's
 * handler can run. So game code never runs while DISPCNT holds our mode.
 *
 *   NDMA3: on each H-Blank from the arming line, a whole DISPCNT word from
 *          dtab (the game's word; mode 3 on the bar's lines).
 *   NDMA1: on each H-Blank up to the line before the bar, a word into NDMA2's
 *          control from btab (0, then the FIFO enable): starts the feed.
 *   NDMA2: the main-memory display FIFO feed (startup 09h).
 * NDMA0 is nds-bootstrap's card engine: never touched, and the bar isn't
 * armed while NDMA0 or NDMA1 is busy.
 *
 * Placement is learned, within the player's choice (NOTICE_EDGE: auto, top
 * half or bottom half of the screen): for
 * the first LEARN_FRAMES frames the module records, at every wake, the line
 * the game halted on and the line it woke on, and scores each possible
 * position by how often the whole bar would have been shown there. It keeps
 * learning whenever a notice is up, and picks again for each new notice.
 *
 * STRIP_RESUME=1 (experiment): after a wake cuts the bar, it is armed again
 * at the next halt from the current line, so a game whose IRQs wake it
 * briefly still shows most of the bar (with gaps where the game ran).
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
#ifndef STRIP_ROWS
#define STRIP_ROWS 16                 /* the default height: the thin bar (compact layout) */
#endif
#ifdef STRIP_LABHW                     /* the lab hardware build: relaxed + the test notice */
#define STRIP_RELAXED 1
#define STRIP_DEMO 1
#endif
#ifdef STRIP_RELAXED
#define RELAXED 1                     /* experiment: the game's IRQ handlers may run while the bar is up */
#else
#define RELAXED 0
#endif
#ifndef STRIP_RESUME
#define STRIP_RESUME 0
#endif
#define ROWS_MAX 32                   /* the big bar (full layout, 32x32 badges) */
static int g_rows = STRIP_ROWS;      /* the player's NOTICE_SIZE: 16 thin, 32 big */
#define ROWS g_rows
#if defined(STRIP_OBSERVE) || defined(STRIP_RELAXED)
#define STRIP_RSCORE 1                /* also score positions with the game's IRQ handlers allowed */
#endif
#define MARGIN 2                      /* rows fed past the bar: a late switch shows our background */
#define FIRST_MAX (191 - ROWS)        /* the bar ends by line 190; 191 is always the game's */
#define LEARN_FRAMES 300               /* 5 s of watching, then pick */
#define RELEARN_FRAMES 3600           /* no place found: watch again a minute later, then 2, 4 ... 16 */
#define RING 256                      /* halt/wake pairs, written masked, read in passes */
#define LINE(cnt, vc) ((cnt) * 263u + ((vc) < 192 ? (uint32_t)(vc) : (uint32_t)(vc) - 263u))

extern const dsd_font_t dsd_font_latin;
extern dscm_notices_t dscm_g_notices;
extern void dscm_dc_clean(const void* addr, uint32_t len);

static dsd_t g_dsd;
static uint16_t g_px[(ROWS_MAX + MARGIN) * 256] __attribute__((aligned(32)));
static uint32_t g_dtab[192] __attribute__((aligned(32)));
static uint32_t g_btab[192] __attribute__((aligned(32)));
static uint32_t g_fifo_word __attribute__((aligned(32)));

/* Read in the masked halt: kept in one place. */
static struct {
  uint32_t base;                      /* the game's DISPCNT the tables were built for (mode 1) */
  uint8_t rows;                       /* bar lines now, 0 = nothing to show */
  uint8_t first;                      /* the bar's first line */
  uint8_t armed;
  uint8_t record;                     /* log halts and wakes (learning, or a notice up) */
  uint32_t frame_flags;               /* this frame: 1 armed, 2 full, 4 cut, 8 resumed */
  uint32_t cut_frame, cut_line;       /* the last cut (frame = VBlank count), for a resume */
  uint32_t h_cnt;                     /* VBlanks counted at the last halt */
  uint16_t h_vc;                      /* VCOUNT at the last halt */
  uint32_t h_line;                    /* that halt as an absolute line */
  uint32_t last_line;                 /* the latest absolute line seen */
  uint32_t wake_line;                 /* RELAXED: the last wake while the bar was armed (absolute) */
  uint8_t woke_armed;                 /* RELAXED: a wake let the game run with the bar armed */
  uint8_t relax_off;                  /* RELAXED: turned off for this session (the game wrote DISPCNT) */
  uint32_t wakes, wakes_last;         /* wakes this frame / last frame */
  uint32_t head;                      /* ring entries written */
  uint32_t miss_run;                  /* frames in a row with something to show and no full bar */
  uint32_t down_until;                /* stood down until this frame */
  uint32_t down_len;                  /* the next stand-down's length: doubles while probes fail */
} S;
static struct { uint32_t a, b; } g_ring[RING];   /* paused from line a to line b (absolute lines) */

/* Learning, in passes. */
static struct {
  uint32_t tail;
  uint32_t frame;                     /* frame being accumulated */
  uint8_t started;
  uint8_t paused[194], woke[194];     /* lines -2..191 of that frame */
#ifdef STRIP_RSCORE
  /* The same, if the game's interrupt handlers were allowed to run while the
   * bar is up: a wake after which the game halts again within a line counts
   * as no wake (only a handler ran, no game thread). */
  uint8_t rpaused[194], rwoke[194];
  uint32_t prev_b;
  uint16_t rfull[192];
  uint32_t obs_frames;
#endif
  uint32_t frames;                    /* frames scored */
  uint16_t full[192];                 /* frames in which a bar starting at this line would have shown whole */
  uint16_t lines[192];                /* frames in which this line was paused (for STRIP_RESUME) */
  uint8_t picked;
  uint32_t learn_start;                /* module frame learning began */
  uint32_t relearn_at;                 /* no place found: learn again from this frame */
  uint32_t relearn_len;                /* the wait before that: 1, 2, 4 ... 16 minutes */
} L;

dscm_strip_stats_t dscm_strip_stats;
static uint32_t g_render_gen;         /* scene drawn into g_px */
static uint8_t g_rendered;

#define DEMO_ID 0xDE30u                /* STRIP_DEMO: the test notice (never an RA id here: no seq) */

static const char* text_fn(void* ctx, const dsd_item_t* it, int which) {
  (void)ctx;
  if (!it->seq && it->id == DEMO_ID) return which == DSD_TEXT_TITLE ? "Test bar - DScheevos lab" : "";
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

static int g_edge;                    /* 0 auto (ties go to the bottom), 1 top half, 2 bottom half */
static const dscm_badges_t* g_badges; /* the set's 16x16 badges, or 0: icons */

void dscm_strip_set_badges(const dscm_badges_t* b) { g_badges = b; }

/* An achievement's badge slot, or 0: unlocks, progress and challenges name one. */
static const uint8_t* badge_slot(const dsd_item_t* it, uint32_t slot_size) {
  if (!g_badges || g_badges->slot_size != slot_size
      || (it->kind != DSD_ITEM_UNLOCK && it->kind != DSD_ITEM_PROGRESS && it->kind != DSD_ITEM_CHALLENGE))
    return 0;
  const int i = dsd_pack_find(g_badges->ids, g_badges->count, it->id);
  if (i < 0) return 0;
  return (const uint8_t*)&g_badges->ids[g_badges->count] + (uint32_t)i * slot_size;
}

static const struct dsd_badge16_t* badge16_fn(void* ctx, const dsd_item_t* it) {
  (void)ctx;
  return (const dsd_badge16_t*)badge_slot(it, DSCM_BADGES_SLOT);
}

/* The big bar (NOTICE_SIZE = big) draws 32x32 ones. */
static const struct dsd_badge_t* badge32_fn(void* ctx, const dsd_item_t* it) {
  (void)ctx;
  return (const dsd_badge_t*)badge_slot(it, DSCM_BADGES_SLOT32);
}

void dscm_strip_init(int hardcore, int edge, int rows) {
  dsd_config_t cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.font = &dsd_font_latin;
  cfg.text = text_fn;
  cfg.badge16 = badge16_fn;
  cfg.badge = badge32_fn;
  cfg.hardcore = (uint8_t)hardcore;
  dsd_init(&g_dsd, &cfg);
  memset(&S, 0, sizeof S);
  memset(&L, 0, sizeof L);
  memset(&dscm_strip_stats, 0, sizeof dscm_strip_stats);
  S.record = 1;
  g_edge = edge;
  g_rows = rows == 32 ? 32 : 16;
  g_rendered = 0;
}

static uint32_t fifo_cnt(void) { return NDMA_ON | NDMA_START(0x09) | NDMA_PHYS(2) | NDMA_DFIX; }

/* Tables by H-Blank line l (the write at the end of line l affects line l+1). */
static void build_tables(uint32_t base, uint32_t first) {
  for (uint32_t l = 0; l < 192; l++) {
    const uint32_t line = l + 1;
    g_dtab[l] = line >= first && line < first + ROWS ? (base | 0x30000u) : base;
    g_btab[l] = line == first ? fifo_cnt() : 0;
  }
  g_fifo_word = fifo_cnt();
  dscm_dc_clean(g_dtab, sizeof g_dtab);
  dscm_dc_clean(g_btab, sizeof g_btab);
  dscm_dc_clean(&g_fifo_word, sizeof g_fifo_word);
}

/* ---- learning: score each possible position from the halt/wake log ---- */

#ifdef STRIP_RSCORE
static void relax_score(void) {
  uint32_t run = 0;
  for (int i = 0; i < 194; i++) {
    const int line = i - 2;
    run = (L.rpaused[i] || L.paused[i]) && !L.rwoke[i] ? run + 1 : 0;
    if (line >= ROWS - 1 && run >= ROWS + 2u) {
      const int first = line - ROWS + 1;
      if (first <= FIRST_MAX && L.rfull[first] < 0xFFFF) L.rfull[first]++;
    }
  }
  memset(L.rpaused, 0, sizeof L.rpaused);
  memset(L.rwoke, 0, sizeof L.rwoke);
}
#endif

#ifdef STRIP_OBSERVE
/* Every OBS_FRAMES frames: the best position's count, strict and relaxed,
 * anywhere and at the bottom edge; then the counts start over. */
#define OBS_FRAMES 300
#define OBS_MAX 64
uint16_t dscm_strip_obs[OBS_MAX][7];  /* frames, best strict, its line, strict at 175, best relaxed, its line, relaxed at 175 */
uint32_t dscm_strip_obs_n;
static void observe_frame(void) {
  if (++L.obs_frames >= OBS_FRAMES) {
    uint16_t* o = dscm_strip_obs[dscm_strip_obs_n++ % OBS_MAX];
    int bs = 0, br = 0;
    for (int f = 1; f <= FIRST_MAX; f++) {
      if (L.full[f] > L.full[bs]) bs = f;
      if (L.rfull[f] > L.rfull[br]) br = f;
    }
    o[0] = (uint16_t)L.obs_frames; o[1] = L.full[bs]; o[2] = (uint16_t)bs; o[3] = L.full[FIRST_MAX];
    o[4] = L.rfull[br]; o[5] = (uint16_t)br; o[6] = L.rfull[FIRST_MAX];
    L.obs_frames = 0;
    memset(L.full, 0, sizeof L.full);
    memset(L.rfull, 0, sizeof L.rfull);
  }
}
#endif

static void score_frame(void) {
  uint32_t run = 0;
#ifdef STRIP_RSCORE
  relax_score();
#endif
#ifdef STRIP_OBSERVE
  observe_frame();
#endif
  for (int i = 0; i < 194; i++) {
    const int line = i - 2;
    run = L.paused[i] && !L.woke[i] ? run + 1 : 0;
    if (line >= 0 && L.paused[i] && L.lines[line] < 0xFFFF) L.lines[line]++;
    /* a bar on lines first..line shows whole if the game halted by first-2 and
     * nothing woke it until after line */
    if (line >= ROWS - 1 && run >= ROWS + 2u) {
      const int first = line - ROWS + 1;
      if (first <= FIRST_MAX && L.full[first] < 0xFFFF) L.full[first]++;
    }
  }
  L.frames++;
  memset(L.paused, 0, sizeof L.paused);
  memset(L.woke, 0, sizeof L.woke);
}

/* Frame f holds absolute lines f*263-71 .. f*263+191 (VBlank f comes first). */
static uint32_t frame_of(uint32_t x) { return (x + 71u) / 263u; }

static void learn_interval(uint32_t a, uint32_t b) {
  uint32_t fa = frame_of(a), fb = frame_of(b), f;
  if (b < a || fb - fa > 600) return;   /* clock skew, or a stretch too long to matter */
  if (fa < 2) return;                   /* the session's first frames: no line before them */
  if (!L.started || fa > L.frame + 30) {
    /* the first entry, or logging resumed after a quiet stretch: start
     * afresh (those frames weren't watched) */
    L.started = 1;
    L.frame = fa;
    memset(L.paused, 0, sizeof L.paused);
    memset(L.woke, 0, sizeof L.woke);
  }
  if (fa < L.frame) return;
  while (L.frame < fa) {                /* watched frames with no halt: the game never paused */
    score_frame();
    L.frame++;
  }
  for (f = fa; ; f++) {
    const uint32_t base = f * 263u;
    const uint32_t lo = a > base - 2 ? a : base - 2, hi = b < base + 192 ? b : base + 192;
    for (uint32_t x = lo; x < hi; x++) L.paused[x - base + 2] = 1;
#ifdef STRIP_RSCORE
    /* the game ran for at most a line since the last wake: a handler only */
    if (f == fa && a - L.prev_b <= 1 && L.prev_b >= base - 2 && L.prev_b < base + 192) {
      L.rwoke[L.prev_b - base + 2] = 0;
      for (uint32_t x = L.prev_b; x < a && x < base + 192; x++) L.rpaused[x - base + 2] = 1;
    }
#endif
    if (f == fb) {
      if (b >= base - 2 && b < base + 192) {
        L.woke[b - base + 2] = 1;
#ifdef STRIP_RSCORE
        L.rwoke[b - base + 2] = 1;
#endif
      }
#ifdef STRIP_RSCORE
      L.prev_b = b;
#endif
      break;
    }
    score_frame();
    L.frame = f + 1;
  }
}

static void learn(void) {
  uint32_t head = S.head;
  if (head - L.tail > RING) L.tail = head - RING;   /* overwritten while a pass was away */
  for (; L.tail != head; L.tail++)
    learn_interval(g_ring[L.tail % RING].a, g_ring[L.tail % RING].b);
  if (L.frames > 30000) {               /* keep the counts in range: older frames weigh half */
    for (int i = 0; i < 192; i++) { L.full[i] /= 2; L.lines[i] /= 2; }
    L.frames /= 2;
  }
}

/* The best position for the bar, or -1 if none has ever shown whole.
 * Positions within 2% of the best score count as equal and the one nearest
 * the preferred edge wins (auto: the bottom), so the bar sits where a player
 * expects it. A bar already placed (prev) stays unless a position is more
 * than 10% better: it shouldn't move from one notice to the next. */
static uint32_t score_at(int f) {
  uint32_t sc = L.full[f];
#ifdef STRIP_RSCORE
  if (RELAXED && !S.relax_off) sc = L.rfull[f];
#endif
  if (STRIP_RESUME) { sc = 0; for (int l = f; l < f + ROWS; l++) sc += L.lines[l]; sc /= ROWS; }
  return sc;
}

static int pick(int prev) {
  int lo = 0, hi = FIRST_MAX, best = -1;
  uint32_t max = 0;
  if (g_edge == 1) hi = (192 - ROWS) / 2;
  if (g_edge == 2) lo = (192 - ROWS) / 2;
  for (int f = lo; f <= hi; f++) if (score_at(f) > max) max = score_at(f);
  if (max) {
    for (int f = lo; f <= hi; f++) {
      if (score_at(f) * 50u < max * 49u) continue;          /* not within 2% */
      if (best < 0 || g_edge != 1) best = f;            /* top: the first; else the last */
    }
    if (prev >= lo && prev <= hi && score_at(prev) * 10u >= max * 9u) best = prev;
  }
  dscm_strip_stats.picks++;
  dscm_strip_stats.pick_first = best < 0 ? 0xFF : (uint32_t)best;
  dscm_strip_stats.pick_score = best < 0 ? 0 : score_at(best);
  dscm_strip_stats.learn_frames = L.frames;
  return best;
}

/* ---- once a frame, in an idle pass (IRQs on) ---- */

void dscm_strip_frame(uint32_t frame) {
  dsd_caps_t caps = { ROWS, 0 };
  int rows = 0;

  learn();
  S.wakes_last = S.wakes;
  S.wakes = 0;
  if (S.rows) {
    if (S.frame_flags & 2) {
      S.miss_run = 0;
      S.down_len = 0;
    } else {
      S.miss_run++;
    }
  }
  /* Stand down: 10 frames in a row with no whole bar: stop arming for 2 s,
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
  /* Learning: watch every wake for LEARN_FRAMES frames, then pick. If no
   * position ever fit, watch again RELEARN_FRAMES later (the game may have
   * moved on from a loading screen or a video). */
  if (!L.learn_start) L.learn_start = frame;
  if (dscm_strip_stats.no_place && (int32_t)(frame - L.relearn_at) >= 0) {
    dscm_strip_stats.no_place = 0;
    dscm_strip_stats.pick_first = 0;
    L.learn_start = frame;
    L.picked = 0;
    memset(L.full, 0, sizeof L.full);
    memset(L.lines, 0, sizeof L.lines);
    L.frames = 0;
  }
  const int learning = frame - L.learn_start < LEARN_FRAMES;
  if (learning) {                       /* still learning where the bar fits */
    caps.strip_lines = 0;
    dscm_strip_stats.frames_learning++;
  }

  if (S.frame_flags & 1) {
    dscm_strip_stats.frames_armed++;
    if (S.frame_flags & 2) {
      if (S.frame_flags & 8) dscm_strip_stats.frames_resumed++;
      else dscm_strip_stats.frames_full++;
    } else if (S.frame_flags & 4) dscm_strip_stats.frames_cut++;
    else dscm_strip_stats.frames_none++;
  } else if (S.rows) {
    dscm_strip_stats.frames_unarmed++;
  }
  S.frame_flags = 0;

  dsd_ingest(&g_dsd, &dscm_g_notices);
#ifdef STRIP_DEMO
  /* LAB hardware runs: a test notice every 30 s, so the bar can be judged in
   * the middle of play. Only the bar draws it; nothing goes to the server. */
  if (frame && frame % 1800 == 0 && g_dsd.queue_count < DSD_QUEUE_MAX) {
    dsd_item_t* q = &g_dsd.queue[g_dsd.queue_count++];
    memset(q, 0, sizeof *q);
    q->kind = DSD_ITEM_PROGRESS;           /* not an unlock: no points, no trophy */
    q->id = DEMO_ID;
    q->raised = frame;
    dscm_strip_stats.demos++;
  }
#endif
  dsd_tick(&g_dsd, frame, &caps);
#ifdef STRIP_OBSERVE
  S.record = 1;                         /* measurement build: watch every wake, all session */
#else
  S.record = learning || S.rows != 0;
#endif
  if (!caps.strip_lines) {
    S.rows = 0;
    return;
  }
  if (dsd_current(&g_dsd)->kind != DSD_ITEM_NONE) {
    if (!g_rendered || g_render_gen != dsd_scene_gen(&g_dsd)) {
      dsd_surface_t sf = { g_px, 256, ROWS, 256 };
      S.rows = 0;                     /* the buffer changes: nothing armed from it meanwhile */
      g_render_gen = dsd_scene_gen(&g_dsd);
      while (!dsd_render(&g_dsd, &sf, ROWS)) {}
      for (int i = ROWS * 256; i < (ROWS + MARGIN) * 256; i++)
        g_px[i] = g_px[(ROWS - 1) * 256 + (i & 255)];
      dscm_dc_clean(g_px, sizeof g_px);
      g_rendered = 1;
      dscm_strip_stats.renders++;
      L.picked = 0;                   /* a new notice: pick again with what was learned since */
    }
    rows = dsd_visible_rows(&g_dsd, ROWS);
    L.relearn_len = 0;                /* a place works again: a later failure starts at a minute */
  } else {
    g_rendered = 0;
  }
  if (!L.picked) {
    L.picked = 1;
    const int f = pick(dscm_strip_stats.picks ? S.first : -1);
    if (f < 0) {
      dscm_strip_stats.no_place = 1;
      L.relearn_len = L.relearn_len ? (L.relearn_len < 16 * RELEARN_FRAMES ? L.relearn_len * 2 : L.relearn_len) : RELEARN_FRAMES;
      L.relearn_at = frame + L.relearn_len;
      S.rows = 0;
      return;
    }
    if ((uint32_t)f != S.first) { S.rows = 0; S.first = (uint8_t)f; S.base = 0; }
  }
  if (dscm_strip_stats.pick_first == 0xFF) { S.rows = 0; return; }
  if (rows > 0) {
    const uint32_t d = DISPCNT_A;
    if ((d >> 16 & 3) != 1) {         /* only a game showing its normal picture */
      dscm_strip_stats.skip_mode++;
      S.rows = 0;
      return;
    }
    if (d != S.base) {
      S.rows = 0;
      build_tables(d, S.first);
      S.base = d;
    }
    /* The slide-in: the bar's full height from the start (one table per
     * position), revealing more of the drawing each frame would need a table
     * per height; the compact bar is short enough to just appear. */
    S.rows = ROWS;
  } else {
    S.rows = 0;
  }
}

int dscm_strip_active(void) { return S.rows != 0 || S.record; }

/* ---- the module's last halt, IRQs masked ---- */

/* Stop every channel and give the game its mode back. */
static void stop_bar(void) {
  NDMA_CNT(CH_TABLE) = 0;
  NDMA_CNT(CH_START) = 0;
  NDMA_CNT(CH_FIFO) = 0;
  while (NDMA_CNT(CH_TABLE) & NDMA_ON) {}       /* no table write may land after the restore */
  if ((DISPCNT_MODE & 3) == 3) {
    DISPCNT_MODE = (uint8_t)((DISPCNT_MODE & ~3) | 1);
    dscm_strip_stats.mode_fixed++;
  }
  S.armed = 0;
  S.woke_armed = 0;
}

#if RELAXED
/* RELAXED, at a halt with the bar still armed after a wake let the game
 * run. Returns 1 if the bar stays armed. Checks, in order:
 * - the table has run out: the bar showed whole (handlers ran inside it);
 * - DISPCNT's other bits changed: the game wrote it while the bar was up.
 *   Stop at once and keep the game's bits; no relaxed bar for the rest of
 *   the session (the table could undo a later write of the game's);
 * - the game ran longer than a line since the wake: a game thread worked
 *   while the bar was up (the cost this experiment measures). */
static int relaxed_halt(uint32_t now) {
  if (!(NDMA_CNT(CH_TABLE) & NDMA_ON)) {
    stop_bar();                                 /* the feed and starter are done too; mode is 1 again */
    S.frame_flags |= 2;
    dscm_strip_stats.wakes_done++;
    return 0;
  }
  if ((DISPCNT_A ^ S.base) & ~0x30000u) {
    stop_bar();
    S.relax_off = 1;
    S.frame_flags |= 4;
    dscm_strip_stats.game_dispcnt++;
    return 0;
  }
  if (S.woke_armed && now - S.wake_line > 1) {
    dscm_strip_stats.work_under++;
    dscm_strip_stats.work_lines += now - S.wake_line;
  }
  S.woke_armed = 0;
  return 1;
}
#endif

/* An absolute line from the VBlank count and VCOUNT. The count is the ARM7's
 * and can lag the ARM9's wake at line 192 by a little: time never runs
 * backwards, so a line that would be earlier than the last one seen belongs
 * to the next frame. */
static uint32_t abs_line(uint32_t cnt, uint32_t vc) {
  uint32_t x = LINE(cnt, vc);
  if ((int32_t)(x - S.last_line) < 0 && (int32_t)(x + 263u - S.last_line) >= 0) x += 263u;
  S.last_line = x;
  return x;
}

void dscm_strip_arm(uint32_t vblanks) {
  uint32_t vc, start, end;
  int resume = 0;
  vc = REG_VCOUNT;
  if (S.record || RELAXED) S.h_line = abs_line(vblanks, vc);
#if RELAXED
  if (S.armed && !S.relax_off && relaxed_halt(S.h_line)) return;
#endif
  if (!S.rows || S.armed) return;
  end = S.first + ROWS - 1;
#if RELAXED
  /* A game that wakes on nearly every line (many wakes last frame): arm only
   * just before the bar, so the table writes DISPCNT on the bar's lines and
   * not from early in the frame: a game write before the bar can't be undone. */
  if (!S.relax_off && S.wakes_last > 60 && (vc >= 192 || vc + 5 < S.first)) {
    dscm_strip_stats.late_waits++;
    return;
  }
#endif
  if (vc >= 192) {
    /* In VBlank (the game finished its frame early and waits for the next):
     * H-Blank DMAs don't run during VBlank, so the table starts at line 0. */
    start = 0;
    dscm_strip_stats.arms_vblank++;
  } else if (vc + 2 <= S.first) {
    start = vc;
  } else if (STRIP_RESUME && S.cut_frame == vblanks && vc >= S.first - 1u && vc < end) {
    start = vc;
    resume = 1;
  } else {
    dscm_strip_stats.skip_late++;
    return;
  }
  if (DISPCNT_A != S.base) { dscm_strip_stats.skip_dispcnt++; return; }
  if ((NDMA_CNT(0) | NDMA_CNT(CH_START) | NDMA_CNT(CH_FIFO) | NDMA_CNT(CH_TABLE)) & NDMA_ON) {
    dscm_strip_stats.skip_busy++;
    return;
  }
  /* The game's own DMAs: one on the display FIFO, or on the geometry FIFO
   * (3D work the bar's bus use could slow), keeps the bar off. One that
   * starts on H-Blank (raster effects) keeps it off only if it could write
   * DISPCNT or the FIFO: its destination is read back (DMAxDAD), and a 0
   * (a DSi that doesn't let it be read) counts as could. */
  for (int x = 0; x < 4; x++) {
    const uint32_t c = REG32(0x040000B8 + x * 12);
    const uint32_t t = c >> 27 & 7;
    if (!(c & (1u << 31))) continue;
    if (t == 4 || t == 7) { dscm_strip_stats.skip_gamedma++; return; }
    if (t == 2) {
      const uint32_t dad = REG32(0x040000B4 + x * 12) & 0x0FFFFFFFu;
      const uint32_t unit = (c & (1u << 26)) ? 4 : 2;
      const uint32_t units = (c & 0x1FFFFF) ? (c & 0x1FFFFF) : 0x200000;
      const uint32_t dc = c >> 21 & 3;        /* 0 increment, 1 decrement, 2 fixed, 3 increment and reload */
      /* the bytes it may write: one unit if fixed, else the block, below DAD
       * when decrementing (a repeating increment without reload moves on,
       * so the whole block is a lower bound: also block if it ever passes) */
      const uint32_t lo = dc == 1 ? dad - (units - 1) * unit : dad;
      const uint32_t hi = dc == 2 ? dad + unit : dc == 1 ? dad + unit : dad + units * unit;
      const int may_hit = !dad || (dc == 0 && (c & (1u << 25)))          /* repeating, moving on: unknown */
                          || (lo < 0x04000004u && hi > 0x04000000u) || (lo <= 0x04000068u && hi > 0x04000068u);
      if (may_hit) {
        dscm_strip_stats.skip_gamedma++;
        return;
      }
      dscm_strip_stats.gamedma_ok++;
    }
  }
  {
    const uint32_t row = resume ? vc + 1 - S.first : 0;   /* the bar's row the next line shows */
    NDMA_SAD(CH_FIFO) = DSCM_PHYS(g_px + row * 256);
    NDMA_DAD(CH_FIFO) = MMEM_FIFO;
    NDMA_TCNT(CH_FIFO) = (ROWS - row + MARGIN) * 128u;
    NDMA_WCNT(CH_FIFO) = 4;
    NDMA_BCNT(CH_FIFO) = 0;
  }
  NDMA_SAD(CH_TABLE) = DSCM_PHYS(&g_dtab[start]);
  NDMA_DAD(CH_TABLE) = 0x04000000u;
  NDMA_TCNT(CH_TABLE) = end + 1 - start;          /* H-Blanks of lines start..end: the last puts line end+1 back */
  NDMA_WCNT(CH_TABLE) = 1;
  NDMA_BCNT(CH_TABLE) = 0;
  if (S.first == 0 && !resume) {
    /* A bar from line 0 can only be armed in VBlank: switch now (nothing is
     * drawn until line 0) and start the feed now. */
    DISPCNT_MODE = (uint8_t)((DISPCNT_MODE & ~3) | 3);
    NDMA_CNT(CH_FIFO) = fifo_cnt();
  } else {
    NDMA_SAD(CH_START) = resume ? DSCM_PHYS(&g_fifo_word) : DSCM_PHYS(&g_btab[start]);
    NDMA_DAD(CH_START) = 0x0400411Cu + CH_FIFO * 0x1C;
    NDMA_TCNT(CH_START) = resume ? 1 : S.first - start;   /* stops once it has started the feed */
    NDMA_WCNT(CH_START) = 1;
    NDMA_BCNT(CH_START) = 0;
    NDMA_CNT(CH_START) = NDMA_ON | NDMA_START(0x07) | NDMA_DFIX;
  }
  NDMA_CNT(CH_TABLE) = NDMA_ON | NDMA_START(0x07) | NDMA_DFIX;
  S.armed = 1;
  S.frame_flags |= resume ? 9 : 1;
  if (resume) {
    dscm_strip_stats.resumes++;
    dscm_strip_stats.gap_lines += vc + 1 - S.cut_line;
  }
  dscm_strip_stats.arms++;
}

/* The first thing after WFI returns, IRQs still masked. */
void dscm_strip_wake(uint32_t vblanks) {
  uint32_t vc = REG_VCOUNT;
  int done;
  S.wakes++;
  if (S.record || RELAXED) {
    const uint32_t now = abs_line(vblanks, vc);
    if (S.record) {
      g_ring[S.head % RING].a = S.h_line;
      g_ring[S.head % RING].b = now;
      S.head++;
    }
#if RELAXED
    /* The game's handler runs with the bar armed. The checks come at the
     * next halt; a VBlank always ends it (the table is done by line 191). */
    if (S.armed && !S.relax_off && vc < 192) {
      S.wake_line = now;
      S.woke_armed = 1;
      dscm_strip_stats.relaxed_wakes++;
      return;
    }
#endif
  }
  if (!S.armed) return;
  done = !(NDMA_CNT(CH_TABLE) & NDMA_ON);       /* the table ran out: every bar line was shown */
  stop_bar();
  vc = REG_VCOUNT;
  if (done) {
    S.frame_flags |= 2;
    dscm_strip_stats.wakes_done++;
  } else if (vc >= S.first && vc < S.first + ROWS && vc < 192) {   /* cut inside the bar */
    S.frame_flags |= 4;
    S.cut_frame = vblanks;
    S.cut_line = vc;
    dscm_strip_stats.wakes_cut++;
  } else {
    dscm_strip_stats.wakes_before++;  /* woke before the bar: armed again at the next halt */
  }
}

void dscm_strip_publish(struct dscm_status_t* st) {
  st->irq_lines_total = dscm_strip_stats.frames_full;
  st->irq_lines_max = dscm_strip_stats.frames_armed;
  st->irq_lines_last = RELAXED ? dscm_strip_stats.work_under : dscm_strip_stats.frames_cut;
}
