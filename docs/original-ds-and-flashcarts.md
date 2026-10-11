# Original DS, DSpico and other flashcarts

Could DScheevos run on an original DS or DS Lite through a flashcart, with Wi-Fi, hardcore, and no compromises in gameplay? Short answer: not without compromises. The DSpico is the most promising route. Figures marked *est.* are estimates, not measurements.

## Why the original DS is hard

| | DSi via nds-bootstrap | Original DS |
|---|---|---|
| RAM for DScheevos | 12 MB above the game's 4 MB (shared with nds-bootstrap's ROM cache) | None. Games can use all 4 MB |
| CPU | 67 MHz (133 MHz optional) | 67 MHz |
| rcheevos cost, typical set | 10.6 ms/frame at 67 MHz (63% of a frame) | Same: too much for many games |
| Wi-Fi | WPA2 possible (Atheros chip) | Open/WEP only; the driver runs on the ARM7, which belongs to the game |

- **Memory:** flashcart kernels survive by squeezing small patches into gaps in each game, which is why they have compatibility lists.
- **Slot-2 RAM:** a GBA-slot RAM pack (8–32 MB, DS and DS Lite only) fixes memory size but not speed: a 16-bit bus with wait states. It also blocks games that use slot-2 accessories.

## DSpico

An open-source DS flashcart (LNH-team):
- **Hardware:** an RP2040 microcontroller (two 133 MHz Cortex-M0+ cores, 264 KB RAM), microSD, USB, and a small dev port.
- **Software:** firmware, loader (Pico Loader) and launcher are open source.

**The interesting part: the cart has its own CPU.** Instead of running rcheevos on the DS:
1. A tiny DS-side shim (~1–2 KB, *est.*) sends the few hundred bytes the achievement set needs to the cart each frame, over the card bus (~80 µs per 512 bytes, *est.*).
2. The RP2040 runs rcheevos on its second core.
3. USB to a phone or PC app carries the HTTPS traffic to RetroAchievements.

**Limits:**
- **Set size:** the RP2040's 264 KB fits a typical set (~135 KB) but not a heavy one (~600 KB). An RP2350-based respin (520 KB plus PSRAM) would.
- **Speed:** rcheevos speed on an M0+ is unmeasured. Benchmarking it with our test sets is the cheapest next step and decides this whole route.
- **Pointer chains:** the cart can't see the DS's RAM, so the DS shim must collect pointer-chain targets itself.
- **Bus time isn't free:** card-bus traffic has to fit around the game's own ROM reads.
- **No network or display:** there's no Wi-Fi on the cart. Live unlocks need the USB tether, and notifications would go to the cart's LEDs or the companion app.

## Other flashcarts

- **Most carts (R4 clones, Acekard, DSTT, SuperCard, M3, EZ-Flash Parallel, Ace3DS X):** fixed-function chips, no programmable CPU. Everything would run on the DS itself, and the kernels are mostly closed. Pico Loader and nds-bootstrap's B4DS mode are the open hooking layers that support them.
- **SuperCard DSTWO:** has a 360 MHz MIPS CPU and 32 MB RAM on the cart, but its kernel is closed and it's long out of production.

## RetroAchievements rules

From RA's [hardcore compliance requirements](https://docs.retroachievements.org/general/hardcore-compliance-requirements.html):
- Correct triggers, measured indicators, rich presence and leaderboards.
- Offline unlocks must be cached securely and synced.
- No cheats, save states, rewind, slowdown or memory tools in hardcore.
- A unique user agent.
- Six months of public availability before applying for approval.

Native-hardware clients aren't mentioned, but there's precedent: a MiSTer FPGA fork got hardcore approval for NES in 2026. Approval is decided case by case by RA staff.

## Verdict

| Target | No compromises | With stated compromises |
|---|---|---|
| DSi / 3DS via nds-bootstrap | Closest. CPU budget and in-game Wi-Fi are the open problems | Per-game compatibility list; Wi-Fi only when the game isn't using wireless |
| DS / DS Lite + DSpico | No | Most promising original-DS path: rcheevos on the cart CPU, USB relay for HTTPS, LED or phone notifications |
| DS / DS Lite + other carts | No | Casual tech demo for light games at most |

Sources: [DSpico hardware](https://github.com/LNH-team/dspico-hardware), [DSpico firmware](https://github.com/LNH-team/dspico-firmware), [Pico Loader](https://github.com/LNH-team/pico-loader), [GBATEK](https://problemkaputt.de/gbatek.htm), [BearSSL speed](https://bearssl.org/speed.html), [RA FAQ](https://docs.retroachievements.org/FAQ/).
