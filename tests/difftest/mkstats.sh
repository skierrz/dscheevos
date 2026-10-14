#!/bin/sh
# mkstats.sh TREE OUTDIR: copy of an rcheevos tree with condition counters (measurement only)
set -e
rm -rf "$2"; mkdir -p "$2"
cp -R "$1/src" "$1/include" "$2/"
f="$2/src/runtime/rc_condset.c"
perl -0pi -e 's/(  for \(; condition < condition_end; \+\+condition\) \{\n)/extern unsigned long g_st_eval;\n$1    ++g_st_eval;\n/' "$f"
grep -q "++g_st_eval" "$f"
f="$2/src/runtime/rc_condition.c"
perl -0pi -e 's/(int rc_test_condition\(rc_condition_t\* self, rc_eval_state_t\* eval_state\) \{\n)/extern unsigned long g_st_test;\n$1  ++g_st_test;\n/' "$f"
grep -q "++g_st_test" "$f"
f="$2/src/client/rc_client.c"
perl -0pi -e 's/(    client->game->progress_tracker.progress = 0.0;\n)/$1    g_st_ach0 = g_st_eval;\n/; s/(    if \(client->game->pending_events & RC_CLIENT_GAME_PENDING_EVENT_PROGRESS_TRACKER\)\n      rc_client_do_frame_update_progress_tracker)/    g_st_ach += g_st_eval - g_st_ach0;\n$1/; s/\A/extern unsigned long g_st_eval, g_st_ach, g_st_ach0, g_st_walk;\n/' "$f"
grep -q "g_st_ach += " "$f"
# leaderboards: around the leaderboard loop of rc_client_do_frame
perl -0pi -e 's/(    if \(client->state.hardcore \|\| client->state.allow_leaderboards_in_casual\) \{\n)/    g_st_lb0 = g_st_eval;\n$1/; s/(    richpresence = client->game->runtime.richpresence;\n    if \(richpresence)/    g_st_lb += g_st_eval - g_st_lb0;\n$1/; s/\A/extern unsigned long g_st_lb, g_st_lb0;\n/' "$f"
grep -q "g_st_lb += " "$f"
if grep -q "rc_client_cd_snapshot" "$f"; then
perl -0pi -e 's/(    for \(; count; --count, \+\+condition, \+\+hits, \+\+flags(?:, \+\+bit)?\) \{\n)/$1      ++g_st_walk;\n/' "$f"
grep -q "++g_st_walk" "$f"
fi
