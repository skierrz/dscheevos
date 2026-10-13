# Round 2 proposal: harvesting the hidden slack of busy engines

Angle: busy engines (games that never wait in OS_Halt or the BIOS). Status: design only. Nothing is built, no repository file
was changed, and the emulator was not run. Evidence comes from static disassembly of the three busy ARM9 binaries (devkitARM
r65 objdump of `~/Developer/DScheevos/private/census/dis/{gs,cl,dkr}_arm9.bin`, VMA 0x02000000), the existing census output
(`~/Developer/DScheevos/private/census/out/<game>/{report.txt,hot.csv,io.csv,frames.csv}`), the census runner source
(melonDS-survey `origin/dscheevos-wait-census:tools/dscheevos/headless/main.cpp`), nds-bootstrap at
`dscheevos/external/nds-bootstrap` (commit 32993f85), and GBATEK (`scratchpad/dl/gbatek.txt`). Guesses are marked
**(guess)**, and estimates are marked **(est.)**.

Abbreviations:
- **D1** = round-1 `decision.md`
- **SG** = the spin gate proposed here
- **FP certificate** = the fixed-point certificate (§3)
- **W** = round 1's unified coroutine (D1 §2.4)
- **X** = IRQ latency added while W runs (D1:232)
- **P** = cache refill cost after W (D1:233)
- Cycle counts are ARM9 cycles at 67 MHz. A frame is ~1,120,000 cycles and a scanline ~4,260 (brief).

---

## 0. Verdict in one screen

1. **A generic, provable "only spinning" signal exists, but the SDK thread state is not it.** NitroSDK always runs the
   highest-priority READY thread. TwlSDK's thread selector walks the priority-sorted list for the first thread with
   `state == 1` (GS DD 0x02053D58-0x02053D7C). So "every thread above the running one is blocked" is true of *every* running
   thread and tells us nothing about progress. The signal that works is a **fixed-point certificate (FP)**:
   - Replay one iteration of the interrupted loop, effect-free, from the interrupted register state.
   - It returns to the same PC with the same registers.
   - It does no store outside the thread's own dead stack.
   - It does no I/O, CP15, SWI or uncached load.
   - Every line it reads is cached and fits the 4-way D-cache without self-eviction.

   With those conditions met, GBATEK's cache and DMA facts (§3.2) imply that the loop's exit inputs, *as the ARM9 sees
   them*, can change only during an ARM9 IRQ handler. So a W that parks on every IRQ delays no loop exit, except by the
   park latency X that round 1 already accepted for waits (D1 Q3).
2. **Two of the three engines are spin engines; the third is not.**
   - **Diddy Kong Racing DS:** ≥ 82% of non-boot ARM9 samples are in the consumer's empty-ring path, and in-engine the
     VBlank lands in that path in 80% of frames.
   - **Code Lyoko:** ≥ 42% of samples are in `while(!flag) poll(obj)` with `poll`'s early-out, and the VBlank lands
     there in 79% of frames. The flag is set by the game's VBlank IRQ handler (0x0204D928, registered at 0x02003064).
   - **Golden Sun: Dark Dawn has no provable spin.** Round 1's "VCOUNT time-base scheduler spin" was a misreading (§1).
     Its hot code is a priority-ceiling mutex and thread churn (~28% in OS code) plus a mixer worker whose passes
     change memory every pass. Its PC at VBlank is spread over 15+ blocks. Nothing in GS DD can be proven idle.
     Harvesting it would change the game's state evolution.
