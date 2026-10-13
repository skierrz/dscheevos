/* DScheevos display library: turns the module's notices into what the player
 * sees. Plain C, no hardware access, no malloc, no timers: the caller owns all
 * memory and passes the frame count. Presenters (the top strip, the in-game
 * menu, LEDs, ...) only move the pixels this library draws.
 * Design: docs/display-library.md. */
#ifndef DSD_H
#define DSD_H

#include <stdint.h>
#include "dscheevos_module.h"

/* ---- Font: 256 glyphs of 8x8 1bpp, one byte per row, bit 0 = left pixel
 * (nds-bootstrap's in-game menu format), and the Unicode character each glyph
 * draws (0 = none). */
typedef struct dsd_font_t {
  const uint8_t* glyphs;
  const uint16_t* codepoints;
} dsd_font_t;

/* ---- Surfaces. Pixels are the DS's 15-bit BGR555. */
typedef struct dsd_surface_t {
  uint16_t* pixels;
  uint16_t w, h;
  uint16_t stride;                    /* in pixels */
} dsd_surface_t;

/* ---- What can be shown this frame (from the presenter). */
typedef struct dsd_caps_t {
  uint8_t strip_lines;                /* 0: nothing on screen can be shown now */
  uint8_t pinned_ok;                  /* owner setting: a leaderboard tracker may stay up */
} dsd_caps_t;

enum {                                /* scene item kinds */
  DSD_ITEM_NONE = 0,
  DSD_ITEM_MODE,                      /* hardcore / softcore at session start */
  DSD_ITEM_UNLOCK,
  DSD_ITEM_MASTERY,
  DSD_ITEM_PROGRESS,
  DSD_ITEM_LB_STARTED,
  DSD_ITEM_LB_FAILED,
  DSD_ITEM_LB_SUBMITTED,
  DSD_ITEM_LB_RANK,
  DSD_ITEM_OFFLINE,
  DSD_ITEM_ONLINE,
  DSD_ITEM_CHALLENGE,                 /* a challenge indicator appeared */
  DSD_ITEM_TRACKER,                   /* a leaderboard tracker appeared (or is pinned) */
  DSD_ITEM_KINDS
};

enum { DSD_PHASE_IN = 0, DSD_PHASE_HOLD, DSD_PHASE_OUT };
enum { DSD_SIDE_LEFT = 0, DSD_SIDE_RIGHT };  /* the strip is full-width; this mirrors its layout */

typedef struct dsd_item_t {
  uint8_t kind;                       /* DSD_ITEM_* */
  uint8_t phase;                      /* DSD_PHASE_* */
  uint16_t frames_left;               /* in this phase */
  uint32_t id;                        /* achievement / leaderboard id */
  uint32_t seq;                       /* source notice (0 for indicators) */
  int32_t value;                      /* points, score, rank, hardcore flag */
  uint32_t raised;                    /* module frame the notice was raised */
  uint16_t more;                      /* unlocks merged into this one ("+N more") */
  uint16_t stalled;                   /* frames waited with nothing able to show it */
} dsd_item_t;

/* Text is never copied into the library: the module serves it from
 * rc_client's resident strings (or the notice ring) when the renderer asks. */
enum { DSD_TEXT_TITLE = 0, DSD_TEXT_DETAIL };
typedef const char* (*dsd_text_fn)(void* ctx, const dsd_item_t* item, int which);

/* Badges (phase 2): the module returns the item's badge once its slot has
 * been read from the pack, or 0 to draw the built-in icon. The pointer must
 * stay valid while the item is on screen. */
struct dsd_badge_t;
typedef const struct dsd_badge_t* (*dsd_badge_fn)(void* ctx, const dsd_item_t* item);

#define DSD_QUEUE_MAX 8
#define DSD_MISSED_MAX 8
#define DSD_LINE_MAX 40               /* glyphs per rendered line */

typedef struct dsd_config_t {
  const dsd_font_t* font;
  dsd_text_fn text;
  void* text_ctx;
  dsd_badge_fn badge;                 /* 0 = icons only (phase 1) */
  uint8_t side;                       /* DSD_SIDE_*: which side the badge and text sit on */
  void* badge_ctx;
  uint8_t hardcore;                   /* accent colour */
} dsd_config_t;

typedef struct dsd_stats_t {
  uint32_t shown, merged, missed, torn, overwritten;
} dsd_stats_t;

