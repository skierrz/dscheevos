## Change-driven skip: rcheevos semantics audit (stock 683197d)

Read-only audit; no files written. All citations are `file:line` under `~/Developer/DScheevos/dscheevos-rnd/external/rcheevos/` (paths below are relative to that).

**Bottom line**
- **S0 is exact**, given 5 corrections (C1–C5). Two of the draft's stated facts are wrong in how they work, but its conclusions survive.
- **S1 is exact** once "plain tally" is defined by the corrected rule below.
- **S0 will skip much less than the draft implies.** Any true condition with no hit target adds a hit every frame (`rc_condset.c:442-444`), and a true PauseIf with no hit target does too. So PRIMED triggers, triggers paused by an ordinary PauseIf, and any trigger with one true plain condition (for example "in level 3") are never S0-clean. rnd's skip-rate data should measure S0 and S1 separately.

### (A) Semantics: what blocks a skip, and when skipping is still exact

| Semantic | Why it isn't skippable / when it is | Citation |
|---|---|---|
| True condition, `required_hits==0` | Adds a hit every true frame, so it writes state every frame. Never S0-clean. It is S1's target case. | `src/runtime/rc_condset.c:438-444` |
| True condition, `required_hits>0` | Adds a hit until the target is met, then stops writing. S0-clean once met. While false with hits, it writes nothing (`has_hits=1`, true only if met). | `rc_condset.c:446-453`, `:455-458` |
| AddHits / SubHits | The condition's own `current_hits` goes into `add_hits` (`:595`, `:604`). That total is used by the next condition with `required_hits!=0`, including the Measured hit path. **Leak:** `add_hits` is cleared only inside `rc_condset_evaluate_total_hits` (`:482`) or per condset (`:689`). A consumer with `required_hits==0` neither uses nor clears it (`:490`), so it carries into a *later, unrelated* hit-target or Measured condition in the same condset. Never a plain tally. | `rc_condset.c:469-499`, `:592-608`, `:689` |
| PauseIf, no hit target | True: adds a hit every frame, sets `is_paused`, stops the group (`:514-521`), so a PAUSED trigger is not S0-clean. False: `current_hits=0` (`:523-525`). S1-eligible. | `rc_condset.c:511-530`, `:703-712` |
| PauseIf with hits (lock) | Once `current_hits==required_hits`, stays true with no writes (`:451-452`, `:455-458`), so a locked-paused trigger is S0-clean. Unmet and false: no write (`:527-529`). A ResetIf in the same group is not evaluated while it is paused (`:709-711`). | `rc_condset.c:451-458`, `:527-529`, `:709-711` |
| ResetIf true | Sets `is_true \|= 0x02` and `was_reset`. Hit-target and "other" groups are skipped (`:727`, `:765`). Measured conditions are zeroed and then re-evaluated (`:739-746`). Then all hits are reset (`rc_trigger.c:274`). If `has_hits` was 1: write and return RESET (`:277-284`). If `has_hits` was 0: a byte-level fixed point (measured hits go 0→1→0). From WAITING with ResetIf true, the state moves to ACTIVE (`:288-315`). | `rc_condset.c:532-549`; `src/runtime/rc_trigger.c:251-290` |
| ResetNextIf | Zeroes the next condition. `reset_next` is **not** cleared by AndNext/OrNext (`_no_add_hits` path), so it covers the whole following chain. It is cleared at `:496`, `:580`, `:598`, `:607`. Returns RESET once, only if a nonzero count was zeroed (`:428`); that is a write, so not clean. | `rc_condset.c:426-432`, `:610-612`; `rc_trigger.c:318-319` |
| AndNext / OrNext | Pure in-evaluation flags (`and_next`/`or_next` reset at `:463-464`). The source condition tallies like any other (S1-eligible if it has no hit target). | `rc_condset.c:434-436`, `:614-620` |
| Measured, no target | `measured_value` = operand1 value, which depends on inputs only. It still tallies `current_hits` through the standard path, and that count is never read. | `rc_condset.c:557-564`; `rc_trigger.c:112` (hit-count read needs `required_hits`) |
| Measured with target | `measured_value` = `current_hits + add_hits`, which grows until the target, so it changes state. Zeroed on reset (`:739-756`; `rc_trigger.c:254-272`). | `rc_condset.c:565-581` |
| MeasuredIf | Clears `can_measure`; measured becomes 0 if false. Depends on inputs only. | `rc_condset.c:584-590`, `:749-757` |
| Measured across groups | The largest value across alt groups wins. Kept unchanged if paused or nothing was captured. Written every non-paused frame (same value means no byte change). | `rc_trigger.c:218-248` |
| Percent display | `measured_as_percent` is set only at parse. It only affects rc_client/runtime progress popups. | `rc_trigger.c:46`; `src/client/rc_client.c:6077-6083`; `src/runtime/rc_runtime.c:564-572` |
| Remember / Recall | **Not an accumulator, and not in `rc_eval_state_t`** (`src/runtime/rc_internal.h:211-237` has no recall field). Remember is a no-op when evaluated (`rc_condset.c:648`). `{recall}` is resolved **at parse** to a copy of the remembered operand, i.e. a memref pointer, often a modified memref (`src/runtime/rc_operand.c:50-61`; `rc_condset.c:146-203`), and read as a memref (`rc_operand.c:594-610`). Recall *is* a memory input: the dependency set must include it (type RECALL, memref access type, `value.memref!=NULL`). | as cited |
| AddSource / SubSource / AddAddress | No-ops when evaluated (`rc_condset.c:645-650`). Folded at parse into modified memrefs (`rc_operand.c:547-573`, `:120-126`; `src/runtime/rc_condition.c:~180-187`). Recomputed every frame in the memref pass, parents first, using `rc_update_memref_value`, so they have their own value/prior/changed. | `src/runtime/rc_memref.c:761-812`, `:832-843`; `rc_client.c:6026-6040` |
| Trigger state machine | Pre-state is read at `:163-177` (TRIGGERED/DISABLED return INACTIVE with **no writes**; INACTIVE only updates own memrefs), `:281` (PRIMED→ACTIVE on reset with hits) and `:293` (WAITING). More detail in the four rows below. | `rc_trigger.c:152-323` |
| — WAITING + true | `rc_reset_trigger` (hits 0, state WAITING, `measured_value` UNKNOWN, `has_hits` 0), returns WAITING. Writes happen but the result is a byte-level fixed point, so S0 is clean only with **snapshot** comparison, not compare-at-each-store. | `rc_trigger.c:293-296`, `:332-344` |
| — WAITING + false | Becomes ACTIVE/PAUSED/PRIMED: a state write. | `rc_trigger.c:304-315` |
| — PRIMED | Needs all non-Trigger conditions true, so plain ones tally. Not S0-clean in practice; S1-clean. | `rc_condset.c:505`; `rc_trigger.c:310-311` |
| — PAUSED | Needs the core paused, or all alts paused. | `rc_trigger.c:215`, `:235` |
| `has_hits` | Read at `:277`, written every evaluation. **Not serialized**: after deserialize it keeps its pre-load value (see E6). | `rc_trigger.c:277-305`, `:343` |
| Trigger flag (challenge indicator) | A normal condition that excludes `is_primed`. Without a hit target it tallies (S1-eligible). Effects are visible only through the state (PRIMED). | `rc_condset.c:551-555`; `rc_client.c:6098-6105` |
| Delta / Prior | DELTA = changed ? prior : value; PRIOR = prior (`rc_memref.c:736-754`). Optimized comparators read value/prior/changed directly (`rc_condition.c:563-719`), same meaning. `changed` is set by `rc_update_memref_value` on every update in the pass (`rc_memref.c:717-726`). | as cited |
| Invalidated memrefs | Type NONE, skipped by the pass (`rc_memref.c:825`; `rc_client.c:6003-6004`). Their **`changed` freezes** at its last value. Only triggers that read the memref directly are disabled (`rc_runtime.c:703-716`; `rc_client.c:1293-1313`). Readers through a modified memref keep running on a stale parent. Constant, so exact either way. | as cited |
| BitCount / float / BCD / inverted / masks | Pure transforms (`rc_memref.c:469-578`; `rc_operand.c:459-545`). Floats are compared bitwise in `rc_update_memref_value` (`rc_memref.c:718`), so ±0 and NaN bit patterns are conservative. A PRIOR with a different mask gets its own memref (`rc_operand.c:110-118`). | as cited |
| Time / frame operands | None. FUNC operands are constant 0 (`rc_operand.c:588-592`). Operand types: `include/rc_runtime_types.h:93-103`. The only frame counters are hit tallies. | as cited |
| Short-circuit | `can_short_curcuit` is never set for triggers (memset at `rc_trigger.c:186`). Every condition is evaluated except after a true PauseIf (stops that group) or a true ResetIf (hit-target and other groups skipped). | `rc_condset.c:706`, `:720`, `:727`, `:762-766` |
| Own-memref triggers | `has_memrefs==1` (standalone `rc_parse_trigger`) updates its own memrefs *inside* `rc_evaluate_trigger`, so skipping would also skip the memref update. rc_client triggers use shared memrefs (`has_memrefs=0`: `rc_trigger.c:49`; `rc_client.c:2122-2125`). | `rc_trigger.c:136-141`, `:175`, `:183` |
| Leaderboards | `rc_test_trigger` **forces state=ACTIVE** before each evaluation (`rc_trigger.c:327`). Start/cancel/submit are tested every frame (`src/runtime/rc_lboard.c:184-187`). Lboard state machine: `:189-238`. The value is evaluated only while STARTED/TRIGGERED (`:241-256`). Value hit-counts: a Measured comparison in a value gets `required_hits=0xFFFFFFFF` (`rc_condset.c:310-315`), so leaderboard "frame timer" values change every frame and can't be skipped by S0 or S1. Value memref state: `src/runtime/rc_value.c:380-385`. rc_client tracker: `rc_client.c:6250-6305`, `:5346-5354`. | as cited |
| Rich presence | Values are evaluated every frame (`src/runtime/rc_richpresence.c:740`; `rc_value.c:471-480`, which can tally). Display triggers *with* hits: every frame through `rc_test_trigger` (`:742-745`). Display triggers *without* hits: only when the display string is built (`rc_richpresence.c:~901-903`): at ping every 120 s, or when the host asks (`rc_client.c:5797-5845`). | as cited |
| rc_client per-achievement side effects | Only when `measured_value` changed (`rc_client.c:6069-6092`) or `new_state!=old_state` (`:6095-6110`). RESET is folded into state (`:6065-6066`). `progress_tracker.progress` is zeroed every frame (`:6462`). Unchanged state, measured value and return value produce no effect. | as cited |
| rc_client per-frame, independent of evaluation | `frames_processed++` (`:6485`, read by ping at `:5807`), pause-penalty decay (`:6489-6503`), `rc_client_idle` callbacks (`:6509-6540`). No per-frame hardcore check (leaderboards are gated at `:6470`). Unlocked/disabled achievements are skipped by public state (`:6056`). | as cited |
| rc_runtime path | Adds an ACHIEVEMENT_RESET event (`rc_runtime.c:547-553`), a measured-progress event (`:556-579`) and state-transition events (`:582-622`). Invalid memrefs are checked before evaluation (`:526-537`). | as cited |

