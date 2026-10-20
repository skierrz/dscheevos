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
  the control in all four games (MKDS 21600 -> 21656, Kirby 30874 -> 30956). Game code with the lockdown set outside
  IRQ mode: only the parked-pass model artefact noted above (MKDS 0.7 a frame, as under A); in IRQ mode only Kirby's HBlank handlers (119 a frame, 226 -> 239 cycles a run).
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

## Owner ruling and masked slices (2026-10-17)

Owner: no IRQ-entry stub, no thread-switch patch: the read-only rule stands (a write path into the game is a cheat
vector). That leaves lockdown only where no game code can run: with IRQs masked.

Masked-slice evaluation needs a unit of our work short enough to hold IRQs off for. The unit is one rc_client_do_frame
(no yield points inside rcheevos). Its length, co3 control, 600 intervals (ev9 at entry 02d87bb8 and the three return
sites; ARM7 timestamps x2 = ARM9 cycles at 67 MHz; one line = 2,130):

| game | calls | min | p50 | p90 | max |
|---|---|---|---|---|---|
| Platinum | 601 | 48k | 53k | 66k | 105k |
| Kirby | 600 | 82k | 94k | 115k | 450k |
| Tetris | 600 | 151k | 169k | 174k | 8.2M (one-off) |
| MKDS | 424 | 148k | 974k | 1.02M | 1.07M |

Wall time includes IRQs and other threads, but even the minimum (an uninterrupted call) is 23-71 lines. Holding IRQs
off for one call delays VBlank, timers and every HBlank (Kirby takes 119 a frame) by tens of lines: not viable. Getting
under a line needs yield points inside rcheevos' evaluation, which the rcheevos rule rules out.

Result: under the read-only rule, cache lockdown is closed beyond the masked capture (LOCKCAP, already covered by
copy-once). The cost has to come down by touching fewer lines (layout, data placement, less work per frame), not by
fencing them. The stub design (SH above: -62% to -73%, lag and longest handler equal to stock) stays here as evidence
only.

## Cache-set confinement (2026-10-17, in progress)

What-if (runner --whatif-module-isets/-dsets, -ibase): module memory uses only some cache sets, as a layout that puts our
code (and data) at chosen offsets in each way would. Read-only by construction: only where our own module sits.

Pre-registered before the quarter sweep's results (owner: don't overfit to a few games):
- Development games: Kirby, MKDS, Platinum, Tetris. Held out (never used to choose anything): Black, Phoenix Wright,
  Digimon World DS (behaviour gate), Zelda Spirit Tracks and Phantom Hourglass (gamecost.sh).
- Candidate: code-only confinement (data can't be confined: stack and rcheevos' heap blocks are contiguous and larger
  than a D quarter's 256-byte windows).
- Choice rule: the I quarter with the lowest mean of (cost / control cost) over the four development games; ties to
  the lower base. Then one held-out run per game with that quarter against its own control, reported whatever it shows.
- Accept only if no held-out game is worse than its control by more than run noise (~0.1% of a frame).

Results (co3 binary, its control = same binary with lockdown ignored; game cost on - off, cycles at 67 MHz):

What-if variants on the development games (Q = quarter of the sets: I 16 of 64, D 8 of 32):

| variant | Kirby | MKDS | Platinum | Tetris |
|---|---|---|---|---|
| control CO3OFF | +26.6k | +10.3k | +5.6k | +2.0k |
| v1: all our accesses folded, game lines we read too (not buildable) | +44.6k | +3.5k | +1.2k | +1.1k |
| v2: module memory, wake block/views line/stack top exempt | +29.1k | +5.5k | +4.1k | +1.5k |
| v3: all module memory, code and data | +44.3k | +3.6k | +2.0k | +1.0k |
| v4: module code only, sets 0-15 | +33.8k | +4.9k | +3.3k | +1.3k |
| v4, sets 16-31 | +93.0k | +4.9k | +3.8k | +0.8k |
| v4, sets 32-47 (chosen: mean ratio 0.46) | +4.7k | +5.8k | +3.3k | +1.0k |
| v4, sets 48-63 | +29.1k | +6.3k | +3.4k | +1.1k |

Every run kept up (all frames evaluated); lag equal to the reference throughout.

Held out, v4 at sets 32-47 against each game's own control:

| game | control | confined | change |
|---|---|---|---|
| Pokemon Black | +27.2k (I 14.7k) | +52.4k (I 45.0k) | x1.9 worse |
| Phantom Hourglass | +11.1k | +8.6k | -23% |
| Phoenix Wright | +3.3k | +2.0k | -37% |
| Digimon World DS | +2.2k | +1.3k | -42% |
| Spirit Tracks | +24.0k | +2.5k | not comparable: both runs diverge from the reference (lag 16 / 14 intervals, screens) |

Verdict by the pre-registered rule: rejected (Black is far worse than its control). Concentrating our code in 16 sets
turns the cost into a bet on whether those sets overlap the game's own hot code: Kirby ranges x0.18 to x3.5 by quarter
alone, and the quarter that wins on the development games doubles Black. Today's layout spreads our lines over every
set, which is costlier on average but has no cliff. A per-game quarter (a table keyed by ROM hash, chosen in melonDS)
would avoid the cliff only as far as the model's set-level behaviour matches the DSi, which nothing has checked; and it
needs a sweep per game. Not pursued.

Also seen: Spirit Tracks' control (today's footprint) already diverges from the reference in lag (16 intervals from
frame 9227), the same timing-tipping effect as the Tetris/Platinum controls earlier.

## Where Kirby's cost goes: the network pass at every wake (2026-10-17)

Runner --evict-attr (melonDS footprint2-lab): each module refill that evicts a game line remembers the module PC; the
game's later re-miss (shadow cache says it would have hit) charges its extra cycles to that PC. Attribution runs replay
the control byte-identical. Kirby (co3 control, +26.6k): charged 41.8k/interval, 96% I-side; rcheevos evaluation
~1-2k of it. The rest is dispatch code: idle_step_locked 9.9k, the halt entry 8.3k, module_idle 7.7k,
dsc_work_pending 4.0k, net_service + link_paused 4.4k. Six I sets (8, 19, 20, 24, 26, 27) carry 88%: a dozen of our
lines re-missed ~58 times an interval each, against Kirby's per-line handler code.

