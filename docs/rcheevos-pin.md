# rcheevos version pin

Which rcheevos DScheevos builds, and why. Checked 2026-10-12.

## Where we are

- Submodule `external/rcheevos`: upstream `develop` at `683197d` ("restructure src folder", 2026-10-04) plus one patch, on the fork `skierrz/rcheevos` branch `dscheevos-683197d`. `git describe` of the base: `v12.5.0-9-g683197d`.
- Patches (each one commit, proven identical to stock with `tests/difftest` on every real set):
  1. **clean stores** (`src/runtime/rc_condset.c` only): a condition's `is_true`, a ResetNextIf'd zero `current_hits` and a condset's `is_paused` are stored only when they change. Same values everywhere; an unchanged store no longer dirties a data-cache line (on the DSi each dirty line costs a write-back when evicted). ARM code: `ldrb; cmp; strbne`, +2 instructions per condition, no branch.
     - **Measured (2026-10-13, melonDS cache model, not hardware), against stock with identical code layout** (a padded stock build, because the patch shifts the code after it by 0x18 bytes):
       - Kirby: evaluation -0.8k and the game's own cycles -1.4k per frame, net -2.2k (-0.19% of a frame); dirty write-backs we cause 196 -> 156.
       - MKDS race: evaluation -2.6% per evaluated frame; game within noise; write-backs 97 -> 73.
     - **Correction:** the first figures (Kirby game +63.5k -> +54.3k, write-backs 300 -> 155, "-15%", also in commit 23f736d) compared against unpadded stock, and the layout shift alone accounts for about two-thirds of those write-backs and over half of that game gain.
     - **Desktop** (Apple M5, clang -O2, real sets): neutral to slower, up to +6.5% evaluation time on TWEWY; there the guarded stores compile to real branches.
     - **Status:** DSi-only, kept in our fork. Upstream PR [RetroAchievements/rcheevos#562](https://github.com/RetroAchievements/rcheevos/pull/562) is being closed for these reasons.
- `RCHEEVOS_VERSION_STRING` still says 12.5.0, so the user agent says `rcheevos/12.5.0` while we run 9 commits past it.
- Latest release tag: **v12.5.0**. Upstream `develop` is 3 commits past us: `aed817f` (runtime types made internal), `23bcf47` (project files), `116d018` (a test script).

## Lab patch: yield points (branch `dscheevos-yield`, not on perf; owner ruling B, 2026-10-14)

One commit on top of the pin (754965e): **806fdc0 "Yield points: optional RC_CLIENT_YIELD_HOOK called between units of a
frame's work"**, 3 files (`src/runtime/rc_internal.h`, `src/client/rc_client.c`, `src/runtime/rc_memref.c`), +29/-2.
- **What:** a build may define `RC_CLIENT_YIELD_HOOK` as the name of a `void f(void)`. rcheevos then calls it before each
  active achievement and leaderboard, every 16 memrefs, every 32 modified memrefs, before the rich presence, and every 64
  entries of rc_client's change-driven index build and comparison tests. The hook must not call into rcheevos or change
  what it reads; it may suspend the caller (a coroutine switch) and resume it later.
- **Why:** NitroSDK games that wait for VBlank in the BIOS (20% of the library) give DScheevos time only inside that wait,
  on the game's main thread, and the work must hand the wait back before the VBlank or the game loses a frame. With yield
  points a frame's evaluation spreads over several waits (research/optimize/biowait.md in the planning notes).
- **Identical by construction when off:** undefined, every yield point is `((void)0)`. Same object code at Thumb -Os; at
  ARM -O2 (how the module builds rc_client.c) two loop compares come out with their operands swapped (`cmp r4,r5; bls`
  vs `cmp r5,r4; bcs`), same size and meaning; the module's default build keeps its hot layout (hotcheck).
- **Identical when on (difftest):** `tests/difftest/core.sh` with `YIELD=1` adds a variant built with the hook, in which
  one yield point in 8 (at most once per evaluation turn, as the console's gate captures once per wait) runs the next
  trace frame's capture and VBlank from inside the hook, in the middle of `rc_client_do_frame` or the index build. Result
  on every real set with a real RAM dump: see biowait.md (corpus run 2026-10-14).
- **Upstream:** DSi-specific; to be raised as an issue/discussion (draft to the owner), not a PR.
- Tried and dropped: an `rc_client_build_change_index()` to build the index at load. Its second caller stops GCC inlining
  `rc_client_cd_build` into `rc_client_do_frame` (moves the hot layout), and the yield points in the build already
  spread the first frame's index build over several waits.

## The 9 commits between v12.5.0 and our pin

| Commit | What | Evaluation semantics | rc_client behaviour | Hashing | API / request format | DS memory map | We rely on it |
|---|---|---|---|---|---|---|---|
| `e904fc4` libretro: partial region chunks | `rc_libretro.c` only | - | - | - | - | - | no (not built) |
| `ea43799` RetroArch compat for Wii/Vita/Switch | `rc_compat.c/.h` additions | - | - | - | - | - | no |
| `ca7746b` rename core/unofficial to promoted/unpromoted | names only (`RC_CLIENT_ACHIEVEMENT_CATEGORY_PROMOTED`, `unpromoted_enabled`) | - | names only | - | none on the wire | - | **yes**: core and launcher use the new names (`core/src/dscheevos.c`, `launcher/source/session.c`) |
| `e3f7b7d` `rc_peek_t` replaced by `rc_read_memory_func_t` | the memory read path: `rc_read_memory` reads `n` bytes in one callback call; rc_client's peek helpers removed | **yes, small.** A direct read that comes back short still disables its achievements and leaderboards (now in `rc_client_read_memory_validator`). A pointer-chain step that comes back short now keeps the bytes it got (zero-filled) instead of reading 0. Our read callback fails a read whole (0 bytes), so for us a chain step across a region end still reads 0, as `docs/architecture.md` says | `legacy_peek` setting removed | - | - | - | **yes**: the core's chain code calls `rc_read_memory` / `rc_get_modified_memref_value` with a read function |
| `f06b14a` unpromoted leaderboards | the `achievementsets` request adds `v=2`; the reply's leaderboard `State` is parsed; unpromoted leaderboards get their own category | unpromoted leaderboards can be loaded | new leaderboard category | - | **yes: `v=2` on `achievementsets`** | - | yes, implicitly: our requests are rcheevos', so they carry `v=2` |
| `f87c0de` rename softcore to casual | field names (`score_casual`, `unlock_time_casual`); JSON keys unchanged (`SoftcoreScore`) | - | names only | - | none on the wire | - | **yes**: core and launcher use the new names |
| `e9a6d58` bound progress deserialization | `rc_runtime_progress.c` checks chunk bounds | safety fix for restoring progress (save states) | - | - | - | - | no (we don't restore progress) |
| `c0a3ebd` stricter JSON arrays | a reply with an array of primitives where objects are expected is now `RC_INVALID_VALUE`; empty-body check uses the length | - | malformed replies rejected | - | parse only | - | no |
| `683197d` restructure `src/` | `rapi/rhash/rcheevos` folders become `api/hash/runtime/util/client`; file renames; md5 moves to `util/` | - | - | **none**: `rc_hash_nintendo_ds` unchanged apart from include paths and a cast | - | none (`rc_consoleinfo.c` loses an unused include) | **yes**: our Makefiles, the launcher's wrapper files and `tools/rahash` use the new paths; the core includes `runtime/rc_internal.h` |

## What pinning to v12.5.0 would take

Our code uses four of these commits (`ca7746b`, `e3f7b7d`, `f87c0de`, `683197d`). Going back to the tag means porting the core's chain code to `rc_peek_t`, renaming back, changing every source path in nine Makefiles and 24 launcher wrapper files, and losing `v=2`. All of that is churn in our glue to land on an older rcheevos, and then forward again at the next release.

## What the next release will bring

`aed817f` (on `develop`, so in the next release) removes `include/rc_runtime_types.h` and splits the runtime internals into new private headers (`rc_condition.h`, `rc_condset.h`, `rc_alloc.h`, `rc_eval_state.h`, ...). For us:
- The core reads rcheevos internals (`rc_modified_memref_t`, operands, memrefs) through `runtime/rc_internal.h`; those includes will need updating.
- The float-operand build (`docs/ra-questions.md` Q11, `fltop.h`) forces a typedef change in `rc_runtime_types.h`, which no longer exists there. It would have to target the new header.
- The struct sizes and the core's chain copies must be re-checked against the new layout (the core's `DSC_CHECK_FAST_STEPS` tests and the 32-set digest).

## Recommendation

1. **Stay on `683197d` now.** It is upstream code with no local patches, and every change since v12.5.0 is either something we use or harmless for a DS client. Don't move to newer `develop` until the next release: `aed817f` needs a planned port of the core.
2. **Before the first public release, move to the first release tag that contains `683197d`** (expected v12.6.0). Port the core to the new private headers, rerun the core tests, the module tests, `tests/games`, the perf gate and the 32-set digest, then pin that tag. The user agent then names the exact rcheevos shipped.
3. **If no such tag exists by release day**, ship `683197d`, and say so where RA and users can see it: `SOURCES.txt` already names exact commits; add "rcheevos 12.5.0 + 9 commits (683197d)" to the release notes. We can't change rcheevos' version string (rcheevos stays unmodified), and the user agent's rcheevos part comes from `rc_client_get_user_agent_clause`.
4. Never pin a `develop` commit that isn't in a tag for a release that RA validates, if a tag is available: it's the cleanest answer to "which rcheevos is this?"

## Carrying local patches (rule changed 2026-10-12)

The owner's rule now allows rcheevos changes only where evaluation results are proven identical to stock rcheevos (differential tests on every real set with real RAM dumps), kept minimal and offered upstream. Never a reimplementation. How to carry such a patch:

- **Where:** a fork, `skierrz/rcheevos`, on a branch named after its base, e.g. `dscheevos-v12.6.0` = the upstream tag plus our patch commits on top. The submodule points at the fork's commit. Each patch is one small commit, and each is opened as a pull request upstream with the same diff, linked from its commit message. (Patch files applied at build time were the alternative. They keep the submodule pristine, but they need applying in nine Makefiles and the launcher's 24 wrapper files. The fork branch is simpler and just as visible, since `git diff <tag>..` shows every byte we changed.)
- **What CI checks:**
  - `git diff <base>..<pin> --stat` lists only the files the patch list allows;
  - rcheevos' own test suite passes;
  - the differential suite passes ("Evaluating one set on two CPUs" in `docs/compliance.md`): patched rcheevos with each patch's feature off is identical to stock, and with it on, identical on every real set;
  - the float-operand digest, if that build flag is in use.
- **Moving the base:** rebase the patch commits onto the new tag, rerun everything above, and drop any patch upstream has taken.
- **Version string:** unchanged. `RCHEEVOS_VERSION_STRING` and the user agent's rcheevos part stay upstream's. Our build is identified by `DScheevos/<version>`, and `SOURCES.txt` and the release notes name the base tag, the fork commit and each patch with its upstream pull request.
- **Builds and the launcher wrappers:** the comment `Built from the rcheevos submodule, unmodified.` in `dscheevos/launcher/rcheevos/*.c` becomes `Built from the rcheevos submodule (docs/rcheevos-pin.md).` when the first patch lands.
