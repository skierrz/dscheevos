# Decision: idle coverage (how DScheevos gets CPU time in every game)

Judge's decision over three proposals and their red-teams (all in this directory):
`proposal-arm9-hooks.md` + `redteam-arm9-hooks.md`, `proposal-uniform.md` (WGC) + `redteam-uniform.md`,
`proposal-arm7.md` + `redteam-arm7.md`. Brief: `../design-brief.md`. Status: design only. Nothing is built.
`module.c`/`start.s` belong to the perf session, and the ARM7 build belongs to eval-speed's arm7eval agent.
Each phase below names who owns it.

---

## 0. The decision in one screen

**Base: arm9-hooks.** It has the 6-byte in-place wrapper patch, exact IntrWait emulation (E) with the discard
at call time, and a work coroutine (W) that every IRQ parks before any game code runs. It has the fewest ways to
cost a lag frame:
- No deadline, line timer or margin K.
- No hand-back race with the real SWI 5.
- No trampoline.
- No cost on IRQs in the game's working time.

The WGC red-team found two lag-frame paths (B1, B2) that come from handing the wait back to the real SWI 5 before
VBlank, plus a wrong trampoline site (B3). Its fixes work, but each adds a race window that E does not have.

**Grafts:**

| From | Idea | Why |
|---|---|---|
| WGC | **One** long-lived coroutine for *every* gate (BIOS wait now, idle-thread OS_Halt next). It removes the per-wake pass, and the old `module_idle` pass is retired | Fixes arm9 red-team B1 (shared-stack crash in mixed games) by construction. Fixes Kirby's 20 passes per frame, strip-only wakes and the stale-flag display problem |
| WGC | The `wait` record as the display's exact "game is waiting now" signal, kept by the park/unpark code | Display requirements 1-4 |
| WGC red-team | Restore `ctx` on every resume; nested-IRQ discipline; no `lt_fired` multiplexing | Applied to our state machine (no LT exists here) |
| ARM7 | "ARM9 captures, ARM7 evaluates" as **phase 2**, with all three ARM7 red-team blockers fixed | The owner's late fact: ARM9 evaluation costs the game ~100k cycles per frame in cache refills (Kirby +21%). ARM7 is measured 83-92% idle in gameplay. This is the only way to cut that cost and to fit RE:DS-class sets |
| ARM7 red-team | A self-contained capture plan (no pointers into rcheevos' heap, no live reads at evaluation) | Needed by phase 2 **and** by the ARM9 coroutine, so a capture can run while an evaluation is suspended |
| New (judge) | Park through a runtime swap of the BIOS IRQ vector pointer **DTCM+3FFC**, not through a toggle of the first word of the game's ITCM `OS_IrqHandler` | Same invariant (it is set only while no game instruction runs). But it needs no SDK signature (GS DD's V4 and unknown handlers work), raises no D/I-cache coherence question (red-team S8), and does not collide with the display's R4 code patch. Owner ruling Q1 |

**Dropped:**
- WGC's NDMA line timer for the BIOS wait (its job, avoiding the SWI 5 trap, is done by E without a deadline);
- the permanent dispatcher patch as the park mechanism (it costs every game IRQ in working time; kept only as the Q1 fallback);
- WGC's trampolines (B3);
- the ARM7 WRAM-A placement (game memory: ARM7 red-team B1);
- G3 spin-borrowing and arm9 §7.2 (not zero, unsafe);
- 133 MHz (owner: test-only flag).

