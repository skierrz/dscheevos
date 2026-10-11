/*
 * Link layer: Ethernet frames to and from the Atheros chip.
 *
 * The launcher left the chip associated; nds-bootstrap's ARM7 card engine
 * moves raw HTC frames between the chip's mailbox and the ring in
 * dscm_shim.h once per VBlank. Here the frames become Ethernet for lwIP:
 *   HTC header (6) | WMI data header (2) | 802.3: dst, src, length |
 *   LLC/SNAP (6) | ethertype | payload
 * WMI events (endpoint 1) tell us when the AP drops us.
 *
 * Cache: the ring sits in cached RAM. Lines the ARM9 writes are cleaned
 * after writing; lines the ARM7 writes are invalidated before reading.
 */
#include "net.h"
#include "dscm_shim.h"

#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

#include <stddef.h>
#include <string.h>

#define WMI_CONNECT_EVENT    0x1002
#define WMI_DISCONNECT_EVENT 0x1003
#define HTC_EP_WMI  1
#define HTC_EP_DATA 2

static shim_shared_t g_ring;     /* aligned to cache lines by SHIM_LINE */
static struct netif g_netif;
static net_config_t g_config;
static int g_connected = 1;      /* the launcher left us associated */
static uint32_t g_rx_frames, g_tx_frames, g_tx_drops;

static void publish_arm9_line(void)
{
  dscm_dc_clean(&g_ring, 32);
}

static uint32_t arm7_rx_head(void)
{
  dscm_dc_invalidate((const void*)&g_ring.attached, 32);
  return g_ring.rx_head;
}

static uint32_t arm7_tx_tail(void)
{
  dscm_dc_invalidate((const void*)&g_ring.attached, 32);
  return g_ring.tx_tail;
}

int link_up(void)
{
  uint32_t dead;
  dscm_dc_invalidate((const void*)&g_ring.attached, 32);
  dead = g_ring.dead;
  return g_connected && !dead && g_ring.attached;
}

void link_stir(void)
{
  uint32_t sample[2];
  sample[0] = *(volatile uint16_t*)0x04000006;   /* VCOUNT */
  sample[1] = *(volatile uint16_t*)0x04000100 | ((uint32_t)*(volatile uint16_t*)0x04000104 << 16); /* timers */
  entropy_stir(sample, sizeof(sample));
}

/* ---- TX ---- */

static err_t linkoutput(struct netif* netif, struct pbuf* p)
{
  static uint8_t frame[1536] __attribute__((aligned(4)));
  shim_slot_tx_t* slot;
  uint32_t head, n, body;
  uint8_t* m;
  (void)netif;

  if (!link_up())
    return ERR_IF;
  n = pbuf_copy_partial(p, frame, sizeof(frame), 0);
  if (n < 14)
    return ERR_ARG;
  body = n - 14;
  if (6 + 24 + body > SHIM_TX_MAX)
    return ERR_MEM;

  head = g_ring.tx_head;
  if (head - arm7_tx_tail() >= SHIM_TX_SLOTS) {
    g_tx_drops++;                 /* TCP retransmits */
    return ERR_MEM;
  }
  slot = &g_ring.tx[head % SHIM_TX_SLOTS];
  memset(slot->data, 0, 6 + 24);
  slot->data[0] = HTC_EP_DATA;
  slot->data[2] = (uint8_t)(24 + body);
  slot->data[3] = (uint8_t)((24 + body) >> 8);
  slot->data[4] = 0x08;           /* the control bytes dswifi uses for data */
  slot->data[5] = 0x20;
  m = slot->data + 6;
  memcpy(m + 2, frame, 6);        /* destination */
  memcpy(m + 8, frame + 6, 6);    /* source (our MAC) */
  m[14] = (uint8_t)((body + 8) >> 8);
  m[15] = (uint8_t)(body + 8);
  m[16] = 0xAA; m[17] = 0xAA; m[18] = 0x03;
  m[22] = frame[12];              /* ethertype */
  m[23] = frame[13];
  memcpy(m + 24, frame + 14, body);
  slot->len = 6 + 24 + body;
  dscm_dc_clean(slot, sizeof(*slot));
  g_ring.tx_head = head + 1;
  publish_arm9_line();
  g_tx_frames++;
  link_stir();
  return ERR_OK;
}

