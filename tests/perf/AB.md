# Stock vs DScheevos A/B in melonDS

`ab.sh` measures what DScheevos costs a real game: the same game and input run twice through the launcher flow (`tests/launcher/run.sh`), with achievements off (`ENABLED = 0` in `dscheevos.ini`) and on. Everything else is the same: the same launcher, the same nds-bootstrap build, the same SD card. With achievements off the launcher starts nds-bootstrap without a session, so nds-bootstrap doesn't load the module, doesn't patch `OS_Halt` and doesn't switch on its DScheevos hooks. That's "stock" here: nds-bootstrap's own hooks are still there in both runs.

```sh
source ../dscheevos.env
tests/perf/ab.sh kirby 6000                    # ~6000 game frames, no input
tests/perf/ab.sh pokemon 6000 my-input.txt     # with a runner input script
SET=tests/bootstrap/sets/live.json tests/perf/ab.sh kirby 6000
```

`GAME` is `kirby`, `pokemon`, `nsmb` (from the env file) or a ROM file. `FRAMES` counts VBlank intervals from the game's first `OS_Halt` call, so both runs measure the same stretch of the game even though the launcher takes longer to start it with achievements on (Wi-Fi, session). Input script frames count from that point too (`--script-from-game`).

The default set is the synthetic typical set (`tools/rcheevos-footprint/harness/gen_set.py 60 8 1 3`: 60 achievements, 8 leaderboards, rich presence), the one the qemu numbers in `docs/rcheevos-arm9-footprint.md` use. Its addresses are random, so it reads random game memory; it measures cost, not unlocks. `live.json` reads no memory at all, so it shows the fixed cost (network, evaluation loop) with an empty snapshot.

