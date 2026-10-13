# Bottom-edge strip: allow list (first cut, 2026-10-12)

This joins two surveys:
- **The M1 survey** (`survey.md`, bottom-edge re-score): no raster IRQs or DMA conflicts in the strip's lines.
- **idle-coverage's idle census** (`docs/idle-census.md` on branch `idle-coverage`): how each game waits for VBlank, and on what share of frames it's already waiting by line 156.

Produced by `tests/display-survey/allow_list.py survey.md idle-census.md`.

The rules (`allow_list.py`): a **candidate** reached play in M1 and passes the bottom re-score, the module gets time in it today (the SDK idle thread's `OS_Halt`), and it's waiting by line 156 on at least 80% of frames. 50-80% is **marginal**: notices would often be cut short (R19).

## Result

**5 candidates:** Digimon World DS, Ghost Trick, Phoenix Wright, Tetris DS and Yoshi's Island DS.

**6 marginal:** Kirby Squeak Squad, NSMB, Peggle, Professor Layton, Sonic Colors and Sonic Rush Adventure.

That is 5 of the 22 SDK-idle titles that reached play in M1. Most of the big games are out:

| Out because | Titles |
|---|---|
| Busy at line 156 on most frames | Mario Kart DS, Cory, Diddy Kong, 999 |
| Raster IRQs in the strip's lines | Kirby SSU, Pokémon Black, Animal Crossing, Starfy, M&L BIS, Trauma Center 2, Clubhouse Games |
| The module gets no time today | the BIOS-wait and busy games: Sonic Rush, RE:DS, Super Princess Peach, Metroid Prime Pinball, Golden Sun DD and others |

## Strip height doesn't widen the reach

Recomputed from the census's per-frame `wait_line` (frames after the first 300). This is the share of frames already waiting by the line each strip height needs:

| Census run | 32-line strip (waiting by 156) | 16-line (by 172) | 8-line (by 180) |
|---|---|---|---|
| cory-in-the-house | 48% | 48% | 60% |
| diddy-kong-racing-ds | 39% | 39% | 39% |
| digimon | 96% | 96% | 96% |
| ghost-trick-phantom-detective | 97% | 97% | 97% |
| kirby-squeak-squad | 77% | 77% | 77% |
| layton | 63% | 64% | 65% |
| nine-hours-nine-persons-nine-doors | 40% | 50% | 54% |
| peggle | 92% | 92% | 92% |
| phoenix | 99% | 99% | 99% |
| sonic-rush-adventure | 75% | 77% | 78% |
| tetris | 97% | 97% | 97% |
| yoshi-s-island-ds | 96% | 96% | 96% |

Games are bimodal: they either finish their frame early or work right up to VBlank. A shorter strip barely helps, so 32 lines can stay.

## Caveats (the numbers are optimistic)

- **melonDS runs Thumb code about 2x too fast on I-cache hits** (eval-speed finding, 2026-10-12): the ARM946 issues at most one instruction per cycle. On a DSi, games finish their frames later, so the real "waiting by line 156" shares are lower and the real allow list smaller. The census should be rerun with eval-speed's `--thumb-cycle` runner before any title is allowed. The DSi decides in the end.
- **41 of 64 census runs are menus only** (generic script). Menus idle more than play. The gameplay-run titles are the more trustworthy rows.
- **The M1 bottom re-score's raster test is an upper bound** (see `survey.md`).

## What this means for the owner

Under the zero-lag rule, the on-screen strip reaches a minority of the library: about 5-11 of the 22 SDK-idle titles that reached play in emulation, and likely fewer on hardware. If RA's rules really require on-screen indicators (ra-compliance's strict reading), most DS games can't meet them on a DSi without spending some of the game's working time. That needs RA's answer (RAdmin question 1), or a looser owner rule for those games.

## Full table

