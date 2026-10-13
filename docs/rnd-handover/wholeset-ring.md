# Red-team of the WHOLE-SET (S0) ARM9→ARM7 hand-over (rnd)

Nothing here was run on hardware or in melonDS. The facts come from code, GBATEK and the ARM946E-S TRM, cited inline. Numbers marked "estimate" are my own arithmetic.

## 1. Findings, ranked

### BLOCKERS

**B1. "Snapshot layout unchanged" carries a core that is built for one CPU. Both sides write the same structures.**
- **Evidence.** The slot ring, arena and tables are plain `S` state with pointers:
  - `slot_t` has `data`, `targets` and `table` (dscheevos.c:185-196).
  - The producer increments `prev->repeats++` on a slot that is already published (commit, dscheevos.c:2683, 2708). It does this only if `prev->state == SLOT_READY`.
  - The consumer sets `state = SLOT_EVAL` and computes `n = 1 + next->repeats` once (dscheevos.c:2970-2978).
  - `arena_alloc` reads the consumer's `S.has_last`, `S.last_offset` and `slot_tail` to find free space (dscheevos.c:2433-2445).
  - Tables are rebuilt and freed by the evaluator: `publish_pending_addresses()` runs from `dsc_idle` (dscheevos.c:3010, 600-645). They are malloc'd and freed on one module heap that has no cross-CPU lock.
  - `read_memory` on the evaluator dereferences ARM9 heap objects: `t->seq`, `t->where`, `t->addresses`, `S.chains.target_slot` (dscheevos.c:2096-2160).
- **Failure scenarios.**
  - (a) The ARM9 reads `prev->state == READY`. The ARM7 then sets EVAL and reads `repeats`. The ARM9 then does `repeats++`. That frame is never evaluated: a silent dropped frame, which in hardcore can produce a false delta edge (docs/dropped-frames.md).
  - (b) The ARM9 reads a stale cached `last_offset` and allocates over the bytes of the slot the ARM7 is evaluating. That frame is evaluated with mixed memory.
  - (c) The ARM7 frees a table on the ARM9 heap while the ARM9 malloc runs. The heap is corrupted.
- **Fix.** Use a real single-producer/single-consumer protocol:
  - The producer only ever writes slots below head. A published slot is never modified.
  - Repeats and carries become their own tiny records, e.g. `{frame, kind=REPEAT_PREV, count}`, and each record is final once published.
  - Records hold offsets, not pointers.
  - All capture metadata (tables, chain program, arena) is ARM9-owned and ARM9-allocated.
  - The ARM7 gets an immutable per-generation read map. A generation is retired when the ARM9 sees `tail` pass its last slot.
  - The ARM7 never calls malloc or free on an ARM9 heap.

**B2. Live reads exist by design. On the ARM7 they are incoherent and from the wrong frame.**
- **Evidence.** `read_memory` falls back to `S.platform.read_memory` (a live read) in four cases:
  - The table is full (`SEQ_LIVE`, max_addresses = 4096; module.c:457).
  - A chain read is not in the table or the targets: live read plus `note_missing` (dscheevos.c:2180-2194).
  - Remember/Recall chains are read live (header, dscheevos.c:11-13).
  - `compile_chains` failed, so "chains then fall back to live reads" (dscheevos.c:1740-1741).
- Item 8 of the interface ("NO live reads on the ARM7") is therefore a wish, not a property of the code.
- **Failure scenario.** A Platinum-class set has a dirty watched line at VBlank in 88% of frames (arm7-notes Q5). An ARM7 live read returns stale main RAM from a later frame. A trigger fires or misses differently than stock would, and nothing records it as a gap. `live_reads` is only a counter.
- **Fix.**
  - At load, the ARM9 proves closure: the table holds every direct byte, `compile_chains` succeeded, there is no Remember/Recall chain, and the set is under max_addresses. If closure fails, the set stays in ARM9 mode.
  - The ARM7's `read_memory` has no live path. A miss is a protocol fault, handled as a gap under the dropped-frames spec (reset, plus hardcore→casual).
  - The difftest harness already fails on a read outside the recording; add an "ARM7 build has no platform read" assertion.

