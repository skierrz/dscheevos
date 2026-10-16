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
