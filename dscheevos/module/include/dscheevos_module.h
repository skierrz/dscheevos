/*
 * DScheevos ARM9 module ABI: the contract between the module binary
 * (dscheevos9.bin) and the nds-bootstrap hook points that load and call it.
 *
 * Memory (cached mirror of the DSi's extra RAM, reserved by shrinking
 * nds-bootstrap's ROM cache):
 *
 *   DSCM_BASE           module image: header, code, data, bss
 *   DSCM_SESSION        session file: dscm_boot_t, then server responses
 *   DSCM_HEAP           heap (rcheevos game data, snapshots)
 *   DSCM_IRQ_STACK_TOP  stack for header->vblank (grows down)
 *   DSCM_STACK_TOP      stack for header->halt (grows down)
 *
 * Before the game boots, nds-bootstrap's ARM7 copies dscheevos9.bin to
 * DSCM_BASE and the session file (written by the DScheevos launcher) to
 * DSCM_SESSION, then its ARM9 calls header->init(DSCM_SESSION). After that
 * only hooks call in:
 *   header->vblank()  from the ARM9 IPC sync IRQ. The ARM7 VBlank handler
 *                     bumps header->vblank_count, then sends the IRQ. The
 *                     module snapshots once per new count, so a sync value
 *                     overwritten by another IPC message loses nothing.
 *   header->halt()    replaces the game's OS_Halt: runs evaluation work,
 *                     then waits for an interrupt as OS_Halt did.
 *   header->reset()   from nds-bootstrap's game reset (in-game menu, soft
 *                     reset), IRQs off: achievement state resets in idle.
 * The ARM7 also reads header->notify_count (kept in RAM, past the ARM9's
 * cache) to signal unlocks with the Wi-Fi LED, and header->net_ring, the
 * frame ring its SDIO shim fills and drains (dscm_shim.h).
 */
#ifndef DSCHEEVOS_MODULE_H
#define DSCHEEVOS_MODULE_H

#include <stdint.h>

#define DSCM_BASE        0x02D80000u
#define DSCM_SIZE        0x00200000u
#define DSCM_IMAGE_MAX   0x00080000u   /* code incl. TCP/IP and TLS, data, bss */
#define DSCM_SESSION     (DSCM_BASE + DSCM_IMAGE_MAX)
#define DSCM_SESSION_MAX 0x00040000u
#define DSCM_HEAP        (DSCM_SESSION + DSCM_SESSION_MAX)
#define DSCM_STACK_SIZE  0x00002000u
#define DSCM_STACK_TOP   (DSCM_BASE + DSCM_SIZE)
#define DSCM_IRQ_STACK_SIZE 0x00001000u
#define DSCM_IRQ_STACK_TOP  (DSCM_STACK_TOP - DSCM_STACK_SIZE)
#define DSCM_HEAP_END    (DSCM_IRQ_STACK_TOP - DSCM_IRQ_STACK_SIZE)

#define DSCM_MAGIC   0x56435344u  /* "DSCV" */
#define DSCM_VERSION 5u
#define DSCM_SESSION_MAGIC 0x53435344u  /* "DSCS" */

/* IPC sync value the ARM7 VBlank handler sends to request a snapshot. */
#define DSCM_IPC_SYNC_VBLANK 0xA

/* Server responses fetched by the launcher at session start. */
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
#define DSCM_NET_REPLY_MAX  4096
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
  volatile uint32_t frames_dropped;   /* snapshot taken, no slot free to evaluate it */
  volatile uint32_t frames_missed;    /* VBlanks that passed with no snapshot (IRQs held off) */
  volatile uint32_t eval_lines;       /* average wall time of one idle pass, in scanlines (x16) */
  volatile uint32_t unlock_count;     /* total; recent[] holds the last DSCM_RECENT_MAX */
  dscm_unlock_t recent[DSCM_RECENT_MAX];
  volatile uint32_t pending_requests;
  volatile uint32_t error;            /* nonzero: DScheevos stopped (see dscm_error_t) */
  char game_title[48];
  /* the core set, for the in-game menu */
  volatile uint32_t achievements;
  volatile uint32_t achievements_unlocked;
  volatile uint32_t points;
  volatile uint32_t points_unlocked;
  /* network, for the in-game menu */
  volatile uint32_t net_state;        /* DSCM_NET_STATE_* */
  volatile uint32_t net_detail;       /* last error code, 0 if none */
  volatile uint32_t unlocks_sent;     /* award requests the server answered */
  volatile uint32_t tls_handshake_ms; /* last HTTPS handshake, wall time (diagnostics) */
  volatile uint32_t net_rejoins;      /* times we reassociated after a drop or a group rekey */
} dscm_status_t;

