# Decision 2: busy engines, one frame-boundary capture, ARM7 scheduling

Judge's decision over the round-2 proposals and red-teams in this directory:
- `proposal-frame-boundary.md` (FB) + `redteam-frame-boundary.md`;
- `proposal-busy-slack.md` (SG) + `redteam-busy-slack.md`;
- `proposal-arm7-sched.md` (W7) + `redteam-arm7-sched.md`.

Inputs: brief `../design-brief2.md` and the round-1 decision (D1:
`~/Developer/DScheevos/dscheevos-idlecov/docs/idle-coverage/decision.md`). Status: design only. Nothing was built
and no emulator was run. Owners are the same as in D1: perf owns `module.c`/`start.s`, arm7eval owns the ARMv4T
build and its CPI, rnd owns the gather, and display owns the strip.

---

## 0. The decision in one screen

1. **One sampling instant for every frame of every game: line 192, at the game's VBlank dispatch, before the
   game's VBlank handler runs** (from FB). This is not melonDS's instant: RetroArch samples at the end of line 262
   (FB §0.1, verified by the red-team). No free mechanism reaches line 262, so the 192/262 gap goes to
   ra-compliance with a measured differential (FB-M8). It is not hidden.
   - A **halt-time copy is a zero-game-time precomputation** of that instant. It is used only if no game
     instruction ran between the copy's completion and the VBlank dispatch (the "fresh" rule, §2.3).
   - Every other frame gets an **in-IRQ capture at the same instant**: lag frames, frames where W was busy at
     VBlank, and every frame of a busy engine. Its cost is C, in game time.
   - **Carry is retired.** `frames_uncaptured` and `captures_missed` are retired as normal outcomes.
2. **The entry is a boot-time patch of the dispatcher's indirect-call instruction** (the FB red-team's B1 fix). It
   is not FB's runtime IRQ-table write, which is a game-visible write of game RAM (red-team B2). The patch:
   - is in the accepted boot code-patch class;
   - covers GS DD and DKR, which have no working IPC path today;
   - needs no trampoline pool, watchdog or re-install;
   - **retires today's steady-state IPC-0xA VBlank IRQ.** That saves about 1.4-1.7k game cycles per frame, so the
     whole FB entry is a net saving for waiting games.
3. **Busy engines (GS DD, DKR, Code Lyoko; at least 3/64) are covered at a quantified minimum cost, not at zero.**
   - Every frame: an FB capture in the VBlank IRQ, then W7 on the ARM7 evaluates it.
   - The cost is C per frame plus the game's cache refill afterwards. For typical sets that is about 0.3-0.6% of a
     frame [est, §3.2].
   - Shipping is gated on a pre-registered no-lag test at a 4× margin (FB-M3', FB-H1). Zero is physically
     impossible here: the ARM9 never waits, so any coherent copy of ARM9-cached memory has to be taken in ARM9 time.
   - Detect-and-tell is **withdrawn as an end state**. The busy detector stays, but only as a mode switch
     (pull gating, housekeeping on the ARM7).
4. **Spin harvesting (SG) is not built now.** Its fixed-point certificate is sound and the best new idea of the
   round, but as written it needs:
   - an in-module instruction interpreter, which conflicts with "No emulation, ever" until the owner rules;
   - a game-time pass-through head in 3FFC;
   - a non-atomic trampoline;
   - a probe that can swallow CE7 commands.

   Its realistic miss cost (0.5-20k per probe) is not clearly lower than C. It covers 2 of the 3 engines and not
   GS DD. It becomes P4 research (§4), gated on rulings and on FB-M3' showing that C is a problem.
5. **ARM7 scheduling: W7 is a coroutine at nds-bootstrap's existing ARM7 halt hook, parked on every ARM7 IRQ by a
   swap of `0x0380FFFC`** (the ARM7 twin of D1's DTCM+3FFC). The proposal has 2 blockers and 11 serious findings
   (§5). All are fixed here:
   - MBK verification moves before the SCFG lock;
   - the backlog is drained in order at quit, reset and menu, and W7 is never abandoned mid-`do_frame`;
   - backpressure uses a run-time backlog slope, not a static 325k threshold (Zelda ST breaks that threshold);
   - in busy games, W7 owns journal and notice counters;
   - the Thumb-ARM7 stub is reworked;
   - the round-1 link-map audit is restored;
   - NWRAM is allocated per session together with the display.
6. **Busy-class coverage depends on throughput that does not exist yet.** GS DD's set (~720-770k ARM7 cycles per
   frame, estimated) does not fit free ARM7 time, and GS DD has only 4 free NWRAM slots. That is eval-speed's
   number to move (A7-M2: data in NWRAM). It is stated, not hidden: until it moves, GS DD frames are captured
   every frame, evaluated as the backlog allows, and counted.

---

## 1. What changes in round 1's decision (D1)

| D1 item | Round-2 change | Why |
|---|---|---|
| §0 / §6: "no generic zero-cost mechanism for busy engines; ship detect-and-tell; never build spin-borrowing" | First half stands: zero is impossible for busy engines. Detect-and-tell is no longer the end state. Busy engines get FB capture + W7 at a priced minimum cost (§3). Spin-borrowing moves from "never" to "P4 research behind a ruling" | Owner's round-2 ruling (always evaluate). SG's certificate answers D1 §6's "can't observe the writers" objection for certified loops |
| §6 Q4 option B (clean by MVA in the 0xA IRQ, ARM7 copies later) | **Rejected.** Replaced by the pre-handler in-IRQ copy | Option B is torn by construction: the game resumes before the ARM7 copies. That is the SG red-team's S3, and D1's own reading of M13 says a torn rate > 0 fails RA |
| §2.4 capture rule: commit the newest complete copy made during the wait that this VBlank ended, else `frames_uncaptured` | Commit a copy only if it is **fresh** (§2.3). Otherwise FB captures in the IRQ | One instant for all frames. No uncaptured frames |
| §2.4 P1a interim `captures_missed` | Gone once FB's capture path is on (P1c') | FB covers W-busy frames |
| `architecture.md` carry path (lag frames reuse the last memory) | **Retired.** Lag frames get the FB capture (mid-logic memory at VBlank, as an emulator would) | W7 red-team S9 and FB §3 |
| `vblank_entry` reached via the IPC 0xA from CE7 | Replaced by `fb_entry` at the dispatcher patch. Each `vblank_entry` duty moves (§2.5). The 0xA send stops | FB G7; FB red-team S2 |
| §4 display VBlank fail-safe in "the VBlank hook" | Lives in `fb_entry`, pre-handler: an earlier and better site | FB red-team S2 |
| §2.3 Q1 fallback: permanent `OS_IrqHandler` patch (R4) | The new dispatcher call-site patch is a related but different site: the indirect call, not the handler's entry. Display's R4, Q1 and Q9 are ruled together | They all choose the game's IRQ path |
| §2.7 P2: entry after `runCardEngineCheckHalt` only if A7-M0 finds a lowest-priority loop; force `b_pingIpc` | **Park W7 at `0x0380FFFC`.** No lowest-priority precondition and no forced ping (verified: `patch_arm7.c:190`, `hook_arm9.c:250-251`) | W7 proposal §2; confirmed by its red-team |
| §2.7 one evaluator per session chosen by heap fit | Kept, plus a run-time backlog-slope report and a defined overflow policy (§5.5). Choosing the evaluator by a static c threshold alone is dropped | W7 red-team S1: Zelda ST (F p10 330.7k) |
| §7 A7-M0 | Replaced by S-A7-2 + A7-M0' (halted share per caller) | Six SDK-5 ROMs, GS DD among them, have extra halt callers |
| D1 facts on GS DD ("VCOUNT time-base scheduler spin"; "+0x70 state"; CE9 hot spot) | **Corrected:** the VCOUNT loop is OS_Init's one boot-time wait (0x020545E0, called from 0x0205464C). +0x70 is OSThread priority. 0x023BFxxx is the bootloader stub during boot frames. GS DD is thread churn, not a spin | SG §1, confirmed by its red-team |
| M12 busy detector → `coverage = NONE` | The detector stays (≥ 3,600 frames with no gate entry). It switches the session to busy mode: W7 pulls without `a9_waiting`, and W7 does the housekeeping. It never means "not evaluated" | §3.4 |

