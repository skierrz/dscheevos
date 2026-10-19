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
  (The change-driven, dependency and changed-memory patches after it, 09ea746..754965e, are not listed here yet.)
  N. **skip unchanged direct reads** (rcheevos `e612011` on 754965e, `src/client/rc_client.c` +33 lines, one field in
     `rc_client_internal.h`; research/optimize/evalfast.md): with `rc_client_set_changed_memory` (754965e), a direct memref
     that didn't change last frame, was read in an earlier frame, and whose bytes are in none of at most 8 changed ranges is
     not read again (its value and changed flag would stay as they are). Deserializing progress makes every memref be read
     on the next frame. Our core passes 0 ranges on a repeat frame, so only last frame's changed memrefs are read.
     - **Identical results:** tests/difftest run.sh (CD=1 CHANGES=1) and core.sh on all 16 real sets with real RAM dumps,
       zero differences; a mutant (skips memrefs whose changed flag is set) shows differences on Kirby.
     - **Measured (melonDS cache model, not hardware):** evaluation of an unchanged frame -10% (Platinum) to -44% (Black,
       Digimon); Kirby -26%, MKDS -16%, Resident Evil -25%, MvDK -28%.
     - **Upstream:** generic (any host that knows its changed memory), but it builds on the unmerged changed-memory API;
       offer it together with that, as an issue, once the methods settle (CLAUDE.md).
  - **Load time for long AddSource chains** (2026-10-18, two commits on branch `dscheevos-683197d-loadfix`, after `754965e`; on the shipped stack cherry-picked after `e612011` as `58027f6` and `1a0269a`, branch `dscheevos-683197d-stack`; `src/runtime/rc_memref.c`, `rc_alloc.c`, `rc_internal.h`). Mario Party DS's official set has one achievement ("Wheel of Fortune") of 2,311 conditions: a SubSource/AddSource chain repeating 9 addresses, ~2,300 modified memrefs, each step's parent the step before. Loading it took 7.1G cycles (~106 s at 67 MHz) before the game started.
    1. **Hash a modified memref's depth in the memref index** (`c61a484`): the index hash followed modified memrefs only 2 levels deep, so the chain's steps shared a few hashes and every new step probed past all the earlier ones. `depth` is compared by `rc_operands_are_equal`, so equal entries still share a hash.
    2. **Index large preparse memref lists too** (`84d26e7`): rc_client's sizing pass parses each definition into a scratch list that had no index, so a definition's own memrefs were deduplicated by a linear scan (quadratic in that definition). A preparse list now gets the same index after 64 entries; `rc_destroy_preparse_state` frees it. Smaller definitions (nearly all) allocate nothing new.
    - **Host** (Apple M5, clang -O2, rc_client load): Mario Party DS 23.5 -> 10.9 -> 3.7 ms; MvDK Mini-Land Mayhem 10.6 -> 4.2; Layton 6.2 -> 4.1; Platinum 8.8 -> 7.0; no set slower beyond noise. On the DSi the load costs ~210-300M cycles per host millisecond (all 25 official title-matched sets), so Mario Party's ~23 ms was the 106 s.
    - **Identical results:** `tests/difftest/loadcmp.sh` (new): on all 76 set files, the load builds the same memref lists and condition operands, byte for byte, as stock 683197d (and as the previous pin); a mutant that skips deduplication in indexed preparse lists is caught on 6 sets. `run.sh` (inert) and `core.sh` on the 16 sets with real RAM dumps: see `docs/lab/footprint.md` on footprint2-lab ("Mario Party DS session load").
    - **Upstream:** helps every platform (a desktop load of this set is 23 ms -> 4 ms), so a candidate for a PR once the owner agrees; e01c29c (the index itself) isn't upstream either.
- `RCHEEVOS_VERSION_STRING` still says 12.5.0, so the user agent says `rcheevos/12.5.0` while we run 9 commits past it.
- Latest release tag: **v12.5.0**. Upstream `develop` is 3 commits past us: `aed817f` (runtime types made internal), `23bcf47` (project files), `116d018` (a test script).

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
