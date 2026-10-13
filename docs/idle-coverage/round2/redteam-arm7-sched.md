# Red-team: proposal-arm7-sched.md (lane 5: where and when the ARM7 evaluator runs)

Reviewer stance: default skeptical. I checked the claims against the nds-bootstrap source the proposal cites (perf's
submodule `dscheevos/external/nds-bootstrap`, and the nwram worktree for `nwramGrant`), GBATEK (`scratchpad/dl/gbatek.txt`),
the proposal's own scan outputs (`design2/site_roles.tsv`, `a7scan/`), and **all 37** census runs that have an
`arm7_halted` column (`private/census/out/*/frames.csv`). The proposal used 8. No emulator was run.

## Verdict: fixable

The core mechanism holds up. W7 is parked on every ARM7 IRQ by swapping `0x0380FFFC`, and the IRQ is re-taken
with the caller as the interrupted context. I traced it against the BIOS IRQ frame, the SDK thread-switch path,
`newSwiHalt`, CE9's no-ping rule and CE7's reset paths. I found no lag-frame path, no timer use and no game-RAM write
beyond the swapped vector, which belongs to the same ruling class as round 1's Q1. The game's IRQ stack gets the
BIOS frame at the same depth as stock. Dropping round 1's lowest-priority precondition (A7-M0) and the forced
`b_pingIpc` is justified. `patch_arm7.c:190` sets `newSwiHaltAddr = 1`, and `hook_arm9.c:250-251` pings only when it is 0.

What is wrong falls into three groups:
- one bug that keeps W7 from ever running as written;
- several RA-correctness holes in the lagging-evaluator lifecycle (quit, reset, overflow, CARRY);
- a budget built on 8 hand-picked games, when the available data already contains a game where the threshold fails.

The proposal also conflicts with two neighbouring lanes: the ARM7 lane's SPLIT mode and the display's NWRAM need.

---

## Blockers (as written, the design does not work or is not RA-correct)

**B1. The run-time verification (§3.3) reads zeros, so W7 is always disabled.**
- §3.3 checks "MBK2-5 read on the ARM7 (readable there)" at first entry and every 600 frames. A mismatch sets
  `w7.disabled`.
- For DS-mode games the ARM7 locks its own SCFG side before the game starts: `main.arm7.c:2691-2692`,
  `if (!dsiModeConfirmed) REG_SCFG_EXT &= ~(1UL << 31)`.
- SCFG_EXT7 bit 31 gates "Access to SCFG/MBK registers (4004000h-4004063h)" (gbatek.txt:24366). MBK1-5 are at
  4004040h-4004053h.
- nds-bootstrap itself uses `REG_SCFG_EXT == 0` as its "SCFG is locked" test (main.arm7.c:1985, 2104). So locked
  registers read as 0.
- Result: every slot reads enable = 0 and master = 0, the check fails on the first gate entry, and the ARM7
  evaluator never runs.
- Fix:
  - read MBK2-5 once in `dscheevosSetup` (before the lock) and record the expected values;
  - at run time rely on the code CRC plus the canaries;
  - or keep only a boot-time assertion.

**B2. Quit, reset and return-to-loader discard the backlog. Unlocks from frames that were played are lost.**
- The evaluator lags by design:
  - up to ~30 s after boot at c = 325k;
  - up to 64 frames for an open repeat record, which is unpublished until it closes (§4.2);
  - for an unbounded time in B3's case.
- §2.5 covers reset and the in-game menu, but nothing drains the queue before `returnToLoader` / `reset`
  (cardengine.c:462, 821). Those run inside `myIrqHandlerVBlank` (callers at 2217, 2267, 2281, 2313), where W7 is
  parked.
- A player who unlocks something and quits within the lag window loses it. In RetroArch that frame is evaluated
  before the core stops.
- §2.5's "the next gate entry re-creates W7's context at its entry" is worse when W7 is in the middle of
  `rc_client_do_frame`. Abandoning that stack leaves half-updated memrefs and hit counts in a live `rc_client`,
  which is silent state corruption.
- Fix:
  - reset is a record, consumed in order. W7 is never abandoned mid-call. The rc_client is re-created only after
    W7 yields at a record boundary;
  - at quit and in the menu, CE7 runs W7 to "queue empty" from IRQ context before the exit path. The game is
    stopped, so the cost doesn't matter. The open repeat record is closed first;
  - the journal is written after the drain;
  - specify a timeout and what happens on it (journal "unevaluated frames n..m").

---

## Serious