3. **Mechanism (SG):**
   - A module probe IRQ, armed only in "busy mode" (runtime detection, no list), samples the interrupted context.
   - If FP certifies it, W runs in place of the spinner with round 1's DTCM+3FFC park.
   - At each unpark the spinner is resumed at a tiny module **re-entry trampoline T** (keyed by the thread's sp). T
     re-checks the memoised certificate after the game's handler has run and re-enters W while the loop is still at
     its fixed point. Otherwise it jumps back to the original PC.
   - Capture for frame F is the last complete copy made in a certified spin with no game instruction between its
     completion and VBlank F. That is exactly memory at VBlank, by the same argument.
   - Frames whose VBlank lands in game work use the minimum-cost fallback: an in-IRQ D-cache clean by MVA, then rnd's
     gather and the ARM7 evaluator.
4. **Not provably zero. The minimum cost is exact in form and small in size (§7):**
   - W time itself costs no game work (it replaces certified spin).
   - The game-time residuals are:
     - c_miss per probe that lands in game work (**est.** 150-300 cycles, ≤ ~1.3k with a bounded replay);
     - c_pass per game IRQ inside an armed window (**est.** 40-100);
     - c_T per T hand-back (**est.** 30-100);
     - X per IRQ that ends a W slice (latency, D1 300-800);
     - P (cache refill);
     - the fallback clean on frames whose VBlank lands in work (**est.** 0.2-2k).
   - The floor is reached when every probe lands in spin and every VBlank lands in spin: then the only game-visible
     effects are X and P, the same residuals the BIOS/OS_Halt gates already have.
   - §9 B3/H-B2 (lag A/B with a game-progress counter, plus the game-cycles-per-frame rule) is the test that must show
     no lag frames.
5. **New blockers found:**
   - GS DD today gets **no DScheevos VBlank hook at all**. nds-bootstrap loads a GS-DD-specific CE9
     (`conf_sd.cpp:1648-1654`, TID "BO5"), where `myIrqHandlerIPC`'s body (`cardengine.c:1626-1730`) and `hookIPC_SYNC`
     (`misc.c:112-125`) are compiled out under `GSDD`.
   - In DS mode, nds-bootstrap leaves SCFG_EXT9 bit 8 clear (rnd `r2-ndma.md:23-24`), so NDMA IRQs (IE 28-31) cannot be
     raised. A module-owned alarm IRQ therefore needs either an owner/display ruling on bit 8 or an ARM7-sent IPC sync
     probe (§5.2).

---

## 1. Corrections to round-1 facts (evidence)

| Round-1 claim | What the code shows | Evidence |
|---|---|---|
| GS DD "task scheduler spin... VCOUNT/time-based"; "its time base polls VCOUNT (33.6 reads/interval at 0x020545EC)" (brief, design-notes.md:8, proposal-arm9-hooks.md:78 F17) | 0x020545E0-0x020545FC is `IME=0; while (VCOUNT != 0); restore IME`. Its only caller is 0x0205464C, the last call of **OS_Init** (0x0205460C, called once from 0x02001814). The 50,323 reads (`io.csv`) are one boot-time wait averaged over 1,499 intervals. The other game VCOUNT readers in play, 0x02001E94 and 0x02001FCC (~0.5 reads per interval), are single reads stored to `[r4+0xA0/0xA4]`: a load meter, not a loop exit **(guess on purpose)**. 0x02000808 is crt0's boot wait | gs disassembly at those addresses; `goldensun/io.csv` |
| "list at +0x68, state +0x70 14/16" (design-notes.md:8) | In TwlSDK's OSThread, +0x64 = state (OSi_SelectThread tests `[t+0x64]==1`, 0x02053D70), +0x68 = next, +0x6C = id, +0x70 = **priority** (OS_GetThreadPriority = `ldr r0,[r0,#0x70]; bx lr` at 0x02053E48). The 14/16 writes (0x02037348-0x0203735C, 0x02038CB8-0x02038CCC) are a **mutex unlock that restores the owner's priority** (16 if id==0, else 14) after a ceiling boost (`cmp [owner+0x70], #2`), and then re-sorts the thread list | gs 0x02036D9C-0x02037434, 0x02038B94-0x02038D2C |
| GS DD hot spot "nds-bootstrap's CE9 at 0x023BF640-0x023BF6BF (11.5%)" (F17) | 0x023BFxxx is nds-bootstrap's **bootloader ARM9 stub** in the boot frames. frames.csv has `arm9_pc` = 0x023BFxxx in exactly the first 185 (GS), 184 (CL) and 380 (DKR) intervals and never after. Not a gameplay wait | idle-census.md:44-46; evalspeed-team/gamecost-notes.md:9-14; frames.csv |
| D1 §6: the busy class is unreachable because "the ARM9 can't observe the ARM7, DMA or VCOUNT writers" | True for an *uncertified* loop. For a loop that passes FP, VCOUNT/timer/I/O and uncached reads are excluded by construction. ARM7/DMA writes to cached lines are invisible to the stock spinner until an ARM9 allocation, which in a pure spin happens only in IRQ handlers (§3.2). So the objection does not apply to certified loops | GBATEK gbatek.txt:7346-7349, 12764-12769 |

---

## 2. The three engines

All census shares below exclude the boot-stub bucket (0x023B0000-0x023C0000). The census runs had no `--thumb-cycle`, so
game work ran too fast and **every spin share is an upper bound** (brief caveat; DKR is Thumb-heavy, so its skew is
likely the largest).

### 2.1 Diddy Kong Racing DS: a game thread spinning on a ring buffer (spin engine)

Thumb, `dkr.thumb.dis`:
```
0201A344 push {r4,lr}; r4=r0
0201A348 r0=r4; bl 0x0201A294; b 0x0201A348        ; for(;;) consume(obj)
0201A294 push {r4,lr}; r4=r0
         r1=[r4+0x84]; r0=[r4+0x80]; cmp r1,r0
         beq 0x0201A340                            ; empty: pop {r4,pc}  (no store except push/pop)
0201A2A6 ... ring of 3 (mod-3 index), bl 0x0201A350 work, [r4+0x8C]++, ..., loop while [+0x84] != [+0x80]
```
- **Empty-path iteration:** about 14 instructions. Its only stores are the `push {r4,lr}` of unchanged values below sp.
  Its loads are [obj+0x80] and [obj+0x84].
- **Census** (`diddy-kong-racing-ds-8k/hot.csv`, 7,237,172 samples):
  - empty check 41.3%, outer loop 19.6%, the `pop` 16.5%;
  - drain work 0.8%;
  - **so 77.4% of all samples, ≥ 82% of non-boot samples, are in the empty path.**
- **PC at VBlank** (frames.csv `arm9_pc`, written in `on_vblank`, census main.cpp:193-211): in the empty path in 64.6%
  of non-boot frames, and **80.1% in the in-engine segment, intervals 3000-7000**.
- **IRQs:** about 12.9 dispatcher entries per frame (`report.txt` IF/IE reads at the ITCM dispatcher 0x01FFF87C).
- **Who writes [obj+0x80]:** not determined statically. The entry 0x0201A345 is not stored as a literal anywhere in the
  static binary (searched), so the thread is created from an overlay or a computed pointer, and **its priority is
  unverified**. By the SDK invariant the writer cannot run while this thread spins unless an IRQ makes it READY. That
  is all the certificate needs (§3).

### 2.2 Code Lyoko: busy VBlank wait with a service poll (spin engine)

ARM, `cl.arm.dis`:
```
020571C4 r5=&flag(0x020FE438); strb 0,[r5]
020571D0 if (byte[r5]) goto out
020571DC r4=&obj(0x020FF550)
020571E0 r0=[r4]; bl 0x0208D3E0 (poll); if (!byte[r5]) goto 020571E0
0208D3E0 push {r4-r6,lr}; sub sp,#0x130; r6=r0
         if ([r6+8]==0 && byte[r6+0x78]==0) { add sp,#0x130; pop {r4-r6,pc} }   ; pure early-out
         else ... vtable read of up to 1024 bytes (0x0208D45C blx [[r6+0x88]]+0xC)   ; real work
```
- **The flag writer** is the game's VBlank IRQ handler 0x0204D928:
  - `[0x020FE40C]++; strb 1,[0x020FE438]; ...; [DTCM+3FF8] |= 1`.
  - It is registered by 0x02003064-0x02003078: `OS_SetIrqFunction(1, 0x0204D928)` (0x0200896C, the SDK bit-loop shape),
    then `OS_EnableIrqMask(1)` (0x020087DC: IME=0, IE|=mask, restore).
  - This confirms the brief's "flag set by an IRQ".
- **Census:**
  - Loop 15.1% + poll early-out 24.2% = **39.3% of all samples (≥ 42% non-boot)**.
  - Poll real work: 0 samples in 0x0208D408-0x0208D800 during this run.
  - PC at VBlank in the spin: **78.8%** of non-boot frames.
  - About 2 IRQs per frame (IF/IE reads at 0x01FF8018).
- Code Lyoko has no RA set (idle-census.md:61). It stands for the class.

### 2.3 Golden Sun: Dark Dawn: a thread-churn engine with no provable spin

- **The mixer worker thread** (entry pointer 0x02030390 stored at 0x0202F758):
  `for(;;){ OS_SleepThread(q) [0x02053C78]; while(count>0 || OS_GetThreadPriority(self)!=14) { count--; every 4th pass: [x+0x10E0]++, OS_WakeupThread(q2) [0x02030414]; pass() [0x02036D9C] } }`.
  - `pass()` locks a mutex per slot for 10 slots (0x02054294 = lock; the unlock is inlined and calls OS_WakeupThread
    0x02053CC8).
  - It calls virtual slot methods and moves a write cursor [r5+0x168] by 680 toward the play cursor [r5+0x16C].
  - **Every pass stores** (count--, the every-4th counter, mutex owner/count/priority, a list re-sort). So it is never
    a fixed point, and fewer passes would change when q2's worker wakes.
- **The main thread waits for VBlank through the SDK.** 0x02055A68 = `SVC_WaitByLoop(1); OS_WaitIrq(1, VBLANK)`
  (0x02052E20 sleeps on the IRQ thread queue via OS_SleepThread). So the SDK idle thread would run if every thread
  blocked. It runs in 0.1% of time (`report.txt`), which means some thread is READY almost always.
- **Census:**
  - OS code 0x02053000-0x02056000: 28.3%, plus OS_Disable/RestoreInterrupts 6.5%.
  - The pass region: 14.1%.
  - An unattributed BIOS Thumb loop at 0xFFFF07CC/07CE: 3.6%.
  - PC at VBlank: spread over 15+ 256-byte blocks, none above 8%.
- **Reading (inference):** GS DD is CPU-bound by thread churn, not spinning. FP can't certify it, so GS DD needs the
  fallback (§5.8) every frame. Its ARM9-side housekeeping (network, notices) must live on the ARM7.

---

## 3. The signal: the fixed-point (FP) certificate

### 3.1 Definition

Input: an interrupted game context:
- r0-r12, sp, lr and pc;
- CPSR mode SYS or USR, I = 0;
- IME = 1;
- not nested: the BIOS frame's SPSR mode is not IRQ.

FP runs a **sandboxed, effect-free replay**. A small ARMv5TE ARM+Thumb interpreter executes from the interrupted PC on a
copy of the registers until the PC returns to its start or a budget of **B = 96 instructions** runs out. Loads read real
memory through the normal ARM9 D-cache. Stores go to a private overlay and never reach game memory. The certificate
holds iff all of the following are true.

1. **Closure:** the replay returns to the start PC with *all* registers, the CPSR flags/T bit and sp equal to the start.
   Conditional paths are those the current memory values select.
2. **No escaping effects:**
   - Every store targets the thread's dead stack, i.e. `[sp_min, sp_start)` where sp_min ≥ sp_start - 512. Nothing
     reads below sp in correct code: SDK IRQs use the IRQ stack, and SDK thread switches save context into the OSThread,
     not the thread stack.
   - No SWP, SWI, BKPT, CDP/MCR/MRC or undefined opcode.
   - No MSR other than a CPSR control write that the iteration itself restores, e.g. OS_Disable/RestoreInterrupts pairs.
3. **Only IRQ-gated loads.** Each load address is classified against the **live PU configuration**. The module reads
   CP15 c6 region bases/sizes and c2 data-cacheable bits; reading CP15 has no side effect. A load is allowed iff it is
   in one of:
   - DTCM: neither ARM7 nor DMA can reach it (gbatek.txt:12764-12766);
   - ITCM: same;
   - a **data-cacheable** main-RAM or WRAM region;
   - BIOS ROM: constant.

   Any load from I/O 0x04xxxxxx, VRAM/palette/OAM, an uncached region (e.g. NitroSDK region 7 0x027FF000, region 2), or
   any address the PU denies, **rejects** the certificate. That excludes VCOUNT, timers, GXSTAT, IPCSYNC/FIFO, DMA busy
   bits and the divider.
4. **No self-eviction:**
   - The replay's distinct data lines, literal-pool reads included, number ≤ 4 per D-cache set. That is 32 sets: 4 KB,
     4-way, 32-byte lines (gbatek.txt:7342-7345). The set index is address bits 5-9 **(guess on index bits; standard
     for this geometry)**.
   - This makes the stock loop's own iterations all hits after its first one.

A certificate is **memoised** as `(start PC, the register file, a list of (address, width, value) loads in order)`.
- Determinism argument: the replay is a deterministic function of the registers and the loaded values. If every load in
  the list returns the same value again, the whole iteration is the same, so the certificate still holds. Checking the
  memo costs one load and compare per entry: DKR 2 entries, Code Lyoko 4.
- Memo invalidation:
  - when CE9 completes any card read (overlay loads can replace code); CE9 bumps a generation;
  - when a T hand-back sees a changed value.

### 3.2 Why a certified loop can only exit at an ARM9 IRQ (the proof)

Facts used:
- **F-A.** DMA cannot access DTCM/ITCM (gbatek.txt:12764-12766). The ARM7 has no path to the ARM9 TCMs; GBATEK lists none.
- **F-B.** The ARM9 D-cache is read-allocate ("writes are not allocating cache lines", gbatek.txt:7346). The CPU sees
  writes by other masters only after the line is re-fetched: "invalidate the cache after DMA-writes" (gbatek.txt:12767).
  There is no snooping.
- **F-C.** A cache line is replaced only when a line is allocated (a read miss or PLD) or by CP15 maintenance. Round-robin
  and pseudo-random replacement act at allocation (gbatek.txt:7347). This is ARM946E-S architecture as GBATEK describes
  it. It must be **confirmed on hardware (H-B1)** because GBATEK doesn't say "only".
- **F-D.** NitroSDK/TwlSDK scheduling is strict priority, with no time slicing: the selector picks the first READY
  thread in a priority-sorted list (0x02053D58-0x02053D7C).

Claim: while a certified spinner runs in stock (no IRQ taken), the values its loads return are constant.
- **TCM words:** only ARM9 instructions write TCM (F-A). The only ARM9 instructions executing are the spinner's, and it
  doesn't store there (condition 2).
- **Cached words:** after the first iteration every line in the read set is resident (read-allocate) and stays resident.
  The spinner allocates nothing new (condition 4) and nothing else on the ARM9 allocates (F-C). So the spinner reads its
  cached copy whatever the ARM7 or DMA write to RAM (F-B).
- **Other masters' writes:** they become visible only after an eviction, and an eviction needs an ARM9 allocation, which
  in a pure spin happens only inside an IRQ handler.
- **Other ARM9 threads:** they can't run unless an IRQ makes one READY (F-D), because the spinner wakes nobody (condition
  2: no OS call with stores).
