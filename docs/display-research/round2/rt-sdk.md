# Red team: prop-sdk.md (S2 / S1 / H-T / H-D)

Read-only. Keys as in the proposal, plus:
- **P**: prop-sdk.md line numbers;
- **mDS**: tools/melonDS/src.

Speculation is marked **(spec.)**. Severity: blocker / major / minor.

## Verdict
S2's headline claims ("provably never writes", "0 per frame", "nothing the game draws damaged", "ours drops first") don't survive as written.

S2's persistence is its strength and its main hazard. A persistent OAM entry outlives any game state change we don't hook:
- display capture;
- VRAMCNT;
- the DISPCNT bitmap-mapping bits.

When that happens it shows garbage, or it leaks our pixels into the game's VRAM. The OAM conflict rule protects OAM only.

S1 is the sounder half, because it re-derives everything once per frame. Both still need fixes B1, M1 and M2 before E1/E3 are worth running.

---

## Blockers

### B1. Our sprite is captured into the game's VRAM (S2 and S1)
**Evidence:**
- DISPCAPCNT source A bit 24 = 0 means "Graphics Screen BG+3D+OBJ" (G:8146-8160).
- Capture writes to a bank A-D in LCDC mode, which is game VRAM.
- survey.md:325: **18 of 45 titles enable capture**. Clubhouse, Peggle, Pac'n Roll and LEGO SW do so every frame. **Golden Sun and Pokémon Black blend A+B** with source B = VRAM.
- The strip never had this problem: main-memory display output isn't in source A. S2 introduces it.

**Consequences:**
- Our pixels are written into game-owned VRAM, which breaks a hard rule.
- In an A+B feedback blend (motion blur), our indicator smears and leaves trails that decay over several frames after we remove it. (spec.: depends on whether source B reads the previous capture)
- A game that captures a frame as a pause or transition background bakes our indicator into its own picture.
- P:174 treats capture only as a benefit for class E.

**Fix:**
- E1 records DISPCAPCNT per frame: bits 24, 29-30, 31 and size.
- For capture sizes up to 256x128 (only the upper-left is captured, G:8158-8159), place our sprite at lines 128-191.
- For 256x192 graphics captures, S2/S1 are off and the title goes to the next mechanism.

**Test:** E3 with the lab "game" capturing source A into bank C while our sprite is up. Pass: the bank C contents are byte-identical with and without our sprite.

### B2. Persistence turns unhooked VRAMCNT/DISPCNT changes into on-screen garbage (S2)
**Evidence:**
- Our OAM entry stays live with zero per-frame work (P:115).
- If the game changes the bitmap mapping (DISPCNT bits 5, 6, 22; G:7940-7946, 8057-8066), our tile number is reinterpreted. Example: tile 256 under 1D-256 is `0x10000` (our bank F). Under 1D-128 it is `0x8000`, inside the game's bank E. The game's tile data is then drawn as 15-bit direct colour over its picture.
- The same happens if VRAMCNT changes through any path we don't hook:
  - direct register writes;
  - a 32-bit store to `0x04000244`, which covers E, F, G and WRAMCNT (G:7475-7483);
  - an inline setter.
- Garbage lasts until our next idle pass. That could be MKDS's 314-frame idle drought, or forever in Golden Sun DD (0% idle).
- The conflict rule (P:128) only watches OAM.
- **(spec.)** `GX_SetOBJVRamModeBmp` is a header inline, so H-D's table (P:263-271) can't catch it. See M9.

**Fix:**
- Pick T so that every mapping interpretation lands in our bank or in unmapped VRAM. With the game's OBJ in E, T=512 gives:
  - 1D-128 → `0x10000`;
  - 2D → `0x10000`;
  - 1D-256 → `0x20000`, which is unmapped and reads as transparent.
- That only scrambles our own pixels, so a per-frame check of DISPCNT and the bank is still needed, run in VBlank, which costs time and needs timing. Or prefer S1, which re-reads DISPCNT every frame.

**Test:** E3 switches the bitmap mapping mode and remaps E/F mid-run. Pass: game pixels outside our rectangle are identical, and no non-game colour appears anywhere.

---

## Major

### M1. "Ours drops first" holds only if every enabled game OBJ has a lower index than ours
**Evidence:**
- Budget: 1210 OBJ cycles per line, or 954 with H-Blank-Interval-Free set (G:1471-1474).
- Cost: a normal OBJ costs n cycles; rotation/scaling costs 10+2n (G:1476-1481).
- Order: evaluation runs in OAM order. GBATEK's own text ends with an open "?" (G:1483-1490), so this is unverified.

