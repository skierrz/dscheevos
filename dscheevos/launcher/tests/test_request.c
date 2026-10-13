/* "Skip unofficial achievements": off, the achievementsets request is exactly
 * what rcheevos builds; on, "&f=3" is appended and nothing else changes. */
#include "request.h"
#include "rc_api_runtime.h"
#include "rc_api_user.h"

#include <stdio.h>
#include <string.h>

static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
  rc_api_fetch_game_sets_request_t params;
  rc_api_start_session_request_t start;
  rc_api_request_t req, req2;
  char buf[512], expect[512];
  const char* out;

  memset(&params, 0, sizeof(params));
  params.username = "tester";
  params.api_token = "Tok3n";
  params.game_hash = "4432098e35a63d0f2ff7bdbc7a848e3c";
  CHECK(rc_api_init_fetch_game_sets_request(&req, &params) == RC_OK);

  /* off: the very bytes rcheevos built */
  out = request_post(req.post_data, 0, buf, sizeof(buf));
  CHECK(out == req.post_data);

  /* on: rcheevos' bytes, then &f=3 */
  out = request_post(req.post_data, 1, buf, sizeof(buf));
  snprintf(expect, sizeof(expect), "%s&f=3", req.post_data);
  CHECK(out == buf && !strcmp(out, expect));
  CHECK(!strncmp(out, req.post_data, strlen(req.post_data)));
  printf("rcheevos: %s\nskip on:  %s\n", req.post_data, out);

  /* only achievementsets */
  memset(&start, 0, sizeof(start));
  start.username = "tester";
  start.api_token = "Tok3n";
  start.game_id = 11732;
  CHECK(rc_api_init_start_session_request(&req2, &start) == RC_OK);
  CHECK(request_post(req2.post_data, 1, buf, sizeof(buf)) == req2.post_data);
  CHECK(request_post(NULL, 1, buf, sizeof(buf)) == NULL);
  /* no room: sent as rcheevos built it */
  CHECK(request_post(req.post_data, 1, buf, 8) == req.post_data);

  rc_api_destroy_request(&req);
  rc_api_destroy_request(&req2);
  printf("%d checks, %d failed\n", checks, failed);
  return failed != 0;
}
