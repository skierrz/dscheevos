/* Host tests for the display library: scheduler properties (D1) and golden
 * images of every layout (D2). Run from tests/ (make test); output PNGs go to
 * the directory in argv[1], goldens live in tests/golden. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "dsd.h"
#include "png.h"

extern const dsd_font_t dsd_font_latin;

static int checks, failures;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ---- a fake module: the notice ring and the strings rc_client would hold */

static dscm_notices_t notices;

typedef struct { uint32_t id; const char* title; const char* detail; } fake_string_t;
static const fake_string_t strings[] = {
  { 101, "Rising Star", "Clear World 1 without taking damage" },
  { 102, "Pokémon Señor: Ünïcødé Àççénts", "Défeat the Élite Four" },
  { 103, "A Remarkably Long Achievement Title That Will Not Fit On One Strip Line",
         "An equally long description that runs well past the right edge of the screen" },
  { 104, "ポケモン", "Japanese title, not in the Latin font" },
  { 105, "Collector", "Collect 30 coins" },
  { 201, "Fastest Lap - Figure-8 Circuit", "1:02.345" },
  { 301, "Hold the Line", "" },
  { 900, "Super Mario 64 DS", "42/150 achievements" },
};

static const char* fake_text(void* ctx, const dsd_item_t* it, int which) {
  (void)ctx;
  if (it->kind == DSD_ITEM_MODE || it->kind == DSD_ITEM_MASTERY) {
    return which == DSD_TEXT_TITLE ? "Super Mario 64 DS" : "42/150 achievements";
  }
  if (it->kind == DSD_ITEM_OFFLINE) return which == DSD_TEXT_TITLE ? "Unlocks are kept and sent later" : "";
  if (it->kind == DSD_ITEM_ONLINE) return which == DSD_TEXT_TITLE ? "Everything waiting was sent" : "";
  if (it->kind == DSD_ITEM_PROGRESS && which == DSD_TEXT_DETAIL) return "12/30";
  if (it->kind == DSD_ITEM_TRACKER && which == DSD_TEXT_DETAIL) return "1:02.35";
  if (it->kind == DSD_ITEM_LB_SUBMITTED && which == DSD_TEXT_DETAIL) return "1:02.345";
  for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++)
    if (strings[i].id == it->id) return which == DSD_TEXT_TITLE ? strings[i].title : strings[i].detail;
  return "";
}

static void reset_notices(void) {
  memset(&notices, 0, sizeof(notices));
  notices.magic = DSCM_NOTICES_MAGIC;
}

static void raise_notice(uint16_t kind, uint32_t id, int32_t value) {
  const uint32_t seq = notices.head + 1;
  dscm_notice_t* e = &notices.ring[seq % DSCM_NOTICE_RING];
  memset(e, 0, sizeof(*e));
  e->seq = seq;
  e->kind = kind;
  e->id = id;
  e->value = value;
  notices.head = seq;
}

static void init(dsd_t* d, int hardcore) {
  dsd_config_t cfg = { .font = &dsd_font_latin, .text = fake_text, .hardcore = (uint8_t)hardcore };
  dsd_init(d, &cfg);
}

static const dsd_caps_t SHOW = { 32, 0 };
static const dsd_caps_t HIDDEN = { 0, 0 };

/* Runs frames until nothing is on screen or queued; returns the kinds shown in order. */
static int drain(dsd_t* d, uint32_t* frame, uint8_t* order, int max) {
  int n = 0;
  uint32_t last_gen = dsd_scene_gen(d);
  for (int i = 0; i < 100000 && dsd_busy(d); i++) {
    dsd_tick(d, ++*frame, &SHOW);
    if (dsd_scene_gen(d) != last_gen) {
      last_gen = dsd_scene_gen(d);
      if (dsd_current(d)->kind && n < max) order[n++] = dsd_current(d)->kind;
    }
  }
  return n;
}

/* ---- D1: scheduler */

static void test_idle_is_cheap(void) {
  dsd_t d;
  reset_notices();
  init(&d, 1);
  dsd_ingest(&d, &notices);
  dsd_tick(&d, 1, &SHOW);
  CHECK(!dsd_busy(&d));
  CHECK(dsd_current(&d)->kind == DSD_ITEM_NONE);
  CHECK(dsd_visible_rows(&d, 32) == 0);
  dsd_ingest(&d, 0);                  /* no notices yet: nothing happens */
  notices.magic = 0;
  raise_notice(DSCM_NOTICE_UNLOCK, 101, 10);
  dsd_ingest(&d, &notices);           /* bad magic: ignored */
  CHECK(!dsd_busy(&d));
}