Everything else in D1 stands: P1a (wrapper patch, E, W, 3FFC park), P1b, invariants I1/I2, the D1 measurements,
and Q1-Q3, Q5-Q8.

---

## 2. The frame-boundary (FB) mechanism

### 2.1 Entry: boot-time patch of the dispatcher's indirect call

- **Sites:** one per dispatcher variant, found by signature (not a game list):
  - V1: `bx r0`;
  - Alt (NitroSDK 2.x) as nds-bootstrap locates it;
  - **Alt5 / TwlSDK** (GS DD): `blx lr`. The VBlank entry is `literal − 31×4` (FB §1.1; nds-bootstrap's
    `tableAddr = addr[0]` returns the table end);
  - **DKR variant:** a 4th start signature, `mov ip,#0x4000000; add ip,ip,#0x210; ldr r1,[ip,#-8]; cmp r1,#0; bxeq lr`.
    It ends in `bx r2`.
- The patched instruction becomes `b fb_disp_<variant>` (module image at 0x02D80000).
  - From ITCM (0x01FF8000+) the distance is about 14 MB, inside B's ±32 MB.
  - A main-RAM dispatcher is in range too.
- **`fb_disp_<variant>`** (module, ARM, in the IRQ cache-line group next to today's `vblank_entry`):
  - `tst <bit reg>, #1`;
  - if the bit is not VBlank: the original indirect call (`bx r0` / `blx lr` / `bx r2`) with every register as the
    dispatcher left it. That is 2-3 instructions;
  - if it is VBlank: `fb_entry`.
- **`fb_entry`:**
  - saves r0-r3, r12 and lr on the module IRQ stack, never on the game's (minor fix: the handler sees
    bit-identical registers);
  - **once-per-VBlank guard:** an ARM9-local sequence number, plus `VCOUNT ≥ 192` (FB red-team S1). A chained
    handler or a re-dispatch can't capture twice;
  - commits the fresh copy or captures (§2.2, §2.3), does the moved duties (§2.5), restores the registers, and runs
    the original indirect call. The game's handler starts with the dispatcher's lr and registers.
- **Game cost on non-VBlank IRQs:** c_d per IRQ, about 3 instructions on a warm line and up to about 50 cycles when
  the I-line is cold [est]. Kirby has about 40 IRQs per frame, which gives about 0.1-2k cycles. A raster game with
  263 H-Blank IRQs gives about 0.8-13k.
  - FB-M0 measures it per game.
  - The pass mark is "game cycles per frame ≤ today's build" (today pays 1.4-1.7k for 0xA), plus pixel-identical
    raster output.
  - If a raster-heavy title fails, its fallback is the 3FFC pre-dispatch stub (FB §2.7 E3), priced the same way.
- **Rejected:** the runtime IRQ-table write with a trampoline pool (game-visible `OS_GetIrqFunction`, ping-pong on
  re-set, no receiver in GSDD); and a permanent DTCM+3FFC stub as the primary entry (it breaks I1, and every IRQ
  pays a load-and-compare).
