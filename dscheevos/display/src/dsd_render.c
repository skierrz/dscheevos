/* Renderer: lays out the visible item once, then rasterises it a band of rows
 * at a time into a BGR555 surface. No stdio, no malloc. */
#include <string.h>
#include "dsd_badge.h"

#define RGB(r, g, b) ((uint16_t)((r) | (g) << 5 | (b) << 10))

enum { C_BG, C_EDGE, C_TEXT, C_DIM, C_GOLD, C_RED, C_BLUE, C_GREEN, C_ORANGE, C_PURPLE, C_SKY };
static const uint16_t palette[] = {
  [C_BG] = RGB(3, 4, 7),     [C_EDGE] = RGB(9, 10, 15),  [C_TEXT] = RGB(31, 31, 31),
  [C_DIM] = RGB(19, 20, 23), [C_GOLD] = RGB(31, 25, 6),  [C_RED] = RGB(31, 9, 8),
  [C_BLUE] = RGB(11, 20, 31), [C_GREEN] = RGB(11, 27, 12), [C_ORANGE] = RGB(31, 17, 6),
  [C_PURPLE] = RGB(23, 13, 31), [C_SKY] = RGB(13, 24, 31),
};

enum { I_TROPHY, I_CROWN, I_FLAG, I_BARS, I_BOLT, I_SHIELD, I_CLOUD };
static const uint16_t icons[][16] = {
  /* trophy */ {0x0000, 0x3ffc, 0x6ff6, 0x4ff2, 0x4ff2, 0x6ff6, 0x3ffc, 0x1ff8, 0x07e0, 0x0180, 0x0180, 0x03c0, 0x07e0, 0x0ff0, 0x0ff0, 0x0000},
  /* crown */  {0x0000, 0x0000, 0x4182, 0x63c6, 0x77ee, 0x7ffe, 0x7ffe, 0x7ffe, 0x6ff6, 0x7ffe, 0x7ffe, 0x3ffc, 0x0000, 0x3ffc, 0x0000, 0x0000},
  /* flag */   {0x0000, 0x0004, 0x0ffc, 0x1ffc, 0x3ffc, 0x1ffc, 0x0ffc, 0x07fc, 0x0004, 0x0004, 0x0004, 0x0004, 0x0004, 0x000e, 0x001f, 0x0000},
  /* bars */   {0x0000, 0x0000, 0x6000, 0x6000, 0x6600, 0x6600, 0x6660, 0x6660, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x7ffe, 0x0000, 0x0000},
  /* bolt */   {0x0000, 0x0f00, 0x0780, 0x03c0, 0x01e0, 0x07f0, 0x07f8, 0x0780, 0x03c0, 0x01e0, 0x00f0, 0x0038, 0x000c, 0x0002, 0x0000, 0x0000},
  /* shield */ {0x0000, 0x0180, 0x0ff0, 0x3ffc, 0x3ffc, 0x3c3c, 0x381c, 0x381c, 0x3c3c, 0x1ff8, 0x1ff8, 0x0ff0, 0x07e0, 0x03c0, 0x0180, 0x0000},
  /* cloud */  {0x0000, 0x0000, 0x03c0, 0x0c30, 0x1008, 0x6004, 0x8002, 0x8242, 0x8002, 0x43c4, 0x7ff8, 0x0000, 0x0240, 0x0180, 0x0240, 0x0000},
};