**Busy engines (GS DD, Code Lyoko, DKR's 3D engine; ≥ 3/64, a floor): no generic zero-cost mechanism exists.**
All three proposals and all three red-teams agree, and so do I. Section 6 says what we do and how the player is
told. It also gives the one generic option (capture at the VBlank IRQ, evaluate on the ARM7), which costs measured
game time. The owner must rule on it, because two of the owner's rulings conflict for these games (Q4).

---

## 1. Phases, in order (most RA games covered per unit of risk first)

| Phase | What | Classes it moves | RA share (est.) | Owner | Gate to start |
|---|---|---|---|---|---|
| **P0** | Measurements that decide everything after them; no game-facing build | — | — | idle-coverage + arm7eval + Clark (HW) | none |
| **P1a** | BIOS-wait gate: wrapper patch + E + W + vector-swap park. W evaluates on the ARM9. The idle OS_Halt hook halts stock once W exists in the session | `bios` 0 → covered | ~9% | idle-coverage (asm, nds-bootstrap patch) + perf (module.c split) | P0: S1, S2, H1 pass; owner Q1, Q2 |
| **P1b** | The idle-thread OS_Halt gate also resumes W (the unified coroutine). `module_idle` passes are retired | `oshalt`: same coverage, less overhead, exact display signal | ~88% (efficiency, not new coverage) | perf + idle-coverage | P1a M-tests pass; M10 on KSSU/MKDS/Platinum |
| **P1c** | Capture context C, separate from evaluation (self-contained capture plan) | fixes missed captures under long evaluations in every class | all | perf (core) | P1a running |
| **P2** | ARM7 evaluator for sets that fit the NWRAM slots (ARM9 = gates + capture + network) | `bios` heavy sets (RE:DS); cuts ARM9 cache pollution in `oshalt` | ~9% + quality for ~88% | arm7eval (build, CPI) + idle-coverage (placement, hook, coherency, contention) | A7-M0..M4 pass; P1c done |
| **P3** | Busy engines: ship detect-and-tell now; build the VBlank-capture option only if the owner rules for it (Q4) | `busy` | ≥ 3-5% | idle-coverage | Q4 |
| **P4** | Optional extensions behind their own A/B: mid-frame waits as evaluation-only time, `SVC_WaitIntr` gate if a caller is ever found, cache-lockdown mitigation | — | — | — | each its own pass mark |

Why this order:
- P1a is the only phase that turns a class from 0 to covered with a small, isolated change: 9 games' wrapper and
  one IRQ path.
- P1b touches the shipped path of 88% of games, so it goes second, gated on "≥ today" A/B tests.
- P2 is the largest change and depends on P0 numbers that nobody has yet (NWRAM CPI, contention, ARM7 hook role).

---

## 2. Exact mechanism (P1a/P1b)

### 2.1 Boot patch (nds-bootstrap, `dscheevos_hooks.c`, next to `patchOsHalt`)

- Scan `[arm9destination, +0x4000)` for halfwords `2200 DF05 4770`. Require **exactly one** match, else leave the
  wrapper stock. The red-team verified 64/64 ROMs have exactly one, < 0x800 past the load address. Nine ROMs load
  at 0x02004000/0x02007000, so scan relative to `arm9destination`, never at an absolute range.
- Rewrite it to `225B 04D2 4710` (`movs r2,#0x5B; lsls r2,r2,#19; bx r2` → ARM 0x02D80000). r2 is the wrapper's
  own scratch, and callers treat the wrapper as AAPCS.
- Apply it **after** every `disable()` check, because `disable()` zeroes header word 0 (hooks.c:211).
- Disable DScheevos only if neither OS_Halt nor the wrapper is found. Log both.
- Header ABI v7: word 0 = ARM `b bw_entry`, and that word is the new magic. Today word 0 is "DSCV". Three readers
  change: CE7 cardengine.c:2089, dscheevosMenu.c:29, hooks.c:227. `bw_entry` must not take 0xA0, which holds
  `vblank_entry` (IRQ cache-line locality, start.s:57-67). `b` reaches anywhere in the image, so perf picks the
  spot.
- `SVC_WaitIntr` (0/64 static callers): not patched (P4).

### 2.2 E: the IntrWait stand-in (arm9-hooks §4.1-4.2, with fixes)

- Entry: `r0=1, r1=1`. Read the caller CPSR. If the caller mode is not SYS (or USR), take **pure E** (red-team
  minor; `ldm^`/`stm^` assume SYS banking).
- With IRQs masked, write IME=1, compute the DTCM base from CP15 c9 (+0x4000), and clear the r1 bits in
  `[DTCM+3FF8]`. This is the discard at the game's call time, so the SWI 5 whole-frame trap cannot happen.
- Ownership: one thread at a time owns the gate. Others, and any entry while ownership is held, get pure E
  (register-only, same loop, no W).
- The owner thread saves r4-r11, sp, lr and CPSR in `hk`, sets `wait.kind = BIOS_VBL`, increments `wait.seq`, and
  calls `cb_enter` (display) with IRQs masked.
- Loop, IRQs masked:
  - IME=1.
  - If `[3FF8] & mask`: clear those bits and go to `e_done`.
  - Else, if W or C has work: `w_resume` (§2.4).
  - Else: CP15 halt with I masked (no lost wakeup), unmask to the caller's I so the waking IRQ is taken in this
    thread's context, re-mask, and loop.
- `e_done`: the swap is OFF (it already is whenever E itself runs), `wait.kind = NONE`, `cb_leave`, release
  ownership, restore CPSR, `bx lr`.
- The SDK may switch threads while the main thread sits in E. The VBlank handler then sets the flag, and E sees it
  when the SDK resumes the main thread. That case (WGC red-team B1) is correct by construction.

### 2.3 Park mechanism: runtime swap of DTCM+3FFC (judge's change; ruling Q1)

- At gate entry, read `h = [DTCM+3FFC]` (the game's IRQ handler pointer) and store it in `hk.game_vec`. Nothing in
  it is checked against a signature.
- `w_resume`, with IRQs masked:
  1. `[DTCM+3FFC] = irq_head`.
  2. Set the module IE bits the display has armed (§4).
  3. Load W's (or C's) context and `movs pc`.
- `irq_head` (IRQ mode, BIOS frame `{r0-r3,r12,lr}` on SP_irq):
  1. **Module-only IRQ** (`IE&IF` holds only module bits): handle it (strip), ack, and return through the BIOS
     epilogue to W or E. The swap stays ON and no game code runs.
  2. **Otherwise:**
     - restore `[3FFC] = hk.game_vec`;
     - clear the module IE bits;
     - read W's r0-r3/r12/pc out of the frame (reads only) and save r4-r11, the SYS sp/lr and the SPSR into the
       context;
     - move SP_irq back over the frame;
     - load the waiting thread's r4-r11 and sp;
     - set SPSR to the caller's CPSR and `movs pc` to `e_after_irq`.

     IF is still set, so the IRQ is taken again at once through the game's own handler, with E as the
     interrupted context. That is exactly what a BIOS-halt wake looks like.
  3. **Passthrough** (E was halted with the swap ON because the display was armed): restore `3FFC`, clear the
     module IE bits, and jump to `hk.game_vec` with the BIOS frame untouched.
- `w_yield` (W has no work): mask, save the context, swap OFF, clear the module IE bits, back to E.
- **Invariant I1:** `[3FFC] != game_vec` ⇒ no game instruction executes.
- **Invariant I2:** a module IE bit is set ⇒ I1's swap is ON.

  This closes arm9 red-team B2: a strip IRQ can never reach the game's dispatcher, because its IE bit is cleared
  in the same masked sequence that hands the CPU back.
- **Why 3FFC and not the ITCM word:**
  - It is the architectural redirect point the BIOS reads on every IRQ.
  - DMA cannot read DTCM, and no game code runs while it is swapped.
  - It is generic: any handler, any SDK, GS DD's V4.
  - DTCM is uncached, so there is no clean/invalidate (red-team S8 is gone).
  - It composes with the display's R4: if R4 is applied, `game_vec` simply points to the patched dispatcher.
  - Failure mode on reset/quit: §2.6 restores it.
- **Fallback if Q1 is refused:** a permanent boot-time `OS_IrqHandler` entry patch (display R4). It is shared with
  the display, signature-limited (43/45 V1+V2), and costs every game IRQ in working time. Use the WGC red-team's
  load-free fast path (range-check the interrupted PC from the frame in DTCM): about 1 cold I-line, ~50-100
  cycles per IRQ.

### 2.4 The coroutine(s)

- **W (evaluation and housekeeping):** `module_idle`'s body as a loop, keeping its **once-per-new-frame gating**
  of `net_service`, `publish_stats`, notices and cues (red-team S6, module.c:607-648). W runs `dsc_idle` with no
  frame cap; parking makes long items safe. W yields when it has no backlog and the network needs nothing. W has
  no deadline and no budget, and timing is kept only for stats.
- **C (capture), from P1c:** a second context with its own 2 KB stack. It runs a self-contained capture plan into
  a snapshot slot. At every resume the scheduler runs C first if a capture is owed, then W.
  - **P1a interim:** no C. Capture runs inside W at its safe points (`!w_busy`). If W is mid-`do_frame` when a
    capture is owed, the capture is skipped and counted (`captures_missed`). M2 has a pass mark on it.
- **Capture rule (red-team S2, S11):**
  - Capture runs with IRQs enabled. A park keeps its progress.
  - A copy during which any game code ran (a re-taken game IRQ or a thread switch) is marked torn and restarted at
    the next resume. The last *complete* copy is kept until then.
  - The VBlank commit (vblank_entry, unchanged) takes the newest complete copy made **during the wait that this
    VBlank ended**. Every SWI 5 wait is ended by a VBlank, so this is the RA 60 Hz frame boundary (RetroArch also
    evaluates memory at the end of each video frame). No "dominant caller" learning is needed. 30 fps games (RE:DS)
    get two captures per game frame, as in an emulator.
  - If no complete copy exists at VBlank, the frame is counted `frames_uncaptured`. It is never silently carried.
- **Stacks:** one 8 KB module stack for W (the existing one). `module_idle` no longer runs from `halt_entry` once
  W exists (P1a: stock halt; P1b: idle gate). That is red-team B1's fix. The C stack is 2 KB. `irq_head` uses
  only the BIOS frame and the module IRQ stack.
  - `vblank_entry`'s `ldr sp,=__irq_stack_top` (start.s:133) must switch only when not already on that stack. It
    is not reachable from `irq_head`'s non-passthrough path, but fix it anyway (shared WGC finding).

### 2.5 P1b: the idle-thread gate

- `halt_entry`'s idle-loop test (start.s:187-199) is kept. On an idle-loop caller with IRQs live it saves the idle
  thread's registers, sets `wait.kind = IDLE`, and enters the same E-style loop with "flag" = always false: halt,
  take the IRQ in this thread, resume W or C when they have work.
- When a game thread wakes, the SDK switches away at IRQ return, exactly as from a stock OS_Halt. The swap is OFF
  by then (`irq_head` restored it before the game handler ran).
- Non-idle OS_Halt callers stay stock (P4 extension).
- Gains:
  - Kirby's ~20 wakes per frame no longer start 20 passes, because W resumes where it parked.
  - Strip-only wakes start nothing.
  - The display's stale-flag problem B2 is gone, because `wait.kind` is set and cleared with IRQs masked at the
    gate.
- Cost: X per idle-time IRQ (Kirby ~20 per frame) instead of today's SDK-switch latency. M10 measures it.

### 2.6 Reset, quit, failure (red-team S10)

`reset_entry` additionally:
- restores `[3FFC] = hk.game_vec` if swapped;
- clears the module IE bits;
- clears `hk.owner`, `w_running` and `w_busy`;
- discards the W and C contexts and re-creates them at their entry points, so `dsc_reset` runs from a fresh W.

Quit does the same before nds-bootstrap's exit path. Any assert in `irq_head`, such as an unexpected mode or a
frame shape mismatch, sets `hk.disabled`, after which every gate is pure E or stock halt for the session:
coverage is lost, never stability.

### 2.7 P2: ARM7 evaluator (proposal-arm7 with its red-team fixes, all mandatory)

- **Placement:** module7 code, stack and heap only in ARM7-master **WRAM-B/C slots chosen per session** from the
  launcher's dry-run heap size. Never WRAM-A: 0x037C0000+ receives game ARM7 code for SDK ≥ 2.8 DS games
  (patch_arm7.c:405-426) and for MBK6=0x080037C0 headers.
- **Boot:** a WRAMONARM9/ARM7 slot handshake around `dscheevosInit`, ordered after the ARM7's NWRAM writes (ROM
  spill, FNT/FAT, CLUT), with a D-cache clean before the handover. Callbacks are re-pointed with rc_client's public
  setters. rcheevos stays unmodified.