- So the first moment any input can differ is the first instruction after an ARM9 IRQ handler returns. Until then every
  iteration is identical and **the loop makes no progress**.

Consequence for W:
- Replacing the spinner's instructions with W's, from a certified state until the next IRQ, removes no work the game
  would have done.
- At that IRQ W parks (D1 §2.3), and the game's handler runs with the spinner as the interrupted context, exactly as in
  stock.
- The only difference is that the handler starts X cycles later.
- W's own memory traffic may evict the spinner's lines, so after unpark the spinner may see an ARM7/DMA write **earlier**
  than stock would, never later. A correct program can't depend on a stale line surviving, because eviction timing is
  architecturally unspecified (F-C). This is "never later than stock", not "identical": §6, E7.

### 3.3 What can be proven and what cannot

| Exit-input writer | Certified-loop status | Provable? |
|---|---|---|
| ARM9 IRQ handler (Code Lyoko flag, SDK OS_WakeupThread of a producer) | exit seen at the first iteration after the handler, X later than stock | **yes**: exit timing differs by X only |
| Another ARM9 thread (DKR producer) | can only become READY via an IRQ (F-D) | **yes** |
| ARM7 or DMA write to a *cached* line | invisible to stock until an IRQ-time allocation evicts the line; with W, visible no later | **yes**, given F-C (H-B1 confirms) |
| ARM7 or DMA write to uncached RAM (PXI shared area, uncached mirrors) | load rejected | n/a: never certified |
| VCOUNT, timers, I/O status (GX, DMA busy, card, IPC FIFO, divider) | load rejected | n/a |
| Time-based waits (SVC_WaitByLoop, OS_SpinWait 0x02055A40-style, tick compares) | SWI or I/O rejected; a pure ALU countdown has no fixed point (register changes) | n/a |
| Cache maintenance inside the loop (DC_Invalidate, then read) | MCR rejected | n/a |
| A loop that is pure on *some* iterations (CL poll with work pending; DKR with a non-empty ring) | certified only when the current memory selects the pure path; re-checked after every IRQ (T, §5.5) | **yes**, per iteration |
| Free-running work with side effects (GS DD mixer passes) | not a fixed point | not harvestable; any harvest changes the game's evolution |

