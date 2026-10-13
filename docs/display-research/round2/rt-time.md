# Red team: prop-time.md (TIME)

Read-only. Scripts and outputs are in `round2/rt-time/`:
- `chk.py`: re-derives the MKDS, BIS and NSMB numbers;
- `chk2.py` → `thumb.txt` and `chk3.py`: sensitivity to slower hardware timing;
- `sched.py` → `sched.txt`: simulates the T5 scheduler.

The data is census `frames.csv` (93 runs as of 22:13 today) and melonDS `origin/dscheevos-wait-census:src/ARM.cpp`.

Severity counts: 2 blockers, 7 majors, 6 minors.

---

## Re-derived numbers

| Claim | Re-derived | Verdict |
|---|---|---|
| Frame = 2,240,760 units = 1,120,380 ARM9 cycles; line = 8,520 units | 263 × 355 × 6 × 4 = 2,240,760 | holds |
| MKDS: 0% idle by 156; 182-189: 53% of frames, 8% in runs ≥ 120 | 0.0%; 53.3% and 7.9% | holds |
| MKDS: "strict period of 5" | Agreement with the frame k frames earlier (autocorrelation): lag 1 = 0.86, lag 5 = 0.83, no peak at 5. The pattern is irregular (`#####..###..####......`). | **wrong** (minor) |
| BIS 170-189: 84%, 47% in runs ≥ 120; 182-189: 86% / 64%; 8k run: 93% / 71% | Same. But the 8k run at 170 gives **69% / 23%**, which the proposal doesn't cite. | holds, with a caveat (M3) |
| NSMB gap-start spacing of 14-15 frames in > 95% of cases | NSMB 94%, Clubhouse 97% | roughly holds |
| Every-line H-Blank games: the late "busy" is a counting artifact | Partly true. The cause is wrong (B1). | see B1 |

---

## Blockers

### B1. T4's "thread woke" test fires on every H-Blank in exactly the games T4 targets

**Evidence.**

The census rule is cumulative: `CensusSeqRun += c` for every non-IRQ, non-BIOS instruction since the run began, and the run breaks above 4,000 (ARM.cpp, census branch, the `CensusSeqRun` lines).

I split `wait_run` by how many lines before VBlank the last wait began (median units added per line):

| Game | Units per H-Blank line |
|---|---|
| Pokémon Black | +1,366 |
| KSSU | +1,318 |
| Mario Party | ~+1,300 |
| Nintendogs | ~+1,300-1,466 |
| Starfy | +1,264 |
| **Animal Crossing** | **+14** |
| **Trauma Center 2** | **+23-46** |

- Black takes 218 halts per frame (2,071,285 halts over 9,499 intervals in `sites.csv`): one re-halt per H-Blank.
- So in Black, KSSU, Mario Party, Nintendogs and Starfy, each H-Blank IRQ is followed by about 650 ARM9 cycles of **thread-mode** code. The idle loop going back into the halt costs about 7 cycles (Animal Crossing).
- With the 4,000-unit cap, the run can't be longer than 3 lines. That is the whole explanation of the 188-191 pile-up.
- Black's hot PCs sit in its ITCM dispatcher (01FF8100-01FF8290) and in main-RAM code at 02084xxx-02085xxx, next to the idle loop (lr 02085B2C). That is most likely the SDK thread module.

**Likely mechanism (speculation from NitroSDK, to be confirmed by disassembly).**
- `OS_WaitVBlankIntr` → `OS_WaitIrq`, which sleeps on `OSi_IrqThreadQueue`.
- `OS_IrqHandler_ThreadSwitch` wakes that whole queue and sets `isNeedRescheduling` after **every** IRQ.
- The main thread then runs, finds the VBlank check flag clear, and goes back to sleep.

**Consequences.**
- T4's learner ("did idle resume before another thread ran?") and its `isNeedRescheduling` check both say "woke" on every H-Blank. T4 then aborts every line, so these 5 titles get **0%**.
- The proposal reads the flag "at the dispatcher's exit, before `OS_IrqHandler_ThreadSwitch`". If the queue wake happens inside ThreadSwitch, that check **misses** real wakes of `OS_WaitIrq` waiters (DMA, GX, IPC completion) and lets them run with the FIFO live. That is a lag risk.
- Either placement fails.
- Separately, Animal Crossing and TC2 do **not** show the artifact (14-46 units per line). Their bottom numbers are real, and Animal Crossing fails on runs anyway (0% in runs ≥ 120 at every window).

**Fix.** Hook the entry of ThreadSwitch and abort only if:
- (a) `isNeedRescheduling` was already set by the handler, before the queue wake; or
- (b) the IRQ check flag word (DTCM+3FF8) gained a bit during this IRQ.

Let a queue-only wake (wake, re-check, sleep) run with the strip live.

