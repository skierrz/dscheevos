# Proposal: ARM9-side wait hooks with a parkable work coroutine

Angle: ARM9 hooks. Covers every way a game waits on the ARM9: the SDK idle thread's `OS_Halt` (existing), the BIOS waits `SVC_WaitVBlankIntr` / `SVC_WaitIntr` (new), and busy engines (no zero-cost answer exists; what the ARM9 can and cannot do is set out in section 7).

Status: design only. Nothing in the repository was changed. `module.c` / `start.s` belong to the perf session, and restructuring the idle pass needs the orchestrator's go-ahead (design-notes.md:38).

Citation keys:
- `brief` = scratchpad/design-brief.md
- `notes` = scratchpad/design-notes.md
- `hw` = scratchpad/facts-hw.md
- `census` = dscheevos-idlecov/docs/idle-census.md
- `static.tsv` = private/census/static.tsv
- `idle` = memplan/idle.md
- `cpu` = memplan/cpu.md
- `GBATEK` = scratchpad/dl/gbatek.txt (line numbers)
- `FreeBIOS` = scratchpad/dl/mel/freebios/src/bios_common.s
- `start.s`, `module.c` = dscheevos/module/src/
- `hooks.c` = external/nds-bootstrap/retail/bootloaderi/source/arm7/dscheevos_hooks.c
- `display` = dscheevos-display/docs/display-library.md

Anything not backed by a citation is marked **(guess)**.

---

## 0. Summary

1. **Hook the SDK wrapper, not the SWI.** `SVC_WaitVBlankIntr` is the same 6-byte Thumb function in all 64 ROMs (`movs r2,#0; svc 5; bx lr`, census:22-25, notes:27). nds-bootstrap rewrites those 6 bytes in place at boot with `movs r2,#0x5B; lsls r2,r2,#19; bx r2`, which jumps to 0x02D80000. The module header's first word becomes an ARM `b` to the BIOS-wait entry.
   - No secure-area fill bytes are touched.
   - No trampoline is needed.
   - The ±4 MB Thumb BL range problem (brief) disappears.
2. **Emulate IntrWait exactly instead of calling it after our work** ("E", the wait emulator). At the call, E does what the BIOS does then:
   - IME = 1;
   - discard the mask's bits in DTCM+3FF8;
   - then halt and check in a loop, clearing the bits on return (GBATEK:12868-12874, hw §1, FreeBIOS:246-345).

   The discard happens at the moment the game called. So a VBlank that arrives while we work sets the flag, and E returns at once. **SWI 5's whole-frame trap cannot happen by construction**: we never issue SWI 5 after doing work.
3. **The work runs as a coroutine W on its own stack, and every interrupt parks it before any game code runs.**
   - While W runs, one ITCM word, the first instruction of the game's `OS_IrqHandler` (`stmfd sp!,{lr}`), is swapped for `b irq_head`.
   - `irq_head` restores the word and copies W's registers out of the BIOS IRQ frame (reads only). It drops the frame by moving SP_irq back to where it was, and returns to E in the waiting thread with the IRQ still pending.
   - The IRQ is then taken again, through the original handler, with the waiting thread as the interrupted context, exactly as if the BIOS halt loop had been interrupted.

   Main-thread work therefore becomes preemptible at every IRQ, with a fixed overhead X per IRQ, estimated at 100-350 cycles **(guess, measured in M4/H3)**. No deadline estimates are needed. **The toggled word is never observable by game code**: it is set only while no game instruction executes (section 5.2).
4. **Coverage:**

   | Class | What this proposal does |
   |---|---|
   | oshalt (53/64) | unchanged in phase 1; phase 2 moves the halt hook to the same W model |
   | bios (9/64, ~9% of RA) | full hook, all the main-thread wait time |
   | busy (2/64, ~3%) | **no zero-cost ARM9 mechanism exists** (proof sketch in 7.1). The best ARM9 option (IRQ-gated or time-sliced spin borrowing) costs bounded but nonzero game time and needs a ruling. Recommended: detect and report "unsupported", and leave these to the ARM7 angle |
5. **Display signal:**
   - `hk.wait_kind` = 2 means the main thread is in a BIOS VBlank wait (exact "frame done").
   - "Our code is running" means `hk.w_running`, or the interrupted PC is in the module image.
   - Strip IRQs are dispatched by `irq_head` while a wait is active and go straight back to W or E (no idle pass).
   - Wait-entry and wait-leave callbacks give the display its arming points.

---

## 1. Facts this rests on