static void test_priority_and_order(void) {
  dsd_t d;
  uint32_t frame = 0;
  reset_notices();
  init(&d, 1);
  raise_notice(DSCM_NOTICE_PROGRESS, 105, 0);
  raise_notice(DSCM_NOTICE_UNLOCK, 101, 10);
  raise_notice(DSCM_NOTICE_UNLOCK, 102, 25);
  raise_notice(DSCM_NOTICE_MASTERY, 0, 0);
  dsd_ingest(&d, &notices);
  uint8_t order[8];
  const int n = drain(&d, &frame, order, 8);
  CHECK(n == 4);
  CHECK(order[0] == DSD_ITEM_MASTERY);
  CHECK(order[1] == DSD_ITEM_UNLOCK);
  CHECK(order[2] == DSD_ITEM_UNLOCK);
  CHECK(order[3] == DSD_ITEM_PROGRESS);
  CHECK(d.stats.shown == 4);
}

static void test_unlock_timing(void) {
  dsd_t d;
  reset_notices();
  init(&d, 1);
  raise_notice(DSCM_NOTICE_UNLOCK, 101, 10);
  dsd_ingest(&d, &notices);
  uint32_t frame = 100;
  dsd_tick(&d, frame, &SHOW);         /* starts showing */
  CHECK(dsd_current(&d)->kind == DSD_ITEM_UNLOCK);
  CHECK(dsd_visible_rows(&d, 32) == 8);   /* first fade-in frame: a quarter */
  int frames_up = 0;
  while (dsd_current(&d)->kind && frames_up < 10000) { dsd_tick(&d, ++frame, &SHOW); frames_up++; }
  CHECK(frames_up == DSD_FADE_FRAMES + 180 + DSD_FADE_FRAMES);
}

static void test_skipped_frames(void) {
  /* Ticks may skip frames (a lag frame, a long idle pass): time still counts. */
  dsd_t d;
  reset_notices();
  init(&d, 1);
  raise_notice(DSCM_NOTICE_UNLOCK, 101, 10);
  dsd_ingest(&d, &notices);
  dsd_tick(&d, 1000, &SHOW);
  dsd_tick(&d, 1000 + 100, &SHOW);
  CHECK(dsd_current(&d)->phase == DSD_PHASE_HOLD);
  dsd_tick(&d, 1000 + 1000, &SHOW);
  CHECK(dsd_current(&d)->kind == DSD_ITEM_NONE);
}

static void test_nothing_lost_on_overflow(void) {
  dsd_t d;
  uint32_t frame = 0;
  reset_notices();
  init(&d, 1);
  /* 12 unlocks and 3 progress updates arrive at once (ring holds 16). */
  for (int i = 0; i < 12; i++) raise_notice(DSCM_NOTICE_UNLOCK, 101, 5);
  for (int i = 0; i < 3; i++) raise_notice(DSCM_NOTICE_PROGRESS, 105, 0);
  dsd_ingest(&d, &notices);
  uint8_t order[32];
  const int n = drain(&d, &frame, order, 32);
  /* Every unlock is shown or merged into a "+N more"; the progress updates
   * collapse into one. Nothing vanishes without a count. */
  uint32_t more = 0;
  CHECK(d.stats.merged + d.stats.shown + d.stats.missed >= 12);
  for (int i = 0; i < n; i++) if (order[i] == DSD_ITEM_UNLOCK) more++;
  CHECK(more + d.stats.merged == 12);
  /* The queue was full of unlocks, so the less important progress updates
   * went to missed[] (listed on the menu page), counted. */
  CHECK(d.stats.missed == 3);
  for (int i = 0; i < 3; i++) CHECK(dsd_missed(&d, i) && dsd_missed(&d, i)->kind == DSD_ITEM_PROGRESS);
}

static void test_ring_overwritten(void) {
  dsd_t d;
  reset_notices();
  init(&d, 1);
  for (int i = 0; i < 20; i++) raise_notice(DSCM_NOTICE_PROGRESS, 105, 0);
  dsd_ingest(&d, &notices);
  CHECK(d.stats.overwritten == 4);
  CHECK(d.queue_count == 1);          /* progress collapses */
}