**Where it breaks:**
- S2 takes the "highest free indices" (P:110), and S1 takes the "top disabled" entries (P:204). If the game uses fixed high slots (for example a HUD at 120-127), our entries sit *before* game sprites in evaluation. They then use up budget, so **game sprites drop**. That is damage.
- Size: four 64-px-wide entries cost 256 cycles, which is 21% of 1210 or 27% of 954.
- Even at index 127, a dense line truncates our sprite. The truncation varies frame to frame, which is flicker on our indicator.
- melonDS has no OBJ cycle budget at all: GPU2D_Soft.cpp has no per-line cycle accounting. E1 can't see this problem; only E3 can.

**Fix:**
- Take entries only above the highest index the game ever enabled. S1 checks this every frame and refuses if a game entry is enabled above ours.
- Place the indicator on rows with low game OBJ load, computed from OAM in E1.

**E3 must measure** whether bitmap OBJs cost the same as normal OBJs, and confirm the evaluation order.

### M2. OAM attr3 holds the game's affine parameters
**Evidence:** the 4th halfword of every entry is PA/PB/PC/PD of affine group n/4 (G:1500-1520, 1616-1622). Yet the proposal compares "8 words" and restores "8 original bytes" per entry (P:128-130).

**What goes wrong:**
- A game using affine group 31 writes attr3 of entries 124-127 every frame. That causes false conflicts, so those four entries are never held.
- If we ever write or restore attr3, we corrupt the game's rotated sprites.

**Fix:**
- Read, write and compare attr0-2 only.
- Track affine ranges separately (NNS managers have their own affine range; spec.).

### M3. "Provably never writes" is observation plus a guard, not proof
**Evidence:**
- E1 sees writers only during census play. hardware.md:163 says "None of these proves future non-use."
- Unhooked paths:
  - SDK-demo-style scene init `MI_CpuFillFast` / `MI_DmaFill32(HW_OAM, 0xC0..)` (spec.);
  - inline `G2_SetOBJ*` on a `HW_OAM` pointer (spec.);
  - H-Blank DMA multiplexing;
  - raw DMA from game code.
- The guard keeps full-entry writes safe, but has two problems:
  - Dropping the entry "for the session" (P:128) means the first scene-change OAM clear ends S2 for the whole session, which is a coverage collapse.
  - The R-O1 restore (P:130) can't tell "the game wrote the same value" from "unchanged". It may overwrite a deliberate game write that the game never repeats, which is **lasting damage, not one frame**.

**Fix:**
- Re-acquire after N clean frames.
- Restore only fields whose write a hook observed, or drop the restore and accept losing the entry.

### M4. The bank rules rest on a write-only register, and the "≈0 RAM" claim is wrong
**Evidence:**
- VRAMCNT is (W) in GBATEK (G:7475-7483).
- melonDS returns VRAMCNT on read (NDS.cpp:2849-2858), so it can't stand in for H4.
- If H4 fails, rule (b) at P:92 is gone. Only the SDK record is left, and it is blind to direct writes. Overlap then means OR-ed output, i.e. damage to game sprites (spec. on hardware).

**Coverage:**
- A game with A/B as OBJ VRAM has no route (P:101).
- Engine B works only with sub OBJ unused and DISPCNT_B.12 already on.
- Ketsui-style layouts have no free bank (prior-art.md §1).

**RAM and time:**
- Give-back (P:142-148) needs a **16 KB save buffer**, which contradicts "≈0 extra RAM, suits F" (P:109, 175).
- The ~16k-cycle restore runs inside the game's own `GX_SetBankFor*` call. Measure it for lag frames.

### M5. "0 per frame" is false
The `GX_LoadOAM`/`GXS_LoadOAM` hooks that maintain the written-mask (P:119) fire every frame, at about 10-30 cycles per call. B1 and B2 add per-frame checks on top.

S2 should be stated as a bounded per-frame cost and measured in T1 like S1.

### M6. Game display state hides ours, or makes it skip the game's fade
**Hidden:** our sprite is hidden by any of these:
- DISPCNT.12 off;
- engine A in display mode 2 or 3;
- windows whose WININ/WINOUT OBJ bit excludes it;
- an OBJ window.

A Trigger indicator that disappears while the challenge is still active is a compliance gap. E1 must log DISPCNT 12-15, WININ/WINOUT and BLDCNT.