Not provable, only bounded or measured:
- **U1. X:** IRQ-delivery latency while W runs: worst W instruction, BIOS entry twice, `irq_head`. D1 estimates 300-800.
- **U2. P:** cache refill in the game's next work after W.
- **U3. Bus contention:** the stock spinner runs entirely from cache and uses no bus. W does, so the ARM7, sound and DMA
  can see contention.
- **U4. F-C on real silicon (H-B1).**
- **U5. Interpreter fidelity (B2 oracle).**
- **U6. Stale I-cache:** if a game rewrites code without invalidating the I-cache, native execution and a D-side replay
  differ. Such a game is broken anyway; a memo dies with the CE9 read generation.
- **U7. Earlier visibility of external writes** (§3.2), and timing-dependent game logic such as GS's VCOUNT load meter
  seeing shifted values.

---

## 4. Why not the SDK thread-state signal, and why not a boot patch

- **"All threads but the lowest-priority runnable one are blocked"** holds for every running thread under F-D. It can't
  tell DKR's empty-ring spin from DKR's 3D drain or GS's mixer passes, which run on the same threads. **Rejected as a
  progress signal.** It stays a cheap *precondition*: the interrupted mode is SYS/USR and the thread is the SDK current
  thread. It is not needed for soundness.
- **A boot-time patch of loop back-edges** found by a structural pattern (e.g. DKR's `bl X; b back`, which is the
  OS_Halt idle-loop shape):
  - It would give a zero-cost entry for DKR, because the outer back-edge runs only after a drain.
  - But it is a code patch of arbitrary game loops chosen by shape. That is close to a per-game patch in risk.
  - It is not path-sensitive: DKR's back-edge runs after real work too.
  - It needs overlay-time patching.
  - **Rejected.** The probe plus T achieves the same entry at runtime without touching game code.