**Test.** In the thread-aware census, for each wake log: thread id, the IRQ check-flag delta, and whether the thread went back to sleep in `OS_WaitIrq` with no other code run. Pass: ≥ 99% of H-Blank wakes in Black and KSSU are queue-only round trips.

### B2. Persistent indicators (challenge and measured) have no flicker-free policy

T5 covers notices only. A challenge or measured indicator stays up for minutes, so every gap means hiding it and showing it again.

Gap onsets per minute at the 182-189 window (best-case gameplay titles):

| Game | Onsets per minute |
|---|---|
| Canvas Curse 8k | 1.8 (best) |
| Platinum | 5.4 |
| SoulSilver | 6.1 |
| Hotel Dusk | 10.1 |
| Phoenix | 11.2 |
| Sonic Rush | 27 |
| Tetris | 29 |
| Digimon | 38 |
| TWEWY | 42 |
| Zelda ST | 92 |

That is a blinking indicator, banned by the owner's bar. The proposal's own T6 calls a slow blink with ≥ 250 ms phases "a UX animation". If that argument is used here, it needs an explicit owner ruling: the gaps arrive at random, 5-90 times a minute.

**Fix.** Give persistent indicators a mechanism that doesn't depend on idle time (space or LED), or hold them hidden for a whole phase with hysteresis, subject to an owner ruling.

---

## Majors

### M1. T5's "≥ 99% completion" is unreachable, and partial banners remain

`sched.py`: share of notices (90 frames) that complete once the window has been clean for W frames (182-189), with W = 30/60/120/240:

| Game | Completion rate |
|---|---|
| Platinum | 77 / 82 / 80 / 71% (n = 13 / 11 / 10 / 7) |
| SoulSilver | 71-83% |
| Digimon | 63-68% |
| BIS 8k | 65-79% |
| Tetris | 25-73% |
| TWEWY | 30-78% |
| Layton | 0-33% |

- No game reaches 99% at any W. The samples are small, but 20-35% of notices are cut short even in the best gameplay titles.
- **Truncated notices can be partial banners.** These frames went idle earlier (at least one line of halt) but the last wait began inside the strip. Per 1,000 frames:

  | Game | Bottom (158-189) | Top (0-31) |
  |---|---|---|
  | Platinum | 8 | 173 |
  | SoulSilver | 15 | 147 |
  | Zelda ST | 67 | 348 |

  These are upper bounds: the census can't show whether the game was idle at line S-1.
- "Never a partial notice" is false. The proposal concedes this itself ("partial banner once").

**Fix/test.** Log the first wake line after S-1 (thread-aware census) and report the partial rate per title. The owner must accept a measured residual, or the strip must not be aborted, which costs working time.

### M2. The coverage evidence is mostly menu screens

18 of the 27 titles in T5's "≥ 60%" list come from "menus (generic)" or generic 8k runs (per the idle-census.md table). So do most of the T3 list and 6 of the 9 BIOS ROMs.

The one title with both kinds of run, MvDK Mini-Land Mayhem, shows the problem:
- menus: 88% / 84% at 182-189;
- gameplay (EU, `mvdk`): 47% / 46%. It includes **2,000 consecutive frames (33 s) with zero halts** (busy median 2.18 M units, PC 0205Dxxx), during a cutscene (`mvdk/f2500.png`).

Layton has 1,000 no-halt frames during an FMV (`layton/f3000.png`). 999 swings from 11% to 92% per 500-frame chunk.

So "busy, never halts" (class D) also appears **inside** good games, by phase.

**Fix.** Run a gameplay census across phases (cutscene, menu, play) for each title before ranking.

### M3. Fixed per-game windows don't hold across phases or runs

Per-500-frame coverage at 182-189:

| Game | Bottom (182-189) | Top |
|---|---|---|
| Platinum | 4 → 99-100 → 39 → 0% | 1-100% |
| BIS | | 62 → 0% |
| Layton | 0-97% | |

The window itself doesn't flicker (T5 gates it), but a fixed `S` chosen from one run can be wrong:
- BIS waits at 168 on 57% of 8k frames and at **169 on 21%**, so S = 170 with K = 2 loses a fifth of its frames to one line of jitter.
- BIS is busy 221 lines and then halts exactly at 168/169. That looks like a poll to the VCOUNT-168 IRQ (speculation).

**Fix.** Choose `S` with a measured hardware jitter margin (BIS S ≥ 172), from multi-phase gameplay data.

### M4. Thumb and no-cache timing: not fatal to safety, but fatal to some margins

- The census data (written 22:10-22:13) predates `--thumb-cycle` and the cache model, which census.sh made default in ef07636 at 22:46. idle-census.md says "no cache model".
- cpi-lab (8ee2d45): melonDS runs Thumb ALU code at 0.51 cycles per instruction, against about 1.0 on real hardware.
- I scaled each frame's busy time by f, which pushes the end of work later. Two models: pessimistic (all busy time scales), and optimistic (earlier halts absorb the extra).