- **Not hooked:** VBlanks with IME=0 or IE bit 0 clear (loads). There is no ARM9 entry, so they can't be captured
  coherently. They are numbered from the ARM7 clock and recorded as **gap** records (§2.6). FB-S1 counts them in all
  64 ROMs.

### 2.2 The in-IRQ capture (FB path)

- **Source:** the self-contained capture plan (D1 P1c), executed with no interrupt polls, no restarts and no
  allocation.
  - Main RAM through the cached mirror, which is coherent with the game's dirty lines.
  - DTCM through the CP15 base.
- **Chain state must be pure:** next chain values = f(RAM, immutable chain program, previous *committed* slot),
  double-buffered (FB R10). Today `sync_save` / `sync_restore` mutate in place. This is a hard prerequisite of
  P1c' and is owned by the capture lane.
- **Destination: an IRQ-owned staging slot** (FB red-team S7). The slot is written through the uncached mirror.
  Publishing is a write-buffer drain followed by one `head` store. W and W7 only read published slots, so the
  capture never touches `S.arena`, `S.current` or the rcheevos heap.
- **Delta records, with S8's fixes:**
  - a key-frame every 64 records and at every PLAN_CHANGE or RESET;
  - a sequence check on the consumer;
  - when the ring is full, the open slot absorbs the merge and a `dropped` counter is incremented (§5.5). A delta
    record is never lost silently.
- **C_cap:** one global value, proposed at 8,000 cycles and set by the owner. It is checked against C predicted
  from the plan's shape (lines, words, chain steps, CS_GENERIC count) in the launcher's dry run. It is not a per-set
  table (FB red-team S10).
- **Sets above C_cap** (today: Platinum-class chain sets, Digimon, TWEWY, HG/SS with table overflow) are a
  capture-lane work item. Until each set is brought under the cap:
  - waiting games still use fresh copies (zero game time);
  - their lag frames and W-busy frames get a **gap** record, counted and reported. They are not carried. This
    interim state goes to ra-compliance (Q-C3);
  - these sets cannot ship on a busy engine.

### 2.3 The fresh rule (when a halt copy stands in for the FB capture)

`fresh` is valid only while it is tagged with the current ARM9 VBlank sequence number. It is set in exactly three
places, each with I masked:
1. **E or idle-gate halt:** the copy completed, then `wfi`, then on wake `fresh = copy_valid && (IE & IF & 1)`.
2. **`irq_head` unpark** (W running when VBlank arrives): `fresh = copy_complete && !torn && (IE & IF & 1)`. This is
   the FB red-team S3 fix. Without it, W-busy frames would always take the FB path.
3. **Never anywhere else.** Any other IRQ or game instruction clears it.

The dispatchers handle the lowest bit first (verified for V1, DKR and GS DD), so `fb_entry` consumes `fresh`
before any game instruction runs.

The "copy only near VBlank" lever (FB §2.5) uses one global, run-time adaptive window: copy when VCOUNT is at or
past the p90 of recent wait-entry VCOUNT. It is not set per game. FB-M4 reports the FB-path frame share and the game
cycles per frame for this lever.

### 2.4 Hand-off to the evaluator (what we tell rnd and arm7eval)

| Item | Value |
|---|---|
| Instant | the line-192 VBlank dispatch, pre-handler |
| T (published) | fresh path: dispatch + ~100-250 cycles. FB path: dispatch + C |
| X (ARM9 cost) | fresh: ~100-250 cycles. FB: C (§3.2) |
| Where | main-RAM ring in the module region, uncached mirror, drain, then `head`. Never NWRAM: ARM7-master slots read as zero on the ARM9 and its NDMA |
| rnd | a gather may replace the CPU copy only if it is **complete before the IRQ returns** (coherent at the instant) **and** costs the ARM9 less than C. A clean-then-copy-later gather is torn and is not accepted |

### 2.5 `vblank_entry` duties moved to `fb_entry`

These move:
- frame numbering (ARM9 sequence; the ARM7 count is used only for gap detection, ±1);
- the `dsc_vblank_queue` / drain handshake;
- `IRQ_MISSED`, `IRQ_TIMED` and the idle-cut stats;
- D1's `wait.vblank_ended`;
- the display's strip fail-safe (DISPCNT mode 3 → 1).

Once the FB patch is confirmed live, CE9's IPC handler stops calling `header->vblank`. Today it calls it on every IPC
sync, which would double-count frames. CE7 stops sending 0xA in steady state. The other CE7 commands (0x5, 0x6, 0x7,
0x9) are unchanged.

### 2.6 Record kinds the evaluator sees

| Record | Meaning |
|---|---|
| `FRESH` / `FB` | the frame's memory at the instant |
| `REPEAT n` | n identical frames, published at once with a growing count. Not held until closed (W7 red-team minor) |
| `GAP n` | VBlanks with no ARM9 entry (IME/IE off), or frames above C_cap with no fresh copy. No memory. How rcheevos treats a gap is ra-compliance Q-C3; until that ruling the frame is **not** evaluated and is counted |
| `RESET`, `PLAN_CHANGE`, `HARDCORE_CHANGE` | consumed in order |

---

## 3. Busy engines: the mechanism and its exact minimum cost

### 3.1 Mechanism

- In every frame of a busy engine, `fb_entry` takes the FB path (there is never a fresh copy) and publishes a record.
- W7 evaluates it on the ARM7 (§5).
- The ARM9 runs no module code apart from `fb_disp` and `fb_entry`.
- Classification is the D1 M12 detector, run at run time with no list: busy mode starts after ≥ 3,600 frames with
  no gate entry and ends at the next gate entry. Busy mode only changes ARM7-side behaviour: W7 pulls without
  waiting for `a9_waiting`, and W7 does the housekeeping (§3.4). The ARM9 path is identical in every mode, so a
  false positive costs nothing on the ARM9.

