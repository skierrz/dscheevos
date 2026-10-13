# Red-team: proposal-arm9-hooks.md (ARM9 wait hooks + parkable work coroutine)

Verdict: **fixable**. The core idea is sound: E emulates IntrWait with the discard at call time, and W is parked on every IRQ. It removes the SWI 5 whole-frame trap by construction. But two defects would crash games as written, one owner ruling is ignored, and the "complete list" of game-visible effects misses at least two classes. Every claim below was checked against the code and data, not the proposal's citations.

## What I verified as correct
- Wrapper patch encodings: 0x225B / 0x04D2 / 0x4710; 0x5B<<19 = 0x02D80000. `b` to header+0xA0 = 0xEA000026.
- Wrapper presence: a fresh scan of all 64 ROMs (`~/RomM/library/roms/nds`) finds exactly one `00 22 05 df 70 47` per ROM, always < 0x800 past the **ARM9 load address**.
  - Nine ROMs load at 0x02004000 or 0x02007000 (M&L PiT wrapper at 0x0200429c, WarioWare at 0x02004428, Drawn to Life at 0x02007176). So F2's "pc in 0x02000000-0x020007FF" is wrong as an absolute range.
  - The scan from `arm9destination` still works.
- F4/F5: static.tsv has oshalt=1 in 64/64 and callers4 = callers6 = 0.
- nds-bootstrap does not patch OS_IrqHandler code. `hookInterruptHandler` only reads the table address (hook_arm9.c). F11/F12 are as cited.
- Register choreography in w_resume and irq_head is consistent (wctx layout, `ldm^`, base-last load, `subs`-compatible pc = lr-4, T bit through SPSR). Restoring m_r4..r11 before the re-take makes an SDK thread switch save a correct context.

## Blockers

**B1. Shared stack corrupts W in mixed games (crash).**
- Section 3 puts W on `__stack_top`. `halt_entry` also runs `module_idle` on `__stack_top` (start.s:200-202).
- The proposed exclusion is "halt_entry runs module_idle only if !w_busy". But a W yielded at its safe point (w_busy = 0) still has live frames on that stack: w_main's frame, and the sp saved in wctx by w_yield.
- The idle thread cannot run while the main thread is inside E. It runs later in the frame, whenever every game thread blocks. Then module_idle pushes from `__stack_top` down over W's frames, and W resumes on a trashed stack.
- This hits exactly the mixed games the proposal lists: RE:DS (11% OS_Halt), MKDS, ACWW, WarioWare and Trauma Center (census: direct SWI 5 callers in OS_Halt games).
- Fix: give W its own stack (4-8 KB, budget it), or never run module_idle once a W context exists (phase 2 merged from day one).

**B2. The display interface as specified crashes on NDMA IRQ bits 28-31.**
- The proposal itself says the game's dispatcher indexes `OS_IRQTable` past its end for bits 28-31. It only disarms in `cb_leave`. But between cb_enter and cb_leave the head is OFF with CPSR.I clear in several windows:
  1. `e_after_irq` (about 4 instructions after every game IRQ returns, before E re-masks);
  2. the re-taken game IRQ after a park: if a strip bit is also pending, the game's handler acks the game bit and returns with I clear, the strip bit is still pending and the head is OFF;
  3. the whole time a higher-priority game thread runs mid-wait, or a second thread sits in pure_E, which never touches the head.
- In each window a strip IRQ goes to the game's handler: crash or IRQ storm, which display-library.md B3 already names.
- Also, the display design assumes a **permanent boot-time patch of OS_IrqHandler** (R4, display-library.md:244, 264, 414, 547). That conflicts with this proposal's signature check of h[0..3] at wait entry and with the runtime toggle of h[0]. If R4 is applied, this proposal's W is disabled for every game. If R4 is not applied, B2 holds.
- Fix: compose with R4. The R4 dispatcher routes bits 28-31 to the module, and the toggle moves to a module flag read by that dispatcher, or vice versa. Alternatively, mask the module IE bits whenever the head goes OFF, and accept the IE-write residual.

## Serious

