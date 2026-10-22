/* Badge packs, launcher side. See dsd_badgepack.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd_badgepack.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../third_party/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define NMAX DSD_BADGE_SIZE
#define PIXELS_MAX (NMAX * NMAX)

/* ---- downscale: each target pixel averages the source area it covers */

static void downscale(const uint8_t* px, int w, int h, int comp, int N, uint8_t rgb[][3]) {
  for (int ty = 0; ty < N; ty++) {
    const double y0 = (double)ty * h / N, y1 = (double)(ty + 1) * h / N;
    for (int tx = 0; tx < N; tx++) {
      const double x0 = (double)tx * w / N, x1 = (double)(tx + 1) * w / N;
      double sum[3] = { 0, 0, 0 }, area = 0;
      for (int sy = (int)y0; sy < h && sy < y1; sy++) {
        const double wy = (sy + 1 < y1 ? sy + 1 : y1) - (sy > y0 ? sy : y0);
        for (int sx = (int)x0; sx < w && sx < x1; sx++) {
          const double wx = (sx + 1 < x1 ? sx + 1 : x1) - (sx > x0 ? sx : x0);
          const uint8_t* p = px + ((size_t)sy * w + sx) * comp;
          const double a = comp == 4 ? p[3] / 255.0 : 1.0;   /* over black */
          for (int c = 0; c < 3; c++) sum[c] += p[c] * a * wx * wy;
          area += wx * wy;
        }
      }
      for (int c = 0; c < 3; c++) {
        const int v = area > 0 ? (int)(sum[c] / area + 0.5) : 0;
        rgb[ty * N + tx][c] = (uint8_t)(v > 255 ? 255 : v);
      }
    }
  }
}

/* ---- median cut over 15-bit colours */

typedef struct { uint8_t c[3]; uint16_t i; } cpix_t;
static int sort_axis;

static int cmp_axis(const void* a, const void* b) {
  const cpix_t* p = a;
  const cpix_t* q = b;
  for (int k = 0; k < 3; k++) {
    const int ax = (sort_axis + k) % 3;
    if (p->c[ax] != q->c[ax]) return p->c[ax] - q->c[ax];
  }
  return p->i - q->i;                 /* stable: deterministic output */
}

typedef struct { int start, len; } box_t;

static int box_range(const cpix_t* p, const box_t* b, int* axis) {
  int best = -1;
  for (int ax = 0; ax < 3; ax++) {
    int lo = 31, hi = 0;
    for (int i = b->start; i < b->start + b->len; i++) {
      if (p[i].c[ax] < lo) lo = p[i].c[ax];
      if (p[i].c[ax] > hi) hi = p[i].c[ax];
    }
    if (hi - lo > best) { best = hi - lo; *axis = ax; }
  }
  return best;
}

static uint16_t bgr555(int r, int g, int b) { return (uint16_t)(r | g << 5 | b << 10); }

/* Unsharp mask on the downscaled image: v + (v - 3x3 mean) / 2. */
static int g_sharpen;
static void sharpen(uint8_t rgb[][3], int n) {
  static uint8_t src[PIXELS_MAX][3];
  memcpy(src, rgb, (size_t)n * n * 3);
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++)
      for (int c = 0; c < 3; c++) {
        int sum = 0, cnt = 0;
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++) {
            const int yy = y + dy, xx = x + dx;
            if (yy < 0 || yy >= n || xx < 0 || xx >= n) continue;
            sum += src[yy * n + xx][c];
            cnt++;
          }
        int v = src[y * n + x][c];
        v += (v - sum / cnt) / 2;
        rgb[y * n + x][c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
      }
}

/* Any size up to 32x32: area-average down to n x n, then the median cut;
 * pixels (n*n/2 bytes) and palette (16) are filled. */
