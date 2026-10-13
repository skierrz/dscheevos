# Proposal FB: one frame-boundary capture for every game (round 2)

Angle: take the watched data at the VBlank boundary in every game, as an emulator does once per video frame, and
evaluate it on the ARM7 (phase-2 machinery). Busy engines are then covered, and the ARM9 pays only for the capture.

Status: design only. Static analysis, code reading and arithmetic. No emulator was run.

Conventions:
- Citations are file:line, GBATEK (local copy `scratchpad/dl/gbatek.txt`, line numbers), or disassembly addresses.
- **[est]** marks arithmetic from cited numbers. **[guess]** marks anything unsupported.
- Frame = 1,120,380 ARM9 cycles at 67 MHz (AB.md:29). Line = 4,260 ARM9 cycles (560,190 / 263 bus cycles × 2).

---

## 0. Verdict in one screen

1. **The emulator reference point is not line 192.**
   - melonDS ends `RunFrame` when `GPU::FinishFrame` runs at the end of line 262 (scratchpad/dl/mel/src/GPU.cpp:1179-1189; NDS.cpp:980, 1049-1052).
   - So RetroArch's once-per-frame rcheevos call sees memory at the line 262→0 transition, 71 lines after VBlank starts. ARCHITECTURE.md:55-57 found the same.
   - A line-0 entry is not available at zero resource cost:
     - all four ARM9 NDMA channels are taken (ce9 has NDMA0, display has NDMA1-3: design-notes.md "Display ... Gate must not use NDMA 1-3 (display) or 0 (ce9)");
     - VCOUNT/DISPSTAT belong to the game on both CPUs;
     - game timers are forbidden.
   - **Line 192 (the game's own VBlank IRQ) is the only universal frame anchor we can reach for free.** Neither 192 nor round 1's halt point equals line 0. RA sets already have to tolerate "any once-per-frame sample" (ARCHITECTURE.md:56-58).
2. **Capture point: before the game's VBlank handler, at the game's dispatcher** (the "first instruction after the IRQ" in practice).
   - Hook: one entry of the game's IRQ table, VBlank (entry 0), through a pool of trampolines.
   - That is the same class of runtime write nds-bootstrap already makes to entries 2 and 16 (cardenginei/arm9/source/misc.c:112-125), and its own code says "The first entry in the table is for the Vblank handler, which is what we want" (bootloaderi/arm7/hook_arm9.c:167).
3. **The halt capture becomes a precomputation of the frame-boundary (FB) capture, not a separate semantic.**
   - A halt-time copy with no game CPU instruction executed between its completion and the VBlank is, by construction, the memory at the pre-handler VBlank instant, except for DMA/ARM7 writes. That exception applies to an in-IRQ copy too.
   - So every frame of every game is sampled at **one instant: VBlank, before the game's handler**.
   - At each VBlank the FB hook:
     - commits the fresh halt copy if there is one (≈100-250 cycles, [est]);
     - otherwise captures right there (cost C).
4. **Should FB replace halt-time capture in all games? No.** It should replace three things:
   - the **carry** path (lag frames evaluated with stale memory, architecture.md:46);
   - round 1's **`frames_uncaptured` / `captures_missed`** (decision.md:160-168);
   - today's **ARM7→ARM9 IPC-0xA VBlank path** (CE7 cardengine.c:2062-2064 → CE9 cardengine.c:1644-1648 → start.s:67).

   Replacing halt capture everywhere would add C of game time to ~97% of games, for no correctness gain over the fresh halt copy. It would also make chain-heavy sets (Platinum, HG/SS) unshippable (§4).
5. **Cost:**
   - **Waiting games, non-lag frames:** less game time than today. The fresh path is ≈100-250 cycles [est] versus today's ≈1.4-1.7k cycles per frame for the 0xA IPC IRQ:
     - nds-bootstrap's IPC handler, 750-880 cycles (AB.md:27);
     - `vblank_entry`, 641 avg on Kirby (baseline.md:29).
   - **Busy engines and lag frames:** C per frame inside the game's IRQ. For typical real sets C ≈ 2-6k cycles = **0.2-0.55 % of a frame** [est, §4]. Its floor is set by the plan's cache lines and changed words (§5).
   - **Not zero.** The test that proves "no added lag" is §8 FB-M3: deterministic delay injection in melonDS with frame hashes, then hardware.
6. **Busy engines also need things this angle does not give them** (§9):
   - an ARM7 evaluator whose throughput fits (GS DD's set is ~211-227k ARM9 instructions per frame, A7 proposal §1.6);
   - **somewhere to run the network**: in a game that never waits, the ARM9 lwIP/TLS stack gets no time at all.

---

## 1. Facts this rests on

| # | Fact | Source |
|---|---|---|
| F1 | Today the ARM9 learns of a VBlank from the ARM7: CE7's VBlank hook runs *after* the game's ARM7 VBlank handler (return-address hook), bumps the module's count, sends `IPC_SendSync(0xA)`. The ARM9's CE9 IPC hook runs after the game's IPC handler and calls `header->vblank` | CE7 card_engine_header.s:184-201, cardengine.c:2062-2064, 2119-2141; CE9 card_engine_header.s:640-656, cardengine.c:1620-1648 |
| F2 | So today's module VBlank point is line 192 + T7 (ARM7 IRQ entry + game ARM7 handler + cheat engine + count) + the ARM9 IPC dispatch. Its order relative to the game's own ARM9 VBlank handler and the woken main thread is not fixed | F1; T7 unmeasured **[guess: 1-10k ARM9 cycles]** |
| F3 | The GSDD CE9 build skips the IRQ-table hooks and the DScheevos call entirely (`#ifndef GSDD`), so Golden Sun DD gets no module VBlank call today | CE9 misc.c:113, 129; cardengine.c:1627 |
| F4 | `vblank_entry` + core already implement "copy in the IRQ on the module's IRQ stack" (`dsc_vblank_copy`, MODE_COPY); today it is used only for a session's first frame | start.s:128-141; core dscheevos.c:2751-2838; module.c:516-520 |
| F5 | Earlier builds copied in the IRQ every frame: synthetic random-address set, 12k instr ≈ 108k cycles (9.7 % of a frame). Platinum went from 50 to 55 lag intervals. The real-set copy is ~10-50x smaller (F7) | tests/perf/AB.md:98-110 |
| F6 | The IRQ-time copy reads through the ARM9 D-cache, so it sees dirty lines. Watched lines are dirty at VBlank in Kirby 9 %, MKDS 1 %, Sonic Rush 61 %, Platinum 88 % of intervals | evalspeed-team/arm7-notes.md:62-72 |
| F7 | Capture size and cost per frame:<br>- qemu `dsc_capture` instructions: Kirby 873, MKDS 1,540, Sonic Rush 1,084, Phoenix 844, Black 2.2k, TWEWY 6.1k, Platinum 23.5k, Digimon 21.6k, HG/SS 121.7k (4,096-entry table overflow, reads go live). Several are on random RAM with an older core.<br>- Measured cycles at the halt (cache model): Kirby 5.9k per capture (223.8k / 38.1 wakes), MKDS ~4k per frame | brief2; baseline.md:24; capture-notes.md:17-21 |
| F8 | Per-frame read sets (words / 32-byte lines): MKDS 56 / 26, Platinum 189 / 77, SoulSilver 562-849 / 195-274, TWEWY 948 / 147, Digimon 345 / 214. **None of these reads DTCM**, but RA's DS map includes it | docs/rnd-r4-deferred.md:29-38; rcheevos rc_consoleinfo.c:738-753 |
| F9 | ARM9 data timings (33 MHz units): main-RAM cache line fill 23; main RAM N32 = 10 (cache off); WRAM N32 = 4 | GBATEK gbatek.txt:7632-7642 |
| F10 | After boot, nds-bootstrap clears SCFG_EXT9 bit 31 for DS-mode games, so MBK1-8 can no longer be written. An NWRAM slot belongs to one CPU for the whole session, so **there is no NWRAM buffer that the ARM9 writes and the ARM7 reads in play** | bootloaderi/arm9/main.arm9.c:426-436; GBATEK 24311, 27741-27742 |
| F11 | NitroSDK dispatchers ack IF, then call the handler of the **lowest** pending bit, in IRQ mode with I set. So VBlank (bit 0) is always dispatched first | KSSU/MKDS/Sonic Rush V1 (hook_arm9.c:46-52, 70-75); DKR 0x020A5B08-0x020A5B58 (below); GS DD 0x0207BA80-0x0207BAB0 (below) |
| F12 | Census: 53 OS_Halt / 9 BIOS / 2 busy (+ DKR's 3D engine). GS DD's hot loop is a VCOUNT/time-based scheduler spin (0x020371A4-0x020373B8); Code Lyoko's is `flag=0; while(!flag) poll(obj)` (0x020571CC); DKR's is a game thread spinning on ring indices (0x0201A344 / 0x0201A294) | idle-coverage branch docs/idle-census.md:11-34, 61-62 |
| F13 | ARM7 free time in gameplay is 83-92 % (p10) in 8 games. Loads drop the worst 1024-window to 44-80 % | arm7-notes.md:41-60 |

### 1.1 Dispatcher and VBlank table entry per busy engine (new static work)

The ARM9 static binaries were extracted with `waitscan.arm9_static` and disassembled with llvm-objdump (armv5te). The scripts are in `scratchpad/fb/`.

| Game | Dispatcher | Table / VBlank entry | nds-bootstrap today |
|---|---|---|---|
| Code Lyoko | V1 signature at static +0xF4400 | literal 0x027E0000 → **entry 0 = 0x027E0000** | matched by V1 (hook_arm9.c:46-52) |
| Golden Sun DD | TwlSDK-style, static 0x0207BA80 (`mov ip,#0x4000000; ldr r2,[ip,#0x208]!; ldrd r0,r1,[ip,#8]!`) | dispatch is `rsb r0,r1,#0; and r1,r0,r1; clz r0,r1; ldr lr,[lr,-r0,lsl #2]` with lr = literal 0x027E00BC (0x0207BA98-0x0207BAA8, pool 0x0207BBF4). So the literal is the bit-31 end of the table, and **VBlank = 0x027E00BC − 31×4 = 0x027E0040** [derived] | `handlerStartSigAlt5` matches, but `tableAddr = addr[0]` (hook_arm9.c:153-168) returns 0x027E00BC, the table *end*. That is a likely reason the GSDD CE9 hooks no table entries [guess] |
| Diddy Kong Racing | V1 variant at static 0x020A5B08: no leading `push {lr}`; `ands r1,r1,r2` / `clz` lowest-bit loop / `str r1,[ip,#4]` ack / `ldr r2,[r1,r0,lsl #2]`; `push {lr}` only `if bit != 10` | literal 0x027E000C → **entry 0 = 0x027E000C**. Return literal 0x01FFF8C4 = ITCM ThreadSwitch | **matches none of the three start signatures** (hook_arm9.c:46-68: V1 needs `e92d4000` first, Alt needs `e14f0000` 4th, Alt5 differs). `hookNdsRetailArm9`'s return is ignored (main.arm7.c:2511). Today's DKR IPC hook, and so DScheevos' VBlank count in DKR, is **suspect**: check the bootstrap log for `ERR_HOOK_9` |
| KSSU / MKDS / Sonic Rush (reference) | V1 | entries 0x027E0FE0 / 0x027E0008 / 0x027E0004 | matched |

---

## 2. Mechanism

### 2.1 Entry: VBlank table-entry trampoline (E2)

At the first module VBlank call after the game's IRQ table is live (§2.6), the module writes `table[0] = tramp_k`. The current value is stored as `orig_k`.

Each pool slot `tramp_k` (k = 0..15) is 12 bytes in the module image:

```
tramp_k:  ldr  r12, [pc, #0]     @ r12 = orig_k
          b    fb_entry
          .word orig_k
```

Why a pool:
- Games often save and restore VBlank handlers: `old = OS_GetIrqFunction(V_BLANK); OS_SetIrqFunction(V_BLANK, B); ...; OS_SetIrqFunction(V_BLANK, old)`.
- With a single stub, `old` would be our stub, and the stub's single `orig` would by then point at B. A restore would call B instead of A, which is a game-visible bug.
- With a pool, each hook generation gets its own address that carries its own `orig`. A restored `tramp_A` still calls A.
- Slots are recycled least-recently-installed first. Sixteen slots make it practically impossible to recycle one the game still holds [guess; FB-M6 checks].

`fb_entry` is ARM code in the module's IRQ cache-line group (start.s:55-67 placement). It is entered in IRQ mode, I set, from the dispatcher's `bx`/`blx`, so r0-r3 and r12 are free and lr is the dispatcher's return:

```
fb_entry:
    push  {r4, r5, r12, lr}          @ 4 words on the game's IRQ stack (today: 3, start.s:68)
    adr   r4, fb                     @ one cache line: state below
    ldr   r0, [r4, #FB_FRESH]
    cmp   r0, #0
    bne   fresh                      @ (a) a halt copy is valid for this instant
    ldrb  r0, [r4, #FB_ON]
    cmp   r0, #0
    beq   out
    mov   r5, sp                     @ (b) capture now, on the module IRQ stack
    ldr   sp, =__irq_stack_top       @ (only if not already on it: decision.md:172)
    bl    fb_capture                 @ copy plan -> open ring slot, publish
    mov   sp, r5
    b     count
fresh:
    mov   r0, #0
    str   r0, [r4, #FB_FRESH]
    bl    fb_publish_open_slot       @ head++ (one uncached store + drain), ~30 instr
count:
    ...   frames++, fb_seen = arm7 count (uncached store), stats
out:
    pop   {r4, r5, r12, lr}
    bx    r12                        @ tail-jump to the game's VBlank handler, lr intact
```

Ordering:
- **Pre-handler, deliberately.** The game's VBlank handler runs after our capture.
- This keeps the FB capture and the fresh halt copy at the *same instant*.
- A post-handler capture would include the handler's writes (frame counters, input latches) in some frames and not in others, so hit counts on such values would jitter with the path taken.

### 2.2 What `fb_capture` does (busy-engine frames, lag frames, no fresh copy)

- This is today's `dsc_vblank_copy` path, unchanged in kind:
  - `copy_snapshot` (dscheevos.c:2510-2598) and `run_chains` (dscheevos.c:964-1061);
  - with `S.capturing = 0`, so `stop_now()` never fires (dscheevos.c:808-811);
  - no `interrupt_pending` polls (module.c:163-172; 9.1-9.8k cycles/frame on Kirby, baseline.md:24), no restarts.
- **Reads:**
  - main RAM through the cached 0x02000000 mirror (module.c:145-149): coherent with the game's dirty lines (F6);
  - DTCM through the CP15 c9 base (start.s:351-358, module.c:177-181): ARM9-only, uncached, about 1 cycle per word.
- **Pointer chains:**
  - Roots and targets are read at the same instant.
  - `CF_DELTA` / `CF_PRIOR` steps need the previous frame's chain values. FB captures exactly once per VBlank (no restarts, no carry), so the chain state advances exactly once per frame.
  - **Requirement on the capture lane:** chain state must be a pure function of (RAM, immutable chain program, previous *committed* slot). Today `sync_save` / `sync_restore` mutate the core's chain copies in place (dscheevos.c:1120-1141).
    - In IRQ context, an FB capture can preempt a half-done halt copy in W.
    - With in-place state, FB would read dirty state, or W's later `sync_restore` would undo FB's frame.
    - Double-buffered chain values kept in the slot remove both hazards. This is arm7-notes.md R6's restructuring, and it is needed for the ARM7 anyway.
  - `CS_GENERIC` steps call rcheevos' `rc_get_modified_memref_value` (dscheevos.c:858-862). That is allowed in IRQ context (no global state is touched) **[guess, to verify by reading rc_operand / rc_memref]**, but it counts toward C and pulls rcheevos lines into the I-cache.
- **Live reads are not allowed** (Remember/Recall chains, table overflow):
  - On the ARM7 they would be stale or impossible (DTCM). On a busy engine there is no ARM9 idle to do them later.
  - A set with live reads can't be evaluated on a busy engine until the capture plan covers them (CAPTURE lane).
  - The launcher's dry run knows this before boot. The coverage record says so.
- **Destination: the open ring slot in main RAM, written through the uncached mirror** (module mirror +0x0A000000: start.s:74, 271).
  - Then a write-buffer drain (`mcr p15,0,r0,c7,c10,4`, start.s:274) before the head store.
  - Not NWRAM: F10.
  - Stores to uncached memory don't allocate D-cache lines (read-allocate cache, facts-hw.md §5), so the destination causes no cache pollution.
- **Delta encoding (proposed; interface with rnd/arm7eval):**
  - Keep a cached private shadow of the last published words (module RAM). Compare as we read (the existing `copy_words_same`, dscheevos.c:2478-2506).
  - Write only `(index, value)` for changed words, plus the chain targets that changed, to the uncached ring.
  - Between dumps 100-2,000 frames apart only 1-13 watched bytes changed (rnd-r4-deferred.md:44-46). The per-frame change rate is unmeasured. Changed words, not N, then drive the uncached-store cost.
  - The ARM7 rebuilds full slots in its NWRAM.

### 2.3 Fresh halt copies: the exact rule that makes them equal to the FB instant

Definition: `fresh` = 1 at the VBlank dispatch iff the newest *complete* halt copy was finished and **no game instruction executed after it**.

The module sets `fresh` only at its own halt, which already runs with I masked (round 1: E/W halt with I masked, decision.md:101; today's `capture()` returns masked and the caller halts, module.c:183-189, architecture.md:46):

```
    [I masked] copy completes, copy_valid = 1        (any unpark/game IRQ in between: copy_valid = 0)
    [I masked] wfi                                    (wakes on IE&IF, I masked: no handler yet)
    [I masked] fb.fresh = copy_valid && (REG_IE & REG_IF & 1)
    restore CPSR -> IRQ taken -> dispatcher -> bit 0 first (F11) -> fb_entry sees fresh
```

- **Non-VBlank wake:** `fresh = 0` before any game code runs. A game handler then runs, so the copy is stale. The idle path re-copies (round 1 rule) or not (§2.5).
- **Simultaneous VBlank + other IRQ:** VBlank is dispatched first (lowest bit, F11), so the copy is still exact at the boundary.
- **A thread switch at IRQ return** can't leave a stale `fresh`, because `fb_entry` consumes it in the same dispatch.

What a fresh copy and an FB capture can still differ by:
- DMA and ARM7 writes in the interval. A pure FB capture has the same exposure *during* its own C cycles.
- Cache staleness of ARM7-written lines the ARM9 holds clean: the game sees the same values.

So both paths sample "memory at VBlank, before the game's handler" **up to non-CPU writers**.

### 2.4 Coherent hand-off to the ARM7 evaluator (what we give rnd and arm7eval)

| Item | Value |
|---|---|
| **T** (data valid) | VBlank IRQ dispatch + entry (≈ 20-60 cycles) + C. Fresh path: the halt copy was already complete, so T = dispatch + ≈100 cycles |
| **X** (ARM9 cost per frame) | fresh path ≈100-250 cycles [est: ~25 instructions, 1-2 I-lines and 1 D-line at 46 cycles each if cold, F9]. FB path = C (§4, §5) |
| Where | main-RAM ring through the uncached mirror. Head published after a write-buffer drain. Never NWRAM (F10). The ARM7 copies slots into its own NWRAM. That drain is main-RAM bus traffic and is A7-M4's question (§9 R4) |
| Order | one slot (or one repeat count) per VBlank, frame numbers contiguous. VBlanks with no ARM9 entry (IE bit 0 or IME off, §9 R7) are numbered from the ARM7's count and marked "no capture" |

### 2.5 Halt-copy policy, now tunable because FB backs it up

Round 1 copies at every wake in Kirby: 38.1 wakes/frame, 223.8k idle cycles/frame and 2,843 line fills/frame (capture-notes.md:17-21). Those fills are part of the game's refill debt (+21 %).

With FB as the guaranteed fallback, the halt path may copy **only when the VBlank is near**: VCOUNT ∈ [192 − L, 191].
- If the wait ends earlier and another wake follows, nothing is wasted.
- If no fresh copy exists at VBlank, FB captures (C in game time).
- L is chosen per game class by FB-M4's metric (the game's own ARM9 cycles per frame, off vs on: brief2 game-cost rule), not by guess.
- L = 263 is round 1's behaviour; L = 0 is "FB always".

### 2.6 Install, re-install and the ARM7 watchdog

- **Table location:**
  - V1 / Alt from nds-bootstrap's `hookInterruptHandler` (hook_arm9.c:96-168);
  - Alt5 corrected to `tableAddr − 31×4` (§1.1);
  - a 4th start signature for the DKR variant (`mov ip,#0x4000000; add ip,ip,#0x210; ldr r1,[ip,#-8]; cmp r1,#0; bxeq lr`).
  - These are signatures, not a game list. FB-S1 measures coverage over all 64 ROMs.
  - The table address goes to the module in the header at boot (new field, ABI v7 shared with round 1).
- **First install:** from the first module VBlank call of any kind (today's IPC path), or from CE9 `initialize()` for GSDD. The game has run `OS_InitIrqTable` by then.
- **The game overwrites `table[0]`** (a new handler): FB stops being called. Detection, at zero steady-state cost:
  - Each FB entry stores `fb_seen = vblank_count` through the uncached mirror.
  - CE7's VBlank hook already reads module words every VBlank (cardengine.c:2001-2002). It adds one compare: if `vblank_count − fb_seen ≥ 2`, it sends `IPC_SendSync(0xA)` that frame only.
  - CE9 → `vblank_entry` then re-installs into a new pool slot and captures through the old IPC path (late point, counted `fb_late`).
  - Healthy cost: one ARM7 main-RAM read per frame.
- **Retire the unconditional 0xA:** once FB is installed, CE7 sends 0xA only in the watchdog case. The `vblank_count` bump stays: it is the frame clock for frames_missed (architecture.md:47).
- **Reset / quit:**
  - `reset_entry` writes back `orig` of the slot found in `table[0]`, if it is one of ours.
  - The game's threads are gone on reset (start.s:216-218), and the table is re-initialized by the game, so the watchdog re-installs.

### 2.7 Alternatives considered for the entry, and why not

| Entry | Point | Cost | Why not primary |
|---|---|---|---|
| E1 today's IPC-0xA path | 192 + T7 (F2): after the game's ARM9 VBlank handler, sometimes after woken game code | ≈1.4-1.7k cycles/frame already today | wrong instant for exactness (mixes handler and early-logic writes); not in GSDD (F3); questionable in DKR (§1.1). Kept as watchdog fallback |
| E3 permanent DTCM+3FFC pre-dispatch stub | BIOS IRQ entry, any dispatcher | **every** game IRQ pays a check: ≈30-40 cycles hot, ≈80-120 cold [est]. Kirby ~40 IRQs/frame → 1.2-4.8k; H-Blank-IRQ games 263/frame → 8-30k | cost on non-VBlank IRQs, raster jitter. It also conflicts with round 1's invariant I1 (decision.md:133). Only if the owner refuses the table write *and* E2's signatures miss a game |
| R4 boot patch of `OS_IrqHandler` | same as E3 | same as E3 (decision.md:145-148) | same |
| Line-0 IRQ (melonDS-exact) | 0 | needs a module timing IRQ in game time plus E3 | no free source: NDMA0-3 taken, VCOUNT and timers are the game's (§0.1) |

---

## 3. Correctness against RA semantics

| Case | Round 1 (halt copy + carry) | FB (this proposal) | RetroArch (melonDS) |
|---|---|---|---|
| Waiting game, non-lag frame | memory at the last halt before VBlank, restarted if game code ran | **identical** (fresh copy = FB instant, §2.3) | line 0: includes the VBlank handler and any next-frame logic before line 0 |
| Waiting game, lag frame | **carry:** last frame's memory again (architecture.md:46: "RetroArch would see the game's half-updated memory there") | **mid-logic memory at VBlank**, like RetroArch's mid-logic sample (different instant) | mid-logic at line 0 |
| W busy for the whole wait (heavy eval, RE:DS) | P1a: `captures_missed`; P1c: needs capture context C | FB captures (C) | — |
| Busy engine (GS DD, Code Lyoko, DKR) | nothing | **every frame**, at VBlank, game ARM9 frozen during the copy | arbitrary logic phase at line 0 |
| Torn by game CPU code during the copy | possible; detected and restarted | **impossible:** IRQ mode, I set, dispatcher does not nest (F11) | impossible (instant) |
| Torn by DMA / ARM7 during the copy | yes, over the copy's span | yes, over C (≈30-90 µs typical [est]) | no |
| Hit counts / deltas | one evaluation per VBlank; carry frames add hits on stale data | one evaluation per VBlank on that frame's memory | one per frame |

Two reference points make this exact rather than approximate:
1. **Fresh halt copy ≡ pre-handler FB capture** (§2.3), so a game's frames are never mixed between two instants.
2. The remaining distance to RetroArch is the 192-vs-0 phase. It applies to round 1 too, and no free mechanism closes it (§0.1).

What FB fixes against round 1: lag frames and uncaptured frames now carry real memory, as an emulator would.

Edge cases:
- **DMA into watched RAM at VBlank** (VBlank-mode DMAs mostly target VRAM/OAM, outside RA's map): a word can be pre- or post-DMA. Rare **[guess]**; FB-M2 lists every differing address.
- **ARM7-written variables** in lines the ARM9 holds clean: the capture sees what the game sees, which may differ from melonDS (no cache model).
  - The SDK keeps ARM7-shared areas uncached (KSSU crt0 PU regions 0x027E0000 128 KB and 0x027FF000 4 KB, design-notes.md "Verified").
  - Same for round 1.
- **Our 4 pushed words** below the game's IRQ SP (DTCM): popped before any game code runs. Precedent: 3 words today (start.s:68).

---

## 4. Cost per set: C in the game's IRQ

Model **[est]**:
- low end = MKDS's measured ≈4k cycles for 1,540 instructions → **2.6 cycles/instr**;
- high end = Kirby's 5.9k per capture for 873 instructions → **6.8 cycles/instr**. That includes `interrupt_pending` and `arena_alloc`, which the IRQ path does not run.
- Capture is memory-bound (F7, F9).

| Set | dsc_capture instr | C low – high (cycles) | % of frame | lines (4,260 cyc) | IRQ-eligible at C_cap = 8k? |
|---|---:|---:|---:|---:|---|
| Phoenix Wright | 844 | 2.2k – 5.7k | 0.20 – 0.51 % | 0.5 – 1.3 | yes |
| Kirby SSU | 873 | 2.3k – 5.9k | 0.20 – 0.53 % | 0.5 – 1.4 | yes |
| Sonic Rush | 1,084 | 2.8k – 7.4k | 0.25 – 0.66 % | 0.7 – 1.7 | yes |
| MKDS | 1,540 | 4.0k – 10.5k (measured ≈4k) | 0.36 – 0.94 % | 0.9 – 2.5 | yes (measured) |
| Pokémon Black | 2.2k | 5.7k – 15k | 0.5 – 1.3 % | 1.3 – 3.5 | measure |
| TWEWY | 6.1k | 16k – 41k | 1.4 – 3.7 % | 3.7 – 9.7 | no, today |
| Digimon | 21.6k | 56k – 147k | 5 – 13 % | 13 – 35 | no |
| Platinum | 23.5k (random RAM, old core) | 61k – 160k | 5.5 – 14 % | 14 – 38 | no |
| HG/SS | 121.7k (table overflow) | 316k – 828k | 28 – 74 % | — | no; the plan is broken until the table holds the set |
| GS DD, DKR (busy) | **not measured** | — | — | — | FB-M1 first |

Reading:
- **Typical real sets cost 0.2-0.55 % of a frame per VBlank.** That is ~10-50x less than the earlier in-IRQ copy of the synthetic set (F5), because the copy program and the real (clustered) addresses replace random-address reads.
- **Chain-heavy sets don't belong in the IRQ.** That is why FB doesn't replace halt capture: in waiting games those sets stay on the zero-game-time halt path, and FB only covers their lag frames under a cap (§6 R3).
- **IRQ latency side effect:** any other ARM9 IRQ that arrives during [VBlank, VBlank + C] waits up to C.
  - That window is VBlank time (lines 192-194 for typical C), so raster H-Blank effects on visible lines (0-191) are untouched.
  - ARM9 timer IRQs see up to C of extra latency. Sound runs on the ARM7 and is unaffected.

---

## 5. The minimum cost, exactly (busy-engine frames and FB-path frames)

```
C_min = E + Σ_lines miss(l)·46 + N_w·2 + N_chg·20 + S_chain + E_pub
```

- **E:** trampoline + `fb_entry` + stack switch ≈ 40-120 cycles [est].
- **miss(l):** source lines not already in the D-cache.
  - The game has just finished its frame, so its hot watched lines are often resident. Dirty is a subset of resident.
  - Cold worst case is 46 ARM9 cycles per line (F9: 23 bus cycles).
- **N_w:** watched words: loaded plus compared with the cached shadow, ≈2 cycles each on hits.
- **N_chg:** changed words: an uncached main-RAM store each. ≈20 cycles of write-buffer drain (F9 N32 = 10 bus cycles), mostly overlapped with the next loads.
- **S_chain:** chain steps. A compiled leaf is a root load + a target load + 2 stores, ≈10-60 cycles depending on misses. `CS_GENERIC` steps cost far more.
- **E_pub:** drain + head store ≈ 30 cycles.

Worked numbers **[est]**:

| Set | Inputs | Worst (all cold) | Hot |
|---|---|---|---|
| MKDS | 26 lines, 56 words, a few changes | 26×46 + 56×2 + 5×20 + 150 ≈ **1.6k cycles (0.14 %)** | ≈ 0.4k |
| Kirby | 33 lines (arm7-notes.md:68) | ≈ **1.9k** | ≈ 0.5k |

These floors are below the measured 4-6k because today's capture also writes a cached arena (2,194 write misses/frame on Kirby across wakes, capture-notes.md:20), polls interrupts, and allocates.

- **Provably zero? No.** Every C cycle is game time in a frame where the game did not wait.
- **The no-lag proof** is the test in §8: FB-M3 (deterministic delay injection) plus FB-M4/FB-H1 (A/B), with pass marks fixed now.

---

## 6. Every game-visible effect, with its bound

| # | Effect | When | Bound / test | Ruling |
|---|---|---|---|---|
| G1 | DScheevos writes the game's VBlank IRQ-table entry (DTCM) at run time, and again after the game replaces it | install, rare re-install | same class as nds-bootstrap's own entry-2/16 writes (misc.c:112-125). Restore semantics kept by the trampoline pool (FB-M6) | **R1** |
| G2 | `OS_GetIrqFunction(V_BLANK)` returns a module address | if the game reads it | pool keeps save/restore correct; calling it calls the original | R1 |
| G3 | 4 words pushed below the game's IRQ SP | every VBlank | popped before game code; today 3 words | no |
| G4 | Pre-handler delay before the game's VBlank handler: fresh ≈100-250 cycles; FB path C | every VBlank | FB-M3 frame hashes (also catches VBlank-upload overruns), FB-M4 lag | **R2** for busy |
| G5 | Added latency of other ARM9 IRQs, ≤ C, within VBlank time | FB-path frames | FB-M5 | R2 |
| G6 | Cache pollution: shadow lines + source lines not resident + code lines (~1 KB) | FB-path frames | in FB-M4's game-cycles metric | R2 |
| G7 | 0xA IPC IRQ removed in steady state: **≈1.4-1.7k fewer game-IRQ cycles per frame** than today | always | FB-M4 | R4 |
| G8 | ARM7 compares one word per VBlank (watchdog); sends 0xA only when FB is missing | VBlank (ARM7) | negligible | no |
| G9 | ARM7 reads the ring from main RAM while the busy ARM9 also uses the bus (no "a9_waiting" gating is possible in busy engines) | busy engines | ≤ N_chg + header words × ~10 bus cycles per frame [est]. A7-M4 (d) must also measure with the ARM9 busy | R2 |
| G10 | Busy-engine scheduling phase shift (GS DD's time-based scheduler sees all its work start C later) | busy engines | FB-M3 frame-hash equality is the direct test | R2 |

**Not written:** game heap, data, thread stacks, IE, IF, DTCM+3FF8/3FFC, timers, DISPSTAT, VCOUNT.

---

## 7. Coverage

| Class | Capture | Evaluation | After FB |
|---|---|---|---|
| `oshalt` 53/64 | fresh halt copy (0 game time); lag frames FB | ARM9 W or ARM7 | as round 1, plus real memory in lag frames, minus the IPC IRQ |
| `bios` 9/64 | fresh copy in E/W (P1a); FB when W is busy or the frame lags | ARM9 W or ARM7 | P1c's capture context becomes optional |
| `busy` ≥ 3/64 | **FB every frame** | **ARM7 only** (no ARM9 time) | covered iff (a) the set is IRQ-eligible (FB-M1), (b) the ARM7 fits it (A7-M2, arm7eval), (c) the network question is solved (§9 R2). Code Lyoko has no RA set (census: "no"); GS DD 11788 and DKR 9546 do |

---

## 8. Measurements (pass/fail fixed now)

Rules:
- melonDS: one slot, cache model, `--thumb-cycle`.
- Hardware: logs to RAM, dumped at quit or the menu only, written up in TESTING.md.

| ID | Where | What | PASS | FAIL → |
|---|---|---|---|---|
| FB-S1 | static, 64 ROMs | dispatcher variant (V1 / Alt / Alt5 / DKR-variant) and the VBlank entry address; GS DD's −31×4 rule checked against a runtime table dump (melonDS memory read at frame 600) | 64/64 located; dump shows `table[0]` = the game's VBlank function | E3 for the misses (owner) |
| FB-S2 | static + log | does nds-bootstrap hook DKR's IRQ table today (`ERR_HOOK_9` in its log; value of `ce9->irqTable`) | report; if it is unhooked, file the 4th signature with nds-bootstrap first | — |
| FB-M1 | melonDS | C per VBlank, FB path forced every frame (test flag), 6,000 intervals: Kirby, MKDS, Sonic Rush, Phoenix, Black, TWEWY, Platinum, Digimon, SoulSilver, **GS DD, DKR**. p50 / p99 / max cycles, D-misses, uncached stores | per set: report. "IRQ-eligible" = p100 ≤ C_cap (proposed 8,000 cycles, owner may change) | sets above C_cap: capture-lane work, or halt-only in waiting games |
| FB-M2 | melonDS | equivalence: in the test build every frame takes both the fresh halt copy (when one exists) and the FB capture, and compares bytes and targets. 10 waiting games × 10,000 frames | identical in ≥ 99.99 % of fresh frames; each difference at an address written by DMA or the ARM7 in that window (runner `--watch-write` attribution) | design error in §2.3: stop |
| **FB-M3** | melonDS (deterministic, Wi-Fi off, achievements OFF) | **delay injection:** a test stub on the FB trampoline spins D cycles pre-handler, D ∈ {0, C_set, 2C, 4C, 8C}; same input script; per-frame framebuffer hash + "frame done" arrival, 10,000 intervals. "Frame done" = idle arrival for waiting games (AB.md:35-44); for busy engines a test-only PC range: GS DD scheduler-spin entry 0x020371A4-0x020373B8 with no runnable task, Code Lyoko loop at 0x020571CC, DKR consumer spin around 0x0201A294 / 0x0201A344 (census addresses; confirm by disassembly whether they are the loop's code or the ring indices) | for each game, **at D = 4×C_set: zero frame-hash differences and zero lag-interval differences** against D = 0. Also report D_min, the smallest D with any difference | D_min < 4×C_set: that game is not provably lag-free at C_set → owner (R2) with the number |
| FB-M4 | melonDS A/B | stock vs FB build (achievements on, ARM9 evaluator off or ARM7 stub) in KSSU, MKDS, Platinum, Sonic Rush, GS DD, DKR, Code Lyoko: lag intervals, **game's own ARM9 cycles per frame** (brief2 rule), module cycles in IRQ mode | lag = stock within stock-vs-stock noise; game cycles per frame: fresh-path games ≤ today's build | — |
| FB-M5 | melonDS | IRQ latency: IF-set → first instruction of each game handler, for non-VBlank IRQs during FB frames | Δp100 ≤ C_set + 200 cycles; no H-Blank IRQ on lines 0-191 delayed | — |
| FB-M6 | lab ROM + games that swap VBlank handlers | save / set / restore churn 10,000×; reset mid-session | right handler called every time (counter per handler); ≤ 1 `fb_late` frame per game-side overwrite; 0 frames lost | — |
| FB-M7 | melonDS | busy engines with an ARM7 evaluator stub that consumes slots: slots published == VBlanks with IE/IME on; frame numbers contiguous | equal; every gap is a counted no-entry VBlank | — |
| FB-H1 | DSi XL | FB-M3's stub at D = C_set and D = 4C on KSSU, MKDS, GS DD, DKR, 30 min each; lag counter + game-cycle probe (display's T1 slack probe) | lag rate ≤ stock + 0.05 pp; D = 4C indistinguishable from D = 0 | owner |
| FB-H2 | DSi XL | T7 and the fresh path: VCOUNT at `fb_entry` vs at today's `vblank_entry`; fresh-path cycles | fresh path ≤ 300 cycles p99 | — |

---

## 9. Risks (ranked)

1. **R1, the IRQ-table write needs a ruling.**
   - The owner has accepted boot-time code patches. Runtime writes of IRQ-table entries are stock nds-bootstrap behaviour (misc.c:112-125), but they are new for DScheevos.
   - If refused: E3 (cost on every IRQ, §2.7) or R4.
2. **R2, the network on busy engines.** A game that never waits gives the ARM9 no time for lwIP/TLS/HTTP (module.c idle path). Unlocks then wait for:
   - an ARM7-hosted network (new design; the ARM7 already runs the SDIO shim); or
   - the journal, sent at the next safe point or launcher start (architecture.md:107: offline semantics).

   Hardcore liveness for busy engines needs ra-compliance and an owner decision. **This angle can't solve it.**
3. **R3, heavy sets and lag frames.**
   - TWEWY- and Platinum-class sets cost 1.4-14 % of a frame per FB capture [est].
   - Proposed policy: in waiting games, lag frames use FB only if C_set ≤ C_cap; above it, carry (as today) and count it.
   - Busy engines with such a set aren't provably lag-free until the capture lane makes C smaller.
4. **R4, ARM7 ring drain with a never-waiting ARM9.**
   - Bus contention can't be gated (round 1's `a9_waiting`, A7 proposal §3.5).
   - EXMEMCNT is set to ARM7 priority by nds-bootstrap (main.arm9.c:184).
   - Delta encoding keeps it to the changed words. A7-M4 must also measure case (d) with a busy ARM9.
5. **R5, GS DD's time-based scheduler.** A C-cycle phase shift can change which frame a task lands in. FB-M3's frame hashes decide; nothing static can.
6. **R6, ARM7 capacity for GS DD.** 211-227k ARM9 instructions per frame (A7 proposal §1.6). The arm7eval lab put Kirby at ≈ARM9 cycles on the ARM7 with hot code in WRAM (arm7-notes.md:107-109). If GS DD's set needs more than its free ARM7 time, frames queue and drop; FB can't help.
7. **R7, VBlank masked** (IME = 0, or IE bit 0 clear during loads): there is no ARM9 entry, so the frame can't be captured coherently. It is counted from the ARM7 clock, same as round 1's `frames_missed`.
8. **R8, DKR's dispatcher.** It matches no nds-bootstrap signature (§1.1), so today's DKR VBlank path may not exist. FB-S2 first.
9. **R9, pre-handler delay pushing VBlank uploads into line 0.** Typical C is 0.5-1.4 of the 71 VBlank lines; FB-M3's frame hashes catch it.
10. **R10, chain-state purity.** It is a prerequisite (§2.2) owned by the capture lane. Without it, FB preempting a half-done halt copy is unsafe.

---

## 10. Recommendation

1. **Adopt the VBlank pre-handler instant as the single definition of "the frame's memory" for every game.**
   - The halt copy stays as its zero-game-time precomputation, valid only under the fresh rule (§2.3).
   - FB covers everything else: busy engines, lag frames, frames W could not capture.
2. **Ship order:**
   - FB-S1/S2;
   - the FB entry with the fresh path only (replaces the IPC-0xA count; a net saving);
   - FB-M1 on all real sets, including GS DD and DKR;
   - FB-M3/M4;
   - then turn on the FB capture path: lag frames under C_cap, and busy engines once P2's ARM7 evaluator exists.
3. **Don't make FB the only capture** for halting games: C_set game time against zero, and impossible for chain-heavy sets.
4. **Owner rulings:**
   - **R1:** the IRQ-table write.
   - **R2:** "C_set per frame in busy engines, proven lag-free by FB-M3 at 4× margin" counts as meeting the zero-cost rule, with the absolute number reported.
   - **R3:** lag frames: FB under the cap (RetroArch-like) vs carry.
   - **R4:** retire the steady-state 0xA IPC.
