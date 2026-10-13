# Proposal: Wait-Gated Coroutine (WGC): one resumable DScheevos context, entered at every frame-done wait, preempted on every IRQ

Angle: one mechanism, applied the same way to all games, that does better than today's design for every class
(OS_Halt, BIOS wait, busy). Status: design only. Nothing here is built. Facts are cited by file:line or GBATEK
line in the local copy (`scratchpad/dl/gbatek.txt`); guesses are marked **(guess)**.

Abbreviations:
- `DL` = `~/Developer/DScheevos/dscheevos-display/docs/display-library.md`
- `SV` = `.../dscheevos-display/docs/display-research/survey.md`
- `HW` = `.../dscheevos-display/docs/display-research/hardware.md`
- `CEN` = `~/Developer/DScheevos/dscheevos-idlecov/docs/idle-census.md`
- `IDLE` = `memplan/idle.md` (path in the brief)
- `FH` = `scratchpad/facts-hw.md`
- `GT` = `scratchpad/dl/gbatek.txt`
- `FB` = melonDS FreeBIOS `scratchpad/dl/mel/freebios/src/bios_common.s` (a reference, not Nintendo's BIOS)
- `NB` = `external/nds-bootstrap/retail`

---

## 0. The idea in one paragraph

Today DScheevos work is a series of *passes*. Each pass starts from scratch when the idle thread calls `OS_Halt`
(`start.s:170-214`, `module.c:569-646`), and the SDK scheduler preempts it. That works only for one kind of wait,
pays fixed per-pass costs on every wake (Kirby wakes ~20x per frame, ~390k cycles of overhead each, per the brief),
and re-copies the snapshot at every halt (`module.c:636-645`).

WGC turns all module work into **one long-lived coroutine**: its own registers and stack, resumable at any
instruction. It runs only inside **gates**. A gate is a boot-time code patch at a place where the game waits for
its next frame: the SDK idle thread's `OS_Halt` (exists today), and the SDK's `SVC_WaitVBlankIntr`/`SVC_WaitIntr`
wrappers (new). A busy-loop gate is offered as an experiment only.

**Every IRQ that arrives while the coroutine runs is caught at the game's IRQ dispatcher.** This is a boot-time
patch of `OS_IrqHandler`, the same patch the display session needs (R4). The coroutine is swapped out *before any
game handler runs*, so the game's handlers and scheduler see exactly what stock shows them: the waiting thread,
parked at the gate.

A private line timer handles the one deadline the BIOS wait has. It is a DSi NDMA H-Blank counter, armed only
while the game waits. It parks the coroutine at line 192-K and hands the wait back to the real BIOS SWI 5 before
VBlank. SWI 5 semantics are then bit-exact by construction, with no cost model and no deadline prediction.

The same gate state gives the display strip an exact "game is waiting now" signal, per wait kind, with no stale
flag.

---

## 1. What changes versus today, per class

| Class (census: CEN:13-15) | Today | WGC |
|---|---|---|
| `oshalt` (53/64) | a pass per idle wake, fixed overhead each time; a full capture at every halt; preempted by the SDK at IRQ return | the coroutine *resumes* on each wake, with no per-wake overhead; it recaptures only if game code ran since the last copy; preempted at the dispatcher (+~150 cycles of latency, in spare time) |
| `bios` (9/64; ~9% of RA sets, CEN:38) | nothing: DScheevos is off (`dscheevos_hooks.c:249-252`) or gets no time (IDLE:25,27) | full coverage. Gate G2 on the SWI 5 wrapper; capture at the wait's entry; evaluation in the wait; real SWI 5 called before VBlank |
| `busy` (2/64; ~3%) | nothing (IDLE:36,59) | **not covered by default.** Experimental gate G3 (a learned spin-wait signature plus a late probe IRQ) has a measurable, nonzero game-time cost. Owner ruling. The honest answer: no generic zero-cost mechanism was found |
| display "waiting" signal | none for BIOS/busy; stale flag for OS_Halt (DL:496) | exact per IRQ: the wrapper knows whether the interrupted context is the coroutine, a gate, or game code |

---

## 2. Mechanism

### 2.1 Pieces

1. **Coroutine `CO`** (module).
   - One saved context (TCB: r0-r12, sp, lr, pc, cpsr = 17 words) plus the existing 8 KB module stack
     (`__stack_top`, start.s:154, 201).
   - It runs `co_main()`: capture when dirty, `dsc_idle()`, housekeeping once per VBlank, network, display render.
   - It never halts. When it has nothing to do, it *yields* to the gate that resumed it.
2. **Gates** (module asm; no game stack use beyond the call itself). A gate saves the waiting thread's resume state
   in a module slot (one per gate kind), sets `wait.ctx`, arms the line timer if needed, and resumes `CO`. Each gate
   has a `gate_resume` label, where the thread continues after any IRQ.
   - **G1** = idle-loop `OS_Halt` (existing patch, `dscheevos_hooks.c:54-70`; idle-loop test at start.s:187-199).
   - **G2** = `SVC_WaitVBlankIntr` (SWI 5).
   - **G2'** = `SVC_WaitIntr` (SWI 4), only when `r0==1 && r1==1` (discard, VBlank only). Any other mask passes
     straight through to the BIOS.
   - **G3** (experimental, off) = learned busy wait.
3. **Dispatcher wrapper `W`** (module asm, entered from an 8-byte boot patch at the `OS_IrqHandler` entry).
   - `OS_IrqHandler` lives in ITCM in all 45 surveyed titles (SV:345).
   - Signatures: V1 + V2 cover 43/45; V3/V4 are one-offs, and V4 is Golden Sun DD (SV:347-357).
   - This is the same patch the display needs for NDMA IRQs, because NitroSDK's `OS_IRQTable` has 22 entries
     (`red-team.md:59`; HW:31).
4. **Line timer `LT`**: one DSi NDMA channel used as an H-Blank counter (startup 07h; FH:102, DL:504).
   - Its IRQ is IE bit 28+n, which needs SCFG_EXT9 bit 8 "Extended ARM9 Interrupts", set before the lock
     (GT:24299; HW:31). It is shared with the display: one channel and a sorted list of deadlines (§2.7).
   - It is armed **only inside a gate** and disarmed whenever a game thread takes the CPU. So it never fires in
     the game's working time.
5. **`wait` record**: in the module header's IRQ cache line (start.s:55-58 style): `ctx` ∈
   {GAME, GATE(kind), CO(kind), BIOSWAIT(kind)}, the entry line, flags (`vbl_seen`, `lt_fired`, `dirty`).
   The VBlank IPC path and the display read it.

### 2.2 Boot-time patches (nds-bootstrap, bootloaderi ARM7, the `patchOsHalt` class)

| Patch | Bytes | Where | Precedent |
|---|---|---|---|
| `OS_Halt` → G1 | 8 | ARM9 binary (unchanged from today) | `dscheevos_hooks.c:60-62` |
| `SVC_WaitVBlankIntr` → G2 | 6 (whole wrapper): `46F4` `mov r12,lr` + Thumb `blx tramp` | secure area. 64/64 ROMs have the shape `movs r2,#0; svc 5; bx lr` (CEN:22-25) | same class |
| `SVC_WaitIntr` → G2' | 6, same form | same | same |
| 2 trampolines `ldr pc,[pc,#-4]; .word gate` | 8 each | **in nds-bootstrap-owned memory within ±4 MB** of 0x02000000 (CE9's area, e.g. 0x023BFxxx per CEN:39), checked at patch time. Fallback: secure-area padding (random fill), or G2 off | new; avoids writing game bytes outside the 6-byte function |
| `OS_IrqHandler` entry → W | 8 (`ldr pc,[pc,#-4]; .word W`); W re-executes the 2 displaced instructions (V1: `stmdb sp!,{lr}; mov ip,#0x04000000`, both position-independent, SV:349) | ITCM autoload source in the ARM9 binary, the way `OS_Halt` in ITCM is patched today for NSMB and ACWW (CEN:26-27) | display R4 |
| SCFG_EXT9 bit 8 | 1 bit | bootloader, before the lock (`NB/bootloaderi/source/arm9/main.arm9.c` ~:429-436 per HW:31) | display R5 |

`mov r12, lr` keeps the caller's return address. r12 is caller-saved (AAPCS) and the BIOS SWI path saves r12
itself (FH:28). `blx` from Thumb reaches ±4 MB and switches to ARM. The tramp then jumps to the module at
0x02D80000. If neither trampoline location is in range, G2 is off for that game and its wait stays stock.

### 2.3 Gate G2 (SWI 5) pseudo-code

```
gate_swi5:                       ; ARM, system mode, main-thread stack, r12 = game return address
  if !module_on || CPSR.I || IME==0 || W_missing || wait.ctx != GAME || slot_busy(SWI)
      goto stock                 ; stock: mov lr,r12 ; bx thumb copy of {movs r2,#0; svc 5; bx lr}
  L = VCOUNT
  if 192-K-1 <= L <= 191: goto stock           ; too close to VBlank: nothing to gain
  slot[SWI] = {r4-r11, sp, r12, cpsr}          ; module RAM, no game stack
  wait = {ctx=GATE(SWI5), entry=L, vbl_seen=0, lt_fired=0}
  LT_arm_until(line 192-K)                     ; H-Blank counter: TCNT = (L<192 ? 192-K-L : 192-K)
                                               ; (H-Blanks in V-Blank don't count, DL:504)
  co_resume(GATE SWI)                          ; never returns here; comes back via gate_resume_swi

gate_resume_swi:                 ; reached (a) after W swapped CO out on an IRQ and no other thread ran,
                                 ; (b) after the SDK resumed the main thread here, (c) on co_yield
  if wait.lt_fired:  LT_disarm ; goto real_wait
  if wait.vbl_seen:  goto fallback_return      ; LT lost (IE cleared, etc.): residual F1, counted
  if CO has work:    co_resume(GATE SWI)       ; spare time continues
  else:              wfi (I=0, IME as game left it); goto gate_resume_swi   ; halt like the BIOS does

real_wait:                       ; VBlank has NOT come yet (LT fires at line 192-K)
  wait.ctx = BIOSWAIT(SWI5)
  restore r4-r11, sp from slot; mov lr, r12; bx thumb_svc5_copy   ; the real BIOS does discard+wait+clear
                                                                    ; exactly as stock, then returns to the game
fallback_return:
  wait.ctx = GAME; LT_disarm; r0..r3 = values measured from the real BIOS (M5); bx r12
```

Why this is exact:
- SWI 5 = IntrWait(r0=1 discard, r1=1) (GT, "BIOS Halt Functions"; FH:19-25; FB:297-345).
- In the normal path the game's wait ends inside the real BIOS SWI 5. That SWI is entered before line 192, so its
  "discard old flags" step discards only flags the stock call would also have discarded: none can arrive between
  the game's call and ours except non-VBlank ones, which stock discards too.
- The BIOS halts, the game's VBlank handler sets DTCM+3FF8 bit 0, and the BIOS clears it and returns. Every write
  to DTCM+3FF8 is made by the BIOS or the game's own handler, as stock.
- From the SDK's point of view the main thread is "running" the whole time, as in stock. Lower-priority threads
  stay starved exactly as in stock.

Margin K. Between the LT IRQ and `svc 5` are W's swap (§2.5, about 150-250 cycles **(guess)**) and the gate's
dozen instructions. K = 2 lines is about 8.5k cycles at 4,260 cycles per line (brief: 1.12M cycles / 263 lines),
a margin of over 30x. K is a constant, checked by M5.

### 2.4 Gate G1 (idle-loop OS_Halt)

```
halt_entry (patched OS_Halt):
  if !idle_loop_caller (start.s:187-199) || CPSR.I || IME==0 || wait.ctx != GAME:  wfi; bx lr   ; stock
  slot[IDLE] = {r4-r11, sp, lr, cpsr}; wait.ctx = GATE(IDLE)
  [optional] LT_arm_until(192-K-C) for the late recapture (§2.6)
  co_resume(GATE IDLE)
gate_resume_idle:
  if CO has work: co_resume(GATE IDLE)  else: wfi ; goto gate_resume_idle
```

The idle thread never returns to the game by itself. The SDK switches away from it at IRQ return when a game
thread wakes, exactly as it does from a stock `OS_Halt`.

Non-idle callers (`halts_other`, module.c:579-582) stay stock. §6 lists an optional extension for them.

### 2.5 The dispatcher wrapper W (the keystone)

Entered in IRQ mode, I=1, after the BIOS pushed {r0-r3,r12,lr} on SP_irq and called `[DTCM+3FFC]` (FB:1186-1209;
GT:3452-3464 is the GBA listing of the same scheme).

```
W:
  ldr r12,=wait ; ldrb r12,[r12,#ctx] ; cmp r12,#GAME
  beq sdk                         ; FAST PATH (all game working time): ~5 instr + displaced 2
  ; --- the IRQ interrupted CO or a gate (spare time) ---
  if ctx == CO(k):                ; swap: CO -> TCB, then present gate k
      TCB.{r0-r3,r12,pc} = BIOS frame (read only); TCB.{r4-r11} = live; TCB.{sp,lr} = stm {sp,lr}^; TCB.cpsr = SPSR_irq
      undo coroutine-only CPU settings (EXMEMCNT bit 15, cache lockdown, §5) -> the game's values
      frame' = {r0-r3=0, r12=0, lr=gate_resume_k+4} on the MODULE IRQ stack, plus game_sp_irq
      ldm {sp,lr}^ <- slot[k] ; r4-r11 <- slot[k] ; SPSR_irq <- slot[k].cpsr ; SP_irq <- &frame'
  else (ctx == GATE/BIOSWAIT):    ; no swap, but keep the frame on the module stack too (uniform epilogue)
      frame' = copy of BIOS frame + game_sp_irq; SP_irq <- &frame'
  mark ours: if IF & LT_bit -> ack it, wait.lt_fired=1, LT_disarm
  note vblank: if IF & 1 -> wait.vbl_seen=1; mark CO's snapshot dirty (IRQ handler will run)
  if (IE & IF & ~ours) == 0: goto epi      ; module-only IRQ: no SDK call, no thread wake, no idle pass
  bl sdk                                   ; game's OS_IrqHandler body, with lr = epi
epi:
  if frame'.lr != gate_resume_k+4: wait.ctx = GAME; LT_disarm; dirty=1   ; the SDK switched to a game thread
  ldmia sp, {r0-r3,r12,lr} ; ldr sp,[sp,#24] ; subs pc, lr, #4             ; SP_irq back to the game's value
sdk:
  stmdb sp!,{lr} ; mov ip,#0x04000000 ; ldr pc,=OS_IrqHandler+8            ; displaced V1 instructions
```

Key properties:
- **The game's own IRQ stack is not written beyond the BIOS's own 6-word push**, which every stock IRQ makes
  (FB:1189).
  - In the slow path the frame the SDK sees is on the module's IRQ stack. SP_irq is a banked register, put back by
    `ldr sp,[sp,#24]` before the final `subs pc,lr,#4`.
  - The abandoned BIOS frame held only module register values (CO's), never game data.
  - Nesting is safe: each level keeps its own `game_sp_irq` word.
- The SDK's `OS_IrqHandler_ThreadSwitch` finds a frame at the offsets the BIOS would give it (SV:349: `stmdb
  sp!,{lr}` then `bx r0` with `lr = ThreadSwitch`). So if a game thread wakes, the SDK saves *gate k's*
  context (the thread's own sp/lr, PC = `gate_resume_k`) into that OSThread, as it saves the idle thread at a
  module PC today. It switches as stock would.
- The fast path costs 5 instructions plus the 2 displaced ones, plus a cold I-cache line now and then: 10-60 cycles
  per IRQ **(guess)**, in game time. This is the one cost WGC adds to the game's working time (§4). The display
  needs the same hook and counts its own `tst` the same way (DL:516).
- **Requirement on existing code:** vblank_entry's copy path sets `sp = __irq_stack_top` unconditionally
  (start.s:133). Under W's slow path SP_irq may already be on that stack. It must switch only when not already
  there, or use a separate 1 KB region.

### 2.6 Capture: the frame boundary, made uniform

- **Boundary (unchanged meaning, uniform rule):** a frame's snapshot is the game's memory at the last moment it
  was in a frame-done gate before its VBlank (architecture.md:46). VBlank queueing stays in `vblank_entry`
  (start.s:65-113).
- **Dirty tracking** replaces "copy at every halt" (module.c:636-645). `dirty` is set by W when:
  - the SDK switched to a game thread (the epilogue test), or
  - any game IRQ handler ran (the handlers can write watched memory).

  CO recaptures only when dirty, at its next resume, before evaluating. That keeps today's exactness: any game code
  since the last copy forces a new one. It drops copies after module-only IRQs (LT, display strip) and after
  copies already made. Display requirement 3 (DL:513) follows directly: a strip IRQ never starts a pass and never
  makes a capture stale.
- **Late recapture (optional, M7 decides):** in G1/G2, LT also fires at line 192-K-C, where C is the measured
  capture length. A dirty snapshot is recopied there, so IRQ-handler writes made late in the wait are included.
  A game that enters its wait after 192-K-C (KSSU's last wait starts at lines 188-191, CEN:35-37) is captured at
  entry, as today.
- **Mid-copy preemption** is unchanged: `dsc_capture` copies in masked chunks and calls `interrupt_pending`
  (module.c:155-172). An IRQ taken there now goes through W's swap. When CO resumes, many lines have passed, so the
  core restarts the copy, which is today's rule.

### 2.7 The line timer LT (shared with the display)

- One NDMA channel (suggested: NDMA3, as in DL:504) as an H-Blank counter. Its fill target is its own FDATA, an
  I/O register, so it uses no main RAM. melonDS showed no difference (DL:504, display-hw-tests.md:64); the hardware
  must confirm (display H11).
- The deadline list holds: 192-K (G2 hand-back), 192-K-C (late capture), and the display's 191-N-K entry and
  191-K exit.
- `LT_arm_until(min deadline > now)` runs only from a gate, CO, or W's slow path. W disarms LT whenever the SDK
  hands the CPU to a game thread. The gate re-arms it on resume, or goes straight to `real_wait` if the deadline
  has passed.
- So **LT never fires in the game's working time.** A disarm that races with a pending IF is acknowledged by W
  as "ours" and costs one slow path. That happens only at the switch point, which is spare time.

### 2.8 The display strip's four requirements (DL:504-516)

1. **"In the wait" PC ranges:** not needed. `wait.ctx` is exact at every IRQ entry, because W is the first code of
   every IRQ. It reads GATE/CO/BIOSWAIT(kind) for waits, and GAME otherwise. This removes B2 (stale flag, DL:496).
2. **Arming at wait entry:** the gate calls `LT_arm_until`. The display's deadlines are in the same list.
3. **Strip-only wakes go straight back:** W handles module-only IRQs without the SDK and returns into CO or the
   gate. No pass is started.
4. **VBlank wait vs PXI wait:** exact for G2 (SWI 5 is a VBlank wait by definition). For G1 it is not (B1,
   DL:490). Option G1b: a read-only check of the SDK thread queue, or a marker patch on the SDK's
   `OS_WaitIrq(TRUE, VBLANK)`. Only build it if the display's survey shows PXI waits at strip lines in titles
   on the allow list.

---

## 3. Coverage per census class

- **oshalt (53/64).** Coverage is the same as today; efficiency is better.
  - No fixed per-wake pass cost (Kirby: ~20 wakes per frame).
  - Copies are made only when game code ran.
  - One `dsc_idle` can span several wakes and several frames' waits. Today `idle_max_frames=4` plus per-pass
    overhead (module.c:420-423).
  - The IRQ latency in spare time rises by the swap (~150-250 cycles **(guess)**). Only the IRQ that ends the
    wait matters to the game.
  - Real sets that don't fit today still don't: Digimon 1.08M, HG/SS 2.1M, TWEWY 4.6M instructions per frame
    (IDLE:53). WGC gives time; it doesn't make evaluation cheaper.
- **bios (9/64).** From zero to the full wait.
  - Sonic Rush: median halted 331k cycles in the BIOS (IDLE:25). Its set is 69k instructions, about 235k cycles
    at CPI 3.4. It fits every frame, minus the K lines.
  - RE: Deadly Silence (30 fps; ~200k mean BIOS halt per interval; set 197k instructions, IDLE:27): fits on
    average only. The queue absorbs it or frames are dropped (DEGRADED).
  - Both use G2 only. RE:DS also has 11% OS_Halt (CEN:52). G1 and G2 share CO, so both kinds of wait feed the
    same evaluation.
- **busy (2/64).** Not covered by default.
  - G3 (§6.2) is the only candidate. It has a game-time cost (late probe IRQs). Its hidden idle may not be idle:
    Code Lyoko's loop calls `poll(obj)` (brief), and stealing iterations of a service routine can change
    behaviour.
  - Owner ruling. Default: off, DScheevos off for the title, as today.

---

## 4. Zero-cost argument and the full list of game-visible effects

**Claim:** on G1/G2 games, everything WGC does runs while the game would be halted in stock, except item E1. The
game regains the CPU at the end of each wait within a bounded swap latency (E3). The module writes no game RAM.

| # | Effect | When | Size | Same as today? |
|---|---|---|---|---|
| E1 | W fast path on every IRQ | **game working time** | ~5+2 instructions, plus an occasional I-cache miss: 10-60 cycles per IRQ **(guess)**; × IRQs per frame | new (shared with the display's R4; DL:516 calls it "the residue") |
| E2 | Boot-time code patches: `OS_IrqHandler` entry (8 B, ITCM image), 2 SWI wrappers (6 B each), `OS_Halt` (8 B, today) | always | a game that checksums its own code would see them | same class as `OS_Halt` |
| E3 | IRQ latency while CO runs: the current instruction (an `ldm` or a cache miss, ~50 cycles) plus W's swap | spare time; only the wait-ending IRQ delays the game | ~150-250 cycles **(guess)**; 0.02% of a frame | today: similar (the SDK switches at IRQ return; capture chunks mask IRQs, module.c:155-172) |
| E4 | Game IRQ handlers interrupting CO/gates run on the module IRQ stack; the interrupted PC/regs they could see are module values | spare time | invisible unless a handler inspects SP_irq or the interrupted PC | today: the interrupted PC is a module PC too |
| E5 | The SDK saves the gate context (module PC, the thread's own sp) into OSThread when a thread wakes | spare time | as today for the idle thread | yes |
| E6 | Caches: CO replaces I/D-cache lines (D 4 KB, I 8 KB, GT:7343), so the next frame's work refills them | after each wait | ≤ 384 line refills × ~46 ARM9 cycles (23 bus cycles each, GT:7643) ≈ 18k cycles worst case **(guess at the bound)**; realistic much less | **yes, today's design has exactly this**; optional mitigation: cache lockdown (§5) |
| E7 | Main-RAM bus use by CO contends with the ARM7 (sound, Wi-Fi shim) and with off-CPU work (DL:490, R21) | spare time | unknown; melonDS has no contention model (FH:58) | yes, today's design has it; optional mitigation: EXMEMCNT bit 15 (§5) |
| E8 | SCFG_EXT9 bit 8 set: the game can read/set IE bits 24-31; our LT bit is visible in IE while armed | always / spare time | R5/R15 of the display (HW:31, `red-team.md:66`) | shared with the display |
| E9 | NDMA channel busy during gates; H-Blank writes to its own FDATA | spare time | no main-bus traffic (to confirm, H11) | shared with the display |
| E10 | G2 fallback F1: LT lost, VBlank preempts CO, the gate returns with DTCM+3FF8 bit 0 **still set** (stock: cleared by the BIOS, GT IntrWait "Return") | failure path only | the next SWI 5/`OS_WaitIrq(TRUE,..)` discards it, so it is equivalent for those; it differs for no-discard readers | new; counted; G2 turns itself off for the session after 3 (runtime rule, no list) |
| E11 | G2 return registers r0-r3 in F1 | failure path | set to the values the real BIOS leaves (measured, M5) | new |
| E12 | nds-bootstrap-owned trampoline words, or secure-area padding as the fallback | always | the padding is random fill (CEN:22-25); risk: anti-piracy reads | new |

**Never touched:** game RAM data, heap, thread stacks, DTCM+3FF8/3FFC, `OS_IRQTable`, timers, VCOUNT/DISPSTAT, game
DMA channels.
- The only writes into game-owned memory are the boot-time code patches (E2) and the BIOS's own IRQ frame push,
  which stock makes too.
- G2 adds no game-RAM write: the BIOS does all flag handling (§2.3).

**What can still add a lag frame:**
- E1 in a frame with almost no slack. Bounded per IRQ; measured by T1.
- E6/E7, the costs today's design already carries. WGC does not increase them: it does fewer copies and no extra
  passes.

This matches the display red-team's conclusion that strict zero isn't reachable and a hardware-measured bound X
is (DL:486-519). WGC's bound is X_WGC = E1 + E6 + E7, measured by T1-SWI5/T1-HALT (§7). Pre-registered
acceptance: **X_WGC ≤ X_today on the same stand-in**, plus the absolute figure for the owner's ruling.

---

## 5. DSi-only resources: what helps, what doesn't

| Resource | Verdict | Why (evidence) |
|---|---|---|
| **NDMA as a private line timer** | **use** (LT) | Games in DS mode have no NDMA (SCFG_EXT9 bit 16 is enabled by nds-bootstrap, FH:77). H-Blank startup 07h (FH:102). IRQs need bit 8 plus the dispatcher (HW:31). melonDS models the H-Blank counter (display T1/H11 smoke, display-hw-tests.md:61-64). |
| **Cache lockdown** (CP15 c9,c0,0/1, GT:94523-94534) | **optional mitigation for E6**, phase 3 | While CO runs, lock 3 of the 4 ways in each cache, so the module allocates only in one way and 75% of the game's lines survive the wait. Restore at every swap-out. Lockdown semantics are only sketched in GBATEK (format only); the "ways below index are locked" behaviour comes from the ARM946E-S TRM **(not in local sources: verify)**. CO gets slower (1 KB D-cache). melonDS's cache model likely lacks lockdown **(guess)**, so hardware decides (H4). |
| **EXMEMCNT bit 15** "Main Memory Access Priority" (FH:57) | **optional mitigation for E7**, phase 3 | Set ARM7 priority only while CO runs; W restores the game's value before any game code runs. The ARM7's main-RAM accesses then win over our cache fills. Effect unmodelled in melonDS; hardware probe (H5). |
| **133 MHz ARM9** (SCFG_CLK bit 0) | phase 4, research | Gain ~1.38x for evaluation (architecture.md:121: 10.6 vs 7.7 ms). Blockers: the clock must change from ITCM code (FH:89; our code could run from a locked I-cache way **(guess)**); SCFG_EXT9 bit 31 is locked by nds-bootstrap for NTR games (FH:77, 93). Keeping it unlocked exposes SCFG to the game (a detection risk). Running *the game* at 133 MHz is a game-visible change; rejected. |
| **NWRAM** | **not free** | nds-bootstrap already uses it in NTR mode: ROM-in-RAM overflow to 0x03700000/0x036F8000 (`NB/bootloaderi/source/arm7/main.arm7.c:890`), +512 KB counted into the ROM size limit (:912-916), FNT/FAT cache at 0x03700000 (:1360-1371; CE9 read at `NB/cardenginei/arm9/source/cardengine.c:902-903`). So NWRAM KB come from the same ROM-cache budget. It could hold CO's hottest data if eval-speed shows a win (ARM9 WRAM data N32=4/S32=1 vs main RAM 10/2, GT:7635-7636). Not part of this proposal. |
| **ARM7 as the evaluator** | **reject as the primary mechanism**; phase 5 research | (1) It can't capture: watched lines may be dirty in the ARM9's write-back D-cache (brief; design-notes:22-24), and an ARM7 copy races the ARM9's next frame. So capture stays on the ARM9 in a frame-done gate, which busy games lack anyway. (2) Memory: rcheevos plus a set (hundreds of KB) doesn't fit in ARM7 WRAM (64 KB, FH:69). NWRAM is taken (above). From main RAM every ARM7 access is 9-10 bus cycles (GT:7624, 7635) *and* contends with the game. (3) ARM7 spare time is unmeasured, and its duties are latency-critical. Interesting **(guess)**: from 1-cycle WRAM (GT:7625) an ARM7TDMI at ~1.5 CPI does ~22M instructions/s, close to the ARM9's measured ~20M (CPI 3.4 at 67 MHz, brief). So the ARM7 isn't slow; memory and coherence rule it out. |

---

## 6. Extensions (each separately gated)

### 6.1 Mid-frame waits (evaluation only, no capture)
With preemption on every IRQ, *any* halt is safe time for evaluation:
- `halts_other` OS_Halt callers (module.c:576-582);
- SWI 4 with masks other than VBlank;
- nds-bootstrap's CE9 read waits. This answers architecture.md:128's "it would delay every read": the delay is
  now only E3 per read.

The cost is E3 + E6 per such wait, *on the game's critical path*, several times per frame. Off by default. Enable
per runtime class only if M8 shows lag unchanged.

### 6.2 G3, busy waits (experimental; owner ruling required)
- Enable only when no G1/G2 gate has fired for 600 frames after boot (a runtime rule, not a list).
- **Probe:** arm LT from the VBlank IPC path to fire at line P = 192-48, then P+16 and P+32 (≤3 probes per frame).
  These IRQs land in game time: ~3 × (W slow path, ~200 cycles) ≈ 600 cycles per frame **(guess)**, about 0.05%.
  This is a real cost, unlike G1/G2.
- **Learn** a signature S = (PC range ≤ 256 B, SP, LR), the interrupted context at the ARM9 VBlank IRQ (W sees IF
  bit 0). Accept S only after 300 frames in which:
  - VBlank lands in S in ≥ 95% of frames;
  - every probe that found S found S again 8 lines later;
  - no frame left S before VBlank.
- **Use:** a probe that lands in S runs CO until VBlank, with LT set to 192-K. Any IRQ swaps CO out, so the loop's
  flag check runs right after the VBlank handler.
- **Unfixable risk:** polled exits (a timer or tick compared in the loop), and work done by the polled routine
  (Code Lyoko's `poll(obj)`). That's why it is behind M9 and an owner ruling.

---

## 7. Measurements (pass/fail fixed in advance)

Runner: the melonDS fork with the cache model, the census counters, and **`--thumb-cycle` corrected timing**
(brief caveat). Everything is logged to RAM and dumped to SD only at quit, or at nds-bootstrap's safe points
(CLAUDE.md).

**melonDS**
- **M1 Signatures (static, 64 ROMs).**
  - What: the `OS_IrqHandler` variant and its first 8 bytes relocatable; both SWI wrappers; a trampoline site
    within BLX range (the CE9 address of each game's nds-bootstrap layout).
  - **Pass:** W patchable in ≥ 62/64; G2 patchable in 9/9 bios-class titles.
  - Fail → write V3/V4 signatures, or take the BIOS-vector question to the owner (HW:31, R4).
- **M2 Bios-class coverage.** 9 bios titles, 10k intervals each (the IDLE/census scripts).
  - **Pass:** `frames_evaluated + frames_dropped == frames_snapshotted`; `frames_dropped == 0` for sets ≤ 150k
    instructions (Sonic Rush, Pac'n Roll, Kirby CC); `F1 == 0`; `lt_late == 0`.
- **M3 No lag regression.** Stock vs WGC (achievements on), same scripts. The census lag metric; all 9 bios
  titles plus KSSU, MKDS, Platinum, SoulSilver, Tetris, Sonic Colors.
  - **Pass:** lag(WGC) ≤ lag(stock) on every title, and ≤ lag(today's build) on the 6 OS_Halt titles.
- **M4 Wait-exit latency.** A runner counter: cycles from the VBlank IRQ assertion to the first instruction at the
  wait's return address. Only frames where G2 parked via LT.
  - **Pass:** p100(WGC) - p100(stock) ≤ 300 cycles.
  - F1 frames are reported separately and must be 0.
- **M5 SWI 5 exactness.**
  - Trace DTCM+3FF8 and r0-r3 at every return to the game from a G2 gate vs stock, with Nintendo's BIOS
    (not FreeBIOS).
  - Trace game reads of DTCM+3FF8 outside the BIOS and the SDK dispatcher, and SWI 4 calls with r0=0 and VBlank
    in r1.
  - **Pass:** identical flag value and r0-r3 on every LT-parked return.
  - **Report:** whether F1 would be observable (any no-discard reader). If none, E10 is moot for the census.
- **M6 Equivalence of evaluation.**
  - tests/games-style: unlock frames and hit counts vs a reference with a perfect snapshot taken by the runner at
    each frame-done wait entry (SWI 5 entry / idle OS_Halt), 10k frames, Sonic Rush plus 2 OS_Halt titles.
  - **Pass:** identical unlock frames and hit counts.
- **M7 Late recapture.**
  - Per frame, compare the watched bytes of the snapshot at gate entry vs at 192-K-C vs today's every-halt
    policy.
  - **Pass to adopt late recapture:** it differs from today's policy in 0 frames (it must be at least as fresh).
    Otherwise keep "recopy on every dirty resume".
  - Also report copies per frame (today vs WGC). Expected: KSSU falls from ~20 to ≤ 2.
- **M8 Mid-frame extension (6.1).** Same as M3 with 6.1 on. **Pass:** lag unchanged on all titles.
- **M9 G3 experiment.**
  - Golden Sun DD and Code Lyoko. S must be learned within 600 frames.
  - The game's own frame counter (a RAM watch) must advance identically to stock for 20k frames.
  - ARM7 sound-channel register writes must match stock to within ±1 frame.
  - **Pass all three, or G3 stays off.**

**DSi XL** (the pass marks hold for each title; 3 runs each; logs dumped at quit):
- **H1 LT accuracy.** Gate armed at random lines; VCOUNT read in W at the LT IRQ.
  - **Pass:** = target or target+1 in 10,000/10,000. Never ≥ 192.
- **H2 W cost (test ROM; timers are ours in a test ROM).** Cycles per IRQ through the W fast path, hot and cold
  I-cache, vs the unpatched handler.
  - **Pass:** ≤ 30 cycles hot, ≤ 120 cold.
  - The result feeds E1 for the owner.
- **H3 T1-SWI5 and T1-HALT slack probes** (the display's T1 stand-in, DL:524-528).
  - The stand-in's frame work ends k cycles before VBlank and then waits through SWI 5 (or idle OS_Halt). A
    synthetic 300k-instruction set runs in CO. Sweep k down; 10,000 frames per k.
  - Record X = the smallest k with zero lag difference vs the module off.
  - **Pass:** X_WGC ≤ X_today (today's build on T1-HALT). The absolute X goes to the owner.
- **H4 Lockdown.** H3 with the 3-way lockdown during CO. **Pass to adopt:** X drops by ≥ 25% and CO throughput
  falls by ≤ 40%.
- **H5 EXMEMCNT priority.**
  - A test ROM with an ARM7 main-RAM loop timed by its own timer, while the ARM9 runs a miss-heavy CO load, bit 15
    = 0 vs 1.
  - **Pass to adopt:** the ARM7 slowdown with bit 15 = 1 is ≤ 25% of that with bit 15 = 0. No audio glitch in
    Sonic Rush gameplay over 10 minutes (listening plus the ARM7 underrun counter).
- **H6 Real games.** Sonic Rush, RE:DS, Kirby Canvas Curse, KSSU, Pokemon Platinum, 30 minutes each.
  - **Pass:** no crash or hang; F1 = 0; `frames_dropped` within M2's figure ±10%.
  - Unlocks fire on a test set (mock server). Lag counters equal the gate-off build ±0.5% (a hardware
    nondeterminism allowance, fixed now).

All results go into TESTING.md and the display's display-hw-tests.md where they're shared (H1, H2 = display T1/H11
inputs).

---

## 8. Memory

| Item | Size |
|---|---|
| TCB (17 words) + 2 gate slots (12 words each) + `wait` record (in the existing IRQ line) | ~170 B |
| W + gates + `co_resume`/`co_yield` + LT service (asm) | ~1.2 KB **(guess)** |
| Trampolines | 16 B in nds-bootstrap memory |
| Module IRQ stack | existing 4 KB (`0x02F7D000`, architecture.md:65). Game IRQ handlers now run on it in the slow path; the SDK's own IRQ stack is usually smaller **(guess)**; check the maximum depth in M2 |
| CO stack | existing 8 KB (architecture.md:66) |

Net: about 1.4 KB, no new heap, no new ROM-cache cut.

---

## 9. Risks

1. **Dispatcher signature coverage.** 4 variants in 45 titles (SV:345-357); Golden Sun DD's is TwlSDK-style (V4).
   A game without W falls back to today's behaviour: G1 runs as now (the SDK preempts), and G2 is off.
2. **Thread-switch frame assumptions.** W relies on `OS_IrqHandler_ThreadSwitch` addressing the frame relative to
   SP at entry. True for V1 by its shape (SV:349). Verify by disassembly for V2-V4.
3. **Tick loss (E10/F1).** An absolute IE write by the game while LT is armed. Mitigations: re-OR the bit at every
   gate resume, the F1 counter, and G2 turning itself off after 3. Shares the display's R15 mitigation.
4. **vblank_entry stack collision** (start.s:133) under the slow path. Must be fixed before W ships.
5. **E1 in game time.** A game with very many IRQs per frame (sound-timer-heavy) multiplies it. H2 × the M3
   IRQ-rate census gives the worst title.
6. **E6/E7 not modelled by melonDS.** Hardware H3-H5 are the only proof. The same unknown exists in today's design.
7. **Trampoline range.** CE9 isn't always within ±4 MB. Fall back to padding (anti-piracy risk) or G2 off.
8. **Nested-IRQ games, or games with their own IRQ vector.** Handled on the module stack. Their own vector →
   no W → fallback.
9. **Owner ruling needed** on the class of change: a boot patch of the IRQ dispatcher, and game handlers running
   on a module stack during spare time (E4). The display also needs the patch (R4).
10. **Upstream footprint.** It adds the "SWI wait wrappers" and "IRQ dispatcher" hook points to CLAUDE.md's list
    (the latter shared with the display).

---

## 10. Order of work

1. M1 (static signatures and trampoline ranges); M5's reader census; the H1/H2 test ROM.
2. CO plus G1 with W (OS_Halt games): M3/M4/M7 on KSSU, MKDS, Platinum. Must be ≥ today.
3. G2: M2-M6 on the 9 bios titles; then H3/H6.
4. Mitigations H4/H5; extension 6.1 behind M8.
5. G3 only if the owner accepts a game-time probe cost; 133 MHz and ARM7 stay research.
