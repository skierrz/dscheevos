/* LAB (strip-lab): the set's 16x16 badges for the notice bar.
 *
 * Before boot, online: badges already in the SD cache (same achievement,
 * same badge name) are reused; the rest are downloaded over one keep-alive
 * connection to the media host, converted (dsdbp_convert_png16) and added to
 * the cache. The session file then carries them all (session_set_badges), so
 * the module draws them from memory: no SD reads or network during play.
 * Nothing here affects achievements, hardcore or what is sent to the server.
 *
 * Cache: sd:/_nds/dscheevos/badges/<game id>.b16 (16x16) or .b32 (32x32, NOTICE_SIZE = big)
 *   "DSC6", version 2, slot size, count, then count x { id, badge name[16], slot } */
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "badges.h"
#include "dscheevos_module.h"
#include "dsd_badgepack.h"
#include "log.h"
#include "net.h"
#include "session.h"
#include "ui.h"

#define CACHE_MAGIC 0x36435344u        /* "DSC6" */
#define CACHE_VERSION 2u
#define MEDIA_HOST "https://media.retroachievements.org"
#define RA_HOST "https://retroachievements.org"

typedef struct entry_t {
  uint32_t id;
  char name[16];
  uint8_t slot[DSCM_BADGES_SLOT32];   /* the first slot_size bytes are used */
} entry_t;

static entry_t* g_entries;           /* the set's, in the list's order */
static int g_count;
static uint8_t* g_block;               /* for the session file */

static uint32_t g_slot;                /* DSCM_BADGES_SLOT or DSCM_BADGES_SLOT32 */

static void cache_path(char* out, size_t size, uint32_t game_id, int tmp)
{
  snprintf(out, size, DSC_DIR "badges/%lu.b%d%s", (unsigned long)game_id, g_slot == DSCM_BADGES_SLOT32 ? 32 : 16,
           tmp ? ".tmp" : "");
}

/* The cached entries (caller frees), or 0. */
static entry_t* cache_read(uint32_t game_id, uint32_t* count)
{
  char path[96];
  uint32_t head[4];
  entry_t* e = NULL;
  FILE* f;
  cache_path(path, sizeof(path), game_id, 0);
  f = fopen(path, "rb");
  *count = 0;
  if (!f)
    return NULL;
  if (fread(head, 1, sizeof(head), f) == sizeof(head) && head[0] == CACHE_MAGIC && head[1] == CACHE_VERSION
      && head[2] == g_slot && head[3] <= DSCM_BADGES_MAX * 2) {
    uint32_t i;
    e = (entry_t*)malloc(head[3] * sizeof(entry_t) + 1);
    for (i = 0; e && i < head[3]; i++)
      if (fread(&e[i].id, 1, 4, f) != 4 || fread(e[i].name, 1, 16, f) != 16 || fread(e[i].slot, 1, g_slot, f) != g_slot)
        break;
    if (e && i == head[3])
      *count = head[3];
    else {
      free(e);
      e = NULL;
    }
  }
  fclose(f);
  return e;
}

/* Written to a .tmp and renamed: a broken write never replaces a good cache. */
static void cache_write(uint32_t game_id, const entry_t* e, const uint8_t* ok, int count)
{
  char path[96], tmp[96];
  uint32_t head[4] = { CACHE_MAGIC, CACHE_VERSION, g_slot, 0 };
  FILE* f;
  int i, bad;
  for (i = 0; i < count; i++) head[3] += ok[i];
  mkdir(DSC_DIR "badges", 0777);
  cache_path(tmp, sizeof(tmp), game_id, 1);
  cache_path(path, sizeof(path), game_id, 0);
  f = fopen(tmp, "wb");
  if (!f)
    return;
  fwrite(head, 1, sizeof(head), f);
  for (i = 0; i < count; i++)
    if (ok[i]) {
      fwrite(&e[i].id, 1, 4, f);
      fwrite(e[i].name, 1, 16, f);
      fwrite(e[i].slot, 1, g_slot, f);
    }
  bad = ferror(f);
  fclose(f);
  if (bad) {
    remove(tmp);
    return;
  }
  remove(path);
  rename(tmp, path);
}

static int cmp_entry(const void* a, const void* b)
{
  const uint32_t x = ((const entry_t*)a)->id, y = ((const entry_t*)b)->id;
  return x < y ? -1 : x > y;
}

