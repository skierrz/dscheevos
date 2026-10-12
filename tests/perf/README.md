# Instruction counts (qemu-arm)

How many ARM946 instructions the core spends per frame, counted exactly under
qemu-arm. The same source gives the same count on any machine, so CI can fail
when the in-interrupt path grows.

```
tests/perf/run.sh                    # typical and heavy sets, prints the table
tests/perf/run.sh --sets typical     # one set
tests/perf/run.sh --frames 20        # quicker, fewer frames
tests/perf/run.sh --gate             # also check tests/perf/limits.txt, exit 1 if over
```

Each run builds `dscheevos/core/src/dscheevos.c` and rcheevos from source with
the rules and flags of `dscheevos/module/Makefile` (Thumb -Os, with rcheevos'
runtime and client and the core's hot functions in ARM; same devkitARM),
links them with `perf.c` into a Linux ARM ELF, and runs it under
`qemu-arm -cpu arm946`. Output goes to `build/perf/`.

`perf.c` fills a fake 4 MB main RAM and 16 KB DTCM with random words (one in
eight looks like a 0x02xxxxxx pointer), loads a synthetic set from
`tools/rcheevos-footprint/harness/gen_set.py` through a fake server, then for
each frame changes 512 random bytes and calls:

- `dsc_capture` – the snapshot, taken as the module does when the game
  halts (`config.capture_at_halt`). First half of the frames: the queue is
  empty. Second half: idle runs only every 4th frame, so the snapshot is
  compared against the frame still queued (repeat check).
- `dsc_vblank` – counts the frame and hands it that snapshot
  (`dsc_vblank_queue`; the next `dsc_capture` or `dsc_idle` queues it). This
  is the core's part of what runs in the game's IPC IRQ on the console.
- `dsc_idle` – one frame evaluated by rcheevos (first half only).

The first frame is copied inside `dsc_vblank` (nothing captured yet) and is
left out of the counts.

Sets: `typical` is `gen_set.py 60 8 1 3` (300 frames), `heavy` is
`gen_set.py 250 40 1 6` (6 frames), the shapes used in
`docs/rcheevos-arm9-footprint.md`. The heavy set needs more addresses than
`max_addresses` (4096): the table holds the first 4096 and each `dsc_idle`
reads the rest live (about 30,000 bytes over 6 frames, ~1.6M instructions
a frame). Its `dsc_capture` is steady from the first frame.

A full run takes about 10 minutes under Docker on an Apple Silicon Mac
(x86 qemu through emulation), less on x86 Linux; nearly all of it is the
`dsc_idle` calls, which qemu has to log too.

The table gives median and max instructions per call, then a breakdown of
`dsc_vblank` by function (instructions in each function itself, not its
callees; static functions inlined by -Os count in their caller).

How it counts: qemu logs every translation block when it is translated
(`-d in_asm`, with its bytes) and every time it runs (`-d exec,nochain`).
`count.awk` sums block sizes between calls to the `mark_*` functions in
`perf.c`. This matches `-one-insn-per-tb` counting to within the marker's own
3 instructions.

Requirements: Docker. `run.sh` runs itself in `devkitpro/devkitarm:20241104`
with a static qemu-arm from Ubuntu 24.04's `qemu-user-static` (fetched into
`build/host/qemu-arm` on first use, or set `QEMU_ARM`). Inside a devkitARM
environment with `qemu-arm` on the PATH it runs directly. The log parsing
expects qemu's `OBJD-` byte lines; other qemu builds may print disassembly
instead, and `report.py` stops if a call counts as 0.

## limits.txt

`<set> <metric> <limit>` per line, `#` comments. Metrics: `vblank_max`,
`vblank_median`, `idle_median`. `--gate` fails if a measured value is over
its limit. Lower the limits when a change makes the snapshot cheaper, so it
can't drift back up unnoticed.

## Real sets (realsets.sh)

```
tests/perf/realsets.sh <dir of achievementsets replies> [game id...]
FLOAT=1 FRAMES=60 tests/perf/realsets.sh ../private/sets
```

Same idea with real RetroAchievements sets: `realsets.c` reads a set file
(`sets-<game>-<hash>.json`, the server's `achievementsets` reply; not in the
repo) at run time, so one build covers every set. Per frame it counts
`dsc_capture` and `dsc_idle` through the core (as the module drives it) and,
in a second run, `rc_client_do_frame` alone. `realsets_report.py` prints
median and p95 instructions per frame and the ARM9 time that is at CPI
1.3-2.0, at 67 and 133 MHz.

- Game RAM is random with pointer-shaped words unless `RAM_<game id>=<dump>`
  gives a 4 MB main RAM dump (optionally followed by 16 KB of DTCM). Random
  RAM changes which conditions are true and where pointer chains lead, so
  treat the counts as an estimate of the real evaluation cost.
- Counting uses a qemu TCG plugin (`qplug/icount.c`): the per-call counts are
  exact, and runs are fast enough for the biggest sets. `realsets.sh` builds
  the `dscheevos-qplug` image from `qplug/Dockerfile` on first use (qemu
  8.2.2 from source, a few minutes).
- `FLOAT=1` adds a variant built with `realsets-float.h` force-included
  (rcheevos' operand constants stored as float). It is for information only:
  not shipped, and it would need RetroAchievements' agreement.
- Both runs also print an evaluation digest (every memref value, condition
  hit count, trigger, leaderboard and rich presence state after each frame,
  taken outside the counted calls) and newlib heap figures. Memory doesn't
  change between capture and evaluation here, so the core's digest must
  equal rc_client's; the report flags any set where it doesn't, and shows
  the core's own heap (core run minus rc_client alone, the snapshot arena
  included). `OUT=build/<dir>` keeps several runs apart.
