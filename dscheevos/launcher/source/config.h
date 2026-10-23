/* DScheevos settings (sd:/_nds/dscheevos/dscheevos.ini) and the game to boot
 * (sd:/_nds/nds-bootstrap.ini, written by TWiLight Menu++). */
#ifndef DSC_CONFIG_H
#define DSC_CONFIG_H

#define DSC_DIR "sd:/_nds/dscheevos/"

typedef struct dsc_settings_t {
  int enabled;          /* 0: boot games without achievements */
  int hardcore;
  int live;             /* send unlocks during play over the session's Wi-Fi connection */
  int spectator;        /* SPECTATOR = 1 (ini only): play and show, send nothing */
  int skip_unofficial;  /* SKIP_UNOFFICIAL = 1: ask the server for the promoted achievements only (f=3) */
  int badges;           /* LAB BADGES (default 1): fetch the set's badges for the notice bar */
  int notice_big;       /* LAB NOTICE_SIZE: 0 thin (default), 1 big (32 lines; the badges are 32x32 too) */
  int notices;          /* LAB NOTICES (default 1): 0 = no on-screen bar (the LED and the menu only) */
  int notice_edge;      /* LAB (strip-lab) NOTICE_EDGE: 0 auto, 1 top, 2 bottom (where on-screen notices go) */
  char username[32];
  char token[64];       /* from the server at login; the password is never stored */
  char host[64];
} dsc_settings_t;

/* Hardcore always keeps Wi-Fi during play; Casual follows the setting. */
#define settings_keep_wifi(s) ((s)->live || (s)->hardcore)

void settings_load(dsc_settings_t* s);
int settings_save(const dsc_settings_t* s);

/* NDS_PATH and DSI_MODE from nds-bootstrap.ini; returns 0 if NDS_PATH is missing. */
int bootstrap_ini_game(char* path, int path_size, int* dsi_mode);
/* DONOR20_NDS_PATH: the ROM whose ARM7 binary nds-bootstrap runs for one
 * build of Yoshi's Island DS. Returns 0 if it isn't set. */
int bootstrap_ini_donor20(char* path, int path_size);

#endif
