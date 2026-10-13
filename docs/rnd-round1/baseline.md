# Cost baseline per frame (ARM9 at 67 MHz, 1,120,380 cycles per VBlank interval)

All numbers come from melonDS with the cache model, which tends to be optimistic. Where a figure is computed by me rather than measured, it says so (D = derived).

## Sources

- **[IW]** `round/idle-windows.txt`: idle the module can get per frame (mean, p10, worst 60- and 1024-frame window).
- **[ID]** `memplan/idle.md`: idle per game, plus qemu instructions per frame on real RAM.
- **[CPU]** `memplan/cpu.md`: qemu instruction split on random RAM, older core.
- **[PM]** `round/profile-mkds.txt`: MKDS in-game profile, ARM -Os, about 2,900 evaluated frames. This run is from before the per-pass fix.
- **[KB]** `evalspeed-team/overhead/out/kirby-base/{idlepcs.csv,ab.txt}`: Kirby SSU in-game profile on the current overhead base, 6,056 evaluated frames, `--thumb-cycle`, cache model. I grouped it with `overhead/roles.py` and a per-function pass over the same CSV, dividing cycles by 2 (`cpi.py:68`).
- **[TM]** `evalspeed-team/TEAM.md:69-86`.
- **[BR]** `round/BRIEF.md`.
- **[SS]** `round/set-stats.tsv`.

The D-miss share is computed (D) as D-read misses × 46-52 cycles per line fill plus D-write misses × 18-19 cycles per store to an uncached line ([TM]:71). melonDS has no write buffer, so the write part may be overcharged (`round/plan.md:50`).

## Table (cycles per evaluated frame)

| | Kirby SSU | MKDS | Platinum (full set) | Pokémon Black | TWEWY | HG/SS (SoulSilver) |
|---|---|---|---|---|---|---|
| **Idle: mean / p10 / w60 / w1024** [IW] | 708k / 691k / 674k / 688k | 387k / 311k / 211k / 319k | 809k / 645k / 424k / 762k | 716k / 676k / 686k / 704k | 392k / 193k / 205k / 239k | 800k / 627k / 106k / 742k |
| **Our total** | **556k** [KB] (runner: 561k avg idle work per interval, [KB] ab.txt) | **873k** [PM] TOTAL | 1,527k set bench, cold, real RAM [TM]:73. No in-game profile | 848k set bench [TM]:73. No in-game profile | 4,567k instr [ID], about 15.1M cycles (D, at CPI 3.3) | 2,128k instr [ID], about 7.0M cycles (D) |
| Snapshot capture/copy | **~191k**: copy_snapshot 141.7k, dsc_capture 25.0k, interrupt_pending 9.1k, arena_alloc 7.1k, capture 5.8k [KB]. Per wake, 20-37 wakes a frame ([TM]:81) | copy_snapshot 2.8k [PM]:45 (pre-fix) | not profiled. Capture 23.5k instr on random RAM [CPU] | capture 2.2k instr [CPU] | capture 6.1k instr [CPU] | capture 121.7k instr: the 4,096-entry table overflows, so reads go live [CPU] header |
| Pointer-chain following | run_chains 15.7k [KB] | run_chains 2.65k [PM]:46 | not profiled. 3,888 AddAddress conditions [SS]:2 | 1 AddAddress [SS]:23 | 205 AddAddress [SS]:24 | 17,306 AddAddress [SS]:26 |
| rcheevos evaluation, including our read_memory callback | **201.8k** (56.8k instr, CPI 3.55) [KB] | **785k** rcheevos plus 15.8k read_memory = **~801k** [PM]:2,33 | bulk of the 1,527k | bulk of the 848k. rc_client_do_frame is 357k of 399k instr [CPU] | rc_client_do_frame 4.64M instr [CPU] | 1.99M instr [CPU] |
| D-miss share of evaluation | 1,489 DR + 468 DW per frame = 77-86k, **38-43%** (D) [KB]. I-miss 443 per frame, about 20k | 5,545 DR + 1,623 DW per frame = 285-318k, **36-41%** (D) [PM]:2. I-miss 1,251, about 58k. rc_reset_condset alone 70k at CPI 10.5 ([PM]:22) | n/a | n/a | n/a | n/a |
| Client/network/journal/other | network (net, link_busy, lwIP, mbedTLS) 35k. module_idle 33.3k, halt_entry 10.8k, dsc_idle glue 10.0k, idle_frames 5.9k, drain 5.3k, dsc_get_stats 3.3k. libc mem* 31.7k, split between rcheevos and our code. Remainder about 10k [KB] | mbedtls 23.3k, linker veneers 4.3k, module.o 2.1k, net 0.9k, libsysbase 0.8k, lwIP 0.4k, start.o 0.2k, heap 0.2k. libc 30.7k, of which memset 19.8k is rcheevos' per-trigger memset (`plan.md`:23) [PM] | glue outside do_frame 48.6k instr [CPU] | glue 42.1k instr [CPU] | glue 196k instr [CPU] | glue 115k instr [CPU] |
| IRQ-time work | VBlank call avg 641 / max 699 cycles [KB] ab.txt ("module cycles in CPSR IRQ mode"). capture() also runs with IRQs masked during idle (`module.c:641-666`; M5 counter, `plan.md:15`) | about 600 cycles per VBlank, queue only ([BR]:10) | same | same | same | same |
| **Fits every frame?** | Yes. 556k against 688k w1024, but that is 79% of idle and is mostly overhead | **No.** 2.3x the mean idle, 2.7x w1024 | No: about 1.9x mean (D) | No: 848k against 704k w1024, about 1.2x (D) | No: about 15x. The 1.1 MB set reply exceeds the 256 KB `DSCM_SESSION_MAX`, so the session can't load ([ID]) | No: about 3.5x the mean [ID] |

