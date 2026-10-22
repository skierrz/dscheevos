/* LAB (strip-lab): the set's 16x16 badges for the notice bar (badges.c). */
#ifndef DSC_BADGES_H
#define DSC_BADGES_H

#include <stdint.h>
#include "config.h"

/* After session_start, before session_write: cached and downloaded badges
 * go into the session file. Shows progress on the bottom screen; B skips. */
void badges_prepare(const dsc_settings_t* settings, uint32_t game_id);

#endif
