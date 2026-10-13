# Red-team: proposal-uniform.md (Wait-Gated Coroutine, WGC)

Verdict: **fixable**. The core idea is the best one on the table. CO is preemptible at the dispatcher, and
the real SWI 5 is handed back before VBlank, so BIOS semantics come from the BIOS itself. As written, though,
the G2 gate has two paths that cost a lag frame. The trampoline site it names is wrong. One throughput claim is
false. The cost it adds in game time is underestimated by about 3-5x. Every item below was checked against code
or data, not against the proposal's citations.

## Blockers (hard-rule breaches or wrong facts)

### B1. G2 loses a whole frame when VBlank happens while another game thread has the CPU (lag frame)
- `vbl_seen` is set only on W's **slow** path (§2.5 `note vblank`). The case:
  - The main thread is parked in G2.
  - An IRQ wakes a higher-priority thread. The SDK switches to it, and W's epilogue sets `ctx=GAME` and disarms LT.
  - VBlank arrives while that thread runs. W takes the **fast** path, so `vbl_seen` stays 0.
  - That thread sleeps. The SDK's `OS_LoadContext` resumes the main thread at `gate_resume_swi` (not an IRQ
    return, so W never sees it).
- In §2.3's pseudo-code, `lt_fired=0` and `vbl_seen=0`, so the gate `co_resume`s CO past VBlank.
  - Either nothing re-arms LT (the pseudo-code has no re-arm, although §2.7 says the gate re-arms it), and CO runs
    until the next VBlank.
  - Or LT re-arms for line 192-K **of the next frame**, and then the real SWI 5 waits for the VBlank after that.
- Stock would return at once: the BIOS finds the flag set by the handler. So WGC costs one frame.
- This is not exotic. SDK 2.x games (the whole bios class, CEN:17) have sound and card threads that wake on IRQs.
  Loading screens run a card thread across VBlank.
- Fix: decide "VBlank already happened" from something W's fast path can't miss.
  - Record the module's VBlank counter at gate entry (`dscm_vblank_count`, ARM7-bumped, read uncached), or a
    VCOUNT-wrap test, and compare it on every gate resume.
  - Re-establish `ctx=GATE` and re-arm LT on every resume.

### B2. A VBlank between the LT IRQ and `svc 5` makes the real SWI 5 discard that VBlank (lag frame)
- `gate_resume_swi` tests `lt_fired` **before** `vbl_seen`.
- The case:
  - LT fires at line 192-K.
  - Before the gate reaches `svc 5`, a slow game IRQ handler runs (sound or PXI), or a game DMA stalls the CPU.
    GBATEK: the ARM9 halts during DMA unless it runs from TCM or cache, and W and the gate are in main RAM.
  - Together these take more than K=2 lines (8.5k cycles).
  - The VBlank IRQ is then taken inside the gate. The handler sets DTCM+3FF8 bit 0, and `vbl_seen=1`.
  - The gate still sees `lt_fired` and calls the real SWI 5.
  - IntrWait with r0=1 discards the flag and waits for the **next** VBlank.
- §2.3's "margin of over 30x" counts only W's swap. It leaves out game handler time, DMA stalls, and the CO
  sections that keep IRQs masked: capture chunks plus whatever lwIP, link.c and the heap mask (no inventory given).
- Fix:
  - Test `vbl_seen` first.
  - Make `real_wait` a restartable sequence. If W takes a VBlank while the interrupted PC is in
    [`real_wait`, `svc`], it rewrites the frame's return PC to `fallback_return`.
  - Size K from a measured worst case (masked spans, DMA, handlers), not from the swap latency.

### B3. The trampoline site does not exist as described
- §2.2 places the trampolines in "CE9's area, e.g. 0x023BFxxx per CEN:39". CE9 is not there.
  - The bootloaderi CE9 is linked at `CARDENGINEI_ARM9_LOCATION = 0x027FC000` (`common/include/locations.h:96`).
  - The ARM7 checks `ce9->ce9 != CARDENGINEI_ARM9_LOCATION` (`cardenginei/arm7/source/cardengine.c:1144`).
- 0x027FC000 is about 8 MB above the secure area, outside Thumb BLX's +0x3FFFFE.
- 0x023BF000 is described in nds-bootstrap as "0x023BF000, which has the arm9 code" (`bootloaderi/source/arm7/main.arm7.c:338`).
  - That is the bootloader's ARM9 stub, inside the game's own 4 MB.
  - Nothing shows it is excluded from the game's arena.
  - A write there may be a write to game heap, a hard-rule breach.
  - CEN:44 only *guessed* "card engine area".
