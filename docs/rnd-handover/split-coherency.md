# Red-team of the SPLIT-mode ARM9→ARM7 hand-over: coherency and ordering

I found two blockers. First, `rc_client` reads `old_state` and `old_measured_value` from the claimed trigger before calling `result()`, so most claimed unlocks would be lost. Second, the NWRAM lane's set zone puts the objects the ARM7 must read into NWRAM-B/C, which only the ARM9 can see. There are five serious findings and three minor ones. Nothing here has been run on hardware or in the emulator; the evidence is code reading, the ARM946E-S TRM and GBATEK.

Sources: rcheevos 683197d (`external/rcheevos`), design at `evalspeed-team/arm7-notes.md:198-321` plus ra-compliance R1-R6 (`:441-462`). TRM = `scratchpad/arm7h/trm.txt` (ARM DDI 0155A).

## 1. Findings, ranked

### BLOCKER B1: `old_state` and `old_measured_value` are read from the claimed trigger before `result()`
**Evidence**
- `rc_client.c:6059-6061`: `old_measured_value = trigger->measured_value; old_state = trigger->state;` comes *before* the `rc_evaluate_trigger` call. The patch replaces only that call (line 6061).
- All events are then gated on `if (new_state == old_state) continue;` (`:6087`). The progress popup is gated on `trigger->measured_value != old_measured_value` (`:6069`).
- The ARM7 has no cache and writes the header straight to RAM. The design wants the ARM7 to finish "before the ARM9 reaches it".

**Failure scenario**
1. Claimed achievement X triggers at frame f. The ARM7 writes `state=TRIGGERED` to RAM.
2. Between frames the ARM9's copy of X's header line was evicted (game code and the memref pass stream far more than 4 KB). The ARM9 reads `old_state` with a line fill and gets `TRIGGERED`.
3. `result()` returns `TRIGGERED`, so `new_state == old_state`. No TRIGGERED event, no award.
4. From f+1 on, `rc_evaluate_trigger` returns INACTIVE for a TRIGGERED trigger (`rc_trigger.c:166-168`). X is stuck active and never unlocks.

Progress indicators are lost the same way. If the line fill lands mid-write on the ARM7, the values are torn. If the line is still cached, the old values are correct. So the outcome depends on cache state, and in the common case it is wrong.

The host harness's sequential mode (frame_begin evaluates everything first) would fail on the first claimed unlock. The design has the bug, but the harness will catch it.

**Fix (patch)**
- For a claimed achievement, `result()` returns `{new_state, old_state, old_measured_value, state, measured_value}`. The ARM7 fills this record: it reads the old values before evaluating, then writes the new ones and a sequence number last.
- rc_client uses the record and does not touch the trigger's header in the per-frame path. `measured_target` and `measured_as_percent` don't change after parse, so the ARM9's own copy is safe to read.
- This also removes the per-frame header invalidate-and-refill (S3).

### BLOCKER B2: the NWRAM lane puts ARM7-read objects in ARM9-only NWRAM-B/C
**Evidence**
- The module heap's set zone 0 is NWRAM-B/C "if granted", and every block allocated during the load goes there first (`dscheevos-nwram/dscheevos/module/src/heap.c:85-91, 274-293`; commits a1ed053, 6a31a19).
- Memref lists are `malloc`'d during the load (`rc_memref.c:175-188`; `rc_alloc.c:176-205`), and so is the claimed trigger unless `alloc` routes it elsewhere.
- In DS mode all 16 B/C slots are owned by the ARM9 and locked (`main.arm9.c:410-436`). GBATEK (DSi New Shared WRAM): slots owned by the other CPU "seem to read as zerofilled".

**Failure scenario.** A claimed condition's `operand1.value.memref` points to `0x0370xxxx`. The ARM7 reads `value=0` (or its own mapping there) and evaluates on zeros. Achievements fire or fail silently. The ARM7 lab cannot show this, because it maps NWRAM to the ARM7 in DSi mode (`sys7.c:63-127`).

**Fix**
- In SPLIT mode, everything reachable from a claimed trigger must be in main RAM (`0x02000000-0x02FFFFFF`). That is the trigger itself, every memref and modified memref it references, and their parent and modifier operands, transitively.
- Route memref-list allocations to zone 1 (main RAM). They can be tagged with the same `--wrap` of `rc_preparse_reserve_memrefs` that memplan used.
- In `claim()`, walk the trigger's pointers and refuse the claim if any target is outside main RAM.
- The ARM9 keeps NWRAM only for local triggers, leaderboards, rich presence and strings.

