# Badge pipeline (display phase 2): design

Status, 2026-10-12: the host side is built behind a build flag (`make -C dscheevos/display test BADGES=1`) and isn't wired into the launcher:
- PNG decoding, 64→32 downscale, the 16-colour median cut;
- the pack writer and reader (written to tmp, then renamed);
- the module's blit and the locked look;
- badges in the strip renderer.

The download step needs the owner's OK. Fetching, the launcher flow and the in-play SD read aren't built.

During play, phase 1 of the display shows text only. This document covers how badge images get from RetroAchievements onto the SD card, and from there into a notice. The launcher does all fetching, converting and SD writing before the game boots. During play, the module only reads, through an nds-bootstrap SD read path the perf session has yet to design.

## Facts this rests on

- RA serves badges as `https://media.retroachievements.org/Badge/<badge_name>.png`, plus `<badge_name>_lock.png` for the locked look. These are 64x64 RGB PNGs, mean 5.7 KB unlocked and 3.3 KB locked (14 sampled; `display-research/assets-and-ux.md`). rc_client gives each achievement's `badge_name`.
- The launcher's HTTP client (`launcher/source/net.c`, `http_request`) opens a new TCP connection and TLS session per request (`Connection: close`). A TLS handshake on the DSi's ARM9 is the slow part (to be measured; RSA/ECDHE at 133 MHz).
- 32x32 at 4bpp is 512 B of pixels + 32 B of palette = 544 B. LZ10 only gets it to about 520 B (measured), so compression isn't worth the decoder. 64x64 at 4bpp is 2080 B (about 1.5 KB with LZ10).

## Decisions in this design