static void test_torn_entry(void) {
  dsd_t d;
  reset_notices();
  init(&d, 1);
  raise_notice(DSCM_NOTICE_UNLOCK, 101, 10);
  raise_notice(DSCM_NOTICE_UNLOCK, 102, 10);
  notices.ring[2 % DSCM_NOTICE_RING].seq = 99;   /* being rewritten */
  dsd_ingest(&d, &notices);
  CHECK(d.stats.torn == 1);
  CHECK(d.queue_count == 1);
}

static void test_game_reset(void) {
  dsd_t d;
  reset_notices();
  init(&d, 1);
  for (int i = 0; i < 5; i++) raise_notice(DSCM_NOTICE_PROGRESS, 105, 0);
  dsd_ingest(&d, &notices);
  reset_notices();                    /* the module started over */
  raise_notice(DSCM_NOTICE_UNLOCK, 101, 10);
  dsd_ingest(&d, &notices);
  int unlocks = 0;
  for (int i = 0; i < d.queue_count; i++) unlocks += d.queue[i].kind == DSD_ITEM_UNLOCK;
  CHECK(unlocks == 1);
}

static void test_leaderboard_started_after_failed(void) {
  dsd_t d;
  uint32_t frame = 0;
  reset_notices();
  init(&d, 1);
  raise_notice(DSCM_NOTICE_LEADERBOARD_STARTED, 201, 0);
  raise_notice(DSCM_NOTICE_LEADERBOARD_FAILED, 201, 0);
  raise_notice(DSCM_NOTICE_LEADERBOARD_STARTED, 201, 0);   /* a new attempt */
  dsd_ingest(&d, &notices);
  uint8_t order[8];
  const int n = drain(&d, &frame, order, 8);
  CHECK(n == 2);
  CHECK(order[0] == DSD_ITEM_LB_FAILED);
  CHECK(order[1] == DSD_ITEM_LB_STARTED);
}

static void test_stalled_goes_to_missed(void) {
  dsd_t d;
  reset_notices();
  init(&d, 1);
  raise_notice(DSCM_NOTICE_UNLOCK, 101, 10);
  raise_notice(DSCM_NOTICE_MASTERY, 0, 0);
  dsd_ingest(&d, &notices);
  uint32_t frame = 1;
  dsd_tick(&d, frame, &SHOW);          /* mastery goes up */
  CHECK(dsd_current(&d)->kind == DSD_ITEM_MASTERY);
  for (int i = 0; i < 2000; i++) dsd_tick(&d, ++frame, &HIDDEN);   /* a long fade, say */
  /* Mastery waits for as long as it takes; the queued unlock went to missed. */
  CHECK(dsd_current(&d)->kind == DSD_ITEM_MASTERY);
  CHECK(d.stats.missed == 1);
  CHECK(dsd_missed(&d, 0) && dsd_missed(&d, 0)->kind == DSD_ITEM_UNLOCK);
  CHECK(dsd_missed(&d, 1) == 0);
}

static void test_indicators(void) {
  dsd_t d;
  uint32_t frame = 0;
  reset_notices();
  init(&d, 1);
  notices.challenges[0].id = 301;
  notices.trackers[0].id = 201;
  notices.indicators_generation++;
  dsd_ingest(&d, &notices);
  uint8_t order[8];
  int n = drain(&d, &frame, order, 8);
  CHECK(n == 2);                      /* one flash each */
  CHECK(order[0] == DSD_ITEM_CHALLENGE && order[1] == DSD_ITEM_TRACKER);

  /* Same indicators again (a tracker value changed): no new flash. */
  notices.indicators_generation++;
  dsd_ingest(&d, &notices);
  CHECK(d.queue_count == 0);

  /* With pinning allowed, the tracker stays up while nothing else shows. */
  const dsd_caps_t pin = { 16, 1 };
  dsd_tick(&d, ++frame, &pin);
  CHECK(dsd_current(&d)->kind == DSD_ITEM_TRACKER && dsd_current(&d)->id == 201);
  CHECK(dsd_busy(&d));
  /* An unlock takes over, then the tracker comes back. */
  raise_notice(DSCM_NOTICE_UNLOCK, 101, 10);
  dsd_ingest(&d, &notices);
  dsd_tick(&d, ++frame, &pin);
  CHECK(dsd_current(&d)->kind == DSD_ITEM_UNLOCK);
  for (int i = 0; i < 400; i++) dsd_tick(&d, ++frame, &pin);
  CHECK(dsd_current(&d)->kind == DSD_ITEM_TRACKER);
  /* The leaderboard ends: the pin goes. */
  notices.trackers[0].id = 0;
  notices.indicators_generation++;
  dsd_ingest(&d, &notices);
  dsd_tick(&d, ++frame, &pin);
  CHECK(dsd_current(&d)->kind == DSD_ITEM_NONE);
  CHECK(!dsd_busy(&d));
}

