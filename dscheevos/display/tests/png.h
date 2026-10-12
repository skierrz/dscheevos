/* Minimal PNG writer for the host tests: RGB8, no compression (stored
 * deflate blocks), so the output is byte-for-byte deterministic. */
#ifndef DSD_TEST_PNG_H
#define DSD_TEST_PNG_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t png_crc_table[256];

static uint32_t png_crc(uint32_t crc, const uint8_t* p, size_t n) {
  if (!png_crc_table[1]) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      png_crc_table[i] = c;
    }
  }
  crc = ~crc;
  while (n--) crc = png_crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

static void png_be32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void png_chunk(FILE* f, const char* type, const uint8_t* data, uint32_t n) {
  uint8_t hdr[8];
  png_be32(hdr, n);
  for (int i = 0; i < 4; i++) hdr[4 + i] = (uint8_t)type[i];
  fwrite(hdr, 1, 8, f);
  if (n) fwrite(data, 1, n, f);
  uint32_t crc = png_crc(0, hdr + 4, 4);
  crc = png_crc(crc, data, n);
  uint8_t c[4];
  png_be32(c, crc);
  fwrite(c, 1, 4, f);
}

/* Writes BGR555 pixels (the DS format) as an RGB8 PNG. Returns 0 on success. */
static int png_write_bgr555(const char* path, const uint16_t* px, int w, int h, int stride) {
  const size_t raw_n = (size_t)h * (1 + 3 * (size_t)w);
  uint8_t* raw = malloc(raw_n);
  size_t r = 0;
  for (int y = 0; y < h; y++) {
    raw[r++] = 0;                                   /* filter: none */
    for (int x = 0; x < w; x++) {
      const uint16_t c = px[y * stride + x];
      const int cr = c & 31, cg = (c >> 5) & 31, cb = (c >> 10) & 31;
      raw[r++] = (uint8_t)(cr << 3 | cr >> 2);
      raw[r++] = (uint8_t)(cg << 3 | cg >> 2);
      raw[r++] = (uint8_t)(cb << 3 | cb >> 2);
    }
  }
  const size_t blocks = (raw_n + 65534) / 65535;
  const size_t z_n = 2 + raw_n + blocks * 5 + 4;
  uint8_t* z = malloc(z_n);
  size_t o = 0;
  z[o++] = 0x78; z[o++] = 0x01;
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
  for (size_t at = 0; at < raw_n; at += 65535) {
    const size_t len = raw_n - at < 65535 ? raw_n - at : 65535;
    z[o++] = at + len == raw_n;
    z[o++] = len & 0xFF; z[o++] = len >> 8;
    z[o++] = ~len & 0xFF; z[o++] = (~len >> 8) & 0xFF;
    for (size_t i = 0; i < len; i++) z[o++] = raw[at + i];
  }
  png_be32(z + o, b << 16 | a);
  o += 4;

  FILE* f = fopen(path, "wb");
  if (!f) { free(raw); free(z); return -1; }
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  fwrite(sig, 1, 8, f);
  uint8_t ihdr[13];
  png_be32(ihdr, (uint32_t)w);
  png_be32(ihdr + 4, (uint32_t)h);
  ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  png_chunk(f, "IHDR", ihdr, 13);
  png_chunk(f, "IDAT", z, (uint32_t)o);
  png_chunk(f, "IEND", 0, 0);
  fclose(f);
  free(raw);
  free(z);
  return 0;
}

#endif
