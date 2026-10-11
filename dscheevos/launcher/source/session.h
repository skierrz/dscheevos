/* Session start: log in, load the game's set and start a session on the
 * server with rcheevos, keeping the replies the in-game module replays. */
#ifndef DSC_SESSION_H
#define DSC_SESSION_H

#include "config.h"

typedef struct session_info_t {
  char game_title[64];
  unsigned game_id;
  unsigned achievements;
  unsigned unlocked;
  unsigned points_total;
  unsigned points_unlocked;
  char display_name[32];
  unsigned score;
} session_info_t;

enum {
  SESSION_OK = 0,
  SESSION_NO_SET,        /* the game isn't known to RetroAchievements */
  SESSION_LOGIN_FAILED,  /* token rejected: log in again */
  SESSION_ERROR,         /* network or server error: retry or play without */
};

/* Logs in with a password and stores the token in settings. */
int session_login(dsc_settings_t* settings, const char* password, const char** error);

int session_start(const dsc_settings_t* settings, const char* rom_path, const char* hash,
                  session_info_t* info, const char** error);

/* Writes sd:/_nds/dscheevos/session.bin for the session just started. */
int session_write(const dsc_settings_t* settings, const char* rom_path, const char* hash);

int session_hash_rom(const char* rom_path, char hash[33]);

#endif