/* All state. The caller allocates it (module bss); nothing else is used. */
typedef struct dsd_t {
  dsd_config_t cfg;
  uint32_t last_seq;                  /* newest notice ingested */
  uint32_t last_gen;                  /* indicators_generation ingested */
  uint32_t challenge_ids[DSCM_INDICATOR_MAX];
  uint32_t tracker_ids[DSCM_INDICATOR_MAX];
  uint32_t frame;                     /* last tick */
  uint8_t queue_count;
  dsd_item_t queue[DSD_QUEUE_MAX];
  dsd_item_t current;                 /* kind NONE = nothing on screen */
  dsd_item_t pinned;                  /* tracker shown while pinned_ok and nothing else is up */
  uint8_t pinned_ok;                  /* from the last tick's caps */
  uint8_t missed_head;
  dsd_item_t missed[DSD_MISSED_MAX];  /* waited too long; for the menu page */
  uint32_t scene_gen;                 /* bumped when what's visible changes */
  dsd_stats_t stats;
  /* render state: laid out once per item, then rasterised a band at a time */
  uint32_t layout_gen;
  uint16_t layout_h;
  uint16_t render_row;
  const struct dsd_badge_t* badge;    /* drawn instead of the icon, 0 = icon */
  uint8_t value_at;                   /* line 0: glyphs from here are the right-aligned value */
  uint8_t value_x;
  uint8_t widths[256];                /* glyph advance, from the font */
  uint8_t line_len[3];
  uint8_t line_x[3];
  uint8_t line[3][DSD_LINE_MAX];      /* glyph indices */
} dsd_t;

/* Timing, in frames (60 Hz). */
#define DSD_FADE_FRAMES 4
#define DSD_WAIT_MAX 600              /* after 10 s with nothing able to show it, a notice goes to missed[] */

void dsd_init(dsd_t* d, const dsd_config_t* cfg);

/* Idle time. Reads new ring entries and indicator changes; returns at once
 * when nothing changed (two word compares). */
void dsd_ingest(dsd_t* d, const dscm_notices_t* n);

/* Idle time, once per frame. Advances timing and picks what to show. */
void dsd_tick(dsd_t* d, uint32_t frame, const dsd_caps_t* caps);

/* What is visible now: an item of kind DSD_ITEM_NONE means nothing. */
const dsd_item_t* dsd_current(const dsd_t* d);

/* Rows of an h-row strip to show this frame: grows over DSD_FADE_FRAMES when
 * an item comes in and shrinks when it goes out, so the presenter animates by
 * changing its line count and the renderer draws each item once. */
int dsd_visible_rows(const dsd_t* d, int h);
uint32_t dsd_scene_gen(const dsd_t* d);
int dsd_busy(const dsd_t* d);         /* something showing or waiting */

/* Render the current item into s (s->h = strip height: 16 compact, 32 full).
 * Resumable: draws at most max_rows rows per call; returns 1 when the whole
 * surface is done. Starts over by itself when the scene changes. */
int dsd_render(dsd_t* d, dsd_surface_t* s, int max_rows);

/* ---- In-game menu "Now" page (P1). The menu draws text as a tile map: one
 * cell per 8x8 glyph, entry = glyph index | palette << 12, using the same font. */
enum {                                /* the menu's palettes (nds-bootstrap FontPalette) */
  DSD_PAL_WHITE = 0, DSD_PAL_LIGHT_GRAY, DSD_PAL_DARK_GRAY, DSD_PAL_BLUE, DSD_PAL_RED, DSD_PAL_LIME
};
typedef struct dsd_grid_t {
  uint16_t* map;
  uint8_t cols, rows;                 /* the area to fill */
  uint16_t stride;                    /* map entries per row (32 for a 256-px map) */
} dsd_grid_t;

/* Fills the grid with what matters now: active challenges, leaderboard
 * trackers, the closest measured achievements, recent unlocks and notices
 * that couldn't be shown. first_line scrolls. Returns the page's total line
 * count. list and notices may be 0. Runs while the game is paused (menu). */
int dsd_render_now(const dsd_t* d, const dscm_notices_t* n, const dscm_list_t* l,
                   dsd_grid_t* g, int first_line);

/* Missed notices, newest first (i = 0..DSD_MISSED_MAX-1), or 0. */
const dsd_item_t* dsd_missed(const dsd_t* d, int i);

/* UTF-8 to the font's glyph indices; characters the font lacks become '?'.
 * Stops at max glyphs, ending in "..." when cut. Returns the glyph count. */
int dsd_text_to_glyphs(const dsd_font_t* f, const char* utf8, uint8_t* out, int max);

#endif
