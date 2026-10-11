/*
 * DScheevos ARM9 module ABI: the contract between the module binary
 * (dscheevos9.bin) and the nds-bootstrap hook points that load and call it.
 *
 * Memory (cached mirror of the DSi's extra RAM, reserved by shrinking
 * nds-bootstrap's ROM cache):
 *
 *   DSCM_BASE           module image: header, code, data, bss
 *   DSCM_PRELOAD        server responses fetched at session start
 *   DSCM_HEAP           heap (rcheevos game data, snapshots)
 *   DSCM_STACK_TOP      stack used by dscm_idle (grows down)
 *
 * The loader (before the game boots) copies dscheevos9.bin to DSCM_BASE,
 * fills a dscm_boot_t, and calls header->init(). After that only two
 * calls happen, both from hooks:
 *   header->vblank()  from the ARM9 IPC sync IRQ that the ARM7 VBlank sends
 *   header->idle()    from the game's OS_Halt when it waits for an interrupt
 */
#ifndef DSCHEEVOS_MODULE_H
#define DSCHEEVOS_MODULE_H

#include <stdint.h>

#define DSCM_BASE        0x02D80000u
#define DSCM_SIZE        0x00200000u
#define DSCM_IMAGE_MAX   0x00040000u
#define DSCM_PRELOAD     (DSCM_BASE + DSCM_IMAGE_MAX)
#define DSCM_PRELOAD_MAX 0x00060000u
#define DSCM_HEAP        (DSCM_PRELOAD + DSCM_PRELOAD_MAX)
#define DSCM_STACK_SIZE  0x00002000u
#define DSCM_STACK_TOP   (DSCM_BASE + DSCM_SIZE)
#define DSCM_HEAP_END    (DSCM_STACK_TOP - DSCM_STACK_SIZE)

#define DSCM_MAGIC   0x56435344u  /* "DSCV" */
#define DSCM_VERSION 1u

/* IPC sync value the ARM7 VBlank handler sends to request a snapshot. */
#define DSCM_IPC_SYNC_VBLANK 0xA

/* Server responses fetched by the loader at session start. */
enum {
  DSCM_PRELOAD_LOGIN = 0,      /* r=login2 */
  DSCM_PRELOAD_SET,            /* r=achievementsets */
  DSCM_PRELOAD_SESSION,        /* r=startsession */
  DSCM_PRELOAD_COUNT
};

/* One request or response in the shared network queue. The module fills
 * requests; the network layer (ARM7 or loader) answers them. */
#define DSCM_NET_SLOTS      4
#define DSCM_NET_URL_MAX    192
#define DSCM_NET_POST_MAX   768
#define DSCM_NET_REPLY_MAX  1024
enum { DSCM_NET_FREE = 0, DSCM_NET_REQUEST, DSCM_NET_IN_FLIGHT, DSCM_NET_REPLIED };

typedef struct dscm_net_slot_t {
  volatile uint32_t state;
  uint32_t sequence;
  int32_t http_status;
  uint32_t reply_length;
  char url[DSCM_NET_URL_MAX];
  char post[DSCM_NET_POST_MAX];
  char reply[DSCM_NET_REPLY_MAX];
} dscm_net_slot_t;

/* Unlocks and health, readable by the in-game menu and the network layer. */
#define DSCM_RECENT_MAX 8
typedef struct dscm_unlock_t {
  uint32_t id;
  uint32_t points;
  uint32_t frame;
  char title[48];
} dscm_unlock_t;

typedef struct dscm_status_t {
  volatile uint32_t active;          /* set loaded, frames being evaluated */
  volatile uint32_t hardcore;
  volatile uint32_t frames_snapshotted;
  volatile uint32_t frames_evaluated;
  volatile uint32_t frames_dropped;
  volatile uint32_t frames_deferred;  /* idle windows too short to evaluate in */
  volatile uint32_t eval_lines;       /* running estimate of one evaluation's cost, in scanlines (x16) */
  volatile uint32_t unlock_count;     /* total; recent[] holds the last DSCM_RECENT_MAX */
  dscm_unlock_t recent[DSCM_RECENT_MAX];
  volatile uint32_t pending_requests;
  volatile uint32_t error;            /* nonzero: DScheevos stopped (see dscm_error_t) */
  char game_title[48];
} dscm_status_t;

enum {
  DSCM_ERROR_NONE = 0,
  DSCM_ERROR_BAD_BOOT,
  DSCM_ERROR_NO_MEMORY,
  DSCM_ERROR_LOGIN,
  DSCM_ERROR_GAME_LOAD,
};

typedef struct dscm_boot_t {
  uint32_t size;                      /* sizeof(dscm_boot_t) */
  char host[64];
  char username[32];
  char token[64];
  char game_hash[33];
  uint8_t hardcore;
  uint8_t reserved[2];
  uint32_t dtcm_base;                 /* game's DTCM address (CP15) */
  uint32_t unix_time;                 /* RTC time at boot, for unlock timestamps */
  uint32_t preload_offset[DSCM_PRELOAD_COUNT];  /* from DSCM_PRELOAD */
  uint32_t preload_length[DSCM_PRELOAD_COUNT];
} dscm_boot_t;

typedef struct dscm_header_t {
  uint32_t magic;
  uint32_t version;
  uint32_t image_size;
  int  (*init)(const dscm_boot_t* boot);
  void (*vblank)(void);
  void (*idle)(void);
  void (*shutdown)(void);
  dscm_status_t* status;
  dscm_net_slot_t* net;               /* DSCM_NET_SLOTS entries */
} dscm_header_t;

#define DSCM_HEADER ((dscm_header_t*)DSCM_BASE)

#endif