**S1. The budget does not generalize. Zelda: Spirit Tracks breaks both thresholds, and the data was already on disk.**
- `zeldast-tc` (7,348 gameplay intervals after the first ARM9 halt): ARM7 free p10 = 330.7k, p50 = 344.3k (59-61%),
  well under the "465-515k" the proposal quotes as general.
- `design2/backlog.py zeldast-tc`:
  - c = 325k: the longest backlog run is **451 frames** (7.5 s);
  - c = 400k: the backlog grows to **1,095 frames and never drains** (7,315 frames in backlog).
- So "c ≤ 400k keeps gameplay backlog ≤ 12 frames" (§0.5, §6.1) is false in general. A static per-set threshold
  cannot be safe, because F is per game (and per scene) and unknown at boot. That is a hidden per-game dependency.
- The other 28 runs (Drawn to Life 456k p10 is the next lowest) are in line. One outlier in 37 games is enough to
  break a "never drop frames" promise.
- Needed:
  - a defined overflow policy. Today the ring "must not drop" (§4.5) and the ARM9 can't block, so the
    behaviour is undefined;
  - a run-time backpressure rule, plus the honest reporting the owner asked for;
  - the selection rule must use F measured at run time (`w7_status` backlog slope), not a constant.

**S2. F is measured with achievements off, Wi-Fi off and no game wireless. All three take ARM7 time away from W7.**
- arm7-notes.md states it: "achievements OFF: no DScheevos ARM7 shim/LED work included".
- In production, CE7's `dscheevosVBlank` runs in the game's VBlank handler every frame (cardengine.c:2062-2074):
  - `shim_poll()`, the SDIO Wi-Fi shim for our own network (it records `lines_busy_max`, so it can take several
    scanlines);
  - I2C LED cues.
- Game local wireless (WM threads on the ARM7, e.g. a 4-player MKDS race) is unmeasured.
- The "30% headroom for Wi-Fi-heavy scenes" (§6.1) has no data behind it. This compounds S1.
- Required before any threshold is fixed:
  - census with achievements on and the shim active;
  - MKDS local race and Download Play.

**S3. The busy class's ARM7 budget is unmeasured, yet the proposal calls the ARM7 "the only possible evaluator" for it.**
- `goldensun`, `code-lyoko` and `diddy-kong-racing-ds` runs have no `arm7_halted` column. Only the `-8k`/`-tc`/
  `-arm7`/`-dirty` runs do, and none of those is a busy game.
- DKR is also one of the 5 Thumb-ARM7 ROMs with an unverified hook.
- The §5 busy row should say "F unknown" and add A7-M1-class census runs for GS DD, Code Lyoko and DKR.

**S4. Busy games have no ARM9 time for the return path.**
- §4.3 routes everything W7 produces back through the ARM9 module: server_call through lwIP, notices, LED cues,
  journal entries, menu list deltas.
- In a busy game the ARM9 never enters a gate, so round 1's W (network, housekeeping) never runs.
- So for exactly the class where the ARM7 is the only evaluator:
  - unlocks are never sent during play;
  - notices are never raised;
  - the journal never gets entries, so the CE7 quit-time journal write saves nothing.
- Fix: for the busy class, W7 itself:
  - bumps the header's NOTIFY/CUES counters (CE7's LED code already reads them, cardengine.c:2001-2002);
  - appends to the journal (main RAM, ARM7 writes, no cache);
  - for the network: either the ARM7 runs the request path (the SDIO shim is already ARM7-side), or say
    plainly that sends wait for the quit/menu safe point.

**S5. Conflict with the ARM7 lane's SPLIT mode (arm7-notes, "Partitioned-evaluation patch").**
- That design is the lane's plan for every set that doesn't fit whole-set offload (MKDS, Black: the proposal's
  §6.2 "no" rows):
  - one rc_client on the ARM9;
  - the ARM7 evaluates claimed triggers for frame f in lockstep;
  - the ARM9 waits in `result()`.
- This proposal:
  - says "one evaluator per session" (§0.3, §3.1);
  - schedules W7 only as a lagging consumer;
  - gives every free slot to the ARM7 as master.
