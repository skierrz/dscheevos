# rcheevos on the DSi ARM9: footprint

Can rcheevos run, unmodified, on the DSi's ARM9 (ARM946E-S, ARMv5TE) next to a running DS game? This doc measures code size, stack, heap and per-frame cost, and checks whether it all fits above 4 MB alongside lwIP and WPA2.

**Short answer:** size and memory fit with room to spare. CPU time is the risk. With a typical achievement set, one `rc_client_do_frame` costs about a third to two-thirds of a 60 fps frame at 67 MHz. That is more idle time than many games have. Hardware timing is the next step before we commit to the design.

## Setup

| | |
|---|---|
| rcheevos | `683197d8` (2026-10-04), unmodified |
| Compiler | arm-none-eabi-gcc 13.2.1 (Ubuntu `gcc-arm-none-eabi`) + newlib. Docker Hub refused the `devkitpro/devkitarm:20241104` pull (HTTP 429), so this is the fallback toolchain from the plan. devkitARM uses GCC 14; expect sizes within a few percent. |
| Flags | `-mcpu=arm946e-s -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -fstack-usage -fcallgraph-info=su -std=gnu99` |
| Link | `--gc-sections`, rooted at a stub that calls only the `rc_client` functions DScheevos needs (create, login with token, identify+load, do_frame, idle, reset, rich presence, achievement info, unload, destroy) |
| Configs | **stock**: `RC_NO_THREADS RC_CLIENT_SUPPORTS_HASH`. **trimmed**: stock + `RC_HASH_NO_DISC RC_HASH_NO_ZIP RC_HASH_NO_ENCRYPTED` |

All trimming happens through these defines and the linker. No rcheevos source was edited.

`RC_NO_THREADS` is required, not optional: libnds has no pthreads, and `rc_compat.h` has no NDS branch.

