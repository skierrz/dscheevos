# Red-team: Proposal FB (one frame-boundary capture for every game), round 2

Reviewer: adversarial red-team. Static checking only: code, disassembly and arithmetic. No emulator was run.
Inputs: design-brief2.md, round-1 decision.md (idlecov worktree), proposal-frame-boundary.md.

**Verdict: fixable.** The core idea holds up. Every frame gets one instant (VBlank, before the game's handler), and
the halt copy becomes a zero-cost precomputation of it. Several facts also check out:
- the melonDS line-262 frame end;
- the GS DD table rule (−31×4);
- DKR's dispatcher matching no nds-bootstrap signature;
- the dispatchers taking the lowest bit first.

But the install/recovery path does not work for the two busy titles that have RA sets. And the runtime table write
is a game-visible write of game RAM that round 1 deliberately avoided. Several cost and correctness claims are
also stated more strongly than the evidence allows.

---

## What I verified (true)

| Claim | Check |
|---|---|
| melonDS ends RunFrame at the end of line 262 | GPU.cpp:1179-1189: `VCount==262` schedules `LCD_FinishFrame`, which sets `TotalScanlines`. NDS.cpp:980 loops `while TotalScanlines==0`. **True** |
| GS DD dispatcher: `blx lr`, with lr = `[0x027E00BC − clz·4]` | Disassembly of scratchpad/fb/gs/irq.elf at 0x0207BA80-0x0207BAB0 and pool 0x0207BBF4 = 0x027E00BC. For VBlank, clz(1) = 31, so the entry is 0x027E0040. **True** |
| nds-bootstrap's Alt5 path returns the literal after the end signature as the table | hook_arm9.c:153-168: `tableAddr = addr[0]`. For Alt5 that is 0x027E00BC, the *end* of the table. **True** (the link to `#ifndef GSDD` is still a guess) |
| DKR's dispatcher matches none of V1/Alt/Alt5 | dkr/irq.elf at 0x020A5B08: no leading `e92d4000`, and the 4th word is `e3510000`, not `e14f0000`. **True** |
| Lowest pending bit dispatched first (VBlank first) | DKR: a `clz`/`bics` loop leaves the lowest bit (0x020A5B2C-0x020A5B38). GS DD: `rsb/and` isolates the lowest bit. **True** for these two |
| nds-bootstrap writes IRQ-table entries at run time | misc.c `hookIPC_SYNC`: entries 2 and 16, **once**, guarded by `IPC_SYNC_hooked`, and **all inside `#ifndef GSDD`**. Partly true: once only, no re-install, no pool |
| NWRAM can't be a cross-CPU buffer in play | GBATEK 24311 (SCFG_EXT9 bit 31) and 27751 (MBK1-5 are ARM9-writable only). **True** |

---

## Blockers

### B1. Busy-engine coverage has no install or recovery path in either busy title that has a set (GS DD, DKR)

- **GS DD:**
  - The CE9 GSDD build compiles out both `hookIPC_SYNC` and the body of `myIrqHandlerIPC`. That is `#ifndef GSDD` in misc.c and cardengine.c around the DScheevos call (idlecov submodule, cardengine.c:1627).
  - So the §2.6 watchdog ("CE7 sends 0xA → CE9 → `vblank_entry` re-installs") has **no receiver** in GS DD.
  - The proposal installs once from CE9 `initialize()`. After that, any `OS_SetIrqFunction(V_BLANK, …)` by the game (scene changes, battle entry, movies) silently ends FB for the rest of the session.
  - GS DD has no halt or wait gate, so no other ARM9 module entry can notice.
- **DKR:**
  - No dispatcher signature matches, so `ce9->irqTable` is never found. Then there is no IPC hook, no module VBlank call and no first install.
  - The proposal does flag this (FB-S2, R8), but only as "file the 4th signature".
- **Result:** with Code Lyoko (no set) excluded, the round's stated goal of covering busy engines reaches **0 of 2** titles as designed. The coverage table in §7 ("FB every frame") is not true for either.
- **Fix:** an ARM9-side install path that doesn't depend on CE9's IPC hook.
  - Preferred: a boot-time patch of the dispatcher's indirect-call instruction, per dispatcher variant (`bx r0` in V1, `blx lr` in GS DD, `bx r2` in DKR). Each becomes a `b`/`bl` to `fb_dispatch`, which tests "is this bit 0?" and falls through. B/BL reach ±32 MB, so ITCM 0x01FFxxxx → 0x02D80000 is in range.
    - Cost: a few cycles per IRQ, plus a possibly cold I-line. Price it.
    - It is robust to table rewrites. It needs no pool, no watchdog and no double-call guard.
  - Or give the GSDD and DKR builds an IPC path, and gate FB-busy on FB-S2.

### B2. The runtime table write is a persistent, game-visible write to game RAM (hard rule), not just a "ruling"

- The CLAUDE.md hard rule says "never touch the game's RAM". The accepted precedent is **boot-time code patches**.
- Round 1 built its park mechanism on invariant I1, so that its one DTCM write (3FFC) is never observable by game code. Its effects list states "Not written: … IRQ table" (decision.md §3).
- FB's `table[0] = tramp_k` is observable for the whole session:
  - `OS_GetIrqFunction(V_BLANK)` returns a module address.
  - Code that compares handler pointers behaves differently. A plausible pattern is `if (OS_GetIrqFunction(VBL) != MyVBlank) OS_SetIrqFunction(VBL, MyVBlank);` once per frame.
- With that pattern, FB ping-pongs:
  - The game resets the entry every frame.
  - The watchdog re-installs every ≥2 frames.
  - FB misses about half of all frames, and each miss takes the late path.
  - Pool slots churn, and the "≤ 1 `fb_late` per overwrite" pass mark can't hold.
- nds-bootstrap's entry-2/16 writes are a weak precedent. They happen once, are never re-installed, and are compiled out for GSDD.
- **Fix:**
  - Treat B2 as a hard-rule question equal to Q1, decided together with B1's dispatcher-site patch (a boot-time code patch, which is inside the accepted class).
  - If the table write is kept, FB-M6 must add the "re-set every frame" pattern, and the census must search the 64 ROMs for `OS_GetIrqFunction` callers.

---

## Serious

### S1. Watchdog off-by-one race, re-install over our own trampoline, and duplicate calls in chained handlers

- **Two clocks with no fixed order:**
  - `fb_entry` stores `fb_seen = vblank_count`, and the ARM7 bumps that count in its own VBlank hook.
  - F2 says the ARM7 and ARM9 VBlank order is not fixed. The ARM9 dispatch is usually first, so `fb_seen = n−1` at VBlank n.
- **Spurious trips:** when the ARM9 runs late (IME=0 or an `OS_DisableInterrupts` span across VBlank), the ARM7 compares before the ARM9 stores. It then sees `n − (n−2) = 2` and **trips the watchdog spuriously**.
- **Double calls:** the re-install path doesn't check whether `table[0]` is already a pool slot.
  - A spurious trip installs `tramp_new` with `orig = tramp_old`.
  - Every later VBlank then calls `fb_entry` **twice**: two captures, two `frames++`, and the second one is not pre-handler.
- **Same failure without any race:** a game that chains handlers (`old = Get(VBL); Set(VBL, mine)`, with `mine` calling `old`) gets `fb_entry` twice through `tramp_mine → mine → tramp_old`.
- **Fix:**
  - Never install when `table[0]` is in the pool, and never let `orig` be a pool address.
  - Give `fb_entry` a once-per-VBlank guard with an ARM9-local clock, for example an ARM9 VBlank sequence that counts only in the first `fb_entry` and checks that `VCOUNT ≥ 192`.
  - Number frames with the ARM9's own sequence. Use the ARM7 count only for gap detection, with a tolerance of ±1.
- B1's dispatcher-site patch removes this whole class of problem.

### S2. Retiring 0xA leaves several duties without an owner, and still double-counts

- **The CE9 hook calls `header->vblank` on every IPC sync IRQ**, not only on 0xA ("any IPC sync IRQ may carry it", CE9 cardengine.c). `vblank_entry` then sees `vblank_count` change and calls `dsc_vblank_queue` (start.s:66-89).
  - With FB also counting, frames are queued twice.
  - Once FB is installed, `vblank_entry` must become the watchdog/re-install path only.
- **Duties `vblank_entry` has today, or will have under round 1, that FB's pseudo-code doesn't carry:**
  - `dsc_vblank_queue` / `queue_frame` (the commit handshake with `drain()`, dscheevos.c:2751);
  - `IRQ_MISSED`, `IRQ_TIMED` / idle-cut stats;
  - round 1's `wait.vblank_ended` (decision.md §4);
  - the display's **VBlank fail-safe** ("if DISPCNT mode reads 3 and strip armed this frame → mode 1", design-notes.md, display requirements).
- **The fail-safe gap:** in watchdog gap frames, and permanently in GS DD (B1), no VBlank hook runs at all. The strip fail-safe then has no site, which is a visible-glitch path.
- **Fix:**
  - List every `vblank_entry` duty and move each into `fb_entry`. Pre-handler is a *better* site for the fail-safe.
  - State the display interaction explicitly.

### S3. The "fresh" rule only applies at a module halt, so the FB path runs far more often than §0.5 claims

- `fresh` is set only after `wfi` (§2.3). Two cases never produce a fresh copy:
  - **W busy at VBlank:** W evaluating when VBlank arrives (the VBlank reaches the game through `irq_head`'s unpark). This is common for medium and heavy sets in round 1's P1a/P1b, and normal in RE:DS.
  - **A late game IRQ:** a non-VBlank game IRQ close to VBlank, before the re-copy finishes. Kirby has 38 wakes per frame, about 1 per 7 lines. A copy takes 1-1.4 lines, so roughly 15-20% of frames lose freshness from timing alone [est].
- **30 fps and lag-heavy games** (most DS 3D titles; Platinum has about 50 lag intervals in AB.md) take the FB path on every other VBlank.
- So "waiting games, non-lag frames: less game time than today" is not established.
- **Fix:**
  - Let `irq_head` set `fresh` when it unparks for a VBlank with a complete, untorn copy.
  - Report the FB-path frame share per game, and its cost (lag rate × C), as a FB-M4 output with a pass mark.

### S4. The throughput floor leaves out I-cache misses and the refill afterwards; the warm halt measurements are not a lower bound

- **I-cache:** `C_min` counts only data-line misses (§5). In a busy engine, a full frame of game code runs between captures, so the capture code is **cold every VBlank**:
  - `copy_snapshot` / `copy_words_same` / `run_chains` / `run_step` is about 1-2 KB, or 32-64 I-lines.
  - At ≈ 46 cycles per fill (F9), that adds **1.5-3k cycles** to the "worst (all cold)" 1.6k for MKDS.
- **Warm measurements:** the "low end" model (MKDS 4k measured at the halt, 2.6 cycles/instr) comes from halt captures whose code and data are warm across back-to-back wakes (Kirby: 38 per frame). That is not representative of a once-per-frame IRQ capture.
- **Refill:** the game then refills what the capture evicted, about (code + data + shadow lines) × 46 = 3-5k more in game time. G6 names this but the numbers leave it out. The real game cost per FB frame is about **C + refill ≈ 1.5-2 × C**.
- **Fix:**
  - Restate C_min with I-misses and refill included.
  - Have FB-M1 report game cycles per frame (brief2 rule), not just module cycles in IRQ mode.

### S5. FB-M3 can't separate lag from divergence, and a spin stub doesn't model cache pollution

- **Divergence:**
  - "Zero frame-hash differences at D = 4C" fails for any game that seeds an RNG or schedules work from VCOUNT or timers (GS DD's scheduler is time-based).
  - A 1-cycle shift can then diverge the run forever without a single lag frame. The test would fail for the wrong reason, and the owner would get no usable number.
- **Cache:** the injected delay is a spin, so it evicts no lines. S4's refill cost is invisible to it.
- **No lag definition for busy engines:** the "frame done" PC heuristics are unconfirmed (the proposal itself says so).
- **Fix:**
  - Measure lag directly. Count presented frames per VBlank (write-watch on `SWAP_BUFFERS` 0x04000540 for 3D games, the 2D/OAM commit for 2D), and use the display's T1 slack probe.
  - Inject a cache-thrash stub that touches the set's real footprint (N_lines data + code size), not a spin.
  - Keep frame hashes as a diagnostic only.

### S6. Conflicts with round 1's masked-span bound (M11 ≤ 400 cycles) and its IRQ-latency marks

- FB runs C (2-8k cycles at the cap) entirely in IRQ mode with I set. Today's capture runs in 16-word chunks for this reason (ARCHITECTURE.md "Snapshot when the game halts").
- Round 1's M11 bounds module masked spans at ≤ 400 cycles p100, and its M4 bounds VBlank/IPC/timer latency at ≤ 800 cycles.
- **Fix:** the proposal must state that FB is an exception, with its own bound (FB-M5), and say why "VBlank lines only" is enough. Note that H-Blank and VCOUNT IRQs do fire on lines 192-262, and some games prepare line-0 raster state there.

### S7. Concurrency with the ARM9 evaluator's ring is unspecified, and P1 needs it

- Round 1 keeps the ARM9 evaluator (W) for most games. FB-for-lag-frames (R3) in that world writes into `S.slots` / `S.arena`, and reads `S.current` and `S.chains`, from IRQ context.
- W can be mid-`drain()`, mid-`arena_alloc`, mid-table-rebuild or mid-`sync_save` at that moment.
- Today's IRQ copy avoids this with `dsc_vblank_queue`'s "one side moves the ring" rule, and runs only once per session (MODE_COPY).
- The proposal specifies only the ARM7 hand-off ring. R10 (chain purity) is necessary but not sufficient.
- **Fix:**
  - Specify the ARM9-evaluator case: an IRQ-owned staging slot and an atomic `S.current` publish.
  - Or state that FB capture is enabled only with the ARM7 evaluator.

### S8. Delta encoding breaks if any slot is dropped

- The ARM7 rebuilds full slots from deltas against the previous frame.
- R6 predicts "frames queue and drop" for GS DD. One overwritten or dropped delta slot corrupts the ARM7's image **for every later frame**, not just one.
- **Fix:**
  - Back-pressure. When the ring is full, the ARM9 merges deltas into the open slot and counts the frame `dropped` explicitly.
  - Periodic key-frames, and a sequence check on the ARM7.
  - Budget the ring in the 2 MB module region, which is already short (session-coordination memory).

### S9. "Exact" overstates the RA-correctness claim

- §3 says the two reference points "make this exact". They make FB self-consistent. They don't make it RetroArch-equal.
- Line 192 pre-handler and line 262 differ systematically in waiting games. The emulator sample includes the VBlank handler and up to 71 lines (~300k cycles) of next-frame logic: new input latched, counters bumped.
- Conditions that mix variables updated on either side of that boundary can differ in hit counts and unlock frame.
- Neither FB-M2 nor round 1's M7 compares against a line-262 reference.
- **Fix:** add a differential test. melonDS snapshots at (a) 192 pre-handler and (b) end of 262, both evaluated on every real set with rcheevos. Compare unlock frames and hit counts, and send the result to ra-compliance.

### S10. Hidden per-set and per-game tuning

- `L` is "chosen per game class by FB-M4" (§2.5).
- "IRQ-eligible = p100 ≤ C_cap" comes from FB-M1 per set in melonDS.
- Both become hand tables unless computed generically. A table of that kind would breach "no exclusions or lists".
- **Fix:**
  - Use one global `L`, or a runtime-adaptive one (for example, start copying at the observed p90 of last-wait VCOUNT).
  - Predict C from the plan's shape (lines, words, chain steps, CS_GENERIC count) in the launcher's dry run.

### S11. The busy-engine premises are unmeasured

- **VBlank IRQ:** nothing checks that GS DD or DKR have IE bit 0 on and IME on in gameplay. FB-S1 checks `table[0]` but not IE.
- **Capture size:** GS DD's and DKR's capture sizes are unknown (§4 "not measured"). If either is above C_cap, the busy-engine story ends there.
- **Fix:**
  - Add IE/IME sampling at VBlank to FB-S1.
  - Run FB-M1 for GS DD and DKR first, before anything else is built.

---

## Minor

- **Trampoline register clobbers:** `tramp_k` clobbers r12, and `fb_entry` clobbers r0-r3. A handwritten handler may rely on what the dispatcher leaves there:
  - GS DD: r12 = 0x04000210, r1 = the acked bit;
  - V1: r0 = the handler, r1 = the table.
  - Save and restore r0-r3 and r12, then jump through a literal (`ldr pc, [slot]`), so the handler gets bit-identical registers.
- **G7 has an extra benefit:** retiring 0xA also stops delivering an unexpected IPCSYNC value 0xA to the ARM9 (the game can read REG_IPCSYNC). The proposal could claim that.
- **Citations:** they refer to the idlecov submodule (cardengine.c:2062-2064). Main nds-bootstrap has `dscheevosVBlank` at 1888-1890. State the tree.
- **`fresh` can go stale:** if `table[0]` is overwritten, `fresh` stays 1 until the next `fb_entry`, which can be frames later, and that copy is then committed as fresh. Tag `fresh` with the ARM9 VBlank sequence, or clear it in the watchdog path.
- **Uncached store cost:** this depends on whether the 0x0Cxxxxxx mapping is bufferable under the game's PU setup. If it is not, every changed-word store costs about 20 cycles, not overlapped. Check the PU region.
- **`CS_GENERIC` in IRQ context:** `rc_get_modified_memref_value` evaluates modifier operands. If a modifier is a memref, it may read `value` fields that W's in-progress `rc_update_memref_values` is changing. Verify that `S.chains.mods` are deep copies before relying on the "[guess]".

## Best ideas worth keeping

1. **One sampling instant:** VBlank, before the game's handler, for every game. The halt copy is its zero-cost precomputation, under a strict "no game instruction since" rule. This replaces carry and `frames_uncaptured` with real memory, as an emulator would.
2. **Retire the steady-state 0xA IPC.** It saves about 1.4-1.7k cycles per frame and removes a game-visible IPC sync value.
3. **Uncached-mirror ring with delta encoding** for the ARM7 hand-off (with S8's back-pressure and key-frames).
4. **Static dispatcher findings:** the corrected Alt5 rule (table end − 31×4), the DKR 4th signature, and the evidence that DKR's VBlank path today is suspect. Send these to nds-bootstrap.
5. **A 4× delay-margin test as the form of the no-lag proof** (with S5's corrections).
6. **Halt-copy policy:** "copy only near VBlank" is a lever on round 1's 2,843 line fills per frame (with S10's generic `L`).
