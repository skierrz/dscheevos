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

- **module VBlank call**: PC inside the module (`DSCM_BASE` 0x02D80000 up to the image size from the module header; see `dscheevos/module/module.ld`) while it runs the VBlank call: `vblank_entry` → `module_vblank` → `dsc_vblank`, the snapshot, called from nds-bootstrap's ARM9 IPC handler inside the game's interrupt. Told apart from idle work by the PC (the entry stub) or the stack pointer (the module's own IRQ stack, which `vblank_entry` switches to), not by the CPU mode. The game is frozen for all of it.
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

Use them directly with `RUNNER_ARGS="--profile ..." tests/launcher/run.sh ...`.

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

Found on the way, both fixed:

- **An idle loop that never halts stalls Kirby.** With frames queued, returning to the idle loop without halting kept the ARM9 busy and Kirby's attract demo stopped streaming. The module now halts after every pass.
- **Wi-Fi next to a ROM read hangs Kirby.** Once the network got idle time, the ARM7 SDIO shim ran while the card engine had an SD read in flight; the reads failed (`sdmmc_init` retries) and the game hung at the same point every run. With the cache model off, or the network off, it didn't. nds-bootstrap now skips the shim's poll while `readOngoing` (`ring->stats.paused_card_read`). Hardware may differ, but keeping the two apart is safe either way.