**B3. The parsed set has to be readable by the ARM7, but the NWRAM lane puts it where the ARM7 cannot read it.**
- **Evidence.**
  - The NWRAM lane gives all of B/C to the ARM9 and puts the parsed set in zone 0 (dscheevos-nwram dscheevos_hooks.c:222-262; main.arm9.c:375-381).
  - Each NWRAM slot has one master, and MBK is locked before the game starts (GBATEK DSi NWRAM; main.arm9.c:436, main.arm7.c:2692).
  - The set load stays on the ARM9 (arm7-notes: 26M-504M ARM7 cycles), so rc_client is built in ARM9 write-back memory and then handed over.
  - Today the ARM9 module reads rc_client for the achievement list and menu (`list_buf`, built in idle).
- **Failure scenarios.**
  - The ARM7 reads zeros from ARM9-owned NWRAM slots ("seem to read as zerofilled", GBATEK).
  - Or, in main RAM, the ARM7 reads stale bytes from lines the ARM9 still holds dirty after the load.
  - Or the ARM9 menu reads rc_client fields through stale cached lines while the ARM7 updates them.
- **Fix.**
  - In whole-set mode, the rc_client heap is in main RAM or in ARM7-mastered NWRAM split off at ARM9_DSCHEEVOS_INIT, before the lock. This is an owner decision against the NWRAM lane.
  - After the load, one clean by MVA over the rc_client heap, then a drain.
  - From then on the ARM9 never calls an rc_client API. Lists, journal and notices come to it as ARM7-published records (see S2).

### SERIOUS

**S1. In whole-set mode the capture costs the ARM9 more than the evaluation it removes. A naive uncached destination makes this worse.**
- **Evidence.** Kirby's capture is ~191k cycles/frame, from re-copying at each of 20-37 wakes (baseline.md:24). Kirby's ARM9 evaluation is ~177k (cpi-lab O2). `dsc_capture` restarts the copy every wake ("replaces a capture taken earlier in this frame", dscheevos.c:2852-2854). An NCNB store stalls until it completes (TRM 6.5), ~18-20 ARM9 cycles per word.
- **Failure scenario.** Kirby in S0 with the capture writing straight into an uncached slot:
  - 58 words × ~20 cycles × 37 wakes ≈ +43k on top of today's capture (estimate).
  - The ARM9 total goes from ~368k to ~234k or more. Less than half of the expected gain arrives.
  - The capture's 20-37 copies per frame remain the dominant ARM9 cost.
- **Fix.**
  - Keep the per-wake copy in an ARM9-private cached stage, as today.
  - Publish once per frame, at commit/drain: either a burst copy from the stage into the NCNB slot (Kirby ~58 words, ≈0.3-1.2k cycles, estimate), or a clean-by-MVA of the stage lines plus a drain.
  - Separately, idle-coverage must remove the per-wake re-copy, for example with option A (in-IRQ copy once per frame: Kirby ~2-7k). Until then S0 is not a clear ARM9 win for Kirby.
