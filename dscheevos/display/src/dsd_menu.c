/* The in-game menu's "Now" page: what is happening in the set right now, as a
 * tile map in the menu's own text format. The menu pauses the game, so this
 * runs with nothing else going on; it still allocates nothing. */
#include <string.h>
#include "dsd.h"

typedef struct {
  dsd_grid_t* g;
  int first, line;                    /* scroll position, next line to emit */
} page_t;

static void emit(page_t* p, const uint8_t* glyphs, int n, int indent, int pal) {
  const int row = p->line++ - p->first;
  if (row < 0 || row >= p->g->rows) return;
  uint16_t* dst = p->g->map + row * p->g->stride;
  for (int x = 0; x < p->g->cols; x++) {
    const int i = x - indent;
    dst[x] = (uint16_t)((i >= 0 && i < n ? glyphs[i] : ' ') | pal << 12);
  }
}

static void emit_text(page_t* p, const dsd_font_t* f, const char* utf8, int indent, int pal) {
  uint8_t glyphs[64];
  const int room = p->g->cols - indent;
  int n = dsd_text_to_glyphs(f, utf8, glyphs, room > 64 ? 64 : room);
  emit(p, glyphs, n, indent, pal);
}

/* "left ... right" on one line, right part right-aligned. */
static void emit_pair(page_t* p, const dsd_font_t* f, const char* left, const char* right, int pal, int rpal) {
  const int row = p->line - p->first;
  uint8_t r[24];
  const int nr = dsd_text_to_glyphs(f, right, r, 12);
  uint8_t l[64];
  int room = p->g->cols - 1 - (nr ? nr + 1 : 0);
  if (room > 64) room = 64;
  const int nl = dsd_text_to_glyphs(f, left, l, room);
  emit(p, l, nl, 1, pal);
  if (row >= 0 && row < p->g->rows) {
    uint16_t* dst = p->g->map + row * p->g->stride + p->g->cols - nr;
    for (int i = 0; i < nr; i++) dst[i] = (uint16_t)(r[i] | rpal << 12);
  }
}

static void blank(page_t* p) { emit(p, 0, 0, 0, DSD_PAL_WHITE); }

/* Progress as a fraction of 10000 from "12/30" or "40%"; -1 if neither. */
static int progress_of(const char* m) {
  uint32_t a = 0, b = 0;
  int seen = 0;
  while (*m >= '0' && *m <= '9') { a = a * 10 + (uint32_t)(*m++ - '0'); seen = 1; }
  if (!seen) return -1;
  if (*m == '%') return a > 100 ? 10000 : (int)(a * 100);
  if (*m != '/') return -1;
  m++;
  while (*m >= '0' && *m <= '9') b = b * 10 + (uint32_t)(*m++ - '0');
  if (!b) return -1;
  return a >= b ? 10000 : (int)((uint64_t)a * 10000 / b);
}

static const char* missed_label(uint8_t kind) {
  switch (kind) {
    case DSD_ITEM_UNLOCK: return "Unlocked: ";
    case DSD_ITEM_MASTERY: return "Mastered: ";
    case DSD_ITEM_PROGRESS: return "Progress: ";
    case DSD_ITEM_LB_STARTED: return "LB started: ";
    case DSD_ITEM_LB_FAILED: return "LB failed: ";
    case DSD_ITEM_LB_SUBMITTED: return "LB submitted: ";
    case DSD_ITEM_LB_RANK: return "LB rank: ";
    case DSD_ITEM_CHALLENGE: return "Challenge: ";
    default: return "";
  }
}