| # | Fact | Source |
|---|---|---|
| F1 | 53 ROMs idle in OS_Halt, 9 wait in SWI 5 on the main thread (all NitroSDK 2.x), 2 never halt | census:11-21 |
| F2 | Every BIOS game's main wait site is the wrapper (pc in 0x02000000-0x020007FF), ≥97% of halted time from one call site (RE:DS 79% from its main caller) | census:21, 46-54 |
| F3 | Wrapper bytes are `00 22 05 df 70 47` (Sonic Rush at 0x02000078; caller `blx` from ARM at 0x02000c48) | `sr_arm9.bin` disassembly: 0x02000078 `2200`, 0x0200007a `df05`, 0x0200007c `4770` |
| F4 | `SVC_WaitIntr` (SWI 4) has 0 static callers in 64/64 ROMs; SWI 6 also 0 | static.tsv column callers4/5/6 |
| F5 | All 64 ROMs have OS_Halt (oshalt=1), so the module is not disabled today for BIOS games; they just never get a pass | static.tsv; hooks.c:249-252 |
| F6 | VBlankIntrWait = IntrWait(r0=1, r1=1); r0=1 discards old flags; NDS9 r0=0 is bugged (always waits for one IRQ first); flags are cleared on return; IME forced to 1 | GBATEK via hw §1 |
| F7 | FreeBIOS reference: SWI runs in System mode with the **caller's** I bit (`and spsr,#0x80; orr #0x1F`); the check does IME=0, test, clear, IME=1; with r0=1 the loop is check-and-clear, then halt+check | FreeBIOS:88-104, 246-272, 297-345 |
| F8 | ARM9 halt = `mcr p15,0,r0,c7,c0,4`; wakes on IE&IF whatever CPSR.I; hangs if IME=0 | GBATEK via hw §1 |
| F9 | BIOS IRQ entry pushes `{r0-r3,r12,lr}` on SP_irq, calls [DTCM+3FFC] with lr = its epilogue, then `ldmia; subs pc,lr,#4` | FreeBIOS:1186-1209; GBATEK:3459-3464 (GBA BIOS, same shape). **Real NDS9 BIOS not disassembled: verify (H1)** |
| F10 | NitroSDK `OS_IrqHandler` starts `e92d4000 stmfd sp!,{lr}; e3a0c301 mov ip,#0x4000000; e28cce21 add ip,ip,#0x210; e51c1008 ldr r1,[ip,#-8]`; it runs from ITCM (Sonic Rush: return literal 0x01FF8058 = ThreadSwitch at handler+0x58, so the handler is at 0x01FF8000); acks IF itself; dispatches the lowest pending bit through `OS_IRQTable` in DTCM | sr_arm9.bin 0x0207cfe0-0x0207d034; acww_itcm.bin 0x01ffd50c; nds-bootstrap hook_arm9.c:47-69 (same signature, plus an alt for GS DD) |
| F11 | nds-bootstrap already rewrites entries of the game's IRQ table in DTCM at runtime (IPC sync, VCOUNT) and calls `header->vblank` from its IPC handler | cardenginei/arm9/source/misc.c:111-124, cardengine.c:1644-1648 |
| F12 | Stock nds-bootstrap rewrites a code patch word while the game runs (in-game menu clock toggle rewrites the waitSysCycles patch) | hw §6 (inGameMenu.c) |
| F13 | NitroSDK main RAM is write-back D-cached; D 4 KB, I 8 KB, 32-byte lines; main-RAM line fill ≈ 23 bus (33 MHz) cycles ≈ 46 ARM9 cycles | brief; hw §3 and §5 |
| F14 | One VBlank interval = 1,120,380 ARM9 cycles (≈4,260 per line) | idle:10 |
| F15 | Set costs: Sonic Rush 73.7k instr/frame, RE:DS 209k, Pac'n Roll 80k (cpu.md); real CPI ≈ 3.4 | cpu:37, 29, 36; brief |
| F16 | Code Lyoko's wait: `strb 0,[0x020FE438]; do { poll(*0x020FF550) } while (!byte[0x020FE438])`. `poll` (0x0208D3E0) returns at once when obj+8 == 0 and obj+0x78 == 0 (a 304-byte stack frame, else decode work) | cl_arm9.bin 0x020571C4-0x020571F0, 0x0208D3E0-0x0208D404 |
| F17 | Golden Sun DD's scheduler loop makes virtual task calls (`blx r1`), calls an interrupt-disable helper, and its time base polls VCOUNT (33.6 reads/interval at 0x020545EC); its hot spots include nds-bootstrap's CE9 at 0x023BF640-0x023BF6BF (11.5%) | gs_arm9.bin 0x020371A4-0x020373B8; out/goldensun.log hot spots, IO pollers |

---

## 2. Patch sites

### 2.1 `SVC_WaitVBlankIntr` (phase 1)

Stock, 6 bytes, Thumb (F3):

```
2200  movs r2, #0
df05  svc  5
4770  bx   lr
```

Patched in place, 6 bytes, Thumb:

```
225B  movs r2, #0x5B
04D2  lsls r2, r2, #19        @ r2 = 0x5B << 19 = 0x02D80000
4710  bx   r2                 @ bit0 = 0: ARM state at 0x02D80000
```

Encodings:
- MOVS(imm8) = `001 00 Rd imm8` → 0x2200 | (2<<8) | 0x5B = 0x225B.
- LSLS(imm5) = `000 00 imm5 Rm Rd` → (19<<6) | (2<<3) | 2 = 0x04D2.
- BX = `010001 11 0 Rm 000` → 0x4710.

Bytes in the ROM image: `5b 22 d2 04 10 47`.

Why this works:
- r2 is free. The wrapper set it to 0 for the DSi7-only IE2 argument (GBATEK, hw §1), and callers treat the wrapper as an AAPCS function: r0-r3, r12 and flags are clobbered anyway.
- No byte outside the 6-byte wrapper is written, so the secure area's random fill (notes:27-28) is untouched.

Module header ABI:
- Word 0 at 0x02D80000 becomes the ARM instruction `b bw_entry`, with `bw_entry` placed at a fixed header offset 0xA0 (after `dscm_irq` at 0x60-0x9F, start.s:55-58): `0xEA000026`.
- That word *is* the new magic: `DSCHEEVOS_MAGIC = 0xEA000026`, `DSCHEEVOS_VERSION = 7`.
- Three readers compare the magic and must use the new constant: cardenginei/arm7/source/cardengine.c:2089, arm9_igm/source/dscheevosMenu.c:29, hooks.c:227.
- `disable()` zeroes word 0 (hooks.c:211). So **the wrapper patch must be applied after every disable() check has passed**, and nothing may zero word 0 afterwards. A zeroed word 0 would execute `andeq r0,r0,r0` and fall into word 1.

nds-bootstrap (`dscheevos_hooks.c`, next to `patchOsHalt`):

```c
// SDK SVC_WaitVBlankIntr (Thumb): movs r2,#0; svc 5; bx lr  ->  movs r2,#0x5B; lsls r2,r2,#19; bx r2
static int patchSvcWaitVBlankIntr(const tNDSHeader* h) {
	u16* p = (u16*)h->arm9destination;
	u16* end = p + 0x4000/2 - 3;            // the secure area: every wrapper seen is below 0x800 (census:22-25)
	u16* hit = NULL; int n = 0;
	for (; p < end; p++)
		if (p[0] == 0x2200 && p[1] == 0xDF05 && p[2] == 0x4770) { hit = p; n++; }
	if (n != 1) return 0;                    // none, or ambiguous: leave the game stock
	hit[0] = 0x225B; hit[1] = 0x04D2; hit[2] = 0x4710;
	return 1;
}
```

`dscheevosSetup` change:
- Call this after the existing checks.
- Disable only if *neither* OS_Halt nor this wrapper was found.
- Log both results (hooks.c:249).

As with OS_Halt, the patch goes in before the page file saves the ARM9 binary (architecture.md "Memory").

### 2.2 `SVC_WaitIntr` (phase 2, only if a game is found that waits in it)