- **Writing the BIOS IRQ frame's return PC** (redirect the spinner to T from inside a game IRQ): this writes the game's
  DTCM IRQ stack. **Rejected:** "never write game RAM", and the round-1 precedent covers only 3FF8/3FFC.

---

## 5. Mechanism: the spin gate (SG)

SG is a third gate kind next to D1's BIOS-wait gate and idle gate. It uses the same W, the same 3FFC park and the same
`wait` record (`wait.kind = SPIN`), so the display strip gets "game is waiting" in spin engines too.

### 5.1 Busy mode (runtime, no list)

- Per frame the module counts gate entries (E or the idle gate).
- **Busy mode** is on after 8 consecutive frames with no gate entry, and off at the next gate entry.
- Only busy mode arms probes. DKR's menus use OS_Halt (4.4% halted over the 8k run), so it switches mode by scene.
- Negative control: KSSU, MKDS, Sonic Rush and other waiting games must never enter busy mode in play (B3, M12-style).

### 5.2 The probe (entry), two variants

A probe is an IRQ that interrupts the game at a learned time t_p in the frame, so that the head sees the interrupted
context with nothing else of the game's pending.

**Probe source.** NDMA completion IRQs (IE 28-31) are unavailable in DS mode, because SCFG_EXT9 bit 8 is 0 and locked
(rnd `r2-ndma.md:23-24`), and NDMA2 is the only channel not claimed (`r2-ndma.md:28-31`). So:
- **S-a (no new SCFG):** an **ARM7-sent IPC sync** with a new value 0xB.
  - CE7 already sends 0xA every VBlank (`cardengine.c` arm7: `dscheevosVBlank`, 2062-2064).
  - CE9 already enables IE bit 16 (`misc.c:128-133`) and owns the game's IPC table entry (`misc.c:118-121`). So no game
    code handles IPC sync IRQs today, and consuming one adds no game-visible handler.
  - Timing source on the ARM7 is **open (guess)**: the ARM7 idle-loop entry from D1 P2 can poll VCOUNT while the ARM7 is
    idle, at no game cost, or CE7 can arm an ARM7 VCOUNT-match IRQ, which needs a check that the game's ARM7 doesn't use
    it.
- **S-b:** NDMA2 interval-delay completion IRQ, armed by the ARM9. It needs a ruling to set SCFG_EXT9 bit 8 before the
  lock (shared with display's request) and NDMA2 (rnd/display). It is cleaner on the ARM9 but has more ownership hurdles.

**Getting the interrupted context: two variants of the armed window.**
- **V1 (head armed through the window):**
  - `vblank_entry` (already in game IRQ time each VBlank) sets `[DTCM+3FFC] = sg_head` once the busy-mode probe is
    scheduled.
  - Every game IRQ until the probe goes through `sg_head`. That is a pass-through: no module bit pending, so
    tail-jump to `game_vec` with the BIOS frame untouched (D1 §2.3 case 3).
  - Cost: c_pass per game IRQ in the window.
  - The probe IRQ (IPCSYNC in-value 0xB, IF == bit 16 only, no CE9 message pending: `sharedAddr[4] != 'DMA9'`) is
    consumed by `sg_head`. It acks IF bit 16 exactly as CE9's handler would and reads the context from the BIOS frame.
- **V2 (two-stage):**
  - Probe-1 goes through the game's dispatcher to CE9's handler. There the module only installs `sg_head` and asks the
    ARM7 for an immediate echo (ARM9→ARM7 IPC sync with the "IRQ to remote" bit).
  - The echo (0xC) arrives µs later, after probe-1's IRQ has returned to whatever the SDK resumed, and is caught by
    `sg_head`.
  - The window holds almost no game IRQs, but a probe-1 that lands in work costs a full dispatcher pass.
  - Choose V1 or V2 by B4.

**Invariants** (extending D1 I1/I2):
- **I1'.** While `sg_head` is installed and W is *not* running, every game IRQ reaches `game_vec` with the BIOS frame
  unchanged, and with IE and IF unchanged except a consumed probe bit.
- **I2'.** A module IRQ source can raise IF only while `sg_head` is installed. With S-a there is no module IE bit at all,
  because IPC sync's IE bit is CE9's. This removes D1's I2 hazard of a game seeing an extra IE bit.

**Schedule (learned per session, no list).**
- t_p starts at line 180 (near the VBlank-in-spin shares of 79-80%).
- Per frame, move earlier by 8 lines after a hit and later by 16 after a miss, subject to a miss budget:
  E[c_miss] ≤ **B_miss = 112 cycles/frame (0.01%)**, an owner-adjustable target.
- At most 2 probes per frame.
- A hit with T re-entry keeps W in the spin across later IRQs (§5.5), so one hit per spin episode is enough.

### 5.3 Certification at the probe

`sg_head` checks, on the probe and in order:
1. **Context precondition:** interrupted SPSR mode SYS/USR, I = 0, not nested. Otherwise miss: return through the BIOS
   epilogue.
2. **Memo hit** for (PC, registers). If it hits, compare the memo's loads: equal means certified.
3. **Otherwise W replays:** save the context (below), enter W, run FP with budget B, IRQs enabled. Any IRQ aborts the
   replay, which is invalid because game code ran: it parks, and the attempt counts as a miss.
   - Certified: record the memo and continue as W (§5.4).
   - Rejected: resume the spinner at its exact PC. The miss cost is the replay plus a resume.
4. **Per-PC rejection cache:** a PC that failed FP is not replayed again for 64 frames. This bounds repeated misses on a
   hot work PC.

### 5.4 Entering W from a certified spin

- **Save the spinner's context into module memory (reads only):**
  - r0-r3, r12 and pc from the BIOS frame;
  - r4-r11 live;
  - SYS sp/lr through a mode switch;
  - SPSR.
- Set SP_irq back over the frame, as the BIOS epilogue would. That changes the register only, with no memory write.
- Switch to SYS mode on the module stack with `[3FFC] = irq_head` (D1's park head) and `wait.kind = SPIN`, then resume W.
- **W's work in a spin, in priority order:**
  1. Capture (§5.6).
  2. Network and housekeeping, gated once per frame (D1 §2.4).
  3. ARM9 evaluation: only for sets that the ARM7 evaluator can't hold, i.e. D1 P2's one-evaluator-per-session choice.

### 5.5 Unpark and the re-entry trampoline T

Game IRQ while W runs: D1's `irq_head` non-module branch.
1. Restore `[3FFC] = game_vec`.
2. Save W's context.
3. Restore the spinner's registers, but set the resume PC to **T**, in ARM state.
4. Record `{sp, orig_pc, orig_T_bit, memo_id}` in a 4-entry table keyed by the spinner's sp.
5. `movs pc`.

IF is still set, so the BIOS takes the IRQ at once with T as the interrupted PC. The game's handler runs, and the SDK may
switch threads, saving PC = T in the spinner's OSThread. That is the same class as D1 G7 (a module PC in a saved thread
context during a wait; D1:235). When the spinner next runs, T executes in the spinner's thread:

```
T:  mask I (one msr; restored before leaving)          ; T is then atomic: no switch mid-check
    save 2 scratch regs PC-relative into module memory ; no game stack, no game RAM
    look up entry by sp
    re-check the memo loads (2-4 loads)
    equal  -> re-enter W (as 5.4, W resumes where it parked)
    differ -> restore scratch, msr cpsr_f flags, unmask, ldr pc -> orig_pc|T   ; ARMv5 LDR PC interworks
```
- T never writes game memory, and its entry is freed only when T consumes it.
- If the table is full, unpark resumes at orig_pc directly: no T, so no re-entry for that episode.
- A thread destroyed while holding PC = T leaks its entry until the same sp value is installed again. Live threads have
  disjoint stacks, so an sp key can't collide.
- After the module is disabled (`hk.disabled`), T only hands back.
- Reset or quit: the game's threads die with the game. `reset_entry` clears the table.
- **T's game-time cost appears only at hand-back** (c_T: one masked span of about 20-40 instructions and a likely I-cache
  miss), which happens when an IRQ actually gave the spinner work. A T re-entry costs nothing, because that time is
  certified spin.