static err_t netif_setup(struct netif* netif)
{
  netif->name[0] = 'w';
  netif->name[1] = 'l';
  netif->output = etharp_output;
  netif->linkoutput = linkoutput;
  netif->mtu = 1500;
  netif->hwaddr_len = 6;
  memcpy(netif->hwaddr, g_config.mac, 6);
  netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
  return ERR_OK;
}

/* ---- RX ---- */

static void handle_wmi(uint16_t id)
{
  if (id == WMI_DISCONNECT_EVENT) {
    g_connected = 0;
    netif_set_link_down(&g_netif);
  } else if (id == WMI_CONNECT_EVENT) {
    g_connected = 1;
    netif_set_link_up(&g_netif);
  }
}

static void handle_frame(const uint8_t* d, uint32_t avail)
{
  uint32_t len, trailer, plen, dlen;
  const uint8_t* p = d + 6;
  struct pbuf* pb;

  if (avail < 6)
    return;
  len = d[2] | ((uint32_t)d[3] << 8);
  trailer = (d[1] & 2) ? d[4] : 0;
  if (len + 6 > avail || trailer > len)
    return;
  plen = len - trailer;

  if (d[0] == HTC_EP_WMI && plen >= 2) {
    handle_wmi((uint16_t)(p[0] | (p[1] << 8)));
    return;
  }
  if (d[0] != HTC_EP_DATA || plen < 24)
    return;
  dlen = ((uint32_t)p[14] << 8) | p[15];
  if (dlen < 8 || 16 + dlen > plen)
    return;

  /* rebuild an Ethernet II frame: dst, src, ethertype, payload */
  pb = pbuf_alloc(PBUF_RAW, (u16_t)(14 + dlen - 8), PBUF_POOL);
  if (!pb)
    return;
  pbuf_take_at(pb, p + 2, 6, 0);
  pbuf_take_at(pb, p + 8, 6, 6);
  pbuf_take_at(pb, p + 22, 2, 12);
  pbuf_take_at(pb, p + 24, (u16_t)(dlen - 8), 14);
  if (g_netif.input(pb, &g_netif) != ERR_OK)
    pbuf_free(pb);
  g_rx_frames++;
}

static void pump_rx(void)
{
  uint32_t head = arm7_rx_head();
  while (g_ring.rx_tail != head) {
    shim_slot_rx_t* slot = &g_ring.rx[g_ring.rx_tail % SHIM_RX_SLOTS];
    uint32_t n;
    dscm_dc_invalidate(slot, sizeof(*slot));
    n = slot->len <= SHIM_RX_MAX ? slot->len : SHIM_RX_MAX;
    link_stir();
    handle_frame(slot->data, n);
    g_ring.rx_tail++;
    publish_arm9_line();
  }
}

/* ---- setup and service ---- */

void* link_init(const net_config_t* config)
{
  ip4_addr_t ip, mask, gw;
  g_config = *config;
  memset(&g_ring, 0, sizeof(g_ring));
  g_ring.magic = SHIM_MAGIC;
  g_ring.run = 1;
  dscm_dc_clean(&g_ring, sizeof(g_ring));

  lwip_init();
  ip.addr = config->ip;
  mask.addr = config->netmask;
  gw.addr = config->gateway;
  netif_add(&g_netif, &ip, &mask, &gw, NULL, netif_setup, ethernet_input);
  netif_set_default(&g_netif);
  netif_set_up(&g_netif);
  netif_set_link_up(&g_netif);
  entropy_stir(config->mac, 6);
  return &g_ring;
}

/* Paused: the game is using its own (DS-mode) wireless, so the card engine
 * left the chip alone this VBlank. */
int link_paused(uint32_t* paused_count)
{
  static uint32_t last;
  uint32_t now;
  dscm_dc_invalidate((const void*)&g_ring.attached, offsetof(shim_shared_t, tx) - offsetof(shim_shared_t, attached));
  now = g_ring.stats.paused_ntr_wifi;
  *paused_count = now;
  if (now != last) {
    last = now;
    return 1;
  }
  return 0;
}

void link_service(void)
{
  pump_rx();
  sys_check_timeouts();
}

void link_stats(uint32_t* rx, uint32_t* tx, uint32_t* drops)
{
  *rx = g_rx_frames;
  *tx = g_tx_frames;
  *drops = g_tx_drops;
}
