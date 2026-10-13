/* Scheduler: notices in, one visible item out. Runs in the module's idle time. */
#include <string.h>
#include "dsd.h"

static const uint8_t priority[DSD_ITEM_KINDS] = {
  [DSD_ITEM_MASTERY] = 1,
  [DSD_ITEM_UNLOCK] = 2, [DSD_ITEM_MODE] = 2,
  [DSD_ITEM_LB_SUBMITTED] = 3, [DSD_ITEM_LB_FAILED] = 3, [DSD_ITEM_LB_RANK] = 3,
  [DSD_ITEM_LB_STARTED] = 4,
  [DSD_ITEM_PROGRESS] = 5, [DSD_ITEM_OFFLINE] = 5, [DSD_ITEM_ONLINE] = 5,
  [DSD_ITEM_CHALLENGE] = 6, [DSD_ITEM_TRACKER] = 6, [DSD_ITEM_TRAY] = 7,
};

/* Hold times in frames. The strip covers game pixels, so these are shorter
 * than RetroArch's 4 s. */
static const uint16_t hold[DSD_ITEM_KINDS] = {
  [DSD_ITEM_MASTERY] = 300,
  [DSD_ITEM_UNLOCK] = 180, [DSD_ITEM_MODE] = 180,
  [DSD_ITEM_LB_SUBMITTED] = 120, [DSD_ITEM_LB_FAILED] = 120, [DSD_ITEM_LB_RANK] = 120,
  [DSD_ITEM_LB_STARTED] = 120,
  [DSD_ITEM_PROGRESS] = 90, [DSD_ITEM_OFFLINE] = 120, [DSD_ITEM_ONLINE] = 120,
  [DSD_ITEM_CHALLENGE] = 60, [DSD_ITEM_TRACKER] = 60,
};

static const uint8_t from_notice[] = {
  [DSCM_NOTICE_SESSION_START] = DSD_ITEM_MODE,
  [DSCM_NOTICE_UNLOCK] = DSD_ITEM_UNLOCK,
  [DSCM_NOTICE_MASTERY] = DSD_ITEM_MASTERY,
  [DSCM_NOTICE_PROGRESS] = DSD_ITEM_PROGRESS,
  [DSCM_NOTICE_LEADERBOARD_STARTED] = DSD_ITEM_LB_STARTED,
  [DSCM_NOTICE_LEADERBOARD_FAILED] = DSD_ITEM_LB_FAILED,
  [DSCM_NOTICE_LEADERBOARD_SUBMITTED] = DSD_ITEM_LB_SUBMITTED,
  [DSCM_NOTICE_LEADERBOARD_RANK] = DSD_ITEM_LB_RANK,
  [DSCM_NOTICE_OFFLINE] = DSD_ITEM_OFFLINE,
  [DSCM_NOTICE_ONLINE] = DSD_ITEM_ONLINE,
};

void dsd_init(dsd_t* d, const dsd_config_t* cfg) {
  memset(d, 0, sizeof(*d));
  d->cfg = *cfg;
  d->layout_gen = ~0u;
  for (int g = 0; g < 256; g++) {
    /* Advance: up to the rightmost lit column, plus one column of space. */
    const uint8_t* rows = cfg->font->glyphs + g * 8;
    uint8_t lit = 0;
    for (int r = 0; r < 8; r++) lit |= rows[r];
    int w = 0;
    while (lit >> w) w++;
    d->widths[g] = w ? (uint8_t)(w + 1) : 4;
  }
}

static void to_missed(dsd_t* d, const dsd_item_t* it) {
  d->stats.missed++;
  /* A missed indicator flash isn't listed: the menu page shows the active
   * challenges and trackers themselves. */
  if (it->kind == DSD_ITEM_CHALLENGE || it->kind == DSD_ITEM_TRACKER) return;
  d->missed_head = (uint8_t)((d->missed_head + 1) % DSD_MISSED_MAX);
  d->missed[d->missed_head] = *it;
}

static void remove_at(dsd_t* d, int i) {
  memmove(&d->queue[i], &d->queue[i + 1], (size_t)(d->queue_count - i - 1) * sizeof(dsd_item_t));
  d->queue_count--;
}

