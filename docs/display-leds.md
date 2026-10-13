# DSi LEDs as achievement cues: design

Status: design, 2026-10-12. Facts with citations: `display-research/leds.md` (GBATEK line numbers). Owner ruling #5: every LED cue is a switch (`display-settings.md`, `LED_*`). The camera LED is off by default and the owner will revisit.

## What software can drive (facts)

All four registers below belong to the power-management chip (I2C device 4Ah: BPTWL on the DSi, UTL-1 on the DSi XL). Only the ARM7 can reach it. None is marked nonvolatile; only 40h (volume) and 41h (backlight) are.

| LED | Register | What it can do | Seen with the lid open? | 3DS (TWL mode) |
|---|---|---|---|---|
| Power LED | 63h "Force Power LED": 0 automatic, 1 red, 2 blue, 3 purple | **Colour.** No hardware blink or brightness | To confirm (test L1) | Not emulated |
| Wi-Fi LED | 30h bits 0-1: 0/2 off, 1 on, 3 blink on traffic. Bit 4 also enables the Wi-Fi chip | On/off; automatic traffic blink (rate undocumented) | To confirm (L2); DScheevos cues already use it | Not emulated |
| Camera LED | 31h: 0 off, 1 on, 2 blink | On/off/blink | **No:** outer lid, by the camera | Emulated on older 3DS only |
| 3DS notification LED (RGB) | 3DS-mode MCU 2Dh | Colour, patterns | — | No DSi-mode route documented |

Cost: one I2C register write is several bytes, each followed by `swiWaitByLoop(0x180)`. That is estimated at 0.1-0.2 ms of ARM7 time per write; L1/L2/H9 measure it. nds-bootstrap's I2C code retries 8 times and its busy-wait is unbounded. The cue code must bound both, and never run inside the game's IRQ (WORKLIST interrupt audit item 3).

## Design

1. **The power LED's colour as the primary cue, if L1 shows the player can see it.** It is the only LED that can say *which* thing happened without a pattern to learn:
   - **purple** while a challenge is active (RA's Trigger indicator), held for its whole duration;
   - **a short purple flash** (about 0.5 s) for an unlock: one write on, one write back to automatic;
   - **progress and leaderboard:** no colour. Blue is the LED's normal colour and red means low battery, so neither can carry a cue. These use the Wi-Fi LED patterns as today.

   Safety rules:
   - always write **0 (automatic)** to give the LED back; never leave it forced across a reset, the in-game menu or the return to the loader (the same points where the Wi-Fi LED is restored today);
   - **never override red:** read the battery level (BPTWL 20h) before forcing a colour, and stand down while it is low, so the system's low-battery red is never hidden;
   - one write per state change, from the ARM7 outside IRQ, bounded retries.
2. **The Wi-Fi LED keeps today's blink cues** (`LED_STYLE` flash/dark/off), now switched per event.
3. **The camera LED is off by default** (`LED_CAMERA = 0`): it faces away from the player. As an opt-in, it could tell people nearby (spectating) that a challenge is live; owner to revisit.
4. **3DS:** none of the DSi LED cues reach the player there, apart from the camera LED on older models. The 3DS answer is the TwlBg overlay (later).

## Hardware tests (in the lab ROM)

| Test | What | Pass |
|---|---|---|
| H9 | Camera LED on / blink / off, timed | Responds; write < 0.5 ms. Also on the New 3DS XL (expect no LED: GBATEK says New 3DS has none) |
| **L1** | Power LED purple 3 s, red 3 s, blue 3 s, then automatic, timed | **Owner notes on video which colours are visible while holding the DSi normally**; write < 0.5 ms; the LED returns to its normal colour after "auto" |
| **L2** | Wi-Fi LED on 3 s, off 3 s, back to traffic blink (low 2 bits only, bit 4 kept), timed | Visible; Wi-Fi chip still enabled afterwards (bit 4 unchanged, read back in the log by the next run's R5 line) |

The DSi XL uses a different chip (UTL-1) behind the same address. Running the ROM on both the DSi and the DSi XL tells whether 63h behaves the same on both.