0/64 static callers (F4), and the census never saw SWI 4 halt time. If one turns up:
- same 6-byte form with `movs r2,#0x5B` replaced by a second fixed entry;
- or the 8-byte `ldr r2,[pc]; bx r2; .word` form at 4-aligned wrappers, which costs 2 fill bytes (notes:28).

E already takes r0/r1 from the caller, so only the entry differs.

### 2.3 OS_IrqHandler head (runtime toggle; no boot patch)

- At wait entry the module reads `h = [DTCM+3FFC]` (the game's IRQ handler pointer, GBATEK:12875-12880; read only).
- It checks that `h` is ARM and 4-aligned, and that `h[0..3]` equal the F10 signature (or the GS-DD alternate, hook_arm9.c:64-69).
- It precomputes `branch = 0xEA000000 | (((irq_head - (h+8)) >> 2) & 0xFFFFFF)`. ITCM 0x01FF8000 to module 0x02D8xxxx is about 14 MB, inside ARM `b`'s ±32 MB.
- If any check fails, phase-1 work is **disabled for that game**: E still runs as the BIOS stand-in, W never runs, and status shows "BIOS wait: no IRQ head".

The displaced instruction is `stmfd sp!,{lr}`, or `mov ip,#0x4000000` for the alternate. Both are position-independent, so `irq_head` can replay it and branch to h+4.

---

## 3. Module state and memory

- **Code.** `bw_entry`, E, `irq_head`, `w_resume`, `w_yield`: about 600-900 bytes of ARM code in `.text.entries` **(guess)**.
- **State.** One 64-byte block `hk` (two cache lines):

  | Field | What it is |
  |---|---|
  | `owner` | a thread is in E with W rights |
  | `w_running`, `w_busy` | W is executing; W is mid-work (not at a yield) |
  | `wait_kind` | 0 none, 1 idle OS_Halt, 2 BIOS VBlank, 3 other |
  | `mask`, `flagbase`, `caller_cpsr` | the wait's mask, DTCM+0x4000, the caller's CPSR |
  | `m_sp`, `m_lr`, `m_r4..r11` | the waiting thread's saved registers |
  | `irqh`, `irqh_word0`, `irqh_branch`, `irqh_plus4` | the IRQ head toggle |
  | `wait_seq`, `enter_vcount`, `enter_frames` | wait bookkeeping |
  | `cb_enter`, `cb_leave` | display callbacks |
  | `capture_wanted` | a capture is owed at W's next safe point |

  `wctx` is 72 bytes: r0-r3, r12, pc, cpsr, r4-r11, sp, lr.
- **Stacks.**
  - W uses the existing 8 KB module stack (`__stack_top`, module.ld:26). W and the OS_Halt pass are made mutually exclusive (section 6), so they never use it at the same time.
  - The hook-side capture runs on the IRQ stack region (`__irq_stack_top`, module.ld:27). It never unmasks IRQs (it aborts instead), so it cannot overlap `vblank_entry`'s copy, which runs only inside an IRQ (start.s:128-141).
- **Net cost:** under 1 KB of module image and no new heap or stack. Nothing comes out of the ROM cache beyond today's 2 MB.

---

## 4. Mechanism in pseudo-assembly

### 4.1 `bw_entry`: the BIOS VBlankIntrWait stand-in

Entry state: the caller's mode (SYS for NitroSDK threads **(guess, verify in M3)**); r2 = 0x02D80000; lr = return address (ARM or Thumb).

```
bw_entry:                        @ header+0xA0, ARM
    mov   r0, #1                 @ VBlankIntrWait: r0 = 1 (discard), r1 = 1 (VBlank)   [F6]
    mov   r1, #1
bw_common:                       @ (SWI 4 entry joins here with the caller's r0, r1)
    mrs   r12, cpsr              @ caller CPSR (its I bit decides IRQs during the wait, as FreeBIOS) [F7]
    orr   r3, r12, #0x80
    msr   cpsr_c, r3             @ mask IRQs for the flag RMW (the BIOS uses IME=0 for the same) [F7]
    mov   r3, #0x04000000
    mov   r2, #1
    str   r2, [r3, #0x208]       @ IME = 1: IntrWait "forcefully sets IME=1" [F6]
    mrc   p15, 0, r2, c9, c1, 0
    mov   r2, r2, lsr #12
    mov   r2, r2, lsl #12
    add   r2, r2, #0x4000        @ r2 = DTCM+0x4000; check bits at [r2,#-8]
    cmp   r0, #0
    ldrne r3, [r2, #-8]
    bicne r3, r3, r1
    strne r3, [r2, #-8]          @ r0=1: discard old flags NOW, at the game's call time
    @ --- ownership: one waiting thread at a time gets W ---
    ldr   r3, =hk
    ldrb  r0, [r3, #OWNER]       @ r0 is free now: r0==1 was consumed by the discard above
    ...   if r0 || !hk.enabled || in_halt -> pure_E (with r0 := 0 only for the SWI 4 r0=0 case)
    ...   save r4-r11, sp, lr, r12(cpsr), r1, r2 into hk.m_*; hk.owner = 1
    ...   hk.wait_kind = (r1 & 1) ? 2 : 3; hk.wait_seq++; hk.enter_vcount = VCOUNT
    ...   switch sp to the hook stack (IRQ-stack region, section 3); the thread's sp is kept in hk.m_sp
    ...   if hk.cb_enter: blx cb_enter      @ display arming, IRQs masked
    ...   if !w_busy: hook_capture()        @ on the hook stack; aborts if IE&IF != 0  (4.4)
    ...   else hk.capture_wanted = 1
    ...   sp := hk.m_sp (E never pushes on it); reload r1, r2, r12 from hk
    b     e_loop
```

### 4.2 E: the halt-and-check loop (runs in the waiting thread, never pushes on its stack)

```
e_loop:                          @ I masked; r1 = mask, r2 = flagbase, r12 = caller cpsr (reloaded from hk)
    mov   r3, #0x04000000
    mov   r0, #1
    str   r0, [r3, #0x208]       @ IME = 1 again (the BIOS check writes IME=1 every pass) [F7]
    ldr   r3, [r2, #-8]
    tst   r3, r1
    bne   e_done
    @ work to do, and W not owned by a suspended OS_Halt pass?
    ldr   r3, =hk
    ... if (w_has_work && !in_halt) b w_resume      @ 4.3; W runs with IRQs on until any IRQ parks it
    ... if (display armed) toggle head ON           @ so strip IRQs are dispatched by irq_head (4.3)
    mov   r0, #0
    mcr   p15, 0, r0, c7, c0, 4  @ halt until IE&IF (IRQs masked, IME = 1: no hang) [F8]
e_after_yield:
    msr   cpsr_c, r12            @ unmask as the caller had it: the waking IRQ is taken HERE,
e_after_irq:                     @   with this thread as the interrupted context (irq_head returns here too)
    ldr   r3, =hk                @ r0-r3, r12 are dead after any IRQ: reload
    ldr   r12, [r3, #M_CPSR]
    ldr   r1,  [r3, #MASK]
    ldr   r2,  [r3, #FLAGBASE]
    orr   r0, r12, #0x80
    msr   cpsr_c, r0
    b     e_loop
e_done:                          @ I masked
    bic   r3, r3, r1
    str   r3, [r2, #-8]          @ clear the selected flags, as IntrWait does on return [F6]
    ... toggle head OFF if on; hk.wait_kind = 0; if cb_leave: call it; hk.owner = 0
    ldr   lr, [hk, #M_LR]        @ r4-r11 and sp are the caller's (4.3 restores them on every park)
    msr   cpsr_c, r12            @ caller's CPSR
    bx    lr                     @ ARM or Thumb caller

pure_E:                          @ register-only E for a second thread or when W is off: same loop,
                                 @ no hk, no W, no head; r0==0 means "halt once before the first check"
                                 @ (the NDS9 r0=0 bug, F6)
```

SWI 5 calls always arrive with r0 = 1, so the r0=0 "halt first" path matters only for SWI 4. Pure E handles it, and so does E (a `need_irq` flag set at entry and cleared at `e_after_irq`).

### 4.3 W resume, IRQ head (park), voluntary yield

```
w_resume:                        @ SYS, I masked, in the waiting thread
    ldr   r12, =hk
    ldr   r0, [r12, #IRQH] ; ldr r1, [r12, #IRQH_BRANCH]
    str   r1, [r0]               @ toggle ON: ITCM is uncached and not write-buffered, so the next IRQ fetch sees it
    mov   r1, #1 ; strb r1, [r12, #W_RUNNING]
    ldr   r3, =wctx
    msr   cpsr_c, #0xD2          @ IRQ mode, I set: lr_irq/spsr_irq are free (no IRQ in progress)
    ldr   r0, [r3, #CPSR] ; msr spsr_cxsf, r0
    ldr   lr, [r3, #PC]
    add   r0, r3, #R4 ; ldmia r0, {r4-r11, sp, lr}^ ; nop      @ W's r4-r11 and SYS-bank sp, lr
    ldmia r3, {r0-r3, r12}       @ W's r0-r3, r12 (base reloaded last: no writeback)
    movs  pc, lr                 @ W continues; CPSR = W's (I on, maybe Thumb)

irq_head:                        @ entered from toggled OS_IrqHandler[0]: IRQ mode, I set,
                                 @ sp_irq -> BIOS frame {r0,r1,r2,r3,r12,lr_irq} [F9]; lr = BIOS epilogue
    ldr   r12, =hk
    @ (display extension: if (IE & IF & module_mask) && !(IE & IF & ~module_mask):
    @   handle the strip IRQ, ack its IF bits, bx lr  -> BIOS epilogue resumes W or E, head stays ON)
    ldr   r0, [r12, #IRQH] ; ldr r1, [r12, #IRQH_WORD0]
    str   r1, [r0]               @ toggle OFF before anything the game could run
    ldrb  r2, [r12, #W_RUNNING]
    cmp   r2, #0
    beq   passthrough            @ the interrupted context is E (halted with the display armed)
    ldr   r3, =wctx              @ ---- park W ----
    ldmia sp!, {r0-r2} ; stmia r3!, {r0-r2}            @ W r0-r2  (reads of the BIOS frame only)
    ldmia sp!, {r0-r2}                                 @ W r3, W r12, lr_irq; sp_irq is back at its pre-IRQ value
    sub   r2, r2, #4 ; stmia r3!, {r0-r2}              @ r3, r12, pc
    mrs   r0, spsr ; str r0, [r3], #4                  @ W cpsr (T bit kept)
    stmia r3, {r4-r11, sp, lr}^                        @ W r4-r11, SYS sp/lr
    mov   r0, #0 ; strb r0, [r12, #W_RUNNING]
    add   r3, r12, #M_R4 ; ldmia r3, {r4-r11}          @ the waiting thread's callee-saved regs
    ldr   r0, [r12, #M_SP]
    msr   cpsr_c, #0xDF ; mov sp, r0 ; msr cpsr_c, #0xD2    @ SYS sp := waiting thread's sp
    ldr   r0, [r12, #M_CPSR] ; msr spsr_cxsf, r0       @ caller mode, caller I bit
    ldr   lr, =e_after_irq
    movs  pc, lr                 @ IF not acked: the IRQ is taken again at once, now through the ORIGINAL
                                 @ OS_IrqHandler, with E as the interrupted context (as if BIOS-halted)
passthrough:
    stmfd sp!, {lr}              @ the displaced first instruction
    ldr   pc, [r12, #IRQH_PLUS4] @ continue in OS_IrqHandler+4 (r12 is free there: BIOS saved it, the handler overwrites ip)

w_yield:                         @ called by W (C), SYS mode: "nothing to do now"
    mrs   r0, cpsr ; orr r1, r0, #0x80 ; msr cpsr_c, r1
    ldr   r3, =wctx              @ save a context that resumes at w_yield_ret with r4-r11, sp, lr
    ... store r4-r11, sp, lr; pc = w_yield_ret; cpsr = r0
    ... hk.w_running = 0; toggle OFF; load hk.m_r4..r11, hk.m_sp
    b     e_after_yield          @ I still masked; E halts or finishes
w_yield_ret:
    bx    lr
```

**Initial W context:** pc = `w_main`, sp = `__stack_top`, cpsr = SYS, I clear, ARM.

**Thumb W code.** For an IRQ in Thumb state, LR_irq = next instruction + 4, the same as in ARM state. `subs pc,lr,#4` returns correctly in both, as the BIOS relies on (F9). Park stores pc = lr-4 with the SPSR's T bit, and `movs pc,lr` restores T.

### 4.4 Capture (the snapshot) in a BIOS wait

The snapshot must be the game's memory once its frame is done (architecture.md "Every frame"). At `bw_entry` with VBlank in the mask, the main thread has finished its frame, and lower-priority threads cannot run during the wait (stock SWI 5 keeps the main thread "running" in SDK terms, and so does E). So entry is the capture point.

The platform's `interrupt_pending` gets a **wait mode**: return 1 when `REG_IE & REG_IF` is nonzero, without unmasking, so the core's `copy_snapshot` aborts (core dscheevos.c:808, 2880-2890) instead of servicing the IRQ in place. An IRQ's wait is then at most one copy chunk (16 words or one chain step).

**Concurrency rule.** `dsc_capture` writes pointer-chain memref state through `S.sync` (core dscheevos.c:981-989, 1100-1137), and a suspended `rc_client_do_frame` uses that state. So:
- capture runs only while W is at a safe point (`!w_busy`);
- otherwise `capture_wanted` is set, and W captures at its next safe point if the same wait is still open and no VBlank has come (`hk.wait_seq` unchanged and `IE&IF&1 == 0`), with the same abort rule.

This is today's behavior, no worse: a halt that finds a pass in progress also skips the copy (start.s:181-184, 211-212). A frame with no capture is evaluated as a carry of the last snapshot (architecture.md). A core follow-up (perf session's call) would remove the limit: stage chain-root values per slot and apply them at evaluation.

