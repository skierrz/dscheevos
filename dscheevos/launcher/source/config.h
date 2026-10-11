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
  char username[32];
  char token[64];       /* from the server at login; the password is never stored */
  char host[64];
} dsc_settings_t;

/* Hardcore always keeps Wi-Fi during play; softcore follows the setting. */
#define settings_keep_wifi(s) ((s)->live || (s)->hardcore)

void settings_load(dsc_settings_t* s);
int settings_save(const dsc_settings_t* s);

/* NDS_PATH and DSI_MODE from nds-bootstrap.ini; returns 0 if NDS_PATH is missing. */
int bootstrap_ini_game(char* path, int path_size, int* dsi_mode);

#endif