1. **One keep-alive connection for all badges.** A new `http_session_t` keeps the TLS connection to `media.retroachievements.org` open and sends GETs one after another (HTTP/1.1 keep-alive, `Content-Length` or chunked bodies). That is one handshake per launch instead of one per badge. If the server closes the connection, reconnect and carry on.
2. **Only the unlocked image is downloaded.** The locked look is made at render time by a palette transform: 16 entries, greyscale, darkened. This halves the downloads and the SD space. (RA's `_lock` images are greyscale versions; ours will differ slightly, which is cosmetic.)
3. **32x32, 4bpp, uncompressed, fixed-size slots.** No decompressor in the module. A slot's offset follows from its index, and a notice needs one SD read of at most two sectors.
4. **One pack file per game**, contiguous, so nds-bootstrap needs one cluster map for it.
5. **The id table is read once.** The module reads the pack's sorted id table at its first badge lookup (via the phase-2 SD path) and keeps it: 4 B per achievement, so 400 B typical and 1 KB heavy. No ABI change.

## SD layout

```
sd:/_nds/dscheevos/badges/<game_id>.pak     one per game, rebuilt by the launcher when the set changes
```

### Pack format, version 1 (little-endian)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `DSCB` |
| 4 | 2 | version (1) |
| 6 | 2 | slot size (544) |
| 8 | 4 | game id |
| 12 | 4 | count (achievements with a slot) |
| 16 | 4 | offset of the id table |
| 20 | 4 | offset of the name table |
| 24 | 4 | offset of slot 0 (512-aligned) |
| 28 | 4 | offset of the 64x64 section (0 = none) |
| id table | 4 x count | achievement ids, ascending; slot i belongs to ids[i] |
| name table | 16 x count | badge_name per slot (NUL-padded), so the launcher can see which badges changed |
| slots | 544 x count | 32x16 bytes of 4bpp pixels (row-major, low nibble = left pixel), then 16 BGR555 palette entries |
| 64x64 section | 2080 x count | optional, for the black-frame card (P3) |

**Sizes:**

| Set size | 32x32 slots | Pack, 32x32 only | With the 64x64 section |
|---|---|---|---|
| 50 achievements | 27 KB | ~28 KB | ~130 KB |
| 100 | 54 KB | ~56 KB | ~264 KB |
| 250 (heavy) | 136 KB | ~141 KB | ~661 KB |

A missing slot (download failed, set changed during play) means the notice shows the built-in icon instead, as in phase 1.

### Real sets (the 31 RA sets matched in our ROM library, `tools/badge_sizes.py`)

The 64x64 column adds the optional section. "First download" is unlocked badges only, at the 5.7 KB mean.

| Game | Achievements | Badges | Pack (32x32) | + 64x64 | First download |
|---|---|---|---|---|---|
| Golden Sun: Dark Dawn | 151 | 131 | 83 KB | 390 KB | 725 KB |
| Resident Evil: Deadly Silence | 150 | 150 | 83 KB | 387 KB | 830 KB |
| Phoenix Wright: Ace Attorney | 141 | 141 | 78 KB | 364 KB | 780 KB |
| Pokémon HeartGold Version \| Pokémon SoulSilver Version | 136 | 136 | 75 KB | 352 KB | 753 KB |
| Mario Kart DS | 132 | 132 | 73 KB | 341 KB | 731 KB |
| Pokémon Black Version | 118 | 118 | 65 KB | 305 KB | 653 KB |
| Tetris DS | 102 | 102 | 57 KB | 264 KB | 565 KB |
| Pokémon Platinum Version | 101 | 101 | 56 KB | 261 KB | 559 KB |
| The World Ends with You | 98 | 98 | 54 KB | 253 KB | 542 KB |
| Spore Creatures | 93 | 76 | 51 KB | 240 KB | 421 KB |
| Kirby: Squeak Squad | 86 | 86 | 48 KB | 222 KB | 476 KB |
| Pac'n Roll | 79 | 79 | 44 KB | 204 KB | 437 KB |
| The Legend of Zelda: Spirit Tracks | 70 | 70 | 39 KB | 181 KB | 387 KB |
| The Legend of Zelda: Phantom Hourglass | 67 | 67 | 37 KB | 173 KB | 371 KB |
| Plants vs. Zombies | 64 | 64 | 36 KB | 166 KB | 354 KB |
| The Legendary Starfy | 58 | 58 | 32 KB | 150 KB | 321 KB |
| Trauma Center: Under the Knife 2 | 54 | 54 | 30 KB | 140 KB | 299 KB |
| Kirby Super Star Ultra | 50 | 50 | 28 KB | 130 KB | 277 KB |
| Mario vs. Donkey Kong: Mini-Land Mayhem! | 49 | 49 | 27 KB | 127 KB | 271 KB |
| Digimon World DS | 49 | 49 | 27 KB | 127 KB | 271 KB |
| Peggle: Dual Shot | 49 | 49 | 27 KB | 127 KB | 271 KB |
| Sonic Rush Adventure | 48 | 48 | 26 KB | 124 KB | 266 KB |
| Rhythm Heaven | 46 | 46 | 25 KB | 119 KB | 255 KB |
| Sonic Colors | 42 | 42 | 23 KB | 109 KB | 232 KB |
| Professor Layton and the Curious Village | 42 | 42 | 23 KB | 109 KB | 232 KB |
| Drawn To Life | 39 | 39 | 22 KB | 101 KB | 216 KB |
| Sonic Rush | 34 | 34 | 19 KB | 88 KB | 188 KB |
| Ghost Trick: Phantom Detective | 33 | 33 | 19 KB | 86 KB | 183 KB |
| Cory in the House | 32 | 32 | 18 KB | 83 KB | 177 KB |
| 999: Nine Hours, Nine Persons, Nine Doors | 23 | 23 | 13 KB | 59 KB | 127 KB |
| Cooking Mama | 16 | 16 | 9 KB | 42 KB | 89 KB |

31 sets; achievements median 58, max 151; pack median 32 KB, max 83 KB; all packs together 1248 KB

Every id table fits the module's 1 KB cap (the largest set has 151 ids = 604 B).

### Conversion quality (host tests)

- **Synthetic 64x64 gradient badge:** mean error 0.53 per channel in 5-bit units.
- **Exact images (≤ 16 colours in 2x2 blocks):** every pixel is preserved.
- **14 real RA badges, checked by eye** (local only; RA's images aren't committed): readable at 32x32. Photographic badges lose fine detail, as expected at 16 colours.

## Launcher flow (pre-boot, online, a safe point for SD writes)

1. After login and game load, rc_client knows every achievement's `badge_name`.
2. Open the existing pack if any. For each core-set achievement, the slot is still good if the name table has the same `badge_name`.
3. Download the missing badges over one keep-alive session. Show `Badges 12/100 (B: skip)` on the bottom screen. Skipping, or an error, leaves the rest as icons; nothing about achievements or hardcore depends on badges.
4. For each PNG:
   1. decode it (stb_image, PNG only, public domain);
   2. box-downscale 64→32;
   3. quantize to 16 colours (median cut in 15-bit space, no dithering: small icons look noisy dithered);
   4. write the slot.

   Also make the optional 64x64 slot if the card is enabled.
5. Write `<game_id>.pak.tmp`, then rename it over the old pack. SD writes happen only here, before boot.
6. Add the pack's path to what nds-bootstrap is told to map (phase-2 SD path, perf session).

**Time, estimated:** one TLS handshake (to measure), plus 100 x 5.7 KB = 570 KB over DSi Wi-Fi at an assumed 50-100 KB/s through mbedTLS (6-12 s), plus decoding and quantizing at about 20 ms each (~2 s). Later launches download only new or changed badges, usually none.

**Memory:** the launcher runs before the game, with no module constraints. Peak use is about one PNG plus a 64x64x4 decode buffer (16 KB), plus stb_image code (~30 KB).

## Module side (phase 2)

- **First badge lookup:** read the id table (count x 4 B) once into module bss; this reserves at most 1 KB, which caps the table at 256 ids.
- **Per notice:**
  1. binary-search the id;
  2. issue one SD read of 544 B at `slot0 + i * 544` through the phase-2 path, into a 544 B staging buffer;
  3. the renderer blits it with the palette, or with the greyscale transform for a locked one.
- **No decompressor and no extra code beyond the blit** (about 200 B).
- **Resident total:** 544 B staging + ≤ 1 KB id table + the blit, which keeps the module under the 16 KB display budget.

## Settings

- `BADGES = 1` in `dscheevos.ini` (default on; 0 never downloads).
- `BADGES_LARGE = 0` (the 64x64 section, only when P3 is enabled).

Neither setting affects achievements, hardcore or anything sent to RA.

## Privacy and compliance

- **Privacy:** downloads go only to `media.retroachievements.org`, with the launcher's user agent and no account data. Update `docs/privacy.md` (what's fetched, the SD cache).
- **Compliance:** badges are RA's own images, shown as RA intends. No rule is affected.

## Options considered and not chosen

- **A DScheevos-hosted proxy** that converts a set's badges into one pack (one small download). It's faster, but it adds a server to run, a third party in the data path, and redistribution of RA's images. Not without the owner and RA agreeing.
- **Per-badge files on SD:** small files waste clusters (32 KB clusters on big cards), and nds-bootstrap would need a cluster map per file.
- **LZ10-compressed slots:** about 5% smaller at 32x32, not worth a decompressor in the module.
- **Downloading `_lock.png` too:** twice the traffic for a greyscale version we can make from the palette.

## Open items

- Measure a TLS handshake and the throughput on the DSi: one launcher run with timestamps in `launcher.log`.
- The phase-2 SD read path in nds-bootstrap (perf session).
- Check that `media.retroachievements.org` keeps HTTP/1.1 keep-alive connections open for at least 100 sequential GETs.
