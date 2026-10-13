# Proposal A7: evaluation on the ARM7 ("ARM9 captures, ARM7 evaluates")

Angle: move evaluation (or part of it) to the ARM7. Every factual claim cites a file:line, a GBATEK
section (local copy `scratchpad/dl/gbatek.txt`, line numbers given) or a measured table. Estimates are
marked **[estimate]**, guesses **[guess]**.

Short verdict, up front:

- **Evaluation on the ARM7 is workable only with rcheevos' hot code and data in DSi NWRAM.** With rcheevos'
  heap in main RAM the ARM7 is ~3x slower than the ARM9, and it keeps the main-RAM bus ~70% busy, so the ARM9
  game pays for it. Rejected.
- **With NWRAM, the estimated ARM7 capacity is 140k-250k rcheevos instructions per frame** [estimate]. That
  covers the ~150k-gate sets and some 150k-250k sets. It is in the same range as what the ARM9 gets from
  OS_Halt idle today.
- **It decouples evaluation from how the ARM9 waits.** The ARM9 only has to capture, at a frame-done point.
  That is the main win for the BIOS-wait class: only a bounded ~12-17k-instruction capture runs inside the
  non-preemptible SWI 5 wait, and no rc_client_do_frame does.
- **It does NOT give busy engines (Golden Sun DD, Code Lyoko) coverage at zero cost.** The ARM7 can't see the
  ARM9's write-back D-cache or DTCM, so someone on the ARM9 must clean the cache or copy at a defined point.
  A game that never waits has no such point except its VBlank IRQ, and any work there is game time.
  **This angle fails the zero-cost rule for the `busy` class.** That variant is written down below only so the
  owner can rule on it.
- **Nothing here is measured yet.** ARM7 idle time, ARM7 CPI from NWRAM, bus contention and whether WRAM-A is
  free are all unmeasured. melonDS cannot measure the contention (it doesn't model cross-CPU bus stalls,
  facts-hw.md §3). The measurement plan (M0-M7) comes before any build, with pass/fail fixed below.

---

## 1. Facts this rests on

### 1.1 ARM7 speed and memory timings

- ARM7TDMI at the 33.513982 MHz bus clock; no cache, no TCM, no CP15 (GBATEK "DS Memory Timings", gbatek.txt:7611-7613;
  facts-hw.md §5 "The NDS7 doesn't have any TCM, Cache, or CP15"). The DSi does not speed up the ARM7: SCFG_CLK7 has no
  CPU clock bit (gbatek.txt:24261-24269; only SD, touchscreen and NWRAM clocks). One frame = 560,190 ARM7 cycles
  (33,513,982 / 59.8261), half of the ARM9's 1,120,380 at 67 MHz (idle.md:10).
- Access times, 33 MHz units (gbatek.txt:7623-7642):
  - NDS7 CODE: main RAM N32 = 9, S32 = 2, N16 = 8, S16 = 1; WRAM/BIOS/I/O 1/1/1/1 (32-bit bus).
  - NDS7 DATA: main RAM N32 = 10, S32 = 2, N16 = 9, S16 = 1; WRAM 1/1/1/1.
  - "Main Memory is ALWAYS having the nonsequential 3 wait PENALTY (even on ARM7)" (gbatek.txt:7681).
- ARM7TDMI instruction timings (GBATEK "ARM Instruction Summary", gbatek.txt:91733-91760):
  - LDR = 1S+1N+1I; STR = 2N; LDM = nS+1N+1I; STM = (n-1)S+2N;
  - B/BL/BX = 2S+1N; data processing = 1S.
- NWRAM timing for the ARM7 is not in GBATEK's table. That it equals WRAM (1 cycle, 32-bit) is a **[guess]**,
  checked by M2.

### 1.2 The ARM7 memory nds-bootstrap leaves in DS mode

- At boot, nds-bootstrap's ARM7 maps all DSi NWRAM to the ARM7 side (bootloaderi/arm7/main.arm7.c:227-256):
  - WRAM-A (256 KB) at 0x037C0000-0x037FFFFF, ARM7 master;
  - WRAM-B (256 KB) at 0x03740000-0x037BFFFF;
  - WRAM-C (256 KB) at 0x03700000-0x0373FFFF.
  - MBK9 = 0x3000000F.
- For DS-mode games, the ARM9 then gives every WRAM-B/C slot to the ARM9 before it locks SCFG:
  `transferToArm9(i)` for i < 16, then `REG_SCFG_EXT &= ~(1UL << 31)` (bootloaderi/arm9/main.arm9.c:426-434).
  WRAM-A stays with the ARM7.
- What nds-bootstrap already keeps in NWRAM during DS-mode play:
  - **WRAM-A:**
    - the ARM7 card engine CE7 at 0x037E0400, 61 KB (locations.h:88; cardenginei/arm7/build/cardengine.ld: `ORIGIN = 0x037E0400, LENGTH = 61K`);
    - the cheat engine at 0x037DC000 (locations.h:82);
    - the ROM/save file descriptors at 0x037EFFA0 (locations.h:9).
    - The top 32 KB (0x037F8000-0x037FFFFF) is where DS games put their ARM7 shared-WRAM code and data
      (find_arm7.c, patch_arm7.c:179 use 0x37F8000). With WRAM-A mapped over it, that is **game memory** and
      must never be touched.
  - **WRAM-B/C (ARM9 side):**
    - ROM-in-RAM can spill into it: `wramSize` 0x80000 is added to `romSizeLimit` (main.arm7.c:912-929), and
      romLocation wraps to 0x03700000 (main.arm7.c:889-890);
    - the NitroFS FNT/FAT pre-cache at 0x03700000, up to 0x80000 (main.arm7.c:1360-1366; CE9 reads it,
      cardenginei/arm9/source/cardengine.c:902-903);
    - the screen colour LUT at 0x03770000 and the CLUT engine at 0x03732800 (main.arm7.c:2021-2033; locations.h:103).
