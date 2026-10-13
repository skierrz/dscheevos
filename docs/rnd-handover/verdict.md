
# Consolidated verdict: ARM9→ARM7 hand-over (SPLIT and WHOLE-SET S0)

Sources: three red-team reviews (split-coherency, split-timing-bus, wholeset-ring). The "helpers" review returned null. Nothing was run in melonDS or on hardware. Before ranking the top findings I checked them against source. "Unverified" means not checked against source or hardware.

## 0. Top findings checked against source

| Claim | Result | Evidence |
|---|---|---|
| Coherency-B1: rc_client reads `old_state`/`old_measured_value` from the claimed trigger after the ARM7 may have written it | **Refuted as stated.** The patch snapshots both values into `claimed_old_state[]`/`claimed_old_measured_value[]` in `rc_client_begin_claimed_triggers` before `frame_begin`, and the per-trigger loop uses those copies. | `dscheevos-arm7eval/external/rcheevos` a93f5a7, `src/client/rc_client.c:6112-6143`, `:6158-6163` |
| What remains of Coherency-B1 | **Confirmed, downgraded to serious (S1 below).** Those snapshots are ARM9 cached loads of `trigger->state`/`measured_value`. After `result()`, rc_client also reads `trigger->state` (RESET path) and `trigger->measured_value` through the ARM9 cache. | `rc_client.c:6172-6180` |
| NWRAM-B/C is ARM9-only and the set zone goes there first | **Confirmed.** The heap tries zone 0 first ("NWRAM (if granted), then main RAM"). MBK2-5 = `0x8C888480`/`0x9C989490` (master bits 0 = ARM9). `rc_memrefs_init` mallocs the memref lists during the load. | `dscheevos-nwram/dscheevos/module/src/heap.c:91, 265-272`; `dscheevos-nwram/.../bootloader/source/arm9/main.arm9.c:64-69`; `dscheevos-rnd/external/rcheevos/src/runtime/rc_memref.c:175-187` |
| The ARM7 entry is `runCardEngineCheckHalt` then `swi 6` with no IRQ check | **Confirmed** (ARM and Thumb variants). | `external/nds-bootstrap/retail/cardenginei/arm7/source/card_engine_header.s:445-463` |
| The `0x0C000000` "uncached mirror" is cacheable in SDK5 titles | **Confirmed.** Herbert's Revenge (SDK5 crt0) sets region 3 = `08000035` (128 MB, `0x08000000-0x0FFFFFFF`), C=`0x4A`, B=`0x0A` (region 3 cacheable, write-back). If nds-bootstrap applies its region-3 patch (`PAGE_32M\|0x08000000`), `0x0C…` falls outside every region. Either way it is not NCNB (non-cacheable, non-bufferable). | `scratchpad/arm7h/dc/mpuscan.txt` (Club Penguin EPF Herbert's Revenge); `external/nds-bootstrap/retail/bootloader/source/arm7/patch_arm9.c:900-903`; `dscheevos/module/include/dscheevos_module.h:300` |

## 1. Blockers

### B1. Everything the ARM7 reads must be in main RAM, but the NWRAM lane puts it in ARM9-only NWRAM-B/C (confirmed)
This affects SPLIT and S0 alike.
- **Data placed in NWRAM-B/C:**
  - memref and modified-memref lists (malloc at load);
  - triggers that are not routed through `alloc()`;
  - `claimed_work` and the old-value arrays (`rc_buffer_alloc(&game->buffer, …)`, a93f5a7 `rc_client.c:2168-2180`);
  - in S0, the whole rc_client heap.
- **Effect:** the ARM7 reads zeros (GBATEK, DSi New Shared WRAM: slots owned by the other CPU "seem to read as zerofilled"; unverified on hardware) and evaluates on garbage, silently.
- **Not caught by the ARM7 lab:** it maps NWRAM to the ARM7 in DSi mode.

**Fix**
- In any ARM7 mode, route everything the ARM7 can reach into the main-RAM zone: memref lists (tag via the `rc_preparse_reserve_memrefs` wrap), claimed triggers, `claimed_work` and the old arrays.
- `claim()` walks each trigger's pointers transitively and refuses any target outside `0x02000000-0x02FFFFFF`.
- S0 instead needs main RAM, or ARM7-mastered NWRAM split off before the MBK lock. That is an owner ruling against the NWRAM lane's all-to-ARM9 grant.

