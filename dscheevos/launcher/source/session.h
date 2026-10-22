/* Session start: log in, load the game's set and start a session on the
 * server with rcheevos, keeping the replies the in-game module replays. */
#ifndef DSC_SESSION_H
#define DSC_SESSION_H

#include <stdint.h>
#include "config.h"
#include "bench.h"

typedef struct session_info_t {
  char game_title[64];
  unsigned game_id;
  unsigned achievements;
  unsigned unlocked;
  unsigned points_total;
  unsigned points_unlocked;
  char display_name[32];
  unsigned score;
  int hardcore_refused;   /* the server sent its "unsupported emulator" warning: hardcore unlocks count as softcore */
} session_info_t;

enum {
  SESSION_OK = 0,
  SESSION_NO_SET,        /* the game isn't known to RetroAchievements */
  SESSION_LOGIN_FAILED,  /* token rejected: log in again */
  SESSION_ERROR,         /* network or server error: retry or play without */
  SESSION_TOO_BIG,       /* the set is too big for the module's memory: play without */
};

/* Logs in with a password and stores the token in settings. */
int session_login(dsc_settings_t* settings, const char* password, const char** error);

/* Logs in with the token and fetches the game's set in spectator mode (no
 * session on the server), then times the set's evaluation (bench.h) for the
 * CPU gate. A session_start() right after it reuses both replies. */
int session_prepare(const dsc_settings_t* settings, const char* hash, bench_result_t* bench, const char** error);

int session_start(const dsc_settings_t* settings, const char* rom_path, const char* hash,
                  session_info_t* info, const char** error);

/* Writes sd:/_nds/dscheevos/session.bin for the session just started. */
int session_write(const dsc_settings_t* settings, const char* rom_path, const char* hash);

/* Nonzero after session_prepare() or session_write() found the set too big for
 * the module (DSCM_SESSION_MAX): trying again can't help. */
int session_too_big(void);

/* Appends "api: HTTP status" (or the network error) to launcher.log. */
void session_log_request(const char* post, int status);

int session_hash_rom(const char* rom_path, char hash[33]);

/* Each achievement of the loaded set with a badge (after session_start OK). */
typedef struct session_badge_t {
  uint32_t id;
  char name[16];                      /* RA badge_name */
} session_badge_t;
int session_badges(const session_badge_t** out);

/* LAB (strip-lab): a dscm_badges_t block for session_write to append (0 = none). */
void session_set_badges(const void* block, uint32_t length);

#endif
