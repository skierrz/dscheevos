# Proposal: cooperate with the game's own SDK (round 2, angle "SDK")

Proposer: Opus. Read-only: no repo edits, no emulator runs. One new number was computed from existing census data (section 0.3).
Source keys:
- **CEN**: idle-coverage `docs/idle-census.md`, plus `private/census/out/<game>/frames.csv`;
- **SUR**: `docs/display-research/survey.md`;
- **HW**: `hardware.md`;
- **DL**: `display-library.md`;
- **RTZ**: `red-team-zero-lag.md`;
- **PA**: `prior-art.md`;
- **G**: GBATEK (`(local scratch)`);
- **HOOKS**: `external/nds-bootstrap/retail/bootloaderi/source/arm7/dscheevos_hooks.c`;
- **START**: `dscheevos/module/src/start.s`.

Claims about NitroSDK/NNS internals that I can't check here (there's no SDK source in the tree) are marked **(spec.)** and come with a check. Each check is a disassembly of a census ROM.

---

## 0. The idea, and the three facts that make it worth doing

The strip (P2b) fights the game for **time**: it can only show while the game is idle at the bottom of the frame. It also fights for a **register**: DISPCNT byte 2. That is why it reaches only 5 of 22 titles (allow-list.md).

This proposal instead puts our indicator **inside the picture the game already composes**, as hardware sprites (OBJs). It finds the slots and the moment from the game's own SDK, by signature, the way nds-bootstrap already finds `OS_Halt` (HOOKS:54-69) and the IRQ-enable function (`cardenginei/arm9/source/cardengine.c:1733` `myIrqEnable`).

### 0.1 A sprite costs no time while it is up
Once an OAM entry and its pixels are in place, the 2D engine draws them every frame by itself:
- no CPU, IRQ, NDMA or FIFO;
- no main-RAM bus: OBJ pixels are fetched from VRAM, not main RAM.

So the strip's problems that come from per-frame timing don't apply:
- B1/R21: the ARM7 slowed by the FIFO's main-RAM traffic;
- B3: arming and IRQ costs;
- R19: dropouts on frames where the game is busy;
- the abort on raster IRQs.

We only spend CPU when the indicator's **content changes** (an unlock, a progress tick). That CPU time can come from any idle slice, at any line.

### 0.2 Sprites are composited, not substituted
Our sprite needs no DISPCNT write, no mode switch and no palette. **Bitmap OBJs** (OAM attr0 mode 3) carry 15-bit direct colour plus an alpha in attr2, so palette RAM is never touched (G:8059-8075, "Bitmap OBJs are 15bit Direct Color data").

Bitmap OBJ mapping reaches 128 KB of OBJ VRAM in every mapping mode the game can pick, and 256 KB only in 1D-256 (G:8059-8066). So we never need to know or change the game's tile-mapping choice: we compute the tile number from its DISPCNT bits 5, 6 and 22, read-only.

### 0.3 The OBJ line budget protects the game
OBJs are evaluated in OAM order, and the per-line rendering budget is fixed (G:1471-1481, with the caution at G:1483-1487). An entry at the **highest indices** is processed **last**. If a line runs out of OBJ cycles, it is our sprite that loses pixels, never the game's.

DS priority is the 9-bit value {BG priority, OAM index} (G:8037-8043). At BG priority 0 and index 127, our sprite sits above every BG layer and the 3D layer, and below the game's own priority-0 sprites where they overlap.

**Idle slices exist in almost every frame.** I recomputed this from CEN frames.csv: frames after the first 300, with `halted` ≥ 4000 cycles at 133 MHz, enough to write 8 OAM entries and check them.

| Group | Share of frames with an idle slice |
|---|---|
| Most of the 64 ROMs | 90-99% |
| Mario Kart DS | 83% |
| Kirby Super Star Ultra | 95% |
| Pokémon Black | 98% |
| Animal Crossing | 71% (90% in the 8k run) |
| Diddy Kong Racing | 14-48% |
| Golden Sun DD | 0% |
| Code Lyoko | 0.2% |

Three of these are classes the strip loses:
- **Mario Kart DS** (class A, busy at the bottom) still has an idle slice on 83% of frames.
- **Kirby SSU** (class B, raster IRQs) has one on 95%.
- **Pokémon Black** (class B) has one on 98%.