**Skips the fade:** bitmap OBJs are handled like semi-transparent OBJs:
- in melonDS, flags 0xC0 are set at GPU2D_Soft.cpp:1202/1376 and take the sprite-blend path at :60-75, which also ignores the window's effect bit;
- for semi-transparent OBJs, GBATEK says the brightness effect is skipped (G:1273-1280);
- hardware for bitmap OBJs is spec.

So during a BLDY fade with a 2nd target under us, **our sprite stays at full brightness while the game fades to black**. P:159 says it fades with the game, which is true for MASTER_BRIGHT only.

**Test:** E3 runs a BLDY fade.

### M7. Dual-screen 3D (P:174) is wrong in both directions
- **Graphics-source capture:** our sprite shows on **both** LCDs, the live one plus the re-shown capture. Keeping it on one screen needs a per-frame toggle synced to the swap. That is an S1-style write in VBlank.
- **Engine A in VRAM-display mode on alternate frames:** we get 30 Hz flicker.

E1 must record which dual-3D technique each title uses before E is called "served".

### M8. H-T's "frame done" test is unsound as written
**(spec., NitroSDK behaviour)**
- `OS_WaitIrq` sleeps on a single `OSi_IrqThreadQueue` for every IRQ mask. Without the mask stash, "on the IRQ queue" does not mean "waiting for VBlank".
- Long-lived workers sleep on message queues indefinitely: the CARD task thread, loaders, sound. So "every game thread is WAITING and on the IRQ queue" is rarely or never true, and "done" never fires.
- The fix needs per-thread roles: identify the main thread, and know which sleeps are benign.
- `OSThread` layout differs across 2.0-5.5 and TWL.

**On reading game RAM:**
- Reading is within the rule; rcheevos already does it.
- Reads made while IRQs are enabled can be torn.
- Cache lines our reads pull in evict game lines. That small refill cost lands in the game's working time; bound it.

### M9. H-D and the signature coverage claims are optimistic
**Inline setters (spec.):** many GX setters are `static inline` header functions:
- `GX_SetDispSelect`;
- `GX_SetMasterBright`;
- `GX_SetOBJVRamModeBmp`;
- `GX_SetVisiblePlane`.

They have no function body to hook, and their writes are scattered through game code. Check: count literal-pool references to `0x04000304` / `0x0400006C` per ARM9 binary. More than one site per engine means the setter was inlined. This is the same failure mode as Bakkerrrs' FF3 swap bug (prior-art.md §1).

**Coverage extrapolation:** "shape follows build options" (P:342) generalises from `OS_IrqHandler`, a hand-tuned path with 4 variants, to C functions built by several CodeWarrior versions in ARM and Thumb. E2 must test this directly.

**ITCM code:** ITCM-resident code (the Animal Crossing idle loop, per the census) needs patching after autoload.

---

## Minor

**m1. Idle share.** I reproduced it: at ≥4k cycles, MKDS 83.1%, Black 98.4%, KSSU 94.7%, GSDD 0%, and the numbers barely move at ≥200k. But:
- it is the frame's *total* halted time, not one contiguous slice;
- the census isn't a `--thumb-cycle` run, so it is optimistic;
- an 8 KB bitmap redraw must be split into chunks across slices.

**m2. Torn entry updates.** One entry is 3 halfword writes made from idle with IRQs enabled. An IRQ can split them until the raster reaches our rows, which gives one frame of a half-updated entry. Fix: write attr1 and attr2 while attr0 is hidden, and attr0 last.

**m3. S1's VBlank cost.** S1's ≤1000 cycles fall in the VBlank window: about 71 lines × 4260 cycles ≈ 302k cycles at 67 MHz, so about 0.3% of it. A game whose VRAM uploads end near line 0 could start tearing. Measure the end line of the game's VBlank work.

**m4. Screen swaps.** A swap we don't hook moves our indicator to the other LCD, possibly over the touch UI. Bakkerrrs saw this in FF3.

**m5. Code patches.** The hooks are writes to game code in RAM. The `OS_Halt` patch is the precedent, but list each hook in the owner ruling.

## Priority experiments
1. E3, adding: capture with ours up (B1); a mapping and bank switch (B2); line-budget order and bitmap OBJ cost (M1); a BLDY fade (M6).
2. E1, adding: DISPCAPCNT; DISPCNT bits 5, 6, 12-15 and 22 per frame; the highest enabled OAM index per frame; attr3 writes; the dual-3D technique.
3. A static inline-site count for the H-D literals (M9).
