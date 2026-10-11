/* Minimal host that calls the rc_client entry points DScheevos would use.
 * Exists only so --gc-sections keeps what a real build keeps. */
#include "rc_client.h"
#include "rc_consoles.h"
#include <stddef.h>
static uint32_t rd(uint32_t a, uint8_t* b, uint32_t n, rc_client_t* c) { (void)a;(void)b;(void)c; return n; }
static void srv(const rc_api_request_t* r, rc_client_server_callback_t cb, void* d, rc_client_t* c) { (void)r;(void)cb;(void)d;(void)c; }
static void ev(const rc_client_event_t* e, rc_client_t* c) { (void)e;(void)c; }
static void cb(int r, const char* m, rc_client_t* c, void* u) { (void)r;(void)m;(void)c;(void)u; }
static void lg(const char* m, const rc_client_t* c) { (void)m;(void)c; }
volatile int sink;
int main(void) {
  char rp[128];
  rc_client_t* c = rc_client_create(rd, srv);
  rc_client_enable_logging(c, RC_CLIENT_LOG_LEVEL_WARN, lg);
  rc_client_set_event_handler(c, ev);
  rc_client_set_hardcore_enabled(c, 1);
  rc_client_begin_login_with_token(c, "u", "t", cb, NULL);
  rc_client_begin_identify_and_load_game(c, RC_CONSOLE_NINTENDO_DS, "sd:/game.nds", NULL, 0, cb, NULL);
  rc_client_do_frame(c);
  rc_client_idle(c);
  rc_client_reset(c);
  sink = (int)rc_client_get_rich_presence_message(c, rp, sizeof(rp));
  sink += rc_client_get_achievement_info(c, 1) != NULL;
  rc_client_unload_game(c);
  rc_client_destroy(c);
  return sink;
}