### 3.2 Minimum cost, exactly

Game cycles per busy frame:

```
G = c_d·N_irq                                             (dispatcher patch; non-VBlank IRQs)
  + E + Σcold D-lines·46 + Σcold I-lines·46 + N_w·2 + N_chg·20 + S_chain + E_pub    (C, in the VBlank IRQ)
  + R                                                     (game refill of lines C evicted, ≤ (D+I+shadow lines)·46)
  − 1.4-1.7k                                              (the retired 0xA IPC IRQ)
```

- 46 = one main-RAM line fill (23 bus cycles, gbatek:7642).
- 20 = one uncached store drain (N32 = 10).
- E ≈ 60-120. E_pub ≈ 30.
- I-lines: the capture code is cold every VBlank in a busy engine, ~1-2 KB = 32-64 lines (FB red-team S4).

Worked estimates [est]:

| Set | C (cold) | R | G net | % of frame |
|---|---|---|---|---|
| MKDS-size (26 D-lines, 56 words, 5 changed, 40 I-lines) | ≈ 3.5k | ≈ 3.1k | ≈ 5-6k | ≈ 0.5% |
| Kirby-size (33 D-lines) | ≈ 3.8k | ≈ 3.4k | ≈ 5.5-6.5k | ≈ 0.55% |
| GS DD, DKR | **unmeasured** (FB-M1 first) | | | |

**Why this is the floor:**
- The ARM9 never waits in a busy engine, so every ARM9 cycle is game time.
- The watched words can be dirty in the ARM9 D-cache (Platinum 88% of frames, Sonic Rush 61%), and DTCM is invisible
  to every other master. So a coherent sample needs ARM9 instructions at the instant: at least one load per dirty
  line and one per DTCM word. Line cleans are no cheaper per line, and a clean without a copy is torn.
- The only remaining choice is *where* the ARM9 does it. The VBlank dispatch is the one point that is reached every
  frame with no extra IRQ.
- The other levers are:
  - shrinking the plan (capture lane);
  - an rnd gather that completes in-IRQ for less than C;
  - SG (P4), which converts a spin-engine frame into a fresh frame. SG's own costs are not yet shown to be lower.

### 3.3 The no-lag test (pre-registered)

- **FB-M3'** (melonDS, deterministic input scripts, cache model, `--thumb-cycle`, achievements off apart from the
  injected stub):
  - At the FB site, inject a **thrash stub** that costs k×G cycles **and** touches k×(D+I) distinct lines,
    k ∈ {0, 1, 2, 4, 8}.
  - Lag is counted directly, never from frame hashes alone (FB red-team S5):
    - presented frames per VBlank (write-watch on SWAP_BUFFERS 0x04000540 for 3D, and the BG/OAM commit for 2D);
    - test-only progress counters: GS DD calls of 0x02055A68 per VBlank; Code Lyoko entries of 0x020571C4 per
      VBlank; DKR producer frames per VBlank.
  - 10,000 intervals each for GS DD, DKR, Code Lyoko, plus the lag-frame mode of KSSU, MKDS and Platinum.
  - **PASS:** at k = 4, zero added lag intervals against k = 0 in every title. Frame hashes are a diagnostic only.
  - Report k_min, the smallest k that changes lag.
- **FB-H1** (DSi XL, 30 min per title: GS DD, DKR, KSSU, MKDS):
  - the k = 1 and k = 4 stubs against k = 0, with the module-logged progress counter;
  - **PASS:** lag-interval rate ≤ k=0 + 0.05 percentage points at k = 4;
  - for GS DD and DKR, also no audible audio change, plus an ARM7 SND-underrun counter.
- If a title fails at k = 4, the owner gets k_min and G for it. No list is made.

### 3.4 Housekeeping when the ARM9 never waits

In busy mode the ARM9 gets no W time (W7 red-team S4, FB R2). So:
- **W7 owns the per-frame side effects:**
  - it appends journal entries to the main-RAM journal (written to SD by CE7 only at safe points, as today);
  - it increments the header NOTIFY/CUES counters that CE7's LED code already reads (cardengine.c:2001-2002);
  - it publishes the menu list deltas.
- **Unlock delivery (Q12):**
  - interim: rc_client queues the request, and it is sent at the next safe point (in-game menu, sleep, quit),
    where the game is stopped and the ARM9 module may run;
  - target: an ARM7-side request path (the SDIO shim is already on the ARM7), scoped as its own design after P2.
    TLS cost on the ARM7 against F is unmeasured.
- **The quit drain** (§5.4) evaluates the remaining backlog before the journal write, so no played frame is lost
  at quit.

### 3.5 Per engine

| Engine | Capture | Evaluation | Open gates |
|---|---|---|---|
| GS DD (RA 11788) | FB every frame. The FB patch reaches it at the Alt5 site, so the IPC hook that upstream e038b45c removed is not needed | W7. Today's estimate is 720-770k vs F unknown, with 4 NWRAM slots | FB-M1(GS); A7-F(GS); A7-M2 (needs ~2x); Q-O1 (FNT/FAT) |
| DKR (RA 9546) | FB every frame through the 4th signature | W7. DKR is a Thumb-ARM7 ROM, so it needs the reworked Thumb stub (§5.2) | FB-S2; S-A7-2; FB-M1(DKR); A7-F(DKR) |
| Code Lyoko (no RA set) | FB (V1 site) | W7 | stands for the class; a negative and positive control |

---

## 4. Spin harvesting (SG): kept as P4 research, not built

- **What is kept now:**
  - the GS DD fact corrections;
  - the finding that GS DD has no DScheevos VBlank hook today (moot under §2.1);
  - the fixed-point certificate as the argument that a certified spin can be borrowed;
  - `wait.kind = SPIN` reserved in the wait record.
