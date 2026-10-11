# DScheevos launcher

Starts each RetroAchievements session before the game boots. TWiLight Menu++ runs it in place of nds-bootstrap (it is installed as `_nds/nds-bootstrap-release.nds`). It:

1. Reads the game from `_nds/nds-bootstrap.ini`, which TWiLight Menu++ just wrote.
2. Hashes the ROM the way RetroAchievements does (rcheevos).
3. Connects with the Wi-Fi settings saved on the console. On a DSi this uses the DSi's own Wi-Fi, so WPA2 works.
4. Logs in with the saved token, loads the game's set and starts a session over HTTPS. mbedTLS checks the server's certificate chain and host name against the root CAs in `tls/cacerts.c`. The console clock isn't trusted, so certificate dates aren't checked.
5. Writes `_nds/dscheevos/session.bin` with the server's replies.
6. Boots the DScheevos build of nds-bootstrap (`_nds/dscheevos/nds-bootstrap.nds`). That build loads the module with the session. `source/chainload.c` does the boot through devkitPro's nds-bootloader (`_nds/dscheevos/load.bin`, built in `loader/`), the way TWiLight Menu++ and hbmenu boot programs.

If the game has no set, achievements are off, or the game will run in DSi mode, it boots the game without achievements. If it can't get online, it asks: try again, or play without achievements. Hardcore needs a session started online, so it doesn't offer "play now, send later".

## Screens

- **First run.** Log in with your RetroAchievements username and password, typed on the touch keyboard. Only the token the server returns is saved. The password is never stored.
- **Settings.** Hold SELECT while a game starts to change them:
  - turn achievements off or on
  - hardcore on or off
  - log out

## Files on the SD card

| File | What it is |
|---|---|
| `_nds/dscheevos/dscheevos.ini` | `ENABLED`, `HARDCORE`, `USERNAME`, `TOKEN`, and optionally `HOST` |
| `_nds/dscheevos/ca.pem` | optional extra trusted CA, for a self-hosted or test server |
| `_nds/dscheevos/session.bin` | written here, used once by nds-bootstrap (renamed to `session.used`) |
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
