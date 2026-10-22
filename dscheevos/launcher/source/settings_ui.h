/* LAB (strip-lab): the settings screen (settings_ui.c). */
#ifndef DSC_SETTINGS_UI_H
#define DSC_SETTINGS_UI_H

#include "config.h"

/* Shows the settings on the bottom screen until START or B; saves them.
 * Returns 1 if anything changed. */
int notice_settings_menu(dsc_settings_t* s);

#endif