- **Preconditions before any SG work:**
  1. Owner ruling R-SG on the replay interpreter versus "No emulation, ever". Recommendation: do not build a general
     interpreter. If anything is built, use a decode-only verifier for straight-line and two-path loops, grown only
     with B2 oracle evidence.
  2. H-B1 on hardware: F-C, "cache lines are replaced only on allocation".
  3. B1 census of DKR and Code Lyoko with `--thumb-cycle` and per-thread attribution: spin share ≥ 30% and VBlank in
     spin ≥ 50%.
  4. FB-M3' shows that k_min is close to 4 for a spin engine, or the ARM9 network needs W time that only SG can give.
- **Mandatory fixes if it is built** (from its red-team):
  - commit at the first unpark with VCOUNT ≥ 192, so that it composes with §2.3 as a fourth fresh source;
  - no pass-through head in 3FFC while game code runs, or else a new ruling Q1';
  - one T copy per entry, SYS-only, and a check of IE&IF&IME before re-entering W;
  - the probe signalled through a sharedAddr word, with CE7 serialisation;
  - a hard replay budget of B ≤ 32;
  - classify each load before it is issued;
  - a rejection cache keyed on (PC, loaded values);
  - busy-mode arming priced across all 62 waiting ROMs.

---

## 5. ARM7 scheduling design (W7)

### 5.1 Entry and park (from the W7 proposal, unchanged in shape)

- Two lines in CE7's `newSwiHalt`: after `runCardEngineCheckHalt`, call `dsc7_gate`. A return of 1 skips `swi 6`.
- **Gate preconditions:** CPSR.I = 0, SYS/USR mode, IME = 1, no IE&IF pending, W7 runnable and not disabled.
  Otherwise it returns 0 and the stock halt runs.
- **Park:** with I masked, save the caller, then `game_vec = [0x0380FFFC]` and `[0x0380FFFC] = irq_head7`, then
  resume W7.
- **`irq_head7`:** restore `0x0380FFFC` first, save W7, drop the BIOS frame, restore the caller, and `movs pc` to
  `gate_ret` with r0 = 1. The pending IRQ is then re-taken through the game's handler from the caller's context,
  which is a stock halt wake.
- **Invariant I1-7:** while `0x0380FFFC` is swapped, no game or CE7 instruction executes.
- **A7-M5 accepts two interrupted PCs:** `gate_ret` and the instruction after it (W7 red-team minor).
- **X7 ≤ 230 ARM7 cycles per IRQ [est];** A7-M10 decides it.

### 5.2 Fixes applied

| Red-team finding | Fix |
|---|---|
| B1: MBK2-5 read as 0 after the ARM7 SCFG lock, so W7 is always disabled | Read and record MBK2-5 in `dscheevosSetup`, before the lock. At run time keep only the module7 text CRC (one slot per pass) and per-slot canaries |
| B2: backlog discarded at quit/reset; W7 abandoned mid-`do_frame` | RESET is an in-order record. W7 is re-created only after it yields at a record boundary. At quit, menu and return-to-loader, CE7 runs W7 from its VBlank-handler context until the queue is empty (the open REPEAT is already published), with a timeout of 10 s. The journal is written after the drain. On timeout, the journal records "frames n..m unevaluated" |
| S1/S2/S3: budget from 8 games, achievements and Wi-Fi off, no busy data | Re-run `backlog.py` over all 37 runs. Add census runs with achievements on and the shim active, an MKDS local race, Download Play, and GS DD / DKR / Code Lyoko with `arm7_halted` (A7-F). Selection uses run-time data (§5.5) |
| S4: busy games have no ARM9 return path | §3.4 |
| S5: the ARM7 lane's SPLIT mode | SPLIT has a different scheduler and is **not** designed here. W7 here is whole-set, lagging-consumer only. SPLIT needs its own scheduling round, with a bounded ARM9 wait in `result()` that only runs inside an ARM9 gate: on timeout the ARM9 evaluates the trigger itself, and the wait never extends into game time. Busy engines can't use SPLIT, because the ARM9 has no time |
| S6: NWRAM shared with the display | §5.3 allocation rule |
| S7: the MBK freeze is a choice | Owner question Q14 (keep ARM9 SCFG access in DS mode) |
| S8: no fallback if W7 dies | With the lock kept: W7 disabled = a reported gap. The session status says "evaluation stopped at frame n" and the journal records it. With Q14 = yes: flip the slots back to the ARM9 and run W there (waiting games only) |
| S9: CARRY | Removed (§2.6) |
| S10: bus bound and link map | The D1 link-map audit is restored: everything reachable from `rc_client_do_frame` and its scheduled callbacks lives in NWRAM, or that path is gated to `a9_waiting`. A7-M4's steady-state pass mark is back to ≤ 300 bus cycles per frame for the pull, with 3k only as a 10× stress ceiling |
| S11: ARM9 code that touches the rc_client | Inventory every ARM9 path that uses the rc_client (menu, hardcore toggle, reset, journal, cues, notices). Each becomes an outbox/inbox message. The ARM9's rc_client pointer is poisoned after handover |
| Minor: the Thumb stub is a fixed 0x18 bytes | The Thumb stub calls one CE7 ARM function that does the check, the gate and the conditional `swi 6`. It fits in 0x18 (DKR, Kirby CC/MA, both Club Penguin games) |
| Minor: MBK slot order is inverted | Translate addresses to bank indices through the offset bits before `transferToArm7` |
| Minor: `altReadMethod` ROMs are unhooked | Counted in S-A7-2 |

### 5.3 NWRAM allocation (per session, at boot, launcher dry run)

1. nds-bootstrap's own uses (FNT/FAT, ROM spill, CLUT): unchanged.
2. **The evaluator's need comes next** (module7 hot code, stack, queue, parsed set), because without it frames are
   not evaluated.
