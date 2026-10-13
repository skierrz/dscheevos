# Red team: prop-dsi.md (DSi-only hardware)

Read-only. Nothing run. **(spec.)** = my inference, not read in a source.
- G = `(local scratch)` (line numbers).
- NB = `dscheevos/external/nds-bootstrap/retail/`.
- MEL = `dscheevos/tools/melonDS/src/`.
- SURV = `dscheevos-display/docs/display-research/survey.md`.

## Verdict

- D1's core idea is sound in principle: H-Blank NDMA table writes, plus "the CPU captures DISPCNT while the game is provably idle, and the dispatcher aborts the strip before any game code runs". It is the best idea in the round.
- As written, it has **three blockers** (B1-B3), all cheap to fix, and several **majors** that cut the claimed coverage:
  - the GXSTAT.27 guard excludes every 3D game;
  - class C/D flicker;
  - IPC/PXI aborts;
  - NWRAM isn't universal.
- **D7** has a real data-corruption path (B4).
- **D4/D5** reintroduce the 4a clobber through a non-atomic abort and RMW.
- The ~11-cycle figure is optimistic but the right order. The ~100-400 cycles a frame is plausible only after the fixes, and lands nearer 300-1000.

---

## Blockers

### B1. D2a's check reads the wrong register, so the strip is never aborted (blocker as written, trivial fix)
**The claim:** `add r0,r12,#0x4100; ldr r1,[r0,#0x38]`, with r12 = `0x04000000`, reads channel A's busy bit.

**What is actually there:**
- **The offset is wrong.** NDMAxCNT is at `0x400411C + x*0x1C` (G:27942). With F = NDMA0 and B below A (so B = NDMA1, A = NDMA2), `0x04004100+0x38` = `0x04004138` = **NDMA1CNT = channel B**.
  - B ends at H-Blank E-1 (TCNT = E-VCOUNT), so during lines E..191 the check reads 0 and **never aborts**.
  - The game then runs with mode 3 live, and A keeps writing the stale `cap` at every H-Blank. That is R1/R2 and the 4a clobber, all at once.
  - A is at `0x04004154` (offset 0x54).
- **r12 is wrong too.** The patch target is not `0x04000000`. SDK V1 (80% of titles) does `mov ip,#0x04000000; add ip,ip,#0x210` (SURV:349), so r12 = `0x04000210`, and `r12+0x4100+0x38` = `0x04004348` (the DSP block, disabled in DS mode, see M6).
- **The patch site is ITCM.** The handler is in ITCM in all 45 titles (SURV:345), so the check can't be inlined. It needs a branch to a stub (see M9).

