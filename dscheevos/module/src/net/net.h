/*
 * The module's network stack: Ethernet frames over the Atheros chip
 * through the ARM7 SDIO shim (link.c), lwIP for TCP/IP, and HTTPS with
 * mbedTLS (https.c). Runs only in the game's idle time, never in an IRQ.
 */
#ifndef DSCM_NET_H
#define DSCM_NET_H

#include <stdint.h>
#include "dscheevos_module.h"

typedef struct net_config_t {
  uint8_t mac[6];
  uint32_t ip, netmask, gateway, dns;   /* network byte order */
  const char* host;                     /* "https://retroachievements.org" */
  const char* user_agent;
} net_config_t;

/* Returns the ring for the module header (0 if the network can't start). */
void* net_init(const net_config_t* config);
/* One slice of network work: frames in, lwIP timers, HTTPS requests. */
void net_service(uint32_t now_ms);
uint32_t net_state(void);   /* DSCM_NET_STATE_* */
uint32_t net_detail(void);  /* last error, for the in-game menu */
uint32_t net_handshake_ms(void);

/* link.c, used by https.c */
int link_up(void);
void link_stir(void);       /* mix event timing into the TLS entropy pool */

/* entropy.c */
void entropy_stir(const void* data, uint32_t len);

/* start.s */
void dscm_dc_clean(const void* addr, uint32_t len);
void dscm_dc_invalidate(const void* addr, uint32_t len);

#endif