3. The display gets whatever is left. Otherwise its strip falls back to a main-RAM source.
4. ARM7 slots are whole banks, translated through MBK's inverted order.
5. If the evaluator doesn't fit: the ARM9 evaluator in waiting games. In busy games: GS DD's case, where Q-O1 and
   eval-speed's footprint work apply.

### 5.4 Hand-off and lifecycle

- **Ring:** the FB ring of §2. The ARM9 writes `head` only, and the ARM7 writes `tail7`, the outbox and `w7_status`
  only. Each sits on its own 32-byte line, uncached on the ARM9 side.
- **Pull gating:** W7 pulls when `a9_waiting` is set (waiting games), or the ring is > 50% full, or the session is in
  busy mode. It polls only on CE7's existing new-VBlank bit.
- **Boot droughts:** FB captures exist from the first VBlank, because the dispatcher patch is installed at boot. So
  the drought buffer holds real records. Size: A7-M9.
- **Order:** W7 consumes strictly in sequence. A sequence gap without a GAP record is treated as a hard fault: it is
  counted, and the next record must be a key-frame.

### 5.5 Throughput, backpressure and overflow (no static threshold)

- **Boot selection:** W7 if the set fits NWRAM, the hook is present, the plan has no live reads, **and** the
  predicted c ≤ 0.7 × F_ref. F_ref = 465k, the worst gameplay p10 in the 37 runs apart from Zelda ST. Otherwise the
  ARM9 evaluator in waiting games, and W7 anyway in busy games (it is the only option there).
- **At run time:** `w7_status` tracks the backlog and its slope over 600 frames.
  - If the slope is > 0 for 3,600 frames, the status becomes "falling behind" (menu line and journal). Nothing is
    dropped yet.
- **Overflow** (ring full; the ARM9 can never block):
  - the open record absorbs the merge, and `dropped` is incremented;
  - the next record is a key-frame;
  - the dropped frames are journaled as unevaluated and shown at the launcher.

  This is the honest floor until eval-speed's throughput work closes the gap. It is never silent.

---

## 6. Phase order

| Phase | What | Gate to start | Owner |
|---|---|---|---|
| **P0** | D1 P0, plus the round-2 static and measurement items in §7.1 (no game-facing build) | none | idle-coverage, arm7eval, Clark (HW) |
| **P1a** | D1 BIOS-wait gate (unchanged) | D1 gates | idle-coverage + perf |
| **P1d** | **FB entry, fresh path only:** dispatcher call-site patch (4 variants), `fb_disp`/`fb_entry`, the once-per-VBlank guard, moved `vblank_entry` duties, the display fail-safe there, 0xA retired. Frames without a fresh copy are counted as GAP for now (no FB capture yet) | Q9; FB-S1 64/64; FB-M0 | idle-coverage (nds-bootstrap patch) + perf |
| **P1b** | D1 idle-thread gate, now setting `fresh` (§2.3) | D1 gates + P1d live | perf + idle-coverage |
| **P1c'** | Capture lane: self-contained plan, pure double-buffered chain state, IRQ-owned staging slot, delta + key-frame ring, plan-shape C predictor. **The FB capture path is turned on** for lag frames and W-busy frames when C_pred ≤ C_cap | FB-M1, FB-M2, FB-M3' (waiting-game lag mode) | perf (core) + capture lane |
| **P2** | W7 on the ARM7 with every §5.2 fix. Gate first, with a spin-counter W7 (A7-M1, A7-M10), then the real evaluator | S-A7-2, A7-M0', A7-M1, A7-M2, A7-M3, A7-M4, A7-M5, A7-M10; Q13 | arm7eval + idle-coverage |
| **P3** | **Busy engines on:** FB capture every frame + W7 + busy-mode housekeeping (§3.4) + safe-point delivery | FB-M1(GS/DKR) ≤ C_cap; FB-M3' + FB-H1 pass; A7-F for busy titles; Q10, Q12 | idle-coverage + arm7eval |
| **P4** | Research behind its own rulings and A/B: SG (§4); an ARM7 network path; the SPLIT scheduler; D1 P4 items | each its own gate | — |

**Why this order:**
- P1d is a net saving on all 64 games and is the base that every later capture needs, so it goes right after the
  isolated P1a.
- P1c' must come before P2, because W7 consumes only self-contained, pure records.
- P3 needs both P1c' and P2.

---

## 7. Measurements (pass/fail fixed now)

Runner: the melonDS fork, one instance at a time, cache model and `--thumb-cycle`. Hardware: DSi XL, with logs kept
in RAM, dumped only at quit or the menu, and written up in TESTING.md. Test-only per-game addresses are allowed in
harnesses only, never in the mechanism.

### 7.1 P0 additions (static first, then melonDS)

