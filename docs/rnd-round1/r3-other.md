# Lane R3-OTHER decision report

## 1. Findings

### 1.1 DSi and 3DS hardware: no compute or offload source passes the hard rules

| Candidate | Why it is closed | Citation |
|---|---|---|
| AES engine | It is ARM7-only and does only CTR/CCM. Our per-frame crypto cost is bignum TLS math (mpi_core_mla 4.9k, gcd_modinv 3.0k cycles per frame on MKDS), which AES cannot do. The journal MAC runs only at safe points. | GBATEK "DSi AES I/O Ports"; round/profile-mkds.txt:40,45; WORKLIST.md:52 |
| Hardware divider/sqrt | It belongs to the game and its result registers are read-only, so putting the game's state back means re-running the game's divide. No division shows up in the MKDS hot path. Kirby's old `__udivmoddi4` cost predates the current 32-bit `millis()`. | GBATEK "DS Maths"; module/src/module.c:194-198 |
| ITCM, DTCM, old WRAM, VRAM | The game owns all of them. VRAMCNT is write-only, so we cannot even detect a free bank. | memplan/ndsb.md:57-59; GBATEK "DS Memory Control - VRAM" |
| D/I-cache lockdown, CP15 c15 test register, write-through region | Each is global cache or MPU configuration that the game's IRQ handlers would see. All were already rejected. | ARM DDI 0201D §2.3.11, §2.3.15; round/plan.md:36 |
| PLD | GBATEK says it behaves as a NOP, and we cannot insert it into unmodified rcheevos anyway. | GBATEK "ARM Opcodes: … PLD" |
| Geometry engine, camera, I2C | The game owns GXFIFO and the matrix stacks. nds-bootstrap turns camera and DSP access off in SCFG_EXT9 and then locks SCFG. | bootloaderi/source/arm9/main.arm9.c:429-436 |
| NDMA | No ARM9 channel is free: ce9 uses NDMA0, plus NDMA1 on the non-TWLSDK path, and display claims 0, 2 and 3. NDMA also misses dirty D-cache lines. | cardenginei/arm9/source/cardDma.thumb.c:86-90,461-462; docs/display-library.md:221-242; evalspeed-team/arm7-notes.md |
| Teak DSP | It is off and locked in DS mode and would compete for NWRAM-B/C. Running rcheevos there would need a port to a 16-bit word-addressed CPU, and its main-RAM access through AHBM is not coherent with the ARM9 cache. It was already rejected. | GBATEK "DSi Teak MMIO AHBM"; research/03-dsi.md Option C |
| Atheros Xtensa | On the AR6013 and AR6014 it is the chip that runs NDS-mode game wireless, so loading code into it breaks "wireless exactly as stock". | GBATEK "Atheros chip References" |
| 3DS ARM11 | The only documented channel from TWL-mode code is RTCom: a few bytes per frame, and it needs Luma plus a patched TwlBg. The extra 16 MB of RAM on 3DS is memory, not compute. | wiki.ds-homebrew.com/ds-index/rtcom; main.arm7.c:889-891 |

The only real new memory is NWRAM-B/C, about 450-500 KB on the ARM9, and that already belongs to plan.md rank 1 and rank 6 (memplan/ndsb.md:171-177).

### 1.2 rcheevos public API and config: no lever that needs no ruling

- **Already minimal.**
  - Only ACTIVE achievements are evaluated (rc_client.c:6051-6061).
  - Achievements already unlocked in hardcore are skipped unless encore mode is on (rc_client.c:1474-1497).
  - Unpromoted achievements are off by default (rc_client.c:2060-2071).
- **No short-circuit inside a condition group.** `can_short_curcuit` is never assigned, and the memset at rc_trigger.c:186 and rc_value.c:339 zeroes it. The only skips are a true PauseIf and a fired ResetIf (rc_condset.c:688-770).
  - Correction for memplan: proposal-minimal.md:393 labels its counts "short-circuiting active". That label is wrong for this rcheevos pin.