void badges_prepare(const dsc_settings_t* settings, uint32_t game_id)
{
  const session_badge_t* list;
  const int n = session_badges(&list);
  uint32_t cached_n = 0, block_len;
  entry_t* cached;
  uint8_t* ok;
  int i, have = 0, fetched = 0, failed = 0, skipped = 0;
  char media[96];

  session_set_badges(NULL, 0);
  free(g_entries);
  free(g_block);
  g_entries = NULL;
  g_block = NULL;
  g_count = 0;
  if (n <= 0 || !settings->badges)
    return;
  g_slot = settings->notice_big ? DSCM_BADGES_SLOT32 : DSCM_BADGES_SLOT;   /* the badge fits the bar */
  g_entries = (entry_t*)calloc((size_t)n, sizeof(entry_t));
  ok = (uint8_t*)calloc((size_t)n, 1);
  if (!g_entries || !ok) {
    free(ok);
    return;
  }
  g_count = n;

  /* reuse what the cache has for the same badge */
  cached = cache_read(game_id, &cached_n);
  for (i = 0; i < n; i++) {
    uint32_t k;
    g_entries[i].id = list[i].id;
    memcpy(g_entries[i].name, list[i].name, sizeof(g_entries[i].name));
    for (k = 0; k < cached_n; k++)
      if (cached[k].id == list[i].id && !strncmp(cached[k].name, list[i].name, sizeof(cached[k].name))) {
        memcpy(g_entries[i].slot, cached[k].slot, g_slot);
        ok[i] = 1;
        have++;
        break;
      }
  }
  free(cached);

  /* the rest over one connection; B skips (they show as icons) */
  if (have == n)
    log_line("badges: all %d cached", n);
  if (have < n) {
    http_ka_t* ka = http_ka_open();
    const int t0 = (int)time(NULL);
    /* RetroAchievements' badges come from its media host; a test or
     * self-hosted server serves them itself */
    snprintf(media, sizeof(media), "%s", strcmp(settings->host, RA_HOST) ? settings->host : MEDIA_HOST);
    for (i = 0; ka && i < n; i++) {
      char url[160];
      http_response_t r;
      if (ok[i])
        continue;
      scanKeys();
      if (keysHeld() & KEY_B) {
        skipped = n - have - fetched - failed;
        break;
      }
      ui_bottom("\x1b[2;0H Badges %d/%d   \n\n B: skip\n", have + fetched + failed + 1, n);
      snprintf(url, sizeof(url), "%s/Badge/%s.png", media, list[i].name);
      http_ka_get(ka, url, &r);
      if (r.status == 200 && r.body
          && (g_slot == DSCM_BADGES_SLOT32 ? dsdbp_convert_png32c(r.body, r.length, (dsd_badge_t*)g_entries[i].slot)
                                           : dsdbp_convert_png16(r.body, r.length, (dsd_badge16_t*)g_entries[i].slot)) == 0) {
        ok[i] = 1;
        fetched++;
      } else {
        failed++;
        if (failed == 1)
          log_line("badges: %s: status %d (%s)", list[i].name, r.status, r.status < 0 ? net_error() : "");
        if (r.status < 0 && failed >= 3 && !fetched) {   /* no connection at all: stop trying */
          skipped = n - have - fetched - failed;
          http_free(&r);
          break;
        }
      }
      http_free(&r);
    }
    http_ka_close(ka);
    log_line("badges: %d cached, %d fetched, %d failed, %d skipped, %d s", have, fetched, failed, skipped,
             (int)time(NULL) - t0);
    ui_clear_bottom();
    if (fetched)
      cache_write(game_id, g_entries, ok, n);
  }

  /* the session's block: ids ascending, then the slots */
  {
    int m = 0;
    entry_t* sorted = (entry_t*)malloc((size_t)n * sizeof(entry_t));
    if (!sorted) {
      free(ok);
      return;
    }
    for (i = 0; i < n; i++)
      if (ok[i])
        sorted[m++] = g_entries[i];
    qsort(sorted, (size_t)m, sizeof(entry_t), cmp_entry);
    if (m > (int)DSCM_BADGES_MAX)
      m = DSCM_BADGES_MAX;
    block_len = (uint32_t)(sizeof(dscm_badges_t) + (size_t)m * (4 + g_slot));
    g_block = (uint8_t*)malloc(block_len);
    if (g_block && m) {
      dscm_badges_t* b = (dscm_badges_t*)g_block;
      uint8_t* slots = (uint8_t*)&b->ids[m];
      b->magic = g_slot == DSCM_BADGES_SLOT32 ? DSCM_BADGES_MAGIC32 : DSCM_BADGES_MAGIC;
      b->count = (uint32_t)m;
      b->slot_size = g_slot;
      for (i = 0; i < m; i++) {
        b->ids[i] = sorted[i].id;
        memcpy(slots + (size_t)i * g_slot, sorted[i].slot, g_slot);
      }
      session_set_badges(g_block, block_len);
    }
    free(sorted);
  }
  free(ok);
}