- **Capture plan:** the P1c self-contained plan. Sets with Remember/Recall chains or more than `max_addresses`
  addresses (live reads) stay on the ARM9 evaluator. The ARM7 never reads game RAM.
- **ARM7 entry:** after `runCardEngineCheckHalt`, before the game's `swi 6`. Only if A7-M0 shows the site is the
  lowest-priority loop. Otherwise find the ARM7 SDK idle thread's `OS_Halt` by signature, the ARM9 method.
- **Card reads:** force `b_pingIpc` in CE9 when A7 is active (red-team B2), and re-run `runCardEngineCheckHalt`
  after the pass, before `swi 6`.
- **Link-map audit:** every function reachable from `rc_client_do_frame` and its scheduled callbacks lives in
  NWRAM. Otherwise that path is gated on `a9_waiting`.
- **One evaluator per session,** picked by the launcher: ARM7 if the heap fits the slots and the plan has no live
  reads, else ARM9 W.
- **ARM9 side in P2:** the gates and W stay. W then does only C (capture), network and publishing, so the ARM9's
  cache footprint per wait drops by the evaluation's share.

---

## 3. Every game-visible effect of P1 (complete list, with how each is bounded)

| # | Effect | When | Bound / test | Ruling? |
|---|---|---|---|---|
| G1 | 6-byte wrapper boot patch | always | code patch class (accepted) | no |
| G2 | E writes IME=1 and clears the mask bits in DTCM+3FF8: the values and order the BIOS uses, on the game's own call | each wait | M5 audit (bit-identical to the BIOS), H1 | **Q2** |
| G3 | DTCM+3FFC swapped while no game instruction runs | spare time | invariant I1 checked per instruction (M5) | **Q1** |
| G4 | IRQ latency X when an IRQ interrupts W or C (worst instruction, BIOS entry twice, irq_head, misses) | spare time; game-visible only for the IRQ that ends the wait and for raster/sound IRQs during the wait | realistic 300-800 cycles (red-team S4); M4, H3 per IRQ kind | Q3 |
| G5 | Cache refill after the wait (P). The owner's late fact: ~100k cycles per frame today, Kirby +21% | after each wait, **in working time** | M3 lag A/B with the cache model, H6 slack probe; P2 shrinks it | Q3 (same as today's design) |
| G6 | Main-RAM bus contention with the ARM7/DMA while W runs (stock SWI 5 leaves the ARM9 halted) | spare time | H7 (ARM7 reply latency A/B) | Q3 |
| G7 | Thread contexts saved by the SDK during the wait hold a module PC (E), not a BIOS ROM PC; garbage below SP differs | spare time | same precedent as today's idle hook | no |
| G8 | Early write-back of dirty game lines (W evicts them) | spare time | same as today's design | no |
| G9 | Module IE bits (display only) are visible to a game handler? **No**: they are cleared before any game code runs (I2) | — | M5 | no |
| G10 | Power: W runs instead of halting | — | not gameplay | no |
| G11 | Masked spans inside W/C add to X | spare time | M11 inventory: max masked span ≤ 400 cycles | no |