### SERIOUS S1: the ARM9 waits inside `do_frame` with no time limit, so frames go uncaptured during an ARM7 drought, and a reset during the wait races the ARM7
**Evidence**
- `dsc_idle` runs `rc_client_do_frame` in the game's idle thread with IRQs on (`module.c:651-655`). Captures happen in `module_idle` before and after it (`module.c:647-651`, plus the halt capture).
- `result()` and R2's `frame_end` spin until the ARM7 is done. The ARM7 evaluates only in its own idle time, and can be busy for hundreds of frames (MKDS boot: 603 intervals with no ARM7 halt, `arm7-notes.md`).
- `start.s:218-220`: on reset, "work that was preempted will never resume". `dsc_reset` then runs `rc_client_reset`, which calls `rc_reset_trigger` (`rc_client.c:4896-4905`).

**Failure scenarios**
1. **Drought.** The ARM9 idle thread is stuck in `result()` for N frames. No halt captures happen, so those VBlanks become `carry` slots (previous frame's memory) or dropped frames. That breaks "every frame evaluated, exact memory".
2. **Reset.** The game soft-resets while `do_frame(f)` is preempted mid-window. The ARM7 is still evaluating claimed trigger T. The ARM9 zeroes T's `current_hits`, and the ARM7 then writes `current_hits+1`. A pre-reset hit count survives the reset, which can produce an invalid hardcore unlock.

**Fix**
- **The wait.** Make the wait an idle-halt loop: while waiting, run the module's halt-capture path (it doesn't touch rc_client), then WFI. On each wake, poll the ARM7's done word. This keeps capturing through a drought and stops the CPU spinning on the main-RAM bus.
- **Reset and unload.** Add an abort/ack handshake. `dsc_reset` sets `abort`. The ARM7 acks between triggers. `rc_client_reset` is deferred (non-blocking: queued frames stay in the ring) until the ack arrives. `unload` and `deserialize` need the same handshake.
- **Optional.** Add per-trigger ownership (Peterson-style, two flags, each written by one side, in uncached or invalidate-polled words) so the ARM9 can evaluate a trigger the ARM7 hasn't started. This uses the same code on the same memrefs, so the result is identical. Ownership questions belong to arm7eval and idle-coverage.

### SERIOUS S2: "uncached words" don't exist for SDK5 titles, so polling a cached word can hang
**Evidence**
- `DSCM_UNCACHED(p) = p - 0x02000000 + 0x0C000000` (`module/include/dscheevos_module.h:300`).
- The crt0 scan shows the 8 pure-SDK5 NTR titles (Golden Sun DD, Ghost Trick, Kirby Mass Attack, Sonic Colors, ...) set region 3 as cacheable write-back.
- When nds-bootstrap patches region 3 it writes `PAGE_32M|0x08000000` (`patch_arm9.c:1070-1072`). That covers `0x08000000-0x09FFFFFF` and leaves `0x0C000000` outside region 3. Unverified what access then does; it may be outside every region and fault.

**Failure scenario.** In a SDK5 game, `result()` polls the per-trigger counter through `0x0C…`. The first read allocates the line, every later poll hits the stale copy, and the wait never ends (it ends only on a chance eviction). The ARM9's result reads are stale too.

This also affects today's module: the journal "written only through the uncached mirror" (`module.c:20, 413, 446`) is cached for those games.

