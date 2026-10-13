# Real-server bench

`run.sh` runs the launcher flow in melonDS (DSi mode, `--net`) against retroachievements.org instead of the mock server: launcher, chainload, DScheevos nds-bootstrap, game, then the module's status from a RAM dump.

```sh
tests/real/run.sh --check                     # no account: proves DNS + TLS to the real server
RA_USER=name RA_PASSWORD=... tests/real/run.sh --expect 12345
```

- `--check` runs only the launcher with a made-up user and token. It passes when `_nds/dscheevos/launcher.log` shows the server's invalid-credentials answer to `login2` (HTTP 401). Another status, such as a Cloudflare 403 or a 5xx during maintenance, fails as "not a login answer". Needs `DSI_FILES` and `RUNNER`.
- A full run needs `DSI_FILES`, `ROM`, `BOOTSTRAP`, `RUNNER` (as `tests/launcher/run.sh`) and `RA_USER` with `RA_PASSWORD`, `RA_TOKEN` or `RA_TOKEN_FILE` (a file holding the token; it is never printed). With a password the script logs in from this machine with curl; the password goes to curl on stdin.
- Spectator mode by default (`SPECTATOR = 1`): the game evaluates and shows unlocks but nothing is sent. `LIVE_UNLOCKS=1` sends unlocks for real.
- Optional: `HARDCORE` (default 1), `FRAMES` (default 6000, 2400 for `--check`), `SCRIPT`, `WORK`, `RUNNER_ARGS`. Extra arguments go to `tests/bootstrap/status.py`. A `SCRIPT`'s frames count from the game's first `OS_Halt` (the runner's `--script-from-game`), so how long the launcher spends on the network doesn't shift the input.

The work directory holds the token in its `dscheevos.ini` and a 450-650 MB SD image. Without `WORK` it is a `mktemp -d` that the script deletes on exit; with `WORK` it is kept, so delete it when done.

## Bench run on the test account (owner)

Two runs on the test account with Tetris DS: a spectator run that sends nothing, then a live run that earns one 1-point achievement for real. `tetris-marathon.txt` starts Standard > Marathon and taps rotate on every piece, which earns "Yep, It Ain't Moving" (achievement 230051, "[Standard - Marathon] Attempt to rotate a Square Block") within the first pieces. Checked against the mock server with the real set (2026-10-12): it unlocked at game frame 972 of 4500, nothing else unlocked, no frames dropped.

Expected results are fixed here, before the runs. Until RetroAchievements validates DScheevos, the server adds its "Warning: Unknown Emulator" entry to the set and files hardcore unlocks as casual: that is the expected result, not a failure.

**0. Set up** (from the repo, with `perf` built: module, launcher, loader, nds-bootstrap):

```sh
source ../dscheevos.env
export ROM="$HOME/RomM/library/roms/nds/Tetris DS (USA, Australia).nds"
export RA_USER=$(cat ../private/ra-user) RA_TOKEN_FILE=../private/ra-token
```

**1. Connection check.** `~/Developer/DScheevos/bin/emu-lock -l real tests/real/run.sh --check`
- Pass: `RESULT PASS: retroachievements.org answered over TLS`.

**2. Spectator run** (sends nothing):

```sh
FRAMES=4500 SCRIPT=tests/real/tetris-marathon.txt ~/Developer/DScheevos/bin/emu-lock -l real tests/real/run.sh --expect 230051
```

- Pass, terminal: `RESULT PASS`. The status shows achievement 230051 unlocked, hardcore "rules on, RA counts as casual" (`'hardcore': 2`, the server's warning entry was seen), 0 sent.
- Pass, `launcher.log` (printed): every request answered with HTTP 200, and no `startsession` request (a spectator starts no session).
- Pass, website (the test account's profile and its Tetris DS page): nothing new. No unlock, no "last played" change from this run.

**3. Live run** (earns the achievement for real):

```sh
LIVE_UNLOCKS=1 FRAMES=4500 SCRIPT=tests/real/tetris-marathon.txt ~/Developer/DScheevos/bin/emu-lock -l real tests/real/run.sh --expect 230051 --expect-sent 1
```

- Pass, terminal: `RESULT PASS`, 1 unlock sent, the hardcore state as in step 2.
- Pass, website, test account:
  - Tetris DS is the most recently played game, and its rich presence (the line under the game, if the set has one) appeared during the run.
  - "Yep, It Ain't Moving" is unlocked, at the time of the run, **as casual** (softcore), not hardcore: the expected result for an unvalidated client.
  - No other achievement unlocked; "Warning: Unknown Emulator" is not counted as an unlock.
- Fail: any other achievement unlocked, a hardcore unlock (it would mean the server recognised a client it shouldn't yet), the unlock missing, or `RESULT FAIL`.

**4. Clean up.** Reset the Tetris DS progress on the website (game page: hover the completion percentage and reset, or Settings > reset game progress). Delete any `WORK` folder you kept: it holds the token.

**5. Record** the results in `docs/compliance.md` ("Real-server results"): date, build (`git describe`), each pass mark, and a screenshot of the unlock on the game page.