### (B) Mutable state a trigger evaluation can write (all must be in the "clean" check)

**Inside `rc_evaluate_trigger`** (`rc_test_condset` is called at `rc_trigger.c:193`, `:214`):
- `trigger->state`: `rc_trigger.c:282`, `:300`, `:308`, `:311`, `:314`; `:338` via `rc_reset_trigger` at `:294`.
- `trigger->measured_value`: `:247`, `:255`, `:262`, `:267`; `:341` via `:294`.
- `trigger->has_hits`: `:278`, `:295`, `:305`; `:343`.
- `condset->is_paused`: `rc_condset.c:708` (only if the group has pause conditions).
- `condition->current_hits`: `rc_condset.c:430`, `:444`, `:448`, `:525`, `:742`; `:776` via `rc_trigger.c:274` / `:336`.
- `condition->is_true`: `rc_condset.c:424`, `:538`. Not rewritten for conditions that weren't evaluated; the stale value is serialized.
- Return value (output, not state): `rc_trigger.c:167`, `:171`, `:176`, `:284`, `:296`, `:301`, `:319`, `:322`.
- Never written at runtime: `measured_target`, `measured_as_percent`, `has_memrefs`, `required_hits`, operands, `optimized_comparator`, group counts.
- Per-evaluation only: `rc_eval_state_t` (memset at `rc_trigger.c:186`; per-group fields reset at `rc_condset.c:688-698`; `has_hits` / `was_reset` / `was_cond_reset` carry across groups within one evaluation).

