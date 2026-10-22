/* Host tests for the badge pipeline (make test BADGES=1): PNG to slot
 * conversion, the pack file, and badges drawn in a notice. Synthetic images
 * only; RA's badges aren't ours to commit. With DSD_BADGE_SAMPLES=<dir>, every
 * PNG there is also converted into out/samples.png for review by eye. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "dsd.h"
#include "dsd_badgepack.h"
#include "png.h"

extern const dsd_font_t dsd_font_latin;

static int checks, failures;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const char* out_dir;
static int update_golden;

/* ---- helpers */

static uint8_t* read_file(const char* path, size_t* len) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  *len = (size_t)ftell(f);
  rewind(f);
  uint8_t* b = malloc(*len ? *len : 1);
  if (fread(b, 1, *len, f) != *len) { free(b); b = NULL; }
  fclose(f);
  return b;
}

static int same_file(const char* a, const char* b) {
  size_t na, nb;
  uint8_t* x = read_file(a, &na);
  uint8_t* y = read_file(b, &nb);
  const int same = x && y && na == nb && !memcmp(x, y, na);
  free(x);
  free(y);
  return same;
}

static void golden(const char* name, const uint16_t* px, int w, int h) {
  char out[512], gold[512];
  snprintf(out, sizeof(out), "%s/%s.png", out_dir, name);
  snprintf(gold, sizeof(gold), "golden/%s.png", name);
  CHECK(png_write_bgr555(out, px, w, h, w) == 0);
  if (update_golden) {
    size_t n;
    uint8_t* b = read_file(out, &n);
    FILE* f = fopen(gold, "wb");
    if (b && f) fwrite(b, 1, n, f);
    if (f) fclose(f);
    free(b);
    return;
  }
  checks++;
  if (!same_file(out, gold)) {
    failures++;
    printf("FAIL golden %s differs (see %s; make test BADGES=1 UPDATE_GOLDEN=1 after review)\n", name, out);
  }
}

/* A PNG of the pixels, read back as bytes (the path a downloaded badge takes). */
static uint8_t* make_png(const uint8_t* px, int w, int h, int comp, size_t* len) {
  char path[512];
  snprintf(path, sizeof(path), "%s/synthetic.png", out_dir);
  png_write_rgb(path, px, w, h, comp);
  return read_file(path, len);
}

static void rgb_of(uint16_t c, int* r, int* g, int* b) { *r = c & 31; *g = (c >> 5) & 31; *b = (c >> 10) & 31; }
static int px_of(const dsd_badge_t* b, int x, int y) {
  const uint8_t two = b->pixels[(y * 32 + x) >> 1];
  return x & 1 ? two >> 4 : two & 15;
}

/* Synthetic badges: a disc with a rim and a highlight over a gradient. */
static void synth(uint8_t* px, int w, int h, int comp, int hue) {
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      uint8_t* p = px + ((size_t)y * w + x) * comp;
      const int dx = x * 64 / w - 32, dy = y * 64 / h - 32, d2 = dx * dx + dy * dy;
      int r = 20 + y * 60 / h, g = 30 + x * 40 / w, b = 60;
      if (d2 < 26 * 26) { r = (hue & 1) ? 230 : 90; g = (hue & 2) ? 190 : 60; b = (hue & 4) ? 220 : 40;
                          r -= (dx + dy) / 2; g -= (dx + dy) / 2; }
      if (d2 >= 22 * 22 && d2 < 26 * 26) { r = 250; g = 220; b = 120; }
      if ((dx + 10) * (dx + 10) + (dy + 10) * (dy + 10) < 30) { r = g = b = 255; }
      p[0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
      p[1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
      p[2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
      if (comp == 4) p[3] = (uint8_t)(d2 < 30 * 30 ? 255 : 0);
    }
}

/* ---- conversion */

static void test_exact_colours(void) {
  /* 4 colours in 2x2 blocks: downscaling is exact, so every pixel must keep
   * its colour (at 5 bits). */
  static uint8_t px[64 * 64 * 3];
  static const uint8_t cols[4][3] = { {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255} };
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++)
      memcpy(px + (y * 64 + x) * 3, cols[((x / 2) + (y / 2) * 3) % 4], 3);
  size_t len;
  uint8_t* png = make_png(px, 64, 64, 3, &len);
  dsd_badge_t b;
  CHECK(dsdbp_convert_png(png, len, &b) == 0);
  free(png);
  int wrong = 0;
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++) {
      int r, g, bl;
      rgb_of(b.palette[px_of(&b, x, y)], &r, &g, &bl);
      const uint8_t* c = cols[(x + y * 3) % 4];
      if (r != c[0] * 31 / 255 || g != c[1] * 31 / 255 || bl != c[2] * 31 / 255) wrong++;
    }
  CHECK(wrong == 0);
}