- **Every memref in the pool is read every frame**, including memrefs used only by unlocked achievements (rc_client.c:5992-6044). There is no batched-read API (rc_client.h:893-920).
- **A failed read is not a skip.** It disables every dependent achievement and leaderboard, unlocked ones included (rc_client.c:5937-5958, 1294-1335).
- **Skipping or fast-forwarding identical frames is unsound**, so plan.md's rejection stands, now with the exact mechanisms:
  - the changed flag and Delta operands (rc_memref.c:744-776);
  - hit counting (rc_condset.c:441-454);
  - leaderboard re-tests (rc_lboard.c:184-187);
  - `frames_processed` and the pause-penalty decay (rc_client.c:6480-6503).
- **`rc_client_is_processing_required`** is a sound gate, but it saves about 0 in the worst case (rc_client.c:5971-5990).
- **Compile-time options** (RC_CLIENT_SUPPORTS_*, RC_NO_VARIADIC_MACROS, DEBUG_BUFFERS) do not change evaluation work.
- **Only two config-sized levers exist, and both need a ruling.** I am not proposing either; under the strict-compliance rule they are questions to ask.
  1. Leaderboards in hardcore. They are about 32% of MKDS conditions, 68% of Peggle's and 60% of Cory in the House's (scratchpad r3/lb.py, approximate). They have no public off switch (rc_client.c:6470-6475), and RetroArch evaluates them every frame.
  2. Rich presence evaluation cadence. It currently runs every frame (rc_richpresence.c:736-745).
- **Unpromoted leaderboards:** none of the 32 cached sets has one, so removing `v=2` is worth about 0.

### 1.3 New facts for other lanes

- **MKDS D-misses come from streaming the conditions, not from memrefs.** This is a derived estimate:
  - 3,124 conditions × 48 B is about 150 KB per frame, or about 4.7k lines, against 5,545 measured D-read misses per frame ([PM]:2).
  - The unique memref pool is only about 2 KB.
  - Every condition line is dirtied every frame: `is_true` is always stored (rc_condset.c:424), `current_hits` on true conditions (:441-450), and the memref `changed` flag (rc_memref.c:744-752). So each line costs a fill plus a write-back.
  - This points the NWRAM set-data zone (rank 1) at condition arrays first, then memref pools. Strings and badge URLs are cold (rc_client.c:2075-2133).