### B2. The ARM7 hook loses the game's wake-up and delays its ARM7 loop (confirmed structure)
- `newSwiHalt` runs our code and then always executes `swi 6`.
- If a game IRQ arrives during an evaluation pass, the handler acknowledges it. The game's loop then halts until the next IRQ, so it resumes up to one frame late.
- The site is found by a DISPSTAT constant, so it may be a working main loop rather than the idle thread (idle-coverage red-team S6).
- Consequences: touch, key, sound and PXI replies are delayed, and the ARM9 can lag. This breaks the "game behaviour unchanged" rule.

**Fix (owned by idle-coverage)**
- Count IRQs in CE7's IRQ wrapper (`cardengine.c:2119-2141`).
- Evaluate in slices of at most S ARM7 cycles.
- After a slice, if the IRQ count moved since hook entry, return without `swi 6`.
- `claim()` refuses any trigger whose worst-case ARM7 cost exceeds S.
- Between slices, also serve a pending `sharedAddr[3]` card request, because card reads are served only in this hook.

### B3. There is no uncached or NCNB word the ARM9 can safely poll or publish through on SDK5 titles (confirmed)
- The module's `DSCM_UNCACHED` alias is cached write-back, or faults, on SDK5 crt0 games.
- The only NCNB windows there (`0x027FF000`, `0x02FFF000`) belong to the game or SDK.
- Possible failures:
  - the ARM9 polls a stale cached ARM7-written word and hangs;
  - ARM9 stores sit dirty and the ARM7 reads old bytes;
  - today's journal "via the uncached mirror" is cached on these games (`module.c:20, 413, 446`).

**Fix**
- At init, read CP15 c2/c3/c6 and decide per game whether the alias is NCNB.
- Where it is not, every handshake line is module-only and 32-byte aligned and uses explicit MVA operations:
  - ARM9 writes: clean `c7,c10,1` then drain `c7,c10,4`;
  - ARM9 reads of ARM7-written words: invalidate `c7,c6,1` then load.
- Invalidating is safe because the ARM9 never writes those lines.
- An nds-bootstrap-added NCNB region would change the MPU and needs an owner ruling.

### B4. The `frame_end` barrier ties ARM9 progress to the ARM7 schedule with no fallback (SPLIT)
- The ARM9 waits in `result()`/`frame_end` with no time limit.
- During an ARM7 drought (load screens; worst windows HG/SS 44%, Platinum 51%) the ARM9 idle thread is stuck there. No halt captures run, so frames become carry or drops, which breaks "every frame evaluated".
- A soft reset, unload or deserialize during the window races an ARM7 that is mid-trigger. A pre-reset `current_hits` can then survive the reset and produce an invalid hardcore unlock.

**Fix**
- **Wait loop:** run the module's halt-capture path, WFI, then poll (with backoff).
- **Lifecycle:** add an abort/ack handshake for reset, unload and deserialize.
- **Take-back:** per-trigger take-back, where the ARM9 evaluates claimed triggers the ARM7 has not started.
  - Use single-writer `taken`/`withdraw` words with Dekker ordering. This needs NCNB, or the B3 fallback, and assumes the ARM7 has no write buffer (unverified).
  - The ARM7 masks IRQs per trigger, which is bounded by the B2 budget.
  - The patch already allows the host to call `rc_client_evaluate_claimed_trigger` itself, so results stay identical. ra-compliance should confirm this.

### B5. ARM7 main-RAM traffic may raise the game's own ARM9 cycles per frame (bus contention, unmeasured)
- Estimate for MKDS SPLIT at f=0.52: 16-23k ARM7 main-RAM accesses per frame, about 29-41% of the main-RAM bus.
- Game ARM9 cost: about +3.6% expected and up to about +20% worst case, from line fills waiting behind ARM7 accesses. This is my arithmetic from `gamecost/out/mkds-gc-base/ab.txt` and GBATEK timings.
- S0 streams about 152 KB per frame of conditions for MKDS, which is worse.
- melonDS has no contention model, and `DSi.cpp:2905-2917` notes that ARM7 priority slows the ARM9 "a lot".
- This is the Kirby +21% debt moved from the cache to the bus.

**Fix**
- Decide by hardware experiment E3 before any ARM7-eval build reaches a game.
- If it fails, either do main-RAM work only while the ARM9 waits (the overlap is only about 290k ARM7 cycles for MKDS, so SPLIT fits only f ≤ ~0.45), or put the data in ARM7-reachable NWRAM. Whether NWRAM is a separate bus path is unverified.

