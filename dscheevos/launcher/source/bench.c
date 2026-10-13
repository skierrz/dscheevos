#include "bench.h"

#include "rc_client.h"

#include <nds.h>
#include <stdlib.h>
#include <string.h>

volatile int bench_active;

/* Fake game RAM in RetroAchievements DS addresses: 4 MB of main RAM, then
 * the 16 KB DTCM at 0x1000000. Random, one word in eight shaped like a
 * pointer into main RAM, as tests/perf/realsets.c fills it: the qemu
 * instruction counts of the same sets use the same RAM. */
#define MAIN_SIZE 0x400000u
#define DTCM_START 0x1000000u
#define DTCM_SIZE 0x4000u
static uint8_t* g_ram;        /* MAIN_SIZE + DTCM_SIZE */
static uint32_t g_rng = 12345;

static uint32_t rnd(void)
{
  g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
  return g_rng;
}

uint32_t bench_read(uint32_t address, uint8_t* buffer, uint32_t num_bytes)
{
  /* as the module: a read not entirely inside one region fails whole */
  if (address < MAIN_SIZE && num_bytes <= MAIN_SIZE - address) {
    memcpy(buffer, g_ram + address, num_bytes);
    return num_bytes;
  }
  if (address - DTCM_START < DTCM_SIZE && num_bytes <= DTCM_START + DTCM_SIZE - address) {
    memcpy(buffer, g_ram + MAIN_SIZE + (address - DTCM_START), num_bytes);
    return num_bytes;
  }
  return 0;
}

static void fill_memory(void)
{
  uint32_t* w = (uint32_t*)g_ram;
  uint32_t i, x;
  for (i = 0; i < (MAIN_SIZE + DTCM_SIZE) / 4; i++) {
    x = rnd();
    w[i] = (x & 7) == 0 ? 0x02000000u | (x & 0x3FFFFCu) : x;
  }
}

/* what changes between two frames (realsets.c's default: 512 bytes) */
static void change_memory(void)
{
  int k;
  for (k = 0; k < 512; k++)
    g_ram[rnd() & (MAIN_SIZE - 1)] = (uint8_t)rnd();
  for (k = 0; k < 32; k++)
    g_ram[MAIN_SIZE + (rnd() & (DTCM_SIZE - 1))] = (uint8_t)rnd();
}

static int cmp_u32(const void* a, const void* b)
{
  uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
  return x < y ? -1 : x > y;
}

int bench_run(rc_client_t* client, int max_frames, uint32_t max_ms, bench_result_t* result)
{
  uint32_t* cycles;
  uint32_t start, limit;
  int n, was_fast = 0, spectator;

  memset(result, 0, sizeof(*result));
  g_ram = (uint8_t*)malloc(MAIN_SIZE + DTCM_SIZE);
  cycles = (uint32_t*)malloc((size_t)max_frames * sizeof(uint32_t));
  if (!g_ram || !cycles) {
    free(g_ram); free(cycles); g_ram = NULL;
    return -1;
  }
  fill_memory();

  /* session_prepare's client is a spectator already; never send what fake memory unlocks */
  spectator = rc_client_get_spectator_mode_enabled(client);
  if (!spectator)
    rc_client_set_spectator_mode_enabled(client, 1);
  bench_active = 1;
  if (isDSiMode())
    was_fast = setCpuClock(false);   /* the game's clock: 67 MHz */

  /* timers 0-1 count the bus clock (33.5 MHz): one tick is two ARM9 cycles at 67 MHz */
  cpuStartTiming(0);
  start = cpuGetTiming();
  limit = (uint32_t)((uint64_t)max_ms * 33514u);
  for (n = 0; n < max_frames; n++) {
    uint32_t t;
    change_memory();
    /* the game ran since our last frame: nothing of ours is cached */
    DC_FlushAll();
    IC_InvalidateAll();
    t = cpuGetTiming();
    rc_client_do_frame(client);
    cycles[n] = (cpuGetTiming() - t) * 2;
    if (cpuGetTiming() - start > limit) {
      n++;
      break;
    }
  }
  result->ms = (cpuGetTiming() - start) / 33514u;

  if (was_fast)
    setCpuClock(true);
  bench_active = 0;
  if (!spectator)
    rc_client_set_spectator_mode_enabled(client, 0);

  result->frames = (uint32_t)n;
  if (n) {
    uint32_t i;
    for (i = 0; i < (uint32_t)n; i++)
      if (cycles[i] > result->max)
        result->max = cycles[i];
    qsort(cycles, (size_t)n, sizeof(uint32_t), cmp_u32);
    result->median = cycles[n / 2];
  }
  free(cycles);
  free(g_ram);
  g_ram = NULL;
  return 0;
}