- **Candidate free ARM7 memory:** WRAM-A 0x037C0000-0x037DBFFF (112 KB). No nds-bootstrap runtime component I
  could find uses it in DS mode. The only reference is the front end's probe at retail/arm7/source/main.c:125-130.
  That it is unused **at run time** is a **[guess]**, checked by M3.
- The ARM7 already reads the module's main-RAM area at 0x02D80000 during play: the VBlank count, the cue words and
  the net ring (cardenginei/arm7/source/cardengine.c:1939, 2001-2002, 2063; dscheevos_hooks.h:12-13). So ARM7 access
  to the module region needs no new mapping.

### 1.3 Coherence: why the ARM7 cannot just read game RAM

- NitroSDK main RAM is write-back cached on the ARM9: PU region 1 0x02000000 8 MB, C=1 B=1. This was verified in
  KSSU's crt0 at 0x02000a98-0x02000b2c (design-notes.md:22-24).
- D-cache: 4 KB, 4-way, 32-byte lines; read-allocate, so writes to lines that aren't cached go straight to the
  write buffer (facts-hw.md §5).
- The ARM7 is a separate bus master. A variable the game keeps hot (written every frame, read often) can sit dirty
  in the ARM9 D-cache for any number of frames, and the ARM7 then reads an old value from RAM.
- DTCM is wired directly to the ARM9 ("DMA cannot access DTCM/ITCM", facts-hw.md §7). The ARM7 cannot read it at
  all. The RA DS memory map includes DTCM (external/rcheevos/src/runtime/rc_consoleinfo.c:738-747), and the module
  serves it (module.c:136-139; core dscheevos.c:357-366).
- The project already chose the ARM9 for the copy for this reason: "The ARM9 does the copy (not the ARM7) because
  only the ARM9 sees what's in its own data cache" (docs/architecture.md:46).

### 1.4 Bus priority

- EXMEMCNT bit 15: "Main Memory Access Priority (0=ARM9 Priority, 1=ARM7 Priority)", writable by the ARM9 only
  (gbatek.txt:7408; facts-hw.md §3).
- nds-bootstrap's ARM9 writes `REG_EXMEMCNT = 0xE880` at boot (bootloaderi/arm9/main.arm9.c:184), which gives
  **ARM7 priority**. Whether the game's crt0/OS_Init rewrites it is **unverified**; M4 reads it.
- How many cycles an ARM7 main-RAM access stalls the ARM9, and at what granularity arbitration happens, is not in
  GBATEK. melonDS does not model it (facts-hw.md §3). Only hardware can measure it (M4).

### 1.5 Where the ARM7 gets CPU time today

- nds-bootstrap already patches the game's ARM7 halt call (`patchSwiHalt`, bootloaderi/arm7/patch_arm7.c:159-200).
  The hook is `newSwiHalt`: `push {lr}; bl runCardEngineCheckHalt; swi 6; pop {pc}` (cardenginei/arm7/source/card_engine_header.s:445-450),
  with a Thumb variant (card_engine_header.s:455-466).
- The site is found by a signature near a DISPSTAT constant in the first 4 KB of the ARM7 binary (find_arm7.c:329-360).
  It is skipped when `altReadMethod` is set (patch_arm7.c:160-163). It also needs a free `newSwiHaltAddr` for Thumb
  ARM7 binaries (patch_arm7.c:174-175).
- **Whether that site is the lowest-priority loop on the ARM7 (an idle thread, or the main thread that does
  nothing else) is unverified.** M0 checks it per game. If it is a wait inside a higher-priority loop, work there
  delays the game's ARM7 (sound, PXI replies): the same trap as the ARM9's BIOS wait, moved to the ARM7.
- ARM9 card reads reach the ARM7 by the IPC FIFO IRQ: `myIrqHandlerFIFO` → `runCardEngineCheck` →
  `start_cardRead_arm9` / `resume_cardRead_arm9` (cardengine.c:1878-1903, 1743-1768). So they don't wait for the
  halt hook. The halt hook also serves requests it finds, with a blocking `fileRead` (cardengine.c:1799-1840).

### 1.6 What evaluation costs (instructions per frame, real RAM where available)

From memplan/cpu.md:12-43 and idle.md:16-38 (dsc_idle + dsc_capture, median):

| Set | instr/frame |
|---|---:|
| Ghost Trick | 19k |
| 999 | 35k |
| Trauma Center 2 | 57k |
| Kirby SSU | 61-68k |
| Drawn to Life | 62k |
| **Sonic Rush (bios)** | **69-74k** |
| **Pac'n Roll (bios)** | **80k** |
| Kirby SqS | 95k |
| Plants vs. Zombies | 94k |
| Cory | 111k |
| **Resident Evil DS (bios)** | **197-209k** |
| **Golden Sun DD (busy)** | **211-227k** |
| Starfy | 244k |
| Phantom Hourglass | 255k |
| MKDS | 268k |
| Tetris DS | 272k |
| Spirit Tracks | 306k |
| Phoenix Wright | 351k |
| Pokémon Black | 364k |
| Layton | 372k |
| MvDK | 473k |
| Sonic Colors | 474k |
| Platinum | 612k |
| Digimon | 1.08M |
| HG/SS | 2.1M |
| TWEWY | 4.6M |

Heap after load (memplan/rc.md:31-35): Kirby rcheevos data 58 KB, Platinum 559 KB, Rhythm Heaven 847 KB,
HG/SS 1.6 MB, TWEWY 3.1 MB. Core fixed allocations are 480 KB today, sized for the maximum, not the set
(memplan/mod.md:63-70). MKDS's set costs 326 KB on top (mod.md:79-80).