static void test_gradient_error(void) {
  /* A smooth 64x64 image: 16 colours must stay close (mean error per channel
   * in 5-bit units). */
  static uint8_t px[64 * 64 * 3];
  synth(px, 64, 64, 3, 3);
  dsd_badge_t b, b2;
  CHECK(dsdbp_convert_pixels(px, 64, 64, 3, &b) == 0);
  CHECK(dsdbp_convert_pixels(px, 64, 64, 3, &b2) == 0);
  CHECK(memcmp(&b, &b2, sizeof(b)) == 0);           /* deterministic */
  long err = 0;
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++) {
      int sum[3] = { 0, 0, 0 };
      for (int k = 0; k < 4; k++) {
        const uint8_t* p = px + (((y * 2 + k / 2) * 64) + x * 2 + k % 2) * 3;
        for (int c = 0; c < 3; c++) sum[c] += p[c];
      }
      int r, g, bl;
      rgb_of(b.palette[px_of(&b, x, y)], &r, &g, &bl);
      err += labs(r - (sum[0] / 4 * 31 + 127) / 255) + labs(g - (sum[1] / 4 * 31 + 127) / 255)
           + labs(bl - (sum[2] / 4 * 31 + 127) / 255);
    }
  const double mean = (double)err / (32 * 32 * 3);
  printf("  gradient: mean error %.2f (5-bit units)\n", mean);
  CHECK(mean < 1.5);
}

static void test_odd_inputs(void) {
  static uint8_t px[96 * 96 * 4];
  dsd_badge_t b;
  synth(px, 96, 96, 3, 5);                          /* a 96x96 game icon */
  CHECK(dsdbp_convert_pixels(px, 96, 96, 3, &b) == 0);
  synth(px, 17, 23, 3, 6);                          /* not a multiple of 32 */
  CHECK(dsdbp_convert_pixels(px, 17, 23, 3, &b) == 0);
  px[0] = 200; px[1] = 100; px[2] = 50;             /* 1x1: one colour everywhere */
  CHECK(dsdbp_convert_pixels(px, 1, 1, 3, &b) == 0);
  int r, g, bl;
  rgb_of(b.palette[px_of(&b, 31, 31)], &r, &g, &bl);
  CHECK(r == (200 * 31 + 127) / 255 && g == (100 * 31 + 127) / 255 && bl == (50 * 31 + 127) / 255);

  /* RGBA: transparent corners come out black */
  synth(px, 64, 64, 4, 1);
  size_t len;
  uint8_t* png = make_png(px, 64, 64, 4, &len);
  CHECK(dsdbp_convert_png(png, len, &b) == 0);
  free(png);
  rgb_of(b.palette[px_of(&b, 0, 0)], &r, &g, &bl);
  CHECK(r == 0 && g == 0 && bl == 0);

  /* Not a PNG, a cut-off PNG: refused */
  CHECK(dsdbp_convert_png("not a png", 9, &b) == -1);
  synth(px, 64, 64, 3, 2);
  png = make_png(px, 64, 64, 3, &len);
  CHECK(dsdbp_convert_png(png, len / 2, &b) == -1);
  free(png);
}

/* ---- the pack */

