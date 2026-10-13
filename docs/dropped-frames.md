# Frames that are never evaluated: recommended spec

Status: **recommended spec, owner ruling pending** (2026-10-12). Docs only; nothing here is built yet. Read with `docs/compliance.md` ("Strict reading").

## The rule we're meeting

RA's Hardcore Compliance Requirements, section A: triggers, Measured and Trigger flags "must work properly". An emulator evaluates every frame. If DScheevos skips one, the frames rcheevos compares are no longer neighbours, so a trigger can fire when it shouldn't (a false edge), or miss when it should fire. Section B allows a session to go from hardcore to casual mid-session, never the reverse. Casual allows save states and cheats, so in casual RA tolerates achievement state that doesn't follow the game frame by frame.

## When a frame goes unevaluated

| Case | Counter | A gap? |
|---|---|---|
| The queue is full (evaluation more than ~1024 frames behind), the arena has no room, or a capture failed with no earlier copy to repeat (`core/src/dscheevos.c`, the three `frames_dropped++` sites) | `frames_dropped` | **yes** |
| A VBlank passes with no IPC IRQ at all (the game held IRQs off for a whole frame) | `frames_missed` | **yes**, after the session's first evaluated frame. Before it (the game hasn't enabled IRQs yet), no |
| A lag frame (no halt since the last VBlank): evaluated with the last finished frame's memory | `frames_stale` | no: evaluated, as RA question 7 describes |
| A repeat (same watched bytes as the frame before) | `frames_repeated` | no: evaluated as its own frame |
| A pointer chain using Remember/Recall read live at evaluation | `live_reads` | no (a fidelity limit, tracked separately) |
| VBlanks while nds-bootstrap's in-game menu is open (the game is stopped) | to check | no: the game is paused, as in an emulator's pause menu. **To verify in the code:** that menu time adds to neither counter |

## Recommended behaviour

Applied at the first frame that is evaluated after a gap, before rc_client evaluates it:

**Both modes:**
1. `rc_client_reset(client)`, the same call DScheevos makes when the game itself is reset. rcheevos clears hit counts and the prior and delta values, and every trigger then has to read false for a frame before it can fire again. So nothing can unlock from comparing frames across the gap. The cost is lost in-progress hit counts: the client undercounts, it never overcounts.
2. The core emits `DSC_EVENT_DEGRADED` (it already does) and the module adds a notice. The in-game menu summary shows "Progress reset: the DSi fell behind (N frames not checked)", with the time and the running count.

**Hardcore, in addition (and first):**
3. `rc_client_set_hardcore_enabled(client, 0)` before the reset. Why the reset alone isn't enough in hardcore: it clears achievement state while the game keeps running. That includes "pause locks" (a PauseIf with a hit target that blocks an achievement for the rest of a level after a failure). A player who could provoke a gap, by staying in a scene that starves idle time for 17 s, could clear a failed no-death lock and still earn the achievement. In hardcore that would be equivalent to loading state, which RA bans. Going to casual is the direction RA allows.
4. The player is told on screen (the notice strip, as for any notice) and in the menu: "Hardcore off for this session: the DSi fell behind. Achievements continue in casual." The LED cue for a mode change is the display session's choice.
5. **The server** has no request for "this session left hardcore". After the switch, every request rc_client builds carries `h=0`: unlocks, leaderboard submissions (rc_client deactivates leaderboards when hardcore goes off) and rich-presence pings. This is what RetroArch sends when a player turns hardcore off mid-game, through the same rc_client call. Unlocks earned before the gap keep `h=1`: they came from consistent frames. The journal stores each entry's own hardcore flag, so the offline queue sends each with the right mode.

**Casual:** steps 1 and 2 only. That stays inside what casual allows (save states can do the same to achievement state).

**Spectator:** as casual, with nothing sent.

**Repeated gaps:** each gap does the same again. After 3 gaps in 10 minutes, the menu also says the game is too heavy for the DSi in this part. Each gap's count and time go to the status block for eval-speed's CPU gate (games that gap should be refused up front).

## What stops gaps happening

Gaps should be rare. This spec is the backstop, not the plan:
- the launcher's CPU gate refuses sets the DSi can't keep up with (eval-speed);
- repeats take no queue space, so cutscenes and loading screens don't fill the queue;
- the queue absorbs about 17 s without idle time.

## Tests to add (core, host)

- Inject a dropped frame while a delta trigger's value changes during the gap (`d0xH0001=0_0xH0001=1`, with the value going 0, then 1 in the dropped frame, then 1). It must not unlock after the gap, and it must unlock on a clean 0 to 1 later.
- Hardcore session plus one dropped frame: hardcore is off before the next evaluated frame, the next award request has `h=0`, an award from before the gap has `h=1`, and the journal entries match.
- A pause-locked achievement (PauseIf with a hit target) across a gap in hardcore: the session is casual before the lock clears.
- `frames_missed` before the first evaluated frame doesn't trigger any of this; one after it does.
- In-game menu open for 10 s: no gap.

## Owner decisions this needs

1. Hardcore: downgrade to casual on the first gap (recommended), versus refusing the whole session up front for any game that might gap (stricter for players, and the CPU gate already does most of that).
2. Casual: reset on a gap (recommended), versus stopping achievements for the session.