static int convert_n(const uint8_t* px, int w, int h, int comp, int n, uint8_t* pixels, uint16_t* palette) {
  static uint8_t rgb[PIXELS_MAX][3];
  static cpix_t p[PIXELS_MAX];
  const int PIXELS = n * n;
  memset(pixels, 0, (size_t)PIXELS / 2);
  memset(palette, 0, 16 * sizeof(uint16_t));
  downscale(px, w, h, comp, n, rgb);
  if (g_sharpen)
    sharpen(rgb, n);
  for (int i = 0; i < PIXELS; i++) {
    for (int c = 0; c < 3; c++) p[i].c[c] = (uint8_t)((rgb[i][c] * 31 + 127) / 255);
    p[i].i = (uint16_t)i;
  }

  box_t boxes[16] = { { 0, PIXELS } };
  int nboxes = 1;
  while (nboxes < 16) {
    int pick = -1, pick_axis = 0, pick_range = 0;
    for (int b = 0; b < nboxes; b++) {
      int axis;
      const int r = box_range(p, &boxes[b], &axis);
      if (boxes[b].len >= 2 && r > pick_range) { pick = b; pick_axis = axis; pick_range = r; }
    }
    if (pick < 0) break;              /* every box is a single colour */
    box_t* b = &boxes[pick];
    sort_axis = pick_axis;
    qsort(p + b->start, (size_t)b->len, sizeof(cpix_t), cmp_axis);
    /* split at the median, moved off a run of equal values where possible */
    int cut = b->len / 2;
    const int v = p[b->start + cut].c[pick_axis];
    int lo = cut, hi = cut;
    while (lo > 0 && p[b->start + lo - 1].c[pick_axis] == v) lo--;
    while (hi < b->len && p[b->start + hi].c[pick_axis] == v) hi++;
    cut = (cut - lo <= hi - cut && lo > 0) ? lo : (hi < b->len ? hi : lo);
    if (cut <= 0 || cut >= b->len) break;
    boxes[nboxes].start = b->start + cut;
    boxes[nboxes].len = b->len - cut;
    b->len = cut;
    nboxes++;
  }

  /* Palette: each box's mean. Pixels: their box's entry. */
  for (int b = 0; b < nboxes; b++) {
    int sum[3] = { 0, 0, 0 };
    for (int i = boxes[b].start; i < boxes[b].start + boxes[b].len; i++)
      for (int c = 0; c < 3; c++) sum[c] += p[i].c[c];
    const int cnt = boxes[b].len;
    palette[b] = bgr555((sum[0] + cnt / 2) / cnt, (sum[1] + cnt / 2) / cnt, (sum[2] + cnt / 2) / cnt);
    for (int i = boxes[b].start; i < boxes[b].start + boxes[b].len; i++) {
      const int at = p[i].i;
      pixels[at >> 1] |= (uint8_t)(at & 1 ? b << 4 : b);
    }
  }
  return 0;
}

int dsdbp_convert_pixels(const uint8_t* px, int w, int h, int comp, dsd_badge_t* out) {
  return convert_n(px, w, h, comp, DSD_BADGE_SIZE, out->pixels, out->palette);
}

/* 16x16: RetroAchievements badges have a frame a few pixels wide that would
 * take a quarter of so small an image: crop 1/16 off each side first (4 px of
 * 64), then sharpen a little after the area average, so edges stay crisp. */
int dsdbp_convert_pixels16(const uint8_t* px, int w, int h, int comp, dsd_badge16_t* out) {
  const int cx = w >= 32 ? w / 16 : 0, cy = h >= 32 ? h / 16 : 0;
  const int cw = w - 2 * cx, ch = h - 2 * cy;
  uint8_t* crop = malloc((size_t)cw * ch * comp);
  int rc;
  if (!crop)
    return convert_n(px, w, h, comp, DSD_BADGE16_SIZE, out->pixels, out->palette);
  for (int y = 0; y < ch; y++)
    memcpy(crop + (size_t)y * cw * comp, px + ((size_t)(y + cy) * w + cx) * comp, (size_t)cw * comp);
  g_sharpen = 1;
  rc = convert_n(crop, cw, ch, comp, DSD_BADGE16_SIZE, out->pixels, out->palette);
  g_sharpen = 0;
  free(crop);
  return rc;
}

/* Decodes a PNG to RGB or RGBA (grey widened) and calls fn on it. */
static int decode_png(const void* data, size_t len,
                      int (*fn)(const uint8_t*, int, int, int, void*), void* out) {
  int w, h, comp;
  if (len > 0x7FFFFFFF) return -1;
  uint8_t* px = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 0);
  if (!px) return -1;
  int rc = -1;
  if (w > 0 && h > 0 && w <= 1024 && h <= 1024) {
    if (comp == 3 || comp == 4) {
      rc = fn(px, w, h, comp, out);
    } else {
      /* grey (+alpha): widen to RGB(A) */
      const int oc = comp == 2 ? 4 : 3;
      uint8_t* wide = malloc((size_t)w * h * oc);
      if (wide) {
        for (int i = 0; i < w * h; i++) {
          wide[i * oc] = wide[i * oc + 1] = wide[i * oc + 2] = px[i * comp];
          if (oc == 4) wide[i * 4 + 3] = px[i * comp + 1];
        }
        rc = fn(wide, w, h, oc, out);
        free(wide);
      }
    }
  }
  stbi_image_free(px);
  return rc;
}

static int to32(const uint8_t* px, int w, int h, int comp, void* out) { return dsdbp_convert_pixels(px, w, h, comp, out); }
static int to16(const uint8_t* px, int w, int h, int comp, void* out) { return dsdbp_convert_pixels16(px, w, h, comp, out); }

int dsdbp_convert_png(const void* data, size_t len, dsd_badge_t* out) { return decode_png(data, len, to32, out); }
int dsdbp_convert_png16(const void* data, size_t len, dsd_badge16_t* out) { return decode_png(data, len, to16, out); }

