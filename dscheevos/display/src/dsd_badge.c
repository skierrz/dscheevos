/* Module side of the badges: find a slot, draw it. No decompressor: slots
 * are stored ready to draw. */
#include "dsd_badge.h"

int dsd_pack_find(const uint32_t* ids, uint32_t count, uint32_t id) {
  uint32_t lo = 0, hi = count;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (ids[mid] < id) lo = mid + 1;
    else hi = mid;
  }
  return lo < count && ids[lo] == id ? (int)lo : -1;
}

void dsd_badge_locked_palette(const uint16_t in[16], uint16_t out[16]) {
  for (int i = 0; i < 16; i++) {
    const uint32_t r = in[i] & 31, g = (in[i] >> 5) & 31, b = (in[i] >> 10) & 31;
    uint32_t y = (r * 77 + g * 150 + b * 29) >> 8;   /* luma, 0-31 */
    y = y * 3 / 4;                                    /* locked badges read darker */
    out[i] = (uint16_t)(y | y << 5 | y << 10);
  }
}

static void blit4(const uint8_t* pixels, const uint16_t* palette, int size, int locked,
                  dsd_surface_t* s, int x, int y) {
  uint16_t grey[16];
  const uint16_t* pal = palette;
  if (locked) {
    dsd_badge_locked_palette(palette, grey);
    pal = grey;
  }
  for (int row = 0; row < size; row++) {
    const int dy = y + row;
    if (dy < 0 || dy >= s->h) continue;
    uint16_t* dst = s->pixels + dy * s->stride;
    const uint8_t* src = pixels + row * (size / 2);
    for (int col = 0; col < size; col++) {
      const int dx = x + col;
      if (dx < 0 || dx >= s->w) continue;
      const uint8_t two = src[col >> 1];
      dst[dx] = pal[col & 1 ? two >> 4 : two & 15];
    }
  }
}

void dsd_badge16_blit(const dsd_badge16_t* b, int locked, dsd_surface_t* s, int x, int y) {
  blit4(b->pixels, b->palette, DSD_BADGE16_SIZE, locked, s, x, y);
}

void dsd_badge_blit(const dsd_badge_t* b, int locked, dsd_surface_t* s, int x, int y) {
  blit4(b->pixels, b->palette, DSD_BADGE_SIZE, locked, s, x, y);
}
