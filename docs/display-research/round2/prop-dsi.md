# Round 2 proposal: DSi-only hardware and the other processors

Angle: find capacity the game can't see or use, so that a notice costs the game nothing.
Read-only work. Nothing was run. **(spec.)** marks my inference rather than something read in a source.

Abbreviations:
- G = GBATEK, `(local scratch)` (line numbers).
- NB = `dscheevos/external/nds-bootstrap/retail/`.
- DL = `dscheevos-display/docs/display-library.md`; RTZ = `docs/display-research/red-team-zero-lag.md`; HW = `docs/display-research/hardware.md`; OR = `docs/overlay-research.md`.
- MEL = `dscheevos/tools/melonDS/src/`.
- CEN = `~/Developer/DScheevos/private/census/out/<game>/` (idle-coverage census raw data).

Units: bus = 33.514 MHz; ARM9 = 2 × bus. One line = 355 dots × 6 = **2130 bus cycles = 4260 ARM9 cycles**. A frame is 263 lines, about 560k bus cycles (G:7828-7831, G:7605-7612).

---

## 0. Headline

1. **The strip needs no CPU and no IRQ of ours.** DSi NDMA can run the whole sequence in hardware:
   - an H-Blank-started channel writes a table of whole DISPCNT words, one per line;
   - a second channel arms the FIFO channel at the entry line;
   - the FIFO channel is fed from NWRAM.

   This is "D1", below. It has zero module IRQs, so:
   - no IRQ entry cost and no handler cache pollution;
   - no M1 module re-pass, so no capture loss;
   - no line-191 latency race (RTZ M5), since DMA timing is exact.
   - SCFG_EXT9 bit 8 (R5) is no longer needed.
   - R4 shrinks to a few instructions in the game's dispatcher.

   **The 4a clobber (OR:111) is not solved by DMA arithmetic.** That is impossible (section 2.1). It is solved by an invariant:
   - DISPCNT is captured by the CPU *inside the idle pass*;
   - the game can only run again through an IRQ;
   - the patched dispatcher kills the DMAs before the game's handler runs.

   With abort-on-every-game-IRQ, the game **never observes** our DISPCNT value, so R1, R2 and R20 go to zero for the classes D1 serves.
2. **NWRAM as the FIFO source removes the strip's main-RAM traffic entirely.** RTZ's B1/M6 and R18/R21 (ARM7 contention, game threads blocked on the ARM7 or PXI) are then closed in principle:
   - NWRAM is 32-bit, 1-cycle;
   - it is off the main-RAM arbiter;
   - the ARM7 doesn't touch an ARM9-mapped slot.

   With DMA-busy and GXSTAT guards, "halted for any reason" becomes as safe as "halted waiting for VBlank". The extra 12 MB is the same chip and arbiter as the first 4 MB, so it doesn't help. Test H-NW decides.
3. **The Teak DSP is the only processor besides the ARM9 that can write ARM9 I/O, and it can do byte writes.** AHBM reaches "04000000h/ARM9 I/O → works" with an 8-bit data type (G:25619-25640, G:25655-25661). melonDS models this (MEL `DSi_DSP.cpp:293-300`). That gives:
   - a co-processor with arithmetic, line-locked to the dot clock (Teak timer 134.056 MHz = 4 × bus, G:24658-24664);
   - the ability to do the mode-bit read-modify-write the NDMA can't;
   - an abort that costs the ARM9 one buffered store.

   It is the enabler for a strip that runs **while the game works** (D5, mode-2 VRAM display, zero ARM9 bus). It costs boot-time SCFG bits and a Teak toolchain.
4. **The biggest remaining item in X isn't the strip; it's cache refill after the module's idle pass.** The worst case is about 17.7k cycles (RTZ M1). The ARM946E-S exposes its cache tags through CP15 c15 debug registers (G:94723-94745) and has an I-cache prefetch-by-address op (G:94505). So the module can:
   - snapshot the game's cache tags at idle entry;
   - re-fill those lines before WFI, in idle time (D7).

   Cache *lockdown* is the wrong tool: it locks whole ways, a permanent 25% loss for the game (G:7344-7348, 94523-94524).
5. **Coverage.**
   - D1 with idle-coverage's hooks serves the allow-list games and classes C and D.
   - D3 (raster pass-through) serves the pure-B titles.
   - D6 (capture-bank patch) serves E.
   - D5 (DSP + mode 2) serves 2D titles in class A that have a free VRAM bank.
   - **Class A on 3D titles (Mario Kart DS, Diddy Kong Racing) can't be served at zero cost on a DSi by any hardware path I found.** The evidence is in section 4. Two outs remain: a per-line idle census showing a top-edge idle band, or test H-ML showing the bus is multi-layer.

---

## 1. Facts: what each DSi block can and can't do

### 1.1 NDMA (G:27878-28025)
- **Four ARM9 channels.** Each has: SAD, DAD, TCNT (total), WCNT (logical block), BCNT (interval timer and prescaler), FDATA (fill), and CNT. CNT holds dest/src update modes, the dest/src **reload** bits (12, 15), the physical block size, the startup mode, repeat mode, IRQ enable and enable (G:27942-27953).
- **ARM9 startup modes** (G:27958-27971):
  - timers 0-3: game state, so not usable;
  - 06h V-Blank;
  - **07h H-Blank (not during V-Blank)**;
  - 08h display sync;
  - 09h main-memory display (inferred, HW 2.1);
  - 0Ah GXFIFO;
  - 0Bh camera;
  - 10h-1Fh immediate.
