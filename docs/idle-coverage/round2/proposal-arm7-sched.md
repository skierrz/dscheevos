# Round 2 proposal: where and when the ARM7 evaluator runs (lane 5)

Angle: the ARM7 entry point, preemption, NWRAM slots and their handover, the hand-off with the ARM9 capture,
boot/load droughts, main-RAM bus contention, and the throughput each set needs. Nothing here was built and no
emulator was run. Evidence is code reading (file:line), GBATEK (local copy `scratchpad/dl/gbatek.txt`, line
numbers), static analysis of the owner's 64 ROMs done for this proposal (scripts and outputs in
`scratchpad/design2/`), and arithmetic over existing census data. **[estimate]** and **[guess]** mark what is not
measured.

Scope: rnd owns the data gather (how the coherent per-frame copy is made), eval-speed's ARM7 lane owns the ARMv4T
build and its CPI, and the busy-engine capture belongs to other proposals. This proposal states what it needs from
them (section 4.6).

---

## 0. Verdict

1. **Entry point: keep nds-bootstrap's existing ARM7 halt hook, and run the evaluator W7 as a coroutine parked on
   every IRQ.** While W7 runs, the ARM7 BIOS IRQ pointer at `0x0380FFFC` is swapped to our handler. That is the
   ARM7 twin of round 1's settled DTCM+3FFC park (decision.md §2.3). With the park:
   - every game ARM7 IRQ reaches the game's handler with the game's main thread as the interrupted context, at
     the same logical point as stock (in the main loop's halt);
   - the game's post-halt work and every SDK thread switch happen exactly as stock;
   - the added cost per IRQ is only the park latency X7, about 115-230 ARM7 cycles **[estimate]** (3.4-6.9 µs).

   Static finding: in 59 of 64 ROMs, the site nds-bootstrap hooks is the SDK ARM7 component's main loop:
   `for(;;){ SVC_Halt(); reset check; 2-3 periodic polls; }`. The remaining 5 have a Thumb ARM7. The park makes
   round 1's "lowest-priority loop" precondition (A7-M0) unnecessary: the only precondition left is "the caller
   has IRQs enabled (CPSR.I=0, IME=1)", which is checked at run time.
2. **Card reads need no CE9 change** (round 1's forced `b_pingIpc` is dropped). With the park, a pending ARM9 card
   request is served at the same moment as in stock: the next ARM7 IRQ, then the main loop re-enters the hook,
   which runs `runCardEngineCheckHalt` first.
3. **NWRAM: the slot set is fixed at boot, never at run time.** After nds-bootstrap clears SCFG_EXT9 bit 31, no CPU
   can change MBK1-5 (GBATEK 24311, 27745-27754). So:
   - one evaluator per session, picked at boot (ARM7 *or* ARM9). A mid-session switch is impossible without
     rc_client's progress serialization, which is a compliance question (Q-C1);
   - the ARM7's slots are the whole 32 KB B/C slots above the FNT/FAT pre-cache, using the free-space rule
     eval-speed already wrote (`nwramGrant`, nwram worktree, nds-bootstrap commit efe545d5);
   - ARM7-master slots are invisible to the ARM9, its DMA/NDMA and the DSP (GBATEK 27841-27849). No game ARM7
     binary of the 64 holds a pointer into 0x03700000-0x037BFFFF (static scan, 0 hits in 64/64);
   - median free ≈ 480 KB, but Golden Sun DD only ≈ 128 KB (366 KB FNT/FAT), and ROM-in-RAM or colour-filter
     sessions get none.
4. **Hand-off goes through main RAM.** No memory except main RAM is visible to both CPUs at once (ARM7-master NWRAM
   is zero-filled to the ARM9):
   - the ARM9 publishes immutable frame records into a staging ring (uncached or cleaned, write buffer drained,
     then `head`);
   - the ARM7 pulls each record with LDM bursts into its NWRAM queue and advances `tail`.
   So rnd's "copy in NWRAM" must be read as "a copy in main RAM that the ARM7 pulls": an ARM9 or NDMA9 write cannot
   reach an ARM7-master slot.
5. **Droughts:**
   - In census data, once the game's frame loop runs (first ARM9 halt), the worst ARM7 backlog in 8 games is
     ≤ 12 frames for any per-frame cost c ≤ 400k ARM7 cycles.
   - The only long droughts are at game boot, before the first ARM9 halt: 121-603 frames with zero ARM7 halt.
     They drain in ≤ 1,795 frames (30 s) at c = 325k and ≤ 3,482 frames (58 s) at c = 400k.
   - At c = 465k, MKDS, Black and SoulSilver never drain within the run.
   - Rule: the ARM7 evaluator is chosen only for c ≤ 325k (hard cap 400k). The staging ring must hold the boot
     drought (≥ 1,024 records, repeat-compressed; as today).
6. **Bus contention is not zero, but its minimum is small and known.** The ARM7's main-RAM traffic is the record
   pull plus ≤ 2 control reads per frame:
   - about 260 bus cycles per frame for MKDS, ≈ 2.6k for SoulSilver and ≈ 2.9k for TWEWY-size records
     **[estimate]**;
   - that is 0.05-0.5% of a frame's 560k bus cycles, an upper bound on what the ARM9 can lose to it;
   - for waiting games the pull is gated to the ARM9's wait. It can't be proven zero, because game DMA may run
     during the wait.
   For comparison, ARM9-side evaluation costs ~100k ARM9 cycles per frame in cache refills (brief). The ARM7 has no
   cache, so W7 leaves no after-effect on the game's ARM7 either.
7. **Throughput (ARM7 cycles per frame, melonDS, hot code in WRAM, data in main RAM; arm7-notes.md):**
   - Kirby SSU 164.5k fits (35% of the 465k p10 budget).
   - These do not fit: MKDS 650k (140%), Black 933k (200%), Digimon 3,014k (650%), the synthetic typical set 873k.
   - Converting the round-1 instruction table: the ≤ ~95k-instruction sets fit (Ghost Trick, 999, TC2, Drawn to
     Life, Sonic Rush, Pac'n Roll, KSqS, PvZ; Cory at the 400k cap).
   - RE:DS (~690k est.) and Golden Sun DD (~720-770k est.) need the ARM7 cost cut ≥ 1.8-1.9x. Moving the parsed
     set to NWRAM is the lever, and it is unmeasured.
   - **For the busy class this is the binding gap: the ARM7 is its only evaluator, and today GS DD's set does not
     fit.**

---

## 1. Facts this rests on (new this round unless marked)

### 1.1 The ARM7 hook site is the SDK component's main loop (static, 64 ROMs)

- nds-bootstrap's finder (`findSwiHaltOffset`, bootloaderi/arm7/find_arm7.c:328-363):
  - looks for the word 0x04000004 (or 0x04000208) in the first 4 KB of the ARM7 binary;
  - then scans back 0x40 bytes from +0x20 for `ldr ip,[pc]` (0xE59FC000; 0xE59FC004 for SDK < 2.8).
  - `patchSwiHalt` (patch_arm7.c:159-200) overwrites 12 bytes there with `j_newSwiHalt` (`ldr pc,=newSwiHalt`).
- I replicated the finder (`design2/a7site.py`) and disassembled every hit (devkitARM objdump, `design2/a7scan/`):
  - **59/64:** the site is an interworking veneer `ldr ip,[pc]; bx ip; .word T`. T is the ARM7's only Thumb
    `swi 6; bx lr` (SVC_Halt).
    - Kirby SSU: veneer at image 0x02380660, T = 0x038037D3.
    - The autoload table at image 0x023A6F10 maps file 0x1E8.. to 0x037F8000, so T is file offset 0xB9BA. That is
      exactly where the only `06 DF 70 47` sits.
  - Every one of the 59 has a caller of the form `bl veneer; bl f1; cmp r0,#0; beq; bl f2; bl f3; bl f4; b <the bl>`.
    That is a forever loop around the halt (`design2/site_roles.tsv`).
    - Kirby SSU image 0x02380614-0x02380638 (runs from WRAM at 0x037F842C).
    - MKDS 0x02380788-0x023807A4.
    - Kirby's loop body:
      - f1 = 0x02385F9C returns a flag at [0x03807702] (reset requested);
      - f2 = 0x02385FE0 stops DMA 0-3 etc. (reset path);
      - f3 = 0x0238E8D8 and f4 = 0x0238B8A4 each compare a counter against a deadline +10 (periodic polls).
      Their exact purpose (power/battery/cartridge checks) is a **[guess]**.
  - The second caller of the veneer in each ROM is an `OS_Terminate`-style loop: `bl OS_DisableInterrupts; bl veneer;
    b` (Kirby 0x02386058-0x0238606C). It halts with IRQs disabled.
  - **5/64 have no ARM site:** Club Penguin EPF, EPF Herbert's Revenge, Diddy Kong Racing DS, Kirby Canvas Curse,
    Kirby Mass Attack. Their main loops are Thumb (`f2 e7 c0 46` before the DISPSTAT word).
    - nds-bootstrap hooks them only through `findSwiHaltThumbOffset` (find_arm7.c:365-417). That needs
      `a7IsThumb` and a free `newSwiHaltAddr`, which the save patcher provides
      (save_patch_universal.c:249, 288-291, 423).
    - Whether these 5 are hooked is **unverified** (nds-bootstrap log, S-A7-2).
- Every one of the 64 ARM7 binaries has exactly one Thumb `swi 6; bx lr` (scan in `design2/`).
- Why the main loop is the lowest-priority runnable context in SDK games:
  - `SVC_Halt` stops the CPU, not the thread, so the main thread never blocks;
  - any thread with lower priority than main would never run in a stock game;
  - all other ARM7 work (SND, SPI/touch, PXI, WM) therefore runs in higher-priority threads or IRQs.
  This reasoning is an inference from the loop shape **[guess for every SDK variant]**. The park design below
  doesn't depend on it.

### 1.2 ARM7 halt and IRQ entry (GBATEK)

- SWI 6 (NDS7) halts until (IE AND IF) ≠ 0. "The state of CPUs IRQ disable bit in CPSR register, and the IME register
  are don't care" (gbatek.txt:95586-95596; HALTCNT 13696-13704).
- The ARM7 IRQ vector jumps to `[3FFFFFCh aka 380FFFCh]` (gbatek.txt:7274-7275). That handler must be ARM code
  (12876-12881). `380FFF8h` holds the IRQ check bits for IntrWait (12883-12890). We do not touch it.
- ARM7 timings (33 MHz units, gbatek.txt:7620-7640):
  - main RAM data N32 = 10, S32 = 2 (16-bit bus);
  - WRAM/BIOS/I/O 1/1 (32-bit);
  - ARM9 cache-line miss from main RAM = 23.
- NWRAM timing for the ARM7 is not in GBATEK. melonDS charges it like WRAM, 32-bit, 1/1 (NDS.cpp:273, region
  0x03000000-0x03FFFFFF) **[guess for hardware]**.
- The ARM7TDMI has no cache, TCM or CP15 (round-1 proposal-arm7 §1.1, gbatek.txt:7611-7613). So code the ARM7 runs
  leaves no cache state behind that the game would pay for afterwards. This is unlike the ARM9's ~100k-cycle refill
  (brief).

### 1.3 NWRAM ownership and its lock (GBATEK, nds-bootstrap)

- MBK1-5 (bank → slot, master) are read-only on the ARM7 side and writable only by the ARM9, depending on MBK9
  (gbatek.txt:27745-27754).
- ARM9 access to any SCFG/MBK register needs SCFG_EXT9 bit 31 (24311).
- nds-bootstrap clears that bit for DS-mode games at `ARM9_SETSCFG`, right after `transferToArm9(i)` for all 16 B/C
  banks (bootloaderi/arm9/main.arm9.c:428-436). The ARM7 locks its side at main.arm7.c:2691-2692.
- **So the slot→CPU assignment is frozen for the whole session.**
- A slot that is disabled or mapped to another CPU reads as zero-filled memory (gbatek.txt:27841-27849). So an
  ARM7-master slot is invisible to the ARM9, its DMA/NDMA, and the DSP.
- Boot mapping:
  - `initMBK` (main.arm7.c:227-256) makes every bank ARM7-master and maps C at 0x03700000-0x0373FFFF and B at
    0x03740000-0x037BFFFF (256 KB image mirrored over a 512 KB window) on the ARM7;
  - the ARM9 uses the same B/C windows (main.arm9.c:78-95). So **one image linked at 0x0370xxxx-0x0377xxxx runs on
    either CPU**;
  - WRAM-A is mapped on the ARM7 only (MBK6 = 0x080037C0).
- WRAM-A is unusable (round-1 red-team B1, confirmed):
  - the SDK ≥ 2.8 "VRAM WiFi binary" autoload is moved to 0x037C0000 (patch_arm7.c:405-426). In Kirby SSU that
    segment is 0x17878 + 0x1968 bss bytes at image 0x027E0000, so 0x037C0000-0x037D91E0 at run time
    (autoload table, `design2/`);
  - the cheat engine sits at 0x037DC000 and CE7 at 0x037E0400 (61 KB) (locations.h:82, 88);
  - ROM file descriptors at 0x037EFFA0 (locations.h:9);
  - the game's own ARM7 WRAM code at 0x037F8000+.
- What nds-bootstrap puts in B/C in DS mode, all of it read by the **ARM9** (CE9):
  - FNT/FAT pre-cache at 0x03700000, up to 0x80000 (0x32800 with the colour LUT), only when not ROM-in-RAM
    (main.arm7.c:1358-1370, 2593-2595);
  - ROM-in-RAM spill when `romLocation` wraps to 0x03700000/0x036F8000 (main.arm7.c:889-890, 912-929);
  - colour LUT at 0x03770000 and its engine at 0x03732800 (main.arm7.c:2021-2033; locations.h:103).
- eval-speed already computes the free part for its ARM9 heap zone:
  - `nwramGrant` (nwram worktree, nds-bootstrap efe545d5, dscheevos_hooks.c) grants
    `0x03700000 + roundup1K(FNT/FAT)` .. 0x03780000;
  - only with DSi WRAM access, no mirror, no colour LUT, no ROM-in-RAM, no shared-WRAM ROM, no ROM-map piece below
    0x04000000, and not for U/HND/HNE titles.

### 1.4 Free NWRAM per ROM (static estimate, `design2/nwram_fit.tsv`)

FNT/FAT size from each header ((fatOff−fntOff)+fatSize). ROM-in-RAM mode is estimated against
`romSizeLimit = 0xBE0000 − DSCHEEVOS_CACHE_RESERVE + 0x88000` (main.arm7.c:912-929; locations.h:137;
dscheevos_hooks.h:18).

| | count (of 64) | ARM7-usable B/C (whole 32 KB slots) |
|---|---|---|
| cache mode, FNT/FAT ≤ 64 KB | 46 | 448-512 KB (14-16 slots) |
| cache mode, FNT/FAT 64-230 KB | 16 (Animal Crossing 185K, Digimon 195K, 999 188K, Scribblenauts 220K, Layton 138K, …) | 288-416 KB |
| cache mode, FNT/FAT 366 KB | 1 (**Golden Sun DD**, busy class) | **128 KB (4 slots)** |
| ROM-in-RAM with NWRAM spill **[estimate]** | 1 (Clubhouse Games, 9.4 MB) | 0 |
| colour filter on (any game) | — | 0 (nwramGrant refuses) |

### 1.5 ARM7 free time and droughts (census data, `design2/backlog.py`)

- Data: `~/Developer/DScheevos/private/census/out/<run>/frames.csv`, column `arm7_halted`. These are the 8 runs
  arm7-notes.md describes: `--thumb-cycle`, achievements off, Wi-Fi off; melonDS figures are upper bounds.
- I simulated a queue: `backlog += c + O − free` per interval, floored at 0, with O = 5k overhead.

| run | intervals | zero-ARM7-halt runs ≥ 30 | after the first ARM9 halt: max backlog (frames) at c = 165k / 325k / 400k / 465k | whole run: max backlog; frames to drain at c = 325k / 400k |
|---|---|---|---|---|
| Kirby SSU | 2,999 | 126 (boot) | 9 / 10 / 11 / 12 | 129; 394 / 681 |
| MKDS | 3,499 | 603 (boot) | 2 / 3 / 3 / 3 | 616; 1,795 / 3,482 |
| Sonic Rush | 2,499 | 121 | 0 / 1 / 1 / 2 | 123; 318 / 538 |
| Platinum | 13,249 | 451 | 4 / 7 / 8 / 9 | 465; 1,267 / 2,034 |
| Black | 9,499 | 249 | 11 / 11 / 12 / 44 | 253; 766 / 1,826 (465k: never) |
| Digimon | 6,999 | 310 | 0 / 0 / 0 / 1 | 316; 866 / 1,586 |
| TWEWY | 8,249 | 150 | 2 / 3 / 5 / 6 | 153; 406 / 644 |
| SoulSilver | 8,249 | 504 | 8 / 9 / 9 / 10 | 520; 1,514 / 2,766 (465k: never) |

- Every long drought is at game boot: the ARM9 has not halted yet (MKDS frames 1-603: ARM9 halted 0 and ARM7
  halted 0).
- During those frames both CPUs are probably busy with loads: the ARM9 spins on CE9's card request, and the ARM7
  serves it with a blocking `fileRead` in `runCardEngineCheckHalt` (cardenginei/arm7/cardengine.c:1799-1840)
  **[guess]**. SD timing in melonDS is not the console's, so hardware decides.

---

## 2. Entry point and preemption: W7 parked on every IRQ

### 2.1 Shape

- Code: two lines in CE7's `newSwiHalt` (card_engine_header.s:445-450; the Thumb variant at 455-466 the same way),
  plus about 1 KB of gate/park assembly in CE7 (WRAM-A, CE7's 61 KB link region).
- W7 (module7: rc_client + the core's evaluation half, ARMv4T build by the ARM7 lane) lives in the ARM7's NWRAM
  slots with its own stack.

```
newSwiHalt:                     @ existing, ARM
    push {lr}
    bl   runCardEngineCheckHalt @ unchanged: ARM9 card requests first, as stock
    bl   dsc7_gate              @ NEW: r0=1 -> an IRQ was taken while W7 ran: the halt is over
    cmp  r0, #0
    swieq #0x060000             @ stock halt when W7 did not run (or yielded)
    pop  {pc}

dsc7_gate:                      @ CE7, ARM, caller context (the game's thread)
    if (CPSR.I || mode not in {SYS,USR} || !(IME & 1) || !w7.runnable || w7.disabled) return 0
    mask I
    if (IE & IF) { unmask; return 0 }           @ a wake is already pending: stock path
    save caller r4-r11, sp, lr, cpsr -> hk7
    hk7.game_vec = [0x0380FFFC]; [0x0380FFFC] = irq_head7      @ swap ON  (I1-7)
    SPSR = w7.cpsr (SYS, I=0); load W7 regs; movs pc           @ resume W7

irq_head7:                      @ IRQ mode, I=1, BIOS frame {r0-r3,r12,lr} on SP_irq (game's IRQ stack)
    [0x0380FFFC] = hk7.game_vec                                @ swap OFF first
    save W7: r4-r11; r0-r3,r12 and pc=lr-4 from the frame; cpsr=SPSR; SYS sp/lr (via a masked mode switch)
    SP_irq += 24                                               @ drop the frame: depth as stock
    restore caller r4-r11, SYS sp/lr from hk7
    SPSR = hk7.cpsr (I=0); r0 = 1; movs pc -> gate_ret         @ the IRQ is still pending (IF set):
gate_ret: bx lr  (reached after the game's handler ran)        @ it is taken again at once through game_vec,
                                                               @ with the caller at gate_ret as the interrupted
                                                               @ context: what a stock halt wake looks like

w7_yield (W7 has nothing left):  mask; save W7; swap OFF; restore caller; r0 = 0; return  -> real swi 6
```

### 2.2 Why this preserves the game's ARM7 timing

| Game ARM7 duty | Stock | With W7 | Difference |
|---|---|---|---|
| Any IRQ (VBlank, VCOUNT/touch alarm, timers/SND alarm, IPC sync/FIFO = PXI, Wi-Fi, SPI, card) | wakes the halt; handler runs in the halting thread's context | parks W7, then the same handler runs in the same thread's context at an equivalent PC | + X7 latency (§2.3) |
| Thread switch (SND, SPI, WM threads) | on the handler's return (SDK) | identical: the game's scheduler saves the *main thread's* registers. W7's registers are never on any game stack | none |
| Main loop's post-halt work (reset check, periodic polls) | right after the first wake | right after the first wake (the gate returns 1, the `swi 6` is skipped) | + X7 |
| ARM9 card read (no IPC ping when the hook exists: patch_arm7.c:190, hook_arm9.c:252-254) | served at the next ARM7 wake, by `runCardEngineCheckHalt` before `swi 6` | the next IRQ parks W7, the main loop re-enters the hook, and `runCardEngineCheckHalt` runs first | + X7. Round 1's `b_pingIpc` force is **not** needed |
| CE7 VBlank work (cheats, DScheevos LED/SDIO shim, IGM) | in the game's VBlank handler chain | unchanged. The swap is always OFF when any game or CE7 handler runs | none |
| Halt with IRQs disabled (OS_Terminate loop, Kirby 0x02386058) | `swi 6` | the gate sees CPSR.I = 1 and returns 0, so stock `swi 6` | none |

- **Invariant I1-7:** `[0x0380FFFC] ≠ game_vec` ⇒ PC ∈ W7 ∪ {gate, irq_head7}. No game ARM7 instruction and no
  CE7 handler runs while it is swapped. The proof structure is round 1's I1, checked per instruction in melonDS
  (A7-M5).
- W7 masks IRQs only around ring-index updates and its own yield, at most ~50 cycles (A7-M11 inventory).
  rcheevos never masks.
- W7 writes only:
  - its NWRAM slots;
  - the module's main-RAM control words, staging tail and outbox (inside 0x02D80000-0x02F80000);
  - `0x0380FFFC`, while no game code runs.
  It never writes the game's ARM7 WRAM, WRAM-A, thread structs or stacks. Today's ARM9 OS_Halt hook lets the
  scheduler save module registers into a thread struct; W7 does not even do that.
- No game timers are used. W7's clock is the header VBlank count (CE7 increments it, cardengine.c:2063) and
  REG_VCOUNT.

### 2.3 X7, the only added latency **[estimate, from gbatek.txt:91733-91760 cycle rules and §1.2 timings]**

| Step | ARM7 cycles |
|---|---:|
| finish W7's current instruction: worst LDM/STM of 16 regs in NWRAM (1-cycle **[guess]**); or the pull's main-RAM LDM×8 = 10 + 7×2 + 1 | 18 / 25 |
| exception entry + BIOS IRQ stub (stmfd 6 regs to ARM7 WRAM, `ldr pc,[r0,#-4]`) | ~18 |
| irq_head7 (~35 instructions from WRAM-A) | ~70 |
| total extra before the game's handler starts (stock pays its own BIOS entry anyway) | **~115** (×2 for safety: **≤ 230 ≈ 6.9 µs**) |

- This is 0.1 scanline (63.5 µs per line).
- Per frame: W7 resumes and parks once per ARM7 IRQ. The ARM7 IRQ count per frame is unmeasured (estimate
  20-50: VBlank, VCOUNT alarms, the SND alarm every 5.2 ms, PXI). Overhead O ≈ 20-50 × (115 + ~50 resume)
  ≈ 3.3-8.3k ARM7 cycles per frame.
- A further optimisation: tail-call the game vector with the BIOS frame rewritten to the caller's r0-r3/r12/lr.
  That saves one BIOS entry (~18) but writes the game's IRQ stack, so it is not in the base.

### 2.4 Fallback if the owner refuses the 0x0380FFFC swap

- W7 runs with game IRQs delivered on top of it, and yields only at record boundaries. That is round 1's A7 shape.
- Cost: the main loop's post-halt work (reset check, polls) and ARM9 card requests wait up to one `do_frame`. That
  is ≤ c / 33.5 MHz: ≤ 9.7 ms at c = 325k.
- The game's scheduler also saves W7's registers into the main thread's context (precedent G7).
- This is measurable (A7-M6 card-read p99) but not equal to stock. Not recommended.

### 2.5 Reset, quit, in-game menu

- CE7's reset/return paths (cardengine.c:462, 777, 821) run only inside game IRQ handlers, so the swap is already
  OFF there.
- They set `w7.reset`. The next gate entry re-creates W7's context at its entry, after a frame-stamped reset
  record (§4.3).
- The IGM runs inside CE7's VBlank handler loop (cardengine.c:2119 `while (1)`), so W7 is parked for the whole menu.
- An assert in irq_head7 (unexpected mode, frame shape) sets `w7.disabled`. Coverage is lost, never stability; the
  session is then reported "not evaluated" (§5).

---

## 3. NWRAM slots: selection, handover, verification

### 3.1 Selection (boot, bootloaderi ARM7, in `dscheevosSetup`)

1. Start from eval-speed's `nwramGrant` (efe545d5): base = 0x03700000 + roundup(FNT/FAT), end 0x03780000, with its
   refusal conditions.
2. ARM7 slots = whole 32 KB slots in `[roundup32K(base), 0x03780000)`. ARM7 master is per bank, so a slot shared
   with the FNT/FAT tail must stay ARM9's.
3. Need = module7 hot code (47 KB at ARM -O2, arm7-notes) + .data/.bss + W7 stack (8 KB) + NWRAM record queue
   (8 × record size: MKDS ~2.4 KB, SoulSilver ~27 KB) + the parsed set (Kirby rcheevos data 58 KB; MKDS ~180-326 KB).
   - Kirby ≈ 150 KB = 5 slots; MKDS ≈ 250-390 KB = 8-12 slots **[estimate]**.
4. The ARM7 evaluator is eligible only if
   - need ≤ ARM7 slots,
   - the ARM7 hook was applied (S-A7-2), and
   - the cost model says c ≤ 325k (§6.2).
   Otherwise the ARM9 evaluator (P1/W) runs, and eval-speed's ARM9 heap zone may use the same grant. **One
   evaluator per session.**

### 3.2 Handover (boot only)

At `ARM9_DSCHEEVOS_INIT` (main.arm9.c:373-377) all banks are still ARM7-master (initMBK). The order:

1. ARM9_DSCHEEVOS_INIT (SCFG still unlocked). eval-speed's commit already takes the B/C banks
   (`transferToArm9`).
2. `dscheevosInit` parses the set with module7's in-heap allocator, into the ARM7 slots at their final addresses.
   This is the same image: cold load code in main RAM, hot code in NWRAM. The ARM7 lane notes the load takes
   26M-504M ARM7 cycles, so the load stays on the ARM9.
3. rc_client callbacks are re-pointed to ARM7-side functions with the public setters (read_memory, event handler,
   time). server_call is module7's own function, so the address is the same on both CPUs.
4. The ARM9 cleans and invalidates its whole D-cache and drains the write buffer.
5. ARM9_SETSCFG (main.arm9.c:428-436): `transferToArm9(i)` for every bank **except** the ARM7 slots, which are
   written back to master = 1 with the same offset and enable. Then the lock, as stock.
6. The ARM7's first gate entry runs the verification (§3.3), then W7 starts.

This is a change inside nds-bootstrap's DScheevos hooks and at ARM9_SETSCFG. It is the same place eval-speed's
grant already edits (Q5-class upstream footprint).

### 3.3 Run-time verification (ARM7, first entry, then every 600 frames as a cheap check)

- **MBK2-5 read on the ARM7** (readable there, GBATEK 27753): every ARM7 slot is enable = 1, master = 1, and has
  the expected offset. ARM7 MBK7/MBK8 are unchanged.
- **Code CRC:** a CRC of module7's text, compared with the value the ARM9 stored at handover. The first check costs
  ~47 KB × ~3 cycles ≈ 150k cycles; the periodic check covers one slot per pass.
- **Canary words** at each slot's last 8 bytes.
- Any mismatch: `w7.disabled`, a status flag, no evaluation.

Why nothing else can write the slots:

| Writer | Excluded by |
|---|---|
| ARM9 CPU, ARM9 DMA/NDMA (game or CE9) | the slot is ARM7-master, so it reads as zero on the ARM9 (GBATEK 27841-27849) |
| DSP | master ≠ DSP |
| game ARM7 code | no literal in 0x03700000-0x037BFFFF in any of the 64 ARM7 binaries (static scan; computed addresses not excluded); melonDS write-watch A7-M3 |
| CE7 | writes B/C only at boot (FNT/FAT, CLUT), and those ranges are excluded by §3.1 |
| cheat engine (AR codes, CE7 VBlank) | cheats are off in hardcore (lockdown). In softcore an AR code could write anywhere; the CRC/canary check catches it |

---

## 4. Hand-off protocol with the ARM9 capture

### 4.1 Where

- Only main RAM is visible to both CPUs:
  - NWRAM banks are exclusive per CPU (§1.3);
  - old shared WRAM is given to one side (WRAM_CR, main.arm9.c:183);
  - the IPC FIFO is the game's PXI channel.
- Everything crosses in the module region 0x02D80000-0x02F80000 (dscheevos_hooks.h:12-13).
  - The ARM9 writes through the uncached mirror (+0x0A000000, start.s:74, 269-276), or cleans by MVA.
  - The ARM7 has no cache.

### 4.2 Structures and ownership

```
ctl (one 32-byte line, ARM9 writes only through the uncached mirror):
  head9      : records published (ARM9-owned)
  a9_waiting : 1 while the ARM9 is inside a wait gate (E or idle), set/cleared by the gate (uncached store)
  gen9       : capture-plan generation (bumped when the plan changes)
ctl7 (separate 32-byte line, ARM7 writes only):
  tail7      : records consumed by the ARM7
  out_head7  : events published by the ARM7 (outbox)
  w7_status  : running / disabled / backlog (frames) / max backlog / frames evaluated
staging ring (main RAM, existing 256 KB / 1024-record queue, architecture.md:52, moved behind ctl):
  record = { u32 seq, u32 frame_no, u16 repeat, u16 flags, u32 gen, u32 len, payload[len] }
  flags: RESET, HARDCORE_CHANGE, CARRY (no capture this VBlank: previous memory), PLAN_CHANGE
```

- **Ownership rule.** Record slot i belongs to the ARM9 iff i ∉ [tail7, head9). Published records are immutable.
- A run of identical frames is published as one record with `repeat = n`, closed when the next distinct frame
  arrives or after 64 repeats. The ARM9 never edits a published record.
- **ARM9 publish order:**
  1. payload and header through the uncached mirror (or cached + clean by MVA of the record's lines);
  2. `mcr p15,0,r0,c7,c10,4` (drain the write buffer);
  3. store `head9`.
- **ARM7 consume:**
  1. read `head9`;
  2. LDM-copy the record into the NWRAM queue;
  3. store `tail7`.
  ARM7TDMI stores complete in order (no write buffer), so no fence is needed.
- **ARM9 reads of `tail7` / `out_head7` / outbox** go through the uncached mirror. Cached reads could be stale, the
  same rule as today's journal (dscheevos_hooks.h:36).
- **W7 consumes in frame order:** for each record, `repeat` × `rc_client_do_frame` on that memory. RESET runs
  `rc_client_reset` at that frame. PLAN_CHANGE takes the new read mapping (rnd's per-generation table, immutable
  once published).

### 4.3 The way back (ARM7 → ARM9), all at W7 record boundaries

- **Events** (unlock, leaderboard, progress, challenge) go to an outbox ring in main RAM. The ARM9 module turns them
  into notices, LED cues, journal entries and the menu list.
  - The list is today a pointer the menu reads (DSCHEEVOS_H_LIST); W7 publishes list deltas and the ARM9 owns the
    list memory.
- **server_call:** rc_client's request (URL and body built in NWRAM) is copied to an outbox request slot. The ARM9's
  lwIP sends it. The reply is copied to an inbox slot, and W7 calls rc_client's stored callback at its next record
  boundary.
  - Rate: unlocks and the ~2-minute ping, so negligible bus.
- The journal and every SD write stay on the ARM9/CE7 safe points, unchanged.

### 4.4 Pull gating (bus)

W7 pulls when any of these holds:
- `a9_waiting` (waiting games: the ARM9 is halted or parked in its gate);
- the staging occupancy is > 50%;
- the session is busy-class (no gate entry for 600 frames, round-1 `no_wait_run`).

It polls `head9`/`a9_waiting` only on a "new VBlank" bit:
- CE7's existing `dscheevosVBlank` (cardengine.c:2062-2074) also sets a word in ARM7 WRAM, one store in code that
  already runs;
- plus once per resume while a backlog exists.

So the polling traffic is ≤ 2 main-RAM reads per frame in steady state.

### 4.5 Boot and load droughts

- The ARM9-side staging ring is the drought buffer, because W7 cannot run while the ARM7 never halts (§1.5:
  ≤ 603 boot frames).
- Today it holds 1,024 records in 256 KB with repeat compression (architecture.md:52).
- It must not drop. Its size is chosen from measurement A7-M9 (bytes used during each set's worst boot drought).
- If a set's boot drought does not fit 256 KB, the staging region grows for that session at boot. It comes out of
  the ROM cache, as the launcher's dry-run decides.
- Catch-up afterwards is automatic: W7 evaluates queued records back to back whenever the ARM7 is idle. Drain time
  = backlog × c / (F − c − O) (§1.5 table).
- Unlocks found during catch-up are sent late: up to 30 s after boot at c = 325k, ≤ 0.2 s in gameplay. That is
  compliance question Q-C2.

### 4.6 What this needs from rnd (gather) and the busy-engine capture

- **T:** record n is complete and published (head9 after the drain) before staging slot n + K must be reused. W7
  tolerates any lag, but there is a contention preference: for waiting games, publish before the ARM9's wait for
  frame n ends, so the pull can land inside that wait.
- **X (ARM7 side, ours):** the payload is contiguous and word-aligned (LDM-friendly), with one header.
  - ARM7 pull ≈ 3 bus cycles per word: MKDS ~86 words (56 watched + ~10 chain targets × 3) ≈ 260;
    Platinum ~200+ ≈ 650; SoulSilver 562-849 + targets ≈ 1.8-2.6k; TWEWY 948 ≈ 2.9k **[estimate; word counts from
    arm7-notes rnd section]**.
- The record must be a pure function of game memory. No rcheevos memref writes happen during capture (arm7-notes R6)
  and no live reads happen on the ARM7 (R7). Sets that need either stay on the ARM9 evaluator.
- "A coherent copy in NWRAM" (brief) cannot be produced by the ARM9 or NDMA9 for an ARM7-master slot (§1.3). The
  gather target is main RAM; the ARM7 does the last hop.

---

## 5. Coverage (what the ARM7 lane can and can't evaluate)

| Session condition | ARM7 evaluator? | Else |
|---|---|---|
| ARM7 hook applied (59/64 ARM by static replication; 5 Thumb unverified) | needed | ARM9 evaluator only |
| NWRAM free ≥ need (§1.4: 62/64 have ≥ 288 KB; GS DD 128 KB; Clubhouse Games est. 0; colour filter 0) | needed | ARM9 evaluator only |
| set cost c ≤ 325k ARM7 cycles per frame (§6) | needed | ARM9 evaluator |
| set with Remember/Recall live reads or a capture that writes rcheevos state | no (R6/R7) | ARM9 evaluator |
| busy-class game (no ARM9 wait) | **the ARM7 is the only possible evaluator** | none today: GS DD's set ~720-770k est. and only 4 free slots. Code Lyoko and DKR set costs unmeasured |

So, with today's ARM7 numbers, the ARM7 lane covers small sets (≈ ≤ 95k ARM9 instructions) in every class where
the capture exists. That covers the `bios` small sets (Sonic Rush, Pac'n Roll) without any evaluation in the SWI 5
wait. It does **not** yet meet "every frame of every game" for medium/heavy sets or the busy class. That needs the
ARM7 cost cut (NWRAM data placement) and GS DD's NWRAM (Q-O1).

---

## 6. Throughput needed per set vs ARM7 free time

### 6.1 Budget

- F = the game's ARM7 halted cycles per frame, gameplay p10: Kirby 484k, MKDS 465k, Sonic Rush 493k, Platinum
  500k, Black 465k, Digimon 486k, TWEWY 515k, SoulSilver 475k (arm7-notes.md).
- Minus W7 overhead O ≈ 3-8k (parks and resumes) and the pull (≤ 3k).
- From §1.5:
  - **c ≤ 400k** keeps gameplay backlog ≤ 12 frames and drains the worst boot drought (603 frames) in ≤ 58 s;
  - **c ≤ 325k** drains it in ≤ 30 s. That is the selection threshold, leaving ~30% headroom for melonDS optimism
    and Wi-Fi-heavy scenes.

### 6.2 Per set (ARM7 cycles per frame)

Measured: ARM7 lane, melonDS, hot code in WRAM, data in main RAM. Estimated: ARM9 instructions × 3.4, using the
ARM7 lane's finding that "ARM7 cycles ≈ ARM9 cycles for the same set".

| set | ARM7 c | source | ≤ 325k? | ≤ 400k? | note |
|---|---:|---|---|---|---|
| Ghost Trick | ~65k | est. (19k instr) | yes | yes | |
| 999 | ~120k | est. | yes | yes | |
| **Kirby SSU** | **164.5k** | measured | yes | yes | 35% of F |
| Trauma Center 2 | ~195k | est. | yes | yes | |
| Drawn to Life | ~210k | est. | yes | yes | |
| Sonic Rush (bios) | ~235-250k | est. | yes | yes | no evaluation in SWI 5 at all |
| Pac'n Roll (bios) | ~270k | est. | yes | yes | |
| Kirby SqS / PvZ | ~320k | est. | borderline | yes | |
| Cory in the House | ~380k | est. | no | yes | |
| RE:DS (bios) | ~670-710k | est. | no | no | needs 2.1x cut |
| **Golden Sun DD (busy)** | ~720-770k | est. | no | no | needs ≥ 2.2x cut and NWRAM (4 slots) |
| **MKDS** | **650k** | measured | no | no | conditions stream ~150 KB per frame (rnd): with the set in NWRAM, ~300k of main-RAM wait could go **[estimate]** |
| synthetic typical | 873k | measured | no | no | |
| **Pokémon Black** | **933k** | measured | no | no | |
| Platinum / Phoenix | ~1.5M / ~1.0M | est. (ARM9 set bench) | no | no | |
| **Digimon** | **3,014k** | measured | no | no | |
| HG/SS, TWEWY | ≫ | — | no | no | |

- The decisive unmeasured number is the ARM7 cost with the parsed set in NWRAM (the ARM7 lane's next
  measurement).
- If it halves the main-RAM-data numbers, MKDS (~325k) and the RE:DS/GS DD class (~350-390k) come within the 400k
  cap **[estimate]**.

### 6.3 Cost model for the boot decision

- The launcher or bootloader needs c before the game runs. rc_client state can't be dry-run without changing it
  (and a reset-to-fresh equivalence is unproven).
- So use a static model: a·conditions + b·memrefs + d·chains, calibrated by the ARM7 lane on all 32 real sets
  (A7-M8).
- A set measured over the threshold must never be selected: margin by construction, plus the hardware counters
  (`w7_status` max backlog) logged at quit.

---

## 7. Zero-cost argument: every game-visible effect and its bound

| # | Effect | When | Bound | Test |
|---|---|---|---|---|
| A1 | Boot code patches: 2 lines in CE7 `newSwiHalt` (CE7 code, not game code); the game's halt veneer patch is stock nds-bootstrap (patch_arm7.c:159-200) | always | accepted class | — |
| A2 | `0x0380FFFC` swapped while W7 runs | ARM7 idle only | I1-7: no game instruction executes while swapped | A7-M5 (per instruction, melonDS) |
| A3 | Extra IRQ latency X7 for IRQs that arrive while W7 runs | ARM7 idle | ≤ 230 cycles (6.9 µs) **[estimate]** | A7-M10 lab ROM: Δp100 ≤ 300 cycles |
| A4 | Main-RAM bus: record pull + ≤ 2 control reads + rare outbox | per frame | ≤ words×3 + 20 bus cycles: MKDS ≈ 280 (0.05% of 560k), SoulSilver ≈ 2.6k (0.47%) | A7-M4 |
| A5 | NWRAM banks given to the ARM7 instead of the ARM9 | session | only banks nds-bootstrap leaves unused (above the FNT/FAT; no CLUT/ROM). The ARM9 sees zeros where stock left unused NWRAM | A7-M3 + ARM9 read-watch in melonDS |
| A6 | ARM7 runs instead of halting | ARM7 idle | power only | — |
| A7 | ARM7 cache after-effect | — | none: the ARM7 has no cache | — |
| A8 | Card-read and PXI latency | per request | same wake point as stock, + X7 | A7-M6 p99 |
| A9 | ARM9 side | — | unchanged from the capture design: the ARM9 no longer evaluates, so its cache-refill cost falls from ~100k cycles per frame to the capture's share | A7-M12 (game-cost rule) |

- Writes: W7 never writes game memory. The only writes outside our own memory are `0x0380FFFC` (A2) and the
  bootloader's MBK bank masters at boot (A5).
- **Minimum nonzero cost (cannot be removed):**
  - A3: any running code adds ≥ 1 instruction plus the exception path versus a halted CPU;
  - A4: main RAM is the only memory both CPUs see.
- A3's per-IRQ bound and A4's per-frame bound are the exact minimum cost (§0.6, §2.3).
- **Test that shows no lag:** A7-M6 lag/audio/wireless A/B, with A7-M4 at 10× the pull size as margin.

---

## 8. Measurements (pass/fail fixed now; no emulator run in this round)

| ID | Where | What | PASS | FAIL → |
|---|---|---|---|---|
| S-A7-1 | static (**done**, `design2/site_roles.tsv`) | hook-site role | 59/64 main-loop veneer | — |
| S-A7-2 | melonDS boot logs, 64 ROMs | "swiHalt hooked" incl. the 5 Thumb ARM7 ROMs; `altReadMethod` off | ≥ 62/64 hooked | unhooked ROMs: ARM9 evaluator; busy unhooked (DKR?) needs a Thumb hook fix |
| A7-M0' | melonDS, 10 games | share of ARM7 halted cycles entered through the hook | ≥ 95% in gameplay | if lower, W7's F is smaller: re-run §1.5 with the hooked share |
| A7-M1 | DSi XL | first hardware build: gate + park + a **spin-counter W7** (counts loop iterations until parked; no rcheevos). Per frame: free cycles, parks per frame, max park latency (VCOUNT) | free p10 ≥ 0.6 × 560k in KSSU/MKDS/Sonic Rush, and within 10% of melonDS | lower: rework the budgets |
| A7-M5 | melonDS | I1-7 per instruction; W7 writes outside its slots/ctl7/outbox/0x0380FFFC | 0 / 0 over 10 games × 6,000 frames | fix before any hardware game run |
| A7-M10 | DSi XL lab ROM (our own ROM, so timers allowed) with an SDK-shaped ARM7 main loop | IRQ entry latency (timer, VCOUNT, IPC), stock halt vs parked spin-W7, 10,000 each | Δp100 ≤ 300 ARM7 cycles | owner (Q3 class) |
| A7-M6 | DSi XL, spin-W7 then real W7: KSSU, MKDS (+ 2-console local race, + Download Play), Sonic Rush, Platinum, a streaming cache-mode game | lag intervals; ARM9 card-read p99 (request → done); PXI reply p99; SND thread jitter; wireless session completes; Clark listens for audio | lag = stock; p99 ≤ +1%; no disconnect; no audible change | stop the ARM7 lane for that class |
| A7-M3 | melonDS | write-watch (all writers) on the ARM7 slots, 10 games incl. SDK ≥ 2.8, wireless, cheats, colour filter (expect: no grant) | 0 foreign writes | shrink the slot set |
| A7-M4 | DSi XL | ARM9 main-RAM miss stream + WRAM-B/C stream (FNT/FAT slot) timed by VCOUNT while the ARM7 (a) halts, (b) runs W7 in NWRAM, (c) pulls 3 KB per frame, (d) pulls 30 KB per frame (10× margin) | (b) ≤ +0.1%; (c) ≤ 3k bus cycles per frame of slowdown; (d) proportional (no cliff) | gate pulls to `a9_waiting` only, or fail zero cost |
| A7-M9 | melonDS, the 8 runs + GS DD, Code Lyoko, DKR | staging bytes used during the worst drought with repeat compression; drain time at the real c | fits 256 KB, or the per-set growth stays within the launcher's budget | larger staging (ROM cache) or a compliance ruling |
| A7-M8 | qemu/set bench (ARM7 lane) | cost model vs measured c, 32 real sets | |error| ≤ 20%; no set > 400k predicted ≤ 325k | ARM7 selection off for unmodelled sets |
| A7-M12 | melonDS cache model + DSi XL | **game-cost rule:** the game's ARM9 cycles per frame, achievements off vs ARM7-evaluator on: Kirby, MKDS, one busy game (GS DD with capture) | equal within run noise (melonDS); hardware lag A/B equal | — |
| A7-M2 | DSi XL (ARM7 lane) | CPI with the set in NWRAM | owns the c column of §6.2 | — |

---

## 9. Risks (ranked)

1. **Throughput:** with data in main RAM only Kirby-size sets fit. The busy class (GS DD) and RE:DS need ≥ ~2x.
   If NWRAM data placement doesn't deliver, the owner's "always evaluated" ruling stays unmet for them.
2. **GS DD has only 4 free slots** (FNT/FAT 366 KB). Covering it needs either a smaller FNT/FAT pre-cache (CE9 file
   opens go to SD: slower loads, game-visible) or another place for it (Q-O1).
3. **Thumb-ARM7 games** (5/64, including DKR, a busy game) depend on nds-bootstrap's Thumb hook, which needs a save
   patch to free space. If one is unhooked, it has no ARM7 evaluator.
4. **X7 on hardware is an estimate.** The NWRAM wait states for the ARM7 are unknown, and so is the IRQ count per
   frame. A7-M1/M10 decide.
5. **Boot droughts on hardware may be longer** (real SD speed). The staging ring may then need to grow (ROM-cache
   cost); A7-M9 plus a hardware drought log.
6. **No mid-session switch:** a mis-predicted heavy set falls behind for the whole session. Mitigated by the
   threshold and the cost model, and reported at quit.
7. **The swap is a runtime write to a BIOS-defined game-RAM word** (same class as round 1's Q1). If refused, the
   fallback (§2.4) is not equal to stock.
8. **melonDS models neither contention nor real SD timing.** Every bus and drought claim is hardware-decided.

---

## 10. Questions

- **Q-O1 (owner):** for GS DD-like sessions, may the FNT/FAT pre-cache be cut or moved so the ARM7 gets ≥ 8 NWRAM
  slots? The cost would be slower file opens during loads, measured. **Recommend:** only after A7-M2 shows GS DD's
  set fits the ARM7 at all.
- **Q-O2 (owner):** the ARM7 `0x0380FFFC` swap, same class as the settled DTCM+3FFC park. **Recommend yes.**
- **Q-C1 (ra-compliance):** may the evaluator migrate between CPUs mid-session through rc_client's public progress
  serialization? That would mean one live instance at a time, a dormant twin loaded from the same replies, and no
  user-visible save state. **Recommend:** don't, unless A7-M8 shows mis-selection in practice.
- **Q-C2 (ra-compliance):** unlocks found during post-boot catch-up are sent up to ~30 s later than the frame they
  happened on (the frame itself is correct). Acceptable?

## Files

- This proposal: `scratchpad/design2/proposal-arm7-sched.md`.
- Evidence:
  - `scratchpad/design2/a7site.py` (finder replica) and `sites.tsv`;
  - `callers.py` and `site_roles.tsv` (caller roles);
  - `a7scan/*.a7.arm.s` (ARM7 disassembly);
  - `nwram_fit.tsv` (free NWRAM per ROM);
  - `backlog.py` and `backlog_all.py` (drought simulation over `private/census/out/*/frames.csv`);
  - `a9nw.py` (ARM9 literal scan: inconclusive, because data tables collide with the range, so not used as
    evidence).
