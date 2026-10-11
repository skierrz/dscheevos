# What a DS game under nds-bootstrap can reach

How much of the DSi's hardware is still usable while nds-bootstrap runs a DS game. Source references are to nds-bootstrap at `b9044ff` (`retail/`).

## Not true DS mode

nds-bootstrap runs a normal DS game in a hybrid mode on a DSi, not true DS mode.

**ARM9** (`bootloaderi/source/arm9/main.arm9.c:388-395`):
- SCFG_EXT9 ends up as `0x0301C000`: 16 MB of RAM (`0x02000000-0x02FFFFFF`, mirrored at `0x0C000000`) and NDMA on, then SCFG locked.
- The game's MPU region 1 is widened to cover all of it (`patch_arm9.c:1062-1079`).
- The game itself uses about 4 MB. nds-bootstrap uses the rest as ROM cache.

**ARM7:**
- Keeps the feature bits the launcher enabled, typically `0x93FFFB06` under TWiLight Menu with Unlaunch or hiyaCFW: NDMA, AES, SD/MMC, the DSi Wi-Fi interface (SDIO), I2C and GPIO.
- The only change is clearing bit 31, which removes access to the SCFG/MBK registers but leaves the features on (`main.arm7.c:2612-2613`).
- nds-bootstrap's own ARM7 card engine reads the SD card through SD/MMC and NDMA during play.

**Clock:** fixed at boot. 133 MHz only if `BOOST_CPU` is set, and some games misbehave at 133 MHz (`twlClockExcludeMap.h`). DScheevos can't switch to 133 MHz just for its own work.

## What that means for DScheevos

| Need | DSi | 3DS (TWL mode) |
|---|---|---|
| ARM9 reads game RAM at VBlank | Yes | Yes |
| Module in RAM above 4 MB | Yes, by shrinking nds-bootstrap's ROM cache | Yes, more room (32 MB) |
| SD card | Yes (ARM7) | Yes |
| Open/WEP Wi-Fi | Yes, unless the game uses wireless | Yes |
| WPA2 Wi-Fi (DSi Atheros chip) | Possible: needs `GPIO_WIFI` bit 8 cleared and an ARM7 driver; not while the game uses wireless | Probable, not confirmed |
| 133 MHz for our code only | No | No |

## Wi-Fi and local wireless

- **Mode switch:** nds-bootstrap sets `GPIO_WIFI` bit 8 ("old DS Wi-Fi mode") for DS games (`main.arm7.c:2242-2243`). On the common DSi Wi-Fi board (DWM-W024), the old DS Wi-Fi and the Atheros chip can't be used at the same time; bit 8 selects which one works.
- **When we can't use the network:** while a game uses local wireless (Download Play, multiplayer, Nintendo WFC), DScheevos must stay off the network.
- **Detecting game wireless use:** nds-bootstrap already detects it by polling `W_IF` (`cardengine.c:2074-2089`).

## Homebrew

Homebrew goes through nds-bootstrap's `hb` bootloader:
- It runs in DS mode by default: 4 MB RAM limit and no NDMA on the ARM9.
- It leaves no card engine resident, so the retail hook points don't exist there.
- Retail nds-bootstrap doesn't run homebrew: its patcher looks for Nintendo SDK code.

## Emulator note

The headless melonDS runner needs two things to run nds-bootstrap like TWiLight Menu does:
- the SD-access fix in the melonDS fork (header `0x1B8`);
- `--arg` to pass nds-bootstrap its own path.