### B6 (S0 only). The capture core is built for one CPU, and the read path still has live reads
- **Shared mutable state:**
  - the producer does `prev->repeats++` on a published slot (`dscheevos.c:2683, 2708`);
  - `arena_alloc` reads consumer state (`:2433-2445`);
  - the evaluator rebuilds and frees tables on the ARM9 heap (`:600-645, 3010`).
- **Live reads:** `read_memory` falls back to live reads on a table overflow, a chain miss, Remember/Recall or a `compile_chains` failure (`:1740, 2180-2194`). On the ARM7 these are incoherent and from the wrong frame.

**Fix**
- Use a strict single-producer/single-consumer ring.
  - Published records are immutable; repeats and carries become records of their own.
  - Records hold offsets, not pointers.
  - All metadata is ARM9-owned, and the ARM7 never calls malloc or free.
- The ARM9 proves read closure at load or the set stays on the ARM9. On the ARM7 a miss is a protocol fault and follows the gap spec.

## 2. Serious

- **S1. Header and work-list coherency (the remainder of coherency-B1).**
  - **Before `frame_begin`:** `begin_claimed_triggers` reads trigger headers through the ARM9 cache, so they must be fresh. It then writes `claimed_work`, which the ARM7 reads, so that must be cleaned.
  - **In `result()`:** it reads `trigger->state`/`measured_value` again (`rc_client.c:6172-6180`).
  - **Fix:**
    - At `frame_end`, invalidate without reading the header lines of the triggers evaluated this frame (N MCRs, no fills).
    - Clean and drain `claimed_work` and the old arrays before signalling.
    - Optional patch improvement: `result()` returns a record `{new_state, state, measured_value}` so the ARM9 doesn't refill N header lines per frame. At 100 claimed triggers that is about 10k ARM9 cycles plus 100 of 128 D-cache lines (estimate).
  - Allocate triggers 32-byte aligned with 32-byte-rounded sizes. Refuse a NULL `alloc()`; otherwise the fallback to `rc_buffer_reserve` shares lines with ARM9-written data. Add a `free` hook.
- **S2. Which clean to use at `frame_begin`.**
  - Every resident memref line is dirty (`rc_memref.c:717-726`).
  - Memref storage is larger than the design assumed: HG/SS ~286 KB, Platinum ~114 KB, Kirby ~2.5 KB (`memplan/rc.md:46-47`). The design said "~30 KB".
  - A set/way clean-all also writes back game lines, which is game-visible for incoherent games (idle-coverage (a)/(b)). For small-footprint sets that is a new exposure.
  - **Fix order:**
    1. Tag-filtered set/way clean: debug index `MCR p15,3,c15,c0,0` and tag read `MRC p15,3,c15,c2,0` (TRM `trm.txt:2255-2330`, unverified on the DSi). Clean only entries in module ranges and restore the index.
    2. Clean by MVA over `rc_client_enum_memref_storage()` for small storage.
    3. Clean-all only for huge sets, where the memref pass has already evicted game lines.
- **S3. melonDS and the host harness cannot see coherency bugs.**
  - The melonDS cache model is timing-only and memory stays coherent (`CP15.cpp:450-455, 501-546`).
  - So the claim in `arm7-notes.md:318-319, 369-371` that the melonDS run "catches glue/coherency bugs" does not hold.
  - **Fix:** a coherency sanitizer in the runner (E1).
- **S4. In S0, the capture costs the ARM9 more than the evaluation it removes.**
  - Kirby: capture ~191k vs evaluation ~177k, re-copied 20-37 times per frame.
  - Writing straight into NCNB per wake adds about +43k (estimate).
  - **Fix:** keep a cached per-wake stage and publish once per frame (burst copy into NCNB, or clean-by-MVA plus drain). idle-coverage must also remove the per-wake re-copy.
- **S5. ARM7 capacity and ring footprint (S0).**
  - Only Kirby-class sets fit: 164.5k with hot code in ARM7 WRAM, 466k with code in main RAM, MKDS 650k, against 465-515k free.
  - 12-byte `target_t` makes rings for chain-heavy sets take megabytes (Platinum ~8.5 MB per 1000 slots).
  - Droughts longer than the ring drop frames.
  - **Fix:**
    - Launch-time gate: cost at the real placement ≤ 0.7×p10, worst drought ≤ ring depth, and catch-up shorter than the gap between droughts.
    - Compact slots (target bytes plus a fail bitmap).
    - Overflow handled as a gap under the dropped-frames spec.