static void test_text(void) {
  uint8_t g[64];
  int n = dsd_text_to_glyphs(&dsd_font_latin, "Abc", g, 64);
  CHECK(n == 3 && g[0] == 'A' && g[2] == 'c');
  n = dsd_text_to_glyphs(&dsd_font_latin, "é", g, 64);
  CHECK(n == 1 && g[0] >= 128 && dsd_font_latin.codepoints[g[0]] == 0xE9);
  n = dsd_text_to_glyphs(&dsd_font_latin, "ポ", g, 64);
  CHECK(n == 1 && g[0] == '?');
  n = dsd_text_to_glyphs(&dsd_font_latin, "\xC3", g, 64);    /* cut UTF-8 */
  CHECK(n == 1 && g[0] == '?');
  n = dsd_text_to_glyphs(&dsd_font_latin, "abcdefgh", g, 5);
  CHECK(n == 5 && g[1] == 'b' && g[2] == '.' && g[4] == '.');
  n = dsd_text_to_glyphs(&dsd_font_latin, "it\xE2\x80\x99s", g, 64);   /* ’ */
  CHECK(n == 4 && g[2] == '\'');
  n = dsd_text_to_glyphs(&dsd_font_latin, 0, g, 64);
  CHECK(n == 0);
}

/* ---- D2: golden images */

static const char* out_dir;
static int update_golden;

static int same_file(const char* a, const char* b) {
  FILE* fa = fopen(a, "rb");
  FILE* fb = fopen(b, "rb");
  int same = fa && fb;
  while (same) {
    const int ca = fgetc(fa), cb = fgetc(fb);
    if (ca != cb) same = 0;
    if (ca == EOF || cb == EOF) break;
  }
  if (fa) fclose(fa);
  if (fb) fclose(fb);
  return same;
}

static void copy_file(const char* from, const char* to) {
  FILE* a = fopen(from, "rb");
  FILE* b = fopen(to, "wb");
  int c;
  while (a && b && (c = fgetc(a)) != EOF) fputc(c, b);
  if (a) fclose(a);
  if (b) fclose(b);
}

static void golden(const char* name, const uint16_t* px, int w, int h) {
  char out[512], gold[512];
  snprintf(out, sizeof(out), "%s/%s.png", out_dir, name);
  snprintf(gold, sizeof(gold), "golden/%s.png", name);
  CHECK(png_write_bgr555(out, px, w, h, w) == 0);
  if (update_golden) {
    copy_file(out, gold);
    return;
  }
  checks++;
  if (!same_file(out, gold)) {
    failures++;
    printf("FAIL golden %s differs (see %s; make test UPDATE_GOLDEN=1 after review)\n", name, out);
  }
}

/* Contact sheet: every strip over a stand-in game screen, for review by eye. */
#define SHEET_W 256
#define SHEET_H 1200
static uint16_t sheet[SHEET_W * SHEET_H];
static int sheet_y;

static void sheet_add(const uint16_t* px, int h) {
  if (sheet_y + h + 4 > SHEET_H) return;
  memcpy(sheet + sheet_y * SHEET_W, px, (size_t)h * SHEET_W * 2);
  sheet_y += h + 4;
}

static int case_side;                 /* DSD_SIDE_* for render_case */