typedef struct { const char* label; const char* short_label; uint8_t icon, accent; } kind_style_t;
static const kind_style_t styles[DSD_ITEM_KINDS] = {
  [DSD_ITEM_MODE] = {"Hardcore", "Hardcore", I_SHIELD, C_RED},  /* softcore swaps label and colour below */
  [DSD_ITEM_UNLOCK] = {"Achievement unlocked", "Unlocked", I_TROPHY, C_GOLD},
  [DSD_ITEM_MASTERY] = {"Set mastered", "Mastered", I_CROWN, C_GOLD},
  [DSD_ITEM_PROGRESS] = {"Progress", "Progress", I_BARS, C_GREEN},
  [DSD_ITEM_LB_STARTED] = {"Leaderboard attempt", "Attempt", I_FLAG, C_SKY},
  [DSD_ITEM_LB_FAILED] = {"Leaderboard failed", "Failed", I_FLAG, C_ORANGE},
  [DSD_ITEM_LB_SUBMITTED] = {"Leaderboard submitted", "Submitted", I_FLAG, C_SKY},
  [DSD_ITEM_LB_RANK] = {"Leaderboard rank", "Rank", I_FLAG, C_SKY},
  [DSD_ITEM_OFFLINE] = {"Offline", "Offline", I_CLOUD, C_ORANGE},
  [DSD_ITEM_ONLINE] = {"Back online", "Online", I_CLOUD, C_GREEN},
  [DSD_ITEM_CHALLENGE] = {"Challenge", "Challenge", I_BOLT, C_PURPLE},
  [DSD_ITEM_TRACKER] = {"Leaderboard", "Leaderboard", I_FLAG, C_SKY},
  [DSD_ITEM_TRAY] = {"Challenge active", "Challenge", I_BOLT, C_PURPLE},  /* no challenge: tracker style */
};

/* ---- text ---- */

static uint32_t utf8_next(const unsigned char** s) {
  const unsigned char* p = *s;
  uint32_t c = *p++;
  int n = 0;
  if (c >= 0xF0) { c &= 0x07; n = 3; }
  else if (c >= 0xE0) { c &= 0x0F; n = 2; }
  else if (c >= 0xC0) { c &= 0x1F; n = 1; }
  else if (c >= 0x80) { *s = p; return '?'; }   /* stray continuation byte */
  while (n--) {
    if ((*p & 0xC0) != 0x80) { *s = p; return '?'; }
    c = c << 6 | (*p++ & 0x3F);
  }
  *s = p;
  return c;
}

static uint8_t glyph_of(const dsd_font_t* f, uint32_t c) {
  if (c < 0x80 && f->codepoints[c] == c) return (uint8_t)c;
  if (c == 0x2019 || c == 0x2018) return '\'';          /* typographic quotes */
  if (c == 0x201C || c == 0x201D) return '"';
  if (c == 0x2013 || c == 0x2014) return '-';
  if (c == 0x00A0) return ' ';
  if (c == 0 || c > 0xFFFF) return '?';
  for (int g = 128; g < 256; g++) if (f->codepoints[g] == c) return (uint8_t)g;
  for (int g = 0; g < 128; g++) if (f->codepoints[g] == c) return (uint8_t)g;
  return '?';
}

int dsd_text_to_glyphs(const dsd_font_t* f, const char* utf8, uint8_t* out, int max) {
  const unsigned char* p = (const unsigned char*)(utf8 ? utf8 : "");
  int n = 0;
  while (*p) {
    const uint32_t c = utf8_next(&p);
    if (c == '\n' || c == '\r' || c == '\t') { if (n < max) out[n++] = ' '; continue; }
    if (n == max) {
      if (max >= 3) out[max - 1] = out[max - 2] = out[max - 3] = '.';
      return n;
    }
    out[n++] = glyph_of(f, c);
  }
  return n;
}

/* Lay a string out in at most px pixels, with "..." when it doesn't fit. */
static int fit(const dsd_t* d, uint8_t* line, int n, int px) {
  int w = 0;
  for (int i = 0; i < n; i++) w += d->widths[line[i]];
  if (w <= px) return n;
  const int dots = 3 * d->widths['.'];
  w = 0;
  int i = 0;
  while (i < n && w + d->widths[line[i]] + dots <= px) w += d->widths[line[i++]];
  while (i > 0 && line[i - 1] == ' ') i--;
  for (int k = 0; k < 3 && i < DSD_LINE_MAX; k++) line[i++] = '.';
  return i;
}

static int width_of(const dsd_t* d, const uint8_t* line, int n) {
  int w = 0;
  for (int i = 0; i < n; i++) w += d->widths[line[i]];
  return w;
}

static int append_ascii(uint8_t* out, int n, const char* s) {
  while (*s && n < DSD_LINE_MAX) out[n++] = (uint8_t)*s++;
  return n;
}

