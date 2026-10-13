# R1-DSP lane report: the DSi Teak DSP in DS-mode sessions

**Decision: NOT WORTH IT. Do not build or experiment with a DSP offload. Ship only a read-only SCFG/MBK probe at boot. If hardware time is ever free, the lab-ROM probes are kept on file as research.**

## 1. Findings

Corrections from the red-teams are included. I re-checked the slot mapping and the IRQ polling against the code myself.

### Enabling the DSP

- **It needs three ARM9-side bits.** The DSP needs SCFG_EXT9 bit 18 (access to 0x40043xx), SCFG_CLK bit 1 (DSP clock) and SCFG_RST bit 0 (reset release). The DSP's IRQ to the ARM9 also needs SCFG_EXT9 bit 8. Source: GBATEK "DSi Control Registers (SCFG)", 4004004h/4004006h/4004008h.
- **nds-bootstrap's DS-mode path never sets them.** It writes `REG_SCFG_EXT = 0x8300C000 | BIT(16)`, which leaves bit 18 and bit 8 at 0. It only reads SCFG_CLK. It gives all 16 NWRAM-B/C slots to the ARM9, then clears bit 31 to lock SCFG (`retail/bootloaderi/source/arm9/main.arm9.c:428-436`, verified). Only the DSi-mode path enables the DSP (`:416-418`).
- **There is one pre-lock window.** ARM9_DSCHEEVOS_INIT runs before ARM9_SETSCFG and before both SCFG locks (`main.arm7.c:2601`, `:2653`, `:2692`; `main.arm9.c:373-377`). It is the only place where SCFG and MBK can still be changed.
- **If the launcher locked the ARM7 SCFG, there is no DSP at all.** nds-bootstrap records this as `b_scfgLocked` (`main.arm7.c:227-229`, `:258-259`).

### Memory

- **The DSP's memory is only NWRAM-B (code) and NWRAM-C (data).** The split is in 32 KB slots set through MBK2-5 (GBATEK "DSi New Shared WRAM"). It is frozen once SCFG is locked.
- **The B/C pool is already spoken for.** It holds the FNT/FAT cache and the optional colour LUT, and memplan earmarks the rest (about 450-500 KB, `memplan/ndsb.md:61`) for the NWRAM placement lever. That lever is the only one that targets the real bottleneck: rcheevos D-misses, 36-41% of MKDS evaluation.
- **Correction (red-team): the ambitious proposal named the wrong slots.** nds-bootstrap writes the slot bytes in "inverted order", MBK3/MBK5 = 0x8185898D (`main.arm7.c:237-246`, verified):
  - Slot 7 has offset 0, so it sits at ARM9 0x03740000 (B) and 0x03700000 (C). C slot 7 is the FNT/FAT cache.
  - Slot 0 (byte 0x9D, offset 7) is the one at 0x03778000 / 0x03738000.
  - Taking slot 7 would break every game in cache mode.
- **Data space is tiny.** The DSP data space is 64 Kwords, of which 62 Kwords (124 KB) are RAM. Code space is at most 256 KB (GBATEK "DSi Teak Misc").

### Speed and bus

- **Speed:** the CPU runs at 107.24 MHz and DMA/Timer0 at 134 MHz. Most opcodes take one cycle and there is no cache (GBATEK "Teak Speed").
- **Reach over AHBM:** AHBM with Teak DMA can read and write main RAM, I/O, VRAM and OAM. DTCM/ITCM are "probably ignored" (GBATEK AHBM section).
- **The 1 KB limit:** libteak cannot cross a 1 KB ARM-side boundary and calls the DMA registers "may be unstable" (blocksds libteak `dma_8h`).
- **Nothing public describes AHBM bandwidth, latency or bus arbitration.** The only hint is the NDMAGCNT "cycle selection" note.

### Coherency

- **Inferred: AHBM reads skip the ARM9 D-cache.** The ARM9 cache and TCMs are not on the system bus (GBATEK NDS9 cache section), so AHBM sees only RAM. A watched line is dirty at VBlank in 88% of Platinum frames (`evalspeed-team/arm7-notes.md` Q5).
- **Correction (red-team): DTCM is part of the snapshot** (`dscheevos.c:355-365`, `:744`, `:2563`), and AHBM cannot read it.
- **Correction (red-team): the ARM7 and game DMA keep running while the ARM9 halts.** A DSP copy can therefore mix values from different moments, for example in the IPC and input area.