static void test_pack(void) {
  char path[512], tmp[520];
  snprintf(path, sizeof(path), "%s/test.pak", out_dir);
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  remove(path);

  static dsdbp_entry_t e[4];
  static uint8_t px[64 * 64 * 3];
  memset(e, 0, sizeof(e));
  const uint32_t ids[4] = { 300, 100, 200, 100 };   /* out of order, one duplicate */
  for (int i = 0; i < 4; i++) {
    e[i].id = ids[i];
    snprintf(e[i].name, sizeof(e[i].name), "%05u", 1000 + i);
    synth(px, 64, 64, 3, i + 1);
    dsdbp_convert_pixels(px, 64, 64, 3, &e[i].badge);
  }
  CHECK(dsdbp_write(path, 1234, e, 4) == 0);
  struct stat st;
  CHECK(stat(tmp, &st) != 0);                       /* no .tmp left behind */
  CHECK(stat(path, &st) == 0 && st.st_size == 512 + 3 * DSD_BADGE_SLOT);

  dsdbp_entry_t* r = NULL;
  CHECK(dsdbp_read(path, 1234, &r) == 3);
  if (r) {
    CHECK(r[0].id == 100 && r[1].id == 200 && r[2].id == 300);
    CHECK(!strcmp(r[0].name, "01001"));             /* the first id-100 entry won */
    CHECK(!memcmp(&r[0].badge, &e[1].badge, sizeof(dsd_badge_t)));
    CHECK(!memcmp(&r[2].badge, &e[0].badge, sizeof(dsd_badge_t)));
    const uint32_t table[3] = { r[0].id, r[1].id, r[2].id };
    CHECK(dsd_pack_find(table, 3, 200) == 1);
    CHECK(dsd_pack_find(table, 3, 300) == 2);
    CHECK(dsd_pack_find(table, 3, 150) == -1);
    CHECK(dsd_pack_find(table, 0, 100) == -1);

    /* An update: one badge changes, the others stay byte-identical. */
    synth(px, 64, 64, 3, 7);
    dsdbp_convert_pixels(px, 64, 64, 3, &r[1].badge);
    strcpy(r[1].name, "09999");
    CHECK(dsdbp_write(path, 1234, r, 3) == 0);
    dsdbp_entry_t* r2 = NULL;
    CHECK(dsdbp_read(path, 1234, &r2) == 3);
    if (r2) {
      CHECK(!memcmp(&r2[0], &r[0], sizeof(dsdbp_entry_t)));
      CHECK(!strcmp(r2[1].name, "09999") && !memcmp(&r2[1].badge, &r[1].badge, sizeof(dsd_badge_t)));
      CHECK(!memcmp(&r2[2], &r[2], sizeof(dsdbp_entry_t)));
      free(r2);
    }
    free(r);
  }

  /* The module's view: the slot for id 200 sits at slots_offset + 1 * 544. */
  size_t len;
  uint8_t* raw = read_file(path, &len);
  if (raw) {
    const dsd_pack_header_t* h = (const dsd_pack_header_t*)raw;
    CHECK(sizeof(dsd_pack_header_t) == 32);
    CHECK(h->magic == DSD_PACK_MAGIC && h->count == 3 && (h->slots_offset & 511) == 0);
    const uint32_t* table = (const uint32_t*)(raw + h->ids_offset);
    const int slot = dsd_pack_find(table, h->count, 200);
    CHECK(slot == 1);
    const dsd_badge_t* b = (const dsd_badge_t*)(raw + h->slots_offset + (size_t)slot * DSD_BADGE_SLOT);
    CHECK(sizeof(dsd_badge_t) == DSD_BADGE_SLOT);
    dsdbp_entry_t* again = NULL;
    if (dsdbp_read(path, 1234, &again) == 3) {
      CHECK(!memcmp(b, &again[1].badge, sizeof(*b)));
      free(again);
    }

    /* Damaged or foreign packs are refused (counted as no pack). */
    dsdbp_entry_t* x = NULL;
    CHECK(dsdbp_read(path, 999, &x) == -1);         /* another game's */
    FILE* f = fopen(path, "wb");
    fwrite(raw, 1, len - 100, f);                   /* cut short */
    fclose(f);
    CHECK(dsdbp_read(path, 1234, &x) == -1);
    ((uint32_t*)(raw + h->ids_offset))[1] = 50;     /* ids out of order */
    f = fopen(path, "wb");
    fwrite(raw, 1, len, f);
    fclose(f);
    CHECK(dsdbp_read(path, 1234, &x) == -1);
    raw[0] = 'X';                                   /* bad magic */
    f = fopen(path, "wb");
    fwrite(raw, 1, len, f);
    fclose(f);
    CHECK(dsdbp_read(path, 1234, &x) == -1);
    free(raw);
  }
  CHECK(dsdbp_read("no/such/file.pak", 1234, &r) == -1);

  /* An empty pack is valid. */
  CHECK(dsdbp_write(path, 1234, e, 0) == 0);
  CHECK(dsdbp_read(path, 1234, &r) == 0);
  free(r);
}

/* ---- badges in notices */

static dsd_badge_t test_badge;

static const char* text(void* ctx, const dsd_item_t* it, int which) {
  (void)ctx;
  if (it->kind == DSD_ITEM_PROGRESS) return which == DSD_TEXT_TITLE ? "Collector" : "12/30";
  return which == DSD_TEXT_TITLE ? "Rising Star" : "Clear World 1 without taking damage";
}

