#include "session.h"
#include "net.h"
#include "dscheevos_module.h"

#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"

#include <dswifi9.h>
#include <nds.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VERSION "0.1.0"

/* replies kept for the session file, in DSCM_PRELOAD_* order */
static char* g_reply[DSCM_PRELOAD_COUNT];
static size_t g_reply_len[DSCM_PRELOAD_COUNT];

static int g_done, g_result;
static char g_error[128];

static int request_kind(const char* post)
{
  if (!post) return -1;
  if (!strncmp(post, "r=login2&", 9)) return DSCM_PRELOAD_LOGIN;
  if (!strncmp(post, "r=achievementsets&", 18)) return DSCM_PRELOAD_SET;
  if (!strncmp(post, "r=startsession&", 15)) return DSCM_PRELOAD_SESSION;
  return -1;
}

/* rcheevos asks for a request; answer it right away (blocking). */
static void RC_CCONV server_call(const rc_api_request_t* request, rc_client_server_callback_t callback,
                                 void* callback_data, rc_client_t* client)
{
  http_response_t r;
  rc_api_server_response_t response;
  int kind = request_kind(request->post_data);
  (void)client;

  http_request(request->url, request->post_data, request->content_type, &r);
  memset(&response, 0, sizeof(response));
  response.http_status_code = r.status > 0 ? r.status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
  response.body = r.body ? r.body : "";
  response.body_length = r.length;
  if (r.status < 0)
    snprintf(g_error, sizeof(g_error), "%s", net_error());

  if (kind >= 0 && r.status == 200 && r.body) {
    free(g_reply[kind]);
    g_reply[kind] = r.body;          /* keep it; freed when replaced */
    g_reply_len[kind] = r.length;
    r.body = NULL;
    callback(&response, callback_data);
    return;
  }
  callback(&response, callback_data);
  http_free(&r);
}

static uint32_t RC_CCONV read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client)
{
  (void)address; (void)buffer; (void)client;
  return num_bytes ? 0 : 0;   /* no game memory here */
}

static void RC_CCONV on_done(int result, const char* error_message, rc_client_t* client, void* userdata)
{
  (void)client; (void)userdata;
  g_result = result;
  if (result != RC_OK && error_message && !g_error[0])   /* a network error says more */
    snprintf(g_error, sizeof(g_error), "%s", error_message);
  g_done = 1;
}

static rc_client_t* make_client(const dsc_settings_t* settings)
{
  char ua[128], clause[64];
  rc_client_t* client = rc_client_create(read_memory, server_call);
  if (!client)
    return NULL;
  if (strcmp(settings->host, "https://retroachievements.org") != 0)
    rc_client_set_host(client, settings->host);
  rc_client_set_hardcore_enabled(client, settings->hardcore);
  rc_client_get_user_agent_clause(client, clause, sizeof(clause));
  snprintf(ua, sizeof(ua), "DScheevos/" VERSION " (Nintendo DSi) %s", clause);
  http_set_user_agent(ua);
  return client;
}

static int wait_done(void)
{
  /* server_call answers synchronously, so this is already done */
  return g_done ? g_result : RC_INVALID_STATE;
}

int session_login(dsc_settings_t* settings, const char* password, const char** error)
{
  rc_client_t* client = make_client(settings);
  const rc_client_user_t* user;
  int result;
  *error = g_error;
  g_error[0] = '\0';
  if (!client) { strcpy(g_error, "Out of memory"); return SESSION_ERROR; }
  g_done = 0;
  rc_client_begin_login_with_password(client, settings->username, password, on_done, NULL);
  result = wait_done();
  user = rc_client_get_user_info(client);
  if (result == RC_OK && user && user->token) {
    snprintf(settings->token, sizeof(settings->token), "%s", user->token);
    snprintf(settings->username, sizeof(settings->username), "%s", user->username);
  }
  rc_client_destroy(client);
  if (result == RC_OK) return SESSION_OK;
  if (!g_error[0]) strcpy(g_error, "Login failed");
  return (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN) ? SESSION_LOGIN_FAILED : SESSION_ERROR;
}

int session_hash_rom(const char* rom_path, char hash[33])
{
  rc_hash_iterator_t it;
  int ok;
  rc_hash_initialize_iterator(&it, rom_path, NULL, 0);
  ok = rc_hash_generate(hash, RC_CONSOLE_NINTENDO_DS, &it);
  rc_hash_destroy_iterator(&it);
  return ok;
}