### 5.6 Capture at the frame boundary (RA semantics)

RetroArch evaluates memory as it is at VBlank, lag frames included (brief).

- **Spin-captured frame:** W makes complete copies during certified spins; a park marks the copy in progress as torn,
  and W restarts it at the next resume (D1 §2.4 rule).
  - Frame F uses copy C iff `C.done_gen == unpark_gen` at the unpark caused by F's VBlank IPC (`vblank_entry`),
    i.e. no game instruction ran between C's completion and that IRQ.
  - By §3.2, memory as the ARM9 sees it is then exactly memory at that IRQ.
  - W starts the final copy only when `lines_to_VBlank × 4,260 > 2 × recent max copy cycles`, using VCOUNT, the only
    timing source used.
- **Fallback frame** (VBlank lands in game work, ~20% of frames in DKR/CL, 100% in GS DD):
  - In `vblank_entry` (the existing IPC IRQ), the ARM9 cleans by MVA the D-cache lines of the plan's direct addresses and
    its last-known chain targets, and copies the plan's DTCM words.
  - rnd's gather then produces the coherent copy and the ARM7 evaluates. In GS DD's case the GSDD CE9 must first gain
    the hook (§0.5).
  - Cost c_fb is **est.** 0.1-2k cycles in game IRQ time: D1 §6 option B, ~16-34 MCRs plus dirty write-backs.
  - What this proposal needs from rnd: **T = the VBlank IPC IRQ of frame F**; the copy ready by about line 200;
    **ARM9 cost X ≤ the clean only** (no pointer chasing on the ARM9 beyond the clean list); zero ARM9 cycles for the
    gather itself; any bus stall it causes reported.
- **No frame goes unevaluated:** every frame has a spin capture or a fallback capture. The ruling "every frame, every
  game" is met. The fallback is the only part that is never free.

### 5.7 Coexistence and display

- SG is armed only in busy mode, so waiting games keep D1's gates unchanged.
- `wait.kind = SPIN` with the PC ranges "module image ∪ certified loop range" can drive the strip.
- Strip IRQs during W are module-only wakes (D1 §4), handled without unparking.

### 5.8 What each engine gets

| Engine | Spin share (upper bound) | VBlank in spin | ARM9 time for W per frame (**est.**) | Frames needing fallback |
|---|---|---|---|---|
| DKR in-engine | ≥ 82% of samples | 80% | up to ~0.8 × 1.12M ≈ 900k cycles, minus probe ramp-up | ~20% |
| Code Lyoko | ≥ 42% | 79% | ~470k | ~21% |
| GS DD | 0 provable | — | 0 | 100% (needs the GSDD CE9 hook first) |

The DKR and CL numbers come from a census without `--thumb-cycle`. Re-measure them (B1).

---

## 6. Every game-visible effect (complete list)

