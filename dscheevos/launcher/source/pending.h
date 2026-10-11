/* Unlocks and leaderboard entries the game couldn't send: the module's
 * journal, which nds-bootstrap saves to sd:/_nds/dscheevos/pending.bin at
 * safe points. The launcher sends them at its next start, online. */
#ifndef DSC_PENDING_H
#define DSC_PENDING_H

#include "config.h"

typedef struct pending_result_t {
  unsigned sent_unlocks;
  unsigned sent_entries;    /* leaderboard entries */
  unsigned failed;          /* kept for next time */
  unsigned dropped;         /* entries that couldn't be sent at all */
  unsigned this_game;       /* unlocks sent for the game being started */
} pending_result_t;

/* Logged in and online: sends what's waiting for this user (nothing when
 * spectating) and clears pending.bin. hash is the game being started. Entries that fail are kept in
 * _nds/dscheevos/unsent/, since the next session overwrites pending.bin. */
void pending_send(const dsc_settings_t* settings, const char* hash, pending_result_t* out);

/* Before booting with achievements: pending.bin exists, zeroed, at
 * DSCM_JOURNAL_FILE_SIZE bytes (nds-bootstrap only writes into it). */
int pending_prepare(const dsc_settings_t* settings);

#endif