| Window / game | Change |
|---|---|
| Bottom windows of early finishers (Platinum, Phoenix, Hotel Dusk, Canvas, PvZ, Yoshi) | unchanged up to f = 2 |
| **T3 top: Digimon** | 91/72 → 66/41 at f = 1.25 (both models) |
| T3 top: Ghost Trick | 85/73 → 70/28 at f = 1.5 |
| T3 top: Herbert's Revenge 8k | 96/94 → 71/4 at f = 2 |
| NSMB 182 | 78 → 40 at f = 2 (optimistic) |
| Sonic Rush 182 | 89/77 → 54-62 / 22-33 at f = 2 |
| MKDS 182 | 53/8 → 28/4 at f = 1.25 (pessimistic) |

Decisions are made live on the console, so a timing error lowers coverage and raises the abort (partial) rate. It doesn't break the lag rule.

**Fix.** Re-run the census now with the new defaults, plus a module-side wait-line log on the DSi, dumped at quit.

### M5. The X bound (0.005-0.01% = 56-112 cycles) leaves out real terms

- **(a) T4 runs game handlers under FIFO contention.** On a first-time wake, the woken thread starts later by the handler's contended main-RAM and I/O accesses. A ~4-word burst wait per access is my estimate (GBATEK: the FIFO takes 4 words per DMA, gbatek.txt:8194-8196). This is game working time and isn't in X.
- **(b) T4's runtime learner needs bookkeeping on every IRQ, session-wide**, including IRQs during game work (263+ per frame in H-Blank games). That contradicts §4 item 1, "0 cycles unarmed". Fix: seed the set offline per title and don't learn at run time.
- **(c) Cache footprint.** Module code and data outside TCM evict game I-cache and D-cache lines, and the game pays the refills later.
- **(d) R21** (ARM7 replies slowed) is still unbounded.
- **(e) The pass mark can't verify the claim.** "Zero lag difference at k = 200" is a slack test, not a measurement of a 56-112-cycle cost.

**Fix.** In the display-lab ROM, use a timer (lab code, not the game) to measure, with the strip on and off:
- abort latency;
- time from IRQ to thread start;
- a cache-pollution case.

### M6. The entry/exit IRQ races the game's H-Blank IRQ

- NDMA3's IRQ is raised after its 1-word transfer, so the game's H-Blank bit (bit 1, serviced lowest-first by the SDK dispatcher) is usually already latched.
- "Ours first" holds only if both bits appear in the same IE&IF read. Otherwise our STRB waits for the whole game handler: line S tears, or the strip spills onto line S+N.

**Fix.** Make the patched dispatcher test our bits before any game bit, re-reading IF when it is entered for the game's H-Blank. Alternatively, a hardware-timed exit: NDMA writes the DISPCNT high halfword. That is only safe if byte 3 is constant (speculation).

**Test.** In display-lab, run H-Blank handlers of 100-1,000 cycles and inspect lines S, S+N-1 and S+N (the proposal's test checks only S-1 and S+N).

### M7. The "unservable by time" list is not backed by evidence

- **DKR.** idle-census.md says the race thread "spins `for(;;) consume(obj)` on a ring buffer's read/write indices (0x0201A344 / 0x0201A294)". That is T8-D case (a), a RAM-polling loop, not "no time". It must be tried with T8-D first.
- **MKDS.** "149 lines are most likely GX/DMA waits" is speculation (the proposal's §1 item 3 test is still pending). The lever of a player-chosen 133 MHz clock (ruling B, compliance.md) isn't listed. DScheevos won't turn it on, but it could serve players who chose it.
- **Class E.** I agree there is no time lever: the engine-A strip shows on each screen at 30 Hz, and capture source B (MMEM FIFO, gbatek.txt:8148) is the game's choice.
- **Class G.** The gap length depends on melonDS's SD timing, so it could be longer or shorter on hardware. nds-bootstrap also polls VCOUNT from 023BF3AE in every game (idle-census side finding). Measure on hardware before reporting.

---

## Minors

- MKDS "period of 5": see the table above.
- NSMB 14/15 spacing is 94%, not > 95%.
- "24 of 66 runs" counts duplicate variants (8k, -arm7, -dirty). Today it is 39 of 93 runs; count distinct titles instead.
- AC and TC2 are grouped with the "fake busy" games but don't show the artifact (B1).
- T8-C (BIOS wait signal):
  - It is sound in outline.
  - Reading the interrupted PC needs the DSi ARM9 BIOS IRQ stack layout in NTR mode; verify it.
  - Its abort inherits T4's predicate. That is fine for Canvas Curse, which has no `OS_WaitIrq` queue.
- §4 says to invalidate the I-cache for an ITCM patch. ITCM isn't cached, so that step isn't needed. Patching game ITCM twice per armed frame still widens the "never touch game RAM" residual, and the owner must accept that.
