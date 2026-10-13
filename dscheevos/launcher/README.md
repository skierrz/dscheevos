# DScheevos launcher

Starts each RetroAchievements session before the game boots. TWiLight Menu++ runs it in place of nds-bootstrap (it is installed as `_nds/nds-bootstrap-release.nds`). It:

1. Reads the game from `_nds/nds-bootstrap.ini`, which TWiLight Menu++ just wrote.
2. Hashes the ROM the way RetroAchievements does (rcheevos).
3. Connects with the Wi-Fi settings saved on the console. On a DSi this uses the DSi's own Wi-Fi, so WPA2 works.
4. Logs in with the saved token, loads the game's set and starts a session over HTTPS. mbedTLS checks the server's certificate chain and host name against the root CAs in `tls/cacerts.c`. The console clock isn't trusted, so certificate dates aren't checked.
5. Sends what the last session couldn't (`_nds/dscheevos/pending.bin`, see below), then writes `_nds/dscheevos/session.bin` with the server's replies.
6. Freezes its Wi-Fi driver on the ARM7 without disconnecting (`arm7/`), so the chip stays associated and the game can keep sending unlocks over the same connection. Then it boots the DScheevos build of nds-bootstrap (`_nds/dscheevos/nds-bootstrap.nds`). That build loads the module with the session. `source/chainload.c` does the boot through devkitPro's nds-bootloader (`_nds/dscheevos/load.bin`, built in `loader/`), the way TWiLight Menu++ and hbmenu boot programs.

In hardcore, nds-bootstrap uses only its built-in AP-fix patches and ignores user files in `_nds/nds-bootstrap/apFix/` (they could patch the game after the launcher hashed it). If such a file exists for the game and nds-bootstrap has no built-in fix for it, hardcore isn't offered: the launcher asks to play in Casual (with the file) or without achievements.
The same goes for the one build of Yoshi's Island DS that nds-bootstrap runs with a donor ROM's ARM7 binary (`DONOR20_NDS_PATH`): hardcore doesn't load the donor, so while one is set that game isn't offered hardcore.

If the game has no set, achievements are off, or the game will run in DSi mode, it boots the game without achievements. If it can't get online, it asks: try again, or play without achievements. Hardcore needs a session started online, so it doesn't offer "play now, send later".

## Screens

- **First run.** Log in with your RetroAchievements username and password, typed on the touch keyboard. Only the token the server returns is saved. The password is never stored.
- **Settings.** Press A on the game in TWiLight Menu++, then hold SELECT, to change them:
  - turn achievements off or on
  - hardcore on or off (off is Casual, RetroAchievements' name for softcore)
  - send unlocks during play (Casual only: hardcore always keeps Wi-Fi, so it shows "on (hardcore)")
  - log out

  `SPECTATOR = 1` in `dscheevos.ini` (not in the menu) turns on rcheevos' spectator mode: the game evaluates and shows unlocks, nothing is sent, and no session starts on the server. The launcher shows "Spectator: nothing is sent". For bench runs on a real account.

## Unlocks sent later

During play the module keeps a journal of every unlock and leaderboard entry and whether the server answered it. nds-bootstrap saves it to `pending.bin` at safe points (in-game menu, reset, exit). At its next start, once logged in, the launcher sends this user's unsent entries with rcheevos' request builders and the time since they happened (`o=`), then zeroes the file. An answer that a retry won't change (e.g. already unlocked) counts as sent. Entries that still fail go to `unsent/` and are tried again next time, since the coming session overwrites `pending.bin`. A spectator session's journal is never sent.

## Files on the SD card

| File | What it is |
|---|---|
| `_nds/dscheevos/dscheevos.ini` | `ENABLED`, `HARDCORE`, `SEND_DURING_PLAY`, `USERNAME`, `TOKEN`, and optionally `HOST` and `SPECTATOR` |
| `_nds/dscheevos/ca.pem` | optional extra trusted CA, for a self-hosted or test server |
| `_nds/dscheevos/session.bin` | written here, used once by nds-bootstrap (renamed to `session.used`) |
| `_nds/dscheevos/pending.bin` | the last session's journal, 4096 bytes; created zeroed here, written by nds-bootstrap |
| `_nds/dscheevos/unsent/` | journals with entries the server didn't take yet |
| `_nds/dscheevos/pending.spectator` | the last session was a spectator one: its journal is dropped |
| `_nds/dscheevos/launcher.log` | the last run: which requests went out, HTTP status, errors shown. No username, password or token |
| `_nds/dscheevos/load.bin` | devkitPro's nds-bootloader (GPLv2+, source in `external/nds-bootloader`) |
| `_nds/dscheevos/nds-bootstrap.nds` | the DScheevos build of nds-bootstrap |

## Building

```sh
git submodule update --init external/rcheevos external/mbedtls external/nds-bootloader
docker run --rm -v $PWD:/repo -w /repo/dscheevos/launcher skylyrac/blocksds:slim-v1.24.0 make
docker run --rm -v $PWD:/repo -w /repo devkitpro/devkitarm:20260610 make -C dscheevos/launcher/loader   # load.bin
```

The `rcheevos/` and `tls/mbedtls_*.c` files only include the unmodified submodule sources. `tls/dsc_mbedtls_config.h` picks what mbedTLS builds: a TLS 1.2 client with ECDHE, AES-GCM and ChaCha20-Poly1305. `tools/cacerts/gen.py` regenerates `tls/cacerts.c`.

## Testing in the emulator

The mock server can serve HTTPS (`--tls-cert`, `--tls-key`). Put its CA in `ca.pem` on the emulated SD and set `HOST = https://10.64.0.1:<port>`. Then run the launcher with `melonds-headless --mode dsi --net`.
