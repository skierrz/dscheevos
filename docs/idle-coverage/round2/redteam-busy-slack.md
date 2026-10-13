# Red-team: proposal-busy-slack.md (spin gate SG + fixed-point certificate)

Verdict: **fixable.** The core idea holds up: the FP certificate (a fixed-point iteration whose only inputs are TCM
words or resident cached lines, so they can change only after an ARM9 IRQ) is the first generic and sound basis for
borrowing busy-engine time. But four parts of the mechanism are wrong as written: the frame-boundary rule, the 3FFC
window, trampoline T, and probe consumption. The cost math is off by about 10x on misses. GS DD, the one
non-spin engine, is not actually covered.

What I checked: the disassembly listings in `design2/dis/` (GS 0x020545E0-0x0205464C, CL 0x020571C4-0x020571F0,
0x0208D3E0-0x0208D404, 0x0204D928-0x0204D9F0, 0x02003058-0x02003078, DKR 0x0201A294-0x0201A34E);
nds-bootstrap 32993f85 (`cardenginei/arm9/source/misc.c:112-133`, `cardengine.c:1620-1730`,
`card_engine_header.s:640-644`, `cardenginei/arm7/source/cardengine.c:2062-2064`), its git history (e038b45c); and
D1 `decision.md`. The emulator was not run.

## Confirmed claims (credit)
- GS DD 0x020545E0 is `IME=0; while(VCOUNT!=0); restore IME`, called only from 0x0205464C at the end of OS_Init.
  Round 1's "VCOUNT time-base spin" was a misreading. **Confirmed.**
- CL: `strb 0,[flag]; while(!flag) poll(obj)`, poll early-out `push{r4-r6,lr}; sub sp,#0x130; ... popeq`. The VBlank
  handler 0x0204D928 does `[0x020FE40C]++; strb 1,[0x020FE438]` and is registered at 0x02003064-0x02003078.
  **Confirmed.** The dead-stack span is 0x140 bytes, within the 512-byte bound.
- DKR: the empty path `push{r4,lr}; ldr [+0x84]; ldr [+0x80]; cmp; beq -> pop{r4,pc}` and the outer
  `bl 0x0201A294; b 0x0201A348`. **Confirmed.** Closure holds from every PC in the loop: the outer loop overwrites r0,
  and lr stays 0x0201A34F.
- The GSDD CE9 compiles out `hookIPC_SYNC`/`enableIPC_SYNC` and the body of `myIrqHandlerIPC`. **Confirmed.** It is
  worse than stated (B-S4).

## Blockers (as written; each has a fix)

**B1. The frame-boundary rule keys on the wrong IRQ.** §5.6 commits copy C for frame F iff
`C.done_gen == unpark_gen` "at the unpark caused by F's VBlank IPC (vblank_entry)". The IPC 0xA comes from CE7's
ARM7 VBlank handler (`arm7 cardengine.c:2062-2064`), so it reaches the ARM9 after the ARM9's own VBlank IRQ (IE
bit 0) at line 192. In a spin engine, bit 0 ends the W slice first. The game's VBlank handler then runs: in CL it sets
the flag and the spinner exits into the next frame's work. Then 0xA arrives. By that point game code has always run
since VBlank, so the rule as written fails on every frame (every frame falls to the fallback). If it is loosened to
"any unpark", it can commit the wrong copy.

Fix: irq_head records `{unpark_gen, VCOUNT}` at each unpark. The commit for F uses the **first unpark with
VCOUNT ≥ 192 since the previous commit** (line 192 starts VBlank). That is memory at the start of VBlank, the RA
boundary. B6 must compare against a snapshot at line 192, not at the IPC IRQ.

**B2. sg_head sits in DTCM+3FFC while game code runs.** This breaks D1's invariant I1, the Q1 ruling's scope, and
the display's exact signal.
- V1 keeps `[3FFC]=sg_head` through the armed window. V2 keeps it from probe-1 to the echo, and game code runs in
  both. D1 Q1 asked the owner only for "swap while no game instruction executes".