int session_start(const dsc_settings_t* settings, const char* rom_path, const char* hash,
                  session_info_t* info, const char** error)
{
  rc_client_t* client = make_client(settings);
  int result, i;
  (void)rom_path;
  *error = g_error;
  g_error[0] = '\0';
  memset(info, 0, sizeof(*info));
  for (i = 0; i < DSCM_PRELOAD_COUNT; i++) { free(g_reply[i]); g_reply[i] = NULL; g_reply_len[i] = 0; }
  if (!client) { strcpy(g_error, "Out of memory"); return SESSION_ERROR; }

  g_done = 0;
  rc_client_begin_login_with_token(client, settings->username, settings->token, on_done, NULL);
  result = wait_done();
  if (result != RC_OK) {
    rc_client_destroy(client);
    if (!g_error[0]) strcpy(g_error, "Login failed");
    return (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN || result == RC_ACCESS_DENIED)
      ? SESSION_LOGIN_FAILED : SESSION_ERROR;
  }
  {
    const rc_client_user_t* user = rc_client_get_user_info(client);
    if (user) {
      snprintf(info->display_name, sizeof(info->display_name), "%s", user->display_name);
      info->score = settings->hardcore ? user->score : user->score_casual;
    }
  }

  g_done = 0;
  rc_client_begin_load_game(client, hash, on_done, NULL);
  result = wait_done();
  if (result == RC_OK) {
    const rc_client_game_t* game = rc_client_get_game_info(client);
    rc_client_user_game_summary_t summary;
    rc_client_get_user_game_summary(client, &summary);
    snprintf(info->game_title, sizeof(info->game_title), "%s", game ? game->title : "");
    info->game_id = game ? game->id : 0;
    info->achievements = summary.num_promoted_achievements;
    info->unlocked = summary.num_unlocked_achievements;
    info->points_total = summary.points_available;
    info->points_unlocked = summary.points_unlocked;
  }
  rc_client_destroy(client);

  if (result == RC_NO_GAME_LOADED || (result == RC_OK && info->game_id == 0))
    return SESSION_NO_SET;
  if (result != RC_OK) {
    if (!g_error[0]) strcpy(g_error, "Couldn't load the game's achievements");
    return SESSION_ERROR;
  }
  for (i = 0; i < DSCM_PRELOAD_COUNT; i++) {
    if (!g_reply[i]) {
      strcpy(g_error, "Server reply missing");
      return SESSION_ERROR;
    }
  }
  return SESSION_OK;
}

int session_write(const dsc_settings_t* settings, const char* rom_path, const char* hash)
{
  dscm_boot_t boot;
  uint32_t offset = (sizeof(boot) + 3) & ~3u;
  static const char pad[4];
  static char ca_pem[8192];
  size_t ca_len = 0;
  FILE* f;
  FILE* ca;
  int i;

  memset(&boot, 0, sizeof(boot));
  boot.magic = DSCM_SESSION_MAGIC;
  boot.size = sizeof(boot);
  snprintf(boot.rom_path, sizeof(boot.rom_path), "%s", rom_path);
  snprintf(boot.host, sizeof(boot.host), "%s", settings->host);
  snprintf(boot.username, sizeof(boot.username), "%s", settings->username);
  snprintf(boot.token, sizeof(boot.token), "%s", settings->token);
  snprintf(boot.game_hash, sizeof(boot.game_hash), "%s", hash);
  boot.hardcore = (uint8_t)(settings->hardcore != 0);
  boot.unix_time = (uint32_t)time(NULL);
  for (i = 0; i < DSCM_PRELOAD_COUNT; i++) {
    boot.preload_offset[i] = offset;
    boot.preload_length[i] = (uint32_t)g_reply_len[i];
    offset += ((uint32_t)g_reply_len[i] + 3) & ~3u;
  }

  /* The game keeps using this Wi-Fi connection: pass its settings (no keys;
   * the chip keeps those itself). */
  {
    struct in_addr gateway, netmask, dns1, dns2;
    struct in_addr ip = Wifi_GetIPInfo(&gateway, &netmask, &dns1, &dns2);
    if (settings->live && ip.s_addr && Wifi_GetData(WIFIGETDATA_MACADDRESS, 6, boot.mac) >= 0) {
      boot.net_valid = 1;
      boot.ip = ip.s_addr;
      boot.netmask = netmask.s_addr;
      boot.gateway = gateway.s_addr;
      boot.dns = dns1.s_addr;
    }
  }
  /* so the module can rejoin it; nds-bootstrap adds the PMK in RAM */
  {
    net_ap_t ap;
    if (boot.net_valid && net_current_ap(&ap) == 0) {
      memcpy(boot.ap_ssid, ap.ssid, sizeof(boot.ap_ssid));
      boot.ap_ssid_len = ap.ssid_len;
      memcpy(boot.ap_bssid, ap.bssid, 6);
      boot.ap_channel = ap.channel;
      boot.ap_security = ap.security;
      boot.ap_pair_cipher = ap.pair_cipher;
      boot.ap_group_cipher = ap.group_cipher;
    }
  }
  /* extra trusted CA for a self-hosted or test server */
  ca = fopen(DSC_DIR "ca.pem", "rb");
  if (ca) {
    ca_len = fread(ca_pem, 1, sizeof(ca_pem), ca);
    fclose(ca);
    if (ca_len > 0 && ca_len < sizeof(ca_pem)) {
      boot.ca_offset = offset;
      boot.ca_length = (uint32_t)ca_len;
      offset += ((uint32_t)ca_len + 3) & ~3u;
    } else {
      ca_len = 0;
    }
  }
  boot.file_size = offset;
  if (offset > DSCM_SESSION_MAX)
    return 0;

  f = fopen(DSC_DIR "session.bin", "wb");
  if (!f)
    return 0;
  fwrite(&boot, 1, sizeof(boot), f);
  fwrite(pad, 1, ((sizeof(boot) + 3) & ~3u) - sizeof(boot), f);
  for (i = 0; i < DSCM_PRELOAD_COUNT; i++) {
    fwrite(g_reply[i], 1, g_reply_len[i], f);
    fwrite(pad, 1, ((g_reply_len[i] + 3) & ~3u) - g_reply_len[i], f);
  }
  if (ca_len) {
    fwrite(ca_pem, 1, ca_len, f);
    fwrite(pad, 1, ((ca_len + 3) & ~3u) - ca_len, f);
  }
  i = ferror(f);
  fclose(f);
  return !i;
}