static void enqueue(dsd_t* d, dsd_item_t it) {
  const uint8_t k = it.kind;

  for (int i = d->queue_count - 1; i >= 0; i--) {
    dsd_item_t* q = &d->queue[i];
    /* One progress slot: the newest replaces the older. */
    if (k == DSD_ITEM_PROGRESS && q->kind == DSD_ITEM_PROGRESS) { *q = it; return; }
    /* An indicator already waiting needs no second entry. */
    if ((k == DSD_ITEM_CHALLENGE || k == DSD_ITEM_TRACKER) && q->kind == k && q->id == it.id) return;
    /* A finished attempt drops its own "started" if that never got shown, so
     * "started" can't appear after "failed" or "submitted". */
    if ((k == DSD_ITEM_LB_FAILED || k == DSD_ITEM_LB_SUBMITTED) &&
        q->kind == DSD_ITEM_LB_STARTED && q->id == it.id)
      remove_at(d, i);
  }

  if (d->queue_count < DSD_QUEUE_MAX) {
    d->queue[d->queue_count++] = it;
    return;
  }

  /* Full. An unlock merges into the newest queued unlock ("+N more"). */
  if (k == DSD_ITEM_UNLOCK) {
    for (int i = d->queue_count - 1; i >= 0; i--) {
      if (d->queue[i].kind == DSD_ITEM_UNLOCK) {
        d->queue[i].more += (uint16_t)(1 + it.more);
        d->stats.merged++;
        return;
      }
    }
  }
  /* Otherwise the least important (newest on a tie) goes to missed[]. */
  int worst = 0;
  for (int i = 1; i < d->queue_count; i++)
    if (priority[d->queue[i].kind] >= priority[d->queue[worst].kind]) worst = i;
  if (priority[d->queue[worst].kind] > priority[k]) {
    to_missed(d, &d->queue[worst]);
    remove_at(d, worst);
    d->queue[d->queue_count++] = it;
  } else {
    to_missed(d, &it);
  }
}

static int has(const uint32_t* ids, uint32_t id) {
  for (int i = 0; i < DSCM_INDICATOR_MAX; i++) if (ids[i] == id) return 1;
  return 0;
}

void dsd_ingest(dsd_t* d, const dscm_notices_t* n) {
  if (!n || n->magic != DSCM_NOTICES_MAGIC) return;
  const uint32_t head = n->head;
  const uint32_t gen = n->indicators_generation;
  if (head == d->last_seq && gen == d->last_gen) return;

  if (head != d->last_seq) {
    uint32_t seq = d->last_seq + 1;
    if (head < d->last_seq) {
      seq = head > DSCM_NOTICE_RING ? head - DSCM_NOTICE_RING + 1 : 1;   /* module restarted (game reset) */
    } else if (head - d->last_seq > DSCM_NOTICE_RING) {
      d->stats.overwritten += head - d->last_seq - DSCM_NOTICE_RING;
      seq = head - DSCM_NOTICE_RING + 1;
    }
    for (; seq <= head; seq++) {
      const dscm_notice_t* e = &n->ring[seq % DSCM_NOTICE_RING];
      const uint32_t before = e->seq;
      dsd_item_t it;
      memset(&it, 0, sizeof(it));
      const uint16_t kind = e->kind;
      it.id = e->id;
      it.value = e->value;
      it.raised = e->frame;
      it.seq = seq;
      if (before != seq || e->seq != seq) { d->stats.torn++; continue; }
      if (kind >= sizeof(from_notice) || !from_notice[kind]) continue;
      it.kind = from_notice[kind];
      enqueue(d, it);
    }
    d->last_seq = head;
  }

  if (gen != d->last_gen) {
    uint32_t ch[DSCM_INDICATOR_MAX], tr[DSCM_INDICATOR_MAX];
    for (int i = 0; i < DSCM_INDICATOR_MAX; i++) {
      ch[i] = n->challenges[i].id;
      tr[i] = n->trackers[i].id;
    }
    if (n->indicators_generation != gen) { d->stats.torn++; return; }  /* retry next time */
    for (int i = 0; i < DSCM_INDICATOR_MAX; i++) {
      dsd_item_t it;
      memset(&it, 0, sizeof(it));
      if (ch[i] && !has(d->challenge_ids, ch[i])) {
        it.kind = DSD_ITEM_CHALLENGE; it.id = ch[i];
        enqueue(d, it);
      }
      if (tr[i] && !has(d->tracker_ids, tr[i])) {
        it.kind = DSD_ITEM_TRACKER; it.id = tr[i];
        enqueue(d, it);
      }
    }
    /* The tray is up while anything is active. Any change (a tracker's
     * value ticks every frame in a timed run) marks it for a redraw, at
     * most every DSD_TRAY_REDRAW frames. */
    int any = 0;
    for (int i = 0; i < DSCM_INDICATOR_MAX; i++) any |= ch[i] || tr[i];
    if (any) {
      d->tray.kind = DSD_ITEM_TRAY;
      d->tray.phase = DSD_PHASE_HOLD;
      d->tray_dirty = 1;
    } else if (d->tray.kind) {
      memset(&d->tray, 0, sizeof(d->tray));
      if (d->tray_ok && !d->current.kind) d->scene_gen++;
    }
    memcpy(d->challenge_ids, ch, sizeof(ch));
    memcpy(d->tracker_ids, tr, sizeof(tr));
    d->last_gen = gen;
  }
}