ARM9 reference: real eval CPI ~3.4 at 67 MHz (brief, eval-speed). `rc_client_do_frame` on the synthetic set takes
10.6 ms at 67 MHz and 7.7 ms at 133 MHz (architecture.md:121). Doubling the clock gives only 1.38x, so about 45% of
the ARM9's evaluation time is memory stall [estimate, from fitting t = c/f + m: c ≈ 392k core cycles, m ≈ 4.75 ms].
**The ARM9 evaluator is bound by main-RAM latency, and a zero-wait ARM7 avoids exactly that.** This is why the
ARM7 is not hopeless, despite half the clock.

---

## 2. ARM7 throughput, honestly

### 2.1 CPI model [estimate]

Assumed instruction mix for rcheevos' evaluation loop (condition fields, memref values, compares, branches)
[guess]: 30% loads, 5% stores, 15% taken branches, 50% ALU. Cycle costs from §1.1:

| Placement of code / data | LDR | STR | B | ALU | CPI |
|---|---|---|---|---|---|
| code + data in WRAM/NWRAM (all 1-cycle) | 3 | 2 | 3 | 1 | **~2.0** (range 1.7-2.4) |
| code in NWRAM, data in main RAM | 1+10+1 = 12 | 1+10 = 11 | 3 | 1 | **~5.1** |
| code and data in main RAM (ARM, S32 = 2; each data access breaks fetch sequencing → next fetch N32 = 9) | ~20 | ~26 | ~13 | 2 | **~9-10** |

Cross-check against the ARM9 [estimate]:

- The fit in §1.6 gives ~392k ARM9 core cycles for the synthetic set's do_frame.
- The ARM7 pays 3 cycles per LDR instead of ~1-2 on an ARM9 cache hit. Call it 1.3x the core cycles: ~510k
  ARM7 cycles, ~15 ms, against 10.6 ms on the ARM9 at 67 MHz.
- Both estimates put **the ARM7 with everything in NWRAM at 1.2-1.4x slower than the whole ARM9 at 67 MHz**,
  and ~3x slower with data in main RAM.

### 2.2 Capacity per frame

Capacity = 560,190 × f / CPI, where f = the share of the ARM7 the game leaves idle. **f is unmeasured** (M1).

| f (ARM7 idle share) | NWRAM, CPI 2.0 | NWRAM, CPI 2.4 | main-RAM data, CPI 5.1 |
|---|---:|---:|---:|
| 0.5 | 140k | 117k | 55k |
| 0.7 | 196k | 163k | 77k |
| 0.9 | 252k | 210k | 99k |

For comparison, ARM9 OS_Halt idle at CPI 3.4 (idle.md:16-38, gameplay mean idle cycles / 3.4):

| Game | ARM9 instr/frame |
|---|---:|
| MKDS | 114k |
| TWEWY | 115k |
| Sonic Colors | 186k |
| Platinum | 238k |
| Tetris | 280k |
| Phoenix | 286k |
| Sonic Rush, RE DS, Golden Sun (bios / busy) | 0 |

Reading of the two tables:

- **Main RAM for the heap: rejected.** At ≤ 99k instr/frame it covers only the ~60-90k sets, and only at high f.
  It also loads the main-RAM bus (§4.2).
- **NWRAM:**
  - Fits the ≤150k class (Kirby SSU, Sonic Rush, Pac'n Roll, PvZ, Kirby SqS, Drawn to Life, Trauma Center,
    999, Ghost Trick, Cory, Sonic Rush Adventure) at any f ≥ 0.5 [estimate].
  - Fits RE DS (~200k) and Golden Sun (~220k) only at f ≥ ~0.75-0.8.
  - MKDS, Tetris and Phantom Hourglass (255-272k) only on average, through the queue.
  - Spirit Tracks and up (≥ 300k) don't fit.
- **The two evaluators can't be added together.** rc_client keeps its state on one CPU (§3.6). So per game it is
  the ARM7 or the ARM9 idle, whichever is larger. For ARM9-busy games like MKDS the ARM7 may win; for ARM9-light
  games like Tetris and Phoenix the ARM9 wins.

### 2.3 What eats the ARM7's time (why f < 1)

- **Game ARM7 work:**
  - the NitroSDK sound driver, every SND frame (~5.2 ms) [guess on cost: 5-30% of the ARM7];
  - SPI touch and microphone sampling, RTC, PXI command handling;
  - wireless (WM/MP) when the game uses it, which can be most of the ARM7 [guess].
- **nds-bootstrap's CE7:**
  - SD reads for the ARM9 card engine (blocking `fileRead` in the halt hook, cardengine.c:1826-1828; nonblocking
    NDMA reads in the FIFO IRQ, cardengine.c:1709-1718);
  - the cheat engine and the VBlank handler (cardengine.c:2119-2140);
  - DScheevos' own SDIO shim (≤ 6 lines when it moves a frame, architecture.md:89) and LED cues.
- **Loading screens** stream from SD, so the ARM7 is busy exactly when the ARM9 is too. The snapshot queue
  (1024 frames, architecture.md:53) absorbs those stretches.

---

## 3. Mechanism

### 3.1 Split of work

