# Real-server bench

`run.sh` runs the launcher flow in melonDS (DSi mode, `--net`) against retroachievements.org instead of the mock server: launcher, chainload, DScheevos nds-bootstrap, game, then the module's status from a RAM dump.

```sh
tests/real/run.sh --check                     # no account: proves DNS + TLS to the real server
RA_USER=name RA_PASSWORD=... tests/real/run.sh --expect 12345
```

- `--check` runs only the launcher with a made-up user and token. It passes when `_nds/dscheevos/launcher.log` shows the server's invalid-credentials answer to `login2` (HTTP 401). Another status, such as a Cloudflare 403 or a 5xx during maintenance, fails as "not a login answer". Needs `DSI_FILES` and `RUNNER`.
- A full run needs `DSI_FILES`, `ROM`, `BOOTSTRAP`, `RUNNER` (as `tests/launcher/run.sh`) and `RA_USER` with `RA_PASSWORD` or `RA_TOKEN`. The script logs in from this machine with curl; the password goes to curl on stdin.
- Spectator mode by default (`SPECTATOR = 1`): the game evaluates and shows unlocks but nothing is sent. `LIVE_UNLOCKS=1` sends unlocks for real.
- Optional: `HARDCORE` (default 1), `FRAMES` (default 6000, 2400 for `--check`), `SCRIPT`, `WORK`, `RUNNER_ARGS`. Extra arguments go to `tests/bootstrap/status.py`.

The work directory (a `mktemp -d` unless `WORK` is set) holds the token in its `dscheevos.ini` and a 450-650 MB SD image. Delete it when done.