### 4.5 W's body (what "budget logic" becomes)

```c
void w_main(void) {
  for (;;) {
    if (!g_initialized || !dscm_g_status.active) { w_yield(); continue; }
    w_busy = 1;
    if (g_reset_pending) { g_reset_pending = 0; dsc_reset(); }
    poll_network(); if (g_progress_stale) update_progress();
    dsc_idle();                       /* <= idle_max_frames (4) queued frames; rcheevos timers */
    clean_notices(); update_cues();
    if (g_net_started) { net_service(millis()); /* status fields */ poll_network(); }
    publish_stats();
    w_busy = 0;                       /* safe point: no core call in progress */
    if (hk.capture_wanted) w_late_capture();    /* 4.4, aborts on any pending IRQ */
    if (!backlog() && !net_wants_cpu()) w_yield();
  }
}
```

There is **no deadline and no cost estimate.** Safety comes from parking, not from budgets:
- every IRQ parks W within X;
- W's own IRQ-masked stretches are bounded by the capture chunk, `idle_frames()`, and `dscm_take` (module.c:584-590, 636-645; start.s:318-341).

Parking makes long items safe too: a TLS handshake step, an unlock's HTTP build, a 4.8M-instruction TWEWY frame. Each simply spans several waits. The "budget" questions left are throughput only:
- what share of evaluated vs. dropped frames a game gets (status counters `eval_lines` and `frames_dropped`);
- the per-slice accounting: `active_lines += lines_between(resume_vcount, park_vcount)` at each park and yield. A slice is shorter than one frame whenever the VBlank IRQ is enabled. Slices are counted with the frame counter as well, so a game with VBlank disabled can't wrap the line count.

