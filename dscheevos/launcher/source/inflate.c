/*
 * gzip decoding for HTTP replies (Content-Encoding: gzip): RFC 1952 around
 * RFC 1951 deflate, decoded into a buffer of the size the gzip trailer
 * gives. Small and slow-but-adequate: canonical Huffman decoding one bit at
 * a time, as in zlib's puff.c. The server compresses an achievement set
 * about tenfold, so the launcher downloads far less.
 */
#include "inflate.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const uint8_t* in;
  size_t in_len, in_pos;
  uint32_t bits, nbits;
  uint8_t* out;
  size_t out_len, out_pos;
} state_t;

typedef struct {
  short count[16];       /* codes of each length */
  short symbol[320];     /* symbols in canonical order */
} huffman_t;

/* -1: input ran out */
static int bits(state_t* s, int need, uint32_t* v)
{
  while (s->nbits < (uint32_t)need) {
    if (s->in_pos >= s->in_len)
      return -1;
    s->bits |= (uint32_t)s->in[s->in_pos++] << s->nbits;
    s->nbits += 8;
  }
  *v = s->bits & ((1u << need) - 1);
  s->bits >>= need;
  s->nbits -= (uint32_t)need;
  return 0;
}

static int decode(state_t* s, const huffman_t* h)
{
  int code = 0, first = 0, index = 0, len;
  for (len = 1; len < 16; len++) {
    uint32_t b;
    int count;
    if (bits(s, 1, &b))
      return -1;
    code |= (int)b;
    count = h->count[len];
    if (code - count < first)
      return h->symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return -1;
}

/* 0, or -1 for an over-subscribed or empty code */
static int build(huffman_t* h, const uint8_t* length, int n)
{
  short offs[16];
  int i, left = 1;
  memset(h->count, 0, sizeof(h->count));
  for (i = 0; i < n; i++)
    h->count[length[i]]++;
  if (h->count[0] == n)
    return 0;                          /* no codes: an error only if one is read */
  for (i = 1; i < 16; i++) {
    left <<= 1;
    left -= h->count[i];
    if (left < 0)
      return -1;
  }
  offs[1] = 0;
  for (i = 1; i < 15; i++)
    offs[i + 1] = (short)(offs[i] + h->count[i]);
  for (i = 0; i < n; i++)
    if (length[i])
      h->symbol[offs[length[i]]++] = (short)i;
  return 0;
}

static const short len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const short dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(state_t* s, const huffman_t* lit, const huffman_t* dist)
{
  for (;;) {
    int sym = decode(s, lit);
    if (sym < 0)
      return -1;
    if (sym < 256) {
      if (s->out_pos >= s->out_len)
        return -1;
      s->out[s->out_pos++] = (uint8_t)sym;
    }
    else if (sym == 256) {
      return 0;
    }
    else {
      uint32_t e, len, d;
      sym -= 257;
      if (sym >= 29 || bits(s, len_extra[sym], &e))
        return -1;
      len = (uint32_t)len_base[sym] + e;
      sym = decode(s, dist);
      if (sym < 0 || sym >= 30 || bits(s, dist_extra[sym], &e))
        return -1;
      d = (uint32_t)dist_base[sym] + e;
      if (d > s->out_pos || len > s->out_len - s->out_pos)
        return -1;
      while (len--) {
        s->out[s->out_pos] = s->out[s->out_pos - d];
        s->out_pos++;
      }
    }
  }
}

static int stored(state_t* s)
{
  size_t len;
  s->bits = s->nbits = 0;              /* to a byte boundary */
  if (s->in_len - s->in_pos < 4)
    return -1;
  len = s->in[s->in_pos] | (size_t)s->in[s->in_pos + 1] << 8;
  if ((len ^ 0xFFFF) != (s->in[s->in_pos + 2] | (size_t)s->in[s->in_pos + 3] << 8))
    return -1;
  s->in_pos += 4;
  if (len > s->in_len - s->in_pos || len > s->out_len - s->out_pos)
    return -1;
  memcpy(s->out + s->out_pos, s->in + s->in_pos, len);
  s->in_pos += len;
  s->out_pos += len;
  return 0;
}

static int fixed(state_t* s, huffman_t* lit, huffman_t* dist)
{
  uint8_t l[288];
  int i;
  for (i = 0; i < 144; i++) l[i] = 8;
  for (; i < 256; i++) l[i] = 9;
  for (; i < 280; i++) l[i] = 7;
  for (; i < 288; i++) l[i] = 8;
  build(lit, l, 288);
  for (i = 0; i < 30; i++) l[i] = 5;
  build(dist, l, 30);
  return codes(s, lit, dist);
}

static int dynamic(state_t* s, huffman_t* lit, huffman_t* dist)
{
  static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
  uint8_t l[320];
  uint32_t nlen, ndist, ncode, v;
  int i;
  if (bits(s, 5, &nlen) || bits(s, 5, &ndist) || bits(s, 4, &ncode))
    return -1;
  nlen += 257; ndist += 1; ncode += 4;
  if (nlen > 286 || ndist > 30)
    return -1;
  memset(l, 0, sizeof(l));
  for (i = 0; i < (int)ncode; i++) {
    if (bits(s, 3, &v))
      return -1;
    l[order[i]] = (uint8_t)v;
  }
  if (build(lit, l, 19))
    return -1;
  for (i = 0; i < (int)(nlen + ndist);) {
    int sym = decode(s, lit);
    uint32_t rep;
    uint8_t value = 0;
    if (sym < 0)
      return -1;
    if (sym < 16) {
      l[i++] = (uint8_t)sym;
      continue;
    }
    if (sym == 16) {
      if (i == 0 || bits(s, 2, &rep)) return -1;
      value = l[i - 1];
      rep += 3;
    }
    else if (sym == 17) {
      if (bits(s, 3, &rep)) return -1;
      rep += 3;
    }
    else {
      if (bits(s, 7, &rep)) return -1;
      rep += 11;
    }
    if (i + rep > nlen + ndist)
      return -1;
    while (rep--)
      l[i++] = value;
  }
  if (l[256] == 0 || build(lit, l, (int)nlen) || build(dist, l + nlen, (int)ndist))
    return -1;
  return codes(s, lit, dist);
}

static uint32_t crc32(const uint8_t* p, size_t n)
{
  uint32_t c = 0xFFFFFFFFu;
  while (n--) {
    int k;
    c ^= *p++;
    for (k = 0; k < 8; k++)
      c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}

char* gunzip(const uint8_t* in, size_t in_len, size_t* out_len)
{
  state_t s;
  huffman_t* h;
  size_t pos = 10;
  uint32_t size, crc, last = 0, type;
  uint8_t flags;
  int err = 0;

  if (in_len < 18 || in[0] != 0x1F || in[1] != 0x8B || in[2] != 8)
    return NULL;
  flags = in[3];
  if (flags & 4) {                      /* FEXTRA */
    if (pos + 2 > in_len) return NULL;
    pos += 2 + (in[pos] | (size_t)in[pos + 1] << 8);
  }
  if (flags & 8) { while (pos < in_len && in[pos]) pos++; pos++; }    /* FNAME */
  if (flags & 16) { while (pos < in_len && in[pos]) pos++; pos++; }   /* FCOMMENT */
  if (flags & 2) pos += 2;              /* FHCRC */
  if (pos + 8 > in_len)
    return NULL;
  crc = in[in_len - 8] | (uint32_t)in[in_len - 7] << 8 | (uint32_t)in[in_len - 6] << 16 | (uint32_t)in[in_len - 5] << 24;
  size = in[in_len - 4] | (uint32_t)in[in_len - 3] << 8 | (uint32_t)in[in_len - 2] << 16 | (uint32_t)in[in_len - 1] << 24;
  if (size > GUNZIP_MAX)
    return NULL;

  memset(&s, 0, sizeof(s));
  s.in = in + pos;
  s.in_len = in_len - 8 - pos;
  s.out = (uint8_t*)malloc(size + 1);
  s.out_len = size;
  h = (huffman_t*)malloc(2 * sizeof(huffman_t));
  if (!s.out || !h) {
    free(s.out);
    free(h);
    return NULL;
  }
  while (!err && !last) {
    if (bits(&s, 1, &last) || bits(&s, 2, &type))
      err = 1;
    else if (type == 0)
      err = stored(&s);
    else if (type == 1)
      err = fixed(&s, &h[0], &h[1]);
    else if (type == 2)
      err = dynamic(&s, &h[0], &h[1]);
    else
      err = 1;
  }
  free(h);
  if (err || s.out_pos != size || crc32(s.out, size) != crc) {
    free(s.out);
    return NULL;
  }
  s.out[size] = '\0';
  *out_len = size;
  return (char*)s.out;
}