static int append_int(uint8_t* out, int n, int32_t v) {
  char buf[12];
  int i = 0;
  uint32_t u = v < 0 ? (uint32_t)-v : (uint32_t)v;
  do { buf[i++] = (char)('0' + u % 10); u /= 10; } while (u);
  if (v < 0 && n < DSD_LINE_MAX) out[n++] = '-';
  while (i && n < DSD_LINE_MAX) out[n++] = (uint8_t)buf[--i];
  return n;
}

/* ---- layout ---- */

/* Each item draws as: accent bar, icon, then up to three lines:
 *   0: label (accent colour) with the value right-aligned, e.g. "10 pts"
 *   1: title (white)
 *   2: detail (grey)
 * A compact strip (< 32 rows) keeps one line: "label: title", value at right. */
#define TEXT_X 26
#define TEXT_X_BADGE 40               /* after a 32x32 badge */
#define RIGHT_PAD 6

static const char* text(const dsd_t* d, const dsd_item_t* it, int which) {
  const char* s = d->cfg.text ? d->cfg.text(d->cfg.text_ctx, it, which) : 0;
  return s ? s : "";
}

/* The titles of every active indicator of one kind, comma-separated. */
static int append_titles(const dsd_t* d, uint8_t* line, int n, uint8_t kind, const uint32_t* ids) {
  int first = 1;
  for (int i = 0; i < DSCM_INDICATOR_MAX && n < DSD_LINE_MAX; i++) {
    if (!ids[i]) continue;
    if (!first) n = append_ascii(line, n, ", ");
    dsd_item_t t;
    memset(&t, 0, sizeof(t));
    t.kind = kind;
    t.id = ids[i];
    n += dsd_text_to_glyphs(d->cfg.font, text(d, &t, DSD_TEXT_TITLE), line + n, DSD_LINE_MAX - n);
    first = 0;
  }
  return n;
}

static uint32_t first_id(const uint32_t* ids) {
  for (int i = 0; i < DSCM_INDICATOR_MAX; i++) if (ids[i]) return ids[i];
  return 0;
}

static int tray_has_challenge(const dsd_t* d) { return first_id(d->challenge_ids) != 0; }