### IRQ latency today (correction, verified)

Today's capture does not run masked for about 5k cycles. `interrupt_pending()` services pending IRQs every `POLL_WORDS = 16` words (`dscheevos.c:807-810`, `:2548`, `:2573`; `module.c:163`).

Any DSP design hits the same problem. When an IRQ wakes the halt, the game's own handler runs before the module gets control back to abort the DSP. AHBM traffic therefore overlaps every game IRQ handler, and that window cannot be bounded by an "abort latency" figure (red-team on the conservative proposal).

### Toolchain

- **teak-llvm has 16-bit `char` (CHAR_BIT != 8).** It is "quite buggy" (BlocksDS FAQ; codeberg blocksds/sdk issue #93) and comes only from Wonderful Toolchain, which is Linux-only.
- **The DSP cannot run rcheevos unmodified.** That rules out DSP evaluation, and read_memory too, because rcheevos calls it synchronously.
- **The pinned devkitARM r65 / libnds 1.8.0 has no DSP API.**

### Emulator

melonDS cannot validate any DSP claim that matters:

- AHBM is instantaneous and ignores bus arbitration (`tools/melonDS/src/DSi_DSP.cpp:291-300`).
- teakra runs DMA synchronously (`teakra/src/dma.cpp:51-60`).
- The DSP is clocked at the ARM9-timestamp rate (`DSi_DSP.cpp:753-793`).
- The cache model tracks timing only, so stale reads never appear (`CP15.cpp:450-454`, `:501-540`).

### 3DS

The 3DS maps NWRAM-B/C onto its own DSP memory in TWL mode (GBATEK "3DS Config - ARM7 Registers"). No source confirms that a DSi-mode program can run the DSP under TWL_FIRM.

### The ARM7 dominates the DSP as a second processor

- It has 465-510k free cycles per frame (`arm7-notes.md:41-49`).
- It has 8-bit `char`, so it can run rcheevos unmodified.
- It has the same coherency problem.
- It needs no NWRAM split, no new toolchain and no SCFG change.

## 2. Recommendation: not worth it

### Expected ARM9 cycles saved per frame

All figures below are derived (D), not measured.

| Game | Movable work | Net saved | Changes "fits"? |
|---|---|---|---|
| **MKDS** | copy_snapshot 2.8k + run_chains 2.65k ([PM]:45-46) | **0-5k** (<1% of 873k) after the clean and handshake cost | No. Still about 2.7x its w1024 idle. The gap is rcheevos (785k). |
| **Platinum** | capture about 23.5k instr ≈ 77k cycles at CPI 3.3 ([CPU]); chains stay on the ARM9 | **30-65k** (2-4% of 1,527k) | No. Still about 1.85x mean idle. |
| **Kirby SSU** | copy_snapshot + dsc_capture ≈ 167k of 191k capture ([KB]) | **90-145k**, minus per-wake clean/handshake (20-75k at 20-37 wakes/frame) | Already fits (556k vs 688k w1024). The same share is available from ARM9-only capture work, such as last-halt-before-VBlank prediction or IF-based copy skipping, with no hardware risk. |
| HG/SS, TWEWY, Digimon | capture overflow / chains | <5% of each gap | No |
| Network, journal MAC, change detection | <5k combined ([KB]) | not worth the IPC; TLS and lwIP cannot be ported (16-bit `char`) | No |

**Net:** the DSP does not change "fits every frame" for any game. Its only meaningful win, on Kirby-class overhead-bound titles, is cheaper to get on the ARM9. Meanwhile it permanently takes at least 64 KB from the NWRAM pool, and that pool feeds the memplan lever that does move MKDS and Platinum. On the games that miss frames, the DSP plausibly makes things worse.

### Zero-game-cost argument

The full DSP design fails or cannot be proven against the hard rules:

1. **Bus.** AHBM traffic overlaps the game's IRQ handlers and the ARM7's sound code and DMA, with arbitration that nobody has documented. We could only measure that on hardware, and melonDS cannot show it.
2. **Visible state.** It changes state the game can see: SCFG_EXT9 bit 18 turns 0x40043xx on for an NTR game. That is invisible only if hardware probes prove it.
3. **IRQs.** It risks extra IRQ latency, depending on how the abort path works.
4. **Snapshots.** It needs a cache clean on every wake, and DTCM words fall back to the ARM9. A missed clean gives silently stale snapshots, i.e. wrong unlocks in hardcore. melonDS cannot detect that.
5. **Upstream.** It needs a 5th upstream hook that edits the SCFG/MBK lock, against the CLAUDE.md budget of about 4 hooks.
6. **Footprint.** It takes 64 KB of NWRAM for the whole session, against the smallest-footprint ruling.

**The read-only probe is provably zero-cost.** It runs once at boot, reads registers and writes nothing to hardware, game RAM or SD in play.

## 3. What to do instead

### Step 0: read-only SCFG/MBK probe (recommended)

It costs nothing, and it answers a question that the display lane (`docs/display-library.md:221-229`, `:384`, which wants SCFG_EXT9 bit 8 set before the lock) and any future SCFG-touching lane also need answered.

**Mechanism**
- In the module's ARM9_DSCHEEVOS_INIT handler, before ARM9_SETSCFG, read REG_SCFG_CLK (0x4004004), REG_SCFG_RST (0x4004006), REG_SCFG_EXT9 (0x4004008) and MBK1-8 into a shared struct.
- The ARM7 logs it through the existing boot `dbg_printf` (`dscheevos_hooks.c:262`), which runs at boot and not in play.
- **The flush path is unverified (red-team).** Confirm that the ARM7 can read the struct after the ARM9 init returns, or name the actual path before building.

**Caveats to write down with the results**
- **Coverage:** `dscheevos_hooks.c` returns early through `disable(...)` (for example "no OS_Halt", around line 250) before it sets the flag at :260. Either put the read before those early returns, which is upstream code, or limit the pass criterion to games where the module is enabled.
- **"What the launcher left" is an inference.** Nothing on the DS-mode path writes SCFG_RST, and `setCpuClock` only toggles bit 0 (libnds 1.8.0 `cpu_clock.s`), but the read happens at module init, not at nds-bootstrap entry.

**Tests**
- **Host test:** the stats/log struct round-trips the new fields.
- **melonDS** (`--thumb-cycle`, cache model; one emulator at a time per session coordination): Kirby and MKDS boots log the fields. In-play cycle delta = 0 by construction, so no lag A/B is needed beyond the normal one.
- **DSi XL test**, decided in advance:
  - Launch Kirby SSU and MKDS from TWiLight Menu, and from hiya/unlaunch if available.
  - PASS = exactly one line `SCFGPROBE clk=XXXX rst=XXXX ext9=XXXXXXXX mbk=...` per boot for module-enabled games, and 5 minutes of play with no observed difference.
  - Record the values in TESTING.md.
- **New 3DS XL:** same procedure. The extra reading is whether the values differ under TWL_FIRM.

### Steps 1-3: DSP lab ROM (parked as research)

Run this only if hardware time is free. It is a standalone lab ROM: it does not touch the game path and does not need the 5th hook.

**Probes**
- **P1, does the enable survive the lock?** Set bit 18, the clock and reset in the pre-lock window. Lock as stock does. Do 1,000 CMD0→REP0 echoes.
  - PASS = 1000/1000 echoes, and the locked 0x4004000-0x4004010 reads 0 from code that runs after the lock.
- **P2, AHBM coherency.** Write through a write-back cached pointer, then read via the Teak.
  - PASS = without a clean, stale data appears in at least 1 of 10,000 trials (which proves the test can see the hazard), and after a VA clean plus write-buffer drain, 10,000/10,000 reads are correct.
  - These results also apply to the ARM7/NDMA lanes.
- **P3, bandwidth and contention.** Copy 8 KB main-RAM→main-RAM in x8 bursts of 32-bit units, timed on an ARM9 timer. Run an ARM7 main-RAM loop and a DMA3 loop alongside it.
  - Record the throughput.
  - Contention PASS = ARM7 and DMA3 slowdown ≤2% each.
  - (This fixes the original proposal's mismatched 6 KB / 8 KB criteria.)
- **P5:** repeat P1 and P2 on the New 3DS XL.

**Reopen gate, all required**
- (a) P1-P3 pass.
- (b) The ARM9-only capture work (last-halt prediction or IF-based skip) has landed, and capture is still above about 100k per frame on some game.
- (c) A game is identified that goes from not fitting to fitting because of the DSP.
- (d) Memplan confirms it can spare 2 slots of B/C.

**If reopened, fix these first** (graft of both red-teams):
- Use slot indices 0 and 8, rewriting their offsets to 0 for the DSP.
- Keep DTCM words on the ARM9.
- Never do a masked tail after the wake: drop the partial snapshot on abort, as today's restart does, or tail-copy using `interrupt_pending` polling.
- Make the ring uncached, or clean it after any ARM9 write.
- Use a same-instant oracle for cross-checks.
- Measure from IRQ entry to DSP quiescent, not just the abort ack.

### Coordination

| Who | What to tell them |
|---|---|
| Display session (dscheevos-43) | The probe's SCFG values. Any pre-lock SCFG_EXT edit should share one hook. Display's bit 8 exposes IE 14/15/24-27. The DSP lane makes no NDMA claim, so there is no channel conflict. |
| memplan / eval-speed | The DSP takes no NWRAM. Flag GBATEK's "ARM9 accesses WRAM at 33 MHz" (Teak Speed): it may undercut the assumption that NWRAM placement beats main RAM on latency, and M1/M3 should measure it directly. |
| idle-coverage / arm7eval | The ARM7 is the second processor for our work. P2's coherency result, if ever run, applies to ARM7 and NDMA reads. The clean-watched-lines-at-halt primitive should be shared code owned by capture/perf. |
| Capture lane | The Kirby capture saving (about 190k) belongs to the ARM9-only per-wake copy reduction, not to the DSP. |
| nds-bootstrap card engines | No change. The ARM9 in-game menu's `REG_SCFG_CLK ^= 1` (`arm9_igm/inGameMenu.c:662-671`) is presumably a no-op once locked; that is unverified. |

## 4. Open questions only hardware can answer

1. What SCFG_CLK bit 1 / SCFG_RST / SCFG_EXT9 / MBK values does each launcher (TWiLight, hiya, unlaunch) leave, on the DSi and under 3DS TWL_FIRM? *(Step 0)*
2. After the bit 31 lock, are the 0x40043xx ports still reachable if bit 18 was set first? Does the locked SCFG window read as 0 from the game side? *(P1)*
3. Do AHBM reads see dirty ARM9 D-cache lines? Expected: no. *(P2)*
4. AHBM bandwidth, latency and arbitration against the ARM9, the ARM7, DMA0-3, NDMA and the display FIFO. Does a burst stall the ARM9? *(P3)*
5. Does a running Teak change anything the game can see: audio (BTDMP, SNDEXCNT mix), power draw, bus timing?
6. Can the Teak run at all from DSi-mode code on a New 3DS XL? *(P5)*
7. Is the 1 KB AHBM boundary a real bus rule or a libteak limitation?
8. Is ARM9 NWRAM access really 33 MHz (GBATEK)? This matters to memplan more than to this lane.

## 5. Summary for the orchestrator

R1-DSP: not worth it. The Teak can't run rcheevos unmodified (16-bit `char`, 124 KB data space, buggy toolchain). As a snapshot copier it saves 0-5k on MKDS, 30-65k on Platinum and 90-145k on Kirby (all derived); that changes "fits" for no game, and the Kirby share is cheaper on the ARM9.
It would cost at least 64 KB of the NWRAM-B/C pool that memplan's evaluation lever needs, a 5th upstream hook on the SCFG/MBK lock, and unprovable bus, coherency and DTCM risks. The ARM7 is better as a second processor in every respect.
Ship only the free read-only boot probe (SCFG_CLK/RST/EXT9/MBK, logged once per boot; it serves the display lane too). The DSi XL lab-ROM probes P1-P3 stay parked as research.