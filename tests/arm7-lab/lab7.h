/* arm7-lab: the block the ARM9 hands the ARM7 benchmark payload (main RAM). */
#ifndef LAB7_H
#define LAB7_H

#include <stdint.h>

#define LAB7_PAYLOAD_BASE 0x02700000u   /* payload.ld: image origin, entry first */
#define LAB7_HEAP_END     0x02FE0000u   /* payload heap: from its bss end to here */
#define LAB7_MAX_FRAMES   256
#define LAB7_LOG_BYTES    4096

/* split mode (DSC_EVAL_CPU=split, arm7-split.nds): the ARM9 runs the core and
 * rc_client with the trigger evaluator; the ARM7 evaluates the claimed
 * triggers of each frame. One mailbox in main RAM, 32-byte lines: the ARM9
 * writes the header and work list then cleans them (by MVA) before writing
 * seq; the ARM7 writes one result line per entry. */
#define LAB7_SPLIT_MAX 2048
typedef struct lab7_work_t { uint32_t id; void* trigger; } lab7_work_t;
typedef struct lab7_result_t {
  volatile uint32_t seq;       /* frame whose result this is */
  volatile int32_t state;
  uint32_t pad[6];             /* one line per result: the ARM9 invalidates it alone */
} lab7_result_t;
typedef struct lab7_split_t {
  volatile uint32_t seq;       /* ARM9: frame to evaluate (written last, own line) */
  volatile uint32_t quit;
  uint32_t pad0[6];
  volatile uint32_t count;     /* entries in work[] for frame seq */
  volatile uint32_t arm7_cycles;   /* ARM7: cycles spent on the last frame */
  volatile uint32_t arm7_done;     /* ARM7: last frame finished */
  uint32_t pad1[5];
  lab7_work_t work[LAB7_SPLIT_MAX];
  lab7_result_t result[LAB7_SPLIT_MAX];
} lab7_split_t;

typedef struct lab7_params_t {
  /* in (ARM9) */
  const char* set_json;        /* NUL-terminated */
  uint8_t* ram;                /* 4 MB fake game RAM (dump or random) */
  uint8_t* dtcm;               /* 16 KB fake DTCM */
  uint32_t ram_dumped;
  uint32_t frames;             /* timed frames */
  uint32_t heap_end;
  lab7_split_t* split;         /* non-NULL: split mode, the ARM7 serves this mailbox until quit */
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