static const struct dsd_badge_t* badge(void* ctx, const dsd_item_t* it) {
  (void)ctx;
  return it->id == 101 ? &test_badge : 0;
}

static void render_side(const char* name, uint16_t kind, uint32_t id, int32_t value, uint16_t* px, int side) {
  static dscm_notices_t n;
  dsd_t d;
  const dsd_config_t cfg = { .font = &dsd_font_latin, .text = text, .badge = badge, .side = (uint8_t)side, .hardcore = 1 };
  dsd_init(&d, &cfg);
  memset(&n, 0, sizeof(n));
  n.magic = DSCM_NOTICES_MAGIC;
  n.head = 1;
  n.ring[1].seq = 1;
  n.ring[1].kind = kind;
  n.ring[1].id = id;
  n.ring[1].value = value;
  dsd_ingest(&d, &n);
  const dsd_caps_t caps = { 32, 0 };
  dsd_tick(&d, 1, &caps);
  dsd_surface_t s = { px, 256, 32, 256 };
  int calls = 0;
  while (!dsd_render(&d, &s, 5)) calls++;           /* in bands: the badge spans several */
  CHECK(calls == 6);
  golden(name, px, 256, 32);
}

static void render(const char* name, uint16_t kind, uint32_t id, int32_t value, uint16_t* px) {
  render_side(name, kind, id, value, px, DSD_SIDE_LEFT);
}

static void test_notices_with_badges(void) {
  static uint8_t px[64 * 64 * 3];
  synth(px, 64, 64, 3, 3);
  dsdbp_convert_pixels(px, 64, 64, 3, &test_badge);

  static uint16_t a[256 * 32], b[256 * 32], c[256 * 32];
  render("badge_unlock", DSCM_NOTICE_UNLOCK, 101, 10, a);
  render("badge_progress_locked", DSCM_NOTICE_PROGRESS, 101, 0, b);
  render("badge_missing", DSCM_NOTICE_UNLOCK, 555, 10, c);   /* not loaded: the icon */
  render_side("badge_unlock_right", DSCM_NOTICE_UNLOCK, 101, 10, a, DSD_SIDE_RIGHT);

  /* The locked look is grey. */
  uint16_t grey[16];
  dsd_badge_locked_palette(test_badge.palette, grey);
  for (int i = 0; i < 16; i++) CHECK((grey[i] & 31) == ((grey[i] >> 5) & 31) && (grey[i] & 31) == ((grey[i] >> 10) & 31));

  /* A contact sheet at 4x for review. */
  static uint16_t sheet[128 * 4 * 128];
  dsd_surface_t s = { sheet, 128 * 4, 128, 128 * 4 };
  for (int i = 0; i < 128 * 4 * 128; i++) sheet[i] = 0x2108;
  static uint16_t tile[32 * 32];
  dsd_surface_t t = { tile, 32, 32, 32 };
  for (int k = 0; k < 4; k++) {
    dsd_badge_t bb;
    synth(px, 64, 64, 3, k + 1);
    dsdbp_convert_pixels(px, 64, 64, 3, &bb);
    dsd_badge_blit(&bb, k == 3, &t, 0, 0);
    for (int y = 0; y < 128; y++)
      for (int x = 0; x < 128; x++) sheet[y * s.stride + k * 128 + x] = tile[(y / 4) * 32 + x / 4];
  }
  golden("badges_synthetic", sheet, 128 * 4, 128);
}

/* ---- the thin bar's 16x16 badges */

static dsd_badge16_t test_badge16;

static const struct dsd_badge16_t* badge16(void* ctx, const dsd_item_t* it) {
  (void)ctx;
  return it->id == 101 ? &test_badge16 : 0;
}

static void render16(const char* name, uint16_t kind, uint32_t id, int32_t value, uint16_t* px) {
  static dscm_notices_t n;
  dsd_t d;
  const dsd_config_t cfg = { .font = &dsd_font_latin, .text = text, .badge16 = badge16, .hardcore = 1 };
  dsd_init(&d, &cfg);
  memset(&n, 0, sizeof(n));
  n.magic = DSCM_NOTICES_MAGIC;
  n.head = 1;
  n.ring[1].seq = 1;
  n.ring[1].kind = kind;
  n.ring[1].id = id;
  n.ring[1].value = value;
  dsd_ingest(&d, &n);
  const dsd_caps_t caps = { 16, 0 };
  dsd_tick(&d, 1, &caps);
  dsd_surface_t s = { px, 256, 16, 256 };
  int calls = 0;
  while (!dsd_render(&d, &s, 5)) calls++;           /* in bands: the badge spans several */
  CHECK(calls == 3);
  golden(name, px, 256, 16);
}

