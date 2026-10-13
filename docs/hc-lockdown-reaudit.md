# Re-audit checklist: hardcore lockdown (branch `perf-hc-lockdown`)

How ra-compliance verifies the 9 items of `docs/compliance.md` "Audit 2026-10-12" when `perf-hc-lockdown` lands. Pass marks are fixed here, before the run. The branch already has reproducers: `tests/hardcore/lockdown.sh` (one case per SD file), which writes the marker `DSCX` over ARM9 bytes 4-7 (the secure-area ID at `0x02000004`, never executed) and reads it back from the RAM dump.

## Set-up

- Worktree on the merged `perf`, submodules updated, nds-bootstrap, module and launcher built there, each docker build through `~/Developer/DScheevos/bin/build-lock` (2 machine-wide slots; `make -j2` at most). The **old build** is nds-bootstrap from `perf` before the merge (to show each reproducer catches the hole).
- `ROM` = Kirby Super Star Ultra (USA) (built-in apFix in cheat form); `ROM_USERONLY` = New Super Mario Bros. (USA, Australia) (no built-in fix); `ROM_EXCLUDED` = Sonic Rush (USA) (on `twlClockExcludeList`); `SRL` = `tests/module/out/module-test.nds`.
- One emulator slot, cases one after another: `emu-lock -l ra-reaudit tests/hardcore/lockdown.sh <cases>`, with `MODE=hc`, then `MODE=casual`, then `MODE=hc` with the old `BOOTSTRAP`.

## Per item

| # | Item | Case(s) | Pass, new build, hardcore | New build, casual | Old build, hardcore (proves the test) |
|---|---|---|---|---|---|
| 1a | user apFix `.ips` | `ips` | `not applied`, session used | `applied` (casual allows patches) | `applied` |
| 1b | user apFix `.bin` (cheat form) | `bin` | `not applied` | `applied` | `applied` |
| 1c | launcher, game with only a user fix | `launcher-ips` | the launcher doesn't offer hardcore: server session `hardcore: false`, `launcher.log` names the apFix file; the marker may be applied (it's casual) | as hardcore column | hardcore session, marker applied |
| 1d | launcher, game with a built-in fix plus a user file | `launcher-builtin` | server session `hardcore: true`, marker `not applied` (built-in pack only) | marker applied | marker applied in hardcore |
| 2 | `wideCheatData.bin` | `widecheat` | `not applied` | `applied` | `applied` |
| 3 | `softResetParams.bin` SRL hand-over | `srl` | booted header TID `YKWE` (the hashed ROM) | either (record it) | the SRL's TID |
| 4 | `patchOffsetCache` | `cache` | "card read patch at the marker: no" | either (record it) | "yes, game code overwritten" |
| 5 | donor ARM7 | `launcher-donor` | hardcore not offered (`hardcore: false`), `launcher.log` names the donor | casual runs | hardcore session |
| 6a | session replay: old session | `stale` | `session NOT used` | `session NOT used` | `session used` |
| 6b | session replay: re-use | `wipe` | `session.used` doesn't start with the session magic (zeroed) | same | starts with the magic |
| 7 | in-game menu speed options | manual: a DScheevos session, open the menu (L+DOWN+SELECT), Options, screenshot | no Clock speed or VRAM mode line | same (ruling B: locked for every DScheevos session) | both lines present |
| 8a | `BOOST_CPU`/`BOOST_VRAM` chosen before launch | `boost` | module status and journal record the speed-ups (CPU 133 MHz, VRAM on: `0x3`) | same | (no record) |
| 8b | excluded game | `boost-excluded` | module and journal: 67 MHz for Sonic Rush (VRAM boost still recorded) | same | (no record) |
| 8c | `DSI_MODE = 2` on a DS game | `dsimode2` | runs in DS mode with the boost recorded as a speed-up, and the exclude list applies | same | achievements off (old behaviour) |
| 9 | "Casual" wording | manual: `grep -rn Softcore` in `dscheevos/launcher/source`, nds-bootstrap `arm9_igm` and `bootloaderi`, plus screenshots of the placard and the menu summary | no user-visible "Softcore" | - | - |

## Code read (no emulator)

- Every hardcore gate reads the same flag: the session's hardcore byte, read once in `arm9/source/dscheevos_session.c`. Item 6 is what makes that flag trustworthy.
- `loadApFix` in hardcore never opens an SD path and falls through to `nitro:/apfix.pck`. `apPatchSize` is 0 when no built-in entry exists.
- `wideCheatSize` is zeroed next to `cheatSize`.
- `dscheevosSanitizeSoftReset` runs before `softResetParams.bin` is read (the `LOAD` magic and `srlAddr` are both cleared), and only in hardcore.
- In hardcore the patch offset cache is neither read nor written.
- The freshness check and the wipe happen before the module loads, and a refused session leaves the game booting stock (no module, no achievements).

## Residual to raise if still open

- **Clock rollback.** The 10-minute freshness check uses the console clock, which the player sets. Someone who copied a fresh `session.bin`, swapped the ROM and wound the clock back could still replay it. Closing it for good: nds-bootstrap (or the module at init) computes the rcheevos DS hash of the ROM it is booting (header, ARM9, ARM7 and icon, about 1 MB of MD5 before boot) and refuses a session whose hash differs. Recommend to orchestrator if the branch doesn't do it.

## Regression (after the cases)

`make -C dscheevos/core test`, `make -C dscheevos/module/tests test`, the launcher host tests (`dscheevos/launcher/tests`), the TWiLight chain for Kirby and Pokémon (`docs/handoff.md`), and the offline-queue pair (`--fail-awards`, then normal). With the time-base fix in the branch, check that the second run's award times (`o=`) match the host clock within a minute when `--rtc` sets the console to local time.

## Merge evidence (COMMON.md "Game-cost evidence")

The branch changes nds-bootstrap and module runtime code, so its merge request must report the game's own ARM9 cycles per frame, achievements off vs on, besides lag intervals. That means melonDS with the cache model and `--thumb-cycle`, on Kirby Super Star Ultra, Mario Kart DS and one busy game from idle.md, using the gamecost lane's tooling. Any increase over the current baseline (Kirby +21% today) needs a justification. Expect no change: the fixes act at boot (file checks, a bootloader signature search instead of the cache) and in the in-game menu, not per frame. Pass: the numbers are in the merge request and within noise of the baseline. Missing numbers or an unexplained increase: report to orchestrator, re-audit on hold.

Note (2026-10-13): nds-bootstrap's front end prints `boostCpu`/`dsiMode` to its console, not to `NDSBTSRP.LOG`, so the module's status and the journal are the record for item 8.

## Recording

Results go into `docs/compliance.md` ("Audit 2026-10-12": the Today column becomes "verified <date>") with the `lockdown.sh` output lines. A fail goes to orchestrator with the case's output.