**Fix**
- At init, read CP15 c2/c3/c6 and decide per game whether the alias is actually uncached.
- Otherwise use invalidate-by-MVA (c7,c6,1) followed by a load. This is safe because the ARM9 never writes these words. Put each word on its own line and poll it slowly (S1's halt loop).
- Add `0x0C000000` and `0x027FF000` to the runner's MPU print.

### SERIOUS S3: invalidating and re-reading the header pollutes the cache with one line per claimed trigger per frame
**Evidence**
- Design: "invalidate THAT trigger's header line(s) … return the published state". rc_client then reads `trigger->state` and `measured_*` (`rc_client.c:6065-6080`), which fills the line again.
- Claimed achievements per set: Kirby 51, Platinum 102, HG/SS 137 (`memplan/rc.md:29-35`).

**Failure scenario.** With 100 claimed triggers, each frame does 100 line fills (~46 cycles each, GBATEK) and evicts 100 of the D-cache's 128 lines. The game later refills those at ~46 cycles each. That is about 10k ARM9 cycles per frame and most of the cache, the same pollution debt as Kirby's +21% that SPLIT is supposed to remove.

**Fix**
- Use the B1 record and read it uncached where possible.
- At `frame_end`, after the barrier, run invalidate-only (no read) on the header lines of the triggers evaluated this frame. That is N MCRs with no fills. Later reads, such as `rc_client_update_achievement_display_information` in `raise_achievement_events` (`:6209`, which reads the header at `:4126-4156`) or list building (`:4337, 4526, 4606`), then fill only what they actually read.

### SERIOUS S4: neither melonDS nor the host harness can detect a coherency bug
**Evidence**
- melonDS's cache model is timing-only and memory stays coherent (`CP15.cpp:450-455, 501-546`).
- The host harness is coherent by construction.
- The DSi run is a smoke test only (H5).
- So the design's claim that the "melonDS end-to-end replay test" catches "(ii) a coherency/ordering bug in the glue" (`arm7-notes.md:318-319`) does not hold.

**Failure scenario.** A missing clean, a stale-neighbour write-back (M1) or the S2 stale poll passes every planned test and diverges only on the console, where nothing is logged per frame.

**Fix.** Add a coherency checker to melonDS's existing cache model (experiment X3).

### SERIOUS S5 (a): clean-all by set/way also cleans the game's dirty lines; clean-by-MVA is only cheaper for small memref storage; filtering by tag is best
**Evidence**
- Every memref, and every modified memref, is written every frame (`changed=0` at least; `rc_memref.c:717-726`), so every resident memref line is dirty at `frame_begin`.
- The memref storage size in the design is wrong. Modified memrefs are 56 B each:

  | Game | Memref storage (rc.md) | Design's figure |
  |---|---|---|
  | HG/SS | ~286 KB (~8,950 lines) | "~30 KB" |
  | Platinum | ~114 KB (~3,580 lines) | |
  | TWEWY | ~29 KB | |
  | Kirby | ~2.5 KB (~80 lines) | |

  Source: `memplan/rc.md:46-47`.
- The TRM says the replacement victim is chosen by "a segment counter" (`trm.txt:2494-2497`). That reads as one counter for the whole cache, so a resident line's lifetime can't be bounded per set. Clean-by-MVA therefore has to cover all of the storage.

**Cost estimates (unverified).** Assumptions: about 5 cycles per MCR plus loop overhead, and about 48 ARM9 cycles per dirty-line write-back (GBATEK 16-bit N16/S16). The TRM gives no cycle counts for cache operations.

| | Kirby | TWEWY | Platinum | HG/SS |
|---|---|---|---|---|
| Clean by MVA over memref storage | ~0.4k + ≤3.8k write-backs | ~4.5k | ~18k | ~45k |
| Clean-all by set/way | ~0.64k + ≤6.1k | same | same | same |

- The design's "~1-3k" holds only when few lines are dirty.

**Game exposure**
- Clean-all writes back whatever game dirty lines survive the memref pass. Those are the lines exposed to idle-coverage's (a), (b) and (b').
- For big sets, the memref pass has probably already evicted the game's lines, so clean-all adds little.
- For Kirby-sized storage, which is exactly where SPLIT shrinks the ARM9's streaming, game lines survive. The clean is then a new game-visible effect, not one the module already causes.

**Fix**
- Preferred: a filtered clean by set/way. For each of the 128 entries, read its tag with the cache debug ops: `MCR p15,3,rd,c15,c0,0` sets the index, `MRC p15,3,rd,c15,c2,0` reads the tag with its valid and dirty bits (`trm.txt:2255-2330`). Clean by index (c7,c10,2) only entries whose address is in a module range. Cost is about 1k cycles plus memref write-backs only, and it never touches a game line. Restore the debug index register afterwards. This is unverified on the DSi.
- Fallback: clean by MVA when the storage is under about 100 lines, otherwise clean-all.
- In every variant, also clean the ranges of claimed triggers written at a lifecycle point before the next `frame_begin`.