- It never addresses the SPLIT scheduling needs:
  - how soon after the ARM9's `frame_begin` signal W7 starts, given it is parked whenever the game's ARM7 runs
    (SND thread, card read, shim);
  - what bounds the ARM9's wait in `result()` so that the wait can't run past the ARM9's idle into game time,
    which is a lag-frame path;
  - where the claimed triggers live (main RAM, per the lane's v1).
- The proposal must either cover SPLIT (latency budget, ARM9 timeout → the ARM9 evaluates that trigger itself
  this frame) or say explicitly that SPLIT needs a different scheduler.

**S6. Conflict with the display: all free NWRAM goes ARM7-master, so the ARM9 loses it.**
- display-library.md plans the strip on a main-RAM framebuffer (R7, ~3.2% of the frame) "unless the plan finds
  NWRAM" (~1.8%), and H5 tests NWRAM as the FIFO source.
- This proposal makes every free slot ARM7-master, which reads as zero on the ARM9 and its NDMA (gbatek.txt:27841-27849).
- The two needs compete for the same 4-16 slots. Golden Sun DD has only 4, and Q-O1 asks to shrink the FNT/FAT
  for it.
- A per-session allocation rule is needed (display bytes first? ARM7 set first?), decided with the display lane.

**S7. "Frozen for the whole session" is a choice, not a hardware fact.**
- It is true only because nds-bootstrap clears SCFG_EXT9 bit 31 at ARM9_SETSCFG (main.arm9.c:443).
- Leaving the ARM9's SCFG access enabled in DS mode (DS games never touch 4004000h+) would allow run-time bank
  master flips. That would allow:
  - rnd's "coherent copy in NWRAM" (NDMA gather into an ARM9-master slot, flipped to the ARM7 at publish);
  - the lane's SPLIT v2;
  - sharing slots with the display over time;
  - a working ARM9 fallback when W7 dies.
- The cost is one upstream line and an anti-cheat review: unlocked SCFG is reachable from cheat codes, which are
  off in hardcore.
- The proposal should present this as an option with its trade-off, not rule it out.

**S8. No fallback when W7 dies, which breaks "always evaluated".**
- Any irq_head7 assert, CRC or canary failure sets `w7.disabled` (§2.5, §3.3). The rc_client state lives in
  ARM7-master NWRAM that the ARM9 can't read, so the ARM9 evaluator can't take over.
- "Coverage lost, never stability" contradicts the owner's ruling. The proposal should state the fallback (S7's
  flip, or an ARM9 re-load from the journal and server state with a compliance ruling) or report it as a gap.

**S9. CARRY records evaluate stale memory as if it were a new frame.**
- §4.2 keeps the flag "CARRY (no capture this VBlank: previous memory)". That makes rcheevos run a frame on memory
  that is known not to be that frame's: deltas read 0 and hit counts accumulate.
- An emulator never does this. Under hardcore it can award something the real frame sequence would not (e.g.
  "value unchanged for N frames" during a load), or reset a delta-based condition.
- Either list exactly when CARRY can occur and get a ra-compliance ruling, or replace it with a real capture.

**S10. The bus bound A4 holds only if everything W7 touches is in NWRAM, and round 1's guards for that were dropped.**
- §0.6 and A4 count only the pull plus control reads. But:
  - every measured c in §6.2 is with data in main RAM (MKDS "conditions stream ~150 KB per frame" ≈ 37k words,
    i.e. tens of percent of the 560k bus cycles, not 0.05%);
  - the 47 KB "hot code" set was measured from ARM7 WRAM (0x037FAD00 in the lab), which in a game belongs to the game.
- Round 1 §2.7 required a **link-map audit** (every function reachable from `rc_client_do_frame` and its
  scheduled callbacks in NWRAM, else gated on `a9_waiting`). Round 1's A7-M4 passed only at ≤ 300 bus cycles per
  frame for the drain.
- The proposal drops the audit and moves the A7-M4 pass mark to ≤ 3k (c). Restore the audit. Keep 300 as the
  steady-state target and give 3k as a measured ceiling with a reason.
- The NWRAM ARM7 wait states are unknown (§1.2). If code fetch there is slower than 1 cycle, every c grows.

**S11. Re-pointing callbacks doesn't cover the ARM9 module's existing direct rc_client users.**
- After handover, any ARM9 code that dereferences the rc_client (menu list, hardcore toggle, reset, journal, LED
  cues, notices) reads **zeros**, with no fault, because ARM7-master slots read as zero on the ARM9.
- The proposal mentions "list deltas" only. It needs an inventory of every ARM9 → rc_client call path and its
  ARM7 message equivalent, plus an assert (the ARM9 rc_client pointer is poisoned to an unmapped address after
  handover).

---

## Minor / factual corrections

