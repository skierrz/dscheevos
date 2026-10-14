/*
 * Measurement only (research/optimize/everyframe.md). Converts the runner's --everyframe log (EVF1,
 * tools/melonDS/tools/dscheevos/headless/everyframe.inc) into copyonce.c's COV1 format, one record per
 * module frame, so copyonce.c, copyonce_stress.py and copyonce_report.py replay it unchanged:
 *   A   = R:  RAM where melonDS's RunFrame returns (end of line 262), what RetroArch evaluates
 *   F1  = P:  what the module evaluates for the frame (sealed capture, or the frame before's again)
 *   F2  = PF: P, but keeping the last completed capture of the frame when a later one stopped
 *   MID = F:  the first module capture after the VBlank (or the one before again, if none)
 *   VB  = V:  RAM at VBlank start (line 192)
 * Module frame f is matched to VBlank interval v = f + d, d = the smallest (queue interval - f) seen
 * (the module queues a frame at its first wake after the VBlank, never before it).
 * Also writes a CSV, one line per frame: v,f,stage,pf,fpresent,flag3ff8,late,p_full,pf_full,f_full,v_full
 * (x_full = bytes of the whole image where x differs from A).
 *
 *   ef2cov <evf.bin> <out cov.bin> <out frames.csv>
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define NIMG 0x404000u

static const uint8_t* B;
static size_t BN;
static uint32_t u32at(size_t p) { uint32_t v; memcpy(&v, B + p, 4); return v; }
/* skips a run list at p, returns the position after it (or 0 if it runs past the end) */
static size_t skipruns(size_t p)
{
  uint32_t n, i;
  if (p + 4 > BN) return 0;
  n = u32at(p); p += 4;
  for (i = 0; i < n; i++) {
    uint32_t len;
    if (p + 8 > BN) return 0;
    len = u32at(p + 4);
    p += 8 + len;
    if (p > BN) return 0;
  }
  return p;
}
static void applyruns(size_t p, uint8_t* img)
{
  uint32_t n = u32at(p), i;
  p += 4;
  for (i = 0; i < n; i++) {
    uint32_t off = u32at(p), len = u32at(p + 4);
    p += 8;
    if (off + len <= NIMG) memcpy(img + off, B + p, len);
    p += len;
  }
}

typedef struct { size_t at; uint32_t flag; int have; } iv_t;      /* per interval */
typedef struct { uint32_t fr, qv; uint8_t st, pf; size_t p, ppf; } q_t;

static FILE* out;
static void put32(uint32_t v) { fwrite(&v, 4, 1, out); }
/* writes b's runs against a (differences closer than 16 bytes joined); returns differing bytes */
static uint32_t runs(const uint8_t* a, const uint8_t* b)
{
  static uint32_t* r;
  static size_t cap;
  size_t n = 0, i = 0, k;
  uint32_t diff = 0;
  while (i < NIMG) {
    if (i + 64 <= NIMG && !memcmp(a + i, b + i, 64)) { i += 64; continue; }
    if (a[i] == b[i]) { i++; continue; }
    {
      size_t s = i, e = i + 1, j = i + 1;
      while (j < NIMG && j - e < 16) { if (a[j] != b[j]) e = j + 1; j++; }
      if (n + 2 > cap) { cap = cap * 2 + 4096; r = realloc(r, cap * sizeof *r); }
      r[n++] = (uint32_t)s; r[n++] = (uint32_t)(e - s);
      for (j = s; j < e; j++) diff += a[j] != b[j];
      i = e;
    }
  }
  put32((uint32_t)(n / 2));
  for (k = 0; k < n; k += 2) { put32(r[k]); put32(r[k + 1]); fwrite(b + r[k], 1, r[k + 1], out); }
  return diff;
}