static void test_badges16(void) {
  /* exact colours survive 64 -> 16 when they come in 4x4 blocks */
  static uint8_t px[64 * 64 * 3];
  static const uint8_t cols[4][3] = { {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255} };
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++)
      memcpy(px + (y * 64 + x) * 3, cols[((x / 4) + (y / 4) * 3) % 4], 3);
  size_t len;
  uint8_t* png = make_png(px, 64, 64, 3, &len);
  dsd_badge16_t b;
  CHECK(dsdbp_convert_png16(png, len, &b) == 0);
  free(png);
  int wrong = 0;
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++) {
      int r, g, bl;
      const uint8_t two = b.pixels[(y * 16 + x) >> 1];
      rgb_of(b.palette[x & 1 ? two >> 4 : two & 15], &r, &g, &bl);
      const uint8_t* c = cols[(x + y * 3) % 4];
      if (r != c[0] * 31 / 255 || g != c[1] * 31 / 255 || bl != c[2] * 31 / 255) wrong++;
    }
  CHECK(wrong == 0);

  /* in the thin bar, in the icon's place; unlocked and locked; no badge: the icon */
  synth(px, 64, 64, 3, 3);
  dsdbp_convert_pixels16(px, 64, 64, 3, &test_badge16);
  static uint16_t a[256 * 16], c[256 * 16], m[256 * 16];
  render16("badge16_unlock", DSCM_NOTICE_UNLOCK, 101, 10, a);
  render16("badge16_progress_locked", DSCM_NOTICE_PROGRESS, 101, 0, c);
  render16("badge16_missing", DSCM_NOTICE_UNLOCK, 555, 10, m);
}

/* ---- optional: real badges from a local folder (never committed) */

static void samples(const char* dir) {
  DIR* d = opendir(dir);
  if (!d) { printf("  no samples dir %s\n", dir); return; }
  enum { SW = 8 * 72, SH = 10 * 72 };
  static uint16_t sheet[SW * SH];
  const int cols = 8, cell = 72;
  for (size_t i = 0; i < sizeof(sheet) / 2; i++) sheet[i] = 0x2108;
  int n = 0;
  struct dirent* de;
  char names[64][256];
  while ((de = readdir(d)) && n < 40) {
    const size_t l = strlen(de->d_name);
    if (l > 4 && !strcmp(de->d_name + l - 4, ".png") && !strstr(de->d_name, "_lock")) snprintf(names[n++], 256, "%s", de->d_name);
  }
  closedir(d);
  for (int i = 0; i < n; i++) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
    size_t len;
    uint8_t* png = read_file(path, &len);
    dsd_badge_t b;
    if (!png || dsdbp_convert_png(png, len, &b) != 0) { free(png); continue; }
    free(png);
    static uint16_t tile[32 * 32];
    dsd_surface_t t = { tile, 32, 32, 32 };
    for (int locked = 0; locked < 2; locked++) {
      dsd_badge_blit(&b, locked, &t, 0, 0);
      const int ox = (i % cols) * cell, oy = (i / cols) * 2 * cell + locked * cell;
      for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
          const int sy = oy + y, sx = ox + x;
          if (sy < SH && sx < SW) sheet[sy * SW + sx] = tile[(y / 2) * 32 + x / 2];
        }
    }
  }
  char out[512];
  snprintf(out, sizeof(out), "%s/samples.png", out_dir);
  png_write_bgr555(out, sheet, SW, SH, SW);
  printf("  %d sample badges -> %s (unlocked above locked, 2x)\n", n, out);
}

int main(int argc, char** argv) {
  out_dir = argc > 1 ? argv[1] : "out";
  mkdir(out_dir, 0755);
  const char* u = getenv("UPDATE_GOLDEN");
  update_golden = u && *u && strcmp(u, "0") != 0;

  test_exact_colours();
  test_gradient_error();
  test_odd_inputs();
  test_pack();
  test_notices_with_badges();
  test_badges16();
  const char* dir = getenv("DSD_BADGE_SAMPLES");
  if (dir && *dir) samples(dir);

  printf("badges: %d checks, %d failed%s\n", checks, failures, update_golden ? " (goldens updated)" : "");
  return failures != 0;
}