/* ---- pack file */

static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t get32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

static int cmp_entry(const void* a, const void* b) {
  const dsdbp_entry_t* const* x = a;
  const dsdbp_entry_t* const* y = b;
  if ((*x)->id != (*y)->id) return (*x)->id < (*y)->id ? -1 : 1;
  return *x < *y ? -1 : 1;            /* first of duplicates first */
}

int dsdbp_write(const char* path, uint32_t game_id, const dsdbp_entry_t* e, uint32_t count) {
  const dsdbp_entry_t** order = malloc((count ? count : 1) * sizeof(*order));
  if (!order) return -1;
  for (uint32_t i = 0; i < count; i++) order[i] = &e[i];
  qsort(order, count, sizeof(*order), cmp_entry);
  uint32_t n = 0;                     /* drop duplicate ids */
  for (uint32_t i = 0; i < count; i++)
    if (!n || order[n - 1]->id != order[i]->id) order[n++] = order[i];

  const uint32_t ids_off = 32, names_off = ids_off + 4 * n;
  const uint32_t slots_off = (names_off + DSD_PACK_NAME * n + 511) & ~511u;
  const size_t size = slots_off + (size_t)DSD_BADGE_SLOT * n;
  uint8_t* buf = calloc(1, size);
  if (!buf) { free(order); return -1; }
  put32(buf + 0, DSD_PACK_MAGIC);
  put16(buf + 4, DSD_PACK_VERSION);
  put16(buf + 6, DSD_BADGE_SLOT);
  put32(buf + 8, game_id);
  put32(buf + 12, n);
  put32(buf + 16, ids_off);
  put32(buf + 20, names_off);
  put32(buf + 24, slots_off);
  put32(buf + 28, 0);
  for (uint32_t i = 0; i < n; i++) {
    put32(buf + ids_off + 4 * i, order[i]->id);
    memcpy(buf + names_off + DSD_PACK_NAME * i, order[i]->name, strnlen(order[i]->name, DSD_PACK_NAME));
    uint8_t* slot = buf + slots_off + (size_t)DSD_BADGE_SLOT * i;
    memcpy(slot, order[i]->badge.pixels, sizeof(order[i]->badge.pixels));
    for (int c = 0; c < 16; c++) put16(slot + 512 + 2 * c, order[i]->badge.palette[c]);
  }
  free(order);

  char tmp[512];
  if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) { free(buf); return -1; }
  FILE* f = fopen(tmp, "wb");
  int ok = f && fwrite(buf, 1, size, f) == size;
  if (f && fclose(f) != 0) ok = 0;
  free(buf);
  if (!ok) { remove(tmp); return -1; }
  remove(path);                       /* rename over an existing file isn't portable to every libc */
  if (rename(tmp, path) != 0) { remove(tmp); return -1; }
  return 0;
}

int dsdbp_read(const char* path, uint32_t game_id, dsdbp_entry_t** out) {
  *out = NULL;
  FILE* f = fopen(path, "rb");
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
  const long size = ftell(f);
  rewind(f);
  if (size < 32 || size > 16 * 1024 * 1024) { fclose(f); return -1; }
  uint8_t* buf = malloc((size_t)size);
  if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(buf); return -1; }
  fclose(f);

  const uint32_t n = get32(buf + 12), ids = get32(buf + 16), names = get32(buf + 20), slots = get32(buf + 24);
  const uint64_t sz = (uint64_t)size;
  int bad = get32(buf) != DSD_PACK_MAGIC || get16(buf + 4) != DSD_PACK_VERSION
         || get16(buf + 6) != DSD_BADGE_SLOT || get32(buf + 8) != game_id || n > 65536
         || ids + 4ull * n > sz || names + (uint64_t)DSD_PACK_NAME * n > sz
         || slots + (uint64_t)DSD_BADGE_SLOT * n > sz;
  for (uint32_t i = 1; !bad && i < n; i++)
    if (get32(buf + ids + 4 * (i - 1)) >= get32(buf + ids + 4 * i)) bad = 1;
  dsdbp_entry_t* e = bad ? NULL : calloc(n ? n : 1, sizeof(*e));
  if (!e) { free(buf); return -1; }
  for (uint32_t i = 0; i < n; i++) {
    e[i].id = get32(buf + ids + 4 * i);
    memcpy(e[i].name, buf + names + DSD_PACK_NAME * i, DSD_PACK_NAME);
    e[i].name[DSD_PACK_NAME] = 0;
    const uint8_t* slot = buf + slots + (size_t)DSD_BADGE_SLOT * i;
    memcpy(e[i].badge.pixels, slot, sizeof(e[i].badge.pixels));
    for (int c = 0; c < 16; c++) e[i].badge.palette[c] = get16(slot + 512 + 2 * c);
  }
  free(buf);
  *out = e;
  return (int)n;
}
