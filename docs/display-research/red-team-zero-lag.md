# Red team: display-library.md section 11 (zero lag, placement), 2026-10-12

Read-only. Nothing was run. Abbreviations:
- DL = `dscheevos-display/docs/display-library.md`
- RT = `docs/display-research/red-team.md`
- HW = `docs/display-research/hardware.md`
- NB = `dscheevos/external/nds-bootstrap/retail/`
- MOD = `dscheevos/dscheevos/module/src/`, CORE = `dscheevos/dscheevos/core/src/`
- MEL = `dscheevos/tools/melonDS/src/`
- G = GBATEK (`(local scratch)`)

**(spec.)** marks my inference, not something verified in a source.

Numbers used below:
- ARM9 at 67 MHz; one line = 355 dots x 12 = 4260 ARM9 cycles; one frame = 263 lines = about 1.12M ARM9 cycles.
- H-Blank = 99 dots = about 1190 ARM9 cycles.
- ARM9 cache: 4 KB D-cache (128 lines), 8 KB I-cache (256 lines), 32-byte lines, read-allocate (G:7343-7346).
- A cache miss to main RAM: G:7685-7686 says cached timing "might drop to 1.4 MHz" on misses, which is about 47 ARM9 cycles per line fill.

## Verdict

The claim "all strip costs fall inside the game's VBlank wait, so the strip cannot add a lag frame" fails in three independent ways:
1. **Halted is not the same as done.** The CPU being halted in the idle thread doesn't mean the game's frame is finished. The frame's critical path can still be running in DMA, the geometry engine or the ARM7, and the strip's main-RAM bus use slows all of them (B1).
2. **The halt flag, as specified, is stale.** It reads "idle" while the game is working (B2).
3. **Some strip work still lands in the game's working time:**
   - an entry IRQ on every frame, busy or not;
   - an H-Blank counter that runs all frame;
   - the IPC-hook arming;
   - a check in the dispatcher on every IRQ (B3).

Some of B3 can be moved into idle time. A remainder can't: the disarm or abort on the first game IRQ after an armed idle period. A literal "zero added cycles in the game's time, so zero lag" is not reachable. The owner has to rule on a bound (cycles per frame), proven by a hardware slack-probe test (T1), not by melonDS.

---

## Blockers

### B1. "Halted in the idle thread" ≠ "frame work done": the critical path continues off-CPU, and the strip slows it
NitroSDK's idle thread runs whenever **every** game thread is blocked, whatever it is blocked on. At line 192-N the main thread can be blocked on any of these:
- **The ARM7, through PXI.** Examples:
  - synchronous `SND_FlushCommand(SND_COMMAND_BLOCK)` / `SND_WaitForCommandProc`;
  - `RTC_*`;
  - `PM_*`;
  - `CARD_ReadBackup` / `WriteBackup`. nds-bootstrap serves saves from SD through the ARM7, and the strip's `readOngoing` stand-down covers ROM reads only;
  - `WM_*` callbacks. (spec. on which titles do this per frame)
- **A game DMA running while the CPU halts:**
  - an async `MI_DmaCopy*Async` waited on with `OS_SleepThread`;
  - a GXFIFO-mode legacy DMA feeding a display list. If the geometry engine isn't done by VBlank, `SwapBuffers` slips a frame, and the next frame's G3 commands block on a full GXFIFO. That is a lag frame.
- **A worker thread** that is itself blocked on one of the above.

The strip's FIFO NDMA takes about 30% of main-RAM bus on strip lines (RT M-f arithmetic). The ARM7's main-RAM accesses, sound-channel fetches, ARM7 SD NDMA and game DMAs all share that bus. Each of them finishes later, so the woken thread starts later. The abort doesn't recover any of this: the delay has already accrued before the completion IRQ arrives.

R18's sentence "No ARM9 lag frame is possible" (DL:428) is therefore unproven.

The same mechanism touches the hard rule "game wireless works exactly as stock". WM MP exchanges are VBlank-synchronised ARM7 work on main-RAM buffers (spec.).