**S1. The owner ruling "frames must ALWAYS be evaluated, every game, no exclusions" is ignored** (design-notes.md:40-42).
- RE:DS is planned to "fall behind and drop frames (DEGRADED)".
- Busy engines get an "achievements can't be checked" message.
- Capture misses are "evaluated as a carry".
- The proposal must say plainly that the ARM9 alone cannot meet the ruling for RE:DS-class sets and busy engines, and name the ARM7 lane (or another one) as the dependency. It should not ship an "unsupported" verdict as the plan.

**S2. Capture aborts on *any* pending IRQ, and both retries can starve.**
- Today `interrupt_pending` services the IRQ in place and continues (module.c:163-172). Wait mode aborts and restarts from scratch (sync_restore).
- A pointer-chain capture is about 12-15k instructions, about 50k cycles at CPI 3.4, or 12 lines. At Kirby-like IRQ rates (about 20 per frame, one per ~56k cycles), most attempts abort.
- The late capture in W has the same rule. So frames run on carried snapshots, which corrupts delta/prior semantics (an RA correctness issue, not only fidelity).
- Fix: run the capture inside W with IRQs enabled. Parking preserves progress. Restart only if game code ran: higher-priority thread switched in, or lines_between ≥ 2, as today.

**S3. The game-visible effect list (5.1, "complete") misses timing-critical IRQs.**
- X applies to **every** IRQ during the wait, not only the VBlank. H-Blank and VCOUNT-match raster IRQs, and timer-driven sound streaming, get X more latency than from a BIOS halt.
- H-Blank is about 1,000-1,400 ARM9 cycles. A raster write pushed out of H-Blank is a **visual glitch** (hard rule).
- Nobody has measured which BIOS games use raster IRQs during the wait. Add an IRQ-source census per BIOS game, and an M4/H3 pass mark per IRQ kind, not just the VBlank.

**S4. X is underestimated.** "100-350 cycles" leaves out:
- the worst-case latency of the instruction W is in: LDM/STM with main-RAM misses, plus dirty-line write-back, since the D-cache is write-back;
- write-buffer stalls from irq_head's ~16-word `stmia` to wctx (stores don't allocate on a read-allocate cache);
- likely I-misses on the BIOS vector, irq_head and OS_IrqHandler-adjacent lines that W's rcheevos working set evicted (8 KB I-cache);
- the BIOS entry running twice.

A realistic X is more like 300-800 cycles. The 400-cycle pass marks in M2, M4 and H3 may fail on hardware for structural reasons.

**S5. ARM7/main-bus contention is missing from 5.1.**
- Stock SWI 5 leaves the ARM9 halted and the main bus to the ARM7 (sound and PXI data, CE7 SD→main-RAM card transfers, Wi-Fi buffers).
- W runs at CPI ~3.4 and is cache-miss bound, so it is heavy main-RAM traffic, far more than the strip's ~30% that display R21 already treats as a residual.
- Slower ARM7 replies (for example a streaming read in flight) can delay the game. Add it as a residual with a measurement (EXMEMCNT bit 15 state, ARM7 reply latency in a W-on/off A/B).

**S6. W drops module_idle's once-per-frame gating.**
- w_main calls `net_service`, `publish_stats`, `clean_notices` and `update_cues` on every iteration.
- module_idle gates them on new_frame (module.c:607-613, 637-648), because of the measured ~390k-cycle fixed overhead per pass (Kirby).
- In Sonic Rush's wait (median 331k, an upper bound), overhead alone can consume the whole wait. Keep the gating in W.

**S7. Throughput claims lean on upper bounds.**
- "Sonic Rush 73.7k → 250k cycles fits most frames" compares against a melonDS median of 331k that the brief says is an upper bound (Thumb second-fetch bug; Sonic Rush is Thumb-heavy).
- Add X × IRQ count, re-resume costs and capture. The margin may be negative after `--thumb-cycle`.
- State the fit as unknown until measured, and make M2-3 the gate rather than the expectation.

