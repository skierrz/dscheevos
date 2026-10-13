/*
 * arm7-lab ARM9: loads the set and RAM (sd:/dscheevos-lab/set.json + ram.bin,
 * as tests/cpi-lab's set bench; else the synthetic typical set and random
 * RAM), copies the ARM7 payload to LAB7_PAYLOAD_BASE and has the ARM7 run
 * it; then prints its log. The ARM9 halts (swiWaitForVBlank) meanwhile, so
 * it barely touches the main RAM bus. Results on screen, in the emulator log
 * (@@ lines) and in sd:/dscheevos-lab/arm7.txt.
 */
#include <nds.h>
#include <fat.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <sys/stat.h>

#include "lab7.h"
#include SET_HEADER

extern const u8 payload_bin[], payload_bin_end[];

static char logbuf[8192];
static int loglen;
static void say(const char* fmt, ...)
{
  char b[200], m[208] = "@@";
  va_list ap;
  int n;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  iprintf("%s\n", b);
  n = strlen(b);
  if (loglen + n + 2 < (int)sizeof logbuf) {
    memcpy(logbuf + loglen, b, n);
    loglen += n;
    logbuf[loglen++] = '\n';
  }
  strcat(m, b);
  strcat(m, "\n");
  nocashMessage(m);
}

static char* load_file(const char* path, u32* size, u32 align, u32 min_alloc)
{
  FILE* f = fopen(path, "rb");
  char* b;
  long n;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  b = (char*)memalign(align, (u32)n + 1 > min_alloc ? (u32)n + 1 : min_alloc);
  if (b && fread(b, 1, n, f) == (size_t)n) b[n] = 0;
  else { free(b); b = NULL; }
  fclose(f);
  if (size) *size = (u32)n;
  return b;
}

static lab7_params_t params __attribute__((aligned(32)));

int main(void)
{
  int ok, i;
  u32 n = 0, rng = 12345;
  char* r;
  lab7_params_t* p = &params;

  consoleDemoInit();
  if (!isDSiMode()) {
    iprintf("arm7-lab needs DSi mode.\n");
    for (;;) swiWaitForVBlank();
  }
  ok = fatInitDefault();
  memset(p, 0, sizeof(*p));
  p->set_json = (const char*)patch_json;
  p->frames = 60;
  p->heap_end = LAB7_HEAP_END;
  if (ok) {
    char* j = load_file("sd:/dscheevos-lab/set.json", NULL, 4, 0);
    if (j) p->set_json = j;
    r = load_file("sd:/dscheevos-lab/ram.bin", &n, 32, 0x404000);
    if (r && n >= 0x400000) {
      p->ram = (u8*)r;
      p->dtcm = (u8*)r + 0x400000;
      if (n < 0x404000) memset(p->dtcm, 0, 0x4000);
      p->ram_dumped = 1;
    }
    else free(r);
    {
      FILE* f = fopen("sd:/dscheevos-lab/frames.txt", "r");
      if (f) { int k; if (fscanf(f, "%d", &k) == 1 && k > 0) p->frames = k; fclose(f); }
    }
  }
  if (!p->ram) {
    u32* w;
    p->ram = (u8*)memalign(32, 0x404000);
    if (!p->ram) { say("FAIL no memory for the fake RAM"); for (;;) swiWaitForVBlank(); }
    p->dtcm = p->ram + 0x400000;
    memset(p->dtcm, 0, 0x4000);
    w = (u32*)p->ram;
    /* as tests/cpi-lab: words that are often pointers into main RAM */
#define NEXT (rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5, (rng & 7) == 0 ? 0x02000000u | (rng & 0x3FFFFCu) : rng)
    for (i = 0; i < 0x400000 / 4; i++) w[i] = NEXT;
#undef NEXT
  }
  say("arm7-lab: rcheevos + core on the ARM7 (%s set, %s RAM, %lu frames)",
      p->set_json == (const char*)patch_json ? "typical" : "SD", p->ram_dumped ? "dumped" : "random", (unsigned long)p->frames);
  if ((u32)p->ram + 0x404000 > LAB7_PAYLOAD_BASE || (u32)p->set_json + strlen(p->set_json) > LAB7_PAYLOAD_BASE) {
    say("FAIL ARM9 buffers reach the payload area");
    for (;;) swiWaitForVBlank();
  }
  memcpy((void*)LAB7_PAYLOAD_BASE, payload_bin, payload_bin_end - payload_bin);
  say("payload %lu bytes", (unsigned long)(payload_bin_end - payload_bin));
  /* NWRAM-B and -C, all 8 slots each, to the ARM7 (for the NWRAM data variant) */
  *(vu32*)0x04004044 = 0x8D898581u; *(vu32*)0x04004048 = 0x9D999591u;
  *(vu32*)0x0400404C = 0x8D898581u; *(vu32*)0x04004050 = 0x9D999591u;
  DC_FlushAll();
  fifoSendValue32(FIFO_USER_01, (u32)p);
  while (!fifoCheckValue32(FIFO_USER_01))
    swiWaitForVBlank();
  fifoGetValue32(FIFO_USER_01);
  DC_InvalidateRange(p, sizeof(*p));
  {
    char* s = p->log;
    while (*s) {
      char* e = strchr(s, '\n');
      if (e) *e = 0;
      say("%s", s);
      if (!e) break;
      s = e + 1;
    }
  }
  /* hand-over cost for "one rc_client, either CPU" (ARM9 side): clean +
   * invalidate the D-cache over the shared image's range, by line (only our
   * lines) or whole (also evicts the game's), and flip the NWRAM slots' master */
  {
    static const u32 sizes[] = { 64 * 1024, 160 * 1024, 320 * 1024, 512 * 1024 };
    u32 t0, t1, k, ime = REG_IME;
    cpuStartTiming(0);
    REG_IME = 0;
    for (k = 0; k < 4; k++) {
      t0 = cpuGetTiming(); DC_FlushRange((void*)0x03100000, sizes[k]); t1 = cpuGetTiming();
      say("hand-over: DC clean+invalidate by line, %3lu KB: %6lu ARM9 cycles", (unsigned long)(sizes[k] >> 10), (unsigned long)((t1 - t0) * 2));
    }
    t0 = cpuGetTiming(); DC_FlushAll(); t1 = cpuGetTiming();
    say("hand-over: DC clean+invalidate all (4 KB): %6lu ARM9 cycles", (unsigned long)((t1 - t0) * 2));
    t0 = cpuGetTiming(); IC_InvalidateAll(); t1 = cpuGetTiming();
    say("hand-over: IC invalidate all: %6lu ARM9 cycles", (unsigned long)((t1 - t0) * 2));
    t0 = cpuGetTiming();
    *(vu32*)0x04004044 = 0x8D898581u; *(vu32*)0x04004048 = 0x9D999591u;
    *(vu32*)0x0400404C = 0x8D898581u; *(vu32*)0x04004050 = 0x9D999591u;
    t1 = cpuGetTiming();
    say("hand-over: 4 MBK writes (16 slots): %6lu ARM9 cycles", (unsigned long)((t1 - t0) * 2));
    REG_IME = ime;
  }
  say("DONE");
  if (ok) {
    FILE* f;
    mkdir("sd:/dscheevos-lab", 0777);
    f = fopen("sd:/dscheevos-lab/arm7.txt", "w");
    if (f) { fwrite(logbuf, 1, loglen, f); fclose(f); }
  }
  for (;;) swiWaitForVBlank();
}