The brief's "deadline handling" is answered by construction:
- **SWI 5 discard trap:** gone, because the discard happens at the call (4.1).
- **Overrun of the main thread's frame start:** bounded by X per IRQ, plus cache refill (P, section 5).

---

## 5. Zero-cost argument and every game-visible effect

### 5.1 What the game can observe (complete list)

| # | Effect | Size | Same as stock? |
|---|---|---|---|
| G1 | 6 bytes of `SVC_WaitVBlankIntr` patched at boot | code patch, like OS_Halt | accepted precedent (brief) |
| G2 | E writes IME=1 and clears mask bits in DTCM+3FF8 | the values and order the BIOS uses (F6, F7); intermediate IME=0 states of the BIOS are invisible to the game (no IRQ can run in between) | identical in value. **Owner to confirm that BIOS-identical writes the game requested are not "touching game RAM"** |
| G3 | OS_IrqHandler[0] in ITCM holds `b irq_head` only while W runs (or E halts with the display armed) | 1 word | see 5.2: no game instruction ever executes while it is set. **Ruling needed (new class: runtime toggle of a code word; nearest precedent F12)** |
| G4 | IRQ latency when an IRQ interrupts W: handler entry is X later than from a BIOS halt | X = BIOS entry + head + park + BIOS re-entry ≈ 100-350 cycles (2-3 I-cache line fills + 2 D-lines at ~46 cycles each, plus about 40 instructions) **(guess; M4/H3)** | not identical: residual X |
| G5 | Cache pollution: W evicts the game's I/D lines during the wait, so the frame after VBlank refills them | ≤ (256 I-lines + 128 D-lines) × ~46 ≈ 17.7k cycles ≈ 4.2 lines ≈ 1.6% of a frame, upper bound (F13); realistic much less **(guess)** | not identical: residual P. **Also true of today's OS_Halt design**, which architecture.md describes as never delaying the game |
| G6 | Early write-back of dirty game lines: W's reads evict dirty lines, so main RAM gets them sooner than stock | visible only to the ARM7 or DMA reading memory the game didn't flush (a game bug) | same exposure as today's design |
| G7 | If the SDK switches threads during the wait, the main thread's saved context has PC in module code (E) instead of BIOS ROM; SP and r4-r11 are the game's own values | thread-struct values written by the SDK itself | equivalent; E pushes nothing on the thread stack (the BIOS SWI uses the SVC stack and some user stack: GBATEK "How BIOS Processes SWIs", hw §1), so the bytes below SP and SVC SP differ as garbage |
| G8 | IRQ stack: `irq_head` *reads* the BIOS frame and moves SP_irq back to its pre-IRQ value; the re-taken IRQ writes a new frame in the same place | no writes by the module | identical memory effect to an IRQ taken from a halt |
| G9 | No SWI 5 is executed, so BIOS ROM lines aren't fetched into the I-cache | cache contents | benign |
| G10 | Power: W runs instead of halting | battery | not gameplay |
| G11 | If the DSi BIOS were the active ARM9 BIOS, its IntrWait does not really halt (GBATEK "DSi9: BUG", hw §1); E always halts with CP15 | wake-latency profile **(guess: nds-bootstrap NTR boots likely select the NDS BIOS via SCFG_A9ROM bit1; check in M1)** | E is the NDS9 behaviour |

