/*
 * HTTPS for the module: takes queued requests from the net slots
 * (dscm_g_net, filled by rcheevos through module.c), sends them over one
 * keep-alive connection, and puts the replies back. Non-blocking: every
 * step returns at once, so net_service() can run in short idle slices.
 *
 * TLS is mbedTLS with the launcher's configuration and root CAs
 * (dscheevos/launcher/tls), plus the extra CA the launcher passes in the
 * session file for a self-hosted or test server.
 */
#include "net.h"

#include "lwip/dns.h"
#include "lwip/tcp.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include "dscheevos_module.h"
#include "mbedtls/platform.h"
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern dscm_net_slot_t dscm_g_net[DSCM_NET_SLOTS];
#include "dsc_cacerts.h"

void* link_init(const net_config_t* config);
void link_service(uint32_t now_ms);
int link_paused(uint32_t* paused_count);

#define REQUEST_TIMEOUT_MS 25000u
#define IDLE_CLOSE_MS      45000u
#define BODY_MAX           8192u

enum { C_NONE, C_DNS, C_CONNECTING, C_HANDSHAKE, C_READY, C_FAILED };
enum { R_NONE, R_SEND, R_HEAD, R_BODY };

static struct {
  int state;
  struct tcp_pcb* pcb;
  ip_addr_t addr;
  int https;
  char host[64];
  u16_t port;
  struct pbuf* rxq;        /* received, not yet read */
  int remote_closed;
  mbedtls_ssl_context ssl;
  int ssl_ready;
  uint32_t last_use;
  mbedtls_ssl_session saved;  /* for resumption on the next connection */
  int have_saved;
  uint32_t handshake_ms;      /* last handshake, wall time (diagnostics) */
  uint32_t handshake_start;
} C;

static struct {
  int state;
  int slot;
  char out[DSCM_NET_POST_MAX + 512];
  uint32_t out_len, out_off;
  char head[1024];
  uint32_t head_len;
  char body[BODY_MAX + 1];
  uint32_t body_len;
  int status;
  int chunked;
  int32_t content_length;
  int close_after;
  uint32_t started;
} R;

static net_config_t g_config;
static uint32_t g_now;
static uint32_t g_state = DSCM_NET_STATE_CONNECTING;
static uint32_t g_detail;
static int g_tls_ok;
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_x509_crt g_cas;
static mbedtls_ssl_config g_tls;

uint32_t net_state(void) { return g_state; }
uint32_t net_detail(void) { return g_detail; }
uint32_t net_handshake_ms(void) { return C.handshake_ms; }

/* ---- connection ---- */

static void conn_close(int failed)
{
  if (C.ssl_ready) {
    mbedtls_ssl_free(&C.ssl);
    C.ssl_ready = 0;
  }
  if (C.pcb) {
    tcp_arg(C.pcb, NULL);
    tcp_recv(C.pcb, NULL);
    tcp_err(C.pcb, NULL);
    if (tcp_close(C.pcb) != ERR_OK)
      tcp_abort(C.pcb);
    C.pcb = NULL;
  }
  if (C.rxq) {
    pbuf_free(C.rxq);
    C.rxq = NULL;
  }
  C.remote_closed = 0;
  C.state = failed ? C_FAILED : C_NONE;
}

static err_t on_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err)
{
  (void)arg; (void)pcb; (void)err;
  link_stir();
  if (!p) {
    C.remote_closed = 1;
    return ERR_OK;
  }
  if (C.rxq) pbuf_cat(C.rxq, p);
  else C.rxq = p;
  return ERR_OK;
}

static void on_err(void* arg, err_t err)
{
  (void)arg;
  C.pcb = NULL;         /* lwIP freed it */
  if (R.state == R_NONE) {
    conn_close(0);      /* the server dropped an idle keep-alive connection */
    return;
  }
  g_detail = (uint32_t)(-err) | 0x100;
  conn_close(1);
}

static err_t on_connected(void* arg, struct tcp_pcb* pcb, err_t err)
{
  (void)arg; (void)pcb;
  if (err != ERR_OK) {
    g_detail = (uint32_t)(-err) | 0x200;
    return ERR_OK;
  }
  link_stir();
  C.state = C.https ? C_HANDSHAKE : C_READY;
  return ERR_OK;
}