Not written: game heap, data, thread stacks, IRQ table, IF, timers. E does not ack IF.

---

## 4. "Game is waiting now": signal and PC ranges for the display strip

The module exports one record in the header's IRQ cache line (perf picks the offset):

```
wait.kind  : NONE | BIOS_VBL (SWI 5 wrapper) | IDLE (idle-thread OS_Halt) | OTHER
wait.state : W_RUN | C_RUN | HALTED | GAME            (GAME = a game IRQ/thread has the CPU)
wait.seq   : ++ at every gate entry
wait.entry_vcount, wait.vblank_ended (set by vblank_entry when it ends a BIOS_VBL wait)
```

- **The exact test is structural:** a module IRQ (strip, NDMA bits 28-31) can only be *delivered* while the
  3FFC swap is ON (invariant I2). The swap is ON only while no game instruction runs. **So every strip IRQ the
  display receives arrives while the game is waiting, by construction.** A strip deadline that falls while game
  code runs stays pending in IF (its IE bit is clear) and fires at the next unpark. The display decides whether
  it is still in its window from VCOUNT.
- **PC ranges, for audits and the display's own checks:** "in the wait" = the interrupted PC is in
  `[0x02D80000, __text_end)` (module image: E, W, C, pure E) **and** `wait.kind != NONE`. BIOS ROM 0xFFFF0000+ is
  never in the wait path, because we don't execute SWI 5. The wrapper itself (6 bytes in the game's secure area) is
  executed only on entry.