### MINOR M1: ARM9 writes to a stale cached copy of a claimed trigger write back stale neighbouring bytes
**Evidence**
- Dirty bits are per half-line (TRM 3.3).
- The ARM9 reads claimed conditions outside the header in `rc_trigger_contains_memref` (`rc_client.c:1304`). It writes them in `rc_reset_condset` (`current_hits` only, `rc_condset.c:772-778`) and in deserialize.

**Scenario.** On reset, `current_hits=0` lands in a stale cached half-line. The clean then writes back that condition's stale `is_true`.
- I checked the layouts: the only mutable byte that can share a half-line with `current_hits` is `is_true`, which is output-only.
- So evaluation stays correct, but `rc_runtime_progress` serialization differs (`rc_runtime_progress.c:351-372`).
- If serialization is ever used, the ARM9 reads stale `current_hits` and `is_paused` everywhere, because these lines are never invalidated.

**Fix.** Make it a rule: before any ARM9 write to, or read beyond the header of, a claimed trigger, invalidate the trigger's whole range. This is safe because no claimed line is dirty on the ARM9, by invariant. After writing, clean it.

### MINOR M2: `alloc` alignment
**Evidence.** Zone blocks are 8-aligned with an 8-byte header (`heap.c:85-90`).

**Scenario.** A trigger ending at 8 mod 16 shares a half-line with the next block's header. Freeing that neighbour during play dirties the half-line and writes back the stale last-condition `is_true`. The damage is again limited to `is_true`.

**Fix.** Align `alloc` to 32 and round the size up to 32 (or use a dedicated pool). Assert both ends are aligned. Also add a `free` hook: the patch has `alloc` but nothing to free, so unload or reload leaks.

### MINOR M3: smaller protocol details
- **Keying.** Key the per-trigger counter on a do_frame sequence number, not on `slot->frame`: repeated and carry slots reuse the frame number (`dscheevos.c:2975-2990`).
- **`client` argument.** `rc_client_evaluate_external_trigger(client, …)` takes `client`. The ARM7 must not dereference it (ARM9-owned, changes during the frame). Drop the parameter or assert.
- **Build ABI.** The ARM7 build must use the same `-include` as the ARM9 build if memplan's float-operand lever is adopted. Add a cross-build `_Static_assert` on the sizes and offsets of `rc_trigger_t`, `rc_condset_t`, `rc_condition_t`, `rc_operand_t`, `rc_memref_t` and `rc_modified_memref_t`.

## 2. What is sound
- **No memory reads in evaluation.** `rc_evaluate_trigger` reads no game memory for rc_client triggers: `has_memrefs=0` makes `rc_update_trigger_memrefs` a no-op (`rc_trigger.c:134-140, 52, 176`). The only reads outside the memref pass are parse-time calls with NULL (`rc_condition.c:416-420`).
- **No mutable globals.** Evaluation touches no mutable global state: only const tables (`rc_memref.c:13, 467, 580-655`). RP variables (`rc_value.c:383, 478`) can't be referenced by achievement triggers (`parse.variables` is set only for RP, `rc_runtime.c:452, 479`). Remember/Recall is resolved into modified memrefs at parse (`rc_condset.c:146-200`).
- **Memref storage has one writer.** Only the ARM9 memref pass writes it (`rc_client.c:5992-6043`). RP and leaderboards use the shared pool (`has_memrefs=0`). So the ARM9 writes nothing the ARM7 reads during the window. False sharing on memref lines is harmless because the ARM7 only reads them.
- **Clean, not invalidate,** at `frame_begin` is right: the line stays in the cache (TRM 3.3.5). So is the drain before the signal.
- **Ordering of ARM7 results.** The ARM7 has no cache or write buffer, so writing the record, then the sequence number, keeps that order. The ARM9 loads in order.
- **ARM7 can address the module.** SCFG_EXT7 bits 14-15 are "set via ARM9" (GBATEK), so the ARM7 sees the full 32 MB and `0x02D8xxxx` pointers are valid on it.
- **R1, R2 and R4 as the code stands.** R1 (work list) and R2 (`frame_end` barrier) are necessary and enough for overlap between memref passes. R4 holds today: lists are built in `dsc_idle` outside `do_frame` (`dscheevos.c:3007`), and reset is deferred to idle (`module.c:590-641`). The exception is the abandoned window in S1.