static void render_case(const char* name, uint8_t notice, uint32_t id, int32_t value, uint16_t more,
                        int h, int hardcore) {
  dsd_t d;
  reset_notices();
  init(&d, hardcore);
  d.cfg.side = (uint8_t)case_side;
  raise_notice(notice, id, value);
  dsd_ingest(&d, &notices);
  if (more) d.queue[0].more = more;
  dsd_tick(&d, 1, &SHOW);
  CHECK(dsd_current(&d)->kind != DSD_ITEM_NONE);

  static uint16_t px[256 * 32], px2[256 * 32];
  dsd_surface_t s = { px, 256, (uint16_t)h, 256 };
  CHECK(dsd_render(&d, &s, 999) == 1);

  /* Rendering in bands of 3 rows gives the same pixels. */
  dsd_t d2;
  reset_notices();
  init(&d2, hardcore);
  d2.cfg.side = (uint8_t)case_side;
  raise_notice(notice, id, value);
  dsd_ingest(&d2, &notices);
  if (more) d2.queue[0].more = more;
  dsd_tick(&d2, 1, &SHOW);
  dsd_surface_t s2 = { px2, 256, (uint16_t)h, 256 };
  int calls = 0;
  while (!dsd_render(&d2, &s2, 3)) calls++;
  CHECK(calls == (h + 2) / 3 - 1);
  CHECK(memcmp(px, px2, (size_t)h * 256 * 2) == 0);

  golden(name, px, 256, h);
  sheet_add(px, h);
}

static void test_goldens(void) {
  render_case("unlock", DSCM_NOTICE_UNLOCK, 101, 10, 0, 32, 1);
  render_case("unlock_accents", DSCM_NOTICE_UNLOCK, 102, 25, 0, 32, 1);
  render_case("unlock_long", DSCM_NOTICE_UNLOCK, 103, 100, 0, 32, 1);
  render_case("unlock_japanese", DSCM_NOTICE_UNLOCK, 104, 5, 0, 32, 1);
  render_case("unlock_more", DSCM_NOTICE_UNLOCK, 101, 10, 3, 32, 1);
  render_case("mastery", DSCM_NOTICE_MASTERY, 0, 0, 0, 32, 1);
  render_case("mode_hardcore", DSCM_NOTICE_SESSION_START, 0, 1, 0, 32, 1);
  render_case("mode_softcore", DSCM_NOTICE_SESSION_START, 0, 0, 0, 32, 0);
  render_case("progress", DSCM_NOTICE_PROGRESS, 105, 0, 0, 32, 1);
  render_case("lb_started", DSCM_NOTICE_LEADERBOARD_STARTED, 201, 0, 0, 32, 1);
  render_case("lb_failed", DSCM_NOTICE_LEADERBOARD_FAILED, 201, 0, 0, 32, 1);
  render_case("lb_submitted", DSCM_NOTICE_LEADERBOARD_SUBMITTED, 201, 62345, 0, 32, 1);
  render_case("lb_rank", DSCM_NOTICE_LEADERBOARD_RANK, 201, 17, 0, 32, 1);
  render_case("offline", DSCM_NOTICE_OFFLINE, 0, 0, 0, 32, 1);
  render_case("online", DSCM_NOTICE_ONLINE, 0, 0, 0, 32, 1);
  render_case("compact_unlock", DSCM_NOTICE_UNLOCK, 101, 10, 0, 16, 1);
  render_case("compact_progress", DSCM_NOTICE_PROGRESS, 105, 0, 0, 16, 1);
  render_case("compact_long", DSCM_NOTICE_UNLOCK, 103, 100, 0, 16, 1);
  case_side = DSD_SIDE_RIGHT;
  render_case("right_unlock", DSCM_NOTICE_UNLOCK, 101, 10, 0, 32, 1);
  render_case("right_progress", DSCM_NOTICE_PROGRESS, 105, 0, 0, 32, 1);
  render_case("right_compact", DSCM_NOTICE_UNLOCK, 101, 10, 0, 16, 1);
  case_side = DSD_SIDE_LEFT;

  char path[512];
  snprintf(path, sizeof(path), "%s/sheet.png", out_dir);
  png_write_bgr555(path, sheet, SHEET_W, sheet_y, SHEET_W);
}

/* ---- P1: the menu's Now page */

static void grid_to_pixels(const uint16_t* map, int cols, int rows, uint16_t* px) {
  static const uint16_t pal[] = { 0x7FFF, 0x5AD6, 0x3DEF, 0x7EA8, 0x2D1F, 0x2BEA };
  for (int ty = 0; ty < rows; ty++)
    for (int tx = 0; tx < cols; tx++) {
      const uint16_t e = map[ty * 32 + tx];
      const uint8_t* glyph = dsd_font_latin.glyphs + (e & 0xFF) * 8;
      for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
          px[(ty * 8 + y) * 256 + tx * 8 + x] = glyph[y] >> x & 1 ? pal[e >> 12] : 0x0C63;
    }
}