- **Arming points:** `cb_enter(kind, vcount)` at gate entry and `cb_leave()` at gate exit, both with IRQs masked.
  At every unpark `irq_head` also clears the module IE bits, and every resume re-sets them, so the display
  supplies `arm_ie_mask` and the module owns the IE writes.
- **Strip-only wakes** are handled in `irq_head`'s module-only branch. They never unswap, never start a pass and
  never make a capture stale (requirement 3).
- **VBlank wait vs ARM7/PXI wait (requirement 4):**
  - `BIOS_VBL` is exact: SWI 5 means VBlank.
  - `IDLE` is not. Options: (a) a read-only check of the SDK's `OSi_IrqThreadQueue` (M8), and (b) in P2, ARM7
    flags for a card read, save I/O or wireless in flight. The display uses `IDLE` only if M8 passes.

---

## 5. Coverage after each phase (honest)

| Class | After P1a/P1b | After P2 | Not reachable |
|---|---|---|---|
| `bios` 9/64 (~9%) | Every main-thread VBlank wait is ours. Small sets (Sonic Rush 69-74k, Pac'n Roll 80k) are *likely* every frame. That is an M2 outcome, not a promise: the 331k median is a melonDS upper bound. RE:DS (197-209k instr ≈ 670k cycles vs ~200k-cycle wait) evaluates ~1 frame in 3-4; the rest queue and are then dropped | RE:DS fits if the ARM7 delivers ≥ ~200k instr/frame (A7-M2) | — |
| `oshalt` 53/64 (~88%) | Same coverage, no per-wake overhead, exact display signal | Small/medium sets on the ARM7; less ARM9 cache pollution | Sets heavier than any idle at 67 MHz (Platinum 612k in most games, Digimon 1.08M, HG/SS 2.1M, TWEWY 4.6-4.8M) need eval-speed's cost reductions. No hook gives time that doesn't exist |
| `busy` ≥ 3/64 (≥ 3-5%) | Detected at run time and reported (§6) | Only with Q4's option B | No zero-cost mechanism |

The owner ruled that frames must always be evaluated, for every game. That ruling **cannot be met** for the busy
class without game time (Q4), nor for the heaviest sets until evaluation gets cheaper (Q7). Both are stated
plainly to the player (§6), not hidden behind DEGRADED.

---

## 6. Busy engines and what the player is told

**Fact:** in a game that never waits, every ARM9 cycle is game code. Borrowing a cycle is free only if no exit
input of the loop can change meanwhile. The ARM9 can't observe the ARM7, DMA or VCOUNT writers of those inputs.
GS DD's scheduler uses VCOUNT as its time base, and Code Lyoko's `poll(obj)` decodes on some iterations. The
ARM7 can't snapshot coherently without an ARM9 clean (Platinum: a dirty watched line in 88% of frames; DTCM is
invisible to it). **So no generic zero-cost mechanism exists.**