## 3. Decisive experiments (pass/fail decided now)
- **X1, host harness, sequential mode, glue as designed (B1).** Prediction: FAIL with a missing TRIGGERED on the first claimed unlock. With the B1 record fix: PASS (zero diffs) on MKDS and Kirby with 20 random partitions.
- **X2, placement check (B2).** In the NWRAM-lane build with SPLIT on, walk every claimed trigger's reachable pointers at claim time. PASS means zero pointers outside `0x02000000-0x02FFFFFF`. Prediction: FAIL.
- **X3, melonDS coherency checker (S4, M1, M2, S5).** For each ARM9 D-cache line, record the fill time. For each byte, record which bus master last wrote it.
  - Flag (i) an ARM9 load hit on bytes another master wrote after the fill.
  - Flag (ii) a dirty half-line write-back over bytes another master wrote after the fill.
  - Flag (iii) an ARM7 read of bytes whose newest value sits in an ARM9 dirty line.
  - Add a switch for a global vs per-set round-robin counter; melonDS now models it per set (`CP15.cpp:472-480`).
  - Run 10k frames each of Kirby, MKDS and Platinum with random partitions, cache model on, `--thumb-cycle`. PASS means zero flags in module ranges. Also count game lines cleaned per frame: clean-all vs tag-filtered. PASS for tag-filtered means 0.
- **X4, game ARM9 cycles per frame (gamecost harness, Kirby and MKDS).** Variants: hand-over stubbed; design (header invalidate and read); fixed (record plus `frame_end` invalidate plus tag-filtered clean).
  - PASS for "fixed": at most +0.2% game ARM9 cycles per frame over stub, and at most +2 game line refills per frame.
  - Prediction for "design": about N_claimed extra refills per frame (FAIL).
- **X5, DSi XL lab ROM (logged to SD at exit).**
  - (a) CP15 c0,c0,1 reads `0x0F0D2112`.
  - (b) Round-robin counter global or per set: fill set 0's four ways, fill three lines into set 1, fill one line into set 0, then read set 0's tags with the debug ops. A global counter evicts way 3, a per-set one evicts way 0. Prediction from the TRM wording: global.
  - (c) Cycle cost of clean-by-MVA on an uncached, clean and dirty line; of set/way clean with 0, 64 and 128 dirty lines; of a drain; and of the tag-filtered loop. Decision: use tag-filtered if the debug tag read works and costs at most 1.5k cycles.
  - (d) The ARM9 stores to a cached line without cleaning and the ARM7 reads stale. After clean and drain the ARM7 reads fresh. Under the SDK5 MPU layout (region 3 write-back), a `0x0C` alias read is cached (confirms S2).
  - (e) The ARM7 reads `0x03700000` while the ARM9 owns all B/C slots and gets zeros (confirms B2).

## 4. Questions
**For the ARM7 lane**
- Will you take the B1 record API, the `free` hook and the abort/ack handshake into the patch? The abort/ack is needed for reset, unload and deserialize in any case.
- Is the `result()` wait allowed to run the module's halt-capture loop?
- How will memref allocation be routed in SPLIT mode, given B2?
- The ARM9 memref pass still streams the whole memref storage every frame (Platinum ~114 KB). SPLIT does not remove that ARM9 pollution. Is that accepted in your cost model?

**For idle-coverage**
- Are the dirty-line (88%/61%) and "evaluation already evicts" figures still valid if the counter is global (X5b)? melonDS models it per set.
- Can tag-filtered cleaning, the debug index and tag-read MCR/MRC with the index register restored, be treated as invisible to the game? These are CP15 debug registers, not MPU or cache configuration.
- Who owns the SDK5 uncached-alias question (S2), which also affects today's journal?

**For ra-compliance**
- Is ARM9 fallback evaluation of a claimed trigger the ARM7 hasn't started acceptable under the patch contract? It is the same code on the same memrefs.

Scripts: none new. Files read include `~/Developer/DScheevos/evalspeed-team/arm7-notes.md`, `~/Developer/DScheevos/dscheevos-rnd/external/rcheevos/src/client/rc_client.c`, `~/Developer/DScheevos/dscheevos-nwram/dscheevos/module/src/heap.c`, `~/Developer/DScheevos/dscheevos-rnd/dscheevos/module/src/start.s`, `rnd scratch arm7h/trm.txt`.