- **Physical blocks are never split** (G:27920-27923). That bounds any stall we cause, and the drain after a stop, at one block.
- **BCNT** inserts a delay after each block: 0-65535 ticks at 33.5 / 8.4 / 2.1 / 0.52 MHz (G:27925-27931). At prescaler 0, one line is exactly 2130 ticks, so the timer is line-exact up to 30 lines. At prescaler 1, a line is 532.5 ticks, so any even line count is exact, up to 123 lines.
- **Fill data:** source = FDATA, "within 0 clock cycles" (G:27933-27940).
- **NDMAGCNT** sets fixed priority (NDMA0 highest) or round-robin. Its "cycles that can be executed by ARM9 and **DSP** during NDMA" (G:27884-27893) says the ARM9 CPU, the DSP's AHBM and NDMA share one arbitrated bus.
- **Writes during a transfer may be limited to the enable bit** (G:28016-28019). This must be tested (H-KILL).
- **No arithmetic, word transfers only.** A DMA can write another DMA's registers, since they are ordinary I/O, so channels can arm channels.

### 1.2 Display FIFO, capture, modes (G:8134-8199)
- **Mode 3 (main-memory display)** shows the FIFO.
- **Mode 2 (VRAM display)** shows bank A-D, selected by DISPCNT bits 18-19, which sit in the same byte as the mode bits. It reads VRAM through the display's own port: **no ARM9-bus traffic**.
- **Capture source A taps before the "Select Video Input" stage** (block diagram, G:8202-8235). So a mode-3 or mode-2 strip **is not captured**: dual-screen-3D games' captured frames won't contain it (class E).
- Capture writes line L as it is drawn, and its busy bit clears at line 192 (G:8162-8166).

### 1.3 NWRAM, main RAM, the extra 12 MB
- **Main RAM** sits on a 16-bit bus: N32 = 10, S32 = 2 bus cycles for data (G:7633-7636).
  - A 4-word FIFO block costs about 16 read + 4 write = **20 bus cycles**.
  - Over 32 blocks a line, that is about **30% of the bus on strip lines**, about 3.6% of a frame for N = 32.
  - It is arbitrated against the ARM7.
- **WRAM/NWRAM, BIOS and I/O** are 32-bit at 1 cycle for the DMA (G:7633-7637; "ARM9 can access WRAM at 33MHz", G:24666).
  - A block costs about 8 bus cycles, about **12% of strip lines**, **about 1.5% of a frame** for N = 32.
  - No main-RAM arbiter is involved. The ARM7 can't see an ARM9-mapped slot.
- **The extra 12 MB is the same chip**, remapped by SCFG_EXT9 bits 14-15 (G:24318-24330). It has the same timing and arbiter, so a source there costs exactly what main RAM costs. It is useful only for assets and module code.
- **NWRAM in DS mode under nds-bootstrap:**
  - the ARM9 owns slots after the DS-mode SCFG step (NB `bootloaderi/source/arm9/main.arm9.c:423-433`, `transferToArm9(0..15)` then lock);
  - CE9's colour-LUT code and tables live at `0x03732800`, `0x0374B800-0x0376Dxxx` and `0x03770000` (NB `cardenginei/arm9_colorlut/source/cardengine.c:15-18,131-360`);
  - nds-bootstrap **spills its ROM cache into NWRAM at `0x03700000`** when main RAM is full (NB `bootloaderi/source/arm7/main.arm7.c:889-891`);
  - so NWRAM is contested in exactly the big-ROM / class-F cases.
- **MBK is locked with SCFG** (SCFG_EXT9 bit 31, G:24314; NB main.arm9.c:432). Slot ownership is therefore fixed at boot; nothing can hand slots between processors at run time.

### 1.4 The ARM7
- **No path to ARM9 I/O.** It can't reach engine registers, the ARM9 NDMA or POWCNT1. VRAM C/D are visible to it only if the game maps them to the ARM7 (game state).
- **It can't feed the ARM9 FIFO.** Its NDMA has no display startup (G:27976-27990).
- **Spare time:** NitroSDK ARM7 code mostly halts: sound is mixed in hardware, and the ARM7 serves PXI commands, touch, RTC and WM. Not measured here; idle-coverage should log ARM7 halted %.
- **As a timer or sequencer for the ARM9,** it only reaches the ARM9 through the IPC IRQ, which belongs to the game and nds-bootstrap (HW 2.9). That is worse than an NDMA timer. Rejected.
- **As a renderer:** to hand pixels over without contention, it would need a buffer the ARM9 can read that isn't main RAM. With MBK locked there is none, and main-RAM writes contend with the game. Rejected (and not needed: rendering is a once-per-notice idle job).
- **What it should do (asks to idle-coverage):** section 6.

### 1.5 The Teak DSP (G:24484-26070)
- **AHBM** lets Teak DMA read and write most of the ARM9 map: main RAM, shared WRAM, **ARM9 I/O**, palette, VRAM and OAM, but not TCM. It does 8-, 16- or 32-bit units (G:25619-25661, 26050-26056).
- **Clocks:**
  - timer and DMA at 134.055928 MHz, which is exactly 4 × 33.513982, so a line is exactly **8520 Teak timer ticks**;
  - the CPU at 107 MHz (G:24658-24664).
- **ARM9 ↔ DSP signalling:** semaphores, command and reply registers at `0x04004310-0x04004334` (G:24721-24762). An ARM9 store to DSP_PSEM is a buffered I/O write.
- **Enabling it** needs SCFG_EXT9 bit 18, SCFG_CLK bit 1 and SCFG_RST bit 0 (G:24246-24255, 24273-24276, 24290-24311), set before the DS-mode SCFG lock. nds-bootstrap sets all three for DSi-mode titles (`REG_SCFG_CLK = 0x87; REG_SCFG_RST = 1`, NB main.arm9.c:416-418), so the path is exercised. An NTR game never touches `0x040043xx`.
- **Memory:** code and data come from NWRAM-B/C slots mapped to the DSP at boot.
- **Toolchain:** devkitARM has none. Options are community teak-llvm or hand assembly. melonDS's teakra runs Teak code with AHBM bound to `ARM9Read8/Write8` (MEL `DSi_DSP.cpp:293-300, 442-452`).