**What ships (P3, default):**
- **Runtime detection, no list:** `no_wait_run` counts consecutive VBlanks with no gate entry of any kind. At ≥ 3,600
  (60 s), the session's status becomes `coverage = NONE`. 3,600 is above the longest wait-free stretches seen in
  waiting games (MvDK intro ~2,050, Layton movie ~1,450, idle.md). M12 checks the false-positive rate.
- **In game:** a one-time notice through the existing notice path (the LED cue and the in-game menu line "This
  game never pauses for DScheevos: achievements can't be checked on this console") the first time it trips.
  Hardcore status shows "not checked". No unlock can be awarded from unevaluated frames.
- **Launcher:** at quit (a safe point, so no SD write in play), the module writes a per-ROM-hash coverage record
  (`none` / `partial (x% evaluated)` / `full`) to SD. The launcher warns before the next boot of that ROM. This is
  learned at run time per hash, not a hand-made list (Q6).
- The same record carries `partial` for heavy sets (§5), so the player sees "x% of frames checked" before playing.

**The one generic option, for the owner (Q4 option B), not built until ruled:**
- At the existing VBlank IPC hook (`vblank_entry`, already in game IRQ time today), the ARM9:
  - cleans by MVA the D-cache lines of the plan's direct addresses and last-known chain targets (~16-34 lines:
    ~100 cycles clean, up to ~2k with dirty write-backs);
  - copies the watched DTCM words.
- Then the ARM7 copies the main-RAM words and evaluates (P2 machinery).
- Cost: game IRQ time on every VBlank, plus the ARM7's copy racing the game's new frame. That snapshot can be torn,
  so correctness is weaker than a waiting game's.
- Generic: it works for any game, because every game has a VBlank IRQ.
- Price it first (M13). Do not build spin-borrowing (G3 / arm9 §7.2) at all.

---

## 7. Measurements (pass/fail fixed now)

Runner: melonDS fork, cache model on, `--thumb-cycle`, one melonDS at a time. Hardware results are logged to RAM,
dumped to SD at quit or the menu only, and written up in TESTING.md. Hardware run-to-run noise is handled with
long runs and fixed tolerances, not "same route" pairing (red-team minor).

### P0 (before any game-facing build)

| ID | Where | What | PASS | FAIL → |
|---|---|---|---|---|
| S1 | static, 64 ROMs | wrapper `2200 DF05 4770` relative to `arm9destination`; no branch target inside it (callers jump to its start only) | exactly 1 hit and no mid-entry in 64/64 | patch only exact-shape ROMs; investigate others |
| S2 | melonDS, 9 bios ROMs | at every wrapper entry: caller mode, `[DTCM+3FFC]` value, IE bits set, and the IRQ sources that fire during waits (VBlank / H-Blank / VCOUNT / timer / IPC / card) | caller SYS in 100%; 3FFC constant per session | non-SYS → pure E for those callers; record raster users for M4 |
| H1 | DSi XL lab ROM | dump the NDS9 BIOS IRQ vector and IntrWait; run cases (a) flag already set, (b) VBlank during the wait, (c) other IRQ, (d) caller I=1, (e) IME=0, (f) SWI 4 r0=0, (g) nested pure E, 10,000× each, BIOS vs E | IRQ frame `{r0-r3,r12,lr}` at SP_irq and BIOS reads `[DTCM+3FFC]` per IRQ; E identical to BIOS in VCOUNT/frame at return, 3FF8, IME and preserved regs in every case | fix E before P1a ships |
| H6 | DSi XL lab ROM | T1 slack probe of **today's** OS_Halt design: frame work ends k cycles before VBlank, module on/off, k swept, 10,000 frames per k | report X_today (baseline, no pass mark) | — |
| A7-M0 | static + melonDS | ARM7 halt hook site role per ROM | hooked in ≥ 60/64 and a lowest-priority loop holding ≥ 90% of ARM7 halted time | try the ARM7 OS_Halt signature; else P2 stops |
| A7-M1 | melonDS (done for 8 games) + DSi XL spin counter | ARM7 idle share f | gameplay p10 ≥ 0.6 in ≥ 80% of games; hardware within 10% for KSSU and Sonic Rush | P2 stops |
| A7-M2 | DSi XL, boot time | `rc_client_do_frame` on recorded KSSU/Sonic Rush/MKDS snapshots, code and heap in WRAM-B/C slots | CPI ≤ 2.4 and digest equal to the ARM9's | CPI > 3.0 → P2 stops |
| A7-M3 | melonDS | write-watch on the candidate B/C slots in 10 games, including an SDK ≥ 2.8 DS game, an MBK6=0x080037C0 title, a wireless session, cheats on, colour filter on | 0 foreign writes | pick other slots or stop |
| A7-M4 | DSi XL | ARM9 main-RAM miss stream **and** ARM9 WRAM-B/C stream (CE9 reads) timed by VCOUNT; ARM7 (a) halted (b) evaluating in NWRAM (c) heap in main RAM (d) ring drain only; EXMEMCNT logged in 10 games | (b) within 0.1% of (a) on both streams; (d) ≤ 300 bus cycles per frame | (b) > 0.5% → P2 fails zero cost |