- The secure-area padding fallback is game-image bytes that anti-tamper code could hash (E12).
- Fix candidate: the first 16 bytes of the DSi extra RAM at **0x02400000**.
  - They are reachable from every wrapper, because a wrapper at PC ≥ 0x02000000 reaches up to PC+4+0x3FFFFE ≥ 0x02400002.
  - They lie inside the ROM cache region nds-bootstrap already owns (ROM_LOCATION 0x0C3EC000 / ALT 0x0C400000).
  - nds-bootstrap must reserve them in every layout (ROM-in-RAM, preload), at a cost of at most one 16 KB cache slot.
  - Verify ownership in M1 before relying on it.

### B4. The RE: Deadly Silence throughput claim is false
- §3 says RE:DS "fits on average only". The numbers in IDLE:27:
  - set 197k instructions × CPI 3.4 (the brief) = **~670k cycles per interval**;
  - mean BIOS halt is **200k per interval**.
- That is 3.3x short on average, and short even at idle.md's own CPI 1.3 (257k).
- 200k is also a melonDS upper bound (thumb-cycle caveat).
- Correct statement: RE:DS evaluates roughly 1 frame in 3-4, and the rest are dropped (DEGRADED).
- Sonic Rush's "fits every frame" rests on a **median** of 331k (IDLE:25), not p10. It is also an upper bound,
  minus K lines, minus the per-frame network and notice work. Re-state it as "likely, to be measured".

## Serious

1. **E1 is underestimated (it is the only cost in game time).**
   - W is in cached main RAM at 0x02D8xxxx. In game time its I-line and the `wait`/literal D-lines are almost
     always cold (4 KB D-cache).
   - A main-RAM cache miss is 23 bus cycles = **46 ARM9 cycles** (GT:7643).
   - Cold fast path: 1-2 I-misses plus 1-2 D-misses ≈ **140-190 cycles**, plus the evicted game lines refilled
     later. Proposal: 10-60. architecture.md:46 measured a comparable "few cold lines" IRQ path at ~500 cycles.
   - At ~20 IRQs per frame (Kirby), that is ~3-7k cycles per frame in game time, for all 53 OS_Halt games, which
     today pay nothing per non-IPC IRQ.
   - H2's "≤120 cold" will likely fail. The acceptance rule "X_WGC ≤ X_today" can then fail on the majority class.
   - Mitigation:
     - Fast-path test with no memory load: read the interrupted PC from the BIOS frame in the game's IRQ stack
       (DTCM, 1 cycle) and range-check it against module/BIOS addresses. That removes the D-misses.
     - Accept that the I-miss remains.
     - Pre-register a realistic H2 bound.
2. **The epilogue misdetects thread switches.**
   - For `ctx==GATE/BIOSWAIT`, `frame'` is a copy of the BIOS frame, so `frame'.lr` is the interrupted gate or
     BIOS PC, never `gate_resume_k+4`.
   - So `epi` always sets `ctx=GAME` and disarms LT, even with no switch. Inside BIOSWAIT the display then sees
     GAME early.
   - Compare against the value `frame'.lr` held before `bl sdk`.
3. **Nested IRQs corrupt the TCB.**
   - After the swap, `ctx` still reads CO(k) while the SDK runs.
   - A game handler that re-enables IRQs (some sound drivers do) re-enters W, which "swaps CO" again and
     overwrites the TCB with handler state.
   - Set `ctx=GATE(k)` before `bl sdk`. "Nesting is safe" (§2.5) is not shown.
4. **F1 is a normal path, not a failure-only path.**
   - B1's preemption case (once fixed) ends with a VBlank already consumed by the handler. The gate must return
     with DTCM+3FF8 bit 0 still set, where stock would clear it.
   - So "F1==0" (M2, M4, H6) can't pass on games with threads, and "G2 turns itself off after 3" fires spuriously.
   - Count LT loss and preemption-over-VBlank separately. Get an owner ruling on whether to emulate the BIOS
     clear: that is a write to game DTCM, which is otherwise forbidden.