| ID | What | PASS | FAIL → |
|---|---|---|---|
| FB-S1 | All 64 ROMs: dispatcher variant, indirect-call site, VBlank entry. GS DD's −31×4 rule checked against a table dump at frame 600. IE/IME sampled at every VBlank in gameplay (census runner) | 64/64 sites located. GS DD, DKR and Code Lyoko have VBlank enabled in ≥ 99% of gameplay VBlanks. Report the GAP share per ROM | Missed sites: the 3FFC pre-dispatch stub for those, priced (owner). A busy title under 99%: Q-C3 with the number |
| FB-S2 | nds-bootstrap log for DKR: ERR_HOOK_9 / `ce9->irqTable` | report. File the Alt5 fix and the 4th signature upstream | — |
| FB-M1 | C and G (game cycles per frame, including refill) with the FB path forced, 6,000 intervals, with a cold I-cache: **GS DD and DKR first**, then Kirby, MKDS, Sonic Rush, Phoenix, Black, TWEWY, Platinum, Digimon, SoulSilver | GS DD and DKR p100 ≤ C_cap. Plan-shape predictor within 20% on every set | Busy title above the cap: capture-lane work before P3. Predictor error: recalibrate |
| A7-F | `arm7_halted` census for GS DD, DKR and Code Lyoko; all 37 runs through `backlog.py`; the 8 reference games with achievements on and the shim active; MKDS 2-console race; Download Play | report F p10 per run. The F_ref rule (§5.5) holds in ≥ 36/37 | rework F_ref; owner |
| S-A7-2 | Bootstrap logs, 64 ROMs: "swiHalt hooked", including the 5 Thumb ROMs with the reworked stub; altReadMethod counted | ≥ 62/64 hooked, DKR included | DKR unhooked: Thumb stub fix before P3 |
| A7-M0' | Share of ARM7 halted cycles entered through the hook, per caller, 10 games incl. the six 4-caller SDK-5 ROMs | ≥ 95% in gameplay | re-run budgets with the hooked share |

### 7.2 FB phases (P1d, P1c', P3)

| ID | What | PASS |
|---|---|---|
| FB-M0 | Dispatcher patch cost: the game's ARM9 cycles per frame, stock vs today vs P1d (fresh path only), 10 games incl. 2 H-Blank raster titles; per-IRQ Δ latency | game cycles per frame of P1d ≤ today's in every game; raster IRQ Δp100 ≤ 50 cycles and the frame buffer pixel-identical to stock |
| FB-M2 | Equivalence: each frame takes both the fresh copy and the FB capture, 10 waiting games × 10,000 frames | identical in ≥ 99.99% of fresh frames; every difference attributed to a DMA or ARM7 write (`--watch-write`) |
| FB-M3' | §3.3 thrash-stub injection | zero added lag intervals at k = 4 in every title; k_min reported |
| FB-M4 | Stock vs P1d vs P1c' in KSSU, MKDS, Platinum, Sonic Rush, RE:DS: lag, game cycles per frame, FB-path frame share | lag ≤ stock; game cycles ≤ today's build; FB-path share reported per title |
| FB-M5 | Latency of non-VBlank IRQs during FB frames | Δp100 ≤ C + 200 cycles; no IRQ on lines 0-191 delayed. This is an explicit exception to D1 M11/M4 for the FB path only |
| FB-M6 | Lab ROM: VBlank handler set/restore churn every frame, chained handlers, reset mid-session | exactly one capture per VBlank, the right handler every time, 0 frames lost |
| FB-M7 | Ring integrity under forced overflow (ARM7 stub stalled 1-60 s) | sequences contiguous or GAP-marked; `dropped` equals the frames lost; image correct after a key-frame |
| FB-M8 | Line 192 pre-handler vs line-262 differential: rcheevos on both snapshot streams, all real sets, recorded inputs | report unlock frames and hit-count differences to ra-compliance (Q-C4). No pass mark: it is a ruling input |
| FB-H1 | §3.3 hardware | lag rate ≤ k=0 + 0.05 pp at k = 4; no audio change; SND underruns equal |
| FB-H2 | VCOUNT at `fb_entry` vs today's `vblank_entry`; fresh-path cycles | fresh path ≤ 300 cycles p99 |

### 7.3 W7 (P2)

| ID | What | PASS |
|---|---|---|
| A7-M1 | DSi XL spin-counter W7 | free p10 ≥ 0.6 × 560k in KSSU, MKDS and Sonic Rush, within 10% of melonDS |
| A7-M10 | Lab ROM IRQ latency, stock halt vs parked W7 (timer, VCOUNT, IPC) | Δp100 ≤ 300 ARM7 cycles |
| A7-M5 | I1-7 per instruction; W7 writes outside its slots, ctl7, outbox, journal and 0x0380FFFC | 0 / 0 over 10 games × 6,000 frames |
| A7-M3 | Write-watch on the ARM7 slots (SDK ≥ 2.8, wireless, cheats, colour filter) | 0 foreign writes |
| A7-M4 | DSi XL: ARM9 main-RAM and NWRAM miss streams while the ARM7 (a) halts, (b) runs W7, (c) pulls at steady state, (d) pulls 30 KB per frame | (b) ≤ +0.1%; (c) ≤ 300 bus cycles per frame; (d) proportional, no cliff |
| A7-M2 | ARM7 cost with the parsed set in NWRAM (arm7eval) | owns c. P3 for GS DD needs c(GS) ≤ 0.9 × F(GS) |
| A7-M6 | DSi XL A/B, spin W7 then real W7: KSSU, MKDS (race and Download Play), Sonic Rush, Platinum, a streaming game | lag = stock; card-read and PXI p99 ≤ +1%; no disconnect; no audio change |
| A7-M9 | Staging bytes in the worst boot drought with FB records | fits the ring, or the launcher grows it within its budget |
| A7-M13 | Quit/reset drain: unlock in the last 2 s before quit, 100× | 0 unlocks lost; the journal is written after the drain; the drain-timeout path is journaled correctly |
| A7-M8 | Cost-model predictor vs measured c, 32 sets | error ≤ 20%; no set over 0.7 F_ref predicted under it |
| A7-M12 | Game-cost rule: the game's ARM9 cycles per frame, off vs on (Kirby, MKDS, GS DD) | equal within noise, apart from FB's G on FB-path frames, which is reported |

### 7.4 P4 (only if SG is opened)

H-B1, B1 census, and B2 oracle (0 violations in ≥ 100k certifications), as in SG §9, with its red-team fixes.

---

## 8. Owner questions (with recommendations)