**Fix (narrows it; doesn't close it).** At entry, also require all of these (read-only I/O, read in idle time):
- legacy DMA0-3 `CNT.31` = 0 for immediate and GXFIFO modes;
- the game's NDMA channels not busy;
- `GXSTAT.27` (geometry busy) = 0 and the GXFIFO empty;
- no ARM7 save or SD I/O in flight. CE7 would publish this flag, as it does for `readOngoing`;
- ARM7 wireless off. CE7 would publish POWCNT2.1; the DScheevos boot work already logs it.

The PXI-wait case can't be detected from the ARM9 without reading the SDK thread queues in game RAM (read-only). Even then, the wait mask of `OS_WaitIrq` isn't stored anywhere.

**Residual for the owner:** per-title evidence only. Extend the M1 survey: at line 192-N, record what every non-idle thread waits on, plus the DMA, GXSTAT and PXI state. Allow-list only titles where, on 100% of sampled frames, the only waiter is the VBlank IRQ queue.

### B2. The "halted" flag as specified reads idle while the game is working
DL:407 and DL:481 say "the module's halt hook sets a flag, and the wake path clears it". There is no such wake path:
- `halt_entry` (MOD `start.s:170-214`) returns only when the **idle thread** next runs.
- An IRQ that readies a game thread makes NitroSDK switch threads on IRQ return. The idle thread stays preempted, often until the next frame's idle period.
- So a flag cleared in `halt_entry`'s return path stays set through the entire working period that follows. On the next busy frame, the entry IRQ at line 191-N sees "halted" and turns the strip on while the game works. That is bus contention in the game's time, the exact case 11.1 exists to prevent.

The only existing flag, `in_halt`, means something else. It is set only while `module_idle` runs and cleared **before** the WFI (start.s:181-208).

There are also two races:
- **(a) Simultaneous pending IRQ.** If a game IRQ is pending together with ours, the dispatcher very likely takes the highest bit first, and NDMA bits 28-31 are highest (spec., from RT B3's `OS_IRQTable[31-clz]`). Our entry then runs before anything clears the flag.
- **(b) Nested-IRQ games.** If a game handler re-enables IRQs, our IRQ can interrupt a game handler.

**Fix:**
- Decide from the interrupted context, not a flag. On the IRQ stack, the BIOS frame's saved LR minus 4 must lie inside `halt_entry`/`module_idle` (the WFI resume address, or module idle code). Equivalent: the SDK current-thread pointer == the idle thread (read-only).
- Also require `(IE & IF & ~our_bits) == 0` at entry.
- Belt and braces: the patched dispatcher clears an `idle` flag on every game IRQ, and `halt_entry` sets it only when `r0 == 1` (the idle-loop caller, start.s:187-199).

### B3. Strip work that still lands in the game's working time
Any added cycle in working time can tip a frame whose slack is smaller than that. The design keeps these:

| # | Cost in working time | Size (est.) | Movable? |
|---|---|---|---|
| a | **Entry IRQ fires on every frame while a notice is armed**, including busy frames where it then "does nothing" (DL:412). Those are the lowest-slack frames. Cost: BIOS vector, dispatcher, trampoline, stack switch, check, plus cold misses (about 3-5 I-lines and 1-2 D-lines) and the same number of game lines evicted | about 500-700 cycles, about 0.05% of a frame, on exactly the busiest frames | Yes. Arm the line counter **from idle** (the `halt_entry` idle-loop path) with TCNT computed from VCOUNT and the DISPSTAT H-Blank flag, and disarm it in the dispatcher on the first game IRQ. Then it never fires during work |
| b | **The NDMA3 H-Blank counter runs from line 0 to 191-N**: one fill-data write per line to a "private destination" | about 170 one-word bus writes a frame; main RAM costs about 10 bus cycles each. A stall only on collision, so small but nonzero | Yes. Arm from idle (above), so it counts only idle lines. Point the destination at a fixed I/O word, not main RAM |
| c | **VBlank IPC hook arming** (DL:406): guard reads (DISPCNT, MASTER_BRIGHT, DISPCAPCNT, 4 DMA CNTs, CE9 shared flags) plus about 7 NDMA register writes plus 1-3 cold I-lines | about 250-400 cycles a frame (about 0.03%) at the start of the game's frame. This regresses P2, which had "nothing added to the VBlank IPC hook" (DL:148) | Yes. Arm from idle (row a), or self-re-arm in the exit handler (TCNT = 192-N counts from line 0) and in the decline path (TCNT = 192) |
| d | **A dispatcher check on every game IRQ for the whole session.** R17 says "~10 cycles … only while the strip is live" (DL:427), but the check itself must run on every IRQ to find out whether the strip is live | If it calls module code or loads a module flag: about 2 cold misses, about 100 cycles per IRQ. With 192 H-Blank IRQs a frame (raster games) that is about 19k cycles, about 1.7% of a frame, always | Mostly. Test a register the dispatcher already loads: keep our IE bit 31 set only while armed, and `tst` the loaded IE. That is about 1-2 cycles per IRQ. It merges with R4's routing test for bits 28-31 |
| e | **Disarm or abort on the first game IRQ after an armed idle period** | about 20-60 cycles: 1-2 I/O writes, the NDMA block drain (≤ 4 words, about 40 cycles, G:27921-27923), one possibly cold line | **No.** The entry decision can't see a future wake. **Residual for the owner** |

**Statement fix:** replace "can't add a lag frame" with: "adds ≤ X ARM9 cycles to the game's critical path per frame (row e, about 60), and none of the bus on the critical path, on titles that pass the B1 survey". The owner then rules on X.

---

## Major

### M1. Every strip IRQ wakes the idle thread and re-runs a full `module_idle` pass; the line-191 pass collides with VBlank
After any IRQ, `module_idle` returns 1 (MOD `module.c:640-644`) and `halt_entry` returns. The SDK idle loop (`b .-4`) calls `OS_Halt` again, and a **new full pass** runs:
- `dsc_idle`;
- `net_service`;
- `capture()`, which copies the snapshot with IRQs masked and polls every 16 words (CORE `dscheevos.c:801-808`).

The entry and exit IRQs therefore add two passes per shown frame. This has three effects:
- **Capture loss (RA correctness, not lag).**
  - `dsc_capture` begins with `Q.stage = 0`, "replaces a capture taken earlier in this frame" (CORE ~2855).
  - The exit IRQ comes at the line-191 H-Blank, about 1190 cycles before VBlank. If the re-pass reaches `capture()` before line 192 and doesn't finish, VBlank stops it (`Q.count != S.capture_frame`). That frame's good snapshot is already discarded, so the frame is committed stale (`commit(0)`, `frames_stale`).
  - Delta, prior and hit-count logic then sees a frame of no change. (spec. on how often: it depends on how long the pass takes before `capture()`.)
- **Cache state at frame start.** The last thing before VBlank is now a fresh module pass, not the WFI. Bound: a full I+D eviction is 384 lines x about 47 = about 18k cycles, about 1.6% of a frame.
  - The marginal cost over the existing pass is smaller, since its lines are mostly already resident (spec.).
  - It is still nonzero in the game's time whenever the re-pass touches lines the earlier pass didn't (network, arena).
- **Idle statistics** (`idle_cut`, `halts_idle`) are skewed, as RT minor already noted.

**Fix:**
- Strip handlers set `module_only_wake`. `module_idle`'s tail loops `wfi` while only module IRQs woke it, so there is no new pass and no new capture.
- Alternatively, `dsc_capture` should not discard a stage unless a game IRQ (a dispatcher counter) occurred since that stage.
- **Test:** `captures_stopped` and `frames_stale` must be equal with the strip on and off (M7 and H7 pass marks).

### M2. P2b dropped P2's safety nets; a late arm or late entry runs mode 3 into the next frame's working lines
P2b's step list (DL:405-414) has no VCOUNT windows, no guard IRQ and no `armed_this_frame` VBlank fixer. Those were P2's B1/B2 fixes (DL:224, 231-237).

Failure chain:
1. The IPC hook can run late. It was measured at lines 192-231 (finding 4), it is missing on 0-15% of load frames (RT M-b), and on heavy ARM7 VBlanks it can run past 262 (spec.).
2. TCNT = 192-N then starts counting mid-frame, so entry fires at line 191-N+d.
3. Entry re-arms N more H-Blanks. H-Blanks during V-Blank don't count (G:27965), so for d > 0 the exit lands at line d-1 **of the next frame**.
4. Mode 3 is then on through VBlank, so the game's VBlank work (`GX_DispOff`) reads mode 3: the R2 leak is back. Lines 0..d-1 show FIFO underrun while the game works.

**Fix:**
- At entry, require VCOUNT = 191-N (±1, with the H-Blank flag), else decline.
- At entry, compute the exit TCNT from the actual VCOUNT so that the exit is ≤ 191-K.
- Exit explicitly writes NDMA0CNT=0 and checks the busy bit; don't rely on an exact TCNT.
- Keep a fixer: at the first game IRQ after a shown frame, restore mode 1 if `armed_this_frame` and the mode reads 3, before the game's handler. The dispatcher hook already exists for the abort.
- Choose K ≥ 2 (see M5).

### M3. The abort: stopping the FIFO NDMA is unverified, and a failed stop is bus time in the game's working time
- NDMA blocks are never split (G:27921-27923). A disable therefore takes effect after at most one 4-word block, about 20 bus cycles (about 40 ARM9). Good, if the disable works at all.
- G:28016-28019 hedges on what is writable during an enabled transfer.
- melonDS keeps sampling the display FIFO in mode 1 once `RunFIFO` is latched (MEL `GPU.cpp:1110-1121, 1135, 1259`). Suppose the hardware does the same and the disable doesn't take. Then the NDMA keeps feeding up to TCNT after the abort, during the game's work.
- The picture stays clean after an abort: mode 1 shows game lines, so underrun is invisible. Stale FIFO words could shift **our** strip on the next entry (melonDS resets the FIFO pointers only at line 192, `GPU.cpp:1280-1281`). That is cosmetic.

**Test H11 (new):** in a lab ROM, abort at a random strip line. **Pass:**
- NDMA0CNT busy reads 0 within 100 ARM9 cycles of the write;
- a main-RAM busy loop timed from the abort to line 191 matches the no-strip baseline within 0.1%.

### M4. H10 pass mark admits a variant that violates zero lag; melonDS's "works when armed at entry" is suspect
- **Variant B feeds the FIFO from line 0.** That is 96 KB a frame and about 9-10% of bus during the game's work (HW:280). It is disqualified under the zero-lag rule, so DL:445's "Pass … in one variant" must become "variant A only". If variant A fails on the DSi, P2b is dead.
- **(spec.) The melonDS result.** melonDS latches `RunFIFO` at `StartFrame` (line 0) from mode 3 or an armed mode-4 DMA (`GPU.cpp:1135`). A FIFO NDMA armed first at line 191-N therefore gets no FIFO requests in that frame, unless the previous frame left it armed. The melonDS observation behind DL:442 should be re-checked before it is used as evidence.

### M5. The exit at line 191 sits about 1190 cycles before the game's VBlank IRQ
- The exit fires at the line-191 H-Blank. The handler, with cold misses, takes about 300-700 cycles. Any extra latency pushes it into line 192:
  - our own IRQs-off sections (capture polling, `idle_frames`);
  - a pending entry handler;
  - a DMA burst.
- From then on, the game's VBlank handler, and so its frame start, waits for the rest of our handler. The dispatcher takes one IRQ per entry, and ours first (spec.).
- The M1 re-pass then also runs into VBlank.

**Fix:**
- Exit at 191-K with K ≥ 2 lines (about 8.5k cycles of margin). The last K strip lines are given up.
- Measure exit latency in H6b with the real module running, and set K to the 99.99th percentile + 1.

### M6. R18 and the ARM7: contention reaches the ARM9's next frame
- Through the PXI waits in B1.
- Through sound: the ARM9's `SND_Main` / `SND_FlushCommand` at frame start can block if the ARM7, slowed on strip lines, is behind on the previous commands (spec.).
- Size bound: ARM7 main-RAM work slowed by up to about 30% over N lines (1-2 ms) is at most about 0.3-0.6 ms of delay.
- Wireless (WM MP) is a hard-rule item, not a "little latency".

**Fix:**
- Stand down while ARM7 wireless is powered or WM is active (CE7 publishes it).
- Add ARM7-side probes to H5: PXI round-trip time and sound-command latency with the strip on and off.

### M7. Placement (11.2): POWCNT1.15 is neither stable nor only the game's
- **Dual-screen 3D titles flip POWCNT1.15 every frame** and show the other screen through capture (spec. on which titles). "Compare each frame with POWCNT1 bit 15" (DL:464) then puts the strip on alternate screens at 30 Hz.
- **nds-bootstrap itself rewrites the swap bit** from its main-screen option in every 0x6/0x7 IPC (NB `cardenginei/arm9/source/cardengine.c:1693-1696`, IGM `mainScreen`). So "the game decides … that's game state" (DL:451) is incomplete.
- **A mid-notice swap** moves the notice to the wrong screen.

**Fix:**
- Require the bit to be stable for 30 frames before showing.
- End the notice (to "missed") on any change.
- Stand down for titles that toggle more than once per second (survey).

### M8. The planned tests can't detect B1, B3 or M1
melonDS doesn't model cache miss cost or main-RAM contention between the ARM7, ARM9, DMA and sound (spec. on exact gaps). So M7's "identical lag count" passes whatever the bus effects are. H7's statistical lag comparison can't prove zero.

**Test T1 (new, hardware): slack probe.**
- A lab ROM whose frame work ends k cycles before VBlank, with k swept from 0 to 4000.
- Variants: pure CPU work, work gated on a GXFIFO DMA, and work gated on an ARM7 PXI round-trip.
- Measure the smallest k with no lag, strip off and strip on.
- **Pass:** Δk ≤ the X the owner accepted (B3) in every variant.

---

## Minor

- **R17 is misstated.** The check runs on every game IRQ, not "only while the strip is live" (B3d). Fold it into R4.
- **Games without the SDK idle loop.** DL:439 says they "keep the LED and menu". In fact:
  - with no `OS_Halt` signature, DScheevos is disabled entirely (NB `bootloaderi/source/arm7/dscheevos_hooks.c:249-251`);
  - with `OS_Halt` but no idle loop, `module_idle` returns 0 at once (MOD `module.c:578-582`), so nothing is evaluated.

  They have no achievements today. The display question is moot for them.
- **BIOS `VBlankIntrWait` variant.** It is a *better* signal than the idle thread: the interrupted PC is in the BIOS IntrWait loop, and the saved r1 mask is VBlank only, so the caller is provably waiting for VBlank. But it needs:
  - an idle source for the module (none exists);
  - a dispatcher hook for non-SDK games (the S4 BIOS-vector wrapper, a DTCM write).

  It is not available now. Busy-wait games have no safe variant.
- **The `DTCM+3FFC` vector check (RT B3) must carry into P2b.** A game that swaps its IRQ vector at runtime (FMV middleware, spec.) would skip our abort. Check it at entry.
- **H-Blank / VCOUNT-IRQ raster games.** Their IRQs fire on lines 192-N..191 and abort every frame, so they get no strip (an availability cost). Detect it (`DISPSTAT.4`, LYC in the strip range) and don't offer the strip.
- **P4 "idle-only".** Register writes take effect mid-frame, so the first and last frames tear (our layer only). With the compare moved to idle, a game re-enabling engine B is detected up to about 2 frames late, so R13 grows from 1 to 2 frames. P4 also depends on the idle signal and on B2's fix.
- **The bottom edge of the touch screen** (engine A on the bottom) hides touch UI. A UX residual.
- **The FIFO contends with the module's own idle evaluation**, which slows it by up to about 30% on strip lines. Notices come later; no lag.
- **Cache pollution from the strip handlers alone** (excluding M1): about 12-20 lines a shown frame, about 600-900 cycles (about 0.07%), refilled after VBlank in the game's time. It is included in X (B3).
- **Write buffer.** Our STRB and NDMA writes drain within a few bus cycles, inside the handler. No effect outlives the strip, as long as the exit is not at the VBlank edge (M5).

## What I'd change in 11.1 and 11.3
1. Restate the guarantee as a cycle bound X plus a per-title B1 survey, and ask the owner to rule on X.
2. Arm and disarm from idle (B3a-c). Use the inline IE test (B3d). Decide "idle" from the interrupted PC (B2).
3. No module pass on a module-only wake (M1).
4. VCOUNT windows, exit at 191-K, explicit NDMA0 stop, and keep a fixer (M2, M5).
5. Entry guards: DMA busy, `GXSTAT.27`, ARM7 save/SD/wireless flags (B1, M6).
6. Add tests H11 and T1. H10 passes on variant A only. Add capture counters to M7 and H7.
7. Fix the 11.2 table: POWCNT1.15 stability and the nds-bootstrap swap (M7).