**Fix:**
- Materialise `0x04004154` explicitly (`mov`+`add`; the difference 0x3F44 from r12 isn't an ARM immediate).
- **Fail-safe second test:** at the module's VBlank hook, if DISPCNT mode reads 3, force mode 1 and stop F. This catches any TCNT off-by-one that leaves mode 3 with A no longer busy, which would otherwise be a full frame of garbage.

**Test:** H-KILL extended. Inject IRQs at lines E+1..191; DISPCNT must read mode 1 inside the game's handler 10^4/10^4 times.

### B2. Arming is not atomic: an IRQ during arming defeats the invariant (blocker)
"Between the CPU's `cap` read and the last table write, game code can run only after a game IRQ, which aborts first." That fails while the module is *still arming*:
- The dispatcher check sees A **not yet busy**, so it doesn't abort.
- The game's handler runs and may write DISPCNT.
- Control returns to the idle thread, which finishes arming with the stale `cap`. A then rewrites the old value at every H-Blank: a clobber.
- If the IRQ readied a thread, the idle thread is switched out mid-arm. It resumes frames later and enables A and B with a stale `SAD=&T1[VCOUNT_old]`, so mode 3 lands on the wrong lines (garbage on game lines).

**Fix:**
- Arm with CPSR.I set, from the `cap` read to the A enable.
- Re-read VCOUNT last.
- Keep a generation counter bumped by the dispatcher stub. Re-check it after enabling; if it changed, stop everything.

**Test:** a lab ROM firing timer IRQs at random cycles across the arm window; 0 wrong-line mode-3 frames in 10^5.

### B3. Abort ordering: A's last DMA write can land after the dispatcher's mode-1 restore (blocker until H-KILL proves otherwise)
**The proposal's sequence:** stop A/B/F, then read DISPCNT, then STRB if 3.

**The problem:**
- A CNT=0 is a buffered store.
- An H-Blank DRQ already latched, or a 1-word block in flight, can complete **after** the CPU's DISPCNT read/STRB.
- If that word is `cap|3<<16` (H-Blank E-1), mode 3 persists after the abort while F is stopped. The rest of the frame, **and every later frame** until the game writes DISPCNT, shows an empty FIFO on game lines.
- G only says "a physical block is never split" (G:27920-27923). It says nothing about pending DRQs.

**Fix:**
- Stop, then poll NDMA2CNT.31 == 0 (one more I/O read, ~8 cycles), then read and restore DISPCNT.
- Pair it with B1's VBlank fail-safe.

**Test:** H-KILL with the abort placed at H-Blank E-1 ± 20 cycles. Pass: 0 frames with mode 3 after the abort.

### B4. D7 restore can load stale data into the D-cache: game data corruption (blocker for D7 as written)
**The scenario:**
- The snapshot is taken at idle entry. During the module pass a game IRQ runs, for example a card or DMA-done handler. Its handler does the standard SDK `DC_InvalidateRange(buf)`, then starts a DMA into `buf` (chained reads), and returns to the idle thread.
- D7 then "touches one word of each snapshot D-line". `buf`'s lines were valid at snapshot time, so D7 **re-allocates them mid-DMA**.
- The game later reads `buf` without invalidating again. It already did, before the DMA, which is the documented pattern. It gets partial or stale data.
- This is a module-caused change to what the game reads: against "never touch the game's RAM" in effect.

**Fix:**
- Restore only if **no IRQ ran since the snapshot** (the dispatcher-stub generation counter from B2) **and** no legacy DMA, NDMA or GX DMA is busy at restore time.
- Otherwise drop the restore.

**Test:** H-CR plus a variant with an IRQ that invalidates and DMAs mid-pass. The checksum of what the game reads must match stock in 10^4/10^4.

---

## Major

### M1. The GXSTAT.27 guard excludes every 3D game (quiet exclusion)
- SWAP_BUFFERS executes "until VBlank" (G:9735). GXSTAT.27 is "busy; commands are executing" (GXSTAT, G:~9752).
- Every 3D title idles after `G3_SwapBuffers` with **bit 27 = 1**, so D1 never arms for them. That includes class E, which D6 relies on, and 3D allow-list titles.
- **Fix:** guard on FIFO empty (bit 26) and no GX DMA (legacy CNT.31), not bit 27. A pending swap uses no bus (spec.).

### M2. Guard contradictions for class E
D1 requires "POWCNT1.15 stable for 30 frames", but class E flips it every frame (brief). D6 + D1 can never arm as written.

**Fix:** replace the guard with a parity predictor (alternation regular for 30 frames, arm only on X-parity).

### M3. Class D's "game resumes only via IRQ" is assumed, not shown
- The proposal assumes poll loops exit on an IRQ-set flag or on VCOUNT=192. A loop that waits on an **ARM7-written main-RAM flag**, or on **VCOUNT == some line in E..191**, resumes game code with mode 3 live and A writing `cap`: clobber, game reads mode 3, and F contention in working time.
- V4 (Golden Sun DD) and V3 (DKR) aren't even the V1/V2 dispatchers (SURV:351-352).
- **Fix:** idle-coverage's loop hook owns the exit path, so make the **hook's exit run the abort** (it's our code, ~20 cycles). Only arm for loops whose exit condition is statically VCOUNT ≥ 192 or IRQ flag.

### M4. Coverage claims for C/D/B ignore per-frame misses, so the strip flickers or never shows
D1 needs the game idle by E-2 **on every frame**. A frame where idle starts later has no strip, and the strip can't carry into the next frame: 07h doesn't fire in VBlank, and the VBlank IRQ aborts.

| Group | Idle by line 156 (census) | Missed frames |
|---|---|---|
| Sonic Rush | 89% | 11% |
| Super Princess Peach | 71% | 29% |
| MPP | 70% | 30% |
| RE:DS | 54% | 46% |
| Raster titles | 36-67% | 33-64% |

- Under R19 ("start after 30 good frames, end early"), these titles mostly get no notice. Shown anyway, they flicker, which breaks the owner's bar.
- "Served" in section 3 is overstated.
- **Fix:** report per-title "P(idle by E-2) over a 60-frame window ≥ 99.x%" as the serving criterion. Treat titles below it as unserved by D1.

### M5. Non-VBlank IRQs in the window: PXI/IPC from the ARM7 abort the strip
- Sources:
  - nds-bootstrap's own IPC syncs (NB `cardenginei/arm7/source/cardengine.c:782,1765,1792,…`);
  - the DScheevos snapshot IPC (NB `cardenginei/arm9/source/cardengine.c:1645`);
  - SDK PXI FIFO IRQs for sound and touch replies, which arrive at arbitrary lines (spec.).
- Each one landing in E..191 cuts the notice.
- Aborts **before** E-1 are harmless only if the idle thread re-arms on return. The proposal implies that ("first thing after halt_entry") but doesn't state it.
- **Fix:**
  - state the re-arm explicitly;
  - add a census column: P(no non-VBlank IRQ in lines E-1..191).
- Streaming games, where CE9 DMA is in flight, also fail the "no CE9 read in flight" guard. CE9 uses NDMA0/1 (NB `cardDma.thumb.c:230,442,462`), which collides with F = NDMA0 and B = NDMA1.

### M6. NWRAM is not universal, and its numbers are unproven
- **G says the source must be main memory.** "source address must be in Main Memory. Transfer starts at next frame." (G:8193-8196). Both sentences contradict D1: an NWRAM source, and a mid-frame F start (H-ARM).
  - The proposal never cites them; HW 2.1 and RT already flagged G:8193.
  - The 12%-vs-30% bus figure and "frees the ARM7" are contingent on H-NW. If it fails, main RAM is back at 30%, with R18/R21.
- **NWRAM can be missing.** `dsiWramAccess` can be false or mirrored, depending on the launch path (`NB/arm9/source/conf_sd.cpp:1539-1556`, probe on `REG_SCFG_EXT7 == 0`). Then there is no NWRAM at all, so a main-RAM fallback is mandatory (no quiet exclusions).
- **It is occupied when it does exist** (DS mode, after `ARM9_SETSCFG`, all B/C slots go to the ARM9: NB `bootloaderi/source/arm9/main.arm9.c:427-433`):
  - non-ROMinRAM titles: FNT+FAT, up to 512 KB, are loaded to `0x03700000` (NB `main.arm7.c:1358-1366`, called at :2593). CE9 reads them **at run time** (NB `cardengine.c:903`);
  - ROMinRAM titles: the ROM spills up to 0x80000 into NWRAM (NB `main.arm7.c:911-914, 890`).
- **The colour LUT doesn't exclude the ROM cache.** The proposal's "exclusive in practice" is wrong: the ROM gets 0x32800, then the LUT follows (`main.arm7.c:911`, `:1362`).
- **Reserving a slot has a cost:** it shrinks the ROM-in-RAM window, so edge-size ROMs become SD-streamed (load stutter; spec.). Placing the slot after FNT/FAT's actual end is dynamic per game.
- **The brief's "ARM7 owns B/C until SETSCFG" is a boot-time fact only.** During play the ARM9 owns them and the ARM7 can't write them. So that worry is resolved, but the occupancy isn't.
- **NWRAM cacheability under the SDK MPU is unknown.** If it is write-back cached, module writes to the strip buffer must be cleaned, and T1/T2 writes evict game D-lines. That is ~24 lines (~1.1k cycles), not ≤230.
- **Test:** H-NW, plus an ARM7 loop running **from ARM7 WRAM / shared WRAM**, not only main RAM. That checks NWRAM traffic doesn't contend with the game's ARM7 code fetch (spec.).

### M7. The B → F arming may hit the NDMA "read-only while enabled" effect
G:28016-28019: during enabled transfers, `4004104h..4004173h` may be read-only except the enable bit. That range covers **all** channels.
- If B's write to NDMA0CNT lands only bit 31, F starts with CNT = `0x80000000`:
  - dest **increment**;
  - startup **00h = game Timer0**;
  - so on a Timer0 overflow it writes up to N×128 words sequentially from `0x04000068` across I/O, IME and IE. That is catastrophic.
- **Fix:**
  - The CPU pre-writes F's full CNT minus the enable bit, before enabling A/B.
  - T2[L<E-1] = that same value (not 0), and T2[E-1] = value|enable. This works under both behaviours.
  - Configure F's SAD/DAD/TCNT before any channel is enabled.

**Test:** H-ARM, with Timer0 running in the lab ROM.

### M8. D3 pass-through: whole-word clobber can't be excluded by a survey
"D3 applies only if raster keys don't include DISPCNT A" rests on a 45-title melonDS sample of sampled play. Rare scenes can't be ruled out.

**Fix:** a post-handler check in the pass-through path. After the game's raster handler returns, if DISPCNT ≠ `cap|mode bits`, abort with a byte restore. That costs ~10 cycles per raster IRQ. Then 2.1k → ~4k cycles a frame for every-line titles: state it.

### M9. ~11 cycles a game IRQ is optimistic; ~100-400 cycles a frame needs re-costing
- The handler is in ITCM (SURV:345), so the check must branch to a stub. The stub runs as follows:
  - in **cached main RAM**: ~13 cycles warm, but +46-92 per IRQ when cold. It is cold after any module pass.
  - in **uncached NWRAM**: instruction fetches at N32 = 4 bus = 8 ARM9 cycles each (G:7633-7636 class), so ~25-35.
- The I/O read stalls up to +16 cycles if F holds the bus.
- The abort path is ~60 cycles plus a cold I-line (+46-92) plus B3's busy poll (+8).
- The "46-cycle refetch per notice toggle" is wrong (ITCM isn't cached) but harmless.
- **Re-estimate (spec.):**

| Case | Cycles a frame |
|---|---|
| 3-10 IRQs a frame, cold stub | ~300-1000 |
| Every-line raster titles | 3-5k |

  Still small, but publish this range, not 0.01-0.04%.
- V3/V4 (DKR, GSDD) need their own patches. Exactly the hard classes.
- **Fail-closed:** re-read the patched ITCM word and DTCM+3FFC at each arm. If the patch is missing or the vector was swapped, don't arm (RTZ:208).

### M10. D4/D5: the DSP abort is asynchronous, so the clobber returns; DS-mode boot doesn't enable the DSP
- **The race.** With D2d, the ARM9's DSP_PSEM store returns at once and the game's handler runs while the DSP is still taking its IRQ and doing an AHBM read-modify-write. If the handler writes DISPCNT in that window, the DSP's byte-2 write (old byte with mode 1) **overwrites the game's byte-2 change**. If the handler reads first, it sees mode 3.
  - Making the ARM9 wait for the DSP ack costs the AHBM round trip (~µs, ~100+ cycles), which kills the 1-2-cycle claim.
- **D5 adds non-atomic AHBM RMWs while the game runs**, every frame:
  - a game change to DISPCNT bits 16-23 between the DSP's read and write is lost;
  - a game mode change inside E..191 (to mode 0/2, or `GX_SetGraphicsMode`) gets "restored" to the saved mode at 191.
  - The watch can't tell the game's intent apart from our leak.
- **Boot.** In DS mode nds-bootstrap writes `SCFG_EXT9 = 0x8300C000|BIT(16)` and locks it (NB `main.arm9.c:427-433`):
  - bit 18 (DSP I/O) off;
  - SCFG_CLK bit 1 and SCFG_RST bit 0 not written;
  - all B/C slots go to the ARM9.

  The "already exercised" path (`:416-418`) is DSi-mode only. D4 needs a new DS-mode boot path that maps two slots to the DSP and loads Teak code before the lock, taking 64 KB from FNT/ROM (M6).
- **Audio.** SNDEXCNT mixes DSP and NITRO by ratio (G:28062-28085). A silent DSP should be inaudible (spec.).
  - **Test:** a bit-exact line-out capture with the DSP on vs off.
- **D5's bank reuse.** Leaving strip pixels in a "free" bank breaks games that later map it and rely on earlier-cleared contents. VRAMCNT is write-only, so the game's re-mapping can't be detected.
  - Needs snapshot-and-restore of the bank lines at notice end, and still races a mid-notice re-map.

### M11. D6 assumes a 1:1 capture → display mapping
The guard checks capture source A only. Dual-screen-3D titles may:
- use capture size < 256×192 (G:8158);
- display the capture as OBJ tiles or an offset/affine BG on engine B.

Then strip lines at E..191 in the bank don't land at E..191 on screen X.

**Fix:** guard on DISPCAPCNT size = 256×192 and on engine B BG2/3 bitmap with identity affine and 0 scroll (reads only). Otherwise unserved: report it.

H-CAP also needs a capture-vs-ARM9-VRAM-write conflict check on the same bank.

### M12. Section 4 rejects class A 3D against the wrong bar
The owner's bar is "no lag frames; cost bounded, measured, minimal" (BRIEF), not zero.
- The FIFO strip during work costs ≤ ~16k ARM9 cycles a frame for N = 32, at most 1.4%. It scales with N, so an 8-line strip is ~0.4%.
- MKDS is halted 57-65% of its frame (CEN), so a bounded 1.4% can't create lag frames there unless frames are already at the edge.
- The real blocker for running during work is the DISPCNT invariant (game reads and writes while mode 3 is live), not bus cost.
- **Fix:** restate class A as "servable at a bounded X ≤ 1.4% if the DISPCNT problem is solved (D4 byte writes or a DISPCNT-writer patch)". Measure lag-frame Δ on MKDS (T1 variant), rather than declaring it unservable.

---

## Minor
- **m1.** A mid-line abort leaves a one-line half-strip, and the FIFO underflows for the rest of that line. These are in our strip area only, but they are a visible tear/flicker of the notice. Do the mode-1 restore before stopping F, then stop F.
- **m2.** F may stay busy into the next frame if FIFO consumption is short of TCNT. Then the next arm writes to an enabled channel (M7). Stop F unconditionally at the VBlank hook.
- **m3.** D7 replacement policy:
  - Under pseudo-random replacement (CP15 c1 bit 14; the SDK setting is unknown), refills evict other still-resident snapshot lines. 95% recovery is unlikely.
  - Under round-robin, a single refill in a set also evicts a resident line.
  - Directed allocation via temporary I/D lockdown index (C9) during the restore is the fix; restore C9 to 0.
  - The c15 debug TAG registers (G:94735-94745) are listed for ARM946E-S, not annotated as NDS9-verified (BIST is). H-CR must first check that a tag readback matches known lines.
- **m4.** D7's restore is up to ~8.8k bus cycles of main-RAM bursts in idle, which contends with the ARM7 (the R18 class the proposal claims to close). An interrupted restore resuming later re-loads a stale snapshot; B4's counter fixes that too.
- **m5.** The strip's bus use slows the module's own evaluation pass on strip lines (≤12% there). That cuts the eval budget the WORKLIST says is short for heavy sets (MKDS).
- **m6.** The abort adds ≤ ~100-170 cycles to the IRQ latency of raster titles' handlers for lines < E. Check with a pixel diff on class-B titles that their H-Blank writes still land in H-Blank.
- **m7.** Option b (IE sentinel) is impossible in DS mode without a boot change: SCFG_EXT9 bit 8 is off and locked (`main.arm9.c:427-433`).
- **m8.** The BCNT variant: G is unsure whether the interval applies per physical or logical block (G:27931). Test the delay basis before relying on 2 writes a frame.
- **m9.** The proposal's G citations for the Teak clock are off by about 15 lines (the text is at G:24644-24648). The content is right.

## Numbers checked
| Claim | Verdict |
|---|---|
| Line = 2130 bus = 4260 ARM9 | correct |
| Main-RAM FIFO ≈ 30% of a strip line, NWRAM ≈ 12% | arithmetic fine; the NWRAM half is contingent on G:8193 being only a legacy-DMA rule (H-NW) |
| D7 worst refill 384×46 ≈ 17.7k = 1.6% | correct |
| D7 snapshot "1-2k" | more like 2-3k (2 CP15 ops × 384 + loop) (spec.) |
| D2a ~11 cycles a game IRQ | wrong as coded (B1); ~13 warm to ~60 cold with the ITCM stub |
| X ≈ 100-400 cycles a frame | after the fixes ~300-1000 (normal), 3-5k (every-line raster) |
| D4 abort 1-2 cycles | only if unsynchronised, which is unsafe (M10) |