### P1a/P1b (melonDS first, then hardware)

| ID | What | PASS |
|---|---|---|
| M1 | lab ROM: H1's cases in melonDS (real BIOS dump if available) | identical in every case |
| M2 | 9 bios ROMs, 10,000 intervals each, achievements on vs off | lag(on) ≤ lag(off) per title; Sonic Rush and Pac'n Roll: `frames_evaluated == frames_snapshotted`, `frames_dropped == 0`, `frames_uncaptured == 0`, `captures_missed == 0`; every title: `frames_uncaptured == 0` (RE:DS evaluated share reported, not passed) |
| M3 | P1b lag A/B: the 9 bios ROMs + KSSU, MKDS, Platinum, SoulSilver, Tetris, Sonic Colors; stock vs today vs P1 | lag(P1) ≤ lag(stock) on every title, and on OS_Halt titles evaluated frames ≥ today's |
| M4 | per IRQ kind (from S2), IF-set → first instruction of the game's table handler, IRQs that interrupted W/C | Δp100 vs stock halt ≤ 800 cycles for VBlank/IPC/timer; for H-Blank/VCOUNT raster IRQs ≤ 300 cycles **and** frame buffer pixel-identical to stock over the run |
| M5 | write/invariant audit over M2-M3 runs: module-PC writes to game memory except 3FF8/3FFC; any game instruction executed while 3FFC is swapped; any module IE bit set while unswapped; any 3FF8/IME write differing from what the BIOS would write | 0 / 0 / 0 / 0 |
| M6 | park/resume integrity: a SHA-256 debug workload in W and C during waits in all 9 bios ROMs + KSSU, 5,000 intervals each | 0 mismatches, ≥ 100,000 parks, no hang |
| M7 | evaluation equivalence: unlock frames and hit counts vs a reference using a perfect runner snapshot at each VBlank, 10,000 frames, Sonic Rush + KSSU + MKDS | identical |
| M8 | `OSi_IrqThreadQueue` heuristic vs ground truth, 10 OS_Halt games | ≥ 99.9% agreement, 0 false "VBlank wait" while the main thread is runnable (else display gets no IDLE-kind exactness) |
| M10 | P1b gate: KSSU, MKDS, Platinum, cycles per frame spent in module code and passes per frame, P1b vs today | module overhead ≤ today; Kirby passes per frame 20 → 0 (resumes only); lag ≤ today |
| M11 | masked-span inventory: longest IRQ-masked span in module code (capture chunks, lwIP, heap, park) | ≤ 400 cycles p100 |
| M12 | busy detector: census scripts of all 62 waiting ROMs + GS DD, Code Lyoko, DKR | 0 false `coverage=NONE` in the 62; trips in all 3 busy games |
| H2 | DSi XL lab ROM with an SDK-shaped ITCM handler, the digest W, 10 minutes | no mismatch, no hang |
| H3 | DSi XL VBlank and timer IRQ latency, parked W vs stock halt, 10,000 samples | Δp100 ≤ 1,000 cycles; above that, owner decides before H4 |
| H4 | DSi XL Sonic Rush, 30 min, P1 vs observe-only E (W disabled), logs at quit | lag-interval rate P1 ≤ observe + 0.05 percentage points; ≥ 99% frames evaluated; 0 uncaptured; no audio glitch (Clark logs) |
| H5 | DSi XL RE:DS, 30 min: wrapper spans of 1 vs 2 VBlanks, P1 vs observe | 2-VBlank share differs ≤ 0.1 percentage point |
| H6' | H6 repeated with P1 (T1-SWI5 and T1-HALT stand-ins, a 300k-instruction synthetic set) | X_P1 ≤ X_today; absolute X to the owner |
| H7 | DSi XL bus contention: ARM7 PXI-reply and card-read latency with W running vs stock halt | p99 within +1%; else owner (Q3) |
| H8 | DSi XL KSSU/MKDS/Platinum, 30 min each, P1b vs today | lag rate ≤ today + 0.05 pp; no crash; frames evaluated ≥ today |

### P2/P3