**Snapshot per trigger:** `state`, `has_hits`, `measured_value`; per group `is_paused`; per condition `current_hits`, `is_true`. Snapshot comparison is exact. Compare-at-each-store is conservative because of the WAITING-true write-then-reset.

**Writers outside evaluation (each must clear "clean" and mark the trigger dirty for 2 frames):**
- `rc_reset_trigger` (`rc_trigger.c:332`). Callers: `rc_runtime.c:143`, `:165`, `:221`, `:691`; `rc_client.c:1463` (hardcore toggle), `:4905` (reset); `rc_lboard.c:279-281`; `rc_richpresence.c:922`; `rc_runtime_progress.c:1099`.
- `rc_test_trigger`: `rc_trigger.c:327`.
- Direct state writes: `rc_runtime.c:532`, `:762`, `:767`, `:772`, `:831`; `rc_client.c:1309`, `:1498` (TRIGGERED on unlock or hardcore toggle).
- Deserialize: `src/runtime/rc_runtime_progress.c:402`, `:406`, `:409`, `:644`, `:645`, `:1015` (UNUPDATED). Memref writes at `:251`, `:284-285`, `:309-310`, `:416-417`, `:425-426`.
- Parse: `rc_trigger.c:44-49`; `rc_condition.c:203-205`.