static void layout(dsd_t* d, const dsd_item_t* it, int compact, int w) {
  d->badge = !compact && d->cfg.badge && it->kind != DSD_ITEM_TRAY ? d->cfg.badge(d->cfg.badge_ctx, it) : 0;
  d->badge16 = compact && d->cfg.badge16 && it->kind != DSD_ITEM_TRAY ? d->cfg.badge16(d->cfg.badge_ctx, it) : 0;
  const int text_x = d->badge ? TEXT_X_BADGE : TEXT_X;
  uint8_t value[DSD_LINE_MAX];
  int nv = 0;
  if (it->kind == DSD_ITEM_UNLOCK) {
    nv = append_int(value, 0, it->value);
    nv = append_ascii(value, nv, " pts");
  } else if (it->kind == DSD_ITEM_LB_RANK) {
    nv = append_ascii(value, 0, "#");
    nv = append_int(value, nv, it->value);
  } else if (it->kind == DSD_ITEM_LB_SUBMITTED || it->kind == DSD_ITEM_PROGRESS ||
             it->kind == DSD_ITEM_TRACKER) {
    nv = dsd_text_to_glyphs(d->cfg.font, text(d, it, DSD_TEXT_DETAIL), value, 16);
  }

  /* Compact strips and merged unlocks use the short label, so the title or
   * the "+N more" keeps its room. */
  const char* label = compact || it->more ? styles[it->kind].short_label : styles[it->kind].label;
  if (it->kind == DSD_ITEM_MODE && !it->value) label = "Casual";

  const int avail = w - text_x - RIGHT_PAD;
  int n0;
  if (it->kind == DSD_ITEM_TRAY) {
    /* Active challenges (RA's Trigger indicator) by title, and the first
     * leaderboard tracker's value (Measured/leaderboard) at the right. */
    const uint32_t tr = first_id(d->tracker_ids);
    dsd_item_t t;
    memset(&t, 0, sizeof(t));
    t.kind = DSD_ITEM_TRACKER;
    t.id = tr;
    if (tr) nv = dsd_text_to_glyphs(d->cfg.font, text(d, &t, DSD_TEXT_DETAIL), value, 16);
    const int ch = tray_has_challenge(d);
    if (compact) {
      n0 = ch ? append_titles(d, d->line[0], 0, DSD_ITEM_CHALLENGE, d->challenge_ids)
              : append_titles(d, d->line[0], 0, DSD_ITEM_TRACKER, d->tracker_ids);
      d->line_len[1] = d->line_len[2] = 0;
    } else {
      n0 = append_ascii(d->line[0], 0, ch ? "Challenge active" : "Leaderboard");
      int n1 = ch ? append_titles(d, d->line[1], 0, DSD_ITEM_CHALLENGE, d->challenge_ids)
                  : append_titles(d, d->line[1], 0, DSD_ITEM_TRACKER, d->tracker_ids);
      d->line_len[1] = (uint8_t)fit(d, d->line[1], n1, avail);
      int n2 = ch && tr ? append_titles(d, d->line[2], 0, DSD_ITEM_TRACKER, d->tracker_ids) : 0;
      d->line_len[2] = (uint8_t)fit(d, d->line[2], n2, avail);
    }
    goto place;
  }
  n0 = append_ascii(d->line[0], 0, label);
  if (it->more) {
    n0 = append_ascii(d->line[0], n0, " (+");
    n0 = append_int(d->line[0], n0, it->more);
    n0 = append_ascii(d->line[0], n0, " more)");
  }

  if (compact) {
    n0 = append_ascii(d->line[0], n0, ": ");
    const int room = DSD_LINE_MAX - n0;
    n0 += dsd_text_to_glyphs(d->cfg.font, text(d, it, DSD_TEXT_TITLE), d->line[0] + n0, room);
    d->line_len[1] = d->line_len[2] = 0;
  } else {
    d->line_len[1] = (uint8_t)fit(d, d->line[1],
        dsd_text_to_glyphs(d->cfg.font, text(d, it, DSD_TEXT_TITLE), d->line[1], DSD_LINE_MAX), avail);
    int n2 = 0;
    if (it->kind != DSD_ITEM_PROGRESS && it->kind != DSD_ITEM_LB_SUBMITTED && it->kind != DSD_ITEM_TRACKER)
      n2 = dsd_text_to_glyphs(d->cfg.font, text(d, it, DSD_TEXT_DETAIL), d->line[2], DSD_LINE_MAX);
    d->line_len[2] = (uint8_t)fit(d, d->line[2], n2, avail);
  }

place:;
  /* The value goes at the right of line 0; the label gives way to it. */
  const int vw = width_of(d, value, nv);
  d->line_len[0] = (uint8_t)fit(d, d->line[0], n0, avail - (nv ? vw + 8 : 0));
  const int right = d->cfg.side == DSD_SIDE_RIGHT;
  for (int k = 0; k < 3; k++)                       /* right side: text ends at the badge */
    d->line_x[k] = (uint8_t)(right ? w - text_x - width_of(d, d->line[k], d->line_len[k]) : text_x);
  if (nv) {
    /* Stored after the label in the same buffer, drawn from its own x. */
    const int at = d->line_len[0];
    if (at + nv <= DSD_LINE_MAX) {
      memcpy(d->line[0] + at, value, (size_t)nv);
      d->value_at = (uint8_t)at;
      d->value_x = (uint8_t)(right ? RIGHT_PAD : w - RIGHT_PAD - vw);
      d->line_len[0] = (uint8_t)(at + nv);
    }
  } else {
    d->value_at = d->line_len[0];
  }
}

/* ---- raster ---- */

static void draw_glyph_row(uint16_t* row, int w, int x, const uint8_t* glyph_row_bits, uint16_t c) {
  uint8_t bits = *glyph_row_bits;
  for (int j = 0; bits && j < 8; j++, bits >>= 1)
    if ((bits & 1) && x + j >= 0 && x + j < w) row[x + j] = c;
}