- **S6. Data flowing ARM7→ARM9 (S0).**
  - Words the ARM9 reads that the ARM7 writes: tail, lists, journal, notices, stats.
  - They must be read through NCNB or invalidate-then-load, on module-only lines (see B3).
- **S7. Busy-loop and no-halt games are hidden by carry.**
  - Carry slots count as "no gap" indefinitely.
  - GSDD has no module VBlank call (`misc.c:115-128`, `#ifndef GSDD`).
  - **Fix:** cap carry age, then treat it as a gap; use an in-IRQ ARM9 copy (option A) as the producer for no-halt games; give GSDD a direct VBlank hook or refuse it.
- **S8. The ARM9's poll competes for the bus.**
  - Use backoff (one NCNB read per ≥1k cycles from ITCM) or a one-frame pipeline.
  - Do not add new IPCSYNC IRQs (game-visible).
- **S9. Stack (minor, but required).** The ARM7 evaluator must switch to its own stack. Otherwise deep rcheevos frames below the game's ARM7 SP write game RAM.

## 3. What is sound

- **The patch's split point.**
  - All memory reads happen in `rc_client_update_memref_values`.
  - `rc_client_evaluate_claimed_trigger` asserts `!has_memrefs` and passes a NULL reader (a93f5a7 `rc_client.c` around :5960).
  - Old values are snapshotted before `frame_begin`.
  - Evaluation touches no mutable globals.
  - Results are identical by construction, and the difftest is the right gate.
- **Memref storage has a single writer** (the ARM9 memref pass). The ARM7 only reads it.
- **Ordering** works once data is coherently placed:
  - the ARM7 has no cache, and its writes land in order;
  - the ARM9 sequence clean → drain (`c7,c10,4`) → publish is correct (TRM §6.5).
- **The single-producer/single-consumer ring** is sound with offsets, immutable published records and head/tail on their own lines.
- **A frame-done ARM9 copy through the D-cache into a never-read or NCNB buffer** is invisible to the game, with no clean of game lines. This is rnd's data-gather baseline.
- **The set load stays on the ARM9.** A load costs 26M-504M ARM7 cycles.
- **Only S0 removes all set-streaming D-cache pollution.** SPLIT still streams the memref storage on the ARM9 (Platinum ~114 KB).

## 4. Ordered experiment plan (pass/fail fixed in advance)

| # | Where | Test | PASS | FAIL → |
|---|---|---|---|---|
| X1 | Host difftest | Glue as patched, sequential + TSan + dynamic withdraw (random j per frame; withdraw-all runs 1-600 frames), MKDS/Kirby/Platinum, 20 random partitions | Byte-identical to stock, 0 live reads | Glue or take-back not equivalent |
| X2 | NWRAM-lane build, SPLIT on | Pointer-closure walk of every claimed trigger + `claimed_work` | 0 targets outside `0x02000000-0x02FFFFFF` | B1 routing missing (predicted FAIL today) |
| X3 | Runner MPU print, all 64 ROMs | C/B bits at `0x0C000000`, `0x027FF000`, `0x02FFF000` after nds-bootstrap patches and at 1,000 gameplay frames | NCNB in all | Use the B3 MVA fallback per game (predicted FAIL for SDK5) |
| X4 | melonDS + coherency sanitizer, cache model, `--thumb-cycle` | Flag: ARM7 read of an ARM9-dirty byte; ARM9 hit on a line another master wrote after the fill; dirty half-line write-back over another master's bytes. SPLIT and S0 builds, Kirby/MKDS/Platinum, 10k frames | 0 flags in module ranges; tag-filtered clean touches 0 game lines | Glue wrong; no console run |
| X5 | melonDS + DSi XL | Per ARM7 hook entry: IRQs taken during the pass, `swi 6` after an IRQ, post-IRQ resume delay. Stock vs ARM7-eval, 10 games | 0 halts after a taken IRQ; resume delay p100 ≤ S (proposed 2k ARM7 cycles); lag = stock | B2 unfixed; stop |
| X6 | DSi XL lab ROM | ARM9 timed stride-32 miss loop (64 KB) while the ARM7 is halted / reads main RAM at u = 0-60% / runs real evaluation from main RAM / from NWRAM; EXMEMCNT bit 15 = 0 and 1 | ARM9 ≤ 0.1% slower in the shipping configuration | > 0.5%: gate to ARM9 wait or move to NWRAM; S0 likely dead |
| X7 | DSi XL lab ROM | (a) CP15 ID; (b) round-robin counter global vs per set; (c) cycle cost of MVA clean, set/way clean with 0/64/128 dirty lines, drain, tag-filtered loop; (d) ARM7 sees stale bytes without clean and fresh after clean+drain; `0x0C` alias cached under the SDK5 layout; (e) ARM7 read of `0x03700000` gives zeros when the ARM9 owns B/C | Tag-filtered loop ≤ 1.5k cycles → adopt it; (d) and (e) confirm B3 and B1 | Fall back to MVA clean |
| X8 | melonDS gamecost, Kirby/MKDS | Game ARM9 cycles/frame: hand-over stubbed vs header-refill design vs fixed (record + `frame_end` invalidate + filtered clean); S0 stage+publish vs per-wake NCNB | Fixed ≤ +0.2% and ≤ 2 extra game refills/frame; S0 ≤ +1% and module ≤ 10k/frame | Redesign the result path |
| X9 | melonDS census, 8 games, whole runs | ARM7 worst consecutive drought, simulated backlog per set, max ring depth, catch-up time | No overflow at the planned ring size | Gate refuses the set |
| X10 | DSi XL lab | PCM playback + capture loopback with both CPUs saturating the bus; card-read p99 in a streaming cache-mode game | Bit-identical capture; p99 ≤ +1% | Gate main-RAM work |

