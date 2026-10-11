/* lwIP options for the DScheevos module: NO_SYS (called only from the
 * game's idle time, one context), IPv4, TCP, UDP for DNS, static address
 * from the launcher. Memory comes from the module heap (malloc). */
#ifndef DSCM_LWIPOPTS_H
#define DSCM_LWIPOPTS_H

#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DNS                    1
#define LWIP_DHCP                   0
#define LWIP_AUTOIP                 0
#define LWIP_IGMP                   0
#define LWIP_RAW                    0
#define LWIP_STATS                  0
#define LWIP_NETIF_LINK_CALLBACK    0

#define MEM_LIBC_MALLOC             1
#define MEMP_MEM_MALLOC             1
#define MEM_ALIGNMENT               4
#define PBUF_POOL_SIZE              16
#define PBUF_POOL_BUFSIZE           1536
#define MEMP_NUM_TCP_PCB            2
#define MEMP_NUM_UDP_PCB            2
#define MEMP_NUM_TCP_SEG            16

#define TCP_MSS                     1460
#define TCP_WND                     (4 * TCP_MSS)
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_SND_QUEUELEN            16
#define LWIP_TCP_KEEPALIVE          1
#define DNS_TABLE_SIZE              2
#define DNS_MAX_NAME_LENGTH         64

#define LWIP_CHKSUM_ALGORITHM       2
#define LWIP_NOASSERT               1
#define LWIP_DEBUG                  0

unsigned int dscm_lwip_rand(void);
#define LWIP_RAND()                 dscm_lwip_rand()

#endif