static void test_now_page(void) {
  dsd_t d;
  reset_notices();
  init(&d, 1);
  notices.challenges[0].id = 301;
  strcpy(notices.challenges[0].text, "Hold the Line");
  notices.trackers[0].id = 201;
  strcpy(notices.trackers[0].text, "1:02.35");
  notices.indicators_generation++;

  static uint8_t list_mem[sizeof(dscm_list_t) + 6 * sizeof(dscm_list_entry_t)];
  dscm_list_t* l = (dscm_list_t*)list_mem;
  l->magic = DSCM_LIST_MAGIC;
  l->entry_size = sizeof(dscm_list_entry_t);
  l->count = 6;
  static const struct { const char* t; const char* m; uint8_t st; uint32_t time; uint16_t pts; } e[] = {
    { "Collector", "12/30", DSCM_ACH_LOCKED, 0, 10 },
    { "Star Hunter", "118/120", DSCM_ACH_LOCKED, 0, 25 },
    { "Halfway", "50%", DSCM_ACH_LOCKED, 0, 5 },
    { "Rising Star", "", DSCM_ACH_UNLOCKED, 1000, 10 },
    { "Pokémon Señor", "", DSCM_ACH_UNLOCKED, 2000, 25 },
    { "Not measured", "", DSCM_ACH_LOCKED, 0, 5 },
  };
  for (int i = 0; i < 6; i++) {
    dscm_list_entry_t* x = &l->e[i];
    memset(x, 0, sizeof(*x));
    x->id = (uint32_t)(500 + i);
    strcpy(x->title, e[i].t);
    strcpy(x->measured, e[i].m);
    x->state = e[i].st;
    x->unlock_time = e[i].time;
    x->points = e[i].pts;
    if (*e[i].m) x->flags |= DSCM_ACH_FLAG_MEASURED;
  }
  /* A notice that never got shown. */
  raise_notice(DSCM_NOTICE_UNLOCK, 102, 25);
  dsd_ingest(&d, &notices);
  for (uint32_t f = 1; f < 700; f++) dsd_tick(&d, f, &HIDDEN);

  static uint16_t map[32 * 24];
  dsd_grid_t g = { map, 30, 22, 32 };
  const int total = dsd_render_now(&d, &notices, l, &g, 0);
  CHECK(total > 15 && total < 40);
  /* "Almost there" is ordered by progress: Star Hunter (98%) before Halfway. */
  int star = -1, half = -1;
  for (int r = 0; r < 22; r++) {
    if ((map[r * 32 + 1] & 0xFF) == 'S' && (map[r * 32 + 2] & 0xFF) == 't') star = r;
    if ((map[r * 32 + 1] & 0xFF) == 'H' && (map[r * 32 + 2] & 0xFF) == 'a') half = r;
  }
  CHECK(star >= 0 && half > star);

  static uint16_t px[256 * 192];
  for (int i = 0; i < 256 * 192; i++) px[i] = 0x0C63;
  grid_to_pixels(map, 30, 22, px);
  golden("now_page", px, 256, 192);

  /* Scrolling past the end clears the area. */
  dsd_render_now(&d, &notices, l, &g, 1000);
  CHECK((map[0] & 0xFF) == ' ');
}

int main(int argc, char** argv) {
  out_dir = argc > 1 ? argv[1] : "out";
  mkdir(out_dir, 0755);
  const char* u = getenv("UPDATE_GOLDEN");
  update_golden = u && *u && strcmp(u, "0") != 0;
  if (update_golden) mkdir("golden", 0755);
  for (int i = 0; i < SHEET_W * SHEET_H; i++) sheet[i] = 0x2108;

  test_idle_is_cheap();
  test_priority_and_order();
  test_unlock_timing();
  test_skipped_frames();
  test_nothing_lost_on_overflow();
  test_ring_overwritten();
  test_torn_entry();
  test_game_reset();
  test_leaderboard_started_after_failed();
  test_stalled_goes_to_missed();
  test_indicators();
  test_text();
  test_goldens();
  test_now_page();

  printf("%d checks, %d failed%s\n", checks, failures, update_golden ? " (goldens updated)" : "");
  return failures != 0;
}
