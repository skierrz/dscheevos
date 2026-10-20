# Hardware safety: can a DScheevos build brick the console?

Audit of what a build puts on the SD card (launcher, loader, nds-bootstrap fork, module) against the hard rule in
CLAUDE.md: nothing we ship may write the NAND, the firmware SPI flash or the Wi-Fi chip's EEPROM. Done 2026-10-20 on
`stack` 64784d7d (nds-bootstrap fork af351f09, BlocksDS 1.24.0). Redo it before each hardware build that changes
the launcher, the nds-bootstrap fork, the module's network code or the libraries.

## Result

| Permanent storage | What we do | How it was checked |
|---|---|---|
| NAND (eMMC) | Nothing | No `nand:` path, `nandInit` call or `nand_WriteProtect` call in our sources. BlocksDS mounts NAND only through `nandInit` and keeps it write-protected by default (`nds/arm9/sdmmc.h`). The NAND driver functions are linked into the launcher only through the library's disk table; nothing of ours calls them. Our nds-bootstrap changes touch no NAND path. |
| Firmware SPI flash | Read only | `readFirmware` (the saved access point's PMK, into the session in RAM, never to SD). No `writeFirmware` in the launcher's ARM9 code, so the ARM7's libnds `firmwareMsgHandler` never gets a write request. |
| Wi-Fi chip (Atheros) EEPROM/OTP | Nothing | The module sends four WMI commands only: CONNECT (0x0001), DISCONNECT (0x0003), SYNCHRONIZE (0x0004), ADD_CIPHER_KEY (0x0016), all firmware-RAM state. No BMI access. The launcher's DSWiFi loads the chip's firmware into its RAM (`ath6k_bmi_start_firmware`), as the DSi system does at every boot. |
| Power chip (BPTWL, I2C 0x4A) | Volatile register only | Register 0x30 (Wi-Fi LED) for unlock cues: read once, LED bits set, restored. Resets at power-off. |
| GPIO_WIFI (0x04004C04) | Volatile register only | Bit 8 selects DS-mode or DSi Wi-Fi; resets at power-off. |
| SD card | Our folder only | Launcher: `sd:/_nds/dscheevos/` (dscheevos.ini, launcher.log, session.bin, pending.bin, unsent/, pending.spectator). nds-bootstrap fork: session.bin -> session.used and pending.bin in the same folder (journal only at safe points), plus nds-bootstrap's own stock files. |

nds-bootstrap's storage drivers carry two DScheevos fixes, both making SD access safer: `my_fat.c` re-reads a partly
written first sector before merging (no stale bytes written back), and `my_sdmmc.c` no longer hangs when an IRQ delays
the command-busy check. Neither changes which device or sector is written.

## What can still go wrong on hardware, and the recovery

- A freeze or crash in a game: hold the power button. Nothing persistent is touched.
- A power loss while a file in `sd:/_nds/dscheevos/` is being written: that file may be damaged; delete the folder's
  files on a PC (the launcher recreates them; unsent unlocks in pending.bin could be lost).
- An SD card filesystem error: the usual risk of any homebrew that writes the SD card; keep a copy of the card.

## Before the first hardware run

- A NAND backup off the console (standard practice, independent of this software).
- Unlaunch installed (boots from SD even if the system menu won't).
- A copy of the SD card on the PC.
- Start DScheevos from TWiLight Menu++ as usual; it installs nothing to NAND.

## Re-checking

```
# our sources: no NAND paths, no firmware writes, no EEPROM/BMI access
git grep -nIiE "nand:/|nandInit|nand_WriteProtect|writeFirmware|eeprom|BMI_" -- dscheevos
# what the launcher links (ARM9 must not contain writeFirmware)
arm-none-eabi-nm -C dscheevos/launcher/build/dscheevos-launcher.elf | grep -iE "writeFirmware|nandInit"
# the module's Wi-Fi commands
grep -n "wmi_cmd(" dscheevos/module/src/net/link.c
```