- The display's "every strip IRQ arrives while the game is waiting, by construction" (D1 §4) rests on I1/I2.
- I2' ("a module IRQ source can raise IF only while sg_head is installed") would let strip IRQs fire during game
  work. sg_head's behaviour on a pending module bit is not defined.
- Any game write to 3FFC inside the window is overwritten when the module restores `game_vec`: OS_InitIrqTable
  again, a library re-installing its handler, an in-game soft reset. That writes game RAM and breaks the game.
- Fix:
  - Restore with compare-and-adopt, IRQs masked: if `[3FFC] != sg_head`, take the new value as `game_vec` and leave
    busy mode.
  - Never arm module IE bits while only sg_head is installed. The display keys exactness on `irq_head` only.
  - Ask the owner for a new ruling Q1' that explicitly covers a game-time pass-through head. It is the same class as
    D1's rejected permanent dispatcher patch, at c_pass per IRQ.

**B3. Trampoline T: the hand-back is not atomic, and it can livelock.** D1 lists "no trampoline" as a reason the base
won, and it dropped WGC's trampolines (B3). SG reintroduces one without addressing the class.
- **(a) No atomic unmask and jump from SYS mode.** SYS has no SPSR, so `movs pc` and `ldm ^` are unavailable. The
  "differ" path must unmask (msr) and then `ldr pc` from a slot.
  - A single PC-relative target or scratch slot is shared by all threads. An IRQ taken between the unmask and the
    `ldr pc` can switch threads, and another spinner's T then overwrites the slot. When the first thread resumes, it
    jumps to the other thread's orig_pc. That is a crash.
  - The flags restore (`msr cpsr_f`) needs a register, so the order of the scratch restore is constrained as well.
- **(b) USR mode.** The precondition allows USR mode, where `msr` cannot set I. T is then not atomic at all.
- **(c) Pending IRQ not yet taken.** After the unpark's `movs pc` to T with IF still set, nothing guarantees the
  ARM946E-S takes the IRQ before T's first instruction (`mask I`) executes. If T masks first:
  - it sees the memo unchanged, because the game's handler has not run;
  - it re-enters W, W unmasks, irq_head unparks again;
  - each cycle adds a full X, and in the worst case it livelocks.
- Fix:
  - One T copy per table entry (T0-T3), each with its own literal and scratch slots, so a slot is never shared
    across threads.
  - SYS-only precondition.
  - T's first test is `IE & IF & IME` (I/O reads, module side). If anything is pending, unmask and let it be taken,
    then re-check.
  - D1's `e_after_irq` → `w_resume` path should be audited for the same (c) pattern.

**B4. Probe consumption by IPC sync value drops CE7 commands, and the "no game handler" claim is false.**
- IPCSYNC holds only the last value, and IF bit 16 merges multiple sends. CE7 sends 0x5 (reset, then waits for
  `sharedAddr[3]`), 0x6/0x7 (LUT/screen swap), 0x9 (in-game menu), 0xA every VBlank, and the 'DMA9' continuations.
- sg_head acks IF16 and consumes when the value reads 0xB. If 0x5 or 0x9 merged into the same IF edge, that command
  is lost. A lost 0x5 leaves the reset handshake waiting forever. The probe starts at line 180, close to the 0xA send.
- §5.2 says "no game code handles IPC sync IRQs today". That is wrong: CE9's `ipcSyncHandler` first runs the game's
  **original** table entry (`ldr pc, intr_ipc_orig_return`, card_engine_header.s:640-644; saved at misc.c:121) on
  every IPC sync. V2's probe-1 and echo add game-handler invocations per probe. In V1 a consumed probe skips it,
  which is benign.
- Fix:
  - Signal the probe through a sharedAddr word that CE7 sets.
  - CE7 sends 0xB only when no other sync is outstanding (an ARM9 ack word).
  - sg_head consumes only if the probe word is set and every other CE7 command word is clear.
  - Otherwise pass through.

## Serious