### 1.6 Camera, SCFG "revised" bits, LCD/I2C
- **Camera NDMA (0Bh)** needs a powered, I2C-configured camera to produce DRQs. It offers nothing that 07h or BCNT doesn't. Rejected.
- **SCFG_EXT9 bit 3 (revised 2D engine), bit 12 (extended LCD circuit), bit 13 (extended VRAM)** (G:24290-24311):
  - undocumented beyond names;
  - DSi mode sets 12 and 13, not 3 (`0x8307F100`);
  - they change NITRO behaviour for the game.

  Curiosity test only (HW H11). Never ship.
- **BPTWL:** the backlight register 41h is nonvolatile (HW 2.10). The camera LED 31h stays P0. The DSi has no LCD-controller overlay; that's a 3DS TwlBg feature. Rejected.
- **SCFG_CLK bit 0 (ARM9 at 134 MHz)** (G:24248) would make games lag less: a gameplay change, and a hardcore problem. G also says to switch it only from ITCM, which is the game's. Rejected.

### 1.7 ARM9 cost mechanics
- **IRQ entry** goes through the BIOS vector, DTCM+3FFC and the SDK dispatcher. Our handlers measured 300-900 cycles (HW 2.1, OR).
- **An I/O read is N32 = 4 bus cycles = 8 ARM9 cycles** (G:7633-7637). An I/O store goes to the write buffer and costs about 1 cycle.
- **A cache line refill from main RAM is 23 bus cycles = 46 ARM9 cycles** (G:7638-7640). There are 256 I-cache lines and 128 D-cache lines (G:7343-7346).
- **Lockdown is per way** (G:7348, 94523-94524). The cache is 4-way, so the smallest lock costs the game 25% of a cache in every frame. Rejected.
- **Cache debug:** the Debug Index and Instruction/Data TAG registers (G:94723-94745) and "Prefetch Instruction Cache Line" by VA (G:94505) allow a tag snapshot and an exact re-fill (D7).
- **TCM:** ITCM and DTCM are the game's, so the module has no TCM. ITCM "moving" through the protection unit (G:7335-7338) changes the game's CP15 setup. Rejected.

---

## 2. Mechanisms

### D1. The hands-free strip: IRQ-less, NDMA-sequenced, fed from NWRAM

**How the pixels reach the player.** Engine A, DISPCNT mode 3, bottom lines E..191 (E = 192-N). The FIFO is fed by NDMA from a 256×N RGB555 buffer in an ARM9-mapped NWRAM slot. The CPU does nothing while it is shown.

**Arming,** in the module's idle pass, first thing after `halt_entry`, so the game is provably halted (we *are* its idle thread). Guards first (all reads):
- DISPCNT A mode = 1;
- capture not using the FIFO;
- all four legacy DMA CNT.31 = 0;
- GXSTAT.27 = 0 and the GXFIFO empty;
- no CE9 card read in flight;
- POWCNT1.15 stable for 30 frames;
- `VCOUNT ≤ E-2` and the DISPSTAT H-Blank flag clear, re-checked after arming. If the line changed, disarm and retry or skip.

Then:
1. `cap = DISPCNT` (32-bit read).
2. Write the **DISPCNT table** T1 (192 words in NWRAM) only if `cap` changed since the last frame:
   - `T1[L] = cap` for L < E-1;
   - `T1[E-1..190] = cap | 3<<16` (mode 3; mode 1 → 3 is OR 0x20000);
   - `T1[191] = cap`.
3. Write the **arming table** T2 (192 words):
   - `T2[L] = 0` for L < E-1;
   - `T2[E-1] = FIFO_CNT`, the enable value for the FIFO channel (startup 09h, physical block 4 words, source increment, dest fixed).