int dsd_render(dsd_t* d, dsd_surface_t* s, int max_rows) {
  const dsd_item_t* it = dsd_current(d);
  const int compact = s->h < 32;
  if (d->layout_gen != d->scene_gen || d->layout_h != s->h) {
    d->layout_gen = d->scene_gen;
    d->layout_h = s->h;
    d->render_row = 0;
    if (it->kind) layout(d, it, compact, s->w);
  }
  if (!it->kind) {
    d->render_row = s->h;
    return 1;
  }

  uint8_t accent = styles[it->kind].accent;
  uint8_t icon = styles[it->kind].icon;
  if (it->kind == DSD_ITEM_TRAY && !tray_has_challenge(d)) { accent = C_SKY; icon = I_FLAG; }
  if (it->kind == DSD_ITEM_MODE && !it->value) accent = C_BLUE;
  if (it->kind == DSD_ITEM_UNLOCK && d->cfg.hardcore == 0) accent = C_GOLD;

  const int icon_y = (s->h - 16) / 2;
  const int line_y[3] = { compact ? (s->h - 8) / 2 : 3, 13, 22 };
  static const uint8_t line_colour[3] = { 0xFF, C_TEXT, C_DIM };  /* 0xFF = accent */

  int end = d->render_row + max_rows;
  if (end > s->h) end = s->h;
  for (int y = d->render_row; y < end; y++) {
    uint16_t* row = s->pixels + y * s->stride;
    const uint16_t bg = palette[y == s->h - 1 ? C_EDGE : C_BG];
    for (int x = 0; x < s->w; x++) row[x] = bg;
    const int right = d->cfg.side == DSD_SIDE_RIGHT;
    if (right) row[s->w - 1] = row[s->w - 2] = row[s->w - 3] = palette[accent];
    else row[0] = row[1] = row[2] = palette[accent];

    if (d->badge || d->badge16) {
      /* drawn below, after the row loop */
    } else if (y >= icon_y && y < icon_y + 16) {
      uint16_t bits = icons[icon][y - icon_y];
      for (int j = 0; bits; j++, bits >>= 1)
        if (bits & 1) row[right ? s->w - 22 + j : 6 + j] = palette[accent];
    }

    for (int k = 0; k < 3; k++) {
      if (!d->line_len[k] || y < line_y[k] || y >= line_y[k] + 8) continue;
      const int gy = y - line_y[k];
      const uint16_t c = palette[line_colour[k] == 0xFF ? accent : line_colour[k]];
      int x = d->line_x[k];
      for (int i = 0; i < d->line_len[k]; i++) {
        const uint8_t g = d->line[k][i];
        if (k == 0 && i == d->value_at) x = d->value_x;
        draw_glyph_row(row, s->w, x, d->cfg.font->glyphs + g * 8 + gy,
                       k == 0 && i >= d->value_at ? palette[C_TEXT] : c);
        x += d->widths[g];
      }
    }
  }
  if (d->badge) {
    /* The badge's rows in this band: a progress or challenge notice is about
     * an achievement not yet unlocked, so it shows the locked look. */
    const int locked = it->kind == DSD_ITEM_PROGRESS || it->kind == DSD_ITEM_CHALLENGE;
    dsd_surface_t band = { s->pixels + d->render_row * s->stride, s->w, (uint16_t)(end - d->render_row), s->stride };
    const int bx = d->cfg.side == DSD_SIDE_RIGHT ? s->w - 4 - DSD_BADGE_SIZE : 4;
    dsd_badge_blit(d->badge, locked, &band, bx, (s->h - DSD_BADGE_SIZE) / 2 - d->render_row);
  }
  if (d->badge16) {
    /* In the icon's place: the thin bar's text layout is unchanged. */
    const int locked = it->kind == DSD_ITEM_PROGRESS || it->kind == DSD_ITEM_CHALLENGE;
    dsd_surface_t band = { s->pixels + d->render_row * s->stride, s->w, (uint16_t)(end - d->render_row), s->stride };
    const int bx = d->cfg.side == DSD_SIDE_RIGHT ? s->w - 22 : 6;
    dsd_badge16_blit(d->badge16, locked, &band, bx, icon_y - d->render_row);
  }
  d->render_row = (uint16_t)end;
  return d->render_row >= s->h;
}
