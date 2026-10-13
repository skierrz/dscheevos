# Sets the DSi can't evaluate every frame

Status: options for the owner's decision, 2026-10-12. Docs only. Input: eval-speed's study. With results required to be identical to stock rcheevos, The World Ends with You and Pokémon HeartGold/SoulSilver can't be evaluated every frame on a DSi even with both CPUs: their conditions' memory traffic alone exceeds every free cycle (TWEWY about 3.5x, HG/SS about 1.3x). Digimon World DS is a stretch goal. The question: what may DScheevos do with such a set, on the strict reading of RA's rules (`docs/compliance.md`, "Strict reading")?

## The rules that decide it

- **RA, section A:** triggers, Measured and Trigger flags "must work properly"; rich presence and leaderboards must function. This applies in every mode, not only hardcore.
- **RA, section B:** hardcore can drop to casual mid-session, never the reverse. Casual tolerates save states and cheats, which also break frame-by-frame continuity, but those are the player's choices. A client that evaluates wrongly on its own is a different matter.
- **Ours** (CLAUDE.md): zero gameplay cost, so we can't slow the game; no SD writes during play; rcheevos results identical to stock; never reimplement rcheevos.

## What RetroArch does when it can't keep up

RetroArch evaluates once per emulated frame. Its main loop calls `rc_client_do_frame` right after each `core_run()` (`runloop.c`, then `rcheevos_test()` in `cheevos/cheevos.c`, RetroArch `master`, checked 2026-10-12). If the host is too slow, the whole loop is slower: the game runs below full speed, and no emulated frame goes unevaluated. Slowing the game is exactly what DScheevos can't do on a DSi, where the game runs natively at its own speed. So we have no RetroArch precedent for "keep the speed, skip evaluation".

## Options

| Option | Hardcore | Casual | Notes |
|---|---|---|---|
| **(a) Refuse the set: no achievements for that game**, with a clear launcher message and "Play without achievements" | compliant | compliant | Nothing wrong is ever sent. The launcher's CPU gate decides from measured cost, generic for every set (no per-game list): the owner's "no per-game hacks" holds. |
| **(b) Casual only, evaluating the frames we can afford** | n/a | **not compliant** | Skipped frames make delta, prior and hit-count conditions compare frames that aren't neighbours. The gap rule (`docs/dropped-frames.md`: reset at each gap) would fire almost every frame, so multi-frame achievements could never unlock, and without the reset, false unlocks are possible. Either way the triggers don't "work properly". Casual's tolerance of save states doesn't extend to a client that evaluates wrongly by design. |
| **(c) Hardcore refused, casual allowed with a warning** | compliant (refused) | **not compliant** if casual then runs as (b) | A warning doesn't make wrong evaluation right. Compliant only where casual would still evaluate every frame, which for these sets it can't. |
| **(d) Evaluate a declared subset every frame, and disable the rest** | **not compliant** | **not compliant** | rc_client has no API for it. Stock rc_client disables an achievement only when the set itself is at fault: a parse error, or a memory read outside the console's map (`rc_client.c`, bucket `UNSUPPORTED`). Disabling by our own cost choice changes results (those achievements can never unlock), so the new rcheevos rule rules out a patch for it (results must be identical). Faking it, for example by making reads fail so rc_client disables them, would be dishonest. Mastery would become impossible on our client without RA knowing why. |
| **(e) Ask RA for a sanctioned exception** (e.g. a "DSi subset") | possible later | possible later | The RA questions are on hold. Question 8 in `docs/ra-questions.md` already asks about refusing heavy sets; add "or a sanctioned subset" there. RA's actual mechanism for this kind of thing is a separate set: subsets are their own game entries with their own hashes. That's for set authors, not a client. |
| **(f) Spectator ("watch only"): evaluate what we can, show it, send nothing** | not a hardcore session | sends nothing | Nothing reaches RA, so RA's rules aren't engaged. But the player would see wrong progress and phantom unlocks. Not recommended as a product mode. |
| **(g) Evaluate every frame later** (store the snapshots, evaluate after play or in loading screens) | compliant in principle | compliant in principle | Results identical and every frame evaluated, but **infeasible**: the RAM queue holds about 17 s; the SD card is off limits during play; HG/SS-sized snapshots would be hundreds of MB per hour. And unlocks would arrive long after play. |
| **(h) Make it cheaper with identical results** (eval-speed's rcheevos patches, both CPUs, the partition patch) | compliant | compliant | The only route that brings these games in. Their current conclusion is that it isn't enough for TWEWY and HG/SS. Re-run the gate whenever their numbers change. |
| **(i) Rely on 133 MHz** | n/a | n/a | Against the owner's ruling (development target 67 MHz). It also doesn't help a memory-bound set, because the memory bus doesn't speed up. |
| **(j) Evaluate off the console** (stream snapshots over Wi-Fi to a PC or phone running stock rc_client) | open | open | Results would be identical. But it sends Wi-Fi traffic every frame, so it has to fit the zero-cost rule (ARM7 shim time each frame, and the game's own wireless) and the "client" identity (C) moves to the companion. A research item at most. |

## Recommendation

1. **(a) for both modes now**: the launcher's CPU gate refuses achievements for any set it measures as unable to be evaluated every frame on this console. The message: "This game's achievements need more processing than a DSi has. Play without achievements, or play it on an emulator: your progress there counts on the same RetroAchievements account." Hardcore and casual alike. The game still runs normally.
2. **No casual fallback with dropped frames** ((b)/(c)): the strict reading applies "work properly" to casual too, and the owner's casual-first principle is about keeping casual sessions honest, not lowering their bar.
3. **No client-side subsets** ((d)): only RA can sanction one (e), once the questions are sent.
4. **Keep the gate data-driven**: the refusal comes from the measured cost (eval-speed's gate), not a list of names. When (h) gets a set under the bar (Digimon is the candidate), the gate lets it through with no further change.
5. **Show it before the player commits**: the launcher shows "Achievements: not available on DSi (too heavy)" before boot, never mid-game. A game the gate passed but that still falls behind mid-session is handled by `docs/dropped-frames.md` (hardcore to casual and reset), with the gate data updated from the session's gap count.

## Owner decision

Accept (a) as the policy for beyond-bound sets in both modes (recommended), or ask for (c) knowing it fails "work properly" in casual.