- **§1.1 hook-site census is incomplete.** Six SDK-5 ROMs have **four** callers of the halt veneer, not two:
  Drawn to Life: TNC, Ghost Trick, **Golden Sun DD**, PvZ, Sonic Colors and Super Scribblenauts
  (`site_roles.tsv`: `loop(0 bl)` ×3).
  - GS DD disassembly: 0x02384DA4 `push; bl 0x023867D4 (CPSR.I=0, OS_EnableInterrupts); loop: bl halt-veneer; b loop`.
    That is an **SDK idle thread** halting with IRQs on.
  - 0x02386918 `OS_DisableInterrupts; halt loop` (terminate) and 0x02386928 (a bare halt loop).
  - The park design copes, because the precondition is only IRQs enabled. But "the main loop is the hook's
    context" is false there, and A7-M0' should count the halted share per caller.
- **The Thumb variant does not fit "the same way" (§2.1).** `newSwiHaltThumb` is copied with a fixed 0x18 bytes
  into the game's freed EEPROM page-write function (patch_arm7.c `tonccpy(..., 0x18)`; save_patch_universal.c:249,
  save_patch_v5.c:97). The stub is already 0x18 bytes, so a `cmp/beq` won't fit.
  - Rework: the stub calls one CE7 ARM function (check, gate and the conditional `swi 6` all inside CE7) and
    returns. That fits in 0x18.
  - This matters for DKR (busy class).
- **"The IRQ is taken again at once"** after `movs pc` (§2.1). One instruction (`bx lr` at gate_ret) may retire
  before the IRQ is recognized, so the interrupted context may be `newSwiHalt` after the `bl`. That is harmless
  (r0 is preserved through the frame), but the PC-range audits (A7-M5) must accept both PCs.
- **Bank ↔ address mapping.** MBK2-5 use an inverted order (`0x9195999D`/`0x8185898D`: bank 0 at offset 7).
  "Slots in `[roundup32K(base), 0x03780000)`" must be translated to bank indices through the offset bits before
  `transferToArm7`, and slot index ≠ address index.
- **Repeat records are unpublished until closed.** This adds up to 64 frames of evaluation latency in static scenes
  (B2 covers the quit case). Consider publishing the open record with a "may grow" repeat count that only the
  ARM9 increments, or cap it at 8.
- **Boot drought buffer vs who captures at boot.** Before the first ARM9 halt (≤ 603 frames) there is no ARM9
  frame-done point, so either nothing is captured (frames lost) or a VBlank-IRQ capture runs (game time, Q4-B).
  The drought buffer sizing in §4.5 assumes records exist. State which.
- **`altReadMethod` games are unhooked** (patch_arm7.c:160-163) and fall to the IPC-ping path. Count them in S-A7-2.
- **X7 per-IRQ count** (20-50) is a guess. melonDS can count ARM7 IRQ entries per frame cheaply in the census
  runner. Do that before quoting O.

---

## Hard-rule check (summary)

| Rule | Finding |
|---|---|
| No lag frames | No ARM9-side path found for whole-set mode. SPLIT mode (S5) has an unbounded ARM9 wait path that is not designed here |
| Never write game RAM | Only `0x0380FFFC` while no game instruction runs (ruling Q-O2, same class as round 1 Q1). The BIOS frame lands on the game's IRQ stack at the stock depth. OK |
| No game timers | OK (VBlank count and VCOUNT) |
| No SD writes in play | OK. B2's quit drain must finish before the journal write |
| NAND/firmware/EEPROM | Untouched |
| RA correctness | B2 (lost backlog, abandoned do_frame), S1 (unbounded backlog), S9 (CARRY) |
| Nothing per-game | S1/S2: the threshold depends on per-game ARM7 load that is not known at boot |

## What I'd keep (best ideas)

1. The ARM7 `0x0380FFFC` park with re-taken IRQ. It gives stock context for every game handler and every SDK
   thread switch. It makes the lowest-priority precondition and the forced IPC ping unnecessary.
2. The observation that an ARM7-master NWRAM slot is invisible to ARM9/NDMA/DSP, so the last hop into ARM7 NWRAM
   must be done by the ARM7 (or by a master flip, S7). This corrects rnd's interface premise.
3. Immutable published records, single-writer indices, uncached publish with a write-buffer drain, and an
   in-order ARMv4 consumer: a clean coherence story.
4. The drought simulation method (`backlog.py`) is the right tool. Run it over all 37 runs, and with
   achievements and wireless on.
5. A7-M10 (lab ROM IRQ latency A/B) and the spin-counter W7 hardware build first.