| # | Effect | When | Bound / test |
|---|---|---|---|
| E1 | `sg_head` pass-through on game IRQs in an armed window (c_pass, **est.** 40-100 cycles each; cold I-line from module RAM ≈ 46 cycles, gbatek "cache miss 8-word load = 23" bus cycles) | busy mode, V1 window | counted per IRQ (B4) |
| E2 | Probe IRQ that lands in game work (c_miss: BIOS entry/exit + head + bounded replay; **est.** 150-300, ≤ ~1.3k at B = 96 with ~10 cycles per replayed instruction) | ≤ 2 per frame, budgeted | B4 |
| E3 | X: IRQ that ends a W slice is delivered later (D1 300-800) | each IRQ during W | B5 per IRQ kind (D1 M4) |
| E4 | P: cache refill after W | game work after a spin | game ARM9 cycles per frame off vs on (B3, H-B2) |
| E5 | T hand-back (c_T **est.** 30-100), one masked span ≤ 40 instructions | an IRQ gave the spinner work | B4; masked-span inventory (D1 M11) |
| E6 | Spinner's saved OSThread PC = T | while switched away after an unpark | precedent D1 G7 |
| E7 | External writes to cached lines seen *no later*, possibly earlier, than stock | after W | allowed by F-C; H-B1 |
| E8 | 3FFC holds `sg_head` or `irq_head` | busy mode | D1 Q1 class; M5-style audit |
| E9 | IF bit 16 acked by `sg_head` for a consumed probe (CE9's handler acks the same bit for every IPC sync today) | probe | audit |
| E10 | Bus contention from W (stock spinner uses no bus) | W | H7-style ARM7 PXI/card latency A/B |
| E11 | Fallback clean c_fb in the VBlank IPC IRQ | fallback frames | B3; rnd's report |
| E12 | One more IPC sync per probe on the ARM7 (S-a) | probe | ARM7 idle share A/B |

Not written: game heap, data, stacks, the IRQ table, IE (with S-a), timers, the PU and the BIOS frame.

---

## 7. Minimum cost (not provably zero)

Game cycles charged per frame:

  C = N_pass·c_pass + N_miss·c_miss + N_Tback·c_T + [fallback frame]·c_fb

Latency-only effects: X per IRQ during W, and P after W.

- **Floor:** the probe always lands in a certified spin, VBlank always lands in a spin, V2 is used (N_pass ≈ 0), and the
  spinner gets no work between IRQs. Then **C = 0 cycles of game work**, and the residual is X and P. These are exactly
  the residuals that D1 already accepts for the BIOS/OS_Halt gates (Q3).
- **Typical (est.):**
  - DKR: 13 IRQs/frame × 0.5 window × 60 (c_pass, V1) + 0.2 × 300 (c_miss) + 2 × 60 (c_T) + 0.2 × 1,000 (c_fb)
    ≈ **770 cycles/frame ≈ 0.07%**.
  - Code Lyoko: ≈ 400 cycles/frame.
  - GS DD: c_fb only, ≈ 0.2-2k cycles/frame (0.02-0.2%), which is D1's option B cost.
- **Worst case per frame** (2 probes, both at the replay budget, a fallback with many dirty lines): ≈ 2 × 1.3k + 2k +
  window pass-through ≈ **5k cycles ≈ 0.45%**. The per-PC rejection cache and the miss budget keep this rare.

The test that must show no lag frames (pre-registered, §9):
- **B3** (melonDS, deterministic input scripts, cache model, `--thumb-cycle`): lag(SG) ≤ lag(off) on each busy title,
  where lag is counted with a game-progress counter.
- **H-B2:** the same on the DSi XL over 30 minutes.
- The brief's rule: report the game's own ARM9 cycles per frame off vs on for Kirby, MKDS and DKR.

---

## 8. Interfaces with other sessions

- **perf (owner of module memory/IRQ):**
  - `sg_head`, T, the FP interpreter (**est.** 4-6 KB code), the memo and T tables (< 1 KB) and the context save area.
  - It shares W, `irq_head`, `wait` and the reset path with D1.
  - The header ABI gains `sg_head` and T entry offsets.
- **rnd (data gather):** T, X and readiness as stated in §5.6; NDMA2 or SCFG bit 8 only if S-b is chosen.
- **arm7eval:** the busy class depends on the ARM7 evaluator for GS DD and for every fallback frame. The ARM7 also
  timestamps probes (S-a) from its idle loop.
- **display (dscheevos-43):** SCFG bit 8 (S-b), the NDMA channels, and the use of `wait.kind = SPIN`.
- **nds-bootstrap upstream footprint:**
  - CE7: probe timing and the 0xB/0xC sends.
  - CE9: a probe-1 hook (V2 only) and a read-generation counter.
  - The GSDD CE9 must gain `hookIPC_SYNC` and the DScheevos call (today `#ifndef GSDD`).
  - These are additions to existing hook points, not new ones, except the GSDD fix.

---

## 9. Measurements (pass/fail fixed now)

Runner: the melonDS fork with the cache model and `--thumb-cycle`, one instance at a time (machine rule). Hardware: DSi
XL, logs to RAM, dumped at quit, written up in TESTING.md. Test-only per-game addresses (progress counters) are allowed in
test harnesses. They are never part of the mechanism.

| ID | Where | What | PASS | FAIL → |
|---|---|---|---|---|
| B1 | melonDS | Re-run census for DKR (in-engine), CL and GS DD with `--thumb-cycle`, excluding boot-stub frames. Add per-sample current-OSThread attribution (the TwlSDK/NitroSDK ThreadInfo current pointer). Report spin share, VBlank-in-spin share, IRQ rate, spin-episode lengths | DKR and CL spin share ≥ 30% and VBlank-in-spin ≥ 50%; GS DD classified (expected: no fixed point) | spin too small: SG's value drops, fallback dominates |
| B2 | melonDS oracle + lab ROM | The FP certifier compiled for the host, fed emulator state at every probe-equivalent sample. On each "certified", the emulator continues natively and records (a) any non-dead-stack store by the spinner and (b) any loop exit before the next ARM9 IRQ. Lab loops: VCOUNT poll, timer poll, uncached ARM7-written flag, cached ARM7-written flag, DMA-written flag, IPC FIFO poll, OS_SpinWait, counter-store loop, PLD loop, mutex-pass (GS-like) | 0 violations over ≥ 100,000 certifications (DKR, CL, 10 OS_Halt games as negative controls); every lab loop classified as §3.3 predicts | fix the interpreter, or drop the class that fails |
| H-B1 | DSi XL lab ROM | The ARM9 spins on a cached word (lines resident, ≤ 4 per set); the ARM7 writes it; run (a) no ARM9 IRQs, (b) an IRQ whose handler touches 0 lines, (c) an IRQ handler that allocates 4 lines of that set; 10,000 trials each | (a) and (b): never observed within 10^7 cycles; (c): observed at the first iteration after the handler | F-C fails: certificate must reject all cached main-RAM loads not provably ARM9-written. DKR/CL coverage falls to fallback |
| B3 | melonDS | Lag A/B: off vs SG, same input scripts. DKR 4,000 in-engine intervals, CL 2,500, GS DD 1,500, KSSU + MKDS (negative controls: busy mode must never arm). Progress counters (test-only): DKR producer VBlank-wait calls per VBlank; CL entries of 0x020571C4 per VBlank; GS calls of 0x02055A68 per VBlank. Also game ARM9 cycles per frame excluding certified-spin cycles | lag(SG) ≤ lag(off) per title; game-cycles delta ≤ +1% (DKR), 0 (KSSU/MKDS: SG never armed); every frame has a spin or fallback capture (`frames_uncaptured == 0`) | owner, with the numbers |
| B4 | melonDS | Cost accounting per frame: N_pass, N_miss, N_Tback, c_* measured in cycles, V1 vs V2 | mean C ≤ 0.05% of a frame, p100 ≤ 0.5%; V-choice picked by lower mean C | tighten the miss budget or move to V2 |
| B5 | melonDS, then DSi XL | X per IRQ kind while W runs in a spin (D1 M4 method) | Δp100 vs stock ≤ 800 cycles for VBlank/IPC/timer; raster IRQs ≤ 300 and pixel-identical frame buffer | per-kind yield rule (D1 risk 2) |
| B6 | melonDS | Capture equivalence: spin captures vs a perfect runner snapshot at each VBlank IRQ, 10,000 frames DKR | identical on every spin-captured frame | fix the generation rule |
| H-B2 | DSi XL | DKR 30 min in races, SG vs off. Module logs per VBlank: spin episodes, probes, hits, T re-entries; the race frame counter (test-only) | lag-interval rate SG ≤ off + 0.05 pp; no audio glitch (Clark); game cycles per frame (sampled by VCOUNT stamps at the spinner's first certified entry) shift ≤ 1% | owner (Q-R2) |

---

## 10. Owner rulings needed

1. **R1. Sandboxed analysis interpreter:** a replay of ≤ 96 game instructions on copies of registers, with stores
   discarded and nothing executed for effect. It reads game memory and writes nothing.
   - **Recommend:** this is analysis, not "emulation". The game runs natively throughout.
   - The alternative without an interpreter is a decode-only verifier for straight-line loops. It is smaller but covers
     fewer engines: CL's poll has a branch, so it needs path selection by current values.
2. **R2. The residual costs in §7** (c_pass, c_miss, c_T, c_fb), in exchange for evaluating every frame of busy
   engines.
   - **Recommend:** accept under the D1 Q3 standard: no lag-frame difference, with absolute bounds reported.
3. **R3. Spinner contexts may hold PC = T** (D1 G7 class).
4. **R4. Probe source:** S-a (ARM7 IPC sync, no SCFG change; recommended) or S-b (SCFG_EXT9 bit 8 plus NDMA2, shared
   with the display ruling).
5. **R5. GS DD:** accept that GS-class engines (no fixed point) get fallback-only coverage at c_fb per frame, plus ARM7
   evaluation and ARM7-side housekeeping. The GSDD CE9 also needs the IPC hook added.

---

## 11. Risks (ranked)

1. **F-C on hardware.** If lines can be evicted without an ARM9 allocation (an undocumented prefetch or a write-buffer
   interaction), cached external writes could end a stock spin outside IRQs. H-B1 decides before any game build.
2. **The busy class may include free-running engines like GS DD more often than spin engines** (the RA library share is
   unknown; busy ≥ 3/64). Those pay c_fb every frame and rely on the ARM7.
3. **Interpreter bugs** cause false certificates, which become real lag. B2's oracle gates shipping, and the replay
   budget is small.
4. **X and P in heavy frames** of spin engines: DKR's producer works right after VBlank with a cache polluted by W. B3
   and H-B2 decide. W's footprint shrinks when evaluation moves to the ARM7.
5. **Probe source timing (S-a) on the ARM7 is unsettled.** The ARM7 idle entry (D1 P2) or a VCOUNT-match IRQ needs a
   check against the game's ARM7 use.
6. **melonDS figures are upper bounds:** the Thumb timing bug and no cross-CPU contention model (D1 risk 8).
7. **DKR's spinner priority and producer are unverified statically** (§2.1). The certificate doesn't depend on them, but
   the expected W time does.

## 12. Footprint

- FP interpreter (**est.** 4-6 KB)
- `sg_head`, T and the tables (~1.3 KB)
- No new stacks: W's stack is D1's.
- The NWRAM and ARM7 parts belong to D1 P2 and rnd.