- **The rich-presence ping string is built from live memory.** On frames that are not evaluated, `dsc_idle` calls `rc_client_idle` with `S.evaluating == NULL` (dscheevos.c:3015-3016). The ping then builds the RP string through `read_memory`, which falls through to a live ARM9 memcpy (dscheevos.c:2087-2092; module.c:130-141).
  - Red-team 2 is right that these reads **are coherent** (they are ARM9 loads through the ARM9's own cache).
  - The values can be torn mid-frame, and they are not the snapshot. The effect is cosmetic: it touches only the RP string sent with the ~2-minute ping. This goes to the dscheevos.c owner as a low-priority note.
- **NDMA0 collides between ce9 and display.** ce9 uses NDMA0 (cardDma.thumb.c:86-90), and display claims NDMA0 for its FIFO. Display's rule of standing down during card reads (display-library.md:221-242) is the only thing that separates them, so channel ownership needs an explicit ruling. That ruling belongs to the display session (dscheevos-43), not this lane.

### 1.4 The one buildable candidate: a unity build of rcheevos (one translation unit)

Both red-teams compiled it with the pinned toolchain (devkitpro/devkitarm:20241104, GCC 14.2). Scripts and objects are in rnd scratch u/ and unity/.

- **At -Os, which ships today (Makefile:19-21,61), it is a no-op.** The call graph is identical call for call, and .text changes by -20 to +169 B.
- **At -marm -O2, only small calls get inlined:**
  - `rc_get_memref_value` into `rc_evaluate_operand`;
  - `rc_read_memory` and `rc_update_memref_value` into the do_frame memref loop, which runs about 122 times a frame on MKDS;
  - `rc_reset_condset`;
  - some `rc_typed_value_convert` calls.
- **The per-condition core stays out of line.** These keep identical call-site counts to the per-file build: `rc_evaluate_operand` (recursive, rc_operand.c:599, 0x350 B), `rc_test_condition`, `rc_transform_memref_value`, `rc_typed_value_compare`, and the AddAddress path (`rc_get_modified_memref_value`, then `rc_evaluate_operand`, then `rc_typed_value_add`).
- **Text size (11 files, before gc-sections):** 66.7 KB per-file at -Os, 81.3 KB per-file at -O2, 86.8 KB unity at -O2. Unity costs +5.5 KB over per-file -O2 and +20 KB over what ships today.
- **The aggressive proposal's estimates are refuted.** It claimed about 9k cross-TU calls removed per frame, -25k to -60k on MKDS and -50k to -120k on Platinum. The measured inlining does not support that.
- **What holds up:** `malloc`, `calloc`, `realloc` and `free` stay undefined in the unity object, so their `--wrap` still works, as long as `api/rc_api_runtime.c` stays out of the TU (Makefile:25; heap.c:228-241). There are no name collisions.
- **It is not a new lever.** It is the same inliner as rank 7 LTO, implemented without the `--wrap` audit, with one extra: it also covers rc_client.c's small memref loop.

## 2. Recommendation: not worth it as a separate lane item

Ship nothing new from R3-OTHER. At most, add the unity build as one extra arm of the rank 7 LTO A/B that is already planned, at no extra cost.

Expected ARM9 cycles saved per evaluated frame, unity -O2 against per-file -O2 (rank 2 landed). These are derived (D) upper bounds:

| Game | Saving | Basis |
|---|---|---|
| MKDS | 0 to -20k | About 1-2k removed calls per frame × about 10-15 cycles of call overhead each, minus the I-cache cost of +5.5 KB on a hot set that is already 274 lines against 256 (plan.md rank 2). D-misses (36-41% of evaluation) are unchanged. |
| Platinum | 0 to -15k | The chain (AddAddress) path stays fully out of line. No in-game profile exists. |
| Kirby SSU | 0 to -5k | Evaluation is 202k of 556k. Capture (~191k) is untouched. |
| At -Os | 0 | Measured. |

None of these changes a "fits every frame?" verdict. MKDS stays at about 2.3x its mean idle.

**Why it costs the game nothing.** It is a code-generation change only:
- no new register access (SCFG, MBK, NDMA, TIMER, DIV, CP15, GX or VRAM);
- no game RAM writes, and reads still come from the snapshot through `read_memory` (dscheevos.c:2084);
- the IRQ-masked capture (module.c:641-666) and the VBlank IRQ work (641-699 cycles) are unchanged;
- no SD, NAND or firmware access;
- the rcheevos sources are byte-identical, so RA compliance is the same as for -O2 or LTO.

Its only cost is the I-cache and ROM-cache KB of our own pass, and the gate below covers that.

## 3. Minimal experiment (only as an arm of rank 7)

The task asks for a plan only for build or experiment verdicts. This is the cheapest decisive version, folded into work that is already planned.

**Prerequisite:** rank 2 (per-file -marm -O2) has landed. Every size and cycle comparison is against that build, not against today's -Os build.

**Coordinate with:** the eval-speed lane, which owns ranks 2 and 7 and the hot link-order list. Idle-coverage and arm7eval only need to know that the same wrapper can be reused for an ARM7 build; that design is theirs. Display (dscheevos-43) and the nds-bootstrap card engines are not affected.

### Steps

1. **Wrapper and Makefile switch.**
   - Add `dscheevos/module/src/rc_unity.c`, which `#include`s `runtime/{rc_alloc,rc_memref,rc_operand,rc_value,rc_condition,rc_condset,rc_trigger,rc_lboard,rc_richpresence}.c`, `util/rc_util.c` and `client/rc_client.c`. It must never include `api/*.c`.
   - Add `RC_UNITY=1` to the Makefile. It drops those files from RC_SRC and builds `rc_unity.o` with `-marm -O2 -fno-strict-aliasing` plus the rest of the module CFLAGS.
2. **Static gate. All must pass, or stop.**
   - `objdump -r rc_unity.o` shows the five WRAPS symbols as UNDEF.
   - `rc_unity.o` does not define `rc_api_process_fetch_game_sets_server_response`.
   - .text grows by no more than 6 KB over the per-file -O2 build.
3. **Decisive cheap A/B on qemu.** Run `tests/perf/realsets` over all 32 cached sets, per-file -O2 against unity -O2.
   - PASS (decided in advance): the realsets digest (realsets.c:18-23) and the event traces are byte-identical, AND MKDS qemu instructions per frame drop by at least 3%.
   - Otherwise close the item with no melonDS or hardware time spent.
4. **melonDS, only if step 3 passes.** Run it inside the rank 7 M0 fixed-frame A/B, taking the emulator lock as usual: `overhead/run_cpi.sh --thumb-cycle` with the cache model, on mkds, kirby, platinum and black, 3 runs per arm.
   - KEEP: the median MKDS saving is at least 10k cycles per evaluated frame, I-misses per frame rise by no more than 10%, D-misses are unchanged within noise, and Kirby and Black regress by no more than 2k.
   - If it is kept, regenerate the rank 2 link-order list, because the symbol names change to `.isra`/`.constprop` clones.
5. **Host tests and lag A/B.**
   - Run the full core host suite.
   - Run `tests/perf/ab.sh` with stock against the build on MKDS and Kirby, 6,000 intervals each, including the missing achievements-off half for Kirby.
   - PASS: the lag-interval count equals stock.
6. **DSi XL test ROM, only if step 4 passes.** Build two ROMs, per-file -O2 and unity -O2, that record VCOUNT lines around `rc_client_do_frame` (VCOUNT is a read-only register) plus `frames_dropped`, lag intervals and the evaluation digest into module RAM, and dump them to SD only at quit.
   - Script: the MKDS attract loop for 10 minutes, runs in the order A, B, A, B.
   - PASS: unity's mean lines are no more than 0.97× per-file's in both pairs, frames dropped are no higher, lag counts are equal, the digests are equal, and there are no hangs.
   - Repeat on the New 3DS XL for the no-hangs criterion only.
   - Write it up in TESTING.md.

**Expected outcome:** reject at step 3 or step 4.

## 4. Open questions only hardware can answer

1. What is the real line-fill latency of DSi NWRAM-B/C? GBATEK does not document it, and melonDS reuses the DS WRAM timing (plan.md M3). This decides the value of rank 1.
2. Per game, what C/B bits does the MPU give 0x037xxxxx and the 0x0C000000 uncached mirror? If the mirror is NCNB, every snapshot store stalls (M1).
3. What is ARM7 `SCFG_EXT7` at game time under TWiLight Menu++? It only matters if AES is ever wanted for a non-per-frame job.
4. How long does an NDMA block copy stall an ARM9 running from cache hits? It only matters if the NDMA0 ruling frees a channel.
5. Does the ARM946E-S on DSi actually prefetch on PLD? It only matters for our own capture loops.
6. Does Kirby's 172/6,000 lag happen with achievements off too? This needs the A/B before Kirby counts as zero-cost.
7. Does the NitroSDK thread switch save the divider context? This is moot unless division ever shows up in a profile.

## 5. Summary for the orchestrator

1. R3-OTHER: no DSi or 3DS hardware block (AES, divider, TCM, DSP, NDMA, Atheros, ARM11) or rcheevos public knob offers zero-cost compute or work avoidance. The only big config levers, leaderboards off in hardcore (32% of MKDS conditions) and rich-presence cadence, need RA or owner rulings.
2. The one build-only candidate, a unity build of rcheevos, was compiled and measured. It is a no-op at -Os. At -O2 it saves at most about 0-20k cycles per frame on MKDS for +5.5 KB, with the per-condition hot calls still out of line. Verdict: not worth it alone; at most a free arm in rank 7 LTO, stopped unless qemu shows at least a 3% instruction drop.
3. Side findings:
   - MKDS D-misses come from streaming about 150 KB of conditions per frame, so NWRAM rank 1 should place condition arrays first.
   - The memplan "short-circuiting" label is wrong for this rcheevos pin.
   - The RP ping reads live, non-snapshot memory (coherent but can be torn; cosmetic).
   - ce9 and display both use NDMA0, and that needs a ruling with dscheevos-43.