int dsd_render_now(const dsd_t* d, const dscm_notices_t* n, const dscm_list_t* l,
                   dsd_grid_t* g, int first_line) {
  const dsd_font_t* f = d->cfg.font;
  page_t p = { g, first_line, 0 };
  if (n && n->magic != DSCM_NOTICES_MAGIC) n = 0;
  if (l && (l->magic != DSCM_LIST_MAGIC || l->entry_size != sizeof(dscm_list_entry_t))) l = 0;

  /* Indicators are copied while indicators_generation holds still, so a
   * half-updated set is never shown. */
  dscm_indicator_t ch[DSCM_INDICATOR_MAX], tr[DSCM_INDICATOR_MAX];
  memset(ch, 0, sizeof(ch));
  memset(tr, 0, sizeof(tr));
  if (n) {
    for (int tries = 0; tries < 3; tries++) {
      const uint32_t gen = n->indicators_generation;
      memcpy(ch, (const void*)n->challenges, sizeof(ch));
      memcpy(tr, (const void*)n->trackers, sizeof(tr));
      if (n->indicators_generation == gen) break;
    }
  }

  emit_text(&p, f, "Active challenges", 0, DSD_PAL_BLUE);
  int any = 0;
  for (int i = 0; i < DSCM_INDICATOR_MAX; i++) {
    if (!ch[i].id) continue;
    char t[sizeof(ch[i].text) + 1];
    memcpy(t, ch[i].text, sizeof(ch[i].text));
    t[sizeof(ch[i].text)] = 0;
    emit_text(&p, f, t, 1, DSD_PAL_LIME);
    any = 1;
  }
  if (!any) emit_text(&p, f, "none", 1, DSD_PAL_DARK_GRAY);
  blank(&p);

  emit_text(&p, f, "Leaderboards", 0, DSD_PAL_BLUE);
  any = 0;
  for (int i = 0; i < DSCM_INDICATOR_MAX; i++) {
    if (!tr[i].id) continue;
    char t[sizeof(tr[i].text) + 1];
    memcpy(t, tr[i].text, sizeof(tr[i].text));
    t[sizeof(tr[i].text)] = 0;
    /* The title comes from the module (rc_client) through the text callback. */
    dsd_item_t it;
    memset(&it, 0, sizeof(it));
    it.kind = DSD_ITEM_TRACKER;
    it.id = tr[i].id;
    const char* title = d->cfg.text ? d->cfg.text(d->cfg.text_ctx, &it, DSD_TEXT_TITLE) : 0;
    emit_pair(&p, f, title && *title ? title : "Leaderboard", t, DSD_PAL_WHITE, DSD_PAL_LIME);
    any = 1;
  }
  if (!any) emit_text(&p, f, "none running", 1, DSD_PAL_DARK_GRAY);
  blank(&p);

  if (l) {
    /* The four locked, measured achievements closest to done. */
    int best[4] = { -1, -1, -1, -1 }, score[4] = { -1, -1, -1, -1 };
    for (uint32_t i = 0; i < l->count; i++) {
      const dscm_list_entry_t* e = &l->e[i];
      if (e->state != DSCM_ACH_LOCKED || !(e->flags & DSCM_ACH_FLAG_MEASURED)) continue;
      char m[sizeof(e->measured) + 1];
      memcpy(m, e->measured, sizeof(e->measured));
      m[sizeof(e->measured)] = 0;
      const int s = progress_of(m);
      if (s < 0 || s >= 10000) continue;
      for (int k = 0; k < 4; k++) {
        if (s > score[k]) {
          memmove(&best[k + 1], &best[k], (size_t)(3 - k) * sizeof(int));
          memmove(&score[k + 1], &score[k], (size_t)(3 - k) * sizeof(int));
          best[k] = (int)i;
          score[k] = s;
          break;
        }
      }
    }
    emit_text(&p, f, "Almost there", 0, DSD_PAL_BLUE);
    if (best[0] < 0) emit_text(&p, f, "nothing measured yet", 1, DSD_PAL_DARK_GRAY);
    for (int k = 0; k < 4 && best[k] >= 0; k++) {
      const dscm_list_entry_t* e = &l->e[best[k]];
      char m[sizeof(e->measured) + 1];
      memcpy(m, e->measured, sizeof(e->measured));
      m[sizeof(e->measured)] = 0;
      emit_pair(&p, f, e->title, m, DSD_PAL_WHITE, DSD_PAL_LIGHT_GRAY);
    }
    blank(&p);

    /* The three newest unlocks. */
    int recent[3] = { -1, -1, -1 };
    for (uint32_t i = 0; i < l->count; i++) {
      const dscm_list_entry_t* e = &l->e[i];
      if (e->state != DSCM_ACH_UNLOCKED || !e->unlock_time) continue;
      for (int k = 0; k < 3; k++) {
        if (recent[k] < 0 || e->unlock_time > l->e[recent[k]].unlock_time) {
          memmove(&recent[k + 1], &recent[k], (size_t)(2 - k) * sizeof(int));
          recent[k] = (int)i;
          break;
        }
      }
    }
    emit_text(&p, f, "Recently unlocked", 0, DSD_PAL_BLUE);
    if (recent[0] < 0) emit_text(&p, f, "none yet", 1, DSD_PAL_DARK_GRAY);
    for (int k = 0; k < 3 && recent[k] >= 0; k++) {
      const dscm_list_entry_t* e = &l->e[recent[k]];
      char pts[12];
      int v = e->points, i = 0;
      char tmp[8];
      do { tmp[i++] = (char)('0' + v % 10); v /= 10; } while (v && i < 7);
      int o = 0;
      while (i) pts[o++] = tmp[--i];
      memcpy(pts + o, " pts", 5);
      emit_pair(&p, f, e->title, pts, DSD_PAL_WHITE, DSD_PAL_LIGHT_GRAY);
    }
    blank(&p);
  }

  /* Notices that couldn't be shown during play. */
  if (dsd_missed(d, 0)) {
    emit_text(&p, f, "Not shown during play", 0, DSD_PAL_BLUE);
    for (int i = 0; i < DSD_MISSED_MAX; i++) {
      const dsd_item_t* m = dsd_missed(d, i);
      if (!m) break;
      const char* title = d->cfg.text ? d->cfg.text(d->cfg.text_ctx, m, DSD_TEXT_TITLE) : 0;
      uint8_t glyphs[64];
      int k = dsd_text_to_glyphs(f, missed_label(m->kind), glyphs, 20);
      const int room = g->cols - 1 - k;
      k += dsd_text_to_glyphs(f, title ? title : "", glyphs + k, room > 0 ? room : 0);
      emit(&p, glyphs, k, 1, DSD_PAL_LIGHT_GRAY);
    }
  }

  /* Clear the rest of the area. */
  const int total = p.line;
  while (p.line - p.first < g->rows) blank(&p);
  return total;
}