| Who | Does | When |
|---|---|---|
| ARM9 (module9, ARMv5TE, main RAM, as today) | VBlank counting (IPC IRQ, unchanged); **capture** of watched words, pointer chains and DTCM into the snapshot ring; network (lwIP/TLS) | capture only at a frame-done wait point: the OS_Halt idle pass (today), or the BIOS-wait hook (another proposal's mechanism; this one depends on it for the `bios` class) |
| ARM7 (module7, ARMv4T, NWRAM) | rc_client (unmodified rcheevos) + the core's evaluation: drains the snapshot ring, `rc_client_do_frame` per frame, events, rich presence, the achievement list and notices | in the game's ARM7 idle, through the existing CE7 halt hook, preempted by every game ARM7 thread |

The ARM9's frame-done work shrinks from "capture + up to 4 evaluated frames + network" (module.c:600-645) to
"capture + network". For BIOS-wait games, only the capture (~12-17k instructions, architecture.md:47, bounded
and restartable) sits in the non-preemptible main-thread wait.

### 3.2 Memory placement

| What | Where | Size |
|---|---|---|
| module7 hot code (rcheevos runtime: condset/condition/memref/operand/trigger/value/richpresence/lboard, rc_client_do_frame path; core eval + ring drain) — ARM mode | WRAM-A 0x037C0000-, if M3 shows it is free | **[estimate] 40-80 KB** (to measure from the link map) |
| module7 stack, ring indices, mailbox | WRAM-A, after the code | 4-8 KB |
| rc_client heap (conditions, memrefs, runtime arrays) + core eval tables + the eval-side copy of queued frames | k × 32 KB slots of WRAM-B/C, ARM7 master, at their existing addresses 0x0370_0000-0x037B_FFFF | Kirby-size sets ~64-96 KB [estimate]; MKDS-size ~350 KB [estimate]; > ~400 KB sets: not supported on the ARM7 (stay on today's ARM9 path) |
| cold code (JSON/rc_api parse, rc_client load path, strings) | main RAM, module region | as today; run by the ARM9 at boot, and rarely by the ARM7 (e.g. rich presence formatting, slow from main RAM but infrequent) |
| snapshot ring ARM9 → ARM7 | main RAM, module region; ARM9 writes it then cleans the lines (`dscm_dc_clean`, start.s:281-291) before publishing the head through the uncached mirror (as `dscm_publish_list`, start.s:269-276) | ~ a few hundred bytes per frame (architecture.md:53) |

Sizing per session, from the launcher's dry-run parse (the idea in memplan/rc.md:14):

- The launcher knows the set's heap need before boot, so nds-bootstrap gives only the slots needed to the ARM7.
- The rest stay with the ARM9 for ROM-in-RAM and the FNT/FAT cache, as stock.
- **Each 32 KB slot given to the ARM7 is taken from nds-bootstrap's ROM-in-RAM headroom** (main.arm7.c:912,
  `romSizeLimit += wramSize`) and from the FNT/FAT pre-cache cap (main.arm7.c:1364).
- In exchange, the main-RAM reserve (`DSCHEEVOS_CACHE_RESERVE` 0x27C000, dscheevos_hooks.h:18) can shrink by the
  heap that moved out. Net ROM-cache effect: roughly neutral [estimate].
- **Conflict:** nds-bootstrap's screen colour filter uses WRAM-C/B (0x03732800, 0x03770000). With the filter on,
  fewer slots are free. That needs either a fixed slot plan or turning ARM7 evaluation off.

### 3.3 Boot: the ARM9 parses, the ARM7 inherits (same addresses)

rcheevos' data is full of absolute pointers, and rc_client keeps function pointers to its own internals (scheduled
ping callbacks, async server callbacks). So one image must build the state and the same image must run it. Plan:

1. module7 is linked once for **ARMv4T** (`-march=armv4t -mthumb-interwork`). The ARM9 (ARMv5TE) executes v4T code
   unchanged. Hot sections are linked at WRAM-A/B/C addresses, the rest in main RAM.
