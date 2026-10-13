/* arm7-lab: the block the ARM9 hands the ARM7 benchmark payload (main RAM). */
#ifndef LAB7_H
#define LAB7_H

#include <stdint.h>

#define LAB7_PAYLOAD_BASE 0x02700000u   /* payload.ld: image origin, entry first */
#define LAB7_HEAP_END     0x02FE0000u   /* payload heap: from its bss end to here */
#define LAB7_MAX_FRAMES   256
#define LAB7_LOG_BYTES    4096

typedef struct lab7_params_t {
  /* in (ARM9) */
  const char* set_json;        /* NUL-terminated */
  uint8_t* ram;                /* 4 MB fake game RAM (dump or random) */
  uint8_t* dtcm;               /* 16 KB fake DTCM */
  uint32_t ram_dumped;
  uint32_t frames;             /* timed frames */
  uint32_t heap_end;
  /* out (ARM7) */
  uint32_t status;             /* 0 running, 1 done, 2 failed */
  uint32_t addresses;
  uint32_t evaluated, dropped;
  uint32_t cap[LAB7_MAX_FRAMES];    /* ARM7 cycles */
  uint32_t idle[LAB7_MAX_FRAMES];
  uint32_t heap_used;
  char log[LAB7_LOG_BYTES];
} lab7_params_t;

#endif