**Memref state** (written by the memref pass, never by evaluation): `rc_memref.c:719-724`; type set to NONE at `rc_client.c:5943`, `rc_runtime.c:791`, `:810`.

**Leaderboard / RP extras:**
- `lboard->state`: `rc_lboard.c:201`, `:209`, `:216`, `:231`, `:235`.
- Value memref: `rc_value.c:384`, `:477-478`, reset at `:402-403`.
- Value group hits: through `rc_test_condset` (`rc_value.c:343`) and `rc_reset_condset` (`:351`).
- rc_client: `leaderboard->value` (out parameter, `rc_client.c:6272`) and `tracker->raw_value` (`:5350`).

### (C) Verdicts and corrections

**S0: exact**, with these corrections:

- **C1. Dependency set.** Use operand1/operand2 memrefs of every *evaluated* condition type. Include RECALL operands with memref access (`rc_operand.c:594-610`); don't reuse `rc_condset_contains_memref`, which skips Recall (`rc_runtime.c:709-711`). AddSource/AddAddress/Remember operands and pointer parents are *not* needed, because the consuming modified memref's own value/prior/changed captures them. Including them is safe but costs skips. Optional relaxation: `changed==0` at f-1 is only needed for memrefs read as DELTA (`rc_memref.c:744-749`; delta comparators `rc_condition.c:570-576`, `:586-624`, `:637-719`).
- **C2. Return clause.** The "returned `T->state`" clause is required, not redundant. TRIGGERED and DISABLED write nothing but return INACTIVE (`rc_trigger.c:165-171`). Skip only when state ∈ {WAITING, ACTIVE, PAUSED, PRIMED} and the last full evaluation returned exactly `T->state`; then return that cached value.
- **C3. Write detection.** Use a byte snapshot of the (B) set. Per-site compare-before-store misses the WAITING-true fixed point (still exact, just fewer skips).
- **C4. Own-memref triggers.** Exclude `has_memrefs==1` triggers, or keep their memref update running.
- **C5. Induction.** Over the last full evaluation e: every dependency must have `changed==0` on every frame in [e, f]. The draft's "dirty for f and f+1" implements this. All writers in (B) must reset clean/dirty, including deserialize (it writes memref `changed`).

