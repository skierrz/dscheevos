## ARM9→ARM7 hand-over in SPLIT mode: red-team findings

The target is the SPLIT-mode hand-over as designed: rcheevos patch a93f5a7 in `dscheevos-arm7eval/external/rcheevos`, arm7-notes.md lines 200-320 for the host protocol, and idle-coverage decision.md §2.7 (P2) for the ARM7 entry. No SPLIT glue exists yet: `tests/arm7-lab` has no handshake code, only the patch and `tests/difftest` exist. So every finding below is against the design and the patch. Numbers are estimates unless marked measured.

### 1. Findings, ranked

#### BLOCKER B1: the ARM7 entry point changes when the game's ARM7 code runs (lost wake-up and delayed work)

**Evidence**
- The hook is `newSwiHalt`: `push {lr}; bl runCardEngineCheckHalt; swi #0x060000; pop {pc}` (`external/nds-bootstrap/retail/cardenginei/arm7/source/card_engine_header.s:445-450`, Thumb variant at :455-463).
- P2 puts the evaluator "after `runCardEngineCheckHalt`, before the game's `swi 6`" and "re-run `runCardEngineCheckHalt` after the pass, before `swi 6`" (idle-coverage `docs/idle-coverage/decision.md` §2.7).
- idle-coverage's own red-team S6: the site is found by a DISPSTAT constant (`find_arm7.c:329-360`), so it is likely a working main loop, not the idle thread.

**Failure scenario**
- The game's ARM7 loop calls halt and expects to resume right after the next IRQ.
- We run part of a SPLIT frame instead (MKDS share about 340k ARM7 cycles, about 60% of a frame). The VBlank IRQ (or a timer, IPC or Wi-Fi IRQ) arrives and the game's handler runs.
- Control comes back to our pass, not to the game's loop body. The loop body is delayed by the rest of the pass.
- Then the hook executes `swi 6` anyway. The wake-up IRQ has already been handled, so the loop sleeps until the next IRQ, up to one more frame.
- Touch, key and RTC sampling, sound sequencing done in that loop, and PXI replies are all delayed by up to a frame.
- The ARM9 waiting on PXI then takes lag frames. This changes game behaviour, so it breaks a hard rule.
- In a true SDK idle thread the thread switch at IRQ return masks most of this. But nothing proves the site is the idle thread (A7-M0 is unrun).

**Fix**
- The hook must emulate the halt exactly, the same idea as the ARM9 "E":
  - CE7's IRQ entry (it already wraps the game's handler, `cardengine.c:2119-2141`) bumps a counter.
  - The evaluator runs in slices of at most S cycles.
  - After each slice, if the counter moved since hook entry, return at once without `swi 6`. A halt would have woken there.
  - Only if no IRQ was taken does the hook execute `swi 6`.
- Then the added latency to the game's loop is at most one slice (S), not a pass.
- S must be chosen and measured (experiment E5). A slice ends between triggers, so S is at least the cost of the largest claimed trigger. `claim()` must refuse triggers whose worst-case ARM7 cost is above S.
- Gate P2 on A7-M0 as decided, and add this rule regardless of the A7-M0 result.

#### BLOCKER B2: ARM7 main-RAM traffic during the game's working time raises the game's own ARM9 cycles per frame

**Evidence**
- SPLIT v1 puts claimed triggers in main RAM ("v1: claimed triggers in main RAM", arm7-notes.md:305-306). Memrefs are in main RAM too, read by the ARM7 (arm7-notes.md:201-206).
- Nothing in the SPLIT protocol limits ARM7 work to the time the game's ARM9 is waiting. The ARM7 evaluates whenever its own hook runs, and arm7-notes.md:231 says the ARM7 polls in its idle time.
- melonDS models no main-RAM contention. Its DSi.cpp:2905-2917 comment says that on hardware the ARM7 "slows down the ARM9 a lot" when the two compete (that case had ARM7 priority).
- EXMEMCNT bit 15 is the game's setting (GBATEK "DS Memory Control - Cartridges and Main RAM", gbatek.txt:7408). Its value per game is unlogged.