**S1. The cost figures are about 10x low (CPI ~3.4).**
- E2 assumes "~10 cycles per replayed instruction". An ARM+Thumb interpreter takes about 20-40 native instructions per
  emulated instruction (decode, condition, shifter, flags, PU classification, overlay lookup). At CPI ~3.4 that is
  70-140 cycles.
- The 4-6 KB interpreter lives in main RAM. It is cold on a probe that lands in work: 128-192 I-lines at ≥23 bus
  cycles each, i.e. 3-9k cycles.
- Realistic c_miss is ~0.5k (early reject, cold head) to ~10-20k (budget run), not 150-300 / ≤1.3k.
- §7's worst case of "≈5k/frame (0.45%)" becomes ~20-40k (2-3.5%), on exactly the frames that are busy.
- c_pass from a cold main-RAM head should use D1's own 50-190, not 40-100.
- B_miss = 112 cycles/frame with c_miss of ~10k allows about one miss every ~90 frames. The t_p learner, which steps
  later by 16 lines on each miss, then converges very slowly.
- Fix:
  - Make the cost cap a hard per-probe instruction budget, e.g. B = 32.
  - Place sg_head and the decoder fast path in an ITCM or locked I-cache slot, or price the cold misses explicitly.
  - Recompute §7.

**S2. The per-PC rejection cache blinds SG on DKR.** DKR's hottest spin PCs are shared with the drain path:
0x0201A29A-0x0201A2A4 (loads, cmp, beq) run on both empty and non-empty rings. A probe there with a non-empty ring
is rejected on the store at 0x0201A2B6. That blacklists the 41%-of-samples empty-check PC for 64 frames. Fix: key
the rejection cache on (PC, values of the loads read so far), or never blacklist a PC that has a live memo.

**S3. Fallback frames are not memory at VBlank.**
- The fallback covers ~20% of DKR/CL frames and 100% of GS DD.
- The clean runs in `vblank_entry`. That IPC IRQ comes after the game's own VBlank handler and its resumed work (B1).
- rnd's gather then reads RAM while the game keeps writing, and lines evicted after the clean write newer values back.
  The result is a mix of pre- and post-VBlank values.
- §5.6 asks rnd for "ARM9 cost ≤ the clean only" **and** a coherent copy at T. Both cannot hold unless the gather
  finishes before any game instruction runs after the clean.
- D1's judge: "torn rate > 0 fails RA correctness" (M13).
- Fix: state the tolerance the owner must rule on, or price an ARM9 in-IRQ copy (the Q4 option-B cost) as the
  correct fallback. Measure the torn rate (M13) before calling fallback frames "evaluated".

**S4. GS DD is not covered by this design.**
- (a) Adding `hookIPC_SYNC` to the GSDD CE9 reverses upstream commit **e038b45c "Do not hook arm9 IPC Sync in GSDD"**
  (RocketRobz, 2023). That was a deliberate compatibility fix, and it suggests GS DD breaks with the hook. The
  fallback hook for GS DD therefore needs a different entry: a CE9 VBlank-table hook, or the GSDD-specific
  `gsdd_fix` site. This is an investigation, not a one-line change.
- (b) With W never running in GS DD, the ARM9 network and notices (D1 W) never run, so unlocks cannot be submitted.
  "Must live on the ARM7" is an unscoped move of the network stack.
- (c) The fallback needs D1 P2. Sets that P2 keeps on the ARM9 (live reads, Remember/Recall, no slot fit) get no
  evaluator at all in GS DD.

  The ruling "every frame, every game" is still unmet for GS-class engines. Say so in §0.

**S5. A spin capture is not "exactly memory at VBlank" for words that are not ARM9-cached.** The certificate covers
the loop's inputs, not the set's watched words. Watched words that are uncached (the 0x027FFxxx shared region: keys,
touch, RTC) or written by the ARM7/DMA can change between the copy and line 192 with no ARM9 IRQ. Fix: copy the
plan's uncached and DTCM-shared words at the first unpark after line 192 (a few loads), or document the bound in B6.