Additional context:

- **Required CPI for zero dropped frames** ([TM]:76-79): TWEWY 0.05, HG/SS 0.35, Digimon 0.70 (impossible on the ARM9 alone), MKDS 1.19, Platinum 1.25, Black 1.94. Measured evaluation CPI is 3.3-3.5 ([BR]:36).
- **Build deltas:** rcheevos at `-marm -O2` saves 70k on MKDS, 13k on Kirby and 52k on Black ([TM]:75). Thumb's measured 12% gain is an emulator artifact (`plan.md:42`).
- **Kirby lag:** the [KB] run logged 172 lag intervals out of 6,000 (2.87%) with achievements on. That file has no achievements-off half, so it is not yet known whether we cause any of it. This needs an A/B before anyone treats Kirby as zero-cost.

## Notes

1. **Kirby is overhead-bound and MKDS is evaluation-bound.** On Kirby, capture is about 191k against 202k of evaluation. Kirby's idle thread wakes 20-37 times a frame, and the snapshot must hold "memory at the game's last halt before VBlank" ([BR]:47-51, [TM]:81), so capture is repeated on every wake. On MKDS, rcheevos is 90% of the 873k total ([PM]).
2. **Platinum and Black have no in-game breakdown yet.** The set bench figures in [TM]:73 are cold totals. Next step: run the `overhead/run_cpi.sh` profile on `platinum` and `black` (the [IW] slugs), and also on the profile games for which no in-game profile is listed (Kirby and MKDS have one), all on the current base. That would fill the "n/a" cells. The MKDS run on the current base (`overhead/out/mkds-base`) had not produced data when I read it; its ab.txt shows the emulator lock was still busy.
3. **The D-miss share is a lower bound on the memory stall.** It counts only line fills and uncached stores. CPI of about 3.3 against a cache-hit baseline of about 1.3-1.8 suggests most of the excess CPI is memory. The write-back count is not measured: an inferred 1.7-1.9k per frame, about 80k cycles (`plan.md:51`).
4. **The table holds totals, not breakdowns, for these columns.** HG/SS and TWEWY are qemu instructions only; their cycle figures are D at CPI 3.3. [CPU] uses random RAM and an older, slower core (see [ID] "Method"). Digimon (1,076k instr against 755k w1024) behaves the same way ([ID]).

## Not rcheevos evaluation: candidates to offload

These must keep the snapshot semantics. Capture also needs ARM9 cache coherency: a watched line is dirty at VBlank in 88% of Platinum frames, so the ARM7 or NDMA must not read game RAM directly (`evalspeed-team/arm7-notes.md:52-69`).

- **Snapshot copy:** `copy_snapshot` (`core/src/dscheevos.c:2510`), `dsc_capture` (:2840), interrupt_pending, arena_alloc, capture(). This is the largest piece on Kirby, about 191k per frame.
- **Pointer-chain following:** `run_chains` (`dscheevos.c:964`), 15.7k on Kirby. It dominates capture on chain-heavy sets: HG/SS has 17k AddAddress conditions, Platinum 3.9k. Chain targets are also excluded from the dirty-line count, so they are incoherent too.
- **Network:** lwIP, mbedTLS and net/link_busy, 23-35k per frame (`module.c:279` poll_network). The TLS handshake took 2,783 ms on Kirby ([KB] ab.txt).
- **Module glue and housekeeping:** module_idle, halt_entry, dsc_idle glue outside do_frame, idle_frames, drain, stats. About 69k on Kirby and 3-5k on MKDS (pre-fix).
- **Linker veneers:** 4.3k on MKDS. This is fixable with `--use-blx` (`plan.md:25`). It is a build fix, not an offload.

## rcheevos evaluation: only memory placement, cache tricks, build flags or the public API

- **The rcheevos hot path:** rc_test_condset_internal, rc_test_condition, rc_evaluate_operand, rc_reset_condset (store-miss bound), rc_client_do_frame's own loop (66k on MKDS, CPI 7), rc_evaluate_trigger, plus the eval_state memset in libc (`rc_trigger.c:186`, `rc_value.c:339`). On MKDS this is about 785k.
- **Our read_memory callback** (`dscheevos.c:2084`, 10-16k): ours to speed up (`plan.md` rank 4), but rcheevos calls it synchronously, so it moves with rcheevos and cannot be offloaded separately.
- **Levers already ranked in `plan.md:17-32`:**
  - set data and stack in NWRAM-B/C, gated on the MPU and latency measurements M1/M3;
  - -O2 plus hot link order;
  - inline memset;
  - LTO on runtime files only;
  - hot .text in NWRAM.

  The best case is MKDS at about 560-620k, which is still above its 319k w1024. Closing that gap needs either a second processor running rcheevos unmodified (ARM7: 465-510k free cycles per frame in gameplay, `arm7-notes.md:41-49`; that lane belongs to arm7eval and idle-coverage) or the float build, which needs an RA ruling.