/* raw byte stream: >0 bytes, 0 nothing yet, -1 closed */
static int raw_read(unsigned char* buf, size_t n)
{
  u16_t got;
  if (!C.rxq)
    return C.remote_closed ? -1 : 0;
  got = pbuf_copy_partial(C.rxq, buf, (u16_t)(n > 0xFFFF ? 0xFFFF : n), 0);
  C.rxq = pbuf_free_header(C.rxq, got);
  if (C.pcb)
    tcp_recved(C.pcb, got);
  return got;
}

static int raw_write(const unsigned char* buf, size_t n)
{
  u16_t room;
  if (!C.pcb)
    return -1;
  room = tcp_sndbuf(C.pcb);
  if (n > room) n = room;
  if (n == 0)
    return 0;
  if (tcp_write(C.pcb, buf, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK)
    return 0;
  tcp_output(C.pcb);
  return (int)n;
}

static int bio_send(void* ctx, const unsigned char* buf, size_t len)
{
  int n = raw_write(buf, len);
  (void)ctx;
  if (n < 0) return MBEDTLS_ERR_NET_SEND_FAILED;
  if (n == 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
  return n;
}

static int bio_recv(void* ctx, unsigned char* buf, size_t len)
{
  int n = raw_read(buf, len);
  (void)ctx;
  if (n < 0) return 0;     /* EOF */
  if (n == 0) return MBEDTLS_ERR_SSL_WANT_READ;
  return n;
}

/* stream through TLS or not: >0 bytes, 0 would block, -1 closed/error */
static int conn_read(char* buf, size_t n)
{
  int r;
  if (!C.https)
    return raw_read((unsigned char*)buf, n);
  r = mbedtls_ssl_read(&C.ssl, (unsigned char*)buf, n);
  if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
  if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return -1;
  if (r < 0) { g_detail = (uint32_t)(-r); return -1; }
  return r;
}

static int conn_write(const char* buf, size_t n)
{
  int r;
  if (!C.https)
    return raw_write((const unsigned char*)buf, n);
  r = mbedtls_ssl_write(&C.ssl, (const unsigned char*)buf, n);
  if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
  if (r < 0) { g_detail = (uint32_t)(-r); return -1; }
  return r;
}

static void on_dns(const char* name, const ip_addr_t* addr, void* arg)
{
  (void)name; (void)arg;
  if (C.state != C_DNS)
    return;
  if (!addr) {
    g_detail = 0x300;
    C.state = C_FAILED;
    return;
  }
  C.addr = *addr;
  C.state = C_CONNECTING;
}

static int tls_setup(void)
{
  static const char pers[] = "DScheevos module";
  if (g_tls_ok)
    return 0;
  mbedtls_entropy_init(&g_entropy);
  mbedtls_ctr_drbg_init(&g_drbg);
  mbedtls_x509_crt_init(&g_cas);
  mbedtls_ssl_config_init(&g_tls);
  if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy, (const unsigned char*)pers, sizeof(pers) - 1) != 0
      || dsc_parse_der_list(&g_cas, dsc_cacerts, dsc_cacerts_size, 1) != 0   /* in place: read-only data */
      || mbedtls_ssl_config_defaults(&g_tls, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
    g_detail = 0x400;
    return -1;
  }
  mbedtls_ssl_conf_authmode(&g_tls, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&g_tls, &g_cas, NULL);
  mbedtls_ssl_conf_rng(&g_tls, mbedtls_ctr_drbg_random, &g_drbg);
  g_tls_ok = 1;
  return 0;
}

/* DER certificates (the launcher converts ca.pem), in the session file:
 * copied, as the arena takes the session's place. */
void https_add_ca(const char* der, uint32_t len)
{
  if (!len || tls_setup() != 0)
    return;
  dsc_parse_der_list(&g_cas, (const unsigned char*)der, len, 0);
}

/* Advance the connection one step. Returns 1 when ready for requests. */
static int conn_step(void)
{
  switch (C.state) {
    case C_NONE:
    case C_FAILED: {
      err_t e;
      C.state = C_DNS;
      e = dns_gethostbyname(C.host, &C.addr, on_dns, NULL);
      if (e == ERR_OK) C.state = C_CONNECTING;
      else if (e != ERR_INPROGRESS) { g_detail = 0x301; C.state = C_FAILED; return -1; }
      return 0;
    }
    case C_DNS:
      return 0;
    case C_CONNECTING:
      if (C.pcb)
        return 0;     /* waiting for on_connected */
      C.pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
      if (!C.pcb) { g_detail = 0x500; C.state = C_FAILED; return -1; }
      tcp_arg(C.pcb, NULL);
      tcp_recv(C.pcb, on_recv);
      tcp_err(C.pcb, on_err);
      if (C.https) {
        if (tls_setup() != 0) { conn_close(1); return -1; }
        mbedtls_ssl_init(&C.ssl);
        if (mbedtls_ssl_setup(&C.ssl, &g_tls) != 0 || mbedtls_ssl_set_hostname(&C.ssl, C.host) != 0) {
          mbedtls_ssl_free(&C.ssl);
          g_detail = 0x401;
          conn_close(1);
          return -1;
        }
        mbedtls_ssl_set_bio(&C.ssl, NULL, bio_send, bio_recv, NULL);
        if (C.have_saved)
          mbedtls_ssl_set_session(&C.ssl, &C.saved);
        C.ssl_ready = 1;
        C.handshake_start = g_now;
      }
      if (tcp_connect(C.pcb, &C.addr, C.port, on_connected) != ERR_OK) {
        g_detail = 0x501;
        conn_close(1);
        return -1;
      }
      return 0;
    case C_HANDSHAKE: {
      /* one step per slice, so achievements are evaluated in between; the
       * key exchange step is the slow one */
      int i;
      for (i = 0; i < 1; i++) {
        int r = mbedtls_ssl_handshake_step(&C.ssl);
        if (mbedtls_ssl_is_handshake_over(&C.ssl)) {
          C.state = C_READY;
          C.last_use = g_now;
          C.handshake_ms = g_now - C.handshake_start;
          if (C.have_saved)
            mbedtls_ssl_session_free(&C.saved);
          mbedtls_ssl_session_init(&C.saved);
          C.have_saved = mbedtls_ssl_get_session(&C.ssl, &C.saved) == 0;
          return 1;
        }
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
          return 0;
        if (r != 0) {
          g_detail = (uint32_t)(-r);
          if (mbedtls_ssl_get_verify_result(&C.ssl) != 0)
            g_detail = 0x402;   /* certificate not trusted */
          conn_close(1);
          return -1;
        }
      }
      return 0;
    }
    case C_READY:
      return 1;
  }
  return 0;
}

/* ---- requests ---- */

static int parse_url(const char* url, int* https, char* host, size_t host_size, u16_t* port, const char** path)
{
  const char* p;
  size_t n;
  if (!strncmp(url, "https://", 8)) { *https = 1; url += 8; *port = 443; }
  else if (!strncmp(url, "http://", 7)) { *https = 0; url += 7; *port = 80; }
  else return -1;
  for (p = url; *p && *p != '/' && *p != ':'; p++) {}
  n = (size_t)(p - url);
  if (!n || n >= host_size) return -1;
  memcpy(host, url, n);
  host[n] = '\0';
  if (*p == ':') *port = (u16_t)strtoul(p + 1, (char**)&p, 10);
  *path = *p ? p : "/";
  return 0;
}

static void finish(int status, const char* body, uint32_t len)
{
  dscm_net_slot_t* s = &dscm_g_net[R.slot];
  if (len > DSCM_NET_REPLY_MAX) len = DSCM_NET_REPLY_MAX;
  if (body && len) memcpy(s->reply, body, len);
  s->reply_length = len;
  s->http_status = status;
  s->state = DSCM_NET_REPLIED;
  R.state = R_NONE;
  R.slot = -1;
}

static void fail_request(void)
{
  finish(-2, "", 0);    /* retryable: rcheevos tries again later */
  conn_close(1);
}

static int start_request(void)
{
  int i, best = -1;
  int https;
  u16_t port;
  char host[64];
  const char* path;
  dscm_net_slot_t* s;
  if (C.state == C_READY && C.remote_closed)
    conn_close(0);      /* reconnect rather than write into a closed connection */
  for (i = 0; i < DSCM_NET_SLOTS; i++)
    if (dscm_g_net[i].state == DSCM_NET_REQUEST && (best < 0 || dscm_g_net[i].sequence < dscm_g_net[best].sequence))
      best = i;
  if (best < 0)
    return 0;
  s = &dscm_g_net[best];
  s->state = DSCM_NET_IN_FLIGHT;
  R.slot = best;
  if (parse_url(s->url, &https, host, sizeof(host), &port, &path) != 0) {
    finish(-1, "", 0);
    return 0;
  }
  if (C.state != C_NONE && C.state != C_FAILED && (strcmp(host, C.host) || port != C.port || https != C.https))
    conn_close(0);
  strcpy(C.host, host);
  C.port = port;
  C.https = https;
  R.out_len = (uint32_t)snprintf(R.out, sizeof(R.out),
    "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\n"
    "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %u\r\nConnection: keep-alive\r\n\r\n%s",
    path, host, g_config.user_agent, (unsigned)strlen(s->post), s->post);
  if (R.out_len >= sizeof(R.out)) {
    finish(-1, "", 0);
    return 0;
  }
  R.out_off = 0;
  R.head_len = 0;
  R.body_len = 0;
  R.status = 0;
  R.chunked = 0;
  R.content_length = -1;
  R.close_after = 0;
  R.started = g_now;
  R.state = R_SEND;
  return 1;
}

static void parse_head(void)
{
  char* line;
  R.head[R.head_len] = '\0';
  R.status = (R.head_len > 12) ? atoi(R.head + 9) : 0;
  for (line = strstr(R.head, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
    const char* h = line + 2;
    if (!strncasecmp(h, "content-length:", 15)) R.content_length = atol(h + 15);
    else if (!strncasecmp(h, "transfer-encoding:", 18) && strstr(h, "chunked")) R.chunked = 1;
    else if (!strncasecmp(h, "connection:", 11) && strncasecmp(h + 11 + strspn(h + 11, " "), "close", 5) == 0) R.close_after = 1;
  }
}

static uint32_t dechunk(char* b, uint32_t len)
{
  char* in = b;
  char* end = b + len;
  char* out = b;
  while (in < end) {
    char* next;
    unsigned long n = strtoul(in, &next, 16);
    char* eol = strstr(in, "\r\n");
    if (!eol || next == in) break;
    in = eol + 2;
    if (n == 0 || in + n > end) break;
    memmove(out, in, n);
    out += n;
    in += n + 2;
  }
  *out = '\0';
  return (uint32_t)(out - b);
}

static int chunked_done(void)
{
  R.body[R.body_len] = '\0';
  return R.body_len >= 5 && !memcmp(R.body + R.body_len - 5, "0\r\n\r\n", 5)
      && (R.body_len == 5 || !memcmp(R.body + R.body_len - 7, "\r\n", 2));
}

static void request_step(void)
{
  int n;
  if (g_now - R.started > REQUEST_TIMEOUT_MS) {
    g_detail = 0x600;
    fail_request();
    return;
  }
  if (R.state == R_SEND) {
    while (R.out_off < R.out_len) {
      n = conn_write(R.out + R.out_off, R.out_len - R.out_off);
      if (n < 0) { fail_request(); return; }
      if (n == 0) return;
      R.out_off += (uint32_t)n;
    }
    R.state = R_HEAD;
  }
  if (R.state == R_HEAD) {
    for (;;) {
      char* end;
      n = conn_read(R.head + R.head_len, 1);   /* byte by byte up to the blank line */
      if (n < 0) { fail_request(); return; }
      if (n == 0) return;
      R.head_len++;
      R.head[R.head_len] = '\0';
      end = strstr(R.head, "\r\n\r\n");
      if (end) {
        R.head_len = (uint32_t)(end - R.head) + 2;
        parse_head();
        R.state = R_BODY;
        break;
      }
      if (R.head_len >= sizeof(R.head) - 1) { fail_request(); return; }
    }
  }
  if (R.state == R_BODY) {
    for (;;) {
      if (!R.chunked && R.content_length >= 0 && R.body_len >= (uint32_t)R.content_length)
        break;
      if (R.chunked && chunked_done())
        break;
      if (R.body_len >= BODY_MAX) { fail_request(); return; }
      n = conn_read(R.body + R.body_len, BODY_MAX - R.body_len);
      if (n < 0) {
        if (!R.chunked && R.content_length < 0) { R.close_after = 1; break; }  /* ends at close */
        fail_request();
        return;
      }
      if (n == 0) return;
      R.body_len += (uint32_t)n;
    }
    if (R.chunked) R.body_len = dechunk(R.body, R.body_len);
    C.last_use = g_now;
    finish(R.status, R.body, R.body_len);
    if (R.close_after) conn_close(0);
  }
}

/* ---- service ---- */

static uint32_t g_paused_seen;

/* mbedTLS's memory through the cache. The module's heap may be seen through
 * the uncached 0x0C000000 mirror (small sets: dscheevos_module.h), and a
 * TLS handshake's bignum work there took longer than a Wi-Fi drop test
 * allows. Blocks are whole cache lines (32-byte aligned and rounded), used
 * only through the cached view while allocated, and written back and dropped
 * before newlib gets them back, so the two views never share a dirty line. */
extern void dscm_dc_clean(const void* addr, uint32_t len);
extern void dscm_dc_invalidate(const void* addr, uint32_t len);
extern void* heap_ptr(const void* p);   /* heap.h */
static void* tls_calloc(size_t n, size_t size)
{
  size_t bytes;
  void* p;
  if (size && n > (size_t)-1 / size)
    return NULL;
  bytes = (n * size + 31) & ~(size_t)31;
  p = memalign(32, bytes ? bytes : 32);
  if (!p)
    return NULL;
  p = (void*)DSCM_PHYS(p);
  memset(p, 0, bytes ? bytes : 32);
  return p;
}
static void tls_free(void* p)
{
  void* q;
  uint32_t len;
  if (!p)
    return;
  q = heap_ptr(p);   /* the heap's view, cached or not (heap_view): as memalign gave it */
  len = (uint32_t)malloc_usable_size(q) & ~31u;
  dscm_dc_clean(p, len);
  dscm_dc_invalidate(p, len);
  free(q);
}

void* net_init(const net_config_t* config)
{
  ip_addr_t dns;
  void* ring;
  mbedtls_platform_set_calloc_free(tls_calloc, tls_free);
  g_config = *config;
  memset(&C, 0, sizeof(C));
  memset(&R, 0, sizeof(R));
  R.slot = -1;
  ring = link_init(config);
  memset(g_config.pmk, 0, sizeof(g_config.pmk));   /* only the link needs it */
  ip_addr_set_ip4_u32(&dns, config->dns);
  dns_setserver(0, &dns);
  return ring;
}

int net_busy(void)
{
  return R.state != R_NONE || C.state == C_DNS || C.state == C_CONNECTING || C.state == C_HANDSHAKE
      || link_busy();
}

void net_service(uint32_t now_ms)
{
  uint32_t paused;
  g_now = now_ms;
  link_service(now_ms);

  if (link_paused(&paused)) {
    g_state = DSCM_NET_STATE_PAUSED;    /* the game uses its own wireless */
    return;
  }
  if (!link_up()) {
    g_state = link_rejoining() ? DSCM_NET_STATE_CONNECTING : DSCM_NET_STATE_OFFLINE;
    if (R.state != R_NONE) fail_request();
    return;
  }

  if (R.state == R_NONE && !start_request()) {
    if (C.state == C_READY && (C.remote_closed || g_now - C.last_use > IDLE_CLOSE_MS))
      conn_close(0);
    g_state = (C.state == C_FAILED) ? DSCM_NET_STATE_ERROR : DSCM_NET_STATE_ONLINE;
    return;
  }
  if (R.state == R_NONE)
    return;
  g_state = DSCM_NET_STATE_SENDING;
  if (conn_step() < 0) {
    fail_request();
    g_state = DSCM_NET_STATE_ERROR;
    return;
  }
  if (C.state == C_READY)
    request_step();
  (void)g_paused_seen;
}
