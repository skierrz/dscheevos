/* Prints dscm_boot_t offsets so mksession.py can check its layout. */
#include <stddef.h>
#include <stdio.h>
#include "dscheevos_module.h"
int main(void)
{
  printf("size=%zu rom_path=%zu host=%zu username=%zu token=%zu game_hash=%zu hardcore=%zu dtcm_base=%zu unix_time=%zu preload_offset=%zu preload_length=%zu ap_ssid=%zu pmk_valid=%zu pmk=%zu\n",
    sizeof(dscm_boot_t), offsetof(dscm_boot_t, rom_path), offsetof(dscm_boot_t, host), offsetof(dscm_boot_t, username),
    offsetof(dscm_boot_t, token), offsetof(dscm_boot_t, game_hash), offsetof(dscm_boot_t, hardcore),
    offsetof(dscm_boot_t, dtcm_base), offsetof(dscm_boot_t, unix_time), offsetof(dscm_boot_t, preload_offset),
    offsetof(dscm_boot_t, preload_length), offsetof(dscm_boot_t, ap_ssid), offsetof(dscm_boot_t, pmk_valid),
    offsetof(dscm_boot_t, pmk));
  return 0;
}