The strip loses them because the idle time isn't **at the bottom**. A sprite doesn't care where in the frame the idle time falls.

---

## 1. Mechanisms, ranked

| Rank | Mechanism | Pixels? | Cost in game working time | Touches game state | Classes |
|---|---|---|---|---|---|
| **1** | **S2: persistent sprite in OAM entries the game never writes, pixels in an SDK-unassigned VRAM bank** | yes, any corner, both screens | **0 per frame**; content updates run in idle | OAM entries outside the game's write set; VRAMCNT of an SDK-unassigned bank | A, B, C*, E*, F; D only for display (module time is the limit) |
| **2** | **S1: per-frame append after the game's own OAM transfer, into entries the game itself just wrote as hidden** | yes | **bounded per frame**, about 150-1000 cycles (≤0.09% of a frame), measured in T1 | the same OAM entries (overwritten by the game every frame); pixels as in S2, or game OBJ VRAM (residual R-V2) | A, B, C, D, E*, F, for games whose OAM goes through SDK transfer functions |
| **3** | **H-T: thread-state "frame done" signal** (read-only `OSi_ThreadInfo`, plus an optional `OS_WaitIrq` mask stash) | no (timing signal for every presenter) | 0 (read-only, from idle); about 10-20 cycles per `OS_WaitIrq` call if the mask stash is used | reads game RAM only | fixes RTZ B1/B2 for the strip; tells S2 when to write |
| **4** | **H-D: display-control event hooks** (`GX_SetDispSelect`, `GX_DispOff/On`, `GX_SetGraphicsMode`, `GX_SetBankFor*`, master brightness) | no | about 10-30 cycles per call (scene changes and per-frame swaps) | none written; game code patched (as with `OS_Halt`) | E (swap tracking), the strip's R2/R20, S2's bank give-back |
| 5 | C-W: coroutine inside the BIOS wait wrapper (`SVC_WaitVBlankIntr`, 64/64 ROMs, CEN) | no (gives C games CPU) | dispatcher check plus a stack swap on the cut frame, about 50-100 cycles | none written | C (the SDK-cooperative form of idle-coverage's hook) |
| ✗ | T: our own thread in the SDK scheduler (`OS_CreateThread`) | — | equal to the idle hook at best | **writes the game's thread list** | none it doesn't already have; useless for C and D |
| ✗ | N: a sound cue through NNS_Snd / SND | audio | in working time | the game's sound state (players, channel allocator, command buffer) | — (and RA asks for *visible* indicators, PA §5) |
| ✗ | G3: append a textured quad before `G3_SwapBuffers` | yes (3D games) | geometry time in working time | geometry state that can't be restored (write-only POLYGON_ATTR, TEXIMAGE_PARAM, DIF_AMB) | rejected unless a game resets all state each frame |

\* C needs the module to get CPU at all (idle-coverage's wrapper hook, or C-W). E needs H-D for per-frame engine tracking, plus a check of the capture source (section 2.5).

---

## 2. S2: persistent sprite (rank 1)

### 2.1 How pixels reach the player
1. **Pixel bank.** Find a VRAM bank that is:
   - (a) unassigned in the SDK's own VRAM record (2.3);
   - (b) disabled in VRAMCNT readback (HW §0.3; hardware test H4);
   - (c) mappable into an OBJ address window that no game OBJ bank covers and that bitmap OBJs can reach.

   Candidates (G:7507-7510, 7525):

   | Engine | If the game's OBJ VRAM is... | We map | Result |
   |---|---|---|---|
   | A | bank E (64 KB at `06400000`) | F or G at OFS.1=1 (`06410000` or `06414000`) | 16 KB, reachable (< 128 KB) |
   | A | only F/G | the other 16 KB slot | |
   | A | bank A or B (128 KB at offset 0) | nothing in bitmap reach, except 1D-256 bitmap mode with A/B at OFS 1 (rare) | **no bank route** |
   | B | no bank at all (sub OBJ unused) | bank I (16 KB) | works only if the game left DISPCNT_B bit 12 on; else residual R-B12 |
   | B | anything | bank D or I, fixed at `06600000` | no offset choice: never overlap |

   **Never overlap a game bank.** melonDS ORs overlapping banks (spec. for hardware; H4 extension), so overlap would corrupt the game's sprites.
2. **Mapping.** One STRB to `VRAMCNT_x` (MST=2, OFS as above), from idle.
3. **Render.** Render the indicator straight into the bank as bitmap pixels:
   - double-buffered: 2 × 8 KB (128×32, or 256×16 each);
   - no main-RAM framebuffer, so about 0 extra RAM, which suits **class F**.
4. **OAM.** 1 to 4 entries at the highest free indices:
   - attr0: mode 3 (bitmap), shape;
   - attr1: x, size;
   - attr2: tile number from the game's bitmap mapping (read-only DISPCNT), priority 0, alpha 15.
5. **Write timing (no tearing).** Write OAM and pixels only while VCOUNT is outside the sprite's rows, from any idle slice (H-T says when). A content change flips the buffer with one 16-bit attr2 write, so there is no half-drawn glyph.
6. **Then nothing.** The sprite persists with zero per-frame work. A challenge icon or a progress counter stays up for minutes at zero cost.

### 2.2 Proving the OAM entries are not the game's (bookkeeping, not inference)
Bakkerrrs' fork failed because it *inferred* "free" from the current picture (PA §1; HW §2.4). S2 instead takes an entry only if **every OAM writer the game has is a known SDK function**, and none of them covers that entry:
- **`GX_LoadOAM` / `GXS_LoadOAM`** (offset, size) (spec.: `GXi_DmaCopy32` to `HW_OAM + offset` / `HW_DB_OAM + offset`). We find them by literal `0x07000000` / `0x07000400` plus shape, and hook them at boot. Each call adds [offset, offset+size) to a per-engine "game-written" mask kept in module memory.
- **NNS OAM managers** (spec.: `NNS_G2dGetNewOamManagerInstance(pMan, fromIdx, num, type)` reserves a range, and `NNS_G2dApplyOamManagerToHW` writes exactly that range). We hook instance creation to record each range; the apply path ends in the hooked load functions anyway.
- **Everything else** (direct CPU stores, `MI_CpuFill` of OAM at scene init, game-specific DMA) is invisible to hooks. That is why the census (E1) must show, per title, that **100% of OAM writes after boot come from the hooked PCs**. Titles that fail are not S2 titles; they can still be S1 titles (S1 tolerates this) or use another mechanism.

At runtime we take an entry only if all of these hold:
- the mask has been clear for it for ≥ 600 frames;
- it currently reads "disabled" (attr0 bit 9 set, bit 8 clear);
- the title passed E1.

Every idle pass compares our 4 entries (8 words) with what we wrote. If the game has written one, we have a **conflict**: we drop the entry for the session and never write it again.

**The conflict rule protects the game, except in one pattern.** A game write always wins, because we never re-assert. The one exception is a game that **un-hides an init-time entry with a partial write**: it sets only attr0 and expects attr1/2 from init. To cover it we keep the 8 original bytes of every borrowed entry. On a conflict, we put back the original attr1/attr2 for fields the game didn't change, on the next idle pass.

**Residual R-O1.** At most one frame of one game sprite at our position or tile, and only in that pattern. E1 counts partial OAM writes to tell whether the pattern exists at all.

### 2.3 Proving the bank is free (SDK bookkeeping)
NitroSDK keeps the current per-purpose bank assignment (spec.: `GX_GetBankForBG/OBJ/TEX/...` and `GX_ResetBankFor*` exist and return it, so the state is in game RAM next to `GXi_VRamLockId`; find it by disassembling `GX_SetBankForOBJ`).

A bank is free when all three hold:
- it appears in **no** purpose's mask, LCDC included (a bank the game uses as LCDC holds staging or capture data);
- VRAMCNT readback agrees (catches games that write VRAMCNT directly);
- E1 shows the bank disabled for ≥ 95% of play.

**Give-back.** We hook `GX_SetBankFor*`, `GX_SetBankForLCDC` and `GX_Disable*` (H-D). On entry, if the game is about to take our bank, we:
1. restore the bank's original 16 KB (saved from the bank when we took it; it was disabled, so it held whatever the game parked there);
2. disable our mapping;
3. clear our OAM entries;
4. then let the call proceed.

That costs about 16 KB / 4 B × ~4 cycles ≈ 16k cycles, about 1.5% of one frame. It happens once, inside a scene change. **Residual R-V1**: a bounded one-time cost in working time at a bank re-assignment.

To drop it: if E2 shows no title ever re-enables a bank expecting the old contents, take only banks whose saved contents we needn't restore. M4 (HW) measures "parked data in disabled banks".

### 2.4 What the game can observe (S2)
| Observable | When | Size |
|---|---|---|
| Our values in OAM entries it never writes | only if it **reads** HW OAM (E1 counts OAM reads by PC) | 4 entries |
| VRAMCNT of a bank its SDK says is unassigned | only by reading VRAMCNT (officially write-only, G:7475-7483) or by checking content of a bank it disabled | 1 byte |
| A partial-write un-hide shows our attr1/2 | the R-O1 pattern only | ≤ 1 frame |
| Blending / windows | if BLDCNT makes OBJ a 1st target, or a window excludes OBJ, **our** sprite blends or hides; the game's picture is unaffected | ours only |
| Master brightness | our sprite fades with the game (the same as the strip) | — |
| OBJ line budget | ours drops first (0.3) | ours only |
| Code bytes | boot-time hook patches, the same class as the accepted `OS_Halt` patch (HOOKS:54-69) | — |

Nothing the game draws is damaged, outside R-O1's one frame. No state the game depends on changes, given that it never reads OAM it doesn't write, or banks it doesn't own.

**This still needs an owner ruling**: the brief counts game-owned OAM and display registers as game state. The case for accepting it is that these entries and this bank are ones the game's own SDK records show it has not claimed.

### 2.5 Classes
- **A (busy at bottom):** served. There's no timing dependence. MKDS has idle slices on 83% of frames (0.3); updates may lag a few frames during its longest dry run (314 frames, likely loading), but the shown content persists.
- **B (raster IRQs):** served. No IRQ interaction at all. The exception is a game that multiplexes sprites by H-Blank OAM DMA (E1 flags OAM writes during lines 0-191).
- **C (BIOS wait):** display served. Content updates need any CPU slice, from idle-coverage's wrapper hook or C-W.
- **D (busy, never halt):** the display isn't the limit. S2 needs CPU only per **change**, not per frame. The module needs CPU to evaluate at all (section 7).
- **E (POWCNT1.15 flips):** engine A's sprite follows engine A to whichever LCD it drives. The H-D hook on `GX_SetDispSelect` (POWCNT1 `0x04000304`) tells us the swap order exactly.
  - **Single-screen 3D games** (no flip): served directly; OBJs compose over BG0 3D.
  - **Dual-screen 3D:** the "chosen screen" is drawn live by engine A on alternate frames. In between, engine B shows engine A's **capture** of that frame. If DISPCAPCNT source A = "graphics screen" (bit 24 = 0, which includes OBJ), our engine-A sprite is captured and re-shown by engine B automatically, with no flicker. If the capture is 3D-only, we need a matching engine-B entry toggled per frame through the swap hook: **S1-style per-frame cost**, about 50 cycles. E1 records DISPCAPCNT per title.
- **F (DSi-enhanced in DS mode):** served. About 0 main RAM, since pixels live in the borrowed bank.

### 2.6 Experiments (S2)
- **E1 (melonDS census, `--thumb-cycle` runner)**: extend the survey's IOHook (SUR §1) to OAM `07000000-070007FF`, VRAMCNT, and the SDK VRAM state. Per title, record:
  - every OAM writer PC and whether it is a hooked SDK function;
  - per-entry write masks;
  - OAM reads;
  - partial-entry writes;
  - bank enable state;
  - the game's OBJ bank windows;
  - DISPCNT bits 4-6, 12 and 20-22;
  - DISPCAPCNT bit 24 on flip games.

  **Pass**: on ≥ 50% of the 27 titles that reached play in M1, there are ≥ 4 entries never written after boot, no OAM reads, and a bank meeting 2.1's rules, on ≥ 95% of frames. **Kill**: < 20%. That would leave S2 a niche and promote S1.
- **E3 (DSi display-lab ROM)**:
  - a stand-in game using bank E for OBJ with 127 sprites, plus our bitmap OBJ at index 127 from G mapped at `06410000`;
  - capture both screens with and without ours (DISPCAPCNT into a free bank);
  - also: VRAMCNT readback (H4), overlap behaviour (map G over E on purpose: OR or priority?), and the line budget with a full line.

  **Pass**: 10,000 frames, game pixels outside our rectangle byte-identical, and under budget overload only our pixels missing.

---

## 3. S1: per-frame append after the game's transfer (rank 2)

S2 needs entries the game *never* writes. Many games write all 128 every frame: a full `GX_LoadOAM(shadow, 0, 1024)`, or one NNS manager over 0-127 (spec.: the usual sample setup). Their *unused-this-frame* entries are still provable from their own data, because the game itself just wrote them as **disabled** in this frame's transfer.

### 3.1 Pixels
Our post-hook on `GX_LoadOAM` / `GXS_LoadOAM` runs after the copy completes. If the SDK used an async DMA, we poll that channel's CNT.31, which the game itself would have to wait for before touching the buffer. The hook:
1. scans attr0 from index 127 downward for disabled entries (the game's NNS manager also exposes `numUsed` read-only (spec.), which bounds the scan);
2. writes our ≤ 4 entries (attr0/1/2) into the top disabled ones.

The game's transfer runs in VBlank (from its VBlank callback or right after `OS_WaitVBlankIntr`), so our entries are in place before line 0: no tearing. Pixels come from the S2 bank if there is one. Otherwise they go into game OBJ VRAM (residual R-V2 below).

### 3.2 Game state and why it may be acceptable
- **OAM** entries the game declared unused **for this frame**, and fully rewrites next frame. A taken entry never outlives the frame in which the game declared it unused. So partial-write damage (R-O1) **can't happen**, because the game rewrites whole entries every frame.
- **Observable** only if the game reads HW OAM after its transfer (E1).
- **R-V2, if there is no free bank.** The pixels would have to go into game OBJ VRAM that no OAM entry referenced during the last N frames. That is inference, the failure Bakkerrrs hit. I recommend **not** doing this. Without a bank, S1 should fall back to the next mechanism rather than borrow game VRAM. Allow it only as a per-title opt-in built from E1 traces of a full playthrough.

### 3.3 Cost in working time (bounded)
| Part | Cycles (ARM9, 67 MHz) |
|---|---|
| Trampoline in and out | about 10-20 |
| Scan, typical (top entries free) | about 4 × 4 |
| Scan, worst case (128 attr0 reads at OAM wait states) | about 600 |
| Write 4 entries (12 halfwords) | about 40 |
| Cold I-cache (3-4 lines) and D-cache misses | about 100-300 |
| **Bound** | **≤ about 1000 cycles = 0.09% of a 1,120,380-cycle frame**; typical about 200 |

The bound is made hard with a cap: the scan stops after 32 entries. If none was found, nothing is shown this frame.

- **Flicker risk.** On frames where the game uses every entry, our indicator would vanish. Mitigation: show only after 120 frames in which the top 4 entries were always free; if that is ever lost during a notice, end the notice (into the menu's missed list) rather than flicker.
- **Measure X on hardware (RTZ's T1 slack probe).** **Pass**: hook worst case ≤ 1000 cycles cold, and zero lag-frame difference at slack k ≥ that over 10,000 frames.

### 3.4 Classes
- **A, B:** served (VBlank-time work, independent of the bottom lines and of raster IRQs).
- **C:** served for display without idle-coverage, because the hook runs inside the game's own transfer.
- **D:** served for display if Golden Sun DD and Code Lyoko call the SDK OAM load functions (E2 checks); Golden Sun DD's TwlSDK has them (spec.).
- **E:** as S2, through H-D.
- **F:** served.

The universality limit is the share of games whose OAM goes through SDK functions (E1), and the bank question for pixels.

---

## 4. H-T: an exact "frame done" from the SDK's thread state (rank 3)

This helps the time angle, and every presenter.

**The problem.** RTZ B1/B2: "the idle thread is running" ≠ "the frame is done". The main thread may be blocked on a PXI reply, a DMA, or a worker.

**The SDK records why each thread sleeps**, in game RAM, read-only. `OSi_ThreadInfo` holds `current` and the thread list. Each `OSThread` has `state` and `queue`, the queue it sleeps on (spec., NitroSDK `OSThread` layout).
- **Finding it:** `OS_IrqHandler`'s V1/V2 literal `ldr lr,=OS_IrqHandler_ThreadSwitch` (SUR §5) leads to the switch code, whose literal pool holds `&OSi_ThreadInfo`. That covers 43/45 titles with the survey's two signatures.
- **The frame-done test, from idle at zero cost:** every game thread is WAITING, and the thread that last called the VBlank wait sleeps on `OSi_IrqThreadQueue`.
  - Threads asleep on any other queue (FS/CARD, SND, PXI callbacks) mark the frame "**not done**". That is exactly RTZ B1's case.
  - Add the cheap I/O checks RTZ already lists: DMA CNT.31, GXSTAT.27, and the CE7 save/wireless flags.
- **Optional mask stash.** `OS_WaitIrq` (spec.: `OS_WaitVBlankIntr` = `SVC_WaitByLoop(1); OS_WaitIrq(TRUE, OS_IE_V_BLANK)`) doesn't store its mask (RTZ B1). A 3-instruction entry hook stores `(thread, mask)` in **module** memory. That costs about 10-20 cycles per call, once or a few times per frame, in working time.
- **Signature by call graph, not bytes.** `SVC_WaitByLoop` is an asm wrapper (`svc 3; bx lr`, the same family as CEN's 64/64 `svc 5` wrapper). Its caller that next calls a function with `r0=1, r1=1` is `OS_WaitVBlankIntr`, and that callee is `OS_WaitIrq`.
- **What it gives:**
  - the strip gets a sound entry test (replaces RTZ B2's stale flag);
  - S2 learns safe moments to write;
  - the census gains a per-frame "done line" that is truer than `wait_line`.
- **E4:** in melonDS, log the classifier at every idle entry, against ground truth (no non-IRQ game code runs between "done" and the next VBlank). **Pass**: 0 false "done" over all census gameplay runs; "done" on ≥ 90% of the frames where the census shows a wait.

---

## 5. H-D: display-control hooks (rank 4)

**Patch points** (each a short C function found by its I/O literal):

| Function | I/O literal | Event |
|---|---|---|
| `GX_SetDispSelect` | POWCNT1 `0x04000304` bit 15 | the swap |
| `GX_DispOff` / `GX_DispOn` | `0x04000000`, with the SDK's saved display mode (spec.: `GXi_DispMode`) | display off/on |
| `GX_SetGraphicsMode` | `0x04000000` | mode change |
| `GX_SetMasterBrightness` / `GXS_SetMasterBrightness` | `0x0400006C` / `0x0400106C` | fades |
| `GX_SetBankFor*` | `0x04000240` | bank re-assignment |

**What each hook does:**
- **E (screen flips):** we learn on which frame engine A drives which LCD, at the moment it is decided. No polling, no inference from POWCNT1 reads (RTZ M7).
- **Strip R2/R20:** while the strip is live, a `GX_DispOff` pre-hook sees mode 3 in DISPCNT and saves the game's mode 1 into `GXi_DispMode` instead. **That writes game RAM**, so it needs a ruling. It is better done as "end the strip before the call proceeds", which writes nothing.
- **S2 give-back** (2.3).
- **Notice gating:** don't start notices during fades or display-off.

**Cost:** about 10-30 cycles per call. Most are scene-change calls; `GX_SetDispSelect` is per frame only in flip games. **State:** none written (code patch only).

---

## 6. Ideas examined and rejected

### 6.1 Our own thread at the lowest priority (`OS_CreateThread`)
- **Scheduling.** NitroSDK runs the idle thread at `OS_THREAD_PRIORITY_MAX + 1` (= 32) and game threads at 0-31 (spec.). The SDK doesn't time-slice equal priorities.
  - **At 31**, our thread ties with any game thread at 31 (background loaders do use 31; E1 can count them). Once running, ours would keep the CPU while such a game thread became ready. That delays the game, which **violates the lag rule**.
  - **At 32** (set by hand after creation), it behaves exactly like the idle thread: it runs only when every game thread waits, and it is preempted on the IRQ return that readies one. That is **equivalent to the `OS_Halt` hook we already have** (START:163-214), with no gain.
- **State it writes.** `OS_CreateThread` links into the game's thread list (`OSi_ThreadInfo` list pointers, the thread-ID counter). Observable effects:
  - `OS_GetNumberOfThread` changes;
  - IDs of later game threads shift;
  - thread-switch callbacks (`OS_SetSwitchThreadCallback`) run for our thread.

  That is a direct **game-RAM write**, so it needs a rule change for no benefit.
- **Class C: no help.** In BIOS-wait games the main thread halts **inside SWI 5** in its own context. The scheduler still considers it running, so a lower-priority thread never runs (spec.).

  Rewriting the wrapper into `OS_WaitIrq(TRUE, VBLANK)` would let *other* lower-priority game threads run during the wait, which they don't in stock. That changes the game's behaviour (load progress, timing). Reject.
- **Class D: no help.** Busy engines never block, so nothing below them runs.
- **Verdict.** It is not cleaner for C or D. For C the cooperative form is **C-W**: a coroutine on the module stack, entered from the hooked `SVC_WaitVBlankIntr` wrapper (64/64 ROMs, CEN).
  - The patched `OS_IrqHandler` (R4) checks, on IRQ return, whether the interrupted PC is in the coroutine and the VBlank check flag is set. If so, it parks our context and returns to the wrapper's caller as the SWI would.
  - Cost: the same class as RTZ B3e (about 50-100 cycles on frames where our work is cut).
  - The thread-switch path needs the same check. This belongs to idle-coverage, and I flag it only as the SDK-shaped way to do it.

### 6.2 Sound cue through NNS_Snd / SND
- **It is game state.** NNS players, handles and the sound heap are in game RAM. `SND_LockChannel` takes a voice from the game's sequence allocator. Commands go through the game's shared command buffer and PXI.
- **The ARM7-side variant is also game state.** It would drive a hardware channel that the ARM7 SND driver's state (read-only) marks inactive. That proves "free now", not "free for 0.5 s". The driver may start a voice on it mid-cue: ours is cut, so the game isn't damaged. But the mixer sum can clip loud game audio.
- **It doesn't meet RA's requirement.** RA requires Measured/Trigger indicators to be *visible* (PA §5).
- **Verdict.** Not an indicator. At most an opt-in unlock chime, below the DSP chime (HW §2.12).

### 6.3 Others
- **A 3D quad before `G3_SwapBuffers`.** The geometry state can't be restored (write-only registers), and it adds geometry time in working time. Reject unless E1 shows a title re-sets all of its state every frame.
- **The MPU as an OAM write trap.** The minimum region is 4 KB, so every game OAM write would fault. It also writes CP15. Reject.
- **Writing our sprite into the game's shadow OAM buffer before its transfer.** That writes game RAM. Reject. S1 achieves the same after the transfer, writing only HW OAM.

---

## 7. Signature coverage

**Evidence:**
- **CEN SDK column:** 64 ROMs, **33 distinct SDK builds** across **12 minor versions**:

  | SDK | Minor versions |
  |---|---|
  | 2.x | 2.0, 2.1, 2.2 |
  | 3.x | 3.0, 3.1, 3.2 |
  | 4.x | 4.0, 4.1, 4.2 |
  | 5.x | 5.3, 5.4, 5.5 |

  The spread is 2.0/2.1/2.2 = 3/8/3, 3.x = 17, 4.x = 22, 5.x = 11, from the first four hex digits of the build field.
- **Hand-written asm functions are universal:**
  - `OS_Halt` is matched by one 3-word pattern (HOOKS:54-69) in all 53 OS_Halt games;
  - the `SVC_WaitIntr` / `SVC_WaitVBlankIntr` wrappers have one Thumb shape in **64/64** (CEN).
- **`OS_IrqHandler`** has **4 shapes in 45 titles** (SUR §5):

  | Variant | Titles | Note |
  |---|---|---|
  | V1 | 36 | |
  | V2 | 7 | |
  | V3 | 1 | Diddy Kong |
  | V4 | 1 | Golden Sun DD, TwlSDK |

  V2 appears only in SDK 4.0-4.2 games, but other 4.2 games (KSSU, Pokémon Platinum's peers) are V1. **Shape follows build options (ARM/Thumb, compiler), not the SDK version.** So expect a few shapes per C function, not 33.
- **C functions** (`GX_LoadOAM`, `OS_WaitIrq`, `GX_SetDispSelect`, `GX_SetBankFor*`, NNS OAM manager) need **anchored** signatures, not byte hashes:
  - the I/O literal they write (`07000000`, `07000400`, `04000304`, `04000240`, `0400006C`);
  - call-graph position (callers of the universal `svc 3`/`svc 5` wrappers; the handler's ThreadSwitch literal);
  - a function size bound;
  - **runtime confirmation**: arm a hook only after it has fired with the expected register write once (cheap, done from idle).

  A function that isn't found turns that mechanism off for that game: there is no damage, and the game goes to the next mechanism.
- **Where the SDK code lives.** SDK and NNS code is in the static ARM9 binary for nearly all games (spec.; E2 confirms). An overlay-resident copy would need re-patching on `FS_LoadOverlay`.

**E2 (static scan plus melonDS trace, all 64 ROMs):** locate each target. **Pass**: exactly one match per engine in ≥ 95% of ROMs, and the trace shows that PC performing the expected write. **The bar per function**: ≥ 95% → build; 80-95% → build plus a per-title table; < 80% → that hook is dropped.

---

## 8. What covers 100%, and what can't be covered

**Combination:**

| Step | Mechanism | Applies to |
|---|---|---|
| 1 | **S2** | titles passing E1: no per-frame cost, no flicker, no timing class matters |
| 2 | **S1** | titles where the SDK load functions carry all OAM writes but every entry is written each frame; per-frame cost ≤ about 1000 cycles, ruled once via T1 |
| 3 | **The strip P2b** with H-T's entry test | titles with no spare bank (bank A/B OBJ, all F/G/I used) that pass the strip's own allow list |
| 4 | **P4** | blank sub screens |
| 5 | **LED + menu** | the rest |

H-D runs alongside all of these, for E and the gating.

**What I believe can't be served on-screen within the rules as written** (each needs the stated ruling or evidence):
1. **Titles with no free OBJ-mappable bank, no spare OAM pattern provable from SDK writers, raster IRQs or busy bottom lines (strip out), and a non-blank sub screen.** Every lever fails here:
   - S2: no pixels without game VRAM;
   - S1: same;
   - strip: timing;
   - P4: no blank screen.

   The remaining lever is game OBJ VRAM (R-V2), which is inference and was proven harmful on hardware (PA §1, Ketsui). Size of the class: unknown until E1. My expectation is that many 2D games that map bank A/B to OBJ land here.
2. **Class D for anything at all.** The module has no CPU in Golden Sun DD (0.0% of frames with an idle slice) or Code Lyoko (0.2%). S1 could *display* from the OAM-load hook, but there is nothing to display without evaluation time. Evaluating needs a per-title hook into the engine's own scheduler or poll loop (idle-coverage). It is not a display problem.

**Owner rulings this proposal needs:**
- **R-O1 / S2 OAM**: our values in OAM entries the game's SDK never writes; ≤ 1 frame in the partial-write pattern.
- **R-V1**: a one-time bank give-back cost (about 16k cycles) at a scene change.
- **S2 VRAMCNT**: one byte of VRAMCNT for a bank the SDK records as unassigned.
- **S1 OAM**: per-frame use of entries the game wrote as disabled this frame.
- **X_S1**: up to about 1000 cycles per frame in working time.
- **Code patches**: on SDK functions (precedent: `OS_Halt`).

Each is a smaller footprint than the strip's (one DISPCNT byte on 32 lines every frame, NDMA, FIFO bus, an IRQ path), and none touches main RAM.

**Order of work:** E2 (static, cheap), then E1 (census), then E3 (lab ROM), then T1 for S1. If E1 kills S2 (< 20%), S1 alone still serves A-E, for games whose OAM goes through SDK functions, provided a bank exists. If banks are the binding constraint, report the bank-less class to the owner with E1's per-title bank table as the hard evidence.