The scripts behind every number are in `tools/rcheevos-footprint/` (see [Reproduce](#reproduce)).

## 1. Code and data size

### Per module, before dead-code removal

Each module built as one relocatable object (`ld -r`). Text includes read-only data.

| Module | stock | trimmed |
|---|---:|---:|
| client (`rc_client*.c`) | 26,994 | 26,970 |
| runtime (incl. `rc_validate.c`) | 41,994 | 41,994 |
| api | 25,537 | 25,537 |
| hash | 33,412 | 11,145 |
| util | 3,920 | 3,920 |
| libretro | not built | not built |

`rc_libretro.c` needs RetroArch's `libretro.h`. It is not in rcheevos' own Makefile object lists either, and DScheevos is not a libretro core, so it stays out.

### Linked, after `--gc-sections`

This is what would ship. Bytes; "strings" are string literals before the linker merges duplicates.

| Origin | code | rodata | strings | data | bss | total |
|---|---:|---:|---:|---:|---:|---:|
| rcheevos client | 14,746 | 0 | 3,662 | 0 | 16 | 18,424 |
| rcheevos runtime | 18,546 | 6,321 | 3,175 | 0 | 0 | 28,042 |
| rcheevos api | 9,092 | 2,297 | 1,516 | 0 | 8 | 12,913 |
| rcheevos hash | 6,328 | 1,344 | 1,730 | 0 | 24 | 9,426 |
| rcheevos util | 2,686 | 228 | 961 | 0 | 0 | 3,875 |
| **rcheevos total** | **51,398** | **10,190** | **11,044** | **0** | **48** | **72,680** |
| newlib libc | 31,400 | 1,329 | 544 | 1,712 | 780 | 35,765 |
| libgcc (soft-float, division) | 6,264 | 0 | 0 | 0 | 0 | 6,264 |
| newlib libm (`fmod`) | 952 | 16 | 0 | 0 | 0 | 968 |

ELF totals (after string merging):

| Build | .text | .rodata | .data | .bss | **total** |
|---|---:|---:|---:|---:|---:|
| stock | 128,064 | 30,012 | 1,712 | 876 | **160,664** |
| **trimmed** | 91,056 | 22,324 | 1,712 | 860 | **115,952** |
| trimmed + `-DNDEBUG` | 90,544 | 21,660 | 1,712 | 860 | 114,776 |
| trimmed, runtime+client in ARM mode | 111,056 | 22,324 | 1,712 | 860 | 135,952 |

Trimming the hash formats saves 44 KB. DScheevos ships with libnds, a TCP/IP stack and TLS anyway, so much of the 43 KB of newlib/libgcc (printf, malloc, string functions) will be shared rather than added.

### 20 biggest rcheevos functions (trimmed, linked)

| Bytes | Function | When it runs |
|---:|---|---|
| 2,028 | `md5_process` | load (hash), and on each unlock (request signature) |
| 1,712 | `rc_client_fetch_game_sets_callback` | load |
| 1,240 | `rc_client_raise_pending_events` | every frame |
| 1,152 | `rc_parse_condset` | load |
| 1,008 | `rc_client_do_frame` | every frame |
| 988 | `rc_parse_richpresence_internal` | load |
| 968 | `rc_client_activate_game` | load |
| 776 | `rc_parse_value_internal` | load |
| 764 | `rc_api_process_fetch_game_sets_server_response` | load |
| 704 | `rc_api_process_fetch_game_data_achievements` | load |
| 694 | `rc_parse_condition_internal` | load |
| 688 | `rc_parse_operand` | load |
| 676 | `rc_test_condition` | every frame |
| 676 | `rc_format_typed_value` | leaderboards, rich presence |
| 648 | `rc_condition_update_parse_state` | load |
| 620 | `rc_json_get_string` | load |
| 616 | `rc_client_submit_leaderboard_entry_callback` | leaderboard submit |
| 612 | `rc_hash_arcade` | never on DS (linked because `rc_hash_rom.c` is all-or-nothing) |
| 612 | `rc_client_award_achievement_callback` | unlock |
| 588 | `rc_hash_nintendo_ds` | load |

Biggest from libc: `_svfprintf_r` 5,224, `_dtoa_r` 4,132, `_vfiprintf_r` 2,688, `_malloc_r` 1,472, `_realloc_r` 900, `__ieee754_fmod` 860, `__sfvwrite_r` 808, soft-float `__adddf3`/`__subdf3` 788.

## 2. Stack

From the `.su` files, joined to a call graph built from the linked ELF's disassembly. Newlib wasn't built with `-fstack-usage`, so its frames come from each function's prologue (`push` + `sub sp`); those are marked ~.

### Biggest single frames

| Bytes | Function | Notes |
|---:|---|---|
| 2,056 | `rc_client_log_message_va` | 2 KB format buffer. Only entered when logging is on. |
| 1,080 | `rc_validate_*` | removed by `--gc-sections` |
| 1,056 | `rc_hash_get_first_item_from_playlist` | load only; not used for `.nds` |
| 1,040 | `rc_hash_dispatch_message_va` | 1 KB buffer for hash log messages |
| 936 | `rc_client_fetch_game_sets_callback` | load |
| 712 | `rc_parse_condset` | load |
| 664 | `rc_hash_nintendo_ds` | 512-byte header buffer |
| 608 | `rc_client_ping` | every ~2 minutes |
| ~328 | `_svfprintf_r` | newlib |

### Worst-case depth by entry point

| Entry point | Logging off | Logging on |
|---|---:|---:|
| `rc_client_do_frame` | **1,592** | 3,396 |
| `rc_client_idle` | 76 | 76 |
| `rc_client_get_rich_presence_message` | 1,336 | 1,336 |
| `rc_hash_nintendo_ds` | 2,660 | 2,660 |
| `rc_client_begin_identify_and_load_game` | 3,380 | 3,380 |

The `do_frame` worst path: `do_frame → raise_pending_events → award_achievement_server_call → rc_api_init_award_achievement_request_hosted → rc_format_md5 → snprintf → _svfprintf_r → _dtoa_r → bignum → malloc`. That path is a static upper bound: `rc_format_md5` prints `%02x`, which never reaches `_dtoa_r`.

Logging gates on `log_level` before it enters the 2 KB frame, so leaving logging off (the default) during play removes it without touching rcheevos.

Recursion: on the per-frame path, only `rc_evaluate_operand` recurses, bounded by how deeply an achievement nests operands. JSON parsing (`rc_json_parse_object`) recurses at load, bounded by the server response.

**What this means for us:** the ARM9's stack normally lives in the game's 16 KB DTCM. We must not borrow it. DScheevos code should switch to its own stack in main RAM before calling rcheevos. Budget **4 KB** for play (covers logging on) and **8 KB** at load to leave room for our file and network callbacks, which run on top of these figures.

## 3. Heap

### Where rcheevos allocates

All allocation goes through plain `malloc`/`calloc`/`realloc`/`free`/`strdup`. No custom allocator hook exists in this version, so we provide them by linking our own `malloc` family (a pool above 4 MB). 143 lines call one of them, by file: `rc_client.c` 77, `rc_runtime.c` 27, `rc_hash.c` 13, `rc_memref.c` 7, `rc_hash_rom.c` 7, `rc_alloc.c` 4, `rc_api_common.c` 3, `rc_util.c` 2, `rc_runtime_progress.c` 2, `rc_compat.c` 1 (counts include `free`).

The ones that matter:

| Site | What | When |
|---|---|---|
| `rc_util.c:77` `rc_buffer_reserve` | 256-byte-rounded chunks holding game strings and parsed triggers | load |
| `rc_alloc.c:176-203`, `rc_memref.c:181-186` | memref arrays | load |
| `rc_runtime.c:466-472` | rich presence | load |
| `rc_hash_rom.c:444` `rc_hash_nintendo_ds` | `max(arm9_size, arm7_size, 0xA00)`. Several hundred KB to a few MB for big games. | session start only |
| `rc_client.c:4872` | award callback data | each unlock, freed when the server answers |
| `rc_client.c:4719, 5463` | scheduled progress/leaderboard callbacks | when progress or a leaderboard tracker changes |
| `rc_api_common.c:1487-1495` | host string | `rc_client_set_host` |

### Measured peak

Measured by running the ARM build under `qemu-arm -cpu arm946` with a wrapped `malloc` that tracks every block. The sets are synthetic (see [Limits](#limits)), built to look like DS sets: pointer chains via AddAddress, deltas, hit counts, ResetIf, OrNext/AndNext, Measured, DTCM reads. Every trigger parses with rcheevos' own parser.

| Set | JSON | Heap after load | Peak during load | + JSON our HTTP layer holds | Allocs in a normal frame | Allocs in an unlock frame |
|---|---:|---:|---:|---:|---:|---:|
| typical: 60 achievements, 8 leaderboards, RP with 3 lookups | 49 KB | **134 KB** | **186 KB** | ~235 KB | 0 | 1 |
| heavy: 250 achievements, 40 leaderboards, RP with 6 lookups | 208 KB | **592 KB** | **797 KB** | ~1.0 MB | 0 | 2 |

What holds the heap after load (typical / heavy): `rc_buffer` chunks 57% / 59%, memref arrays 36% / 38%, rich presence 3% / 1%. No `realloc` calls happened in either run.

The synthetic sets use mostly unique random addresses. Real sets reuse the same pointer bases and addresses a lot, so real memref arrays should be smaller. Treat these as upper-end figures.

Session start also needs the ROM hash buffer above, freed before the game boots, so it doesn't stack on top of the game-data peak unless we overlap them.

Normal frames allocate nothing. Unlock and tracker frames make one or two small allocations; our `malloc` must be fast and must not touch game RAM.

One run, from an early generator bug where about half the achievements failed to parse, left 10.7 KB allocated after `rc_client_destroy`. With every trigger parsing, nothing leaks. I didn't track down the error-path leak; it only matters if a bad set reaches us.

### libc needs beyond what libnds/newlib give us

Everything rcheevos imports exists in devkitARM's newlib. The list:

- **memory/strings**: `malloc calloc realloc free memcpy memmove memset memcmp strlen strcmp strncmp strcpy strstr strdup strcasecmp strncasecmp`
- **conversion/sort**: `strtol strtoul atoi qsort bsearch _ctype_`
- **printf**: `snprintf vsnprintf`, including `%.1f`…`%.6f` (`rc_format.c:253-278`, the FLOAT value formats). This pulls newlib's full float printf (`_svfprintf_r` + `_dtoa_r`, ~10 KB). `iprintf` can't stand in.
- **math**: `fmod` (libm), plus libgcc soft-float for `float` and `double`. The ARM946E-S has no FPU.
- **division**: libgcc `__aeabi_uidiv`, `__aeabi_idivmod`, `__aeabi_uldivmod`. ARMv5TE has no divide instruction.
- **time**: `time` (unlock timestamps), `clock` (default timer, `rc_client.c:369`), `gmtime`/`mktime` (parsing dates in API responses). Supply our own timer with `rc_client_set_get_time_millisecs_function`; that removes the runtime need for `clock()`, which devkitARM may not back.
- **files**: `fopen fread fseek ftell fclose` in the default hash file reader. Replace it with `rc_client_set_hash_callbacks` so hashing reads the SD card through our own code.
- **`assert`**: 19 asserts (`rc_condition.c`, `rc_runtime_progress.c`, `rc_compat.c`). Build with `-DNDEBUG`; a failed assert would otherwise print and `abort()` inside a game.

Nothing needs threads, signals, sockets or dynamic loading. Networking stays on our side: rcheevos hands us a request (URL + POST body) and takes the response back.

## 4. Call graph from `rc_client_do_frame`

Ghidra wasn't available, so the call graph comes from the linked ELF: `objdump` disassembly for calls (catches every direct `bl`/`blx`, including into newlib and libgcc), DWARF `DW_TAG_inlined_subroutine` records for inlining, and indirect `blx rN` sites for callbacks.

### What `do_frame` reaches

196 functions: 88 rcheevos (14.2 KB), 104 newlib (24.5 KB), 4 libgcc.

Inlined into `rc_client_do_frame`: `rc_client_do_frame_process_achievements`, `rc_client_do_frame_process_leaderboards`, `rc_client_do_frame_update_progress_tracker`, `rc_client_update_memref_values`. Inlined into `rc_client_raise_pending_events`: the award, mastery, leaderboard, tracker and progress event raisers.

Indirect calls (our callbacks run inside `do_frame`): memory read (`rc_read_memory`), event handler (10 sites in `rc_client_raise_pending_events`), server call (award, leaderboard submit), log. Each of these runs on rcheevos' stack, inside our idle-time budget. The server-call callback must only queue the request; it must not touch the network inline.

### Pulled in unexpectedly

- **Full float printf** (`_svfprintf_r`, `_dtoa_r`, bignum helpers) via `snprintf`: leaderboard tracker text and rich presence.
- **MD5** on unlock: the award request carries a signature.
- **`malloc`/`free`** on unlock and tracker frames.
- **Soft-float double math** (`__adddf3`, `__divdf3`, `__muldf3`) and `fmod`: float memory sizes, divide and modulus operators.
- **`time()` → `_gettimeofday`** on unlock.
- **`abort`/`raise`/`kill`/`getpid`** behind `assert` (and newlib's own `dtoa` asserts, which `-DNDEBUG` can't remove).
- **`__gnu_thumb1_case_uqi`/`uhi`**: Thumb-1 has no jump-table instruction, so every dense `switch` becomes a call to this helper. It is 9.4% of all per-frame instructions (below).

Most of these fire on events, not every frame. The every-frame set is the condition evaluator, memref update, and our read callback.

### Too slow for idle time?

Counted with `qemu-arm -cpu arm946 -one-insn-per-tb -d exec`: one trace line per executed instruction, tallied between markers around each `do_frame`. RAM changes randomly between frames so that hit counts, deltas and unlocks all fire.

| Set | Build | Instructions per `do_frame` (median / max) |
|---|---|---:|
| typical | Thumb, `-Os` | **345,644** / 379,746 |
| typical | runtime+client ARM mode | **285,632** / 318,541 |
| heavy | Thumb, `-Os` | **1,609,222** / 1,691,897 |

Session start (`rc_client_begin_load_game`: JSON parse plus activation) costs 36M instructions for the typical set (~0.7–1.1 s at 67 MHz) and 591M for the heavy set (~11–18 s). Load cost grows much faster than set size (16× for 4× the achievements); I didn't profile why. It runs before the game boots, but 10+ seconds is a long wait.

`rc_client_idle`: 76 bytes of stack and negligible time. `get_rich_presence_message` returns a cached string (407 instructions); rich presence is rebuilt inside `do_frame`.

That's ~350 instructions per condition per frame (typical set: ~990 conditions, ~670 unique memrefs, 315 pointer chains).

To turn instructions into time I assume 1.3–2.0 cycles per instruction. The ARM946E-S takes 3 cycles per taken branch and stalls on load-use. On top of that, the typical set's 134 KB of trigger data doesn't fit the 4 KB data cache, so every frame streams it in from main RAM. qemu doesn't model either effect, so these are estimates:

| Set | At 67 MHz (DS mode) | Share of a 16.7 ms frame | At 133 MHz (DSi mode) |
|---|---:|---:|---:|
| typical | 6.7–10.3 ms | 40–62% | 3.4–5.2 ms |
| typical, ARM mode | 5.5–8.5 ms | 33–51% | 2.8–4.3 ms |
| heavy | 31–48 ms | 187–287% | 16–24 ms |

Where the typical set's frame time goes:

| Share | Function |
|---:|---|
| 11.7% | `rc_evaluate_operand` |
| 9.4% | `__gnu_thumb1_case_uqi` (Thumb switch helper) |
| 8.2% | `rc_client_do_frame` (with its inlined loops) |
| 7.1% | `rc_read_memory` |
| 6.6% | `rc_get_modified_memref_value` |
| 6.4% | `rc_typed_value_convert` |
| 6.2% | `memcpy` (from our read callback) |
| 4.4% | `read_mem` (our read callback) |
| 4.1% | `rc_condset_evaluate_condition_no_add_hits` |
| 4.0% | `rc_test_condition` |

**Verdict: yes, for many games.** At 67 MHz the typical set needs 40–62% of a frame. Plenty of DS games use more than half their frame. A heavy set can't fit. We can't skip or spread frames either: hit counts like `.300.` count frames, so evaluation has to keep pace at one per frame.

Ways to cut it without touching rcheevos source:

1. **Compile runtime and client in ARM mode** (`-marm`). Removes the switch helper and uses 3-operand ARM instructions. Costs ~20 KB of code. Measured: 17% fewer instructions per frame (345,644 → 285,632), i.e. 5.5–8.5 ms at 67 MHz, 33–51% of a frame. ARM code is bigger, so it puts more pressure on the 8 KB instruction cache; hardware timing will show how much of the 17% survives.
2. **Fast read callback.** Our callback plus its `memcpy` is ~11% of the frame in Thumb and ~13% in ARM mode. A direct 1/2/4-byte load from the frozen snapshot should drop most of it.
3. **Capture only what rcheevos reads.** Copy the memref addresses rcheevos asked for last frame into a small buffer at vblank (a few hundred loads), then evaluate from it during idle time. This keeps "frozen snapshot, once per frame" without copying 4 MB + 16 KB DTCM.
4. **`-O2` for the hot runtime files**, measured against size.
5. **133 MHz.** Halves the time but changes timing for some DS games, so it can't be the default across the library.

Even with all of these, a heavy set probably won't fit every game's idle time. We need real cycle counts on hardware (DSi timer around `do_frame`, real sets) before locking the design.

## 5. What the build can leave out

| Leave out | How | Saves |
|---|---|---:|
| Disc, zip and 3DS-encrypted hashing | `RC_HASH_NO_DISC RC_HASH_NO_ZIP RC_HASH_NO_ENCRYPTED` | 44.7 KB linked |
| libretro glue | don't compile `rc_libretro.c` | all of it |
| `rc_validate.c` | `--gc-sections` drops it | 5.7 KB unlinked |
| RAIntegration, external client | leave `RC_CLIENT_SUPPORTS_RAINTEGRATION` and `RC_CLIENT_SUPPORTS_EXTERNAL` undefined (default) | already out |
| Editor API (`rc_api_editor.c`) | `--gc-sections` | already out |
| Asserts | `-DNDEBUG` | 1.2 KB, and no `abort()` path from rcheevos |
| Logging | don't call `rc_client_enable_logging` in release | 2 KB of stack at runtime |
| Threads | `RC_NO_THREADS` | required anyway |

What can't go through config alone: `rc_hash_rom.c` is all-or-nothing behind `RC_HASH_NO_ROM`, and the DS hash lives there. So hashers for arcade, N64, Neo Geo, SNES, NES, Lynx and the rest stay linked (~2.1 KB). Dropping them would mean editing rcheevos, or computing the DS hash ourselves and passing it to `rc_client_begin_load_game` with `RC_CLIENT_SUPPORTS_HASH` off.

## 6. What can page from SD

Split by static reachability from what runs during play (`do_frame`, `idle`, `reset`, rich presence, achievement info, and the award/leaderboard/ping server callbacks):

| | Resident during play | Session start only (can page from SD) |
|---|---:|---:|
| rcheevos code | 21.6 KB | 29.8 KB |
| libc/libgcc code | 25.6 KB | 22.6 KB |

The session-start-only rcheevos code breaks down as: trigger/value/rich presence parsers 10.3 KB, login/load/list code 8.2 KB, hashing and MD5-for-hash 5.8 KB, JSON/API response parsing 4.1 KB, other 1.5 KB.

Paging rules that keep us inside the project rules:

- Load cold code from SD before the game boots, run login/hash/load, then drop it. SD reads only, no writes.
- Unlocks during play need MD5 and URL building, so those stay resident (they're in the 21.6 KB).
- Read-only data (strings, tables, ~22 KB) splits along the same lines. Most parser and JSON field-name strings are cold.

## 7. Does it fit above 4 MB with lwIP and WPA2?

### Our budget

| Item | Typical set | Heavy set | Source |
|---|---:|---:|---|
| rcheevos + libc code and data, resident | ~70 KB | ~70 KB | measured |
| rcheevos heap, steady | 134 KB | 592 KB | measured (synthetic sets) |
| Stack (play) | 4 KB | 4 KB | measured + margin |
| Snapshot of what rcheevos reads | <16 KB | <64 KB | estimate (point 3 above) |
| **rcheevos subtotal during play** | **~225 KB** | **~730 KB** | |
| lwIP (TCP, UDP, DHCP, DNS) + buffers | ~100 KB | ~100 KB | not measured; typical lwIP footprint |
| TLS client (RetroAchievements is HTTPS) | ~150 KB | ~150 KB | not measured; typical mbedTLS client + record buffers |
| WPA2 supplicant + driver glue | ~50 KB | ~50 KB | not measured |
| **Total during play** | **~0.5 MB** | **~1.0 MB** | |

At session start, add the game-data parse peak (+50 KB / +205 KB), the held JSON (49 KB / 208 KB), cold code, and the ROM hash buffer (up to a few MB, freed before the game boots).

The DSi has 12 MB above the 4 MB the game sees. On paper, ~0.5–1 MB is easy.

### What already lives there

nds-bootstrap (`DS-Homebrew/nds-bootstrap` at `b9044ff`, `retail/common/include/locations.h`) already claims nearly all of it for DS-mode games:

- **ROM cache**: `ROM_LOCATION 0x0C3EC000`, size `retail_CACHE_ADRESS_SIZE 0xBE0000`. That's 11.9 MB, starting right at the 4 MB line of the DSi's `0x0C000000` mirror of main RAM. Small ROMs load whole into it; big ones use it as a read cache.
- **Card engine, cheat engine, in-game menu, donor ROM data** at `0x0268xxxx`–`0x027Fxxxx` (offsets 6.5–8 MB).
- DSi-enhanced games running in DSi mode use all 16 MB for themselves. rcheevos' own DSi memory map reads all 16 MB, so there is no free main RAM in that mode at all.

So it fits, but only by taking space from nds-bootstrap's ROM cache. A 1 MB slice leaves a 10.9 MB cache. Two side effects to check against "zero cost to gameplay":

- ROMs between ~10.9 and ~11.9 MB that load whole today would fall back to cache mode, which reads SD during play and can stutter.
- Big ROMs already in cache mode get a ~9% smaller cache, so more misses.

Shrinking that cache is a constant change in nds-bootstrap's memory map, which needs a decision against the "hook points only" rule. DSi-mode titles need a different plan entirely.

## Limits

- **Synthetic sets.** I couldn't fetch real DS sets without RetroAchievements credentials. The generator follows common DS authoring patterns and every trigger passes rcheevos' parser, but address reuse is lower than in real sets. Rerun the harness with a few real sets (small, medium, the biggest DS set on the site).
- **Instruction counts, not cycles.** qemu counts instructions exactly but doesn't model the ARM946E-S pipeline, caches or main-RAM wait states. The ms figures rest on an assumed 1.3–2.0 CPI.
- **GCC 13.2 + Ubuntu newlib, not devkitARM.** Sizes should be close; devkitARM's newlib may differ in printf and malloc size.
- **lwIP, TLS and WPA2 sizes are estimates**, not measured here.
- **No Ghidra/REA pass** (see section 4).

## Next steps

1. Time `rc_client_do_frame` on a real DSi with a hardware timer, using real sets, in Thumb and ARM mode.
2. Measure idle time per frame across a sample of games to know the real budget.
3. Decide how to carve DScheevos' region out of nds-bootstrap's ROM cache, and what to do for DSi-mode titles.
4. Build lwIP + TLS + WPA2 for the ARM9 and measure them the same way.
5. Rerun the REA/Ghidra pass once Ghidra 12.1.x can be installed.

## Reproduce

```sh
git clone https://github.com/RetroAchievements/rcheevos   # tested at 683197d8
export RC=$PWD/rcheevos
cd tools/rcheevos-footprint

# sizes, .su/.ci files, linked probe ELF
./build.sh out/trim -DRC_NO_THREADS -DRC_CLIENT_SUPPORTS_HASH \
  -DRC_HASH_NO_DISC -DRC_HASH_NO_ZIP -DRC_HASH_NO_ENCRYPTED
python3 mapsplit.py out/trim/link.map
python3 stack.py out/trim rc_client_do_frame                # logging on
EXCL=rc_client_log_message_formatted,__assert_func \
  python3 stack.py out/trim rc_client_do_frame              # logging off
python3 callgraph.py out/trim/dscheevos_probe.elf rc_client_do_frame   # needs pyelftools

# heap and per-frame instruction counts (needs qemu-user)
cd harness
python3 gen_set.py 60 8 1 3 > set_typical.h
./run.sh ../out/trim set_typical.h typical
qemu-arm -cpu arm946 typical.elf 1800 512        # heap summary
qemu-arm -cpu arm946 typical.elf 1 0 0 dump > live.txt && python3 bycaller.py typical.elf live.txt
./count.sh typical.elf 120 512 > trace.txt && ./summarize.sh trace.txt && python3 hot.py typical.elf trace.txt
```

`ARM_MODS="runtime client" ./build.sh ...` builds those modules in ARM mode.