/* Index of the next item to show: most important, then oldest. */
static int pick(const dsd_t* d) {
  int best = -1;
  for (int i = 0; i < d->queue_count; i++) {
    if (best < 0 || priority[d->queue[i].kind] < priority[d->queue[best].kind]) best = i;
  }
  return best;  /* queue order is arrival order, so the first of a priority is the oldest */
}

int dsd_busy(const dsd_t* d) {
  return d->current.kind || d->queue_count || (d->tray.kind && d->tray_ok);
}

void dsd_tick(dsd_t* d, uint32_t frame, const dsd_caps_t* caps) {
  uint32_t elapsed = frame - d->frame;
  d->frame = frame;
  if (d->tray_ok != caps->tray_ok) {
    d->tray_ok = caps->tray_ok;
    if (d->tray.kind && !d->current.kind) d->scene_gen++;
  }
  if (d->tray_ok && d->tray.kind && !d->current.kind && d->tray_dirty &&
      frame - d->tray_drawn >= DSD_TRAY_REDRAW) {
    d->tray_dirty = 0;
    d->tray_drawn = frame;
    d->scene_gen++;
  }
  if (d->tray_ok) {
    /* The tray shows active indicators itself; their one-shot flashes would
     * only repeat it. */
    for (int i = d->queue_count - 1; i >= 0; i--)
      if (d->queue[i].kind == DSD_ITEM_CHALLENGE || d->queue[i].kind == DSD_ITEM_TRACKER) remove_at(d, i);
  }
  if (!d->current.kind && !d->queue_count) return;

  /* Waiting counts only frames when nothing can be shown at all (a fade, a
   * card read, no idle slack), not time spent behind other notices, so a
   * burst of unlocks all gets shown. Too long and it goes to missed[] for
   * the menu page (mastery always waits). */
  if (!caps->strip_lines) {
    const uint16_t add = (uint16_t)(elapsed > 0xFFFF ? 0xFFFF : elapsed);
    for (int i = d->queue_count - 1; i >= 0; i--) {
      dsd_item_t* q = &d->queue[i];
      q->stalled = (uint16_t)(q->stalled + add > 0xFFFF ? 0xFFFF : q->stalled + add);
      if (q->kind != DSD_ITEM_MASTERY && q->stalled > DSD_WAIT_MAX) {
        to_missed(d, q);
        remove_at(d, i);
      }
    }
  }

  dsd_item_t* c = &d->current;
  if (c->kind) {
    if (!caps->strip_lines) {
      /* Nothing can show it right now (fade, card read, no idle slack):
       * hold its place; if that lasts, it is missed. */
      const uint32_t st = c->stalled + elapsed;
      c->stalled = (uint16_t)(st > 0xFFFF ? 0xFFFF : st);
      if (c->kind != DSD_ITEM_MASTERY && c->stalled > DSD_WAIT_MAX) {
        to_missed(d, c);
        memset(c, 0, sizeof(*c));
        d->scene_gen++;
      }
    } else {
      while (elapsed && c->kind) {
        const uint32_t step = elapsed < c->frames_left ? elapsed : c->frames_left;
        c->frames_left = (uint16_t)(c->frames_left - step);
        elapsed -= step;
        if (c->frames_left) break;
        if (c->phase == DSD_PHASE_IN) {
          c->phase = DSD_PHASE_HOLD;
          c->frames_left = hold[c->kind];
        } else if (c->phase == DSD_PHASE_HOLD) {
          c->phase = DSD_PHASE_OUT;
          c->frames_left = DSD_FADE_FRAMES;
        } else {
          memset(c, 0, sizeof(*c));
          d->scene_gen++;
        }
      }
    }
  }

  if (!c->kind && caps->strip_lines && d->queue_count) {
    const int i = pick(d);
    *c = d->queue[i];
    remove_at(d, i);
    c->phase = DSD_PHASE_IN;
    c->frames_left = DSD_FADE_FRAMES;
    d->stats.shown++;
    d->scene_gen++;
  }
}

const dsd_item_t* dsd_current(const dsd_t* d) {
  if (!d->current.kind && d->tray.kind && d->tray_ok) return &d->tray;
  return &d->current;
}

uint32_t dsd_scene_gen(const dsd_t* d) { return d->scene_gen; }

int dsd_visible_rows(const dsd_t* d, int h) {
  const dsd_item_t* c = dsd_current(d);
  if (!c->kind) return 0;
  if (c->phase == DSD_PHASE_IN) return h * (DSD_FADE_FRAMES - c->frames_left + 1) / DSD_FADE_FRAMES;
  if (c->phase == DSD_PHASE_OUT) return h * c->frames_left / DSD_FADE_FRAMES;
  return h;
}

const dsd_item_t* dsd_missed(const dsd_t* d, int i) {
  if (i < 0 || i >= DSD_MISSED_MAX) return 0;
  const dsd_item_t* m = &d->missed[(d->missed_head + DSD_MISSED_MAX - i) % DSD_MISSED_MAX];
  return m->kind ? m : 0;
}