Order rationale:
- X1-X3 are free or cheap and decide B1/B3 and equivalence.
- X4-X5 come before any console run.
- X6 decides whether any ARM7 main-RAM design survives.
- X7-X10 size the result.

## 5. Interfaces rnd states (data-gather scope only)

These are the items still open in the hand-over. Arm7eval and idle-coverage pick from them.
- **Buffer.** Main RAM `0x02xxxxxx`, module-only, 32-byte-aligned lines.
  - Header holds offsets, never ARM9 pointers.
  - `head` (written by the ARM9) and `tail`/`done` (written by the ARM7) each sit on their own 32-byte line.
- **ARM9 publish.** Copy at frame-done through the cache into the stage. Then an NCNB burst, or clean-by-MVA plus drain (`c7,c10,4`). Write `head` last.
- **ARM9 read of ARM7 words.** NCNB if the per-game MPU check passes, else invalidate `c7,c6,1` then load. Poll with backoff only.
- **ARM7.** No live reads, no ARM9 heap calls, its own stack, sliced work with the IRQ-count early exit (B2).
- **NDMA.** Not used for the hand-over: no gather, cannot follow chains, only NDMA2 is possibly free (rnd round 1). It stays a candidate only for a linear stage→ring burst after X6.

## 6. Evidence checklist for red-teaming any ARM7 concept

1. **Where does every byte the ARM7 reads live?** It must be main RAM or ARM7-mastered NWRAM. NWRAM-B/C is ARM9-only.
2. **Which CPU writes each shared line?** Is it 32-byte aligned and module-only? Is there a clean+drain (ARM9 to ARM7) or invalidate (ARM7 to ARM9) at each hand-off?
3. **Is the alias NCNB in this game's actual MPU layout** (SDK5 region 3)?
4. **Any live or fallback read on the ARM7?**
5. **Does the ARM7 hook ever `swi 6` after servicing an IRQ?** What is the worst slice latency? Is card service delayed?
6. **What are the ARM7 main-RAM accesses per frame, and the measured ARM9 slowdown (X6)?**
7. **Drought behaviour:** does the ARM9 wait, drop or carry? Is there a ring-depth bound? Does reset, unload or deserialize race the ARM7?
8. **Does a test exist that can actually see the bug?** melonDS and host are coherent; they need the sanitizer.

## 7. Summary

- The patch's evaluation split is sound. The old-state blocker is refuted because a93f5a7 snapshots old values before `frame_begin`.
- Four checked blockers stand: ARM7-read data placed in ARM9-only NWRAM-B/C; the halt hook losing game wake-ups; no NCNB word on SDK5 titles; and an unbounded ARM9 wait with lifecycle races. Bus contention (X6) is unmeasured and could kill both modes.
- Next steps: X1-X3 (cheap, decisive), then the melonDS coherency sanitizer and the halt-hook check (X4/X5), then the DSi bus-contention lab (X6) before any ARM7-eval build reaches a game.