- **What is real in S0.** It is the only mode that removes the set-streaming D-cache pollution (Kirby's +21%) entirely. The capture's own pollution is at most min(lines, 128) lines.

**S2. Data flowing ARM7→ARM9 is not covered. Item 7 assumes only ARM9→ARM7.**
- **Evidence.** Today the evaluator side writes `slot_tail`, `has_last`/`last_offset`, `list_published`/`list_buf`, journal entries, notices and stats (dscheevos.c:237-275, 2062). In S0 all of these are written by the ARM7. The ARM9 D-cache allocates on read (GBATEK), so a hot line the ARM9 polls can stay resident and stale.
- **Failure scenarios.**
  - The ARM9 reads `tail` through a cached alias and keeps a stale old value. The ring looks full, so frames are dropped (dscheevos.c:2670) and hardcore goes to casual.
  - The in-game menu shows a stale unlock list.
- **Fix.**
  - Every ARM7-written word the ARM9 reads (tail, list or journal heads and bodies, stats) is read only through an NCNB alias.
  - Or the ARM9 runs an invalidate-by-MVA over lines that hold only our data. That is allowed because the lines never hold game data. Give each such line its own 32 B.

**S3. The "uncached mirror" is not uncached in every game, and the core reads published slots on the ARM9.**
- **Evidence.**
  - In pure-SDK5 NTR crt0 (Golden Sun DD, Ghost Trick, Kirby Mass Attack, Sonic Colors and others), region 3 is cacheable write-back: C=0x4A, B=0x0A (mpuscan.txt).
  - memplan ndsb.md:29 assumes 0x0C is uncached. Not verified for these games.
  - `copy_snapshot(t, slot->data, &m, prev->data)` reads the previous slot for the repeat check (dscheevos.c:2883).
- **Failure scenarios.**
  - (a) On an SDK5 game, writes through 0x0C allocate or hit cache lines. The ARM7 then reads stale main RAM.
  - (b) A write-back "never-read" slot is read by the repeat check. Its lines are allocated. When the ring wraps, the next store hits and stays dirty, and the ARM7 reads old bytes.
- **Fix.**
  - At module init, read back the live MPU regions and C/B bits for the slot address (extend the runner MPU print to 0x0C000000 and 0x027FF000). If the slot address is not NCNB, fall back to clean+drain.
  - Keep a private ARM9 copy of the last capture for the repeat check. The ARM9 never reads a published slot.

**S4. Busy-loop games are not served, and carry hides it as "evaluated".**
- **Evidence.**
  - With no halt there is no `dsc_capture`, so `commit(0)` makes carry slots of `S.last` indefinitely. These count as `frames_stale`, which is "no gap" (dropped-frames.md table).
  - GSDD has no module VBlank call at all: IPC hook `#ifndef GSDD`, misc.c:115-128.
- **Failure scenario.** Code Lyoko halts on a loading screen and then busy-loops in gameplay. Every gameplay frame is evaluated with the loading screen's memory. Achievements never fire, or fire on stale state, with no gap event. S0 inherits this unchanged.
- **Fix.**
  - Cap the carry age (for example 2 consecutive carry frames, or a measured lag-frame bound). Past the cap, treat it as a gap.
  - For no-halt games, the producer is the in-IRQ ARM9 copy (option A), publishing into the same ring.
  - GSDD needs a direct VBlank hook (irqTable+0) or it is refused.

**S5. The ARM7 alone carries only Kirby-class sets, and even Kirby only with fast code memory DS mode doesn't give.**
- **Evidence.**
  - Lab: Kirby 164.5k with 47 KB of hot code in ARM7 WRAM, 466k with code in main RAM. MKDS 650k, Black 933k, Digimon 3.0M (arm7-notes:83-111).
  - p10 free time is 465-515k.
  - ARM7 WRAM belongs to the game. NWRAM-A has ~112 KB free only without the VRAM Wi-Fi binary (ndsb.md:60,177).
- **Failure scenarios.**
  - Kirby with code in main RAM uses 466/484k = 96%. After a 600-frame ARM7 drought, catch-up runs at 18k/466k ≈ 0.04 frames per frame, so it takes ~15,000 frames. A second drought arrives first and the ring overflows.
  - MKDS (650k against 465k) is never stable: a 1000-slot ring fills in about 1000/(1-465/650) ≈ 3,500 frames (~1 min) of gameplay.
- **Fix.** Gate S0 at launch on all three:
  - (cost at the real placement) ≤ 0.7 × p10;
  - the measured worst drought ≤ ring depth;
  - catch-up time < the interval between droughts.
  - Today only Kirby-class sets can pass, and only if the 47 KB of hot code gets NWRAM-A (E3 below).

**S6. Ring footprint is wrong by about an order of magnitude for chain sets, and "never dropped" is not achievable with a fixed ring.**
- **Evidence.**
  - A slot is `words*4 + target_count*12` (dscheevos.c:2858). Each indirect read stores a 12-byte `target_t` (dscheevos.c:130-134). rnd-r4 estimates ~7.7 KB/frame of targets for Platinum and ~25 KB for SoulSilver (rnd-r4-deferred.md:69-71).
  - 1000 slots: Platinum ~8.5 MB, SoulSilver ~25-28 MB. That is more than the 16 MB of RAM, against ~3 MB of memory available at zero ROM-cache cost (ndsb.md:180) and today's 256 KB arena (module.c:459).
  - Kirby: 54 words, 0 AddAddress → ~232 B/slot → 232 KB. MKDS: 224 B plus up to ~73 targets → 0.24-1.1 KB/slot → 0.24-1.1 MB (target count unmeasured).
  - Measured droughts: MKDS boot 603 frames. ARM9 zero-idle runs: Layton 1,455, MvDK 2,260. ARM7 drought lengths for other games are not measured.
- **Failure scenario.** Any drought longer than the ring, or any set whose mean cost exceeds the free time, drops frames. The interface's "frames must never be dropped" is then broken silently, or handled through the gap spec.
- **Fix.**
  - Size the ring from the measured maximum ARM7 drought per game.
  - Store targets compactly: per generation, the read order and sizes are fixed, so a slot needs only the target bytes plus a failed-read bitmap, ≈1-4 B per read instead of 12.
  - State the overflow policy explicitly as the dropped-frames spec (reset, plus hardcore→casual), and have the launcher gate refuse S0 when a gap is likely.

**S7. Bus contention replaces the cache-pollution debt. It is unmeasured and melonDS cannot show it.**
- **Evidence.**
  - S0 streams the parsed set from main RAM on the ARM7: MKDS ~152 KB of conditions per frame (48 B each, rc_runtime_types.h).
  - DSi.cpp:2905-2917 says ARM7 priority slows the ARM9 "a lot". Behaviour under the default ARM9 priority is unknown. melonDS has no contention model.
- **Failure scenario.** The game's ARM9 cache-line fills wait behind ARM7 main-RAM accesses, so the game's ARM9 cycles per frame go up. That breaks the hard rule invisibly in every emulator test.
- **Fix.** Hardware experiment E5 before S0 is adopted. If it fails, the ARM7 data must be in NWRAM (another bus, unverified) or S0 is dead.

### MINOR

- **M1. The two-consumer variant (item 5) is obsolete.** S1 with two rc_clients is ruled out by ra-compliance, and in split mode the ARM7 does not read slots. Delete it so the protocol stays single-producer/single-consumer.
- **M2. NWRAM "preferred" (item 4) cannot be done after the lock.** The cross-CPU ring must be in main RAM. The ARM7 may copy a slot into NWRAM-A itself: 0.2-1 KB at S32 = 2 cycles, ~0.2-2k cycles (estimate).
- **M3. Header fields must be offsets, not ARM9 pointers.** It is unverified whether the ARM7 can address the 0x0C mirror; the ARM7 should use 0x02xxxxxx plus an offset. Put `head` and `tail` each on their own 32-B NCNB line (no false sharing with `S`).
- **M4. Repeat compression changes once published slots are immutable.** "Repeat of prev unless prev is being evaluated" becomes "append a repeat record". That costs ~16 B per load or menu frame: 1000 × 16 B = 16 KB, which is fine.

## 2. What is sound

- **The single-producer/single-consumer ring.** Head is written only by the ARM9 and tail only by the ARM7. The sequence is: slot written, then drain (c7,c10,4), then head written. This is correct for ARM946E-S plus ARM7TDMI: the ARM7 has no cache or write buffer, and the ARM9 write buffer is a FIFO that the drain empties (TRM 6.5, 2.3). This holds provided S2 (the reverse direction) and M3 (offsets, own lines) are applied.
- **A frame-done ARM9 copy through the D-cache into an NCNB or never-read buffer.** It sees exactly what the game's ARM9 sees, and it changes nothing the game can observe. There is no clean of game lines.
- **A fixed, ordered read list per table generation.** All reads happen in `rc_client_update_memref_values` (rnd logs; ra-compliance review), so the slot-plus-map design is complete once B2 closure holds.
- **Set load stays on the ARM9.** This is necessary given 26M-504M ARM7 cycles.
- **Compliance.** S0 raises no RA concern (ra-compliance on C7). The difftest harness, which fails on any read outside the recording, is the right gate.
- **S0 removes all evaluation pollution from the game's D-cache.** It is the only mode with a path to taking Kirby's +21% to ~0, once S1 is fixed.

## 3. Cheapest decisive experiments (pass/fail fixed in advance)

| # | Where | What | PASS | FAIL |
|---|---|---|---|---|
| E1 | host | S0 protocol: producer and consumer threads on a shared ring image (TSan), the ARM7 `read_memory` with no platform read (abort on a miss), difftest S0 variant on real sets plus fuzzed frames | Byte-identical to stock on every set/scenario; 0 live reads; TSan clean | Any diff, any live-read abort on a set the gate accepted |
| E2 | melonDS `--cache-model --thumb-cycle`, tests/perf AB, Kirby + MKDS | Game ARM9 cycles/frame: stock; vs S0-capture-only (cached stage per wake, one NCNB publish per frame); vs naive per-wake NCNB | S0-capture-only: game cycles ≤ stock +1% and module ARM9 ≤ 10k/frame (with the per-wake fix) | > +2%, or naive per-wake beating stage+publish (that would mean my store-cost model is wrong) |
| E3 | melonDS arm7-lab | Kirby and MKDS with hot code placed in the NWRAM-A range (not ARM7 WRAM) and set data in main RAM; plus nwHotData | Kirby ≤ 339k (0.7 × 484k); MKDS ≤ 325k | Kirby > 339k means S0 has no headroom even for Kirby |
| E4 | melonDS census runner, 8 games, whole runs | Per-interval ARM7 free time, then simulate the backlog with each set's measured ARM7 cost; report maximum ring depth and catch-up time | Max depth ≤ slot budget (Kirby 1000 × 232 B) and no overflow | Any overflow means a gap in S0 for that game |
| E5 | DSi XL lab ROM | ARM9: timed main-RAM miss loop (e.g. 64 KB stride-32 reads, ARM9 timer). ARM7: off / reading main RAM at 50k and 150k accesses per frame. 10 repeats each, SD log | ARM9 slowdown ≤ noise floor (≤ 0.1%) at 150k | > 0.5% means S0 data must leave main RAM or S0 is dropped |
| E6 | DSi XL lab ROM | ARM9 writes 10^6 slots (pattern + sequence) through 0x0C and through write-back never-read memory, drain, then head; ARM7 verifies; time per word. Also read the MPU at runtime in one SDK5 NTR game (runner print at 0x0C000000) | 0 mismatches; ≤ 25 ARM9 cycles per word for NCNB; region 3 NCNB (or fallback chosen) | Any mismatch, or region 3 cacheable for SDK5 without the fallback |

## 4. Questions

**For the ARM7 lane**
1. Who builds table generations in S0? I propose the ARM9 only, at load, with closure proven (B2). Do you agree that the ARM7 never calls `publish_pending_addresses`, malloc or free on an ARM9 heap?
2. Where does the S0 rc_client heap live (B3)? It needs main RAM or ARM7-mastered NWRAM split off before the lock, which conflicts with the NWRAM lane's all-to-ARM9 grant.
3. Will you accept a compact slot (target bytes plus a failed bitmap, read order fixed per generation) in place of 12-byte `target_t` records (S6)?
4. What is MKDS's `chains.target_count`, and Platinum's? This settles S6's footprint numbers.
5. How do rc_client callbacks (server_call, event handler) and the ARM9-side menu/journal reach the ARM9 in S0? They need ARM7→ARM9 records read through NCNB (S2).

**For idle-coverage**
1. How do you remove Kirby's per-wake re-copy (20-37 copies per frame)? S0's ARM9 benefit depends on it (S1).
2. What is the maximum consecutive ARM7 drought per game, not the window mean (E4)?
3. Is the ARM7 swiHalt hook available in MKDS and Gen 4/5 Pokemon? memplan ndsb.md:84 says `altReadMethod` disables it. I could not find `altReadMethod` in this tree (unverified).
4. For no-halt games: an in-IRQ ARM9 copy into the same ring, plus a carry-age cap (S4). Is that acceptable from your side, and what cap?
5. Which games have the VRAM Wi-Fi binary? That decides whether NWRAM-A is free for the ARM7's hot code.