Why: over the full window idle_step_locked runs 62 times a frame (median 81, ~ every idle wake) while
rc_client_do_frame runs once. From frame 3855 on, H.requests = 1 and H.net_busy = 1: a request waits for an answer and
the network is busy, and work_wanted() asks for a full pass at every wake while either is set ("so an unlock goes out
at once"). Under --dsc-nonet the answer never comes, which is the DSi with no Wi-Fi or RA unreachable. Kirby split by
phase (gamecost.py on each part): no request pending +2.9k (0.26%), request pending +34.3k (3.07%).

Every earlier Kirby number in this lab includes that phase; comparisons between variants still hold (same phase in
both), the absolute level doesn't.

Fix (module, read-only by construction): DSC_NET_PER_FRAME drops H.requests and H.net_busy from work_wanted(): a busy
network or a waiting request gets the frame's first pass (idle_step_locked services it on every pass it runs), not one
per wake. Each build with its own state (a state holds the module image; a first try loaded the control's state for
both and measured the control twice):

| game | control (npf0) | DSC_NET_PER_FRAME | change |
|---|---|---|---|
| Kirby | +15.9k | +2.4k | -85% |
| Pokemon Black | +26.9k | +5.2k | -81% |
| MKDS | +11.1k | +11.0k | = |
| Platinum | +5.6k | +5.3k | = |
| Tetris | +2.0k | +1.9k | = (screens 771/1218 -> 5/4) |
| Phoenix Wright | +3.3k | +3.2k | = |
| Digimon World DS | +2.1k | +2.2k | = |
| Phantom Hourglass | +10.9k | +10.6k | = |

All frames evaluated, lag equal to the reference in every run. Network items of the suite (wifidrop with 4 unlocks
sent and 2 rejoins, offline journal 1+2, hardcore online, spectator) PASS on both builds; award spacing identical
(10/10/15 s), last TLS handshake 8,933 vs 8,834 ms (+1%: up to a frame per round trip).

## The same fix on perf's code (branch netpace off perf 59e9978b, not committed; 2026-10-17)

Two versions, patches in docs/lab/patches/: netpace-perframe (H.requests and H.net_busy out of work_wanted, as
DSC_NET_PER_FRAME above) and netpace-linkgated (H.requests out; H.net_busy = net_busy() && link_up()). Both shrink
module_idle and idle_step_locked (work_wanted is inlined in both), moving the wake block by 0x18 and hot code by
0x10-0x18: the hot-layout firewall fails. "pad" builds add exactly that gap in module.ld/hot.ld so every hot symbol keeps
perf's address (layout identical to hot-layout.ref; logic change only).

| | perf | perframe pad / unpadded | linkgated pad / unpadded |
|---|---|---|---|
| online (gamecost.sh, mock server), Kirby | +17.6k | +28.0k / +28.2k | +28.0k / +28.0k |
| online, MKDS | +17.0k | +15.1k / +27.6k (*) | +14.7k / +19.2k (*) |
| online, Pokemon Black | +34.4k | +32.5k / +30.3k | +32.4k / +30.6k |
| no network (behaviour gate), Kirby | +37.9k | - / +26.4k | +43.8k / +37.5k |
| no network, Pokemon Black | +36.7k | - / +29.7k | +36.2k / +36.0k |

(*) shadow unchanged and game CPI down: the game took a different path (more game instructions), not our cache use.

Reading:
- perf copies the snapshot at every wake (copy-once is stagefix phase 2, not on perf): that D-side copy (wake.core,
  ~21.7k charged on Kirby online) is the same in every row, so the network pass is a smaller share here than in the lab.
- Online Kirby is worse with the change even at perf's exact layout. Attribution (online, identical layout): I-side
  charged 8.7k -> 15.4k, from module_idle (0.8k -> 5.9k) and the halt entry (0.3k -> 2.1k). Same module time per
  interval, same network work. In perf, online, the pass at every wake is cheaper for Kirby than the light wakes that
  replace it: what Kirby pays is which of our lines run between its per-line handler runs, not how much we run.
- The link-gated version keeps per-wake passes while the link is up, yet online Kirby is unchanged from the broad
  version (so H.requests alone, link up, drives perf's per-wake passes online), and offline it loses the gain.
- Verdict: neither version goes to perf. On Kirby the cost of the per-wake path is a cache-set collision effect that
  doing less work can make worse; the lab's -85% (copy-once base, offline) does not carry over to perf online. The
  dependable result is the diagnosis: with no answer coming, a pass at every wake costs Kirby/Black ~3% of a frame,
  and any fix has to be judged online and offline, at a fixed layout, on more than Kirby.

## On copy-once: the network fix holds (2026-10-18; branch copyonce-net = perf + stagefix 1432411 + 39714f8a)

Same eight games, copy-once base (perf merged with stagefix phase 2) vs the fix. Online = gamecost.sh with the mock
server (last 1500 intervals of each census script); offline = behaviour gate, --dsc-nonet, each build its own state.
"pad" = the fix with the base's exact hot layout; the committed build (unpadded, hot-accept) was measured too.

| game | online base | online fix (pad / committed) | offline base | offline fix (pad / committed) |
|---|---|---|---|---|
| Kirby | +2.8k | +2.7k / +2.4k | +15.9k | +2.8k / +2.5k |
| Pokemon Black | +5.4k | +5.4k / +5.3k | +26.8k | +5.3k / +5.1k |
| MKDS (online at equal work) | +12.0k | +6.8k / +12.2k | +11.1k | +10.2k / - |
| Platinum (online at equal work) | +23.0k | +23.9k / +23.1k | +5.6k | +5.4k / - |
| Tetris | drift (instructions +34% in one run) | - | +2.1k | +1.9k / - |
| Phoenix Wright | +2.4k | +2.4k / +2.5k | +3.3k | +3.2k / - |
| Digimon World DS | +2.2k | +2.1k / +2.2k | +2.1k | +2.1k / - |
| Phantom Hourglass | +10.6k | +10.6k / +10.7k | +10.9k | +10.5k / - |

Copy-once alone takes online Kirby from +17.6k (perf) to +2.8k and Black from +34.4k to +5.4k; the network fix then
removes the offline penalty. The link-gated variant did nothing here (CNLG rows: Kirby +15.7k, Black +26.0k offline).
Committed 39714f8a on copyonce-net (pushed); suite network items PASS on that exact binary. Not a perf merge: stagefix
phase 2 still waits on its owner gate (real unlocks).

## What the costly games say about the whole catalogue (2026-10-18, copyonce-net 39714f8a, offline profiles)

Footprint (lines our code touches per stretch of module code between two game instructions) and eviction attribution:

| game | stretches/interval light / pass | I lines per light / per pass | own D lines per pass | game cost | charged I / D |
|---|---|---|---|---|---|
| Kirby | 321 / 32 | 21 / 29 | 9 | +2.4k | 5.6k / 1.3k |
| Pokemon Black | 226 / 83 | 21 / 24 | 9 | +5.1k | 7.6k / 2.6k |
| MKDS | 1.5 / 4.3 | 11 / 189 | 33 | +10.8k | 14.8k / 3.3k |
| Phantom Hourglass | 7.3 / 1.9 | 20 / 294 | 42 | +10.6k | 13.5k / 4.2k |
| Pokemon Platinum | 1.4 / 1.1 | 9 / 471 | 1,247 | +5.5k | 6.5k / 4.2k |

(I-cache 256 lines, D-cache 128 lines.)

Two classes:
1. Long-pass games (MKDS, Phantom Hourglass, Platinum): one evaluation pass touches more lines than the cache holds,
   so it replaces the game's whole working set; the game then pays about one full refill (~10k cycles) every time it
   resumes after such a pass. Cost ~ (resumes after a pass) x min(our footprint, cache), nearly independent of how much
   work the pass does. Levers: fewer resumes inside a pass (shorter passes in wall time), a pass footprint below the
   cache size (only for light sets), never two big passes a frame.
2. IRQ-heavy games (Kirby, Black: HBlank/timer IRQs, 225-320 idle wakes a frame): ~21 I lines per light wake (halt
   entry, module_idle, the capture and drain checks that run even when the frame's capture is done). Lever: a minimal
   fast path for "captured, nothing waiting: halt" in a handful of lines. Every game with per-line or timer IRQs gains.
3. Network robustness (all games, whenever the server can't be reached): the request timeout didn't cover DNS, connect
   or the TLS handshake (only C_READY), so a silent server held a request (and the queue behind it) until lwIP gave up
   on TCP, with a handshake step (~23 allocations) every pass. Fix in progress: fail after 25 s without progress.

## Stall timeout (kept) and fast wake (dropped), copyonce-net (2026-10-18)

| | online Kirby | online Black | online Platinum (eq. work) | online MKDS (eq. work) | offline Kirby | offline Black | offline MKDS |
|---|---|---|---|---|---|---|---|
| 39714f8a (network once a frame) | +2.4k | +5.3k | +23.1k | +12.2k | +2.4k | +5.1k | +10.8k |
| + stall timeout (66a50d8c) | +2.5k | +4.7k | +23.1k | +12.3k | +2.5k | +4.6k | +10.9k |
| + fast wake (patches/fastwake.patch) | +2.7k | +5.8k | +25.1k | +11.7k | +2.8k | +5.4k | +9.9k |

Fast wake (halt at once when the frame is captured, nothing waits and no VBlank came: one uncached read and the H
line) did what it was built for: Kirby's light wakes 21.4 -> 9.1 I lines and 2.8 -> 1.0 own D lines each. The game
cost didn't follow (Kirby, Black, Platinum up; MKDS, Tetris down; held-out Phoenix/Digimon/PH equal). Below ~20 lines a
wake, the light path no longer sets the cost; what is left is the pass. Dropped (patch kept, with its core test for
dsc_frame_done).

Stall timeout: committed 66a50d8c (correctness first: a silent server no longer holds the request queue), cost equal
or lower; network suite items PASS.

## evalfast-p2 on copyonce-net (2026-10-18, branch copyonce-net-ef, not committed)

Merged evalfast-p2 (017e0d30 + rcheevos e612011; its WAKE_COLOUR 0x440) onto copyonce-net 66a50d8c. Expected the long-pass
games to get cheaper (shorter passes, fewer resumes). They didn't:

| game | copyonce-net | + evalfast | |
|---|---|---|---|
| Platinum late window (online, eq. work) | +23.1k | +39.6k | I-cache 5.9k -> 27.5k, same 371 resumes/interval |
| Platinum early window (offline) | +5.5k | +5.7k | |
| MKDS online eq. / offline | +12.3k / +10.9k | +11.5k / +10.6k | |
| Kirby online eq. / offline | +5.0k / +2.5k | +5.4k / +3.2k | |
| Black online eq. / offline | +7.8k / +4.6k | +7.0k / +4.6k | |
| Phantom Hourglass, Phoenix, Digimon | | equal | |

All frames evaluated in every run. The Platinum loss is the I-side layout lottery (same resumes, twice the cost each),
not evalfast's logic; it still rules evalfast out on this base for now.

Also found (Platinum late window, any build): module time with IRQs masked 158k cycles/interval (the copy), 20.6k of it
with a game IRQ waiting; longest single hold-off p50 2.9k, max 5.9k cycles (1.5-3 lines, 486 IRQ runs/interval). The
behaviour gate's early window sees 4 cycles. Late-game windows need behaviour checks, not only cost.

## Catalogue run 1 (2026-10-18, tests/perf/catalogue.sh; 64 census games, 61 measured)

Metric: shadow I+D (cycles the game lost to our cache use; raw and equal-work cost also move when a game's own logic
diverges between runs). Window: last 1500 intervals of each census script (gameplay games) or of 3000 generic-menu
intervals. Online (mock server). Builds: control = copyonce-net 66a50d8c; unc-all = the same with every set's heap
uncached (DSCM_HEAP_UNCACHED_MAX 4 MB).

| class | games | median | p90 | max | over 1% | over 0.1% | frames dropped (games) |
|---|---|---|---|---|---|---|---|
| oshalt (SDK idle halt) | 50 | 0.30% | 1.18% | 5.51% | 6 | 48 | 12 |
| bios (BIOS wait) | 9 | 0.00% (no evaluation) | | | 0 | 0 | 1 |
| busy | 2 | 0.00% (no evaluation) | | | 0 | 0 | 0 |

Not measured: Animal Crossing, SoulSilver, TWEWY (sets too big for the module).
Set kinds: 31 games have a set matched by ROM hash ("real"); 28 have an RA set only by title (our dump's hash isn't
listed), measured with the synthetic typical set (provisional).

Over 1% (control): Mario Party DS 5.51% (synthetic; D 52k), Platinum 2.26%, Mario Hoops 1.94% (synthetic), Legendary
Starfy 1.77%, Cory in the House 1.18%, MKDS 1.16%. Near: Phantom Hourglass 0.99%, Drawn to Life 2 0.95% (synth),
Super Scribblenauts 0.94% (synth), Kirby Mass Attack 0.90% (synth).

unc-all changes only the sets over 160 KB: Platinum 2.26% -> 0.79%, Rhythm Heaven 0.76% -> 0.38%, NSMB 0.27% -> 0.24%,
Digimon 0.20% -> 0.17% but 26 frames dropped during boot (control 0; uncached evaluation 46% of each frame there, up to
91%). Everything else identical. A size threshold can't pick: Platinum (304 KB) keeps up uncached, Digimon (194 KB)
doesn't.

Frames dropped in the CONTROL (whole run, boot included): SM64DS 3357, RE 2449, MvDK 1457, Mario & Luigi BIS 660,
Layton 440, LEGO Star Wars 377, MvDK2 156, Nine Hours 94, PMD Blue 81, Drawn to Life SpongeBob 68, Club Penguin 55,
Mario Hoops 29, Yoshi's Island 2. Achievement frames never evaluated: a correctness gap, separate from cost.

Attribution (online): Cory in the House = long-pass class (16.8 resumes x 790 cycles, evaluation code). Legendary Starfy:
IRQ-heavy (420 IRQ runs, 222 resumes) plus ~2k charged to mbedtls bignum: the module's first connection does a full
TLS handshake (the launcher's TLS session isn't handed over); one per game session (ev9: one ecdh_calc_secret in the
run), here inside the window. Lever (all games): hand the launcher's session ticket to the module so the first
connection resumes.

## Catalogue run 2: evalfast on copyonce-net (2026-10-18)

Build: copyonce-net + evalfast-p2 (lab branch copyonce-net-ef 959db333; WAKE_COLOUR 0x440, TEXT_COLOUR 0x500).
- Frames dropped (whole run): SM64DS 3357 -> 8, LEGO Star Wars 377 -> 0, M&L BIS 660 -> 79, MvDK2 156 -> 53, PMD Blue
  81 -> 4, Drawn to Life SpongeBob 68 -> 0, Club Penguin 55 -> 0, Mario Hoops 29 -> 0, Yoshi 2 -> 0. Games with drops
  13 -> 8. The rest (Layton, MvDK, RE, Nine Hours) unchanged.
- Shadow I+D, 50 oshalt games: median change +94 cycles (8 better, 17 worse), p90 1.18% -> 1.05%, max 5.51% -> 4.19%.
  Swings both ways: Mario Party DS 5.51 -> 3.11, Cory 1.18 -> 0.93, Rhythm 0.76 -> 0.50; Platinum 2.26 -> 4.19, Starfy
  1.77 -> 2.87, Mario Hoops 1.94 -> 3.12. The I-side layout lottery: every code change reshuffles which game lines
  ours collide with.
- The shipped placement knobs (WAKE_COLOUR, TEXT_COLOUR) were chosen on Kirby alone. Next: choose them on the catalogue.

Pre-registered placement sweep (before results): dev subset of 10 games (Mario Party DS, Platinum, Mario Hoops,
Legendary Starfy, Cory, MKDS, Phantom Hourglass, Super Scribblenauts, Rhythm Heaven, Kirby), WAKE_COLOUR in
0x040..0x740 step 0x100 (0x440 = shipped, from run 2), TEXT_COLOUR 0x500. Choice: lowest mean shadow over the subset,
ties to the lower max. Validation: the chosen colour on the full catalogue vs 0x440; judged on the 51 games outside
the subset.

## TLS session handover (branch tls-resume c601a121, off perf)

The launcher resumes its own TLS session across its requests and hands it to the module (flag DSCM_BOOT_TLS_SESSION,
session at the end of the session file then its length; dscm_boot_t unchanged because nds-bootstrap checks its size).
The module's first connection then resumes: last TLS handshake 166 ms (before 6.4-8.8 s); no full key exchange in the
game's first seconds. Suite network items PASS. Official-only launcher: branch official-only 15179b3b.

Evalfast + every set's heap uncached (build-efunc): Digimon 0 frames dropped from boot (uncached without evalfast: 26;
eval_lines 768 vs 1583), game cost +2.0k (control +2.1k). Candidate catalogue configuration: evalfast + all heaps
uncached + the sweep's placement; queued on the full catalogue against copyonce-net.

## Placement sweep and catalogue run 3 (2026-10-18)

Sweep (10-game dev subset, evalfast on copyonce-net, TEXT_COLOUR 0x500; mean / max shadow I+D, cycles): 0x140
17.6k / 34.2k, 0x740 19.0k / 45.0k, 0x540 20.0k / 37.8k, 0x440 (shipped) 20.5k / 47.0k, 0x040 21.4k / 42.8k, 0x340
21.7k / 50.6k, 0x240 25.1k / 66.7k, 0x640 25.6k / 54.6k. Chosen by the pre-registered rule: WAKE_COLOUR 0x140.
Builds (../dscheevos-conet3 959db333): build-efw140 (evalfast, 0x140), build-efuncw140 (the same + every set's heap
uncached, DSCM_HEAP_UNCACHED_MAX 4 MB). Both on the full catalogue (tags efw140, efuncw140).

Validation, efw140 vs ef (0x440) on the 51 held-out games (shadow I+D):

| | median | p90 | max | over 1% | over 0.1% | games with frames dropped |
|---|---|---|---|---|---|---|
| ef (0x440) | 2547 (0.23%) | 7576 (0.68%) | 9578 (0.86%) | 0 | 38 | 8 |
| efw140 (0x140) | 2528 (0.23%) | 7681 (0.69%) | 9581 (0.86%) | 0 | 38 | 9 (Drawn to Life SpongeBob 1 frame) |

Read literally, the pre-registered rule ("not worse" on median, p90, max, over-1%) FAILS: p90 +105 cycles and max +3
cycles (0.009% and 0.0003% of a frame). Both are an order of magnitude below the run noise this lab has used before
(~0.1% of a frame); no held-out game moved more than 0.05%. What the validation does show: the held-out games
are insensitive to WAKE_COLOUR (6 better, 6 worse, median change 0). Every game over 1% is in the dev subset, so the
held-out set can neither confirm nor refute the placement gain; it only shows 0x140 costs the other games nothing
beyond noise. Not a generalisation result. Accepting it is the owner's call (the rule had no tolerance written down).

Candidate stack, efuncw140 (evalfast + all heaps uncached + 0x140) vs control ctl-copyonce-net, all 61 games:

| | median | p90 | max | over 1% | over 0.1% | games with frames dropped (total frames) |
|---|---|---|---|---|---|---|
| control | 0.25% | 0.99% | 5.51% | 6 | 48 | 13 (9225) |
| candidate | 0.26% | 0.91% | 3.05% | 4 | 48 | 9 (4569) |
| oshalt only (50): control / candidate | 0.30% / 0.32% | 1.18% / 0.99% | 5.51% / 3.05% | 6 / 4 | | |

- Over 1%, control: Mario Party DS 5.51 (synth), Platinum 2.26, Mario Hoops 1.94 (synth), Starfy 1.77, Cory 1.18,
  MKDS 1.16. Candidate: Mario Party DS 3.05 (synth), Mario Hoops 2.97 (synth), Starfy 2.43, MKDS 1.09.
- Better: Mario Party -2.45, Platinum -1.49 (2.26 -> 0.78; uncached heap, efw140 alone 2.23), Kirby Mass Attack, Drawn
  to Life 2, Cory, Rhythm ~-0.25 each. Worse: Mario Hoops +1.03, Starfy +0.66, WarioWare DIY +0.17, WarioWare
  Touched +0.12, 20 more by under 0.1%. 17 better, 24 worse, median change 0.
- Uncached heap on top of efw140 changes only Platinum (-1.45) and MKDS (-0.06); everything else within 0.01%.
  Digimon: 0 frames dropped (evalfast keeps up uncached, as in the single-game check).
- Frames dropped (whole run): SM64DS 3357 -> 10, M&L BIS 660 -> 78, LEGO SW 377 -> 0, MvDK2 156 -> 51, PMD Blue 81 -> 10,
  SpongeBob 68 -> 1, Club Penguin 55 -> 0, Mario Hoops 29 -> 0, Yoshi 2 -> 0. Unchanged by every build so far: RE
  ~2440, MvDK ~1450, Layton ~435, Nine Hours ~95 (not evalfast-bound; next to look at).
- Lag (game-made, window and whole run, off vs on): no build adds lag; differences are +-1-3 intervals except games
  whose own logic diverges between runs (NSMB, Tetris, Animal Crossing), same in the control. results.tsv's lag_on was
  never filled (gamecost prints "on  (" with two spaces); fixed in catalogue.sh, read from the logs for this run.

Verdict: the candidate is better than the control on max, p90, over-1% count and frames dropped, equal on median
(+0.01%). It is still a lottery per game: two of the four games left over 1% (Mario Hoops, Starfy) are worse than
the control, and the hard goal (every game under 1%) is not met. Next: --evict-attr on Mario Party DS, Mario Hoops,
Starfy, MKDS; the 28 title-only sets are still synthetic (Mario Party, Mario Hoops among them), so their numbers are
provisional until the official sets are measured.

## Official sets for the title-matched games (2026-10-18, owner OK to fetch with the RA token)

Fetched r=achievementsets by game id with f=3 (official only, as the official-only launcher asks) for the 29 games
measured with the synthetic set: 25 sets (all core only, 0 Flags 5), stored as private/sets/sets-<id>-title.json.
4 have no RA game ("Unknown game"): Club Penguin Herbert's Revenge, Clubhouse Games, Code Lyoko, Drawn to Life: The
Next Chapter; they stay synthetic. The 31 hash-matched set files do contain Flags 5 entries, but rc_client never
loads unofficial achievements, so every measurement here is official-only. The 25 games were re-run on the control
(copyonce-net build-st, df05fefeb9d1) and the candidate (build-efuncw140); tags ctl-official, efuncw140-official,
merged into the full runs as ctl-merged / efuncw140-merged.

Mario Party DS can't be measured with its official set: the session load before the game's first OS_Halt takes
7.1G cycles (~106 s at 67 MHz; synthetic set 0.25G, ~3.7 s), so the game starts at frame 8963 and only 36 intervals
of the 9000-frame run are in the window. The set has one achievement ("Wheel of Fortune", official) of 2311 conditions
(50.8 KB of MemAddr, a long AddSource chain on 9 addresses); the next largest is 735 bytes. Same in both builds. A
~2-minute stall at boot is a user-facing problem in its own right, separate from game cost. Next: where the load time
goes (rcheevos parse vs our session load), on the host first.

Synthetic vs official, same build: the synthetic set was not "typical" for these games. Control shadow, synthetic ->
official: Mario Hoops 1.94 -> 2.44, Super Scribblenauts 0.94 -> 0.57, Kirby Mass Attack 0.90 -> 0.59, SpongeBob 0.75 ->
0.38, PMD Sky 0.23 -> 0.46, SM64DS 0.16 -> 0.31. Frames dropped in the control vanish with the official sets: SM64DS
3357 -> 0, M&L BIS 660 -> 150, LEGO SW 377 -> 0, MvDK2 156 -> 0, PMD Blue 81 -> 0, SpongeBob 68 -> 0, Club Penguin 55 -> 0,
Mario Hoops 29 -> 0, Yoshi 2 -> 0. So run 2's "evalfast fixes dropped frames" was mostly about the synthetic set.

Candidate vs control with official sets (60 games: 56 real sets, 4 synthetic, Mario Party DS not measurable):

| | median | p90 | max | over 1% | over 0.1% | games with frames dropped (total) |
|---|---|---|---|---|---|---|
| control | 0.27% | 0.99% | 2.44% | 5 | 46 | 5 (4590) |
| candidate | 0.27% | 0.78% | 2.43% | 2 | 46 | 4 (4419) |
| oshalt only (49): control / candidate | 0.31% / 0.32% | 1.16% / 0.78% | 2.44% / 2.43% | 5 / 2 | | |

- Over 1%, control: Mario Hoops 2.44, Platinum 2.26, Starfy 1.77, Cory 1.18, MKDS 1.16. Candidate: Starfy 2.43, MKDS 1.09.
- Better: Mario Hoops -1.82 (2.44 -> 0.62 with its official set; with the synthetic set it went 1.94 -> 2.97: the I-side
  lottery depends on the set too), Platinum -1.49, Drawn to Life 2 / Cory / Rhythm -0.24, Scribblenauts -0.14, PMD Sky
  -0.10. Worse: Starfy +0.66, Kirby +0.08, 12 more under 0.06. 24 better, 14 worse, median change 0.
- Frames dropped: M&L BIS 150 -> 0; RE ~2440, MvDK ~1450, Layton ~435, Nine Hours ~95 in both (not ours to fix by
  evaluation speed; next to look at).
- Lag: unchanged (window on-off sums -25 / -89, whole run +197 / +50, all within the games' own run-to-run spread).

Verdict with official sets: the candidate is better on p90 and over-1% count (5 -> 2), equal on median and max,
fewer frames dropped. Still not every game under 1%: Starfy 2.43% (worse than control), MKDS 1.09%, and Mario Party DS
unmeasured.

Owner ruling (2026-10-18): WAKE_COLOUR 0x140 accepted. The held-out differences (p90 +105 cycles, max +3) are within
run noise; future pre-registrations state their tolerance (default: run noise, ~0.1% of a frame).

## Mario Party DS session load: rcheevos' memref deduplication (2026-10-18)

Cause: rcheevos' load, quadratic in one achievement's length. "Wheel of Fortune" is 2,311 conditions, a SubSource /
AddSource chain repeating `0xP154f55` / `0xL154f55` (9 addresses); each step is a modified memref whose parent is the
step before (~2,300 of them, depth 0 to ~2,300). Every new memref is deduplicated against the ones already made:
- Shared list (the game's, indexed since e01c29c): the index hash follows modified memrefs 2 levels deep, so the steps
  of the chain look alike and share a few hashes; each new step probes past all earlier ones.
- Sizing pass: rc_client parses every definition twice; the first pass dedups into a preparse scratch list with no
  index, a linear scan over the definition's own memrefs.
Not ours: the module's heap wraps, the core's chain compiler and the rest of the session load are linear (host,
realsets harness built natively: core load 27.6 ms vs rc_client alone 23-28 ms).

How it was found: host rc_client load of each official title-matched set vs that game's "module before it" in
ctl-official. Every game sits at 170-250M DS cycles per host millisecond (cached and uncached heaps alike: sets over 160 KB run cached); Mario Party
was 23.4 ms on the host (others <=5 ms for sets up to 200 KB) and 304M cycles/ms. `sample` on the host: the time was
in rc_alloc_modified_memref / rc_operands_are_equal / rc_memrefs_index_insert under rc_operand_addsource.

Fix (rcheevos fork, branch dscheevos-683197d-loadfix, two commits on 754965e; dscheevos branch mp-loadfix off perf
bumps the submodule; docs/rcheevos-pin.md has the patch notes):
1. c61a484: mix a modified memref's `depth` into the index hash (depth is compared by rc_operands_are_equal, so
   matching entries still share a hash).
2. 84d26e7: preparse lists get the same index after 64 entries; rc_destroy_preparse_state frees it.

| | host rc_client load (M5) | melonDS: module before the game | game starts (frame of first OS_Halt; off 1552) |
|---|---|---|---|
| before | 23.5 ms | 7,111M cycles (106 s) | 8963 |
| depth hash only | 10.9 ms | - | - |
| both | 3.7 ms | 700M cycles (10.5 s) | 3242 |

Mario Party's load is now in line with its size (host 0.030 ms/KB, SM64DS 0.025; on the DS 0.70G vs SM64DS 1.12G,
PMD Blue 1.01G). Other sets on the host: MvDK Mini-Land Mayhem 10.6 -> 4.2 ms, Layton 6.2 -> 4.1, Platinum 8.8 -> 7.0,
SoulSilver 23.1 -> 18.3, none slower beyond noise. The module (control copyonce-net build-st recipe, reproduced
byte-identical: df05fefeb9d1; with the patch bede937b3e6d) grows by 104 bytes of code; hot layout unchanged.

Game cost, now measurable (control + patch, tag ctl-loadfix): Mario Party DS shadow I+D 1.09% of a frame (raw +0.72%,
equal work +0.93%; I 7.7k / D 4.4k cycles), 0 frames dropped, lag 5 -> 5. So it is over 1% on the control, like
Starfy and MKDS, and needs a candidate run (the full stack) like the others.

Identical results (CLAUDE.md rule):
- tests/difftest/loadcmp.sh (new, branch mp-loadfix): loads each set with stock 683197d and the patched tree and
  compares what the load built (memref lists, modified memrefs' operands, every condition's operands as list indexes):
  all 76 set files identical (also identical to the previous pin 754965e). Sensitivity: a mutant that skips dedup in
  indexed preparse lists differs on 6 sets.
- run.sh (inert, 10,000 fuzzed frames x 8 scripts x 2 session replies x hardcore on/off) and core.sh (2,000 frames,
  plain / hand-over / no flags) on the 16 sets with real RAM dumps, patched tree 84d26e7 vs stock 683197d: 16/16 sets,
  512 run.sh runs and 1,152 core.sh runs, zero differences. The change-driven / changed-memory variants were not rerun
  (the patch touches only the load; loadcmp shows the loaded structures are identical).

What's left: the load still stalls the boot ~10 s for Mario Party and up to ~17 s for SM64DS (most games 1-5 s), on
top of the launcher's own wait. It tracks the host rc_client load at about 200M ARM9 cycles (3 s) per host millisecond, so it is mostly rcheevos' parse (not split per game on the DS).
Separate question; options in the hand-back.

## The candidate as a branch: `stack` (2026-10-19)

Branch `stack` e668b634 (pushed, not merged), off `mp-loadfix` (perf + the Mario Party DS load fix), with copyonce-net
(stagefix phase 2 + network wake fix + stall timeout) and evalfast-p2 merged, and two new defaults: WAKE_COLOUR 0x140
and DSCM_HEAP_UNCACHED_MAX 4 MB (every set's heap uncached). rcheevos: fork branch `dscheevos-683197d-stack` 1a0269a =
the direct-read skip e612011 with the load fix cherry-picked after it (58027f6, 1a0269a).

- Host tests: core 33,381 checks, module 54 + 33, 0 failed. hotcheck: all 161 hot symbols at the same addresses as
  build-efuncw140 (the measured candidate); hot-layout.ref regenerated.
- rcheevos gate, the shipped tree as one vs stock 683197d: loadcmp 76 set files 0 differ; run.sh CD=1 CHANGES=1 on the
  16 real-dump sets 2,560 runs and core.sh 1,152 runs, zero differences.
- Catalogue (tag stack, official sets where fetched; 61 games, 60 comparable with efuncw140-merged + Mario Party DS):

| | median | p90 | max | over 1% | games with frames dropped |
|---|---|---|---|---|---|
| control (ctl-merged) | 0.27% | 0.99% | 2.44% | 5 | 5 |
| candidate efuncw140-merged | 0.27% | 0.78% | 2.43% | 2 | 4 |
| `stack` | 0.27% | 0.80% | 2.66% | 2 (Starfy 2.66, MKDS 1.11) | 4 (RE, MvDK, Layton, Nine Hours) |

- `stack` vs efuncw140-merged: 58 games within 0.04%; Starfy 2.43 -> 2.66 (I 18.3k -> 20.4k, D 8.9k -> 9.4k), Nintendogs
  0.78 -> 0.90 (I +0.7k, D +0.7k). Hot code identical, so this is the lottery outside it: the load fix's bigger load
  functions shift the non-hot code after them, and its preparse index changes heap addresses. Not evaluation work.
- Mario Party DS measurable with its official set: 0.98% (I 8.1k, D 2.9k), game from frame 3241 (no module 1552;
  session load 0.70G cycles, ~10.5 s), 0 frames dropped.
- Lag: catalogue.sh's lag_on now filled; window lag on - off summed over all games 0, whole run +14 intervals.

Over 1% on `stack`: Starfy, MKDS. Mario Party DS just under. Next: --evict-attr on Starfy (IRQ-heavy class) and MKDS.

## Boot delay across the catalogue, before and after the load fix (2026-10-19)

How much later the game's first OS_Halt comes with DScheevos than without (melonDS, same runs as the catalogue:
ctl-merged = control, `stack` = with the load fix; frames at 59.83 Hz). Emulator numbers, not hardware.

- 64 games: none slower; 44 within 2 frames. Median change 0.
- Big wins: Mario Party DS 124 -> 28 s, MvDK and MvDK Mini-Land Mayhem 62 -> 27 s, Platinum 41 -> 32 s, Rhythm
  Heaven 40 -> 34 s, Layton 32 -> 22 s, M&L Partners in Time 22 -> 17 s, Cooking Mama 18 -> 14 s.
- Floor 7-13 s for every game; still 26-34 s for Rhythm Heaven, Platinum, Digimon, Mario Party DS, SM64DS and both
  MvDK. The rest scales with rcheevos' parse: the next lever for boot time.
- Host (Apple M5, upstream develop dfdc64e vs the same series ported, rc_runtime activation of all 76 sets):
  948.5 -> 201.7 ms total, Animal Crossing 301 -> 53 ms, HG/SS 153 -> 16 ms, Rhythm Heaven 66 -> 6.5 ms; none slower.
  The port is fork branch upstream-memref-index; loadcmp vs develop 76/76 identical; write-up in the planning
  repo, research/upstream/rcheevos-memref-index.md. Not sent upstream (owner, 2026-10-19: no PRs or issues on
  RetroAchievements/rcheevos).

## Why RE, MvDK, Layton and Nine Hours drop frames in every build (2026-10-19)

"dropped" = frames never evaluated (core commit(): the backlog passed MAX_SLOTS = 1024 frames, ~17 s, or no copy to
repeat). Under docs/dropped-frames.md each gap turns a hardcore session casual, so this is worse than cost.
The module captures only at the game's first idle halt after a VBlank (read-only rule: nothing in the IRQ path), so a
frame with no halt is evaluated later as a repeat of the last copy, and a long enough no-halt stretch overflows.

- Longest run of frames with no game halt (catalogue run `stack`, prof csv; achievements off / on): MvDK 2,261 /
  2,364 (~38 s), Layton 1,455 / 1,456 (~24 s), Nine Hours 174 / 458, RE 76 / 77 but RE waits in the BIOS (bios
  class): the module's VBlank work didn't run for 5,678 frames in a row. The no-halt stretches are the games' own
  (same lag off and on): MvDK 2,563 lag intervals both, Layton 1,685 both, Nine Hours 1,425 / 1,428.
- Nine Hours timeline (launcher run with a RAM dump every 200 frames; 60 dropped this run): evaluation stalls at
  ~1,800 frames from frame ~3,800 to ~4,800 (game frames ~1,700-2,800, a stretch where the game lags 80-100 of every
  100 frames) while frames queue; the backlog passes 1,024 when the game recovers; the 60 drops land at 4,800-5,000.
  Idle work in that stretch (--profile-idle-pcs, runs stopped at game frames 1,650 and 2,850, difference): 220.9M
  cycles, over half of it a full TLS handshake (mbedtls_mpi_core_mla 17.8%, gcd_modinv_odd 13.1%, P-256, malloc/
  free). `stack` lacks tls-resume (c601a121, launcher hands its TLS session to the module: first handshake 6-9 s ->
  166 ms), so the module's first connection does full ECDHE on the ARM9 just when idle time is scarce.
- Causes: Nine Hours = the first-connection handshake (fix: tls-resume, to verify); MvDK, Layton = the game itself
  doesn't halt for 24-38 s (no capture point under the read-only rule; the only lever is a longer repeat backlog,
  i.e. evaluating up to ~40 s of frames with stale memory instead of a gap: an owner/compliance call); RE = BIOS
  waits (idle-coverage's BIOS-wait hook P1a, half built since 2026-10-13). The round-2 idle-coverage design's
  in-IRQ capture would cover all of these but is ruled out by the read-only rule.

## tls-resume merged into `stack` (2026-10-19)

`stack` 457c84b6 = the previous stack + tls-resume (c601a121: the launcher hands its TLS session to the module).
Hot layout unchanged (hotcheck, 161 symbols). Host tests pass; launcher rebuilt. Catalogue runs used the stack
checkout's launcher (GC_REPO), tags stack-tls (the 4 drop games) and stack-tls-all (all 64).

- Frames dropped: Nine Hours 94 -> 0 (its full first handshake took 16.5 s of wall time in a scarce-idle stretch;
  resumed: 2.7 s there); MvDK 1,443 -> 1,338 (handshake 3.0 s -> 0.27 s; the rest is its own 38 s no-halt stretch);
  Layton 433 and RE 2,437 unchanged (own no-halt stretch / BIOS waits). Games dropping frames: 4 -> 3.
- Catalogue vs `stack` (61 games): median 0.27 -> 0.26%, p90 0.80 -> 0.72%, max 2.66 -> 2.85% (Starfy), over 1%:
  2 (Starfy 2.85, MKDS 1.10). 29 better, 6 worse; Nintendogs 0.90 -> 0.35 (its window held the full handshake),
  Nine Hours 0.27 -> 0.18; Starfy +0.20 (layout outside the hot code again).
- Lag: unchanged except NSMB, whose lag varies between runs with achievements OFF too (window 119-272 across six
  runs of the same stock game), so it's the game's own run-to-run variation.

## Where Starfy's and MKDS's cost goes on `stack` (2026-10-19)

Runner --evict-attr (melonDS footprint2-lab b0371901, rebuilt in private/lab/melon-fp), `stack` + tls-resume module,
same set/script/window as the catalogue (Starfy 2.85%, MKDS 1.04% here). Charges cover the whole profiled run.

- Starfy: charged 16.4k/interval (I 10.9k, D 5.5k): dsc_capture I 36%, module_idle D 12%, dsc_capture D 12%,
  rc_client_do_frame I 4%, the rest spread. IRQ-heavy: 420 game IRQs a frame, the game resumes after module code 252
  times a frame at ~127 cycles of refill each, which is its whole window cost (~32k). dsc_capture is 3.3 KB of code,
  ~2 KB (62 lines) of it executed per capture.
- MKDS: charged 17.0k/interval (I 13.9k, D 3.1k), spread: rc_client_do_frame 10%, dsc_capture 9%, dsc_idle 8%,
  run_chains 7%, module_idle 5%, changes_before 3%.
- Reading: Starfy under 1% needs ~3x less module work between its IRQs, not a placement change (confinement was
  rejected on 2026-10-17); halving the capture's executed lines would save ~3k (2.85% -> ~2.6%) and reshuffle every
  game's layout. MKDS is within reach of general evaluation speed-ups.

## Cutting module work between IRQs: what's left in the current method (2026-10-19)

Starfy (stack + tls-resume, catalogue window): module idle work 271k cycles a frame (24%, ~72 scanlines); at ~1.6 game
IRQs a scanline that, plus ~52 idle wakes a frame, gives its ~252 resumes. --profile-idle-pcs by function:
rc_client_do_frame (own body, incl. our change-driven code) 19.6%, module_idle 16.2%, dsc_capture 11.4%, changes_before
8.4%, run_chains 4.9%, rc_client_cd_skip 4.9%, snapshot_same 4.5%, halt_entry 3.9%, read_memory 2.9%; condition
evaluation proper (rc_test_*, rc_evaluate_*) ~4%. module_idle's own cycles are the per-wake bookkeeping (52 wakes a
frame, a swp lock, H fields and the stack in the uncached mirror at 37-74 cycles an access, ~1.7k a wake).

Levers inside the method and their size:
- One diff pass instead of two (snapshot_same at commit and changes_before at evaluation both compare the new and
  the previous snapshot word by word, uncached): ~-9% of module time net, ~-0.25% of a frame on Starfy. Folding it
  into the capture copy would lengthen the IRQ-masked copy (hold-off already up to 5.6k cycles): not that way.
- Wake path: already touches the minimum lines; its cost is uncached latency (~1.7k x 52), a few % of module time.
- Every code change reshuffles layout: Starfy alone moved +0.20 to +0.66% between builds this week, more than any
  single lever above is worth, so none of them can be judged on the catalogue one at a time.
Starfy under 1% needs ~3x less ARM9 module work per frame, which inside the method means evaluation leaving the ARM9
(the arm7eval route, parked; WORKLIST: method decisions wait for the owner).

## Evaluation off the ARM9: the upper bound of the ARM7 route (2026-10-19)

Lab build `lab-noeval` 739960ef (pushed, not for merge): `stack` + tls-resume with DSC_LAB_NO_EVAL (no
changes_before / rc_client_do_frame / changes_after; frames still captured, queued, consumed). Catalogue tag noeval vs
stack-tls-all: median 0.26 -> 0.22%, p90 0.72 -> 0.63%, max 2.85 -> 2.52% (Starfy), over 1%: 2 -> 1; MKDS 1.10 ->
0.59, Mario Party DS 0.96 -> 0.68, Kirby 0.55 -> 0.33. 44 better, 4 worse. Starfy keeps its cost: capture + wake.
Write-up for the owner: planning repo research/offload/arm7-reopen.md.

## Where the ARM9 cost really is: upper bounds, and the no-op wake fast path (2026-10-19/20)

Lab flags on `lab-noeval` (pushed, not for merge), each a full catalogue run vs stack-tls-all (stack + tls-resume):

| build | median | p90 | max | over 1% | Starfy (I / D) | MKDS |
|---|---|---|---|---|---|---|
| stack + tls-resume | 0.26% | 0.72% | 2.85% | 2 | 2.85% (17.9k / 14.1k) | 1.10% |
| no evaluation (DSC_LAB_NO_EVAL) | 0.22% | 0.63% | 2.52% | 1 | 2.52% | 0.59% |
| + no capture (DSC_LAB_NO_CAPTURE) | 0.17% | 0.47% | 2.53% | 1 | 2.53% (12.7k / 15.6k) | 0.47% |
| + hook only (DSC_LAB_HOOK_ONLY: module_idle returns at once) | 0.01% | 0.03% | 1.19% | 1 | 1.19% (0 / 13.2k) | 0.00% |

- The wake bookkeeping in module_idle (run at every idle wake, ~53 a frame on Starfy, ~40 code lines plus uncached
  H/stack/lock accesses) is the biggest single cost on the whole catalogue: removing it takes the median 0.17 -> 0.01%.
- Starfy's floor with a bare hook (1.19%, all D-side) is one instruction (--evict-attr, hook-only build): halt_entry's
  `pop {ip, lr}` after `bl module_idle`: the module stack's top line (cached main RAM) is evicted by the game between
  wakes and refilled by that read at every wake (322 resumes a frame x ~41 cycles).
- In module_idle's no-op wakes, dsc_capture is entered every wake (52.9 a frame on Starfy) and finds nothing to take.

Fix (branch `wakefast` 73fa3478 off `stack`, pushed): module_idle records quiet_vbl = last_vblank + 1 at the end of a
full wake when !work_wanted() and dsc_capture_settled() (core: capture once, Q.taken == Q.frames, no stage failure);
halt_entry checks it first (four uncached words through the 0x0C000000 mirror: quiet_vbl, alias_cached,
H.reset_pending/progress_stale, header vblank_count) and does the stock halt before the stack switch. Only when the
game's MPU leaves the mirror uncached (alias_cached 0); SDK5 games keep the full path. Every later wake of a quiet
frame would have found the same (H changes only in passes or reset_entry, which the fast path checks; the ARM7 only
moves vblank_count). Stats: fast-path wakes are not counted in halts_idle or the idle self-timing. The pinned asm
entries after halt_entry move 0xEC -> 0x130; C hot code unchanged (hotcheck 161 symbols).
- Starfy (single run, same set/script/window): 2.85% -> 0.92% (I+D 10.3k), 3006/3006 frames evaluated, 0 dropped.
- Full catalogue (tag wakefast) running; then suite, behaviour gate, merge into stack.
