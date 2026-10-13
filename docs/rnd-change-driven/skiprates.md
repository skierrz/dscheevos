## Change-driven skip measurement: results for all 6 games (core sets, rc_client-default activation)

### 1. Per-game tables (pooled over 2 windows x 198 frames; frames 0-1 of each fresh-runtime window are excluded)
Setup: core official achievements only (Flags==3; the 1010xxxxx warning achievement is dropped), all leaderboards (hardcore), rich presence (RP). This models a fresh account: nothing unlocked, so nothing is deactivated, which is the worst case. Stock rc_runtime_do_frame runs every frame. Dependencies follow the audit's C1 rule: operand memrefs, including Recall memrefs; a modified memref counts as one dependency through its own changed flag. Running with full parent recursion (SKIPH_FULLDEPS=1) gave identical skip rates on all 6 games.

Variants:
- **S0**: no state write at f-1.
- **S1s** (your strict rule): STANDARD conditions only, required_hits 0, not preceded by a combining condition.
- **S1a** (the audit's rule): STANDARD, TRIGGER, PAUSE_IF, MEASURED_IF, MEASURED with no target, AND_NEXT, OR_NEXT, RESET_NEXT_IF; required_hits 0; pre value < 0xFFFFFFFF.

S1c (S1s plus chain finals) equals S1s on every game. All numbers below are median / p10 / p90 / min over frames.

| game | active ach / conds (excl. indirect; incl.) | S0 %trig | S0 %cond | S1s %trig | S1s %cond | S1a %trig | S1a %cond |
|---|---|---|---|---|---|---|---|
| mkds | 132 / 1139; 2150 | 49.2/49.2/49.2/25.0 | 37.0/37.0/37.0/18.7 | 51.5/51.5/51.5/27.3 | 38.2/38.2/38.2/19.9 | 51.5/51.5/51.5/27.3 | 38.2/38.2/38.2/19.9 |
| twewy | 98 / 60939; 61286 | 53.1/53.1/57.1/49.0 | 8.1/8.1/8.3/6.2 | 62.2/62.2/68.4/58.2 | 11.5/11.5/11.7/6.3 | 79.6/79.6/100/68.4 | 43.8/43.8/100/16.8 |
| soulsilver | 136 / 4255; 24603 | 30.9/30.9/31.6/4.4 | 71.5/71.5/71.7/1.3 | 78.7/77.9/78.7/8.8 | 93.3/93.1/93.3/3.9 | 100/99.3/100/15.4 | 100/99.8/100/4.6 |
| digimon | 49 / 10646; 11581 | 34.7 (constant) | 4.2 (constant) | 44.9 | 5.5 | 100/100/100/67.3 | 100/100/100/9.8 |
| platinum | 101 / 2635; 7545 | 69.3/68.3/69.3/63.4 | 57.3/57.1/57.3/53.9 | 98.0/97.0/98.0/72.3 | 65.9/65.8/65.9/55.6 | 100/99.0/100/73.3 | 100/99.8/100/89.0 |
| black | 118 / 470; 2599 | 26.3 (constant) | 14.9 (constant) | 99.2/99.2/99.2/97.5 | 98.3/98.3/98.3/89.8 | 100/100/100/98.3 | 100/100/100/91.5 |

Per window: the two windows are near-identical. The exception is SoulSilver window 0 (frames 7800-7999): S0 %cond p10 is 39.5 there against 71.5 in window 1.

Memrefs and inverted index (index pairs and fan-out count achievements only):

| game | memrefs (direct + modified) | changed/frame med / p90 / max | memrefs read by ach | (memref, trigger) pairs | fan-out mean / max | lb pairs | RP pairs |
|---|---|---|---|---|---|---|---|
| mkds | 102 + 124 = 226 | 12 / 12 / 16 | 112 | 1328 | 11.86 / 100 | 765 | 2 |
| twewy | 1494 + 24 = 1518 | 1 / 1 / 3 | 1512 | 8577 | 5.67 / 70 | 12 | 0 |
| soulsilver | 4 + 5035 = 5039 | 0 / 0 / 1424 | 1973 | 3670 | 1.86 / 136 | 259 | 0 |
| digimon | 382 + 627 = 1009 | 2 / 2 / 2 | 620 | 2112 | 3.41 / 46 | 6 | 84 |
| platinum | 21 + 2022 = 2043 | 0 / 0 / 2 | 432 | 931 | 2.16 / 64 | 63 | 0 |
| black | 382 + 1151 = 1533 | 0 / 0 / 0 | 400 | 771 | 1.93 / 90 | 0 | 0 |

Plus-one catch-up updates per frame under S1 (med / p90 / max; per skipped trigger is the median):

| game | S1s | S1s per skipped trigger | S1a | S1a per skipped trigger |
|---|---|---|---|---|
| mkds | 3 / 3 / 3 | 0.04 | 3 / 3 / 3 | 0.04 |
| twewy | 84 / 86 / 86 | 1.38 | 5104 / 13632 / 13632 | 65 |
| soulsilver | 116 / 116 / 116 | 1.08 | 192 / 192 / 192 | 1.41 |
| digimon | 46 / 46 / 46 | 2.09 | 4630 / 4630 / 4630 | 94 |
| platinum | 101 / 102 / 102 | 1.02 | 112 / 113 / 113 | 1.11 |
| black | 153 / 153 / 153 | 1.31 | 155 / 155 / 155 | 1.31 |

Why a trigger is not skippable, as % of active trigger-frames:

| game | S0 in_f | S0 in_f1 | S0 state | S0 tally_only | S1a in_f | S1a in_f1 | S1a state |
|---|---|---|---|---|---|---|---|
| mkds | 48.4 | 0.2 | 0.0 | 2.3 | 48.4 | 0.2 | 0.0 |
| twewy | 11.0 | 0.1 | 0.1 | 33.9 | 11.0 | 0.1 | 0.1 |
| soulsilver | 1.0 | 1.0 | 0.5 | 68.0 | 1.0 | 1.0 | 0.5 |
| digimon | 0.0 | 0.0 | 0.2 | 65.1 | 0.0 | 0.0 | 0.2 |
| platinum | 0.2 | 0.3 | 0.1 | 30.4 | 0.2 | 0.3 | 0.1 |
| black | 0.0 | 0.0 | 0.0 | 73.7 | 0.0 | 0.0 | 0.0 |

Leaderboards (one unit each: start + submit + cancel + value + progress + the runtime's lboards[i].value):
- S0 skips 0% of units on every game.
- S1a skips this share of leaderboard conditions (median, min): mkds 65 lb / 861 conds, 71.1% (constant); twewy 1 lb, 100%; soulsilver 30 lb, 100% (min 25.5); platinum 14 lb, 100% (min 16.5); digimon 1 lb, 0%; black has no leaderboards.
- Leaderboards whose value counts a hit-count timer (a Measured condition with a hit target): soulsilver 1, all other games 0.

Rich presence, per-frame work (values plus displays with hit counts):
- Only digimon has any: 172 conditions (2 values). S0 skips 0% of frames, S1a 100%.
- On every other game RP evaluates 0 conditions per frame. Displays without hit counts run only when the display string is built.

Cycle estimate: base × skipped condition share, where the share is ach skipped conditions over (ach + lb + RP conditions) and leaderboards and RP are always evaluated (v1). The "low" estimate also counts indirect (AddAddress/AddSource) conditions in the denominator as cost that can't be skipped, since memref and pointer-chain updates still run.

| game | base cycles/frame | S0 share → saving (med; worst frame) | S0 low | S1a share → saving | S1a low | S1a incl. lb |
|---|---|---|---|---|---|---|
| mkds | 763,780 (set bench; 693,376 at -O2) | 21.0% → 161k (81k) | 13.3% → 102k | 21.7% → 166k | 13.7% → 105k | 52.3% → 399k |
| twewy | 4,567k instr x CPI 3.3 = 15.07M | 8.1% → 1.22M (0.94M) | 8.1% → 1.21M | 43.8% → 6.60M (min 2.54M) | 43.5% | 43.8% |
| soulsilver | 2,128k x 3.3 = 7.02M | 66.5% → 4.67M (86k) | 12.0% → 0.84M | 93.0% → 6.53M | 16.7% → 1.17M | 100% |
| digimon | 1,076k x 3.3 = 3.55M | 4.2% → 148k | 3.8% → 136k | 98.3% → 3.49M | 90.5% → 3.21M | 98.3% |
| platinum | 1,527,448 (set bench, full set) | 55.1% → 842k (793k) | 19.5% → 298k | 96.2% → 1.47M | 34.1% → 521k | 100% |
| black | 848,004 (set bench) | 14.9% → 126k | 2.7% → 23k | 100% → 848k | 18.1% → 153k | 100% |

Cost-model assumptions:
- Cost is proportional to conditions.
- The cost of the skip check is not included. From the data it is changed memrefs x fan-out, plus one flag test per trigger, which is tiny here.
- The S1 catch-up cost is not included. It matters for TWEWY (S1a: about 5k-13.6k increments per frame) and Digimon (S1a: 4.6k per frame).
- The memref update cost is not removed: the "low" column only approximates it through indirect conditions.
- Platinum's base was measured on the full set, but the shares are from the core set.
- The CPI 3.3 bases are crude.

All-achievements variant (core + unofficial; S0 %cond / S1a %cond, medians):

| game | achievements | S0 %cond | S1a %cond |
|---|---|---|---|
| mkds | 133 | 37.2 | 38.5 |
| twewy | 98 (identical to core) | 8.1 | 43.8 |
| soulsilver | 170 | 69.4 | 100 |
| digimon | 49 (identical to core) | 4.2 | 100 |
| platinum | 114 | 25.9 | 100 |
| black | 208 | 10.3 | 100 |

### 2. Validation: 0 violations everywhere
On every frame the predicate called skippable, I checked that the stock evaluation at f left the unit's bytes exactly equal to the pre-state, plus 1 on f-1's tally set, with no events, and the leaderboard's external value unchanged.

Checked unit-frames, core run (violations 0 in every cell):

| game | S0 | S1s | S1a |
|---|---|---|---|
| mkds | 25,676 | 40,328 | 40,724 |
| twewy | 21,692 | 26,006 | 35,244 |
| soulsilver | 16,335 | 53,345 | 64,685 |
| digimon | 6,732 | 8,725 | 19,781 |
| platinum | 27,976 | 44,871 | 45,661 |
| black | 12,674 | 46,726 | 47,122 |

The all-achievements and full-deps runs also had 0 violations.

The validator does catch real mistakes. With deliberately broken predicates:
- **Indirect memrefs ignored as dependencies (SKIPH_MUTATE=1):** soulsilver 191 violations at S0 and 255 at S1a; twewy 1; platinum 11 at S1a.
- **Change at f-1 ignored (SKIPH_MUTATE=2):** soulsilver 14 at S0 and 40 at S1a; platinum 9 at S1a.
- **mkds, digimon, black:** these broken predicates happened to cause no violations.

State-coverage checks:
- The per-unit buffer size is recomputed exactly with stock preparse, and malloc_size is at least that size.
- Every trigger, condset and condition pointer lies inside its unit's buffer (0 outside).
- 0 memrefs (direct or modified) lie inside any unit buffer, so memrefs are in the shared pool as expected.
- No achievement or leaderboard uses RP variables.

### 3. How to run it
- Run one game: `bash .../skip/harness/run.sh <game> [core|all]`. The game's window starts are read from skip/plan.tsv, 200 frames each.
- Rebuild: `.../skip/harness/build.sh` (host cc, stock rcheevos 683197d, read-only).
- Regenerate the defs: `python3 -I .../skip/harness/extract.py <sets json> .../skip/harness/sets/<game>`. This writes `.core.defs`, `.all.defs` and `.rp`.
- Direct use: `skiph DEFS RP OUTPREFIX DUMPDIR WINLEN START...`.
- Env options: `SKIPH_FULLDEPS=1` (recurse into pointer-chain parents); `SKIPH_MUTATE=1|2` (validator self-test).
- If you loop over games from zsh, the starts variable must word-split (use bash). Otherwise only window 0 runs.

Outputs, in `.../skip/harness/out/`:
- `<game>.<variant>.frames.tsv`: one row per frame, with header. It holds per-variant skip counts for triggers and conditions, plus-one counts, changed memrefs, reason counts, leaderboard and RP columns, failed reads, and the DTCM flag.
- `<game>.<variant>.info.txt`: inventory, fan-out, top dirtying memrefs, sanity checks, events, validation.
- `<game>.<variant>.viol.txt`: one line per violation.
- `<game>.<variant>.summary.txt`: the tables above.
- Also: `<game>.fulldeps.*` and `mut{1,2}-<game>.*`.

Sources: skiph.c, summarize.py, run.sh, build.sh, extract.py, all in `rnd scratch skip/harness/`.

### 4. Surprising rcheevos behaviour and data findings
- **Plain tallies are the main thing blocking S0.** Always-true conditions with required_hits 0 increment every frame, and that is the reason 30-74% of trigger-frames can't be skipped under S0 (mkds only 2%). S1 recovers most of it. TWEWY and Digimon need the audit's wider S1a set (AndNext/OrNext etc. tallies), and its catch-up is large there: TWEWY med 5.1k and max 13.6k plus-ones per frame, about 65 per trigger; Digimon 4.6k per frame.
- **Leaderboards are skippable under S1, contrary to the audit.** The audit says rc_test_trigger forcing ACTIVE makes a leaderboard never clean. On this data it is a write-then-restore: evaluation doesn't depend on the stored ACTIVE/PRIMED/PAUSED state, only WAITING. Pre/post snapshots are clean, and S1a skips were validated with 0 violations. Leaderboards are not skippable under S0 only because every start/cancel trigger carries plain tallies.
- **mkds: one pointer-chain memref dirties half the set every frame.** It is an indirect read via 0x1661b0, size 1, and 64 achievements read it. Another 32-achievement memref via the same pointer and two 31-32-achievement depth-4 chains also change every frame. The likely race state or timer caps skipping at about 50% of triggers.
- **Twewy:** one direct memref (0x073aa4, 24-bit, fan-out 20) changes on 213 of 396 frames.
- **soulsilver:** 4 RESET returns (ResetIf). One frame changed 1,424 memrefs (the S0 min of 1.3%). 2 frames had 2 failed reads.
- **black:** 1 failed read every frame (a pointer chain out of range). The runtime path does not disable anything.
- **Activation:** has_memrefs==1 triggers never occur through rc_runtime (C4 does not apply). All parses succeeded.

### Caveats
- **Most windows are early-game intro or cutscene scenes, not normal play.** Frames from the runner screenshots:
  - Platinum, SoulSilver and Black: the player's bedroom with dialogue boxes.
  - Digimon: the office intro with dialogue.
  - TWEWY: the intro sequence.
  - mkds: a race, lap 1, with the kart stuck against a wall at f4500. The race timer is running and memref values are non-zero, so it looks sane.
- **RAM is nearly static in these windows** (4-byte words changed per frame in the first 4 MB): Platinum 19, SoulSilver 200, Black 160, Digimon 141, TWEWY 327, mkds 1,769. Treat these skip rates as optimistic upper bounds for real play.
- **Black and mkds have few non-zero memrefs**: Black 12 of 382 direct memrefs non-zero, mkds 14 of 102 direct.
- **Dump layout:** dumps are 16 MB and only the first 4 MB are mapped, plus a valid 16 KB DTCM on every frame.
- **Sample and timing:** 400 frames per game, and the runner's input is scripted. Dumps are taken at frame end, not at the module's halt point.
- **Fresh account:** each window starts a fresh runtime, so long-run hit counts are absent. No achievement triggered.