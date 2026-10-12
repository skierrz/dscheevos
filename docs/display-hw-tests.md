# Display hardware tests (DSi XL): the lab ROM

One standalone homebrew ROM, `tests/display-lab` → `display-lab.nds`, runs every DSi-only question the strip design (`docs/display-library.md`, P2) depends on.

- **No game, no nds-bootstrap and no DScheevos module are involved.** Engine A draws a scrolling stand-in "game" pattern on the top screen; the log is on the bottom screen.
- **Each test's pass mark is fixed here, before any run.**
- **Results:** the ROM writes `sd:/dscheevos-lab/log.txt`, and the owner films both screens with the phone at 60 fps or more (240 fps if available) for the whole run. Results go in TESTING.md.
- **Writes:** SD only (the log), and only between tests. Nothing writes NAND, the firmware or the Wi-Fi EEPROM. The camera LED test writes one MCU (BPTWL) register, 31h, which isn't stored permanently, and sets it back to 0 at the end.

## How to run

1. Copy `display-lab.nds` to the SD card (for example `sd:/_nds/display-lab.nds`).
2. Start it from TWiLight Menu++ in **DSi mode**. NDMA and SCFG_EXT9 need DSi mode; the ROM refuses to run in DS mode and says so.
3. Start filming. Press A. The tests run by themselves (about 6 minutes); each one names itself on the bottom screen first.
4. At the end the bottom screen says `DONE, log written`. Power off, put the SD card in the Mac, and copy `sd:/dscheevos-lab/log.txt` into the session.

The ROM also runs in our melonDS fork (`--mode dsi`) as a smoke test. melonDS can't answer these questions, though: it doesn't model NDMA intervals, SCFG bit 8, bus contention or mid-line latching.

## Tests and pass marks

| Test | Question | How | Pass |
|---|---|---|---|
| **H1** | Does NDMA startup 09h feed the main-memory display FIFO? | Strip of 32 lines from main RAM, mode 3 switched in at VBlank (the old way), 10 s | **Video:** the banner shows on lines 0-31 and the game pattern from line 32-36 down. **Log:** `H1 fifo_ndma_done=600/600` (the FIFO channel finished every frame) |
| **H2a** | One-shot delay: NDMA started *immediately* from the VBlank IRQ, 2 words, 1-word blocks, BCNT interval | 1000 frames each at 3 delays (prescaler 1: 20, 40 and 69 lines' worth) | IRQ VCOUNT within ±1 line of target in ≥ 99.9% of 1000 frames, for each delay; the measured delay matches BCNT within 1% |
| **H2b** | Same with startup 06h (V-Blank) and repeat-infinitely: does the IRQ fire every frame without re-arming? | 1000 frames, delay to line 261 | IRQ every frame (1000/1000) at line 261 ±1. **Record** whether the IRQ is per logical block (no pass/fail on the mechanism; it picks P2's arming) |
| **H3** | Clean mid-frame switch with a byte write (`STRB` to DISPCNT+2) | New timing: entry at line ~261 (H2), exit from an NDMA3 H-Blank counter IRQ (TCNT = 32), 10 s | **Video:** a clean edge at line 32, no torn or flickering line in 600 frames. **Log:** DISPCNT bytes 0, 1 and 3 never changed by the switch (`H3 byte_clobber=0`) |
| **H4** | Is VRAMCNT A-I readable? | Write test values to each bank control, read back 8-bit, restore | `H4 vramcnt readable=9/9` → readable; otherwise GBATEK's "write-only" holds |
| **H5** | Bus cost of the strip (FIFO source in main RAM) | Count iterations of an uncached main-RAM read loop per frame, strip off vs on, 300 frames each | **Record** the percentage. **Pass** (owner's threshold, proposed): ≤ 3.5% of a frame |
| **H6** | Starvation: does a card-like NDMA (128-word blocks) or a legacy DMA3 burst corrupt the strip? | Strip on (FIFO on NDMA0), with (a) NDMA1 128-word block copies in a loop, (b) DMA3 4 KB copies in a loop, 10 s each | (b) **Pass:** no banner corruption on video. (a) Corruption is *expected*, which confirms the "no strip during card reads" rule; record what it looks like |
| **H6b** | IRQ latency: how late is the exit switch when the "game" masks IRQs? | New timing; the stand-in game disables IRQs for 0-2 lines at random times, 10,000 frames | **Record** the distribution of exit lateness. **Pass:** the 99.99th percentile + 1 line ≤ 4 lines → that is margin M |
| **H8** | Arming at line 261 starts the banner at line 0 | Same run as H3 | **Video:** the banner starts at line 0 in every frame. **Log:** `H8 late_entry=0` (entry IRQ outside lines 259-262) |
| **R5** | SCFG_EXT9 bit 8 (extended ARM9 IRQs) | Read SCFG_EXT9; check whether IE bits 28-31 stick with bit 8 set and clear (if SCFG is unlocked in this mode) | **Record** the values and whether the bits stick |
| **H9** | Camera LED (BPTWL 31h) from the ARM7 | ARM7 writes 31h = 01h (3 s), 02h (5 s), 00h; times each I2C write | **Video:** the LED is on, then blinks, then off. **Log:** every write < 0.5 ms. Repeat on the New 3DS XL, where the LED should respond too. Wi-Fi association and audio aren't part of this ROM; they need the real chain (H7) |

Not in this ROM:
- **H7** (the real nds-bootstrap chain with games, 30 min each) comes after the owner's rulings and the M1 survey.
- **H5 with an NWRAM framebuffer:** NWRAM isn't available to the display (memory plan).

## Reading the results

| Outcome | What it means for P2 (the top strip) |
|---|---|
| H1 fails | 09h isn't the FIFO feeder; P2 is dead (P2's VRAM-display fallback needs a free bank: allow list only) |
| H2a and H2b both fail | No private timer; P2 would fall back to the game-owned VCOUNT IRQ (variant 4c), which has hardware evidence against it |
| H3 fails | A byte write doesn't switch cleanly; P2 would need a different switch method |
| H6b gives M > 4 | Late exits happen too often; P2 needs a taller margin or a per-game stand-down |
| H5 > threshold | Notices only when the idle-slack gate allows a larger cost, or P2 is out |

## Build and smoke test

```sh
docker run --rm --platform linux/amd64 -v "$PWD:/src" -w /src devkitpro/devkitarm:20241104 \
  make -C tests/display-lab           # → tests/display-lab/out/display-lab.nds
```

The smoke run in melonDS DSi mode (2026-10-12) completed and wrote its log to the emulated SD. The banner showed on lines 0-31 over the stand-in game. melonDS answers none of the questions: NDMA intervals aren't modelled, so H2's IRQ lands at once (line 192) and the ROM correctly counts every new-timing entry as late. melonDS's DMA stalls are worst-case (H5 showed a 52% loss in the strip lines), and its I2C isn't real (H9). It only shows the ROM runs, logs and draws.