Optional: `CACHE_MODEL=0` (melonDS's flat cache timing), `MODULE=dir` (test another module build, e.g. one made from a clean checkout while the tree has work in progress), `KEEP=1` (keep the work folders), `BOOT_FRAMES` (frames allowed before the game starts, default 6000). Each run takes 450–650 MB, deleted at the end unless `KEEP=1`.

## What the runner measures (`--profile`)

The headless runner (`tools/melonDS`, branch `dscheevos-perf`) counts, for every instruction the ARM9 interpreter runs, the cycles melonDS charges for it (`ARMv5::ProfileInstr` in `src/ARM.cpp`). Per VBlank interval (from the start of one VBlank, line 192, to the next) it adds up:

- **module VBlank call**: PC inside the module (`DSCM_BASE` 0x02D80000 up to the image size from the module header; see `dscheevos/module/module.ld`) while it runs the VBlank call: `vblank_entry` → `dsc_vblank_queue` (and, for a session's first frame, `dsc_vblank_copy` on the module's IRQ stack), called from nds-bootstrap's ARM9 IPC handler inside the game's interrupt. Told apart from idle work by the PC (from `vblank_entry` up to `halt_entry`, which takes in `dsc_vblank_queue`) or the stack pointer (the module's own IRQ stack, which `vblank_entry` switches to), not by the CPU mode. The game is frozen for all of it.
- **module idle work**: PC inside the module the rest of the time: the idle work behind `halt_entry` (evaluation, network). This runs only when the game has nothing to do.
- **module in IRQ mode**: module cycles with CPSR in IRQ mode, whatever the context. On Kirby and Pokemon it equals the VBlank call: the call runs in IRQ mode.
- **ARM9 busy**: all executed cycles, module or not. 100% means the ARM9 never halted.

Not counted as module time: nds-bootstrap's own code around the call (its IPC handler, about 750–880 cycles a frame), and IRQ entry. Counting starts at the first VBlank after the game's first `OS_Halt` call, so the session load before the game is printed separately ("module before it").

Cycles are printed at the ARM9 clock in use (67 MHz for DS games here) and as a share of a frame (560,190 × 2 cycles at 67 MHz).

**Cache model.** `ab.sh` turns melonDS's cache model on (`--cache-model-from-game`: hits cost 1 cycle, misses a line fill; see `tools/melonDS/tools/dscheevos/README.md`), because the snapshot reads hundreds of separate lines and the flat cost would hide that. It's switched on at the game's first `OS_Halt` call, starting from empty caches: with the model on from boot, nds-bootstrap's start hangs (the ARM9 waits in the BIOS and the game never starts; not investigated). `CACHE_MODEL=0` leaves it off.

## Lag frames

A **lag interval** is a VBlank interval in which the game never reached idle: from one VBlank to the next, the game's threads never all ran out of work. Games on the NitroSDK do their frame, then the idle thread calls `OS_Halt` until an interrupt. A game that doesn't get its frame done before the next VBlank misses that VBlank, and the interval shows no idle.

What counts as "reached idle", outside IRQ mode:

- **Achievements off:** the ARM9 halts (the SDK's `OS_Halt`, or a BIOS wait) outside the module range. After every interrupt that doesn't wake a game thread, the idle thread loops back and halts again.
- **Achievements on:** nds-bootstrap patches `OS_Halt` to jump to the module's `halt_entry` (the module header holds its address at 0x14; nds-bootstrap logs "DScheevos: OS_Halt at ..." with `LOGGING = 1`). The idle thread then runs our idle work instead of halting. Reaching idle is *arriving* at module code: jumping in from the game (the `OS_Halt` call, or the game's scheduler switching back to a preempted idle thread) or returning into it from an interrupt. That last case matches the stock idle thread halting again after an interrupt. Halts outside the module (BIOS waits) count too, as in the stock run.

So our own idle work is never game lag: as soon as the game hands over to the idle thread, the interval has its idle. Only work that delays the game itself shows: the snapshot inside the interrupt, interrupts held off, bus and cache effects on the game's own code.

It's an arrival, not a state: an instruction of our idle work that runs between the VBlank and the interrupt being taken doesn't count, so a frame that overruns the VBlank by a little is still a lag interval.

Some games run at 30 fps (or slower in heavy scenes): they idle only every other VBlank and show 50% lag stock. Compare the two runs, not the absolute number. The two runs aren't frame-exact either (the game starts at a different emulated time and network traffic differs), so small differences are noise; look at the share and the longest run.

The runner also learns the game's first `OS_Halt` call from these events (the stock SDK sequence `mov r0, #0; mcr p15, 0, r0, c7, c0, 4; bx lr` with achievements off, a jump to `halt_entry` with them on). The launcher and nds-bootstrap don't contain that sequence.

## Runner options

| Option | What |
|---|---|
| `--profile` | the counts above; summary lines starting `[prof]` at exit |
| `--profile-frames N` | stop N VBlank intervals after the game's first `OS_Halt` call |
| `--profile-log FILE` | CSV per interval: `frame,module_vblank,module_idle,module_irq_mode,busy,idle_arrivals,lag` (cycles in 133 MHz units) |
| `--module-base ADDR` | module address if `DSCM_BASE` moves (default 0x02D80000) |
| `--script-from-game` | `--script` frames count from the game's first `OS_Halt` call |
| `--cache-model-from-game` | cache model on from the game's first `OS_Halt` call |
| `--watch-write ADDR:SIZE` | DSi mode: print bus writes (CPU or DMA) to a range with the frame and the CPU's PC; for finding who overwrites memory |
| `--profile-pcs FILE` | with the cache model: the VBlank call's cycles and cache misses by PC, and its data misses and uncached accesses by address (CSV); `ab.sh` takes `PCS=file` |
| `--trace-sd FRAME` | from that frame on: every SD/SDIO command and error IRQ, ARM9/ARM7 IPC sync sends, and (with `--watch-write`) every watched write, with ARM7 timestamps, on stderr; per frame, the ARM7's PC and IRQ state, the SD/MMC controller and the ARM7 NDMA channels; after each SD/MMC command, any ARM7 IRQ taken soon after and when the first status read came |
| `--sd-cmd-busy N` | SD/MMC CMD_BUSY reads as set for N ARM7 cycles after a command (default about 96 SD clocks); 0 means software never sees it, as when an IRQ delays its first status read |
| `--profile-game-pcs FILE` | with the cache model: the game's shadow-cache extra cycles by fetched code line and by data-access PC (CSV); `ab.sh` takes `GAMEPCS=prefix` |
| `--profile-arm7 FILE` / `--profile-arm7-range LO:HI` | ARM7 cycles by 256-byte PC block over the run (CSV) / per interval in a range; `ab.sh` takes `ARM7=prefix` |
| `--whatif-module-isets MASK` / `--whatif-module-dsets MASK` | timing study: the module's code / data use only the cache sets in MASK, as if laid out there (page colouring) |

Use them directly with `RUNNER_ARGS="--profile ..." tests/launcher/run.sh ...`.

## What DScheevos costs the game (`gamecost.sh`)

`tests/perf/gamecost.sh GAME [FRAMES] [SCRIPT]` runs `ab.sh` (achievements off, then on; same
input, `--thumb-cycle`, cache model from the game's first `OS_Halt`) and prints `gamecost.py`'s
table: the **game's own ARM9 cycles per VBlank interval** (PC outside the module, IRQ mode
included) off and on, the delta (avg / median / p90 / max), the game's CPI and cache misses, and
the runner's **shadow-cache** estimate: a second I/D-cache model fed only the game's accesses, so
the difference to the real model is what the module's use of the shared caches costs the game
(I and D split, the part in the game's IRQ handlers, misses and dirty write-backs we cause, how
often the game resumed right after module code). Without a module the shadow equals the real
model (0 on NSMB direct boot). `SKIP=N` drops the first N intervals (menus), `OUT=dir` keeps the
logs, `JSON=1` for machines; `gamecost.py OFF.csv ON.csv` re-reads kept logs. Runner columns:
`game, game_irq, game_instr, game_imiss, game_dreadmiss, game_dwritemiss, game_writeback,
game_iextra, game_dextra, poll_i, poll_d, poll_wb, rev_i, rev_d, arm7_busy, arm7_range,
game_extra_irq, resumes, irqs, irq_max` (cycles in 133 MHz units; the scripts print 67 MHz).

Merge rule (orchestrator, Oct 2026): every module, core or nds-bootstrap runtime change reports
this table for Kirby, MKDS (race) and one busy title against the baseline below.

### Baseline (Oct 2026, eval-speed 85309e2 module (-Os, rcheevos runtime/client -marm), nds-bootstrap 32993f85)

Real sets, idle-budget input scripts (`kirby.txt`, `mkds.txt` ...), melonDS cache model (optimistic).
Cycles at 67 MHz per interval; a frame is 1,120,380.

| | Kirby SSU (1500 intervals) | MKDS race (intervals 2000-3000) |
|---|---|---|
| game cycles off: avg / median / p90 / max | 429,313 / 312,728 / 1,119,622 / 1,119,854 | 707,947 / 702,449 / 746,316 / 866,261 |
| game cycles on: avg / median / p90 / max | 521,059 / 418,211 / 1,118,884 / 1,119,600 | 721,302 / 718,299 / 759,901 / 860,589 |
| **on - off, avg** | **+91,746 (+8.19% of a frame)** | **+13,356 (+1.19%)** |
| on - off per interval: median / p90 / max | 106,085 / 117,496 / 862,635 | 14,834 / 30,851 / 129,531 |
| game CPI off -> on | 2.490 -> 3.061 (+22.9%) | 3.885 -> 3.955 (+1.8%) |
| game I / D-read misses per interval, off -> on | 1050 -> 1860 / 1131 -> 2167 | 4491 -> 4613 / 4406 -> 4494 |
| shadow: game cycles lost to our cache use (I / D) | 100,211 (34,551 / 65,660) | 14,862 (6,317 / 8,546) |
| misses we cause per interval (I / D / write-backs) | 816 / 1497 / 297 | 149 / 159 / 101 |
| lag intervals off / on | 177 / 174 | 0 / 0 (whole run 261 / 264) |

Same build three times on Kirby: +91,839, +91,746, +92,073 (the emulator is deterministic; noise ~0.01% of a
frame). The per-interval delta's max is a load interval shifted by a frame between the runs.

**Why.** Kirby takes IRQs at scanline rate (~460 IRQ-mode runs, ~254 handler dispatches and ~207
`OS_Halt` calls per frame). Every IRQ that lands while module work runs resumes the game into caches we just used:
~200-240 resumes per interval, ~400-500 game cycles lost each (the HBlank path's code at
0x0201e3xx / 0x02024bxx and its data, plus the SDK handler's data). Resumes follow our work time
(correlation 0.97, about one per 3k cycles of module work), so the game's loss scales with the
module's ARM9 time: ~0.17 game cycles per module cycle on Kirby, ~0.03 on MKDS (few IRQs). The
game's IRQ handler runs get slower by ~80 cycles on average (208 -> 287, Kirby), well inside an
HBlank (~1,200 cycles); the longest run (the VBlank handler) p99 31,189 -> 32,509.

### Cache colours (Oct 2026)

What the module costs the game depends heavily on **where** its hot code and data sit, not only on
how much work it does: the game loses cycles when our lines evict its lines from the same cache
sets (ARM946E-S: I-cache 8 KB 4-way, 2 KB per way; D-cache 4 KB 4-way, 1 KB per way). Found when
a 320-byte change ahead of rcheevos in the link (the SDK5 cache fix, no per-frame work of its own)
moved all rcheevos code by 0x140 and cost Kirby +28k cycles a frame (+2.5%); the same build padded
so that the shift was a whole way (0x800) cost nothing.

So `module.ld` pins them: rcheevos' runtime and client, then the core, start at a 2 KB way boundary
plus `TEXT_COLOUR`, ahead of everything else that changes often; `.rodata`, `.data` and `.bss` (and
the low heap after it) start at a 1 KB boundary plus `DATA_COLOUR`. `make -C dscheevos/module
TEXT_COLOUR=0x.. DATA_COLOUR=0x..` builds other colours. Sweep (game's own cycles, on - off avg):

| TEXT_COLOUR (DATA 0) | 0x000 | 0x100 | 0x200 | 0x300 | **0x400** | 0x500 | 0x600 | 0x700 |
|---|---|---|---|---|---|---|---|---|
| Kirby (1500 intervals) | 82.7k | 99.7k | 88.5k | 83.2k | **67.0k** | 75.2k | 69.8k | 72.0k |
| MKDS race (2000-3000) | 21.7k | | | | **8.1k** | | 5.7k | |

DATA_COLOUR at TEXT 0x400: 0 67.0k, 0x100 72.4k, 0x200 80.1k, 0x300 69.3k (Kirby). Chosen: text
0x400, data 0. Cost: up to 2 KB + 3 x 1 KB of padding (+2,120 bytes today). The numbers come from
melonDS's cache model (its replacement policy, optimistic timings); the hardware CPI lab
(`tests/cpi-lab`) should confirm the ranking. Re-sweep when the hot code changes size a lot
(say over 1 KB); a change that moves only cold code no longer moves the hot code.

**For the ratchet below:** before the pin, any change that grew or shrank code ahead of rcheevos
shifted the layout and moved Kirby's number by up to ±2.5% of a frame, ten times the ratchet.
With the pin, a change only moves it through what it really runs, or by moving the pinned
sections themselves (rcheevos, the core): for those, report the table at the chosen colour and the
next best.

### Baseline after the SDK5 cache fix and the colour pin (Oct 2026, perf-next)

| | Kirby SSU (1500 intervals) | MKDS race (intervals 2000-3000) |
|---|---|---|
| on - off, avg | **+67,135 (+5.99% of a frame)** (was +91,746) | **+3,667 (+0.33%)** (was +13,356) |
| game CPI off -> on | 2.491 -> 2.907 (+16.7%) | 3.868 -> 3.988 (+3.1%) |
| lag intervals off / on | 177 / 175 | 0 / 0 |

Same tooling as the baseline above; module `perf-next` (-O2 rcheevos, idle wake split, `--use-blx`,
the SDK5 cache fix, text 0x400 / data 0), runner `dscheevos-perf-runner` (834d8bc0). MKDS moves
by a few thousand cycles between builds with the same pinned layout (8.1k in the sweep, 5.9k and
3.7k for the last two commits; its delta is small next to its 0.7M-cycle frames), so judge MKDS
against the 1% target rather than the ratchet; Kirby repeats within ~0.2k. Lag A/B (`ab.sh`, 3000
intervals, `emu-lock -x`): Kirby 884 off / 877 on, Pokemon Platinum 38 / 38. Platinum's game cost
can't run until its set fits the session area (311 KB reply, 256 KB area: WORKLIST blocker).

### Data through the mirror (opt-gamecost, Oct 2026)

Runner changes first (they move earlier numbers slightly): the 0x0C000000 mirror has main-RAM timing
(melonDS charged it as unmapped, 1 cycle); the module's range is its whole 2 MB region; new columns
`masked, masked_pend, masked_pend_max` (module cycles with CPSR.I set, the part with a game IRQ waiting,
the longest such wait), printed by `gamecost.py` with a CPI-normalized cost ("at equal work": MKDS's
game does up to 1.8% more or less work between runs, +-7k cycles of noise in the raw delta).

Nothing of DScheevos runs in the game's interrupts any more (no IPC sync 0xA; the idle thread polls the
ARM7's VBlank count). The module's data goes through the uncached 0x0C000000 mirror (statics always; heap,
arena and the core's state for sets whose reply is <= 64 KB). Kirby (1500 intervals): +55.4k -> +20.9k
(4.95% -> 1.86% of a frame; shadow D 47.2k -> 14.6k, I 11.7k -> 13.6k, +-5k by layout); MKDS keeps its heap
cached (shadow ~12-13k either way); Pokemon Black (8000-9500) keeps it cached too. Details, dropped ideas and
the floor: research/optimize/opt-gamecost.md in the planning folder.

### Proposed gate

1. Lag intervals equal off vs on (the existing A/B rule).
2. **Ratchet:** a change may not raise the game-cost delta (on - off, avg) by more than **0.25%
   of a frame (2,800 cycles)** on any of the three games against this table (or the table its
   merge updates); a change that lowers it updates the table. 0.25% is ~25x the run-to-run noise
   and catches regressions of the size real module changes make (-O2 rcheevos: -0.2%).
3. **Target** (the zero-compromise goal; not met today): <= 1% of a frame on every game. Kirby's
   6.0% is the debt (8.2% before the colour pin); MKDS meets it (0.3-0.7%).

## Results

Oct 2026, module built from a clean checkout of `8e35fa1` (`MODULE=`), nds-bootstrap `73f2adf` plus the cache slot fix below, synthetic typical set, no input (title screens and attract demos), 6000 intervals from the game's first `OS_Halt` call, cache model on. Cycles at 67 MHz per VBlank interval; a frame is 1,120,380.

**Kirby Super Star Ultra:**

| | Achievements off | Achievements on |
|---|---|---|
| lag intervals | 1809 (30.15%) | 1855 (30.92%) |
| longest lag run | 7 | 9 |
| module VBlank call, avg / max | 0 | 113,951 / 403,243 (10.2% / 36.0%) |
| VBlank call when it takes a snapshot (1694 intervals) | | 398,189 avg (35.5%) |
| module idle work, avg / max | 0 | 310,131 / 797,752 (27.7% / 71.2%) |
| ARM9 busy | 59.5% | 100% |

The attract demo runs at 30 fps, so a third of the intervals lag in both runs; per 500-interval block the two runs track each other (the 60 fps title blocks show 0 lag in both). Evaluation can't keep up: the module dropped 4307 of the 6074 frames it saw (queue full), and a VBlank call that finds the queue full skips the snapshot, so the average hides the real cost. Each snapshot costs about 398k cycles, a third of a frame inside the game's interrupt. No network traffic got out during play (status "connecting", 5 unlocks unsent): the idle work never ran out.

**Pokemon Platinum:**

| | Achievements off | Achievements on |
|---|---|---|
| lag intervals | 49 (0.82%) | 78 (1.30%) |
| longest lag run | 20 | 31 |
| module VBlank call, avg / max | 0 | 396,373 / 401,369 (35.4% / 35.8%) |
| module idle work, avg / max | 0 | 609,017 / 744,825 (54.4% / 66.5%) |
| ARM9 busy | 9.0% | 98.9% |

The intro and title screen leave the ARM9 idle 90% of the time, so even a third of a frame in the interrupt rarely makes the game miss a VBlank. That headroom is what hides the cost here; a busy scene won't have it. Here every VBlank call takes a snapshot. With the cache model off (an earlier run, first 4900 intervals) the VBlank call averaged 241,163 cycles and a snapshot 274,506: the cache model makes the snapshot about 45% dearer, as its reads are scattered (random addresses in the synthetic set).

**Cache slot fix (nds-bootstrap).** Without it, Pokemon data-aborts with achievements on about 4950 intervals in (nds-bootstrap's crash screen, PC 027FDB9F), with the module's RAM at 0x02D80000 overwritten with 0xAA, header included. `--watch-write 0x0CD80000:4` caught the writer: nds-bootstrap's ARM7 card engine, in `sdmmc_send_command` (an SD sector read into a ROM cache slot), writing 0xAAAAAAAA to 0x0CD80000, the same RAM as 0x02D80000. `hook_arm9.c` cuts the cache size at `DSCHEEVOS_CACHE_END`, but the slot loop skips holes (`romLocationAdjust`) and so places the last slots past the cut. The fix in `retail/bootloaderi/source/arm7/hook_arm9.c` drops any slot that would end past `DSCHEEVOS_CACHE_END` when DScheevos is on. Kirby's numbers are the same with and without it.

### After the direct snapshot copy (perf branch, Phase 2)

Same setup, module with the copy program and `platform.memory_map` (reads main RAM and DTCM directly, rcheevos in ARM mode), nds-bootstrap with the IPC hook after the card-read continuation and the bounded SDIO shim.

| | Kirby off | Kirby on | Pokemon off | Pokemon on |
|---|---|---|---|---|
| lag intervals | 1808 (30.13%) | 1798 (29.97%) | 50 (0.83%) | 55 (0.92%) |
| longest lag run | 7 | 7 | 21 | 23 |
| module VBlank call, avg / max | 0 | 40,023 / 122,795 (3.6% / 11.0%) | 0 | 108,159 / 123,941 (9.7% / 11.1%) |
| module idle work, avg / max | 0 | 361,562 / 788,494 (32.3% / 70.4%) | 0 | 895,301 / 1,112,688 (79.9% / 99.3%) |
| ARM9 busy | 59.5% | 99.9% | 9.0% | 98.9% |

The snapshot is about 12k instructions (qemu) but about 108k cycles here: with the cache model on, the scattered reads miss the data cache, so memory latency now dominates. It is still about 100 times the 0.1% budget, inside the game's interrupt. Kirby's lag matches stock; Pokemon's is 5 intervals over (it was 28 before). The next step takes the copy out of the interrupt (snapshot when the game halts).

### Snapshot when the game halts (perf branch, Phase 1)

The module copies at the halt (`dsc_capture`, IRQs masked in chunks of 16 words or chain steps, letting any waiting IRQ run in between) and the VBlank IRQ only queues the copy. Each idle pass evaluates at most 4 frames, then the network, then copies again and halts. Same setup as above, network live (hardcore).

| | Kirby off | Kirby on | Pokemon off | Pokemon on |
|---|---|---|---|---|
| lag intervals | 1808 (30.13%) | 1798 (29.97%) | 49 (0.82%) | 49 (0.82%) |
| longest lag run | 7 | 7 | 20 | 20 |
| module VBlank call, avg / max | 0 | 2,752 / 4,066 (0.25% / 0.36%) | 0 | 3,081 / 3,882 (0.28% / 0.35%) |
| unlocks the mock server got | | 5 | | 2 |

Lag equals stock in both. The VBlank call is about 75 instructions plus the module's self-timing; with melonDS's flat cache timing (`CACHE_MODEL=0`) it is 467 cycles (0.04%), so the rest is cache misses on the cold IRQ path. Evaluation of this synthetic set still needs about a frame of CPU per frame: Kirby evaluated 1812 of 2652 queued frames and dropped 3399, Pokemon dropped 412 (evaluation speed is the next phase).

Found on the way (the second one turned out not to be fixed; see the next section):

- **An idle loop that never halts stalls Kirby.** With frames queued, returning to the idle loop without halting kept the ARM9 busy and Kirby's attract demo stopped streaming. The module now halts after every pass.
- **Wi-Fi next to a ROM read hangs Kirby.** Once the network got idle time, the ARM7 SDIO shim ran while the card engine had an SD read in flight; the reads failed (`sdmmc_init` retries) and the game hung at the same point every run. With the cache model off, or the network off, it didn't. nds-bootstrap now skips the shim's poll while `readOngoing` (`ring->stats.paused_card_read`). Hardware may differ, but keeping the two apart is safe either way. (That guard did not stop the hang at frame 5037: see "Kirby's hang" below; it was an nds-bootstrap race.)

### The VBlank call trimmed (perf branch, `perf-irq-trim`)

The IRQ now only counts the frame: the core's `dsc_vblank_queue` (27 instructions, no stack, no calls) hands it the capture taken at the halt, and idle time adds it to the queue (`drain`, at the start of `dsc_capture`, `dsc_idle`, `dsc_get_stats` and `dsc_reset`). The module's part is in assembly next to the header (`vblank_entry`): it reads the ARM7's count past the cache, keeps its state in one cache line (a second one only when an idle pass is cut or VBlanks were missed), runs on the game's IRQ stack (the three words it always pushed), and leaves the per-frame idle statistics to the idle thread. The copy for a session's first frame still runs in the IRQ, on the module's stack.

Where the old call went (`--profile-pcs`, Kirby, cache model): all of it was cache misses on a cold path, about 28 instruction and 32 data line fills a frame: Thumb interworking stubs, literal pools, the module's own stack (4 lines), and fields spread over the core's state and the status. Now: about 7 instruction lines, 2 data lines, a literal and 3 uncached reads (the ARM7's count, `REG_VCOUNT` twice).

| | Kirby off | Kirby on | Pokemon off | Pokemon on |
|---|---|---|---|---|
| lag intervals | 1808 (30.13%) | 1795 (29.92%) | 50 (0.83%) | 50 (0.83%) |
| longest lag run | 7 | 7 | 21 | 21 |
| module VBlank call, avg / max | 0 | 525 / 699 (0.05% / 0.06%) | 0 | 529 / 2,072 (0.05% / 0.18%) |
| unlocks the mock server got | | 5 | | 2 |

Before (same setup): 2,752 and 3,081 cycles. With the flat cache (`CACHE_MODEL=0`) Pokemon's call is 151 cycles. The Pokemon max is the first frame's copy in the IRQ. Steps, Pokemon avg: deferred queueing and the assembly entry 758; a shorter entry 673; idle statistics out of the IRQ 529.

**Kirby's hang: an nds-bootstrap race, fixed.** With the network live, Kirby's attract demo froze at the same frame (5037, interval 2300) in about half the runs, with the Phase 1 module (`perf-baseline-p1`) and with this one (10 of 22 runs in all). It wasn't the network or the module. `--trace-sd` showed the card engine's ARM7 sending CMD18 for one sector from `sdmmc_send_command_nonblocking_ndma`, the game's Timer1 IRQ coming in 2 cycles later and running for about 4,000 ARM7 cycles (120 us), and the first status read after it finding the command done (response and data end set, the NDMA had moved the sector) with CMD_BUSY already clear. That function waits for CMD_BUSY to be set and has no other way out (except under no$gba), so the ARM7 spun there for good and the ARM9 waited on the read. CMD_BUSY only lasts while the command and response are on the bus (about 96 SD clocks, 12 us at the card engine's clock), so any IRQ longer than that, in that two-instruction window, does it. That is nds-bootstrap's own code (`retail/common/source/my_sdmmc.c`, upstream too), so it can happen on hardware and without DScheevos; the network only moved the ARM7's timing so that the read met Timer1, while with the network off the run is deterministic and the read happens to miss it. `--sd-cmd-busy 0` (CMD_BUSY never seen) makes it certain: stock nds-bootstrap with achievements off then never gets the game to its first `OS_Halt`. The fix (nds-bootstrap `dscheevos-perf`): the loop also returns once STATUS0 shows the response came in; `sdmmc_check_command_ndma` finishes the read as before. After it: `--sd-cmd-busy 0` runs Kirby to the end with achievements off and on, and 12 of 12 network runs passed (2,600 intervals each), against 10 of 22 frozen before. The `readOngoing` guard on the shim stays: it costs nothing and keeps Wi-Fi SDIO traffic away from in-flight reads. The same loop is in nds-bootstrap's `hb/` copies (homebrew), not fixed there.


### Mario Kart DS: longer loading screens, the skipped preload (`perf-mkds-lag`)

Mario Kart DS with its real set lagged more with achievements on: 217/216/224 lag intervals against 203/204/201 stock (cache model), 192 against 172 with the flat cache, so not the cache model; also with the network off (197 against 171). All the lag is in five loading runs (title, attract demo loads), and with achievements on each load was longer, while the module ran nothing in them but the VBlank call (no idle arrivals: the ARM9 is busy all through a load). `--trace-sd` counted the card engine's SD reads during play: 275 stock, 494 on, about 100 more in the first load alone.

The cause is nds-bootstrap's preload. For 140 DSi entries (`preLoadSettings`), nds-bootstrap loads the ROM ranges a game reads most into the ROM cache at boot, if they fit (`dataToPreloadFound`, with 256 KB left over). Mario Kart DS (AMCE) preloads two ranges, 10.1 MB in all. DScheevos takes its 2 MB out of the same RAM (`DSCHEEVOS_CACHE_RESERVE` in that check), so the preload no longer fit and nds-bootstrap skipped all of it: the game read those 10 MB from SD while loading. Proof: stock nds-bootstrap (achievements off) with only the preload switched off gave 194 and 196, with the same loads as the achievements-on run (first load 26 intervals instead of 22, second 15 instead of 14) and 513 reads.

40 of the 140 entries are in that band: they fit without DScheevos and not with it. Among them New Super Mario Bros. (A2D\*), Super Mario 64 DS (ASM\*) and Zelda: Phantom Hourglass (AZL\*).

The fix (nds-bootstrap `retail/bootloaderi/source/arm7/main.arm7.c`, `dscheevosTrimPreload`): when the preload fits without DScheevos but not with it, keep as much as fits, trimming from the end in 16 KB steps, instead of dropping it all. Mario Kart DS keeps 9.25 MB of its 10.1 MB. Games whose preload fits either way, or never fit, are unchanged. `--watch-write 0x0CD80000:0x200000 --watch-skip 0x02D80000:0x80000` saw no write into the module's RAM other than the module's own (stock nds-bootstrap's cache writes there, which shows the watch works).

Mario Kart DS, real set, network live, 6000 intervals, after the fix:

| | Stock | Achievements on |
|---|---|---|
| flat cache (`CACHE_MODEL=0`), lag | 185, 181, 178, 180 | 185, 178, 175, 183 |
| cache model, lag | 199, 199, 202, 206 | 205, 206, 209, 210, 209, 200, 202 |
| cache model, stock nds-bootstrap with the same trimmed preload | 208, 205, 204 | |

The two short loads are now the same length in every run (22 and 14 intervals flat, 34 and 17 with the cache model); the three long ones vary by up to 8 intervals between runs in both modes. With the flat cache the two match. With the cache model, achievements on equals stock nds-bootstrap with the same trimmed preload, so the module itself adds nothing. What is left, about 4 intervals in 6000 on average, all in loading screens, is the 0.9 MB of preload that doesn't fit beside the module's 2 MB: about 30 more SD reads. Only a smaller module region would win that back. Evaluation: 0 frames dropped with the flat cache, about 1,600 of 6,000 with the cache model (evaluation speed, a separate item).

### Boot: nds-bootstrap loads over the module (`perf-boot-overlap`)

Three ways nds-bootstrap could write over the module while booting a game, all after the module was loaded:

1. **ROM-in-RAM.** The size check allowed `0xBE0000 - 0x260000 + 0x88000 = 0x9E8000` bytes, but only 0x964000 fit below `0x0CD80000` once nds-bootstrap's holes are skipped, and `loadROMintoRAM` ran after `dscheevosSetup`. A ROM whose mapped size fell in between was loaded over the module.
2. **AP-fix overlays.** For a game whose AP-fix patch touches overlays (and Super Mario 64 DS), `loadOverlaysintoRAM` reads the overlays to `0x0C800000`, patches them, writes them to `apfixOverlays.bin` and zeroes that RAM. With an overlay span over 5.5 MB that range reaches the module. The overlays are only passing through: the card engine serves them from the file afterwards.
3. **The reserve constant.** `DSCHEEVOS_CACHE_RESERVE` was 0x260000; the cut actually costs 159 of 760 slots, 0x27C000, in every layout (stock vs cut slot tables for both SDK variants and the VRAM Wi-Fi start). The preload check was 112 KB too generous.

Reproducers (melonDS, launcher flow, the module's text and rodata in the RAM dump compared with `dscheevos9.bin`, apart from `.data.irq`, which is writable state placed next to the code):

| Case | Before | After |
|---|---|---|
| Clubhouse Games with the header's ROM size raised by 0x50000 (mapped 0x983370, ROM-in-RAM) | module not loaded, 35,948 words overwritten | cache mode, module active, evaluating, intact |
| Super Princess Peach (overlay span 5.56 MB) with a one-byte AP-fix IPS that rewrites an overlay byte with its own value | module not loaded, 16,260 words overwritten | module active, evaluating, intact |
| Super Princess Peach without the IPS (control) | passes | passes |

The fix (nds-bootstrap `retail/bootloaderi/source/arm7/main.arm7.c`):
- `dscheevosRomMapFits` builds the ROM map as `buildRomMap` will and checks that every piece ends by `DSCHEEVOS_CACHE_END`. ROM-in-RAM is only used when it does (otherwise cache mode), and `dscheevosTrimPreload` trims until the real map fits, not only the arithmetic check.
- `buildRomMap` stops instead of writing past the card engine's 8 map lines.
- `dscheevosSetup` moved after the ROM, overlay, preload and FNT/FAT loads, and before the page file saves the ARM9 binary.
- `DSCHEEVOS_CACHE_RESERVE` = 0x27C000.

Mario Kart DS (real set, 6,000 intervals, cache model): stock 197, 206, 198; achievements on 206, 205, 199, the same band as before the change (199-206 / 200-210).