| Game | Wait class | Census run | Waiting by line 156 | M1 reached play | M1 bottom re-score | Verdict |
|---|---|---|---|---|---|---|
| Animal Crossing - Wild World (USA) (Rev 1) | oshalt | menus (generic), 2499 | 36% | yes | no: raster IRQs in band on 100% of frames | no: raster IRQs in band on 100% of frames |
| Children of Mana (USA, Australia) | bios | menus (generic), 2499 | 73% | not surveyed | ? | no: the module gets no time today (bios) |
| Club Penguin - EPF - Herbert's Revenge (USA) (En,Fr,Es) | oshalt | menus (generic), 2499 | 87% | not surveyed | ? | unknown: not in the M1 survey |
| Club Penguin - Elite Penguin Force (USA) (En,Fr,Es) (Rev 2) | oshalt | menus (generic), 2499 | 80% | not surveyed | ? | unknown: not in the M1 survey |
| Clubhouse Games (USA) (En,Fr,De,Es,It) (Rev 1) | oshalt | menus (generic), 2499 | 67% | yes | no: raster IRQs in band on 100% of frames | no: raster IRQs in band on 100% of frames |
| Code Lyoko (USA) (En,Fr,Es) | busy | menus (generic), 2499 | 0% | not surveyed | ? | no: the module gets no time today (busy) |
| Cooking Mama (USA) | oshalt | menus (generic), 2499 | 91% | partial | unknown (partial play) | unknown: didn't reach play in M1 |
| Cory in the House (USA) | oshalt | menus (generic), 2499 | 48% | yes | yes | no: busy at line 156 on 52% of frames |
| Diddy Kong Racing DS (USA) | oshalt | menus (generic), 2499 | 35% | yes | yes | no: busy at line 156 on 65% of frames |
| Digimon World DS (USA) | oshalt | gameplay, 6999 | 92% | yes | yes | **candidate** |
| Drawn to Life (USA) (En,Fr) | oshalt | menus (generic), 2499 | 79% | partial | unknown (partial play) | unknown: didn't reach play in M1 |
| Drawn to Life - SpongeBob SquarePants Edition (USA) (En,Fr) | oshalt | menus (generic), 2499 | 89% | not surveyed | ? | unknown: not in the M1 survey |
| Drawn to Life - The Next Chapter (USA) (En,Fr) (Rev 1) | oshalt | menus (generic), 2499 | 81% | not surveyed | ? | unknown: not in the M1 survey |
| Ghost Trick - Phantom Detective (USA) (En,Fr,De,Es,It) | oshalt | menus (generic), 2499 | 86% | yes | yes | **candidate** |
| Golden Sun - Dark Dawn (USA, Australia) (En,Es) | busy | gameplay, 1499 | 0% | yes | yes | no: the module gets no time today (busy) |
| Hotel Dusk - Room 215 (USA) | oshalt | menus (generic), 2499 | 85% | not surveyed | ? | unknown: not in the M1 survey |
| Kirby - Canvas Curse (USA) | bios | menus (generic), 2499 | 87% | no | unknown (didn't reach play) | no: the module gets no time today (bios) |
| Kirby - Squeak Squad (USA) | oshalt | menus (generic), 2499 | 69% | yes | yes | marginal (waiting by 156 on 69%) |
| Kirby Mass Attack (USA) (En,Fr,Es) | oshalt | menus (generic), 2499 | 89% | not surveyed | ? | unknown: not in the M1 survey |
| Kirby Super Star Ultra (USA) | oshalt | gameplay, 2999 | 0% | yes | no: raster IRQs in band on 100% of frames | no: raster IRQs in band on 100% of frames |
| LEGO Star Wars - The Complete Saga (USA) | oshalt | menus (generic), 2499 | 55% | partial | unknown (partial play) | unknown: didn't reach play in M1 |
| Legend of Zelda, The - Phantom Hourglass (USA) (En,Fr,Es) | oshalt | gameplay, 1499 | 79% | no | unknown (didn't reach play) | unknown: didn't reach play in M1 |
| Legend of Zelda, The - Spirit Tracks (USA, Australia) (En,Fr,Es) | oshalt | gameplay, 7499 | 88% | not surveyed | ? | unknown: not in the M1 survey |
| Legendary Starfy, The (USA) | oshalt | menus (generic), 2499 | 48% | yes | no: raster IRQs in band on 65% of frames | no: raster IRQs in band on 65% of frames |
| Mario & Luigi - Bowser's Inside Story (USA) (En,Fr,Es) | oshalt | menus (generic), 2499 | 41% | yes | no: raster IRQs in band on 64% of frames | no: raster IRQs in band on 64% of frames |
| Mario & Luigi - Partners in Time (USA) (Rev 1) | bios | menus (generic), 2499 | 85% | not surveyed | ? | no: the module gets no time today (bios) |
| Mario Hoops 3 on 3 (USA) | oshalt | menus (generic), 2499 | 0% | not surveyed | ? | unknown: not in the M1 survey |
| Mario Kart DS (USA, Australia) (En,Fr,De,Es,It) | oshalt | gameplay, 3499 | 0% | yes | yes | no: busy at line 156 on 100% of frames |
| Mario Party DS (USA) (Rev 2) | oshalt | menus (generic), 2499 | 0% | no | unknown (didn't reach play) | unknown: didn't reach play in M1 |
| Mario vs. Donkey Kong - Mini-Land Mayhem! (Europe) (En,Fr,De,Es,It) (NDSi Enhanced) | oshalt | gameplay, 4999 | 47% | not surveyed | ? | unknown: not in the M1 survey |
| Mario vs. Donkey Kong - Mini-Land Mayhem! (USA) (En,Fr,Es) (Rev 2) (NDSi Enhanced) | oshalt | menus (generic), 2499 | 88% | not surveyed | ? | unknown: not in the M1 survey |
| Mario vs. Donkey Kong 2 - March of the Minis (USA) | oshalt | menus (generic), 2499 | 71% | not surveyed | ? | unknown: not in the M1 survey |
| Metroid Prime Pinball (USA) | bios | menus (generic), 2499 | 70% | yes | yes | no: the module gets no time today (bios) |
| New Super Mario Bros. (USA, Australia) | oshalt | gameplay, 4249 | 70% | yes | yes | marginal (waiting by 156 on 70%) |
| Nine Hours, Nine Persons, Nine Doors (USA) | oshalt | menus (generic), 2499 | 36% | yes | yes | no: busy at line 156 on 64% of frames |
| Nintendogs - Best Friends (USA) | oshalt | menus (generic), 2499 | 0% | no | unknown (didn't reach play) | unknown: didn't reach play in M1 |
| Pac'n Roll (USA) | bios | menus (generic), 2499 | 85% | no | unknown (didn't reach play) | no: the module gets no time today (bios) |
| Peggle Dual Shot (USA) | oshalt | gameplay, 1499 | 75% | yes | yes | marginal (waiting by 156 on 75%) |
| Phoenix Wright - Ace Attorney (USA) | oshalt | gameplay, 2249 | 92% | yes | yes | **candidate** |
| Plants vs. Zombies (USA) | oshalt | menus (generic), 2499 | 85% | no | unknown (didn't reach play) | unknown: didn't reach play in M1 |
| Pokemon - Black Version (USA, Europe) (NDSi Enhanced) | oshalt | gameplay, 9499 | 0% | yes | no: raster IRQs in band on 100% of frames | no: raster IRQs in band on 100% of frames |
| Pokemon - Platinum Version (USA) (Rev 1) | oshalt | gameplay, 13249 | 92% | no | unknown (didn't reach play) | unknown: didn't reach play in M1 |
| Pokemon - SoulSilver Version (USA, Australia) | oshalt | gameplay, 8249 | 87% | no | unknown (didn't reach play) | unknown: didn't reach play in M1 |
| Pokemon Mystery Dungeon - Blue Rescue Team (USA, Australia) | oshalt | menus (generic), 2499 | 85% | not surveyed | ? | unknown: not in the M1 survey |
| Pokemon Mystery Dungeon - Explorers of Sky (USA) | oshalt | menus (generic), 2499 | 61% | not surveyed | ? | unknown: not in the M1 survey |
| Pokemon Ranger (USA) | bios | menus (generic), 2499 | 58% | not surveyed | ? | no: the module gets no time today (bios) |
| Professor Layton and the Curious Village (USA, Australia) | oshalt | gameplay, 4999 | 60% | yes | yes | marginal (waiting by 156 on 60%) |
| Resident Evil - Deadly Silence (USA) | bios | gameplay, 3249 | 54% | yes | yes | no: the module gets no time today (bios) |
| Rhythm Heaven (USA) | oshalt | gameplay, 2999 | 68% | no | unknown (didn't reach play) | unknown: didn't reach play in M1 |
| Scribblenauts (USA) (En,Fr,Es,Pt) | oshalt | menus (generic), 2499 | 80% | not surveyed | ? | unknown: not in the M1 survey |
| Sonic Colors (USA) (En,Ja,Fr,De,Es,It) | oshalt | gameplay, 3749 | 76% | yes | yes | marginal (waiting by 156 on 76%) |
| Sonic Rush (USA) (En,Ja,Fr,De,Es,It) | bios | gameplay, 2499 | 89% | yes | yes | no: the module gets no time today (bios) |
| Sonic Rush Adventure (USA) (En,Ja,Fr,De,Es,It) | oshalt | menus (generic), 2499 | 66% | yes | yes | marginal (waiting by 156 on 66%) |
| Spore Creatures (USA) (En,Fr,De,Es,It,Nl) | oshalt | menus (generic), 2499 | 86% | no | unknown (didn't reach play) | unknown: didn't reach play in M1 |
| Super Mario 64 DS (USA, Australia) (Rev 1) | oshalt | gameplay, 8249 | 78% | no | unknown (didn't reach play) | unknown: didn't reach play in M1 |
| Super Princess Peach (USA) | bios | menus (generic), 2499 | 71% | yes | yes | no: the module gets no time today (bios) |
| Super Scribblenauts (USA) (En,Fr,Es,Pt) | oshalt | menus (generic), 2499 | 74% | not surveyed | ? | unknown: not in the M1 survey |
| Tetris DS (USA, Australia) | oshalt | gameplay, 2999 | 93% | yes | yes | **candidate** |
| Trauma Center - Under the Knife (USA) | oshalt | menus (generic), 2499 | 85% | not surveyed | ? | unknown: not in the M1 survey |
| Trauma Center - Under the Knife 2 (USA) | oshalt | menus (generic), 2499 | 39% | yes | no: raster IRQs in band on 100% of frames | no: raster IRQs in band on 100% of frames |
| WarioWare - D.I.Y. (USA) | oshalt | menus (generic), 2499 | 73% | not surveyed | ? | unknown: not in the M1 survey |
| WarioWare - Touched! (USA) | oshalt | menus (generic), 2499 | 90% | no | unknown (didn't reach play) | unknown: didn't reach play in M1 |
| World Ends with You, The (USA) | oshalt | gameplay, 8249 | 95% | no | unknown (didn't reach play) | unknown: didn't reach play in M1 |
| Yoshi's Island DS (USA) (Rev 1) | oshalt | menus (generic), 2499 | 85% | yes | yes | **candidate** |

Candidates: 5 (Digimon World DS (USA), Ghost Trick - Phantom Detective (USA) (En,Fr,De,Es,It), Phoenix Wright - Ace Attorney (USA), Tetris DS (USA, Australia), Yoshi's Island DS (USA) (Rev 1)). Marginal: 6. Of 22 oshalt titles that reached play in M1, 5 are candidates.
