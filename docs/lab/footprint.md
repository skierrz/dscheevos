# LAB footprint: cache lockdown and deferral (2026-10-16, branch footprint-lab, not for merge)

melonDS runner (cache model, --thumb-cycle, DSi), tests/behaviour gate, 3,000 VBlank intervals from a state saved
in play. Every build has its own state and reference (the state holds the module image). Each run alone: two
identical loads give byte-identical profiles. Game cost = the game's own ARM9 cycles per interval, on - off
(67 MHz). Builds: lock0 = same-layout control; lock3 = lockdown base 3 for the whole wake (DSC_LOCK_MODE 1);
LOCK3MOD = lock3 with the runner oracle --dsc-lock-modonly (lockdown applies to module refills only: the upper
bound of evaluating in IRQ-masked slices); lockcap = locked only while CPSR.I is masked (the capture);
defer60 = no pass when the interval's first wake came 60+ lines after VBlank (never fired: see below).

| run | game cost on-off | shadow I | shadow D | screens top/bottom | lag (idle) | sound cmds | stuck |
|---|---|---|---|---|---|---|---|
| LOCK0-tetris | +1898 cycles = +0.17% | 1343 | 515 | FAIL (5 / FAIL (4 | PASS (20 vs 20) | PASS | 1 |
| LOCK0again-tetris | +1898 cycles = +0.17% | 1343 | 515 | FAIL (5 / FAIL (4 | PASS (20 vs 20) | PASS | 1 |
| LOCK3-tetris | +2513 cycles = +0.22% | 1222 | 1390 | FAIL (2329 / FAIL (2780 | FAIL (4 intervals differ, first frame 3928) | FAIL (2694 frames | 2 |
| LOCK3MOD-tetris | +571 cycles = +0.05% | 344 | 156 | FAIL (1 / PASS (3001 | PASS (20 vs 20) | FAIL (2 frames | 1 |
| LOCKCAP-tetris | +1980 cycles = +0.18% | 1291 | 648 | FAIL (7 / FAIL (5 | PASS (20 vs 20) | FAIL (92 frames | 1 |
| DEFER60-tetris | +1910 cycles = +0.17% | 1347 | 522 | FAIL (5 / FAIL (4 | PASS (20 vs 20) | FAIL (2 frames | 1 |
| LOCK0-platinum | +5140 cycles = +0.46% | 3350 | 2076 | PASS (3001 / PASS (3001 | PASS (3 vs 3) | FAIL (6 frames | 0 |
| LOCK3-platinum | +1397 cycles = +0.12% | 922 | 516 | FAIL (1 / PASS (3001 | PASS (3 vs 3) | FAIL (10 frames | 0 |
| LOCK3MOD-platinum | +1458 cycles = +0.13% | 962 | 548 | FAIL (1 / PASS (3001 | PASS (3 vs 3) | FAIL (8 frames | 0 |
| LOCKCAP-platinum | +5710 cycles = +0.51% | 3220 | 2746 | FAIL (1 / PASS (3001 | PASS (3 vs 3) | FAIL (4 frames | 0 |
| DEFER60-platinum | +5273 cycles = +0.47% | 3375 | 2174 | FAIL (1 / PASS (3001 | PASS (3 vs 3) | FAIL (6 frames | 0 |
| LOCK0-kirby | +38300 cycles = +3.42% | 26360 | 20188 | PASS (3001 / PASS (3001 | PASS (58 vs 58) | FAIL (4 frames | 7 |
| LOCK3-kirby | +194947 cycles = +17.40% | 46405 | 167985 | FAIL (2948 / FAIL (2863 | FAIL (62 intervals differ, first frame 3148) | FAIL (2843 frames | 3 |
| LOCK3MOD-kirby | -3312 cycles = -0.30% | 2758 | 1349 | PASS (3001 / PASS (3001 | PASS (58 vs 58) | FAIL (2 frames | 6 |
| LOCKCAP-kirby | +24551 cycles = +2.19% | 24819 | 7892 | PASS (3001 / PASS (3001 | PASS (58 vs 58) | FAIL (11 frames | 11 |
| DEFER60-kirby | +43010 cycles = +3.84% | 33685 | 17819 | PASS (3001 / PASS (3001 | PASS (58 vs 58) | PASS | 6 |

Reading:
- The cost is the game's cache refill after our evaluation pass. The oracle removes most of it (Tetris -70%,
  Platinum -72%, Kirby to below zero), lag equal, Tetris screens 5+4 -> 1.
- Plain lockdown (lock3) is unsafe: a game thread that preempts the pass runs under our lockdown (one refill
  way) until the pass resumes. Tetris: a CPU-bound load stretch ran 4 intervals longer; Kirby +195k. Nothing on
  the ARM9 can unlock at preemption without the game's IRQ path, so a lockdown is safe only while IRQs are masked.
- lockcap: no gain on Tetris/Platinum, -36% on Kirby (its per-halt copies); copy-once (stagefix phase 2) covers it.
- defer60 never fired: in heavy frames the first idle wake after VBlank comes mid-frame. module.c now has the
  previous-frame rule (g_idle.last against the game's running average, DSC_DEFER_LATE=24); built, not run.
- MKDS not run (stopped).

DEFER24 (previous-frame rule, DSC_DEFER_LATE=24), run 2026-10-16 afternoon:
| run | game cost on-off | screens top/bottom | lag (idle) |
|---|---|---|---|
| LOCK0-tetris (control) | +1898 | FAIL 5 / FAIL 4 | PASS 20 = 20 |
| DEFER24-tetris | +1895 | FAIL 4 / FAIL 2 | PASS 20 = 20 |
| LOCK0-platinum (control) | +5140 | PASS / PASS | PASS 3 = 3 |
| DEFER24-platinum | +5360 | FAIL 1 / PASS | PASS 3 = 3 |
It barely fired: the module still ran full passes in Tetris's heavy stretch (3929-3939). Screens moved by about as much
as one state to the next does. Dropped as built; not worth more without a counter showing why the rule misses.

Next: then a masked-slice evaluation (yield points, lock only while
masked) on top of copy-once, measuring the IRQ hold-off per slice first.

## Approach A on copy-once (branch footprint2-lab: footprint-lab + stagefix 1432411)

Unlock-on-IRQ model (runner --dsc-unlock-on-irq): every IRQ entry clears the lockdown (what a stub at the game's IRQ
handler entry would do); our code relocks when it runs again outside IRQ mode (not in halt_entry). One binary and one
state per game; control = the same binary with the lockdown ignored (--dsc-lock-ignore, identical layout). CO3 = lock
3 ways (our code gets 1/4 of each cache), CO2 = lock 2 ways (1/2). MOD = oracle (lockdown applies to our refills only).

| run | game cost on-off | shadow I | shadow D | screens top/bottom | lag (idle) | sound cmds | stuck |
|---|---|---|---|---|---|---|---|
| CO3OFF-tetris | +1959 cycles = +0.17% | 1271 | 697 | FAIL (862 / FAIL (1213 | PASS (20 vs 20) | FAIL (1042 frames | 1 |
| CO3MOD-tetris | +856 cycles = +0.08% | 324 | 470 | FAIL (2 / PASS (3001 | PASS (20 vs 20) | PASS | 0 |
| CO3A2-tetris | +856 cycles = +0.08% | 324 | 470 | FAIL (2 / PASS (3001 | PASS (20 vs 20) | PASS | 0 |
| CO2OFF-tetris | +2007 cycles = +0.18% | 1278 | 697 | FAIL (805 / FAIL (1214 | PASS (20 vs 20) | FAIL (959 frames | 1 |
| CO2A-tetris | +1331 cycles = +0.12% | 683 | 569 | FAIL (3 / FAIL (4 | PASS (20 vs 20) | PASS | 1 |
| CO3OFF-kirby | +26639 cycles = +2.38% | 33965 | 1563 | PASS (3001 / PASS (3001 | PASS (58 vs 58) | PASS | 6 |
| CO3MOD-kirby | +2153 cycles = +0.19% | 4946 | 1291 | PASS (3001 / PASS (3001 | PASS (58 vs 58) | FAIL (4 frames | 2 |
| CO3A2-kirby | +10169 cycles = +0.91% | 12753 | 1274 | PASS (3001 / PASS (3001 | PASS (58 vs 58) | PASS | 8 |
| CO2OFF-kirby | +26667 cycles = +2.38% | 33999 | 1562 | PASS (3001 / PASS (3001 | PASS (58 vs 58) | PASS | 8 |
| CO2A-kirby | +20203 cycles = +1.80% | 18963 | 5194 | PASS (3001 / PASS (3001 | PASS (58 vs 58) | PASS | 6 |
| CO3OFF-platinum | +5604 cycles = +0.50% | 3322 | 2597 | PASS (3001 / PASS (3001 | PASS (3 vs 3) | FAIL (10 frames | 0 |
| CO3MOD-platinum | +1532 cycles = +0.14% | 1016 | 577 | PASS (3001 / PASS (3001 | PASS (3 vs 3) | FAIL (4 frames | 0 |
| CO3A2-platinum | +1532 cycles = +0.14% | 1016 | 577 | PASS (3001 / PASS (3001 | PASS (3 vs 3) | FAIL (4 frames | 0 |
| CO2OFF-platinum | +5782 cycles = +0.52% | 3328 | 2736 | FAIL (196 / FAIL (37 | FAIL (2 intervals differ, first frame 8065) | FAIL (125 frames | 0 |
| CO2A-platinum | +3018 cycles = +0.27% | 1849 | 1296 | FAIL (1 / PASS (3001 | PASS (3 vs 3) | FAIL (4 frames | 0 |
| CO3OFF-mkds | +10306 cycles = +0.92% | 8487 | 3435 | PASS (3001 / PASS (3001 | PASS (0 vs 0) | FAIL (120 frames | 0 |
| CO3MOD-mkds | +4588 cycles = +0.41% | 3191 | 2017 | PASS (3001 / PASS (3001 | PASS (0 vs 0) | FAIL (113 frames | 0 |
| CO3A2-mkds | +4553 cycles = +0.41% | 3168 | 2002 | PASS (3001 / PASS (3001 | PASS (0 vs 0) | FAIL (118 frames | 0 |
| CO2OFF-mkds | +10305 cycles = +0.92% | 8710 | 3390 | PASS (3001 / PASS (3001 | PASS (0 vs 0) | FAIL (23 frames | 0 |
| CO2A-mkds | +6303 cycles = +0.56% | 4957 | 2459 | PASS (3001 / PASS (3001 | PASS (0 vs 0) | FAIL (19 frames | 0 |

Reading:
- Safety: with A, game code never runs under our lockdown in IRQ mode, and outside it only in a model artefact (the
  "pass parked" exit of module_idle relocks before returning to the stock halt: Kirby 284 fetches/interval, MKDS 1;
  real relock points sit in the pass and avoid it). Controls show the exposure A removes: 13-24,700 game fetches per
  interval under the lockdown.
- Cost, CO3A vs control: Tetris -56%, Platinum -73%, MKDS -56%, Kirby -62% (oracle -92%: Kirby takes ~195 IRQs a
  frame and its handlers run unlocked). CO2A: Tetris -34%, Platinum -48%, MKDS -39%, Kirby -24%.
- Throughput: MKDS falls behind with 3 ways locked (4628 of 5010 evaluated, also under the oracle); with 2 ways it
  keeps up. Light sets keep up either way.
- The controls themselves diverged from stock in two runs (Tetris CO3OFF/CO2OFF: 800+ screen frames; Platinum
  CO2OFF from frame 8065, as the first broken A model did at 8064): timing knife-edges that today's cost tips and the
  locked runs did not. One sample each.

Next: unlock at the thread switch instead of IRQ entry (handlers run locked, bounded; only a switch to a game thread
unlocks); lockdown width chosen by backlog (3 ways when keeping up, 2 or 0 when behind).

## Unlock at the thread switch; adaptive width (2026-10-16 evening, footprint2-lab)

Runner models (melonDS 896d3c6f): --dsc-unlock-on-switch keeps the lockdown through IRQ handlers and clears it when
OSThreadInfo.current (found from the pointer at 0x02FFFFA0/0x027FFFA0) leaves the thread that set it, as a patch in the
SDK's thread switch would; our code relocks when it runs again outside IRQ mode. --dsc-unlock-irq-mask M: an IRQ entry
with IE & IF & M also clears it (a stub at the game's IRQ entry that tests the source). Module DSC_LOCK_ADAPT=3 (coad):
lock 3 ways while fewer than 3 frames wait, 2 ways below 12, none above. Every runner change replays CO3OFF-tetris
byte-identical. S = switch only, SV = switch + VBlank (mask 1), SH = switch + every source but HBlank (mask fffffffd).
COAD has its own control (COADOFF: same binary, lockdown ignored).

| game | control CO3OFF | A (IRQ entry) | S | SV | SH | oracle | COADOFF | COADSH |
|---|---|---|---|---|---|---|---|---|
| Tetris | +1959 | +856 | +848 | +850 | +856 | +856 | +2067 | +877 |
| Platinum | +5604 | +1532 | +1479 | +1478 | +1532 | +1532 | +5701 | +1669 |
| Kirby | +26639 | +10169 | +6926 | +6948 | +7323 | +2153 | +29078 | +8384 |
| MKDS | +10306 | +4553 (behind) | +45417 (behind, 33 dropped) | +45532 (same) | +4553 (behind) | +4588 (behind) | +10560 | +7363 (keeps up) |

Lag (idle) equal to the reference in every run. Screens: PASS or the 1-2 frame noise of the controls, except COAD
Tetris (9/8 against its control's 12/10).

Reading:
- Running every handler locked (S) is unsafe: MKDS has a ~21.6k-cycle IRQ handler (timer 1 or IPC, not VBlank: SV did
  not change it) that thrashes the one free D way when it lands in our pass: longest handler run 21.6k -> 140k cycles,
  +45k a frame, and the slower pass dropped 33 frames. The game's timing moved by 2 ms in those frames even though lag
  held.
- SH: only HBlank handlers run locked. An HBlank handler has to finish inside a line (~2,100 ARM9 cycles) or the game
  breaks on stock hardware, so its stretch under the lockdown is bounded by construction. Longest handler run equal to
  the control in all four games (MKDS 21600 -> 21656, Kirby 30874 -> 30956). Game code with the lockdown set: none
  outside IRQ mode; in IRQ mode only Kirby's HBlank handlers (119 a frame, 226 -> 239 cycles a run).
- Where a game takes no HBlank IRQs, SH is exactly A (Tetris, Platinum, MKDS: identical numbers). Kirby: A -62%, SH
  -73%; the rest of the gap to the oracle is its other IRQs (VBlank, IPC) unlocking.
- Adaptive width fixes MKDS throughput: COADSH evaluated 5010 of 5010 (fixed 3 ways: 4628, 382 behind at the end) at
  +7.4k against +10.6k, i.e. -30%. Light sets stay at 3 ways (Tetris/Platinum: same as fixed). Kirby COAD +8.4k vs +7.3k
  is the layout move plus the odd 2-way wake.
- The control divergence seen before (CO3OFF Tetris 800+ screen frames) is layout-specific: COADOFF Tetris, same
  module with a slightly different layout, has 12/10.

Shape for the real thing: lockdown base 3 (2 when behind, none when far behind) around the pass; a stub at the IRQ
vector that clears it unless the only pending source is HBlank; a patch in the SDK thread switch that clears it when
the incoming thread is not the idle thread; relock at our pass's resume points. The stub and the switch patch are new
hook points in the game (not in nds-bootstrap's current four).