Nothing else is written: no game heap, data, thread stacks, IRQ table, IE or IF. E does not ack IF, and `irq_head` re-raises by leaving IF set. The display extension (module-owned NDMA IRQs) is the exception: it needs IE bits, which is the display's own residual.

### 5.2 Why the toggled ITCM word is unobservable

Invariant: **toggle ON ⇒ no game instruction executes.**
- It is set in `w_resume` with IRQs masked, immediately before `movs pc` into W (module code only: rcheevos, lwIP, mbedTLS, newlib, all in the image), or before E's `mcr` halt when the display is armed (no instructions execute while halted).
- Every exit from those states goes through `irq_head` (toggle OFF as its first store), `w_yield` (OFF), or `e_done` (OFF).
- The only fetch of the word while it is ON is the IRQ fetch that lands in `irq_head`.
- DMA can't read TCM (GBATEK DMA section, hw §5), and the ARM7 has no access to ARM9 ITCM.
- An anti-piracy checksum over ITCM would need game code to run, which can't happen while the word is ON.

Test: M5 asserts the invariant at every instruction in the runner.

### 5.3 Why no lag frames from the wait itself

- The game's own deadline semantics are kept: SWI 5's discard happens at the call, and any later flag ends the wait.
- W can't hold the CPU past an IRQ: X bounds it.
- W holds no lock the game uses and touches no game I/O.

The two residuals, X per IRQ during waits and P after waits, are the same kind as the display's X (display §11.4). They are measured, not estimated (H6). **Strictly "zero" holds only if the owner rules these sub-scanline, hardware-measured bounds acceptable. Nothing in-CPU on the ARM9 can make P exactly zero**: any work on the ARM9 during the wait changes cache state. The existing OS_Halt design has the same P.

---

## 6. Coverage per census class

| Class (owner's 64 / RA estimate) | Phase 1 | Phase 2 |
|---|---|---|
| **bios** (9 / ~9%) | All main-thread VBlank wait time. Census upper bounds: 41-81% of each interval halted in the BIOS (census:46-54); Sonic Rush median 331k cycles/interval (idle:25). Sets: Sonic Rush 73.7k instr (~250k cycles at CPI 3.4) fits most frames; Pac'n Roll 80k fits. RE:DS 209k (~710k cycles) against a 200k-cycle mean wait (idle:27) **will fall behind** and drop frames (DEGRADED), with no lag | Late capture fixed by the core change in 4.4 |
| **oshalt** (53) | Unchanged (start.s:170-214). Exclusion with W: `bw_entry` gives W rights only if `in_halt` == 0; `halt_entry` runs `module_idle` only if `!w_busy` (else stock halt, as start.s:181-184 does for re-entry). Mixed games (RE:DS 11% OS_Halt) are correct, at some lost time | `halt_entry` becomes "capture at a safe point, then E-style halt that resumes W, park on IRQ". Fixes: Kirby's 20 wakes/frame no longer start 20 passes (W resumes where it was, brief); strip-only wakes don't start passes (display req. 3); B2 stale flag (display §11.4) is gone. Also lets W use non-idle halts (`halts_other`, module.c:576-582), safe under parking. Cost: X per idle-time IRQ (Kirby: ~20 × X per frame) |
| **busy** (2 / ~3%) | Detect and report (7.3) | Research only (7.2) |
| BIOS waits inside OS_Halt games (MKDS / ACWW / WarioWare / Trauma Center have direct SWI 5 callers, census:30-31) | Covered by the same wrapper hook in those phases | — |
| Inline `svc 5` not through the wrapper (loose counts in static.tsv, e.g. Kirby CC 2) | Not hooked; the census shows the wrapper is the main site in all 9 BIOS games | — |

---

## 7. Busy engines (no halt at all)

### 7.1 Is there a generic zero-cost ARM9 way? No.

- In a busy engine every ARM9 cycle runs game code. Taking a cycle is free only if the cycle would have been spent in a loop that cannot make progress until something happens.
- To prove that, one needs every writer of the loop's exit inputs:
  - ARM9 IRQ handlers;
  - the ARM7 writing main RAM (no ARM9 IRQ needed);
  - DMA;
  - time sources (VCOUNT, timers).
- From the ARM9 the last three are not observable at the moment they act, so any borrowed slice can delay the loop's exit by up to the slice length.
- Golden Sun DD's loop reads VCOUNT for its time base (F17), so its "idle" ends on time, not on an IRQ. Code Lyoko's `poll` does real decoding when its object is busy (F16), so the same loop is idle on some iterations and working on others.

Conclusion: borrowing can be *bounded*, never zero.

### 7.2 Best ARM9 option (not recommended under the zero rule): verified spin borrowing

It needs this proposal's machinery plus a periodic module IRQ:
- an NDMA one-shot with interval timing, or an H-Blank counter, which DS-mode games never use (brief);
- dispatched by an always-armed IRQ head, which breaks invariant 5.2 because the head must be ON while game code runs.

How it would work:
1. Sample the interrupted PC at the existing IPC hook (header->vblank, F11) to find the hot loop.
2. Statically decode the loop and its callees up to depth 2: no stores except sp-relative, no I/O loads, no SWI or MCR. Record the loop's watch set (constant-offset loads).
3. Learn the watch set's writers in a no-borrow phase.
4. Then borrow W slices of length Q, returning to the loop at every game IRQ and at every Q.

Game cost: ≤ Q + switch per exit not caused by an IRQ, ≤ X per IRQ exit, plus head cost on every game IRQ while borrowing. It fails 5.2 and needs a dispatcher patch like the display's R4. **Verdict: not zero; only with an explicit owner ruling, and after M7 shows the watch-set writers.** For GS DD (VCOUNT-based) the static rule already rejects it.

### 7.3 Recommended for busy engines now

- The module counts "frames with no wait of any kind" (no OS_Halt idle pass, no BIOS wait). After 600 consecutive frames it sets status `wait = none` and the in-game menu says "This game never waits: achievements can't be checked on the DSi".
- No per-game list: this is detected at runtime, so it covers unknown engines.
- The launcher can't know this in advance.

Leave the actual coverage to the ARM7 angle. The cache-staleness problem (write-back, F13) is the ARM7 angle's to solve.

### 7.4 Side finding (an nds-bootstrap-owned wait)

nds-bootstrap's CE9 busy-waits for the ARM7 on card reads: `waitForArm7()` polls `sharedAddr[3]` with `sleepMs(1)` between polls (cardenginei/arm9/source/cardengine.c:375-389). Its area shows up as 11.5% of Golden Sun's samples and about 185 VCOUNT reads per interval in every game (census:39-40, F17).

This is our own code, so hooking it needs no game patch. With the W model, W could run in that wait provided completion raises an ARM9 IRQ: a CE7 change to send an IPC sync when a read completes. Otherwise W would delay the read by up to a slice, which is what architecture.md open question 2 rejected. That is a fifth upstream hook point (CLAUDE.md asks for about 4), so it is listed as phase 3, after measurement.

---

## 8. Display strip interface (display §11.4 requirements)

| Req. | Provided |
|---|---|
| (1) "In the wait" PC ranges | One range, the module image `[0x02D80000, __text_end)`, plus `hk.wait_kind`. While `wait_kind == 2`, game code runs only if a *higher-priority* thread was woken. Its PC is then outside the module, so `in_wait = wait_kind == 2 && (hk.w_running \|\| interrupted_pc ∈ module)`. `irq_head` knows `w_running` directly; a strip IRQ landing in E's halt has PC = E |
| (2) Arming at wait entry | `hk.cb_enter(kind, vcount)` called by `bw_entry` with IRQs masked, before W or halt; `hk.cb_leave()` from `e_done` with IRQs masked, before return. The display must disarm its NDMA IRQ sources in `cb_leave` (and ack their IF bits). Once the head is OFF, the game's own dispatcher would index `OS_IRQTable` past its end for bits 28-31 (F10: lowest-bit table dispatch) |
| (3) Strip-only wakes go straight back | `irq_head`'s module-only branch handles them and returns through the BIOS epilogue to W or E. No pass is started; the head stays ON. While the strip is armed, E keeps the head ON during its halt, so a game IRQ there pays the passthrough cost (part of X) |
| (4) Main-thread VBlank wait vs. PXI waits | BIOS path: `wait_kind == 2` is exact (SWI 5 means VBlank). OS_Halt path (phase 2, **guess**): at halt, `[DTCM+3FF8] & 1 == 0` and the SDK's `OSi_IrqThreadQueue` non-empty suggests a thread is in `OS_WaitIrq` (the SDK's `OS_WaitVBlankIntr` sleeps there, notes:7). The queue address must be found by signature in `OS_WaitIrq` per SDK version (read only). Validate in M8 before the display relies on it |