Proof check: evaluation reads only trigger bytes plus operand memrefs. Nothing in condset/condition/operand reads memory (`read_memory` is used only for own memrefs at `rc_trigger.c:175`/`:183`), there are no time operands, and the eval state is memset each time. So F(s, x) is deterministic and the fixed-point argument holds.

**S1: exact** with plain tally defined as follows:
- **Static part:** `required_hits==0` and type ∉ {ADD_HITS, SUB_HITS}. Eligible types: STANDARD, TRIGGER, PAUSE_IF, MEASURED_IF, MEASURED (no target), AND_NEXT, OR_NEXT, RESET_NEXT_IF.
- **Dynamic part:** at the last full evaluation, its only write was `++current_hits` at `rc_condset.c:444`, with pre value < 0xFFFFFFFF. Under a snapshot diff, a ResetNextIf zeroing a count of 0xFFFFFFFF looks like +1 mod 2³², so it must be excluded.

Why the count is never read anywhere that changes a result:
- `:428` runs only on the reset_next path, which zeroes rather than increments.
- `:446-449`, `:470-475` and `rc_trigger.c:112-113` need `required_hits!=0`.
- `:455-458` runs only when the condition is false, which can't happen with identical inputs and identical non-tally state. Its `and_next`/`or_next` output is the same whatever the count.
- `:595`/`:604` are AddHits/SubHits (excluded).
- `has_hits` is set at `:440` regardless of the count.
- Serialization (`rc_runtime_progress.c:372`) is fixed by catch-up.
- rc_client never reads `current_hits` (grep).

Catch-up rules:
- Increment *eagerly* on each skipped frame, because serialize can run between frames.
- Use the same `uint32 ++` as stock. Wraparound matches `:444`, and the count isn't read afterwards.
- ResetIf can never be in the plain-tally set: true resets everything at `rc_trigger.c:274`.
- The draft's wording "not read by any AddHits/SubHits total" is wrong. It is the AddHits/SubHits condition's *own* count that is read, and it leaks past its chain. Exclude those types outright.

### (D) Draft open questions
- **Q1:** Answered by (B).
- **Q2: Confirmed.** rc_client acts only on measured change or state change (`rc_client.c:6069-6110`) and ignores RESET (`:6065`). Progress tracker, challenge indicators and pending events depend on those alone. `frames_processed`, the pause penalty and idle/ping don't depend on evaluation. In the rc_runtime path, a RESET return with unchanged state does raise an event, but it always implies a write, so S0 excludes it.
- **Q4: Derive it from the per-trigger 2-frame dirty bits.** `rc_memref_value_t` is a public 12-byte struct with no spare byte (`include/rc_runtime_types.h:63-78`), so adding one breaks ABI. Find changed memrefs by scanning the lists after the pass (`rc_client.c:5992-6044`). After deserialize, or any dirty-bit loss, mark everything dirty.
- **Q3 (not asked, but relevant):**
  - Keep leaderboards and RP always evaluated in v1.
  - Leaderboard triggers have their state forced to ACTIVE each frame, so a pre/post snapshot is never clean for PRIMED/PAUSED ones. They would need a post(f-1)==post(f-2) rule.
  - STARTED leaderboards with hit-count (frame-timer) values change every frame.
  - On a skip, a leaderboard's `*value` output would have to be reproduced.

### (E) Surprises
1. Recall is resolved at parse to a memref pointer, not stored per evaluation. The draft and ra-compliance both describe the mechanism wrongly; the conclusion still holds.
2. Triggers never short-circuit, so every condition runs each frame.
3. AddHits counts leak across chains to later hit-target or Measured conditions in the same group (`rc_condset.c:490`, `:482`).
4. Invalidation disables only triggers that read the memref directly, and an invalid memref's `changed` freezes.
5. A true PauseIf with no hit target adds a hit every paused frame.
6. `has_hits` is neither serialized nor reset by a successful deserialize. Post-load RESET behaviour depends on the pre-load `has_hits`. The diff harness should compare `has_hits` explicitly, since `serialize_progress` can't see it.
7. ResetNextIf's reset carries through AndNext/OrNext chains.
8. A WAITING trigger that is true is a write-then-restore fixed point.
9. RP display triggers without hits are evaluated only when the display string is built (at ping or host request).
