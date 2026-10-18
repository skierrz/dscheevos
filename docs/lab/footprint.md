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