**S6. The busy-mode threshold of 8 frames contradicts D1's data.** D1 §6 measured wait-free stretches of ~2,050 frames
(MvDK intro) and ~1,450 (Layton movie) in *waiting* games. SG will arm in waiting games during intros and loads and
pay c_pass/c_miss there. B3's "KSSU/MKDS: SG never armed" is then likely to fail. Fix: either accept arming
everywhere and price it in B3 for all 62 waiting ROMs, or gate on a stronger signal.

**S7. DKR's W time and P/bus are overstated or unpriced.**
- After a T hand-back the spinner drains, then returns to its spin with no T. Only a fresh probe (≤ 2 per frame)
  re-arms W, so "~0.8 × 1.12M" is an upper bound on an upper bound, given the `--thumb-cycle` gap in a Thumb-heavy
  game.
- Each W slice pollutes the cache before the producer's next burst. D1 measured Kirby at +21% from ~20 wakes.
- W replaces a bus-silent spinner with ~900k cycles of main-RAM traffic. The ARM9 has main-RAM priority by default
  (EXMEMCNT), which risks ARM7 sound and GX DMA stalls.
- E10/H7 are listed but have no pass/fail on audio for DKR before build. Add an H-B2 audio pass mark measured by the
  ARM7 (SND underrun counter), not only Clark's ears.

**S8. The interpreter needs an owner ruling against "No emulation, ever" before any work, and its size is
optimistic.** A replay of game instructions is emulation in mechanism even if not in purpose. ARMv5TE ARM+Thumb with
LDM/STM, all shifter forms, flags, interworking pops and PU classification is more likely 8-12 KB than 4-6 KB, and a
false certificate becomes a real lag frame. R1 must be asked first. Consider the decode-only alternative for the two
known loop shapes, and grow it only with B2 evidence.

**S9. Loads must be classified before they are issued.** §3.1 says loads "read real memory" and that I/O "rejects".
The classification against PU/c6/c2 and the I/O map must happen *before* the access, because:
- IPCFIFORECV (0x04100000) and the card data register (0x04100010) pop on read;
- a PU-denied address raises a data abort inside the module.

## Minor
- §6 says "not written: the BIOS frame". The BIOS writes a frame on SP_irq (the game's DTCM IRQ stack) for every
  probe that lands in work. That is dead stack and the same class as today's IPC 0xA, but list it.
- The interaction with D1's single `hk` owner context is unspecified, now that W can be entered from E, the idle
  gate, or a spinner. The unpark path must know whose context to restore.
- The memo is invalidated only by CE9 card-read completion. nds-bootstrap's ARM7-DMA card path completes
  asynchronously ('DMA9' continuation), so bump the generation there as well.
- D1 says "never build spin-borrowing". SG is spin-borrowing with a soundness proof. State explicitly that it
  overrides D1 Q4 under the owner's round-2 ruling.

## Fixes (summary)
1. Commit at the first unpark with VCOUNT ≥ 192; re-base B6 on line 192 (B1).
2. Compare-and-adopt 3FFC restore; no module IE while only sg_head is installed; new ruling Q1' (B2).
3. Per-entry T copies with private slots; SYS-only; T checks IE&IF&IME before re-entering W; audit D1's
   `e_after_irq` for the same issue (B3).
4. Probe via a sharedAddr word with CE7 serialisation; never consume IF16 when another CE7 command may be merged;
   correct the "no game handler" claim (B4).
5. Recompute c_miss/c_pass at CPI 3.4 with cold I-lines; hard B ≤ 32; place the hot path in ITCM or price it (S1).
6. Rejection cache keyed on (PC, loaded values) (S2).
7. Rule on fallback tear, or price an in-IRQ ARM9 copy; measure M13 first (S3).
8. Find a non-IPC hook for GSDD and scope ARM7 networking, or state that GS DD stays uncovered (S4).
9. Line-192 copy of uncached and ARM7-shared watched words (S5); busy-mode cost measured in all waiting ROMs (S6).
10. Owner ruling R1 before any interpreter work; classify-before-load (S8, S9).