The B2 stale flag can't occur: `wait_kind` is cleared with IRQs masked before `bx lr` back to game code, and a thread switch during the wait leaves PC outside the module.

---

## 9. Measurements (pass/fail fixed in advance)

All melonDS runs use the runner with the cache model and `--thumb-cycle` (brief caveat). One melonDS at a time (session rule). Hardware results are logged to RAM and dumped to SD only at quit or the menu (CLAUDE.md), then written up in TESTING.md.

### melonDS

**M1, E semantics.** A lab ROM (homebrew; it may use timers since it is ours) runs each case 1,000× against the real BIOS (melonDS with the owner's NDS9 BIOS dump, or FreeBIOS if none) and against E linked in:
- (a) flag already set before the call;
- (b) VBlank during the wait;
- (c) a non-VBlank IRQ during the wait;
- (d) caller CPSR.I = 1;
- (e) IME = 0 at the call;
- (f) the SWI 4 r0 = 0 bug;
- (g) nested call from an IRQ-woken thread (pure E).

Logged per call: VCOUNT and frame at return, DTCM+3FF8, IME, r4-r11/sp preserved. **Pass:** identical in every case. Fail on any difference.

**M2, BIOS games A/B.** Sonic Rush, RE:DS, Kirby CC, M&L PiT and Pokémon Ranger, each with the census gameplay or generic script, 9,000 intervals, achievements on vs off.

Pass if all of:
1. Lag intervals on ≤ off.
2. Per frame, cycles from the VBlank IRQ to the main thread's return from the wrapper: max(on - off) ≤ 400 at p100 over frames where both have a wait.
3. Sonic Rush and Pac'n Roll: `frames_evaluated / frames_snapshotted` ≥ 0.99 and `frames_dropped == 0`.
4. RE:DS: report the evaluated share (expected < 1; no pass mark, it feeds the gate).

**M3, park/resume integrity.** A debug W workload (SHA-256 over a 64 KB module buffer in a loop, checking against a precomputed digest) runs during waits in all 9 BIOS games for 5,000 intervals each. Also check that the caller mode at `bw_entry` is SYS. **Pass:** zero digest mismatches, ≥ 100,000 parks in total, no hang.

**M4, X distribution.** The runner logs, per IRQ, IF-set to the first instruction of the game's table handler. **Pass:** p100(on) - p100(off) ≤ 400 cycles for IRQs that interrupted W.

**M5, write and invariant audit.** The runner flags:
1. any ARM9 write with PC in the module image to game memory: main RAM 0x02000000-0x023FFFFF, DTCM except the word at +3FF8, ITCM except `h[0]`;
2. any executed instruction outside the module image and BIOS ROM while `h[0] != original` (invariant 5.2);
3. any +3FF8 write whose value differs from what the BIOS would write in the same state.

**Pass:** 0 / 0 / 0 over M2's runs.

**M6, coverage rerun.** Census with achievements on. **Pass:** every bios-class ROM shows module time > 0 in ≥ 95% of intervals that have a wait.

**M7, busy-engine watch sets** (for 7.2, research). On GS DD and Code Lyoko, find the hot loop's watch set and log every writer (ARM9 IRQ, ARM9 thread, ARM7, DMA) over 20,000 intervals. **Hypothesis "IRQ-only exits" holds** only if 100% of writes are from ARM9 IRQ handlers and the loop reads no I/O. Expected: fails for GS DD (VCOUNT).

**M8, OSi_IrqThreadQueue heuristic** (phase 2 display req. 4). For 10 OS_Halt games, compare the heuristic's "VBlank wait" verdict at each halt with ground truth from the runner (the main thread's PC in `OS_WaitVBlankIntr`). **Pass:** ≥ 99.9% agreement, and zero false "VBlank wait" when the main thread is runnable.

### DSi XL (and New 3DS XL in DSi mode)