int main(int argc, char** argv)
{
  int fd;
  struct stat st;
  size_t p = 4;
  iv_t *R = NULL, *V = NULL, *F = NULL;
  q_t* Q = NULL;
  uint32_t nint = 0, nq = 0, qcap = 0, icap = 0, i;
  int64_t d = INT64_MAX;
  uint8_t *imR, *imV, *imF, *imP, *imPF, *prevA;
  uint32_t doneR = 0, doneV = 0, doneF = 0;
  FILE* csv;
  uint32_t hist[8] = {0}, written = 0, gaps = 0;
  if (argc < 4) { fprintf(stderr, "usage: ef2cov evf.bin cov.bin frames.csv\n"); return 2; }
  fd = open(argv[1], O_RDONLY);
  if (fd < 0 || fstat(fd, &st)) { perror(argv[1]); return 2; }
  BN = (size_t)st.st_size;
  B = mmap(NULL, BN, PROT_READ, MAP_PRIVATE, fd, 0);
  if (B == MAP_FAILED || BN < 4 || memcmp(B, "EVF1", 4)) { fprintf(stderr, "bad log\n"); return 2; }
  /* pass 1: index */
  for (;;) {
    uint8_t tag;
    uint32_t v;
    size_t q;
    if (p + 5 > BN) break;
    tag = B[p]; v = u32at(p + 1); p += 5;
    if (v + 2 > icap) {
      uint32_t nc = (v + 2) * 2 + 1024;
      R = realloc(R, nc * sizeof *R); V = realloc(V, nc * sizeof *V); F = realloc(F, nc * sizeof *F);
      memset(R + icap, 0, (nc - icap) * sizeof *R); memset(V + icap, 0, (nc - icap) * sizeof *V); memset(F + icap, 0, (nc - icap) * sizeof *F);
      icap = nc;
    }
    if (tag == 'V') {
      if (p + 4 > BN) break;
      V[v].flag = u32at(p); p += 4;
      q = skipruns(p); if (!q) break;
      V[v].at = p; V[v].have = 1; p = q;
      if (v > nint) nint = v;
    } else if (tag == 'R') {
      q = skipruns(p); if (!q) break;
      R[v].at = p; R[v].have = 1; p = q;
    } else if (tag == 'F') {
      uint8_t h;
      if (p + 1 > BN) break;
      h = B[p++];
      if (h) { q = skipruns(p); if (!q) break; F[v].at = p; F[v].have = 1; p = q; }
      else F[v].have = 2;   /* no capture in that interval */
    } else if (tag == 'Q') {
      q_t e;
      if (p + 6 > BN) break;
      e.qv = v; e.fr = u32at(p); e.st = B[p + 4]; e.pf = B[p + 5]; p += 6;
      e.p = e.ppf = 0;
      if (e.st) { e.p = p; q = skipruns(p); if (!q) break; p = q; }
      if (e.pf) { e.ppf = p; q = skipruns(p); if (!q) break; p = q; }
      if (nq == qcap) { qcap = qcap * 2 + 4096; Q = realloc(Q, qcap * sizeof *Q); }
      Q[nq++] = e;
      if ((int64_t)e.qv - e.fr < d) d = (int64_t)e.qv - e.fr;
    } else {
      fprintf(stderr, "bad tag %d at %zu\n", tag, p - 5);
      break;
    }
  }
  if (!nq) { fprintf(stderr, "no frames\n"); return 1; }
  fprintf(stderr, "intervals %u, module frames %u, offset %lld\n", nint, nq, (long long)d);
  imR = calloc(1, NIMG); imV = calloc(1, NIMG); imF = calloc(1, NIMG); imP = calloc(1, NIMG); imPF = calloc(1, NIMG); prevA = calloc(1, NIMG);
  out = fopen(argv[2], "wb");
  csv = fopen(argv[3], "w");
  if (!out || !csv) { perror("output"); return 2; }
  fwrite("COV1", 1, 4, out);
  fprintf(csv, "v,f,stage,pf,fpresent,flag3ff8,late,p_full,pf_full,f_full,v_full\n");
  for (i = 0; i < nq; i++) {
    const q_t* e = &Q[i];
    int64_t v = (int64_t)e->fr + d;
    uint32_t pd, pfd, fd2, vd;
    if (i && e->fr != Q[i - 1].fr + 1) gaps++;
    /* the module's images advance even for frames that can't be matched */
    if (e->st) applyruns(e->p, imP);
    if (e->pf) applyruns(e->ppf, imPF);
    if (v < 1 || v > (int64_t)nint || v >= (int64_t)icap) continue;
    while (doneR < v) { doneR++; if (R[doneR].have) applyruns(R[doneR].at, imR); }
    while (doneV < v) { doneV++; if (V[doneV].have) applyruns(V[doneV].at, imV); }
    while (doneF < v) { doneF++; if (F[doneF].have == 1) applyruns(F[doneF].at, imF); }
    if (!R[v].have || !V[v].have || !F[v].have) continue;   /* the run ended before this interval's records */
    put32((uint32_t)v); put32(1);
    runs(prevA, imR); memcpy(prevA, imR, NIMG);
    fputc(1, out); pd = runs(imR, imP);
    fputc(1, out); pfd = runs(imR, imPF);
    fputc(1, out); fd2 = runs(imR, imF);
    fputc(1, out); vd = runs(imR, imV);
    {
      int64_t late = (int64_t)e->qv - v;
      hist[late < 0 ? 0 : late > 6 ? 7 : late + 1]++;
      fprintf(csv, "%lld,%u,%u,%u,%u,%08x,%lld,%u,%u,%u,%u\n", (long long)v, e->fr, e->st, e->pf, F[v].have == 1, V[v].flag, (long long)late, pd, pfd, fd2, vd);
    }
    written++;
  }
  fclose(out); fclose(csv);
  fprintf(stderr, "frames written %u, frame-number gaps %u, queue lateness (intervals) 0:%u 1:%u 2:%u 3:%u 4:%u 5:%u 6+:%u negative:%u\n",
          written, gaps, hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7], hist[0]);
  return 0;
}
