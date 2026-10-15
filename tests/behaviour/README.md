# Behaviour-identity gate

Owner standard (2026-10-15): "game's behavior is identical, with sound as close to identical (if not identical) as
possible". Reference: the same game through unmodified nds-bootstrap, same settings, same start phase, same input, in
melonDS (deterministic). Implemented as one timeline (as research/optimize/audio-identical.md): a DScheevos run saves
a state in play; the reference loads it with `--dsc-neutral` (OS_Halt back to stock, ce7 valueBits bit 28 cleared:
nds-bootstrap's own no-session path), the run under test loads it as is. Both use `--dsc-nonet` (a live network makes
a DScheevos run non-repeatable in melonDS). Same state, same input script, same runner: any difference is ours.

```
run.sh save GAME STATE                           # once per build: state in play
run.sh load GAME STATE REF --dsc-neutral --dsc-nonet
run.sh load GAME STATE RUN --dsc-nonet
bgate.py REF RUN                                 # PASS/FAIL per category, first frame and address
```

## Categories (bgate.py)

| category | compared | PASS means |
|---|---|---|
| state (quiescent) | game-visible memories at the game's quiescent instant of each VBlank interval, block hashes with the exclusions below | every record identical, same records present |
| state (boundary) | the same at each runner frame end (line 262) | informational: a game busy at that instant shows timing, not behaviour |
| screens | top and bottom framebuffers per frame | identical every frame |
| lag | melonDS's lag flag (no keypad read in the frame) and the profile's lag (no idle in the interval) | identical per frame |
| input | the keypad values the game read per frame (ARM9 KEYINPUT; ARM7 KEYINPUT/EXTKEYIN), consecutive repeats folded | identical per frame (read counts are informational: polling) |
| sound commands | ARM7 SPU register writes (register, size, value) per frame | identical per frame; time within the frame may differ |
| IPC | IPCSYNC and FIFO-send writes per CPU per frame (the ARM9's sound commands travel this way) | identical per frame |
| sound samples | 48 kHz PCM | a figure, "as close as possible" |
| timing-sensitive reads | VCOUNT, DISPSTAT, timer counters read by game code, per (cpu, pc, register) | diagnostic: where the game first saw a different value |

Memory is compared by 64-bit hashes of 4 KB blocks; `--bgate-dump` + bgdiff.py give byte addresses; `--watch-write
ADDR:SIZE --watch-log FILE` + wdiff.py give the writers. No tool prints memory contents (the RA token must never be
printed), only addresses, counts and "differs".

## What "game state" is, and what is left out (first principles)

Game state = memory whose contents the game can observe later. A byte differing between the two runs is a behaviour
difference unless it is provably never read by game code before being overwritten, or is not the game's.

Compared (game-visible): main RAM 0x02000000-0x023FFFFF; the SDK system areas 0x027FF000 and 0x02FFF000 (4 KB each);
ARM9 DTCM and ITCM; shared WRAM; ARM7 WRAM; VRAM A-I; palette; OAM; configuration registers (IME/IE, POWCNT, VRAMCNT,
WRAMCNT, DISPCNT, timer and DMA control). Main RAM elsewhere (nds-bootstrap's ROM cache and code) is reported
separately as not game memory; the DScheevos module region (0x02D80000-0x02F7FFFF) is never hashed.

Left out, each applied identically to both runs; the masked bytes are still hashed per rule, so the report says how
often each rule hid a difference and whether the masked ranges themselves differed:

| rule | what | why it is not game state |
|---|---|---|
| hook | the 8 bytes of OS_Halt DScheevos patches | our code (the neutral reference puts the stock bytes back) |
| stk9 / stk7 | each SDK thread's stack below its stack pointer | dead: the ABI never reads below SP; anything there is written before it is read |
| idle9 | the ARM9 idle thread's saved context and whole stack | the SDK idle thread is a stateless loop (`OS_Halt` forever); its registers and stack carry nothing the game reads. In a DScheevos run it is the thread our module runs on |
| ctx9 / ctx7 | the running thread's saved context | stale: rewritten at its next switch-out, before the next switch-in reads it |
| irq9 / irq7 | IRQ stack below SP_irq (from the next stack below) | dead, as above; it differs because an interrupt that arrives while our code runs pushes our registers |
| svc9 / svc7 | SVC stack below SP_svc (from the IRQ stack's top) | dead, as above |
| cache lines | (nothing to mask) | melonDS's cache model keeps tags only; RAM is always the CPU-coherent view |

Not left out: thread contexts of blocked game threads, live stack frames, every other byte. Thread lists come from the
SDK's OSThreadInfo pointers (0x027FFFA0/A4 or 0x02FFFFA0/A4); offsets (next pointer, stack bounds) are found by
checking the list structure, and the result is in BASE.bg.meta. If a thread list or stack bound can't be found, the
rule doesn't apply (nothing is masked) and the record is flagged.

## The quiescent instant

A fixed time (the frame end) catches a busy game mid-computation, where any timing shift shows in its registers, stack
and half-written data without any change in what it computes. So the primary comparison is at the first instant after
each VBlank at which the SDK has switched to the idle thread, outside any interrupt (OSThreadInfo.current = idle thread,
irqDepth 0, CPU not in IRQ mode): every game thread is then blocked at a point of its own program. The idle thread is
the one current at the game's first OS_Halt call. This instant exists for OS_Halt-idle games (all games measured here);
for others only the boundary comparison applies.

## Outputs (BASE.bg.*)

- `.meta`: regions (name, base, size, block size, first block index), rules, idle thread, thread lists at the end.
- `.snap`: records of 24-byte header (magic, kind 0 = frame end / 1 = quiescent, key = frame / VBlank count, runner
  frame, VCOUNT, flags), per-rule masked-content hashes, per-rule range hashes, per-rule masked byte counts, block hashes.
- `.frames.csv`: per frame ARM7 time at the start, screen hashes, melonDS lag flag, keypad reads.
- `.io.csv`: timing-sensitive reads and IPC writes with cpu, time, pc, value, repeat count.