| ID | What | PASS |
|---|---|---|
| A7-M7 | stock vs P2 in 10 games (5 oshalt, 3 bios, KSSU, MKDS) incl. a streaming cache-mode game: lag, ARM9 card-read p99, ARM7 PXI p99, SND jitter, ROM mode | lag = stock; p99 latencies ≤ +1%; no ROM-in-RAM → cache regression unless the owner accepts it; every frame evaluated for sets ≤ 150k |
| A7-M8 | ARM9 cache refill after waits, P2 vs P1 (melonDS cache model): game cycles per frame | ≥ 50% lower refill cost than P1 on KSSU (else P2's main rationale fails; report) |
| M13 | Q4 option B priced: cycles added to the VBlank IRQ (clean + DTCM copy) in GS DD, Code Lyoko, DKR, KSSU; torn-snapshot rate vs a perfect VBlank snapshot | report only. The owner rules with the numbers. The judge's reading: torn rate > 0 fails RA correctness |

---

## 8. Questions for the owner (each with a recommendation)

1. **Q1: park mechanism.** May the module swap the BIOS IRQ pointer at DTCM+3FFC to its own handler, only while
   no game instruction executes (invariant checked per instruction in M5)?
   - **Recommend yes.** It is the only option with zero cost on the game's own IRQs, and it is SDK-independent.
   - Alternatives: the same runtime toggle on the first word of the game's ITCM `OS_IrqHandler` (same class,
     signature-limited); or a permanent boot-time dispatcher patch (accepted class, but every game IRQ pays
     ~50-190 cycles in working time). Decide this together with the display's R4, because both choose the game's
     IRQ entry.
2. **Q2: E's writes.** May E write IME=1 and clear the VBlank bit in DTCM+3FF8, exactly as the BIOS does on the
   game's own SWI 5 call (audited bit-identical by M5 and H1)?
   - **Recommend yes:** these are the BIOS's writes on the game's request, not our data.
   - If no, the fallback is a "BIOS-kick" variant: the real SWI 5 runs, W is entered from a module NDMA IRQ that
     wakes the BIOS halt. It needs SCFG_EXT9 bit 8 and NDMA (the rnd session owns NDMA) and re-kicks after each
     game IRQ. It is more complex and has more race windows.
3. **Q3: what "zero" means for the residuals.** X (IRQ latency in the wait), P (cache refill afterwards) and bus
   contention are nonzero for any ARM9-side design, including today's shipped one.
   - **Recommend:** zero = no lag-frame difference and pixel-identical output in the pre-registered A/B tests
     (M3, M4, H4-H8), with the absolute bounds reported. This is the display's T1 standard.
   - P2 exists to shrink P.
4. **Q4: busy engines.** "Every frame evaluated for every game" and "zero gameplay cost" conflict for games that
   never wait.
   - **Recommend:** keep zero cost (a CLAUDE.md hard rule). Ship detect-and-tell (§6), price option B (M13), then
     rule.
   - Never build spin-borrowing.
5. **Q5: upstream footprint.** The wrapper patch is a 5th nds-bootstrap hook point, and P2 adds the NWRAM slot
   handover and the `b_pingIpc` force.
   - **Recommend accept.** Each is small and has a precedent; update CLAUDE.md's list.
6. **Q6: coverage record.** A per-ROM-hash coverage verdict, learned at run time, written to SD at quit and used
   by the launcher to warn before boot.
   - **Recommend accept:** it is not a hand list, and it is written at a safe point.
7. **Q7: sets heavier than any available time** (HG/SS, TWEWY, Digimon; Platinum in most games). No hook fixes
   these; only cheaper evaluation does (eval-speed).
   - **Recommend:** until then the launcher shows "x% of frames checked" from the coverage record, and the
     hardcore claim for such sessions goes to ra-compliance before ship. Don't call it full support.
8. **Q8: P1b changes the shipped OS_Halt path for 53 games.**
   - **Recommend** doing it only after M3/M10/H8 pass, with a build flag to fall back to today's passes.

---

## 9. Risks that remain (ranked)

1. **The real NDS9 BIOS may differ from GBATEK/FreeBIOS** (frame layout, the 3FFC read per IRQ, I-bit handling).
   H1 decides, before any game build.
2. **X on hardware may exceed the bounds** (300-800 cycles is the realistic estimate) for raster-IRQ games in the
   bios class. M4/H3 per IRQ kind decides. If a title fails, W must yield before its raster window, which needs
   per-kind handling and is not designed yet.
3. **P (cache refill) is already ~100k cycles per frame today.** P1 adds W time in the BIOS class, so tight-frame
   BIOS games could lag. M2/H4/H6' catch it. P2 is the structural fix.
4. **P1a interim missed captures** under long evaluations (RE:DS) until P1c. Counted, never silently carried.
5. **The ARM7 lane depends on four unmeasured facts** (hook role, NWRAM CPI, contention, free slots). Any A7-M0..M4
   failure stops P2. RE:DS-class sets then stay partial.
6. **Busy engines stay unchecked** unless Q4 rules otherwise.
7. **Anti-piracy hashing the secure area** would see the wrapper patch. This is the same class as the OS_Halt
   patch, and the bios class is SDK 2.x (pre-AP, a guess).
8. **melonDS has no cross-CPU contention model and the Thumb timing bug**, so every contention and margin claim is
   hardware-decided.