**H1, BIOS ground truth.** The M1 lab ROM on hardware, 10,000 trials per case. It also dumps the NDS9 BIOS IRQ vector and the IntrWait code (not read-protected, hw §2) to confirm F9's frame layout and the register and stack use. **Pass:** E matches the BIOS in every case, and the frame is `{r0-r3,r12,lr}` at SP_irq on handler entry. Any difference means E is changed before H4.

**H2, toggle and park on hardware.** The lab ROM installs an SDK-signature IRQ handler in ITCM and runs M3's digest W for 10 minutes. **Pass:** no mismatch, no hang, park count logged.

**H3, X on hardware.** The lab ROM measures VBlank IF to handler entry with a timer (ours), W parked vs stock halt, 10,000 samples. **Pass:** Δp100 ≤ 400 cycles. Above that, the owner decides before H4.

**H4, Sonic Rush play.** 10 minutes of Leaf Storm.
- Baseline: the same build in observe-only mode (E without W).
- Logged per frame: wrapper calls that spanned ≥ 2 VBlanks (game lag), `frames_evaluated`, `frames_dropped`, parks, X max.

**Pass:** lag(W) ≤ lag(observe) in a paired run with the same route; ≥ 99% evaluated; 0 dropped; no audio glitch heard (logged by Clark).

**H5, RE:DS cadence.** 10 minutes. Compare the histogram of wrapper call→return spans (1 vs 2 VBlanks; the game is 30 fps) between W and observe mode. **Pass:** the share of 2-VBlank spans differs by ≤ 0.1 percentage points.

**H6, T1 slack probe extended for P.** This is the display's T1 (display §11.4) with a 4 KB D / 8 KB I hot working set:
- frame work ends k cycles before VBlank;
- W on vs off;
- 10,000 frames per k;
- k swept down.

`X_total` = the smallest k with zero lag difference. This is a report, not pass/fail: the owner rules whether `X_total` counts as zero. The result also applies to today's OS_Halt design (G5).

---

## 10. Risks

1. **Owner ruling on G3** (runtime toggle of one ITCM code word). The invariant in 5.2 makes it unobservable, but it is a new kind of change. The fallback is a permanent boot-time head patch, whose per-IRQ cost (≈10-110 cycles on every game IRQ, all session) lands in working time and fails the zero rule. Without either, BIOS games get no evaluation.
2. **The real NDS9 BIOS may differ from GBATEK and FreeBIOS:** I-bit handling, IME writes, register clobbers, stack use, IRQ frame layout. H1 decides. E is easy to adjust; the frame layout is load-bearing for `irq_head`.
3. **P (cache refill) and X are nonzero.** Strict zero depends on the owner accepting hardware-measured sub-scanline bounds, as for the display. P applies equally to the shipped OS_Halt design and is unmeasured today.
4. **IRQ handler not recognized** (`[DTCM+3FFC]` not the SDK handler, or out of `b` range in a low ITCM mirror). Phase-1 work is disabled for that game. E still runs, so this costs coverage, not stability. A 2-word `ldr pc` toggle could reach the low mirror.
5. **Capture vs suspended evaluation** (4.4). Heavy sets in BIOS games may miss captures and evaluate carries: an RA fidelity loss, counted in status. The fix is in the core (perf session).
6. **Header ABI change** (word 0 = branch = magic, v7) touches 3 readers in nds-bootstrap. `disable()` must never zero word 0 after the wrapper patch.
7. **Mixed games** (RE:DS) lose some W time to the OS_Halt/W exclusion until phase 2.
8. **The display's module IRQs need IE bits** that the game also writes (read-modify-write). Our writes happen only in E or the head while no game code runs, but a game IRQ handler that reads IE during the wait would see our bit. This is the display's residual; listed for completeness.
9. **Thread-switch corner:** a higher-priority thread woken during the wait that itself calls `SVC_WaitVBlankIntr` gets pure E (no W). Correct, less time.
10. **Wrapper false match** in data. The scan requires exactly one match inside the secure area (0x4000); zero or several leaves the game stock.
11. **Anti-piracy that hashes the secure area** would see G1, the same class as the OS_Halt patch. BIOS-class games are SDK 2.x (2005-06), before the AP era **(guess)**.
12. **melonDS fidelity:** no cross-CPU bus contention, the Thumb cycle bug. The hardware tests (H3-H6) decide.
13. **Phase 2 adds X to every idle-time IRQ** in OS_Halt games (Kirby about 20 per frame). Measure with M4 on KSSU before switching.

---

## 11. Rejected alternatives (and why)

| Alternative | Why not |
|---|---|
| Do the work, then execute the real SWI 5 if no VBlank came (notes:32) | A race between the check and the SWI: a VBlank in between is discarded, costing a lag frame. Masking IRQs across the SWI depends on unverified BIOS I-bit handling (FreeBIOS keeps the caller's I, so the wait would hang) |
| Cooperative admission: evaluate only if "lines left × 4260 > max cost × margin" (notes:33) | Probabilistic. The first unexpected spike overruns the frame start; the owner rejected "rare lag" (display §11). Kept only as a measurement mode |
| Yield inside rcheevos' `read_memory` callback | Not dense: condition evaluation after the memref update reads nothing for long stretches (cpu.md: `rc_client_do_frame` dominates) |
| Redirect the IRQ return from the existing CE9 IPC hook (header->vblank) | The ARM7's IPC arrives a variable time after the ARM9 VBlank (not zero). It must also write the saved lr in the game's IRQ stack frame (a game-stack write) |
| Hook the game's VBlank table entry (as CE9 does for IPC, F11) | Games replace their VBlank handler between scenes (`OS_SetIrqFunction`), so the hook must be re-installed by writing game DTCM, and a game reading its handler back sees ours |
| Make the main thread sleep via the game's own `OS_WaitIrq` | Lower-priority game threads would then run during the wait, which stock SWI 5 forbids: a behaviour change |
| 133 MHz for W only | SCFG_CLK is locked for NTR games (nds-bootstrap clears SCFG_EXT bit 31, hw §6), and GBATEK requires clock changes from ITCM code (game memory) |
| An ARM7 VCOUNT-match IRQ as a pre-VBlank deadline | NitroSDK's ARM7 touch auto-sampling uses V-alarms **(guess)**, so the VCOUNT IRQ isn't free; and parking makes a deadline unnecessary anyway |