**Quantified for MKDS** (estimate, my arithmetic)
- ARM7 data accesses for the full set: 3,168 conditions × 10-14 main-RAM accesses = 32-44k per frame (the ARM7 lane's own model).
- SPLIT share f = 0.52: 16.5-23k accesses. At about 10 bus cycles per N32 (GBATEK DS Memory Timings), that is 165-230k bus cycles, **29-41% of the 560k-cycle main-RAM bus per frame**.
- The game's own stock ARM9 traffic (measured, melonDS cache model, `evalspeed-team/gamecost/out/mkds-gc-base/ab.txt`, achievements off): I misses 2,248, D read misses 2,284, D write misses 2,352 and write-backs 467 per interval. That is about 4.5k line fills, about 104k bus cycles.
- With ARM9 priority and an arbiter that cannot pre-empt an access already in flight, each fill waits about u × L/2:
  - expected: 0.35 × 5 = about 1.75 bus cycles, about 3.5 ARM9 cycles per fill. That is about +16k ARM9 cycles per frame, **+3.6% of the game's 447k busy cycles**;
  - worst case (a full 10-cycle wait per fill): about +91k, **+20%**.
- If the game sets ARM7 priority, it is worse, per the melonDS comment.
- This is the same kind of debt as Kirby's +21%, just moved from cache pollution to the bus.

**Fix**
- Option (i): the ARM7 does main-RAM-heavy SPLIT work only while the game's ARM9 is waiting. idle-coverage's `wait.kind`/`wait.state` record is the signal; it must be readable by the ARM7 (it is in the header's IRQ cache line, so it needs an uncached or cleaned home, see S2).
  - Cost: the usable ARM7 window becomes the overlap of the ARM9 wait and ARM7 idle time.
  - For MKDS: the ARM9 waits about 60% of a frame (measured 39.9% busy), which is about 337k ARM7 cycles. At 87% ARM7 idle (median), about 290k is usable.
  - The SPLIT share at f = 0.52 is 338k, which **does not fit**. f = 0.74 needs 481k. A gated SPLIT fits only f ≤ about 0.45, or about 0.31 with 70% catch-up headroom.
  - This needs restating in the arm7-notes fit table.
- Option (ii): claimed triggers and a copy of memref values in ARM7-local NWRAM. This takes the traffic off the shared bus if NWRAM is a separate path, which is unverified (idle-coverage red-team S5).
- Either way, A7-M4 (in idle-coverage's decision.md) and experiment E3 below must pass before any SPLIT build reaches a game.

#### BLOCKER B3: the frame_end barrier ties the ARM9's progress to the ARM7's schedule, with no fallback

**Evidence**
- The patch calls `frame_end` after the achievement loop (rc_client.c patch, `rc_client_do_frame`, around :6579). ra-compliance R2 makes the host wait there until the ARM7 has finished frame f (arm7-notes.md:449).
- The ARM7 evaluates inside the game's ARM7 thread context. Game ARM7 threads or IRQ work can pre-empt it for as long as the game likes.
- Whole-run worst windows: HG/SS 44%, Platinum 51% (arm7-notes.md:47-58).
- MKDS's 603 zero-halt intervals coincide with nds-bootstrap's boot stub, ARM9 PC in 0x023BFxxx for frames 1-597 (gamecost-notes.md). That is before the game, so it is not a real drought. Load droughts are real.

**Failure scenarios**
- (a) A load drought on HG/SS: the ARM9 cannot run frame f+1's memref update, so the snapshot queue grows. Past the 1024-frame queue, frames are dropped. That breaks hardcore every-frame evaluation.
- (b) The ARM7 accepted frame f and is in the middle of a trigger when a game thread pre-empts it for hundreds of frames. The ARM9 can neither finish frame f nor take it back, because a half-evaluated trigger has already mutated hit counts.
- (c) Quit, soft reset or the in-game menu during the window: lifecycle writes and achievement lists are forbidden between frame_begin and frame_end (ra-compliance R4/R5, rc_client.h patch comment). If the game's ARM7 is reset or parked, the quit or menu path hangs.

**Fix**
- Per-frame and per-trigger ownership with take-back. The patch already allows the ARM9 to call `rc_client_evaluate_claimed_trigger` itself inside `result()`, and the results are identical by construction.
- The protocol uses single-writer words in NCNB memory (see S2 for where):
  - The ARM7 writes `taken = k` before starting trigger k, then re-reads `withdraw`. If `withdraw ≤ k`, it backs off before writing anything.
  - The ARM9 writes `withdraw = j` (an NCNB store, which stalls until complete per ARM946E-S TRM §6.5), then reads `taken`. It evaluates every trigger at index ≥ j that the ARM7 has not taken.
  - This is Dekker ordering. It needs the ARM7 to have no write buffer (unverified; DS ARM7 docs are silent) and NCNB stores on the ARM9.
- To bound waits for an in-flight trigger: per-trigger evaluation on the ARM7 runs with IRQs masked, and `claim()` admits only triggers whose measured worst-case ARM7 cost is at most an IRQ-latency budget proven by E5. Otherwise a pre-empted in-flight trigger can still stall the ARM9 for a whole drought.
- difftest must add dynamic partitions: a random withdraw point every frame. Today it covers only static partitions (arm7-notes.md:326-327).

#### SERIOUS S1: melonDS cannot see any of the coherency bugs this protocol can have, and the plan relies on it

**Evidence**
- melonDS's cache model is timing-only and memory stays coherent (`tools/melonDS/src/CP15.cpp:450-455, 501-546`; rnd D-cache evidence).
- Yet arm7-notes.md:369-371 says the melonDS end-to-end run "catches glue/coherency bugs the host model cannot".
- The difftest's "TSan concurrent mode" models coherent shared memory.

**Failure scenario**
- Two bugs pass every planned test and fail on the DSi:
  - a forgotten clean of the work list `claimed_work`, which is written by the ARM9 into `game->buffer` just before frame_begin (patch, `rc_client_begin_claimed_triggers`);
  - a missing invalidate of a trigger header that straddles two lines.
- On the console these surface as stale hit counts, which means wrong unlock frames.

**Fix**
- A melonDS "coherency sanitizer" in the runner, built on idle-coverage's `--dirty-watch` cache state. It flags:
  - (1) any ARM7 read of a byte whose line is dirty in the ARM9 D-cache model;
  - (2) any ARM9 cache hit on a line written by the ARM7 or DMA since that line was filled;
  - (3) any ARM9 write-back of a half-line overwriting ARM7-written bytes.
- Pass mark: zero flags. This is cheap and decisive (E1).

#### SERIOUS S2: "uncached words" have no safe home in SDK5 NTR games

**Evidence** (rnd D-cache scan, `scratchpad/arm7h/dc/mpuscan.txt`)
- SDK5 pure-NTR titles mark region 3 (0x08000000-0x0FFFFFFF) cacheable and write-back. That includes Golden Sun DD, Ghost Trick, Kirby Mass Attack and Sonic Colors.
- So the 0x0C000000 mirror is not proven uncached for them.
- The only NCNB windows are 0x027FF000 and 0x02FFF000, which are game/SDK shared areas. We must never write game RAM.
- arm7-notes.md:293 and :310 rely on "an uncached word".

**Failure scenario**
- The result counter or handshake word sits in a write-back line on the ARM9. The ARM9 polls a stale cached copy forever, or its stores sit in the D-cache and the ARM7 never sees them.
- The result is a hang, or wrong ordering for the B3 protocol.

**Fix**
- Extend the runner's MPU print to 0x0C000000 and 0x027FF000 across all 64 ROMs (E4).
- If any game is not NCNB there, use cleaned and invalidated lines with explicit `c7,c10,1` / `c7,c6,1` / `c7,c10,4` around every handshake access. Accept that the Dekker step then costs a clean plus a drain plus an invalidate.
- Alternatively, have nds-bootstrap add a module-owned NCNB region. That changes the MPU, which needs an owner ruling.

#### SERIOUS S3: the design cleans the whole D-cache at frame_begin, which writes back game lines

**Evidence**
- arm7-notes.md:289-290: "D-cache CLEAN ... of the memref storage (clean-all by set/way ...)".
- A set/way clean writes back the game's dirty lines too.
- idle-coverage: a clean is game-visible for cache-incoherent games, cases (a), (b) and (b'). nds-bootstrap's per-card-read flush for IPK/IPG/CLJ is precedent, not proof.
- In SPLIT mode the ARM9 still evaluates, so today's evictions still happen. But the SPLIT goal is to shrink the ARM9's footprint, after which this clean becomes the remaining exposure.

**Fix**
- Clean by MVA over exactly `rc_client_enum_memref_storage()` ranges, `claimed_work` and the old_state/old_measured arrays. MKDS is about 2 KB, about 64 lines, about 0.3-1.3k cycles; SoulSilver is about 30 KB, about 940 lines, about 5-7k cycles (estimate, no TRM cycle counts).
- This touches no game line.

#### SERIOUS S4: the ARM9's poll in result() and the ARM7's wake-up latency

**Evidence**
- The ARM7 notices `frame_begin` only when its hook runs, and the game's ARM7 stays halted until its next IRQ. That can be up to one VBlank period, so `result()` waits.
- An ARM9 tight loop on an NCNB main-RAM word costs about 10 bus cycles per iteration (GBATEK N32). That takes most of the main-RAM bus. W is parked by game IRQs, but the spin still competes with the ARM7 evaluator, the game's ARM7 and the game's DMA (sound, GX) while the game's ARM9 waits.

**Failure scenario**
- The ARM7 evaluator's throughput drops by up to about 2x during the window it needs most.
- Sound or DMA reading main RAM may be starved; this is unverified (GBATEK does not document sound-fetch priority).

**Fix**
- Poll with backoff: an ITCM delay loop, one NCNB read per ≥ 1k cycles.
- Or treat SPLIT as a one-frame pipeline: publish f in wait window n, consume it in window n+1. That needs no wake-up signal.
- Do not add an ARM9→ARM7 IPCSYNC IRQ for this. `IPC_SendSync` rewrites the ARM9's IPCSYNC output nibble, which the game's ARM7 reads. nds-bootstrap already does that for card pings, but we should not add a new game-visible write.

#### SERIOUS S5: card reads and CE7 SD transfers queue behind our pass and share the bus

**Evidence**
- In hooked games, ARM9 card requests are served only in `runCardEngineCheckHalt` (idle-coverage red-team B2; `cardengine.c:1799-1840`).
- CE7's SD reads use ARM7 NDMA0 into main RAM (`cardengine.c:333-337`).
- Our ARM7 main-RAM stream competes with those NDMA writes.

**Failure scenario**
- A streaming game (cache mode, FMV or audio streaming) gets longer card latency, so its ARM9 threads stall and frames lag.

**Fix**
- B1's slices plus a `sharedAddr[3]` check between slices, which serves a pending request immediately.
- Gate SPLIT main-RAM work while `sharedAddr[3]` holds a request or CE7's NDMA0 is busy.
- Pass marks per A7-M7 (card-read p99 ≤ +1%).

#### MINOR
- **M1.** The partition cost model is built from solo-CPU measurements, the ARM7 lab with the ARM9 halted (arm7-notes.md:86-88). Under SPLIT both CPUs stream main RAM at once, so the "ARM7 finishes before the ARM9 reaches it" assumption (arm7-notes.md:310-311) is optimistic. Re-derive it from E3's numbers.
- **M2.** Trigger memory must come from `alloc()` with 32-byte-aligned start and size. If `alloc()` returns NULL, the patch falls back to `rc_buffer_reserve` (patch hunk in `rc_client_copy_achievements`), and the trigger shares lines with ARM9-written data: a write-back clobbers ARM7 hit counts. The glue must assert non-NULL and aligned, or refuse the claim.
- **M3.** Stack: the evaluator runs in the game's ARM7 thread context. It must switch to its own stack in NWRAM, because SDK ARM7 thread stacks are small and deep rcheevos frames below the game's SP write game RAM.
- **M4.** IRQ latency from a single long instruction (an LDM from main RAM under contention, about 20-40 ARM7 cycles) is negligible. The real latency terms are B1's slice and any IRQ-masked per-trigger section (B3).
- **M5.** EXMEMCNT bit 15 is unlogged per game. B2's size depends on it, so log it in every census run.
- **M6.** Power: the ARM7 running instead of halting costs a few mW, which does not matter for gameplay. Thermal effect is nil. Not worth a test.

### 2. What is sound
- The patch's split point. All reads happen in `rc_client_update_memref_values`. `rc_client_evaluate_claimed_trigger` passes NULL read_memory and asserts `!has_memrefs`. The old state and measured value are captured before frame_begin (patch, `rc_client_begin_claimed_triggers`). Results are identical by construction, and the difftest is the right gate.
- One frame in flight: frame_begin comes after the memref update, and frame_end before the next one. With a coherent transport there are no data races on memrefs.
- The ARM7 never reads game RAM in SPLIT mode, only memrefs that the ARM9 has already read through its cache. That makes the dirty-line problem (Platinum 88%) irrelevant here.
- No deadlock cycle between our code and the game: W yields to every game IRQ, and the ARM7 waits for nothing from the ARM9 except frame_begin. The hazards are latency and drops (B3), not deadlock.
- NCNB stores on the ARM9 stall until complete (ARM946E-S TRM §6.5), so single-writer counters published after a `c7,c10,4` drain are ordered correctly.

### 3. Decisive cheapest experiments (pass/fail fixed in advance)

| # | Where | What | Pass | Fail → |
|---|---|---|---|---|
| E1 | melonDS runner + coherency sanitizer (S1), cache model, `--thumb-cycle` | SPLIT build, MKDS / Platinum / SoulSilver, 10k frames each, random per-frame withdraw points | 0 sanitizer flags, and per-frame hash equals host replay | the glue is wrong; no console run |
| E2 | melonDS, ARM7 bus counter | ARM7 main-RAM accesses per frame for the production SPLIT share, MKDS and Platinum | report u = bus cycles / 560k (input to E3) | — |
| E3 | DSi XL lab ROM | ARM9 stride-32 line-fill loop (64 KB, invalidated each pass), timed with the lab's own timer, while the ARM7 is (a) halted, (b) issuing N32 main-RAM reads at swept rates u = 0-60%, (c) running real claimed-trigger evaluation from main RAM, (d) the same with data in NWRAM. Run with EXMEMCNT bit 15 = 0 and = 1. Also record the ARM7's slowdown | the ARM9 loop is ≤ 0.1% slower in the configuration we would ship | > 0.5%: ARM7 main-RAM work must be gated to the ARM9's wait (B2 option i), or data moves to NWRAM |
| E4 | runner MPU print, 64 ROMs | C/B bits at 0x0C000000, 0x027FF000, 0x02FFF000 after nds-bootstrap's patches and at 1,000 gameplay frames | NCNB in all 64 | S2 fallback (clean/invalidate handshake) |
| E5 | melonDS + DSi XL | per ARM7 hook entry: IRQs taken during the pass, whether `swi 6` was skipped, the PC at the loop head after the IRQ. Stock vs SPLIT, 10 games | 0 halts after a taken IRQ; increase in the loop's post-IRQ resume time p100 ≤ S (proposed 2k ARM7 cycles); lag = stock | B1 is unfixed; P2 stops |
| E6 | DSi XL lab | ARM7 plays a PCM channel from main RAM plus a sound-capture loopback into RAM, while ARM9 and ARM7 saturate the bus | capture buffer bit-identical to a quiet-bus run | sound-fetch starvation; SPLIT must be gated |
| E7 | DSi XL lab | ARM7 evaluator throughput with the ARM9 polling tight vs with backoff | backoff loses ≤ 1% | redesign the wait (S4) |
| E8 | host difftest | dynamic withdraw (random j per frame, and withdraw-all for runs of 1-600 frames) | byte-identical to stock | B3 fallback is not equivalent |
| E9 | melonDS A/B (A7-M7) | card-read p99 and lag in a streaming cache-mode game, stock vs SPLIT | p99 ≤ +1%, lag = stock | S5 gating |

### 4. Questions

**For the ARM7 lane**
1. Will the B3 take-back protocol be added? `result()` evaluating locally is allowed by your own patch comment. Who owns the IRQ-masked per-trigger budget in `claim()`?
2. Are the claimed triggers and the work list allocated line-aligned (M2)? Does the glue refuse a NULL `alloc()`?
3. Will you restate the fit table under gated SPLIT (usable ARM7 time = ARM9 wait ∩ ARM7 idle, about 290k for MKDS)?

**For idle-coverage**

4. Can the hook adopt "IRQ taken since entry → skip `swi 6`" (B1)? Can CE7's IRQ wrapper provide the counter?
5. Can the `wait` record be exported where the ARM7 can read it coherently, given S2, so that SPLIT main-RAM work can be gated?
6. Will `--dirty-watch` be extended into the E1 sanitizer, or should rnd build it?
7. Can EXMEMCNT be logged per game in the census (M5)?

**For the orchestrator**

8. The memref-storage clean by MVA (S3) instead of set/way: does anyone object?

Files referenced:
- `~/Developer/DScheevos/evalspeed-team/arm7-notes.md`
- `~/Developer/DScheevos/dscheevos-arm7eval/external/rcheevos` (commit a93f5a7)
- `~/Developer/DScheevos/dscheevos-rnd/external/nds-bootstrap/retail/cardenginei/arm7/source/card_engine_header.s`
- `~/Developer/DScheevos/dscheevos-rnd/external/nds-bootstrap/retail/cardenginei/arm7/source/cardengine.c`
- `~/Developer/DScheevos/evalspeed-team/gamecost/out/mkds-gc-base/ab.txt`
- idle-coverage branch `docs/idle-coverage/decision.md`
- idle-coverage branch `docs/idle-coverage/redteam-arm7.md`