enum {
  DSCM_NET_STATE_NONE = 0,            /* no network in this session */
  DSCM_NET_STATE_CONNECTING,
  DSCM_NET_STATE_ONLINE,
  DSCM_NET_STATE_SENDING,
  DSCM_NET_STATE_PAUSED,              /* the game is using its own wireless */
  DSCM_NET_STATE_OFFLINE,             /* the access point dropped us */
  DSCM_NET_STATE_ERROR,               /* last connection attempt failed */
};

enum {
  DSCM_ERROR_NONE = 0,
  DSCM_ERROR_BAD_BOOT,
  DSCM_ERROR_NO_MEMORY,
  DSCM_ERROR_LOGIN,
  DSCM_ERROR_GAME_LOAD,
};

/* Start of the session file (sd:/_nds/dscheevos/session.bin). The launcher
 * writes it after going online; nds-bootstrap uses it only for the ROM it
 * names, then deletes it, so every play session starts online. */
typedef struct dscm_boot_t {
  uint32_t magic;                     /* DSCM_SESSION_MAGIC */
  uint32_t size;                      /* sizeof(dscm_boot_t) */
  uint32_t file_size;                 /* whole session file, <= DSCM_SESSION_MAX */
  char rom_path[256];                 /* as in nds-bootstrap.ini NDS_PATH */
  char host[64];
  char username[32];
  char token[64];
  char game_hash[33];
  uint8_t hardcore;
  uint8_t reserved[2];
  uint32_t dtcm_base;                 /* 0: read the game's DTCM base from CP15 */
  uint32_t unix_time;                 /* RTC time at boot, for unlock timestamps */
  uint32_t preload_offset[DSCM_PRELOAD_COUNT];  /* from the start of this struct */
  uint32_t preload_length[DSCM_PRELOAD_COUNT];
  /* the launcher's Wi-Fi connection, which the game keeps using (no secrets) */
  uint8_t mac[6];
  uint8_t net_valid;                  /* 0: no network during play */
  uint8_t reserved2;
  uint32_t ip, netmask, gateway, dns; /* network byte order */
  uint32_t ca_offset, ca_length;      /* extra trusted CA (PEM), from the start of this struct */
  /* the access point, so the module can rejoin it (no secrets in the file) */
  char ap_ssid[32];
  uint8_t ap_ssid_len;
  uint8_t ap_bssid[6];
  uint8_t ap_channel;
  uint8_t ap_security;                /* DSCM_AP_* */
  uint8_t ap_pair_cipher;             /* DSCM_CIPHER_* */
  uint8_t ap_group_cipher;
  uint8_t pmk_valid;                  /* the two pmk fields are 0 in the file: nds-bootstrap */
  uint8_t pmk[32];                    /* fills them in RAM from the console's own Wi-Fi settings */
} dscm_boot_t;

/* dswifi's numbering, which the launcher reads off its connection */
enum { DSCM_AP_OPEN = 0, DSCM_AP_WEP, DSCM_AP_WPA, DSCM_AP_WPA2 };
enum { DSCM_CIPHER_NONE = 1, DSCM_CIPHER_WEP, DSCM_CIPHER_TKIP, DSCM_CIPHER_CCMP };

typedef struct dscm_header_t {
  uint32_t magic;
  uint32_t version;
  uint32_t image_size;
  int  (*init)(const dscm_boot_t* boot);
  void (*vblank)(void);
  void (*halt)(void);                 /* ARM code: the OS_Halt patch jumps here */
  void (*shutdown)(void);
  dscm_status_t* status;
  dscm_net_slot_t* net;               /* DSCM_NET_SLOTS entries */
  volatile uint32_t vblank_count;     /* 0x24: written by the ARM7 VBlank hook */
  void (*reset)(void);                /* 0x28 */
  uint32_t reserved[5];
  volatile uint32_t notify_count;     /* 0x40: unlocks so far; own cache line, cleaned to RAM */
  void* net_ring;                     /* 0x44: dscm_shim.h ring, 0 if none; same line */
  uint32_t reserved2[6];
} dscm_header_t;

#define DSCM_HEADER ((dscm_header_t*)DSCM_BASE)

#endif