Rulings already requested in D1 (Q1-Q3, Q5-Q8) stand. Q4 is answered by the owner's round-2 ruling and is
superseded by Q10.

- **Q9 — VBlank entry for every game.** May nds-bootstrap patch, at boot, each IRQ dispatcher's indirect-call
  instruction (4 signature variants) to branch through the module?
  - Cost: about 3 instructions on non-VBlank IRQs. It retires today's IPC 0xA, so it is a net saving (FB-M0 proves
    it).
  - **Recommend yes.** It is in the accepted boot code-patch class, and it covers GS DD and DKR.
  - **Reject** the runtime IRQ-table write.
  - Rule it together with Q1 and the display's R4.
- **Q10 — Busy-engine minimum cost.** Accept G per frame (≈ 0.5% for typical sets [est]) in a busy engine's
  VBlank IRQ as meeting "as close to zero as physically possible", provided FB-M3' and FB-H1 show zero added lag at
  a 4× margin, with the absolute number reported?
  - **Recommend yes.** It is the only mechanism that evaluates every frame of a game that never waits. §3.2 shows
    why it can't be zero.
- **Q11 — Lag frames.** Evaluate lag frames on FB's mid-logic memory at VBlank (RetroArch-like) instead of carry.
  - **Recommend yes.**
- **Q12 — Unlock delivery in busy engines.** Interim: queue, journal and send at the next safe point (menu, sleep,
  quit). Target: an ARM7 request path (P4 design).
  - **Recommend yes to the interim,** with ra-compliance signing off on hardcore liveness (Q-C2).
- **Q13 — ARM7 park.** Swap `0x0380FFFC` only while no game or CE7 instruction executes (I1-7, audited per
  instruction).
  - **Recommend yes:** same class as Q1.
- **Q14 — Keep the ARM9's SCFG/MBK access in DS mode** (do not clear SCFG_EXT9 bit 31). That would allow run-time
  NWRAM master flips: an ARM9 fallback if W7 dies, slot sharing with the display, and an rnd gather into NWRAM.
  - **Recommend: not yet.** Keep the stock lock until an anti-cheat review covers what an unlocked SCFG exposes
    (clock, MBK, mappings). Reopen it if A7-M13, the W7-death rate or the display's NWRAM need shows the gap matters.
- **Q15 — Spin harvesting (SG)** needs a replay of game instructions inside the module.
  - **Recommend: do not build now** (§4).
  - If it is reopened, allow only a decode-only verifier, never a general interpreter, and rule on it against
    "No emulation, ever" first.
- **Q16 — NWRAM priority.** The evaluator before the display strip (§5.3).
  - **Recommend yes:** evaluation is the owner's top ruling, and the display has a main-RAM fallback.
- **Q-O1 — GS DD's FNT/FAT pre-cache** (366 KB, leaving 4 slots). Cut or move it so the ARM7 gets ≥ 8 slots?
  - **Recommend: only after A7-M2** shows GS DD's set can fit the ARM7 at all. Loads would be measurably slower.
- **For ra-compliance:**
  - **Q-C2:** unlocks sent late (catch-up after boot droughts; safe-point delivery in busy games), with the frame
    itself correct. Acceptable?
  - **Q-C3:** GAP frames (VBlank masked; above-cap sets without a fresh copy). Should rcheevos see no frame (current
    plan) or something else? Use the FB-S1 counts.
  - **Q-C4:** line 192 vs line 262 sampling, with FB-M8's differential.
  - **Q-C1:** stays "no mid-session evaluator migration".

---

## 9. Risks that remain (ranked)

1. **Busy-class throughput.** GS DD's set is about 2x over free ARM7 time, and its ARM7 F is unmeasured. Until
   A7-M2 and eval-speed close that gap, GS DD frames are captured but the evaluator falls behind, and the overflow
   is counted and reported (§5.5). That is the one place "always evaluated" is still not met in real time.
2. **C for GS DD and DKR is unmeasured.** FB-M1 runs first. If either is above C_cap, P3 waits for the capture lane.
3. **The dispatcher patch on raster-heavy games.** c_d × 263 IRQs could exceed today's 0xA cost. FB-M0 decides; the
   fallback is the per-ROM 3FFC stub, chosen by measurement and not by a list.
4. **A 4× margin may fail on a lag-prone busy engine.** In that case the owner gets k_min and G (Q10).
5. **GAP frames** (VBlank masked) are unevaluable by construction. Their share is unknown until FB-S1.
6. **The 192/262 phase difference** may change unlock frames on some sets (FB-M8 → Q-C4).
7. **W7 hardware unknowns:** NWRAM wait states, the ARM7 IRQ count, and real SD droughts (A7-M1, A7-M10, A7-M9).
8. **The Thumb-ARM7 hook for DKR** depends on the reworked 0x18-byte stub and on save-patch space.
9. **melonDS has no cross-CPU contention model and has the Thumb timing bug,** so every margin is decided on
   hardware.

## 10. Hand-offs to other lanes

- **nds-bootstrap upstream:** the Alt5 table-end fix (VBlank = literal − 31×4), the DKR 4th dispatcher signature,
  and the evidence that DKR's IPC hook is suspect today.
- **rnd:** the instant and the coherence rule of §2.4. A gather is accepted only if it completes in-IRQ for less
  than C, because ARM7-master NWRAM can't be an NDMA9 target.
- **arm7eval:** A7-M2 (set in NWRAM), the GS DD target c ≤ 0.9 F(GS), the SPLIT scheduler as a separate design,
  the link-map audit, and the ARM9 rc_client inventory.
- **display:** the fail-safe moves to `fb_entry`. NWRAM is the evaluator's first. Q9 is ruled with R4.
- **perf:** `fb_entry`, the vblank duties, the pure chain state and the IRQ-owned staging slot (P1c').