5. **The dirty rule is inconsistent.**
   - §2.6 says any game IRQ handler marks the snapshot dirty. The §2.5 pseudo-code marks dirty only on IF bit 0
     and on thread switches.
   - Under §2.6's rule, a game with ~20 IRQs per frame recaptures ~20 times per frame (~50k cycles each at
     12-17k instructions × CPI 3.4). That contradicts M7's "KSSU ~20 → ≤2", and the "fewer copies" efficiency
     claim for the oshalt class becomes unproven.
   - Also not covered: ARM7 writes to the shared area (0x023FFxxx is inside read_memory's 4 MB) and DMA
     completions with no ARM9 IRQ.
6. **The LT deadline list is multiplexed, but `lt_fired` is one bit.**
   - A display deadline (191-N-K) or the late-recapture deadline (192-K-C) sets `lt_fired`, and G2 then hands
     back to the real SWI 5 early.
   - `lt_fired` must name which deadline fired.
7. **The LT bit must never be in IE&IF on the fast path.**
   - NitroSDK's dispatcher indexes `OS_IRQTable` (22 entries) by the IF bit. Bit 28+n jumps through garbage
     (survey §5 point 7).
   - `LT_disarm` must clear the IE bit and IF atomically in IRQ mode. Say so, and handle a game handler that
     saved IE with our bit set and later restores it.
   - nds-bootstrap already patches the game's IRQ-enable function (`myIrqEnable`). Use that for the R15
     re-OR, rather than "re-OR at every gate resume".
8. **The stack collision (start.s:133) is real, and it grows.**
   - `vblank_entry` runs from CE9's IPC handler on whatever SP_irq is current.
   - Under the slow path that is the module IRQ stack, and line 133 resets `sp` to `__irq_stack_top` over W's frame.
   - nds-bootstrap's in-game menu also runs inside the CE9 IPC IRQ. Under the slow path it would run on the 4 KB
     module IRQ stack. Check its depth.
9. **Throughput rests on melonDS upper bounds.**
   - The brief forbids relying on idle margins. WGC doesn't need a budget model, since it is preemptive, which is good.
   - But §3's coverage claims ("full coverage", "fits every frame") should be restated as measured outcomes (M2),
     not predictions.
10. **The trampoline relies on BLX alignment.**
    - Target = (LR + off) & ~3. The trampoline words must be 4-aligned, which is easy, and the 4-byte BLX may
      straddle a word in the wrapper (fine on ARMv5).
    - M1 must also confirm that the wrappers in the 9 bios-class titles are the exact 6-byte shape, and that no
      caller branches into their middle.

## Checked and OK
- SWI 5 = IntrWait(r0=1, r1=1). The BIOS saves SPSR, r11, r12 and r14 on SP_svc, so `mov r12,lr` is safe in the
  stock path.
- `0x46F4` is `mov r12, lr`.
- The BIOS IRQ frame is {r0-r3, r12, lr} (FB:1189). W's direct `subs pc, lr, #4` matches the BIOS epilogue.
- Main RAM is NTR BIOS-backed: `REG_SCFG_ROM=0x703` (main.arm7.c:217), so the DSi9 IntrWait bug does not apply.
- NDMA access: SCFG_EXT9 = `0x8300C000|BIT(16)` before the lock (main.arm9.c:429-436). Bit 8 is not set today
  (display R5).
- Cache sizes (D 4 KB, I 8 KB) and the miss cost of 23 bus cycles per line are correct.
- Memory: ~1.4 KB is credible, with no ROM-cache cut, apart from B3's possible 16 KB slot.
- Busy class: an honest "no generic zero-cost mechanism". G3 is learned at run time, not from a list, and off by
  default. It is acceptable as an owner-ruled experiment.
- ARM7 rejection: the reasons are correct (write-back D-cache coherence, 64 KB WRAM, contention).

## Per-game check
- Nothing in G1, G2 or W is per-game. G1b's "titles on the allow list" (§2.8.4) imports the display's allow list.
  Keep it out of WGC's mechanism.
- The dispatcher signatures (V1-V4) are signatures, not a list. A V3/V4-only patch would be a one-off per title
  and should be argued as a signature.

## Best ideas to keep
- CO preempted at the dispatcher, with a swap that presents the parked waiting thread to the SDK.
  - Main-thread spare time becomes preemptible, which answers the brief's "not preemptible" problem without a
    cost model.
  - It also kills the per-wake pass overhead.
- Hand-back to the **real** SWI 5 before VBlank via a private NDMA H-Blank counter. BIOS semantics come by
  construction, not by emulation, and there are no DTCM writes.
- Patching the 6-byte Thumb wrapper with `mov r12,lr; blx tramp`.
- One `wait.ctx` written by the first code of every IRQ, as the display's "waiting now" signal. It removes the
  stale-flag problem (B2 of the display red-team), once the epilogue bug is fixed.
- Module-only IRQs never enter the SDK and never start a pass (display requirement 3).
- Pre-registered tests M4 (wait-exit latency vs stock) and M5 (the flag and r0-r3 bit-exact vs Nintendo's BIOS).