4. **FIFO channel F** (NDMA0, the highest fixed priority, so card or other DMAs can't starve it): SAD = the strip buffer, DAD = `0x04000068`, TCNT = N×128, WCNT = 4. CNT is left **0**; T2 enables it.
5. **Channel B** (writes T2): startup 07h (H-Blank), WCNT = 1, physical block 1, source increment, dest fixed = `NDMA0CNT`, SAD = &T2[VCOUNT], TCNT = E - VCOUNT.
6. **Channel A** (writes T1): the same, but dest fixed = DISPCNT, SAD = &T1[VCOUNT], TCNT = 192 - VCOUNT.
7. Enable the dispatcher check (D2) for this notice if it isn't enabled yet.

**Per frame in hardware:**
- At each H-Blank from the arm line on, A rewrites DISPCNT with `cap`, its current value, so nothing changes, and B writes 0 to the disabled F.
- At H-Blank E-1, in channel-number order, B enables F (lower channel first; B must be numbered below A), then A writes mode 3.
- Lines E..191 come from the FIFO.
- At H-Blank 191, A writes `cap` (mode 1). F has delivered N×128 words and ends.
- Nothing happens during V-Blank (07h doesn't fire there, G:27965).
- **No IRQ is raised.**

**A variant with fewer register writes** (for raster titles, D3): replace A with an *immediate* channel whose 3 words are spaced by BCNT. For example, prescaler 1 with N even:
- word 0 = `cap`;
- word 1 = mode 3 at +k lines;
- word 2 = `cap` at +2k lines.

B starts it at the right H-Blank by writing its CNT. That gives 2 DISPCNT writes per frame instead of about 190.

**Why the whole-word writes don't clobber (the 4a answer):**
- Between the CPU's `cap` read and the last table write, game code can run only after a game IRQ.
- Every game IRQ passes the patched dispatcher first (D2), which stops A, B and F and restores mode 1 before the game's handler runs.
- So every DISPCNT value A writes equals the game's current value, except the mode bits on lines E..191, which only the hardware sees.
- The game reads DISPCNT only from code that runs after the abort, so **it never sees mode 3**. R1, R2 and R20 drop to zero. The `GX_DispOff` patch (R3) isn't needed for D1.

**What it touches:**
- DISPCNT, rewritten with the game's own value plus the mode bits on strip lines;
- the dispatcher code patch (R4, reduced);
- NDMA0-2;
- one NWRAM slot.

It doesn't touch game VRAM, OAM, palettes, timers, IE (unless D2 option b is chosen), SCFG bit 8 (dropped, so no R5) or game RAM.

**Worst-case cost in the game's working time** (per frame with a notice armed):
- The strip's own hardware work (tables, FIFO) runs only between the arm line and line 191, while the game is halted. On a wake, the abort stops it within one physical block.
- The arming code runs in idle. It touches about 3 I-lines and 1-2 D-lines; NWRAM is presumably uncached in the SDK's protection map (spec., check the game's CP15 region for `0x03000000`). The game refills those lines later: **≤ about 230 cycles**, or about 0 with D7.
- The dispatcher check: section D2. **About 11 cycles per game IRQ** (option a), only while a notice is active.
- One abort per frame at most: **≤ about 60 cycles** (stop A, B and F; one I/O byte read; conditional STRB; ≤ one 4-word block in flight, about 8 bus cycles = 16 ARM9 cycles).
- **Typical X:**
  - with 3-10 game IRQs a frame: about 100-400 cycles a frame, **about 0.01-0.04%**;
  - for every-line raster titles: about 2.1k with option a, or about 0.2k with option b.
- **No module IRQ, ever.** RTZ B3 a-c (entry IRQ on busy frames, the counter's bus writes, IPC arming) and M1/M5 all disappear.

**Bus during the strip** (game halted):
- NWRAM: about 12% of the bus on strip lines;
- the tables: 2 bus cycles a line;
- none of it on main RAM.

R18 and R21 (ARM7 slowed) go to zero if H-NW confirms that ARM9 NDMA NWRAM → ARM9-I/O traffic doesn't hold the main-RAM arbiter. B1's other paths are closed by the guards. Only an IRQ can start game DMAs, and it aborts us first. Hardware-started DMAs already armed are excluded by CNT.31.

**Serves:**
- the allow-list titles (5 candidates, 6 marginal, `allow-list.md`);
- class C, armed from idle-coverage's `SVC_WaitVBlankIntr` wrapper hook. The IRQs still go through the SDK 2.x `OS_IrqHandler`, so the same dispatcher patch applies. The census has Sonic Rush waiting by line 156 on 89%, Super Princess Peach 71%, MPP 70%, RE:DS 54%;
- class D, armed from idle-coverage's poll-loop hook. The loop exits on a VBlank flag set by an IRQ (aborts us first) or on VCOUNT reaching 192 (after our exit). X adds ≤ one block of delay to the loop's final read, about 16 cycles.

**Risks and experiments:**
- H1 / H5 / H-NW: 09h from NWRAM.
- H-TBL: an H-Blank table write to DISPCNT is line-exact and side-effect-free.
- H-ARM: a DMA-written NDMA0CNT starts the FIFO cleanly mid-frame. This is RTZ's H10 variant A without a CPU.
- H-KILL: stopping a running channel.

melonDS can smoke-test the logic: it implements 07h, 09h and immediate modes (MEL `DSi.cpp:52-60`, `DSi_NDMA.cpp:124-135`). It doesn't model BCNT, bus or cache costs. Use the `--thumb-cycle` runner.

### D2. The abort in the game's dispatcher: four options, costed

The dispatcher patch (R4) remains the one game-code change. Its job: on any game IRQ while armed, stop A, B and F, and restore mode 1 if it reads 3, *before* the game's handler.

| Option | Fast path per game IRQ | Abort | Game-visible state | Notes |
|---|---|---|---|---|
| a. **Read channel A's busy bit** | `add r0,r12,#0x4100` (r12 = `0x04000000` in the SDK prologue) + `ldr r1,[r0,#0x38]` + `tst` + `bne`. **About 11 cycles** (an I/O read is 8). No literal pool, so no D-cache line | about 60 cycles | none | **Recommended.** No SCFG bit 8, no IE bit |
| b. IE sentinel `tst` on the IE value the dispatcher already loaded | **about 1 cycle**. At the first IRQ after a shown frame, one I/O read plus one write (about 16) clears the sentinel | about 60 | IE bit 31 readable by the game (R15); needs SCFG_EXT9 bit 8 (R5) | For every-line raster titles: about 200 instead of about 2.1k cycles a frame |
| c. "Kill channel": an unconditional store that starts a pre-configured immediate NDMA3 | **about 2 cycles** + 1-3 DMA words | about 20 | none | NDMA3 must be reprogrammed to a no-op when not armed. Needs one more channel, and CE9 uses 0/1 (NB `cardDma.thumb.c:230,442,462`). Coordinate |
| d. **DSP semaphore** (D4): one buffered store to DSP_PSEM | **about 1-2 cycles** | done by the DSP, in parallel | none | Needs D4 |

**Toggle per notice, not per frame.**
- With no notice active, the check instruction is a `nop`.
- The idle pass rewrites it and invalidates that I-line (C7,C5,1), so the game refetches one line (about 46 cycles) per notice start and end.
- The no-notice cost is about 1 cycle per IRQ for the whole session. That fixes RTZ B3d.

**One remaining caveat.** Abort-on-every-IRQ means a notice can be cut on any frame a non-VBlank IRQ lands between the arm line and 191. That is R19, our notice's continuity, never the game's picture. Keep R19's rules: start only after 30 good frames, and end early to "missed" rather than flicker.

### D3. Raster pass-through (class B)

Every-line H-Blank or VCOUNT titles abort D1 on every frame. In pass-through, the dispatcher aborts only if:
- the IRQ isn't an H-Blank or VCOUNT IRQ (IE bits 1 and 2), or
- the SDK's reschedule path is taken (`OSi_ThreadInfo.isNeedRescheduling` → the thread-switch branch). Patch that branch to abort. It is taken only when a game thread becomes ready.

**Why that is enough:**
- A raster handler that readies no thread leaves the game idle: nothing on the frame's critical path runs.
- Its own work is slowed by at most one FIFO block per bus access, about 16 cycles.
- On strip lines its effect is hidden under the strip. Above E, only the H-Blank table writes coincide, at 2 bus cycles each. The BCNT variant removes even those.

**Precondition:** whole-word writes clobber a raster handler's DISPCNT changes made after `cap`. So D3 applies as is only to titles whose raster keys don't include DISPCNT A. The M1 survey's raster keys per title decide this (`survey.md` method 3). Titles whose handlers write DISPCNT need a byte-wise exit: either the DSP (D4), or a CPU exit IRQ that runs in idle (one IRQ entry plus about 5 lines of cache pollution, about 250 cycles refilled in working time; about 0 with D7).

**Serves** pure-B titles: Animal Crossing, Starfy, M&L BIS, Trauma Center 2, Clubhouse Games, as far as they are idle at E. The census has them at only 36-67% waiting by line 156, so most of them are also class A.

### D4. The Teak co-pilot (DSP sequencer with byte writes)

**Role.** The ARM9 module posts "armed" (E, N, `cap` byte 2, a VCOUNT stamp) to DSP_CMD0 and DSP_PSEM from the idle pass: two buffered stores. The DSP then does the following, using AHBM for every ARM9-side access:
1. **Phase-lock:** read VCOUNT and the DISPSTAT H-Blank flag until the H-Blank edge (a few reads, game halted). From then on its timer counts lines exactly (8520 ticks a line).
2. **At H-Blank E-1:** enable F (or the ARM9 pre-arms F through D1's B), then an **8-bit write** of byte 2 with the mode bits set to 3.
3. **At H-Blank 191:** 8-bit read-modify-write of byte 2, mode bits back to the saved value. A raster handler's changes to bits 18-23 are kept.
4. **Abort,** on the ARM9's DSP_PSEM store from the dispatcher (D2 d): the byte-2 RMW to mode 1 if it reads 3, and `NDMA0CNT = 0`.
5. **Watch:** read byte 2 once around line 200 and once at line 261. A game read-modify-write that wrote mode 3 back after our exit is fixed before line 0 (invisible). The only leak left is a `GX_DispOff` that saved mode 3, which the R3 patch covers.

**What it buys over D1:**
- **byte-exact writes**, so raster titles whose handlers write DISPCNT are served (D3);
- the ARM9 abort costs about 1-2 cycles, with no read;
- game-running strips (D5).

**Cost to the game:**
- AHBM transactions while the game is halted are free.
- After a wake, at most about 3 AHBM transactions (about 6 bus cycles), stalling the ARM9 by up to about 12 cycles once.
- The phase-lock reads are in idle.

**Residuals:**
- SCFG_EXT9 bit 18, SCFG_CLK bit 1 and SCFG_RST bit 0 set in DS mode. The registers are invisible to NTR code; same class as the bit-16 NDMA enable nds-bootstrap already sets.
- Two NWRAM slots (32 KB code in WRAM-B, 32 KB data in WRAM-C).
- DSP power draw, unmeasured. SCFG_CLK can't be gated after the lock; the Teak PMU can shut down modules (G:24852-24873), so the DSP should halt between events.

**Risks:**
- AHBM 8-bit writes to I/O may be done as a 32-bit RMW by the bridge (spec.). H-DSP1 checks bytes 0, 1 and 3.
- AHBM latency is unknown. H-DSP2 checks abort latency.
- Toolchain effort is high.

**A no-patch variant to test, not to plan on:** the DSP polls IF over AHBM instead of receiving a semaphore. The SDK acks IF within about 10-20 cycles of entry, so the poll period would need to be about 0.15 µs (heavy bus use, and a miss means contention in working time). H-DSP3 measures the miss rate; I expect it to fail.

### D5. A bus-free mode-2 strip while the game works (class A, 2D titles with a free bank). Needs D4

**Pixels.** DISPCNT mode 2 shows bank X (A-D) through the display's own VRAM port: no ARM9 bus, no FIFO, no CPU (G:7983-7995, 8162-8166). At H-Blank E-1 the DSP writes byte 2 = (mode 2, block X); at H-Blank 191 it restores the game's byte 2. The strip's pixels live in lines E..191 of bank X, written once per notice in idle (ARM9 or DSP).

**Why this is the one design that can run during the game's work:**
- No ARM9 instruction runs and no ARM9 bus cycle is used, except about 2 AHBM byte writes and 2-3 watch reads a frame (about 10 bus cycles, **≤ about 20 ARM9 cycles of stall per frame**).
- Byte writes don't clobber the game's other DISPCNT bits.

**What it touches:**
- **A VRAM bank** the game has in LCDC mode or disabled and never writes. A disabled bank needs one VRAMCNT write (MST = 0, enable), a game register. If the game later maps the bank, its own write wins, and it uploads before use (spec.). That is R12/R14 territory, owner-ruled, and allow-listed from the M4 bank census.
- **DISPCNT byte 2 reads mode 2 / block X on lines E..191 while the game runs.** A game RMW in that window writes it back. The DSP's watch fixes it before line 0, so it is invisible. `GX_DispOff` in that window would leak, so the R3 patch is needed. Capture with source B = VRAM uses bits 18-19, so stand down whenever capture is enabled.

**Serves** 2D class-A titles: Nine Hours (999) and Cory in the House, plus Layton's busy frames, if M4 finds a free bank (spec.: visual novels and 2D titles rarely use all four of A-D). It doesn't serve 3D titles: their A-D banks are texture-mapped (MST 3).

**Tests:**
- H-M2: a mid-frame mode-2 switch is clean.
- An ARM9 main-RAM loop runs at the same speed with the strip on and off (Δ < 0.05%).
- The DSP watch catches an injected write-back before line 0 in 10,000 trials.

### D6. Capture-bank patch for dual-screen-3D titles (class E)

**The problem.** Engine A alternates screens every frame (POWCNT1.15 flips). The other screen shows last frame's capture through engine B. Capture source A is taken before the display selector (G:8202-8235), so a FIFO strip on engine A shows on screen X only every other frame: 30 Hz flicker.

**Fix, on frames where engine A drives the chosen screen X:**
1. The D1 FIFO strip, as usual.
2. A fourth H-Blank NDMA copies one strip line (128 words, NWRAM → the capture bank's LCDC address of line L) at H-Blank L, for L = E..191. Capture has written line L by then.
   - The cost is about 128 × (1 + 2) = 384 bus cycles per H-Blank; the 16-bit VRAM bus has 2-cycle words. An H-Blank is 594 bus cycles, so it fits, and only while the game is halted.
   - On the next frame engine B shows the patched capture on screen X.

Screen Y is untouched. Both frames on X show the strip.

**What it touches:** writes to the game's capture bank lines E..191 after capture, overwritten by the next capture of that bank. That is game VRAM, owner-ruled. Guards: capture source = A only (bit 29-30 = 0), the bank in LCDC mode at arming, and the bank not used as a source-B read block.

**Serves** E, where E titles are idle at line E. The survey's "Capture en" column identifies them.

**Tests:**
- H-CAP: does capture of line L finish before H-Blank L? If its write is delayed into H-Blank, our pixels may lose. Then copy one line later, at H-Blank L+1, for L = E..190, and give up the strip's last line.
- Pass: the strip on X on 600 consecutive 240 fps frames, and capture lines 0..E-1 bit-identical.

### D7. Cache snapshot and restore (removes the largest item in X)

**The cost it removes.** The module's idle pass (evaluation, network, capture) and our arming code evict the game's lines. The game pays the refills after VBlank: up to 384 × 46 = about **17.7k cycles, 1.6% of a frame** (RTZ M1). That dwarfs every strip cost above, and it exists with or without the display.

**Mechanism** (ARM946E-S CP15 c15 cache debug, G:94723-94745):
1. At idle-pass entry, read the 256 I-tags and 128 D-tags through the Debug Index and Instruction/Data TAG registers. That is about 1-2k cycles, in idle.
2. Run the pass.
3. Before WFI:
   - prefetch the snapshot's I-lines by VA (C7,C13,1, G:94505);
   - touch one word of each snapshot D-line (a plain load: read-allocate).

   About 17k cycles, in idle, only when the pass actually ran. Stop the restore at once on a pending IRQ: a partial restore still beats none.

**Effect.** The game resumes with the cache it had when it went idle, so its refill cost after our pass goes to about 0.
- Dirty game D-lines were cleaned when evicted; that write would have happened at their eviction anyway, and now it happens in idle.
- The replacement pointer state differs (second-order). Under pseudo-random replacement it is irrelevant.

**Game state?** It reads CP15 debug state and game addresses (reads only, which the module already does). It writes nothing of the game's. It is a perf-session lever (it owns `halt_entry`); I list it because it is what makes X small enough for the owner.

**Test H-CR:** a lab ROM with a 6 KB hot loop, then a module-like polluting pass, then the loop's first-iteration time. **Pass:** the restore recovers ≥ 95% of the pollution cost, and the restore itself always finishes before line 191 when started by line 150.

### D8. NWRAM as the strip's home (class F, and D1's source)

- One ARM9-mapped 32 KB slot holds:
  - the 256×32 RGB555 buffer (16 KB);
  - T1 and T2 (1.5 KB);
  - the 8×8 font (2 KB).
- Nothing comes from main RAM, which class F lacks.
- **Conflict:** nds-bootstrap's ROM cache spills into `0x03700000` exactly when main RAM is short (NB main.arm7.c:889-891). Reserve one slot in the bootloader. The CE9 colour-LUT feature already reserves NWRAM (`0x03732800+`), and the two are exclusive in practice (spec.). This needs the perf session's memory plan.
- The **module itself** in class F is a memory-plan problem, outside this angle.

### Rejected, with the reason

| Idea | Why not |
|---|---|
| DMA arithmetic for the DISPCNT RMW (snapshot DISPCNT, use it as an address or index) | A lookup table indexed by a 32-bit register needs about 4G entries. Abusing address counters (write W into SAD, run k words, copy SAD back) would read about 128 KB of arbitrary address space derived from game state, including I/O FIFOs that pop on read, and assumes SAD readback is the live counter (unknown). Dead. D1's capture-in-idle invariant makes it unnecessary |
| Legacy DMA 16-bit write to the DISPCNT high half | Legacy DMA is game state; it would also clobber byte 3 |
| Cache lockdown for our code and data | Per way: a permanent 25% of the game's cache (G:7344-7348) |
| Module code in TCM | Game memory |
| ARM7 as sequencer, renderer or display driver | No ARM9 I/O access; the IPC IRQ is shared; no contention-free handover with MBK locked |
| Camera interface as a timer | Needs a powered camera and I2C; BCNT already does it |
| SCFG_EXT9 bits 3/12/13 | Unknown semantics, NITRO-behaviour changes for the game |
| ARM9 at 134 MHz | Changes the game's lag (gameplay); must be switched from ITCM |
| Backlight or LCD-controller cue | Nonvolatile MCU register; no overlay hardware on a DSi |
| A FIFO strip during the game's work (class A, 3D) | A shared bus (NDMAGCNT note, G:27884-27893): 12% of the bus on strip lines is contention in working time, up to about 20k ARM9 cycles of stall per frame in the worst case. Only H-ML could revive it |

---

## 3. Classes: who is served, by what

| Class | Titles (from brief/census) | Mechanism | Condition | Status |
|---|---|---|---|---|
| Allow list | Digimon, Ghost Trick, Phoenix, Tetris, Yoshi; marginal Kirby SS, NSMB, Peggle, Layton, Sonic Colors, SRA | D1 + D2a (+ D7) | idle at E (census, rerun with `--thumb-cycle`) | served; X ≈ 0.01-0.04% |
| C BIOS wait | Sonic Rush, RE:DS, SPP, MPP, CoM, Canvas Curse, M&L PiT, Pac'n Roll, Ranger | D1 armed from idle-coverage's SWI-5 wrapper hook | the hook exists; the SDK 2.x dispatcher is in the M2 variants | served when idle-coverage ships |
| D busy | Golden Sun DD, Code Lyoko | D1 armed from idle-coverage's poll-loop hook | the hook finds the wait loop; that loop must be a pure wait | served if the hook exists; otherwise no idle signal at all |
| B raster | Animal Crossing, Starfy, M&L BIS, TC2, Clubhouse; KSSU and Black are also A | D3 (whole-word if the raster keys have no DISPCNT A; else D4 byte exit) | idle at E | served where idle at E; most are also A |
| E dual-screen 3D | from survey "Capture en" | D6 + D1 | idle at E; the H-CAP ordering | served if H-CAP passes |
| F DSi-enhanced in DS mode | MvDK MLM, Pokémon Black/White | D8 for the strip | NWRAM slot reserved | strip served; module memory is another session's problem |
| A busy at the bottom, 2D | 999, Cory, Layton (busy frames) | D5 (DSP + mode 2) | free bank (M4), D4 working | served if both hold |
| **A busy at the bottom, 3D** | **Mario Kart DS, Diddy Kong Racing; KSSU and Black (also B)** | none at zero cost | see section 4 | **not served on DSi at zero**; LED + menu |

**The combination that covers the most:** D1 + D2a + D7 as the base, D3, D6, D8, then D4 + D5 as the phase-2 experiment. Together with idle-coverage's hooks, that serves every class except 3D class A.

---

## 4. The class I can't serve, with evidence: 3D titles busy through the bottom lines

**Census (melonDS, optimistic by about 2× on Thumb).** Share of frames already waiting by line 168 / 176 / 184 (CEN `frames.csv`, frames after 300):

| Title | 168 | 176 | 184 |
|---|---|---|---|
| Mario Kart DS | 29% | 46% | 66% |
| Diddy Kong Racing | 39% | 39% | 39% |
| KSSU | 0% | 0% | 0% (22% by 188) |
| Pokémon Black | 0% | 0% | 0% (0% by 188) |

MKDS is halted 57-65% of its frame overall, but in about 8 halts a frame (28,633 halts / 3,499 intervals, CEN `mkds/report.txt`, `sites.csv`). Its idle is fragmented mid-frame, and its last wait starts at lines 160-191.

**Each lever tried:**
1. **FIFO strip during work** (mode 3): the FIFO DMA shares the ARM9 bus with the CPU. GBATEK's NDMAGCNT describes one arbitration among NDMA, ARM9 and DSP (G:27884-27893). From NWRAM it takes about 12% of the bus on strip lines. A 4-word block holds the bus for about 8 bus cycles, and the CPU stalls whenever it misses cache or touches I/O during one: **nonzero, up to about 20k ARM9 cycles a frame** for N = 32. That isn't zero. Only H-ML (a multi-layer bus) could change this.
2. **Mode 2 (VRAM display), bus-free:** needs a bank A-D in LCDC mode or free. 3D titles map their banks as texture slots (MST 3), and the display can't read a bank mapped as texture (G:8162-8166: "VRAM must be allocated to LCDC"). Capture into a texture bank is equally unavailable.
3. **Engine B:** no FIFO, no VRAM display (G:8198); only the game's own layers, OBJ, VRAM and palettes. That is P4, for blank sub screens only, and racing titles show maps there.
4. **A strip during the game's mid-frame halts:** its position would move frame to frame. A notice that jumps or blinks is a visual compromise.
5. **Shorter strips:** MKDS is waiting by 184 on only 66%. Notices would drop out (R19) on a third of frames. KSSU and Black finish at 188-191.
6. **The DSP or ARM7 can't show pixels;** only engine A and B outputs reach the LCDs.
7. **ARM9 at 134 MHz would create idle time,** but it changes the game's lag. Rejected (section 1.6).

**Two outs, both cheap to check:**
- **(i) A per-line halted census:** P(halted at line L) for L in 0..191, plus what each halt waits on (legacy DMA CNT.31, GXSTAT.27). If MKDS or DKR are reliably halted with no DMA on lines 0..31, a **top-edge** D1 serves them. NWRAM makes "halted on GX or ARM7" safe (closing B1), and the owner's placement setting allows a top edge with a fallback (DL 11.2). Not in the current census; it is idle-coverage's runner's job.
- **(ii) H-ML.** If the DSi's ARM9 side is a multi-layer bus, so that an NWRAM → I/O NDMA doesn't stall CPU main-RAM traffic, the FIFO strip can run during work using D4's byte writes.

Side hint for the perf session, unverified: in the census, 41% of MKDS's *running* samples, 23% of DKR's and 16% of 999's fall in `0x023BF640-0x023BF6BF`. That is the nds-bootstrap-area VCOUNT poller the census flagged (`idle-census.md`, "side finding"). If that is a loader-side busy wait, not game work, part of the "busy at the bottom" may be ours and convertible to a halt. Check it before declaring class A lost.

---

## 5. Experiments (pass marks set now)

**Hardware, display-lab ROM** (our own ROM, so its timers are ours):

| # | Question | Pass |
|---|---|---|
| H-NW | 09h from NWRAM; ARM7 contention | Banner clean on lines E..191 in 600 frames. An ARM7 main-RAM loop and an ARM7→ARM9 PXI round trip are within 0.1% with the strip on vs off (main-RAM source recorded for contrast) |
| H-TBL | H-Blank table writes of DISPCNT | Mode 3 starts at line E and ends after 191 on 10,000/10,000 frames (VCOUNT log plus photos). Game-pattern lines 0..E-1 are pixel-identical with the table running and the FIFO off |
| H-ARM | DMA-armed FIFO mid-frame (B writes NDMA0CNT at H-Blank E-1) | Banner from line E with 0 underrun pixels in 600 frames, with no CPU at entry. Failure kills D1's CPU-free entry (then use one entry IRQ in idle) |
| H-KILL | Stop latency | After `NDMAxCNT = 0`, busy reads 0 within 100 ARM9 cycles. A main-RAM loop from the abort to line 191 matches baseline within 0.1% (RTZ H11) |
| T1 | Slack probe with D1 + D2a | The smallest k with zero lag-frame difference, strip on vs off, in the CPU, GXFIFO-DMA and PXI variants: Δk ≤ the owner's X |
| H-CR | Cache restore (D7) | ≥ 95% of the pollution cost recovered; the restore never crosses line 191 when started by 150 |
| H-ML | Multi-layer bus? | An uncached main-RAM read loop runs at the same speed (Δ < 0.1%) with the NWRAM FIFO on. Expected to fail; a pass revives the FIFO strip during work (class A) |
| H-DSP1 | AHBM 8-bit write to `0x04000002` | Only byte 2 changes (bytes 0, 1, 3 read back unchanged) in 10^6 writes. AHBM VCOUNT reads are correct. Teak-timer line lock drifts < 1 dot in 600 frames |
| H-DSP2 | Abort latency (an ARM9 DSP_PSEM store → mode-1 write) | < 1 µs at the 99.99th percentile over 10^5 trials |
| H-DSP3 | No-patch IF polling | Report the miss rate. Expect it to fail, and don't plan on it |
| H-M2 | Mode-2 strip while the CPU works | A clean edge at E and 191 in 600 frames. An ARM9 loop runs at the same speed (Δ < 0.05%). Injected game write-backs are fixed before line 0 in 10^4/10^4 |
| H-CAP | Capture-line timing (D6) | Our line L pixels survive in the captured bank for L = E..191 in 600 frames. Otherwise switch to L+1 copying and rerun |

**melonDS** (logic only, `--thumb-cycle` runner, coordinated through emu-lock):
- D1's state machine and tables in DSi mode (07h, 09h, immediate are modelled; BCNT isn't);
- D4 on teakra (AHBM is mapped);
- **Pass:** M7's existing marks (0 pixel differences outside the strip, the same lag count, equal `captures_stopped` and `frames_stale`).

melonDS can't decide any cost question (RTZ M8).

---

## 6. Asks of other sessions

**idle-coverage (it owns the ARM7 and the new hooks):**
1. Arm-point callbacks: from the `SVC_WaitVBlankIntr` wrapper hook (C) and the poll-loop hook (D). The module must be able to run D1's arming (about 200 cycles) before the wait.
2. ARM7 flags in ARM9-readable module RAM, needed only for the main-RAM fallback or if H-NW fails:
   - WM active (POWCNT2.1);
   - save/SD I/O in flight (CE7);
   - ARM7 busy on a PXI command.
3. A per-line halted census: P(halted at L), with legacy DMA CNT.31 and GXSTAT.27 sampled at each halt. That decides the top-edge out for class A.
4. ARM7 halted % per title, to confirm the ARM7 has room for the camera-LED I2C writes.

**perf:**
1. The D2 dispatcher patch: option a in the SDK `OS_IrqHandler` variants (survey M2: 2 variants cover 96%), toggled per notice.
2. The D7 cache snapshot and restore in `halt_entry`.
3. Reserve one ARM9 NWRAM slot (D8), plus two DSP slots if D4 goes ahead.
4. NDMA channel allocation versus CE9: F = NDMA0 and B/A below it; CE9 stands down while armed.
5. The `0x023BF640` hot spot (section 4).

**The owner** rules on:
- the reduced R4 (D2a);
- D4's SCFG bits and power draw;
- D5's VRAM bank use and VRAMCNT write;
- D6's capture-bank writes;
- whether X ≈ 100-400 cycles a frame (measured by T1) counts as zero.
