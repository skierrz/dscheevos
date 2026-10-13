/* Set benchmark: how much ARM9 time one frame of this set's evaluation takes
 * on this console, measured before the game starts (the launcher CPU gate).
 * rc_client evaluates the loaded set on fake game RAM at 67 MHz, the DS-mode
 * clock the game runs at, with both caches emptied before each frame as the
 * game's own work leaves them. Nothing reaches the server: the client is put
 * in spectator mode and session.c refuses requests while it runs. */
#ifndef DSC_BENCH_H
#define DSC_BENCH_H

#include <stdint.h>

typedef struct rc_client_t rc_client_t;

typedef struct bench_result_t {
  uint32_t frames;        /* frames measured */
  uint32_t median;        /* ARM9 cycles at 67 MHz per frame */
  uint32_t max;
  uint32_t ms;            /* wall time the benchmark took */
} bench_result_t;

/* 1 while bench_run() runs: session.c reads game memory through bench_read()
 * and answers every server request with an error. */
extern volatile int bench_active;

uint32_t bench_read(uint32_t address, uint8_t* buffer, uint32_t num_bytes);

/* Evaluates up to max_frames frames, stopping early after max_ms. Returns 0
 * and fills result, or -1 (no memory for the fake RAM: result is zeroed). */
int bench_run(rc_client_t* client, int max_frames, uint32_t max_ms, bench_result_t* result);

#endif
