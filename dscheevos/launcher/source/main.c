/*
 * DScheevos launcher. TWiLight Menu++ starts it in place of nds-bootstrap
 * (it is installed as _nds/nds-bootstrap-release.nds). It reads the game
 * from nds-bootstrap.ini, goes online to start a RetroAchievements session,
 * writes the session file, then boots the DScheevos nds-bootstrap, which
 * reads the same ini and loads the module with the session.
 */
#include "chainload.h"
#include "config.h"
#include "net.h"
#include "session.h"
#include "ui.h"

#include <dswifi9.h>
#include <fat.h>
#include <nds.h>
#include <stdio.h>
#include <string.h>

#define BOOTSTRAP_PATH DSC_DIR "nds-bootstrap.nds"

static dsc_settings_t settings;
static char rom_path[256];

static const char* basename_of(const char* path)
{
  const char* s = strrchr(path, '/');
  return s ? s + 1 : path;
}

/* Hands over to nds-bootstrap; only returns on failure. */
static void boot_game(int with_achievements)
{
  const char* argv[1] = { BOOTSTRAP_PATH };
  if (!with_achievements)
    remove(DSC_DIR "session.bin");
  ui_status(with_achievements ? "Starting with achievements..." : "Starting without achievements...");
  Wifi_DisableWifi();
  int err = chainload(BOOTSTRAP_PATH, 1, argv);
  ui_status("\x1b[31;1mCouldn't start nds-bootstrap (%d)\x1b[39;0m", err);
  ui_status("%s", err == CHAINLOAD_ERR_LOADER ? CHAINLOAD_LOADER_PATH : BOOTSTRAP_PATH);
  for (;;) swiWaitForVBlank();
}

static int game_is_dsi_mode(int ini_dsi_mode)
{
  unsigned char unit = 0;
  FILE* f = fopen(rom_path, "rb");
  if (f) {
    fseek(f, 0x12, SEEK_SET);
    if (fread(&unit, 1, 1, f) != 1) unit = 0;
    fclose(f);
  }
  return (unit & 2) && ini_dsi_mode;
}

static int log_in(void)
{
  char password[64];
  const char* error;
  for (;;) {
    int r;
    if (ui_choose("Log in to RetroAchievements to\n earn achievements.", "Log in", "Play without achievements") == 'B')
      return 0;
    if (!ui_input("Username", settings.username, sizeof(settings.username), 0))
      continue;
    password[0] = '\0';
    if (!ui_input("Password", password, sizeof(password), 1))
      continue;
    ui_status("Logging in as %s...", settings.username);
    if (net_connect(60 * 20) != 0) {
      ui_status("\x1b[31;1m%s\x1b[39;0m", net_error());
      continue;
    }
    r = session_login(&settings, password, &error);
    memset(password, 0, sizeof(password));
    if (r == SESSION_OK) {
      settings_save(&settings);
      ui_status("Logged in.");
      return 1;
    }
    ui_status("\x1b[31;1m%s\x1b[39;0m", error);
  }
}

static void settings_menu(void)
{
  for (;;) {
    int k;
    ui_clear_bottom();
    ui_bottom("\n Settings\n\n");
    ui_bottom(" \x1b[32;1mA\x1b[39;0m  Achievements: %s\n", settings.enabled ? "on" : "off");
    ui_bottom(" \x1b[32;1mX\x1b[39;0m  Hardcore: %s\n", settings.hardcore ? "on" : "off");
    ui_bottom(" \x1b[32;1mY\x1b[39;0m  Log out%s%s\n", settings.username[0] ? " " : "", settings.username);
    ui_bottom("\n \x1b[31;1mB\x1b[39;0m  Done\n");
    do {
      swiWaitForVBlank();
      scanKeys();
      k = keysDown();
    } while (!(k & (KEY_A | KEY_B | KEY_X | KEY_Y)));
    if (k & KEY_A) settings.enabled = !settings.enabled;
    if (k & KEY_X) settings.hardcore = !settings.hardcore;
    if (k & KEY_Y) { settings.token[0] = '\0'; settings.username[0] = '\0'; }
    if (k & KEY_B) { settings_save(&settings); return; }
  }
}

int main(int argc, char** argv)
{
  int dsi_mode;
  char hash[33];
  (void)argc; (void)argv;

  ui_init();
  ui_header("", NULL);
  if (!fatInitDefault()) {
    ui_status("\x1b[31;1mCan't read the SD card\x1b[39;0m");
    for (;;) swiWaitForVBlank();
  }
  settings_load(&settings);
  if (!bootstrap_ini_game(rom_path, sizeof(rom_path), &dsi_mode)) {
    ui_status("No game in _nds/nds-bootstrap.ini.");
    ui_status("Start games from TWiLight Menu++.");
    for (;;) swiWaitForVBlank();
  }
  ui_header(basename_of(rom_path), NULL);

  /* hold SELECT while the game starts for settings */
  scanKeys();
  if (keysHeld() & KEY_SELECT)
    settings_menu();

  if (!settings.enabled)
    boot_game(0);
  if (game_is_dsi_mode(dsi_mode)) {
    ui_wait("DSi-mode games don't support\n achievements yet.", 120);
    boot_game(0);
  }

  ui_status("Checking the game...");
  if (!session_hash_rom(rom_path, hash)) {
    ui_wait("Couldn't read the game file.", 120);
    boot_game(0);
  }

  for (;;) {
    session_info_t info;
    const char* error;
    int r;

    if (!settings.token[0] && !log_in())
      boot_game(0);

    ui_status("Connecting to Wi-Fi...");
    if (net_connect(60 * 20) != 0) {
      ui_status("\x1b[31;1m%s\x1b[39;0m", net_error());
      if (ui_choose("Achievements need the Internet\n when the game starts.", "Try again", "Play without achievements") == 'B')
        boot_game(0);
      continue;
    }

    ui_status("Starting the session...");
    r = session_start(&settings, rom_path, hash, &info, &error);
    if (r == SESSION_OK) {
      char line[40];
      snprintf(line, sizeof(line), "%u/%u achievements, %u/%u pts", info.unlocked, info.achievements,
               info.points_unlocked, info.points_total);
      ui_header(info.game_title, line);
      ui_status("%s%s (%u pts)", settings.hardcore ? "\x1b[31;1mHARDCORE\x1b[39;0m  " : "Softcore  ",
                info.display_name, info.score);
      if (!session_write(&settings, rom_path, hash)) {
        if (ui_choose("Couldn't write the session file.", "Try again", "Play without achievements") == 'B')
          boot_game(0);
        continue;
      }
      if (ui_wait("Have fun!\n\n B  Play without achievements", 90) & KEY_B)
        boot_game(0);
      boot_game(1);
    }
    if (r == SESSION_NO_SET) {
      ui_wait("RetroAchievements doesn't know\n this version of the game.", 150);
      boot_game(0);
    }
    if (r == SESSION_LOGIN_FAILED) {
      ui_status("\x1b[31;1m%s\x1b[39;0m", error);
      settings.token[0] = '\0';
      settings_save(&settings);
      continue;
    }
    ui_status("\x1b[31;1m%s\x1b[39;0m", error);
    if (ui_choose("Couldn't start the session.", "Try again", "Play without achievements") == 'B')
      boot_game(0);
  }
}