2. nds-bootstrap's boot ARM9 already sees WRAM-B/C at the same addresses the ARM7 uses (MBK7/MBK8 equal on both
   sides: main.arm9.c:90-94, main.arm7.c:251-255).
   - At `module_init` time the ARM9 runs module7's init (rc_client load, from the preloaded session as today,
     module.c:426-448), allocating from an allocator whose state lives inside the NWRAM heap (not newlib's .bss).
   - WRAM-A, though, is ARM7-only (MBK1 master = ARM7, main.arm7.c:239). Either the hot code is copied there by
     the ARM7 at boot (it is position-dependent, so it must be linked there and only executed there by the ARM7),
     or the init path calls no hot function. Simplest: the ARM9 runs only the cold load path, and the ARM7 runs
     the first `rc_client_do_frame`. **[guess that rc_client's load path calls no runtime-eval function; to check
     in the link map]**
3. The ARM9 cleans and invalidates its whole D-cache. Then at `ARM9_SETSCFG` (main.arm9.c:426-434) it gives the
   module's B/C slots to the ARM7 (MBK master = 1) instead of `transferToArm9`, then locks SCFG as stock.
4. The callbacks rc_client must call on the ARM7 are re-pointed with the public setters before handover:
   `rc_client_set_read_memory_function` (rc_client.h:898), `rc_client_set_event_handler` (rc_client.h:893),
   `rc_client_set_get_time_millisecs_function` (rc_client.h:134). The server-call callback is the core's own
   function inside module7, so it is the same address on both CPUs.
   - rcheevos itself stays unmodified (hard rule).
   - The core's ARM9 module (capture) and module7 (evaluation) become two programs sharing a ring. That is a
     core split, not an rcheevos change.

### 3.4 ARM7 entry (pseudo-code)

Hook point: the existing CE7 halt hook, one added call (an nds-bootstrap hook point; CLAUDE.md lists "ARM7
VBlank/idle").

```
newSwiHalt:                         @ card_engine_header.s:445, existing
    push {lr}
    bl   runCardEngineCheckHalt     @ card reads first, unchanged
    bl   dscheevos7_idle            @ NEW: returns at once when nothing to do
    swi  #0x060000                  @ the game's halt, unchanged
    pop  {pc}

dscheevos7_idle():                  @ CE7, WRAM-A
    if !(valueBits & DSCHEEVOS_VALUEBIT) || !module7_ready: return
    if CPSR.I || !REG_IME: return            @ nothing could preempt us
    if !caller_is_lowest_priority_loop(lr): return   @ pattern from M0, per SDK
    if in7: return; in7 = 1
    sp_save = sp; sp = module7_stack_top     @ WRAM-A
    module7_idle()
    sp = sp_save; in7 = 0
```

`module7_idle` (module7, WRAM-A code, IRQs on throughout):

```
module7_idle():
    if sharedAddr[3] is a card request: return   @ CE7's work first
    drain_ring():                                @ main-RAM reads, bounded
        while ring.head != ring.tail and chunk_budget:
            copy one frame record (few hundred bytes) main RAM -> NWRAM queue
        publish ring.tail (uncached store)
    n = 0
    while queue not empty and n < MAX_FRAMES_PER_PASS:
        rc_client_do_frame()                     @ reads only the NWRAM copy
        n++
        if sharedAddr[3] is a card request: break
    deliver_net_replies()                        @ ARM9 -> ARM7 mailbox, rc_client callbacks
    publish_changes()                            @ notices / list / cues -> main RAM (uncached stores)
    return                                       @ then the real halt
```

- Preemption comes for free *if* M0 confirms the site is the lowest-priority loop. NitroSDK's ARM7 OS switches
  threads on IRQ return (as on the ARM9, architecture.md:51) [guess for ARM7, to check in M0].
- Card reads, PXI replies, the sound thread and wireless then run first. Our work resumes the next time the ARM7
  is idle, on our stack. The game's ARM7 thread context saves our SP, the same precedent as the ARM9 halt hook
  (start.s:200-206).
- The critical sections inside module7 are only the ring index updates (a few instructions). rcheevos never masks
  IRQs. ARM7 IRQ latency is therefore unchanged except for LDM/STM lengths.

### 3.5 ARM9 side changes

- `module_idle` (module.c:569-646): drop `dsc_idle`'s evaluation; keep capture, VBlank queueing and the network.
  After capture, clean the snapshot record's lines (≤ ~20 lines) and publish the head through the uncached mirror.
- BIOS-wait games: whatever hook the BIOS-wait proposal uses runs **only capture + publish** there (bounded). It
  keeps that proposal's SWI 5 rule: if a VBlank came during capture, return without calling SWI 5 again, and the
  frame is a repeat.
- `a9_waiting` flag (uncached main-RAM word): set by the ARM9 right before its WFI/SWI, cleared on return. The
  ARM7 drains the ring only while it is set. Each check is one main-RAM read, 10 bus cycles. This keeps the
  ARM7's main-RAM reads in time when the ARM9 is waiting (§4.2).

### 3.6 Why not split one frame between both CPUs, or migrate rc_client per frame

- rc_client's state (conditions, memrefs, hit counts) is touched by every `do_frame`, and its scheduled callbacks
  are code addresses of one image. The rc_client API has no supported way to run on two CPUs at once.
- Migrating per frame would mean cleaning/invalidating the ARM9 D-cache over a multi-hundred-KB heap and keeping
  one allocator coherent across CPUs.
- Rejected: one evaluator per session. The launcher picks it from the set's size and its heap fit:
  - ARM7 for sets that fit NWRAM;
  - ARM9 (today's path) otherwise.

### 3.7 The `busy` class: the only ARM7 variant that reaches it, and why it breaks the rule

A game that never halts (Golden Sun DD, Code Lyoko; idle-census.md:14, 22) gives the ARM9 no frame-done point. The
only ARM9 entry DScheevos has in such a game is the IPC IRQ that counts the VBlank (`vblank_entry`, start.s:67-113).
Variant A7-busy:

1. In `vblank_entry`, the ARM9 cleans the whole D-cache by set/way: 128 MCRs plus a write-back of each dirty line.
   Each dirty line is ~24 bus cycles ≈ 48 ARM9 cycles [estimate from gbatek.txt:7633-7642]. The ARM9 also copies
   the watched DTCM words (few) into the ring.
   - Cost: ~300 cycles with a clean cache, up to ~6.5k cycles with all 128 lines dirty. That is 0.03-0.6% of a
     frame, **inside the game's IRQ, in the game's working time.**
2. The ARM7 captures watched words and chains from main RAM right after (its VBlank IRQ comes at the same time),
   racing the resumed game. Writes to uncached lines hit RAM immediately (read-allocate cache), so the copy is a
   mix of VBlank-time and later values unless it completes first. That is weaker than today's "frame done"
   snapshot (architecture.md:46).
3. Cache-clean hazard: a clean is an early write-back. A game that is only correct because a dirty line is never
   written back before its own invalidate (DMA into a buffer, then DC_Invalidate, with the line dirty) would read
   stale data. Hardware may evict at any time, so a correct game is immune, but "correct by accident" can't be
   ruled out. This is a game-visible risk.

**This variant adds work to the game's interrupt on every frame. It does not meet "zero gameplay cost".** It is
listed for an owner ruling only, together with its bound (M6 measures it). Without it, this proposal gives the
`busy` class nothing.

Rejected outright: an ARM7 capture *without* the ARM9 clean. Hot variables can stay dirty in the D-cache for many
frames, so achievements would test wrong values. That is a correctness failure. M5 quantifies it so the rejection
rests on data.

---

## 4. Zero-cost argument and every game-visible effect

### 4.1 Claim, per class

- **`oshalt` (53/64):** the ARM9 does less than today (capture only, preemptible as today), so ARM9 cost ≤ today.
- **`bios` (9/64):** the ARM9 does only a bounded capture in the wait (needs the BIOS hook from the other proposal).
- **The ARM7 side is free under three conditions:**
  - (a) it runs only in the game's lowest-priority ARM7 context (M0);
  - (b) its code and hot data never leave NWRAM, so it adds no main-RAM bus traffic beyond the ring (M4);
  - (c) it touches main RAM only while the ARM9 waits (`a9_waiting`).
- If any of (a)-(c) fails on hardware, the claim fails, and this proposal says so rather than shipping.

### 4.2 Bus contention

- **Heap in main RAM (rejected):** loads alone are 0.3 × 10 bus cycles per 5.1-cycle instruction, so ~60-70% of
  the main-RAM bus [estimate], continuously while the ARM7 evaluates. With EXMEMCNT at ARM7 priority (0xE880 from
  nds-bootstrap, main.arm9.c:184, unless the game changes it), every ARM9 cache-line fill (23 cycles,
  gbatek.txt:7642) and every game DMA from main RAM (texture/OBJ uploads, GX FIFO DMA) can wait behind ARM7
  accesses. GBATEK says the ARM9's effective bus speed is already "barely about 8..16MHz" (gbatek.txt:7667-7671),
  so this would lengthen the game's frames: lag. This is why that placement is rejected.
- **NWRAM placement:** the ARM7's main-RAM traffic is the ring drain and the publishing:
  - per frame W words, with W ≈ 100-300 for a few hundred bytes [estimate];
  - ≤ W × ~12 ARM7 cycles ≈ 1.2k-3.6k bus cycles;
  - plus one 10-cycle flag read per chunk.
- **Gated by `a9_waiting`,** that traffic overlaps game ARM9 work only:
  - in the IRQ handlers that run between the ARM9's wake and the flag clear (at most one chunk, ~16 words ≈ 200
    bus cycles, per wake);
  - and with game DMAs that run while the ARM9 waits ("B1" in the display red-team, display-library.md 11.4).
- **The DMA overlap is a bounded but nonzero cost, R-A7-1 below.** It can't be zero: any extra main-RAM access
  during a game DMA that a thread waits on delays that DMA.
- NWRAM and main RAM are separate memories. That an ARM7 NWRAM access never stalls an ARM9 main-RAM or cache
  access is a **[guess]**, measured by M4.

### 4.3 Every game-visible effect (full list)

1. **Code patches at boot** (accepted precedent):
   - the ARM9 OS_Halt patch (existing, dscheevos_hooks.c:55-70);
   - the BIOS-wait wrapper patch (other proposal, for `bios`);
   - the ARM7 halt patch already applied by stock nds-bootstrap (patch_arm7.c:159-200), plus one call inside CE7.
2. **ARM9 capture at its wait** (existing for `oshalt`; new for `bios`): time inside the wait. For BIOS games, a
   capture that crosses VBlank must not call SWI 5 (other proposal's rule). Otherwise one lag frame.
3. **ARM7 runs instead of halting** during game ARM7 idle: more power draw and battery use [guess: small]. Not
   visible in gameplay.
4. **The game's ARM7 idle-thread context holds our SP and registers** while preempted: a game-RAM write by the
   game's own scheduler, the same as the ARM9 precedent (start.s:200-206). If the ARM7 site is not the
   lowest-priority loop, our work **delays the game's ARM7** (sound, PXI replies the ARM9 waits on: R21 in
   display-library.md 11.4). Blocker unless M0 passes.
5. **Main-RAM bus:** ring drain and publish (§4.2): ≤ ~3.6k bus cycles per frame, gated to ARM9 waits; nonzero
   while a game DMA runs during the wait.
6. **NWRAM slots taken from nds-bootstrap:**
   - ROMs that fit ROM-in-RAM only thanks to NWRAM fall back to cache mode, so loads can read the SD at run time
     (cf. MKDS's longer loads when its preload didn't fit, architecture.md:57);
   - the FNT/FAT pre-cache may not fit, so NitroFS lookups go to SD (slower file opens);
   - the colour filter feature conflicts.
   - Measurable per ROM by the dry-run (M7). Offset by a smaller main-RAM reserve.
7. **CE7 card-read latency:** our pass yields when `sharedAddr[3]` shows a request, and reads start from the FIFO
   IRQ anyway (cardengine.c:1898). But the halt-hook path (`runCardEngineCheckHalt`) runs before our pass, not
   during it. A request that arrives *during* our pass is served by the FIFO IRQ path. [to verify: every request
   raises the FIFO IRQ]
8. **SDIO shim / Wi-Fi:** unchanged (VBlank IRQ). Our pass is preempted by the VBlank IRQ as any idle work is.
9. **Busy class (only if the owner accepts A7-busy):** the D-cache clean in the game's IPC IRQ, 0.03-0.6% of a
   frame per VBlank, and the early write-back hazard (§3.7). **Not zero.**

None of these writes game data, stacks or heap, except effect 4, which is the game's own scheduler saving the idle
context, as today. Game timers are never used: ARM7 timing uses REG_VCOUNT (readable from the ARM7, 0x04000006),
and test builds use a spin counter.

---

## 5. Coverage per census class

| Class (owner's 64; RA est.) | Covered? | How | Limits |
|---|---|---|---|
| `oshalt` 53 (~88%) | yes, as today | ARM9 capture at OS_Halt; ARM7 evaluates | Better than today for ARM9-busy games (MKDS: ARM9 ~114k vs ARM7 ~160-250k instr/frame [estimate]); worse for ARM9-light games (Tetris, Phoenix: ARM9 ~280k). Sets > NWRAM stay on the ARM9 path. |
| `bios` 9 (~9%) | yes, *with* the BIOS-wait capture hook | capture-only in SWI 5 wait; evaluation fully off the main thread | Sonic Rush (69-74k) and Pac'n Roll (80k) fit; RE DS (~200k) fits at f ≥ 0.75 [estimate]. This is the strongest case for A7: no rc_client_do_frame in a non-preemptible wait. |
| `busy` 2 (~3%) | **no** at zero cost | only A7-busy (§3.7), which costs game IRQ time | Golden Sun (~220k) would also need f ≥ 0.8. |

### "Better across all games" and the display strip

- A7 is uniform on the **evaluation** side: every game evaluates the same way, in ARM7 idle, whatever the ARM9's
  wait looks like.
- It is **not** uniform on the **capture** side: the ARM9 still needs one frame-done point per wait kind.
- For the display strip (display-library.md 11.4) it helps indirectly:
  - **Bounded, short ARM9 wait path.** The ARM9's wait path no longer contains evaluation: capture, publish,
    halt. Strip arming at wait entry and "strip-only wakes go straight back to the wait without an idle pass"
    (requirement 3) become simple, since there is no idle pass left on the ARM9 except capture and network.
  - **ARM7-side signals.** The ARM7 can publish facts the red-team's step 2 asks for:
    - whether a game PXI command is in flight;
    - whether save I/O or a CE7 card read is running;
    - whether wireless is active.
    CE7 already sees the card and wireless states (cardengine.c:1743-1768, 2390-2410). This partly addresses B1
    ("main thread waiting for VBlank" vs "threads blocked on ARM7 replies").
  - It does not provide the PC ranges for "in the wait". Those still come from the ARM9 hooks.

---

## 6. Risks

1. **R-A7-0 (blocker until measured): the ARM7 hook site may not be the lowest-priority loop.**
   - If it is a wait in a loop that does game work after waking, our pass delays the game's ARM7 (sound
     underruns, late PXI replies, so the ARM9 waits: lag).
   - The site is found by a DISPSTAT-relative signature (find_arm7.c:329-360), and its role is unverified.
2. **R-A7-1: DMA during the ARM9's wait.** Ring reads while a game DMA (card, GX FIFO) runs delay that DMA by up
   to ~W × 10 bus cycles. Bounded (≤ ~3.6k bus cycles per frame) but nonzero. Mitigation: drain only when
   `GXSTAT` bit 27 is clear and no CE7 read is in flight (ARM7 can see its own; can't see ARM9 DMA, so
   **unmitigable from the ARM7**).
3. **R-A7-2: throughput is an estimate.** CPI 2.0 comes from a guessed instruction mix and an assumed 1-cycle
   NWRAM. If NWRAM is slower for the ARM7, or f is low (sound-heavy or wireless games), capacity drops below the
   ARM9's.
4. **R-A7-3: NWRAM free space.**
   - WRAM-A 0x037C0000-0x037DBFFF being unused at run time is a guess.
   - Slot assignment is fixed at boot (MBK writes need SCFG unlocked, main.arm9.c:426-434).
   - The colour LUT feature uses NWRAM.
   - DSi/3DS only: an original DS has no NWRAM, but DS-mode games booted by nds-bootstrap on DSi/3DS are the
     project's target.
5. **R-A7-4: ROM-cache regression** from taking NWRAM slots (§4.3 item 6). Mitigated by sizing per set and
   shrinking the main-RAM reserve.
6. **R-A7-5: engineering size.** This is not a patch. It means:
   - splitting the core into a capture program and an evaluation program with a cross-CPU ring;
   - an ARMv4T build of rcheevos and the core;
   - an allocator with in-heap state;
   - the NWRAM slot handover in nds-bootstrap (outside the ~4 hook points CLAUDE.md wants);
   - two evaluation modes (ARM7 for small sets, ARM9 for large), so two paths to test.
7. **R-A7-6: rc_client on two CPUs over a session.**
   - The ARM9 does the load, the ARM7 everything after. Any rc_client internal state that caches ARM9-only
     pointers (newlib's `_impure_ptr`, locale, `time()`/`gmtime()` from newlib's 64-bit time, rc.md:132) must
     also be valid on the ARM7.
   - newlib is linked into module7 itself, so it runs on both CPUs, but its static state (errno, reent) lives in
     module7's .data at fixed addresses. Fine if module7's .data is in shared memory. [guess; to check]
8. **R-A7-7: busy class not covered** at zero cost (§3.7). If the owner rejects A7-busy, these games stay
   uncovered; this angle cannot fix them.
9. **R-A7-8: melonDS blind spots.** No cross-CPU bus contention, so contention claims are hardware-only. The
   ARM7 NWRAM timing in melonDS may differ from hardware.

---

## 7. Measurements (pass/fail fixed in advance)

All hardware tests log to RAM and dump to SD only at a safe point (in-game menu / exit), per the project rule.
Each goes into TESTING.md. ARM7 timing never uses game timers. It uses REG_VCOUNT, or a spin counter (`subs r0,#1;
bne` in WRAM = 4 cycles per iteration [from gbatek.txt:91747-91760: 1S + 2S+1N]).

**M0: ARM7 hook site role (static + melonDS)**

- Method: for all 64 census ROMs, read nds-bootstrap's log ("swiHalt hooked" / "not found", patch_arm7.c:193-198).
  Disassemble the site's caller and classify it: lowest-priority loop (branch back to the call, as the ARM9
  idle-loop test at start.s:187-199), or a wait inside a working loop. In melonDS, count ARM7 halted cycles at that
  site vs at all ARM7 halts.
- PASS: in ≥ 60/64 ROMs the site is hooked, and the caller is a lowest-priority loop accounting for ≥ 90% of
  ARM7 halted time.
- FAIL: the site is a working-loop wait in any game, or is unhooked in > 4/64. Then A7 is not generic: stop.

**M1: ARM7 idle share f**

- melonDS: runner counter of ARM7 halted cycles per VBlank interval (new counter, as the ARM9 one in idle.md:9),
  same input scripts as idle.md, gameplay segments.
- DSi XL test build: CE7's halt hook spins the counter loop instead of halting until the next IRQ, and accumulates
  iterations per frame into a RAM log.
- PASS: gameplay p10 of f ≥ 0.6 in ≥ 80% of the measured games, and hardware within ±10% of melonDS for KSSU and
  Sonic Rush.
- FAIL: p10 < 0.4 in more than 20%. Then ARM7 capacity is below the ARM9's for most games: stop.

**M2: ARM7 evaluation CPI from NWRAM (hardware, decisive)**

- Method: at boot, before the game starts, the ARM7 runs `rc_client_do_frame` on recorded snapshots for 3 real
  sets: KSSU (61k), Sonic Rush (69k) and MKDS (268k). Code in WRAM-A, heap in a WRAM-B slot. 600 frames each,
  timed in VCOUNT lines (± 1 line over ≥ 600 frames). Instruction counts come from the ARMv4T build under qemu
  (realsets.sh with `-cpu arm946`: v4T code runs there).
- Repeat with the heap in main RAM.
- PASS: NWRAM CPI ≤ 2.4, and the digest equals the ARM9 build's digest (realsets.c digest).
- FAIL: CPI > 3.0 (capacity at f = 0.7 < 130k): stop.
- Expected for main-RAM heap: CPI ≥ 4. That confirms the rejection.

**M3: Is WRAM-A 0x037C0000-0x037DBFFF really free in DS-mode play?**

- melonDS: write-watch on that range (and on the chosen B/C slots) for full launcher-flow sessions of 10 games,
  including one with cheats and one with the colour filter.
- PASS: zero writes by anything but our test pattern after boot. FAIL: any write. Then find other space or stop.

**M4: Bus contention (hardware only, decisive for zero cost)**

- Probe: an ARM9 test loop in the module (boot-time test build, game not started) that streams main RAM with
  cache misses (a fixed 256 KB stride read). Time 600 iterations by VCOUNT, under:
  - (a) ARM7 halted;
  - (b) ARM7 running module7's evaluation from NWRAM;
  - (c) ARM7 evaluating with its heap in main RAM;
  - (d) ARM7 doing only the ring drain of 300 words per frame.
- Also log EXMEMCNT (ARM7 reads EXMEMSTAT 0x04000204) at boot and after 600 game frames in 10 games.
- PASS:
  - (b) within ±0.1% of (a);
  - (d) adds ≤ 300 bus cycles per frame worst case.
- FAIL: (b) > 0.5% slower than (a). Then NWRAM accesses contend after all, and zero cost fails.
- Expected: (c) clearly slower, which documents the main-RAM rejection with data.

**M5: Staleness of an ARM7 view without cleaning (melonDS cache model)**

- Method: at each VBlank, compare main RAM vs the ARM9's coherent view for every watched address of 5 real sets.
- Output: % of frames with any watched address stale, and the maximum stale age in frames.
- No pass mark: it decides nothing on its own. It documents the rejection in §3.7. Expected: nonzero for hot
  variables.

**M6: A7-busy cost bound (only if the owner wants the option priced)**

- Method: the D-cache clean in `vblank_entry`. Cycles per call in melonDS (cache model) on Golden Sun, MKDS and
  KSSU; lag A/B against stock over 6,000 intervals.
- On hardware: the T1 slack probe from display-library.md 11.4 with the clean on/off, giving X = the smallest k
  with no lag difference.
- Pass/fail is for the owner to rule, with the number. The proposal's own reading: any nonzero X fails "zero cost".

**M7: Game-visible regressions (melonDS A/B, then DSi XL)**

- For 10 games (5 `oshalt`, 3 `bios`, KSSU, MKDS), stock vs A7, 6,000 intervals:
  - lag intervals;
  - ARM9 card-read latency (request to completion);
  - ARM7 PXI reply latency (ARM9 send to ARM7 reply);
  - SND thread start jitter;
  - ROM mode chosen (ROM-in-RAM / cache) and FNT/FAT pre-cache on/off.
- PASS:
  - lag equal to stock (±0 beyond run-to-run noise measured stock vs stock);
  - p99 PXI and card latency within +1%;
  - no ROM that was ROM-in-RAM stock drops to cache mode, or the owner accepts the listed ones;
  - all frames evaluated for sets ≤ 150k.
- FAIL: any extra lag interval attributable to A7, or a ROM-mode regression not accepted.
- On the DSi XL: the same for KSSU, Sonic Rush and MKDS, with the frames-evaluated and lag counters from the
  module status, dumped at exit.

Order: M0 → M3 → M1 → M2 → M4 (all cheap, no core split needed) before building anything in §3.

---

## 8. Recommendation

1. Do **not** adopt ARM7 evaluation as the mechanism for "CPU time in all games". It can't reach the `busy` class
   within the zero-cost rule, and its throughput is unproven.
2. Do run **M0-M4 now**. They are cheap (no core split), and they settle three open questions for every other
   proposal too:
   - ARM7 idle;
   - whether bus contention exists for NWRAM-only ARM7 work;
   - whether the ARM7 hook site is safe.
3. If M0-M4 pass, A7 is the strongest way to make the **`bios` class** safe:
   - it removes rc_client_do_frame from the non-preemptible SWI 5 wait;
   - it shrinks the ARM9 wait path for the display strip;
   - it may raise evaluation capacity for ARM9-busy `oshalt` games with small-to-medium sets.
   Build it as a per-session option (ARM7 evaluator when the set fits NWRAM), after the BIOS-wait capture hook
   exists.
4. If the owner wants the busy engines covered, the A7-busy variant's cost must be measured (M6) and ruled on. By
   this proposal's reading of the hard rules it is not zero-cost.