**S8. The OS_IrqHandler location is not checked.**
- The runtime check tests the signature and that h is ARM and 4-aligned. It does not check that h is in ITCM.
- If a game's handler were in cached main RAM, the toggle store would sit dirty in the write-back D-cache while the I-fetch reads old code. W would not be parked, and the game's handler would run with W as the interrupted context. That means lag, and on a thread switch the SDK saves W's PC and module SP as the main thread's.
- Fix: require h ∈ 0x01000000-0x01FFFFFF (and in `b` range), or do clean D, drain, invalidate I. Verify the handler is in ITCM in all 9 BIOS ROMs, not just Sonic Rush.

**S9. G3 is a runtime write into game ITCM.** The hard rule is "never touch the game's RAM", and the accepted precedent is **boot-time** code patches only.
- The invariant argument (5.2) is good, but this is a new class, and the proposal's own rejected-alternatives table refuses a DTCM table write on the same grounds (inconsistent).
- If the toggle stays OFF when ownership is lost, a game that reloads ITCM code mid-wait gets a stale `irqh_word0` written back over new code. Rare, but it is the failure mode of a runtime code toggle.
- It needs an explicit owner ruling, presented alongside R4, since both target the same instruction.

**S10. Reset and quit are unhandled.**
- `reset_entry` (start.s:221-228) clears only in_vblank and in_halt.
- With W, hk.owner, w_running, w_busy, the head toggle and wctx must also be reset. W abandoned mid-core-call (w_busy = 1) must be re-created from scratch, and dsc_reset must run outside it.
- A stuck `owner = 1` (main thread destroyed mid-wait, soft reset) silently disables W forever.

**S11. "Every bw_entry is frame done" is not exact.**
- RE:DS's main caller is 79%. Captures taken at other callers' waits (fades, loading, other threads) are mid-frame or off-thread snapshots.
- wait_kind == 2 tells the display "VBlank wait", not "main thread frame done" (a higher-priority non-main thread may call the wrapper).
- Record the caller lr per wait. Capture only on the dominant caller, learned at runtime (not a per-game list), or justify why any VBlank wait is a valid capture point.

## Minor
- `bw_entry` at header+0xA0 is where `vblank_entry` lives today (start.s:57-67, chosen for IRQ cache locality). The layout change needs to say where vblank_entry goes.
- Check the caller mode at bw_entry: if it is not SYS, take pure_E. `ldm^`/`stm^` assume SYS/USR banking.
- 7.3: a 600-frame "no wait" threshold false-fires on cutscenes and loads in normal games: MvDK intro 2,050 intervals, Layton movie ~1,450 (idle.md Findings 1).
- 7.4 misreads the code:
  - CE9 `sleepMs` calls the game's own `OS_Sleep` (`sleepRef`) when `runSleepMs` is set, which already goes through OS_Halt, and otherwise `swiDelay(50)` (cardengine.c:189-200);
  - the census says the ~185 VCOUNT reads per interval are nds-bootstrap's bootloader stub in boot frames, not gameplay.
- 11: "SCFG_CLK locked for NTR games" conflicts with nds-bootstrap's own boostCpu option and the IGM's runtime `REG_SCFG_CLK ^= 1` (facts-hw §6). The real objection is that 133 MHz changes stock game timing.
- H4/H5 "paired run with the same route" on hardware is not deterministic. Use lag-interval rates over long runs with a pre-set tolerance, or the lab-ROM T1-style probe.

## Best ideas to keep regardless
- 6-byte in-place wrapper patch (`movs/lsls/bx r2` to 0x02D80000): no secure-area padding, no trampoline.
- Emulate IntrWait with the discard at the call, so the SWI 5 trap is impossible by construction.
- Park on IRQ by consuming the BIOS frame and re-taking the IRQ through the original handler with E as the interrupted context. This makes main-thread work preemptible with no deadline estimate.
- An IRQ-masked E halt (wake on IE&IF with IME = 1) with no lost wakeup.
- A pure_E fallback, so a missing IRQ head costs coverage, not stability.
- The write and invariant audit (M5) and the lab-ROM BIOS ground truth (H1, dumping the NDS9 BIOS IRQ vector and IntrWait).
- The honest statement that P (cache refill) and X are nonzero and that the shipped OS_Halt design has the same P.
