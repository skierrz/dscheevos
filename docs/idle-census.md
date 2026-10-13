# Wait census: how DS games wait for the next frame (2026-10-12)

Branch `idle-coverage`. Tools: `tests/perf/census.sh` (runner `--wait-census`, melonDS branch
`dscheevos-wait-census`), `tests/perf/census_report.py`, static `tests/perf/waitscan.py`.
Stock runs (achievements off, DS mode, launcher flow), melonDS interpreter, no cache model.
"menus (generic)" = 2,500 intervals of a menu-masher script from the game's start (title, file select,
sometimes the first scene); "gameplay" = the idle-budget input scripts (tests/perf README) that reach play.

## Result

| Class | ROMs | What DScheevos gets today |
|---|---:|---|
| `oshalt`: the SDK idle thread's `OS_Halt` (the module's hook) | 53 | idle time (as designed) |
| `bios`: SWI 5 `SVC_WaitVBlankIntr` on the game's main thread | 9 | **nothing** |
| `busy`: never halts (own scheduler / poll loop) | 2 (+ Diddy Kong Racing in its 3D engine) | **nothing** |

- BIOS waits occur **only in NitroSDK 2.x games** (9 of the 14 SDK 2.x ROMs: Sonic Rush, RE: Deadly Silence,
  Children of Mana, Kirby Canvas Curse, M&L Partners in Time, Metroid Prime Pinball, Pac'n Roll, Pokemon Ranger,
  Super Princess Peach). All 50 SDK 3.x-5.x ROMs use OS_Halt except two busy engines (Golden Sun: Dark Dawn,
  Camelot's task scheduler; Code Lyoko). In every BIOS game one call site accounts for >= 97% of halted time
  (the main loop's call of the SDK wrapper `SVC_WaitVBlankIntr`).
- **The BIOS wait is hookable the same way in every game**: all 64 ROMs have the SDK wrappers
  `SVC_WaitIntr` / `SVC_WaitVBlankIntr` with the identical Thumb shape `movs r2,#0; svc 4|5; bx lr`, at a random
  offset inside the ARM9 secure area (first 2 KB, random fill around them). Static callers are a lower bound
  (Metroid Prime Pinball reaches it through a register branch), so the hook must sit on the wrapper.
- The module's idle-loop check (call followed by `b` back to it) accepts every OS_Halt game measured (incl. NSMB and
  Animal Crossing, whose idle loop runs from ITCM). Legendary Starfy has 2% of halted time in OS_Halt from another caller.
- Longer runs (8,000 intervals, generic script): **Diddy Kong Racing never halts in its 3D engine** (intervals
  3000-7000: 0% halted): a game thread spins `for(;;) consume(obj)` on a ring buffer's read/write indices
  (0x0201A344 / 0x0201A294), so the SDK idle thread never runs. It is a third busy engine (SDK 3.x), invisible in the
  menu run, so menu-only classifications can miss gameplay-only busy loops and the busy estimate is a floor.
  LEGO Star Wars (44% halted), M&L Bowser's Inside Story (18% halted but OS_Halt in 95% of intervals), 999 (67%),
  Trauma Center (61%), WarioWare Touched (71%): OS_Halt, no BIOS waits in their later scenes. Code Lyoko: busy
  throughout (`flag=0; while(!flag) poll(obj);` at 0x020571CC).
- SDK 2.x games that idle in OS_Halt in menus but have direct SWI 5 callers (gameplay may differ):
  Trauma Center, WarioWare Touched, Mario Kart DS / Animal Crossing (SDK 3.0 RC; MKDS race: OS_Halt).
- RetroAchievements: 681 DS games with sets (580 not hacks/demos); 538 matched to No-Intro serials: A 178,
  Y 118, B 92, C 88, other 62. In the owner's library 14 of 34 A-serial ROMs are SDK 2.x. Estimate for the
  RA library: ~45-50 games (~9%) wait in the BIOS, ~3% are busy engines (unknown engines; needs dumps).
- "Waiting by line 156" (for the display strip): share of intervals whose last wait (a run of halts with
  < 4000 133-MHz cycles of non-IRQ code between them) began by line 156, lines counted from VBlank.
  0% for KSSU, MKDS, Pokemon Black, Mario Hoops, Mario Party, Nintendogs: a thread runs late in every frame
  (KSSU's last wait starts at lines 188-191).
- Side finding: every game shows ~185 VCOUNT reads per interval from 0x023BF3AE (nds-bootstrap's ARM9 card
  engine area in DS mode), i.e. nds-bootstrap itself polls; worth a look by whoever owns nds-bootstrap perf.

## Per ROM

| Game | code | SDK | RA set | run | class | busy | halted: OS_Halt / BIOS / other / OS_Halt-rejected | waiting by line 156 | main halt site | static SWI4/5/6 callers |
|---|---|---|---|---|---|---|---|---|---|---|
| Children of Mana (USA, Australia) | ASEE | 02027533 | 9752 | menus (generic), 2499 | bios | 37% | 1 / 61 / 0 / 0 % | 73% | SWI5 VBlankIntrWait pc 02000196 lr 0203df0c (98%) | 0/4/0 |
| Kirby - Canvas Curse (USA) | ATKE | 02007531 | 6102 | menus (generic), 2499 | bios | 19% | 0 / 81 / 0 / 0 % | 87% | SWI5 VBlankIntrWait pc 0200008e lr 020026f5 (100%) | 0/4/0 |
| Mario & Luigi - Partners in Time (USA) (Rev 1) | ARME | 02017532 | 3118 | menus (generic), 2499 | bios | 23% | 0 / 76 / 0 / 0 % | 85% | SWI5 VBlankIntrWait pc 0200429e lr 020295c4 (100%) | 0/3/0 |
| Metroid Prime Pinball (USA) | AP2E | 02017532 | 15542 | menus (generic), 2499 | bios | 26% | 0 / 74 / 0 / 0 % | 70% | SWI5 VBlankIntrWait pc 0200072c lr 02011a9c (100%) | 0/0/0 |
| Pac'n Roll (USA) | APNE | 02017530 | 14804 | menus (generic), 2499 | bios | 38% | 0 / 59 / 0 / 0 % | 85% | SWI5 VBlankIntrWait pc 02000514 lr 0202aa28 (100%) | 0/2/0 |
| Pokemon Ranger (USA) | ARGE | 02027533 | 16295 | menus (generic), 2499 | bios | 27% | 0 / 73 / 0 / 0 % | 58% | SWI5 VBlankIntrWait pc 02000306 lr 02001301 (97%) | 0/22/0 |
| Resident Evil - Deadly Silence (USA) | ABHE | 02027530 | 16274 | gameplay, 3249 | bios | 47% | 11 / 41 / 0 / 0 % | 54% | SWI5 VBlankIntrWait pc 020005f0 lr 020058c0 (79%) | 0/5/0 |
| Sonic Rush (USA) (En,Ja,Fr,De,Es,It) | ASCE | 02017532 | 14806 | gameplay, 2499 | bios | 42% | 0 / 56 / 0 / 0 % | 89% | SWI5 VBlankIntrWait pc 0200007a lr 02000c4c (100%) | 0/4/0 |
| Super Princess Peach (USA) | ASPE | 02017532 | 9252 | menus (generic), 2499 | bios | 39% | 0 / 58 / 0 / 0 % | 71% | SWI5 VBlankIntrWait pc 0200072c lr 02001ca4 (99%) | 0/1/0 |
| Code Lyoko (USA) (En,Fr,Es) | AL5E | 03017534 | no | menus (generic), 2499 | busy | 99% | 0 / 0 / 0 / 0 % | 0% | OS_Halt pc 0200b2e4 lr 02008e84 (100%) | 0/0/0 |
| Golden Sun - Dark Dawn (USA, Australia) (En,Es) | BO5E | 05047531 | 11788 | gameplay, 1499 | busy | 92% | 0 / 0 / 0 / 0 % | 0% | OS_Halt pc 02055dc0 lr 02053f44 (100%) | 0/0/0 |
| Animal Crossing - Wild World (USA) (Rev 1) | ADME | 030028a0 | 9594 | menus (generic), 2499 | oshalt | 53% | 46 / 0 / 0 / 0 % | 36% | OS_Halt pc 01ffa3c4 lr 01ffa4fc (100%) | 0/1/0 |
| Pokemon - Black Version (USA, Europe) (NDSi Enhanced) | IRBO | 0503757c | 3887 | gameplay, 9499 | oshalt | 37% | 62 / 0 / 0 / 0 % | 0% | OS_Halt pc 020882d4 lr 02085b2c (100%) | 0/0/0 |
| Club Penguin - Elite Penguin Force (USA) (En,Fr,Es) (Rev 2) | CLPE | 04027539 | 9260 | menus (generic), 2499 | oshalt | 33% | 66 / 0 / 0 / 0 % | 80% | OS_Halt pc 0203dc00 lr 0203c6b3 (100%) | 0/0/0 |
| Club Penguin - EPF - Herbert's Revenge (USA) (En,Fr,Es) | CY9E | 05037536 | no | menus (generic), 2499 | oshalt | 21% | 78 / 0 / 0 / 0 % | 87% | OS_Halt pc 020459c8 lr 0204408b (100%) | 0/0/0 |
| Clubhouse Games (USA) (En,Fr,De,Es,It) (Rev 1) | ATDE | 03017534 | no | menus (generic), 2499 | oshalt | 36% | 64 / 0 / 0 / 0 % | 67% | OS_Halt pc 020611bc lr 0205f189 (100%) | 0/0/0 |
| Cooking Mama (USA) | ACCE | 03007532 | 9759 | menus (generic), 2499 | oshalt | 23% | 77 / 0 / 0 / 0 % | 91% | OS_Halt pc 02006438 lr 020048d8 (100%) | 0/0/0 |
| Cory in the House (USA) | YIHE | 04017531 | 12712 | menus (generic), 2499 | oshalt | 29% | 67 / 0 / 0 / 0 % | 48% | OS_Halt pc 02008328 lr 02006aa0 (100%) | 0/0/0 |
| Diddy Kong Racing DS (USA) | AWDE | 03027530 | 9546 | menus (generic), 2499 | oshalt | 60% | 35 / 0 / 0 / 0 % | 35% | OS_Halt pc 0207488c lr 02073657 (100%) | 0/0/0 |
| Digimon World DS (USA) | ADNE | 03007532 | 16153 | gameplay, 6999 | oshalt | 23% | 77 / 0 / 0 / 0 % | 92% | OS_Halt pc 020e6248 lr 020e3c24 (100%) | 0/0/0 |
| Drawn to Life (USA) (En,Fr) | YDWE | 04007531 | 14782 | menus (generic), 2499 | oshalt | 17% | 82 / 0 / 0 / 0 % | 79% | OS_Halt pc 02006c1c lr 02005534 (100%) | 0/0/0 |
| Drawn to Life - SpongeBob SquarePants Edition (USA) (En,Fr) | CDLE | 04027531 | 17565 | menus (generic), 2499 | oshalt | 21% | 78 / 0 / 0 / 0 % | 89% | OS_Halt pc 0202828c lr 0202719c (100%) | 0/0/0 |
| Drawn to Life - The Next Chapter (USA) (En,Fr) (Rev 1) | BDRE | 05037536 | no | menus (generic), 2499 | oshalt | 20% | 80 / 0 / 0 / 0 % | 81% | OS_Halt pc 02062550 lr 0206100b (100%) | 0/0/0 |
| Ghost Trick - Phantom Detective (USA) (En,Fr,De,Es,It) | BGTE | 05047530 | 4911 | menus (generic), 2499 | oshalt | 26% | 74 / 0 / 0 / 0 % | 86% | OS_Halt pc 02075fc8 lr 02074394 (100%) | 0/0/0 |
| Hotel Dusk - Room 215 (USA) | AWIE | 03027530 | 6032 | menus (generic), 2499 | oshalt | 18% | 81 / 0 / 0 / 0 % | 85% | OS_Halt pc 020c3344 lr 020c18ec (100%) | 0/0/0 |
| Kirby Super Star Ultra (USA) | YKWE | 04027531 | 14763 | gameplay, 2999 | oshalt | 42% | 58 / 0 / 0 / 0 % | 0% | OS_Halt pc 020201e4 lr 0201ea80 (100%) | 0/0/0 |
| Kirby Mass Attack (USA) (En,Fr,Es) | TADE | 05057531 | 14788 | menus (generic), 2499 | oshalt | 16% | 84 / 0 / 0 / 0 % | 89% | OS_Halt pc 0204bafc lr 0204a637 (100%) | 0/0/0 |
| Kirby - Squeak Squad (USA) | AKWE | 03007532 | 14700 | menus (generic), 2499 | oshalt | 41% | 59 / 0 / 0 / 0 % | 69% | OS_Halt pc 0214fc08 lr 0214db8c (100%) | 0/0/0 |
| Professor Layton and the Curious Village (USA, Australia) | A5FE | 04017531 | 9518 | gameplay, 4999 | oshalt | 41% | 54 / 0 / 0 / 0 % | 60% | OS_Halt pc 0209b348 lr 0209988c (100%) | 0/0/0 |
| Legendary Starfy, The (USA) | YSSE | 04017533 | 9119 | menus (generic), 2499 | oshalt | 41% | 53 / 0 / 0 / 2 % | 48% | OS_Halt pc 02060568 lr 0205ea68 (96%) | 0/0/0 |
| LEGO Star Wars - The Complete Saga (USA) | YLGE | 03027532 | 15351 | menus (generic), 2499 | oshalt | 65% | 33 / 0 / 0 / 0 % | 55% | OS_Halt pc 020f9a88 lr 020f77f8 (100%) | 0/0/0 |
| Mario Hoops 3 on 3 (USA) | AB3E | 03007532 | 15614 | menus (generic), 2499 | oshalt | 45% | 55 / 0 / 0 / 0 % | 0% | OS_Halt pc 02009d00 lr 02007f50 (100%) | 0/0/0 |
| Mario & Luigi - Bowser's Inside Story (USA) (En,Fr,Es) | CLJE | 04027538 | 5323 | menus (generic), 2499 | oshalt | 59% | 38 / 0 / 0 / 0 % | 41% | OS_Halt pc 0203a084 lr 02038fc4 (100%) | 0/0/0 |
| Mario Party DS (USA) (Rev 2) | A8TE | 04007531 | 11730 | menus (generic), 2499 | oshalt | 38% | 62 / 0 / 0 / 0 % | 0% | OS_Halt pc 020652d0 lr 02063640 (100%) | 0/0/0 |
| Mario vs. Donkey Kong 2 - March of the Minis (USA) | AMQE | 03017532 | 14814 | menus (generic), 2499 | oshalt | 24% | 76 / 0 / 0 / 0 % | 71% | OS_Halt pc 02043454 lr 020417c4 (100%) | 0/0/0 |
| Mario vs. Donkey Kong - Mini-Land Mayhem! (USA) (En,Fr,Es) (Rev 2) (NDSi Enhanced) | V2GE | 05047532 | 14815 | menus (generic), 2499 | oshalt | 23% | 77 / 0 / 0 / 0 % | 88% | OS_Halt pc 0205eee4 lr 0205c894 (100%) | 0/0/0 |
| Mario Kart DS (USA, Australia) (En,Fr,De,Es,It) | AMCE | 030028a0 | 12711 | gameplay, 3499 | oshalt | 40% | 57 / 0 / 0 / 0 % | 0% | OS_Halt pc 0200fd08 lr 0200dbe4 (100%) | 0/7/0 |
| Mario vs. Donkey Kong - Mini-Land Mayhem! (Europe) (En,Fr,De,Es,It) (NDSi Enhanced) | V2GV | 05047532 | 14815 | gameplay, 4999 | oshalt | 58% | 40 / 0 / 0 / 0 % | 47% | OS_Halt pc 0205fb10 lr 0205d408 (100%) | 0/0/0 |
| Nine Hours, Nine Persons, Nine Doors (USA) | BSKE | 04027539 | 999 | menus (generic), 2499 | oshalt | 57% | 42 / 0 / 0 / 0 % | 36% | OS_Halt pc 0207ef50 lr 0207d8c0 (100%) | 0/0/0 |
| Nintendogs - Best Friends (USA) | AD5E | 02017530 | no | menus (generic), 2499 | oshalt | 48% | 51 / 0 / 0 / 0 % | 0% | OS_Halt pc 02080c90 lr 0207f3ac (100%) | 0/0/0 |
| New Super Mario Bros. (USA, Australia) | A2DE | 03007532 | 11729 | gameplay, 4249 | oshalt | 47% | 47 / 0 / 0 / 0 % | 70% | OS_Halt pc 01ffa6b0 lr 01ffa7e8 (100%) | 0/0/0 |
| Peggle Dual Shot (USA) | C7PE | 04027533 | 17633 | gameplay, 1499 | oshalt | 36% | 64 / 0 / 0 / 0 % | 75% | OS_Halt pc 020b0f14 lr 020af598 (100%) | 0/0/0 |
| Phoenix Wright - Ace Attorney (USA) | AGYE | 02017530 | 12747 | gameplay, 2249 | oshalt | 11% | 88 / 0 / 0 / 0 % | 92% | OS_Halt pc 02006e00 lr 02004df4 (100%) | 0/0/0 |
| Plants vs. Zombies (USA) | BWBE | 05047532 | 12718 | menus (generic), 2499 | oshalt | 23% | 77 / 0 / 0 / 0 % | 85% | OS_Halt pc 02006a38 lr 02004d60 (100%) | 0/0/0 |
| Pokemon - Platinum Version (USA) (Rev 1) | CPUE | 04027531 | 11732 | gameplay, 13249 | oshalt | 7% | 93 / 0 / 0 / 0 % | 92% | OS_Halt pc 020c42bc lr 020c25a0 (100%) | 0/0/0 |
| Pokemon Mystery Dungeon - Blue Rescue Team (USA, Australia) | APHE | 03007530 | 17492 | menus (generic), 2499 | oshalt | 26% | 74 / 0 / 0 / 0 % | 85% | OS_Halt pc 02074b34 lr 02072cc4 (100%) | 0/0/0 |
| Pokemon Mystery Dungeon - Explorers of Sky (USA) | C2SE | 04027534 | 14853 | menus (generic), 2499 | oshalt | 35% | 64 / 0 / 0 / 0 % | 61% | OS_Halt pc 0207bc34 lr 02079c10 (100%) | 0/0/0 |
| Rhythm Heaven (USA) | YLZE | 04027531 | 9985 | gameplay, 2999 | oshalt | 41% | 59 / 0 / 0 / 0 % | 68% | OS_Halt pc 02031f28 lr 02030af8 (100%) | 0/0/0 |
| Scribblenauts (USA) (En,Fr,Es,Pt) | BSLE | 04027537 | 6616 | menus (generic), 2499 | oshalt | 18% | 81 / 0 / 0 / 0 % | 80% | OS_Halt pc 02057608 lr 020564bb (100%) | 0/0/0 |
| Super Mario 64 DS (USA, Australia) (Rev 1) | ASME | 020029a6 | 9983 | gameplay, 8249 | oshalt | 26% | 70 / 0 / 0 / 0 % | 78% | OS_Halt pc 02058ad4 lr 02056b94 (100%) | 0/0/0 |
| Sonic Rush Adventure (USA) (En,Ja,Fr,De,Es,It) | A3YE | 03027531 | 14807 | menus (generic), 2499 | oshalt | 40% | 59 / 0 / 0 / 0 % | 66% | OS_Halt pc 020e6e44 lr 020e4548 (100%) | 0/0/0 |
| Sonic Colors (USA) (En,Ja,Fr,De,Es,It) | BXSE | 05047531 | 14808 | gameplay, 3749 | oshalt | 34% | 64 / 0 / 0 / 0 % | 76% | OS_Halt pc 020dc9fc lr 020dad20 (100%) | 0/0/0 |
| Pokemon - SoulSilver Version (USA, Australia) | IPGE | 04027533 | 7212 | gameplay, 8249 | oshalt | 14% | 86 / 0 / 0 / 0 % | 87% | OS_Halt pc 020d3f5c lr 020d21f8 (100%) | 0/0/0 |
| Spore Creatures (USA) (En,Fr,De,Es,It,Nl) | YSPE | 04017533 | 15401 | menus (generic), 2499 | oshalt | 17% | 83 / 0 / 0 / 0 % | 86% | OS_Halt pc 020b6f38 lr 020b555c (100%) | 0/0/0 |
| Super Scribblenauts (USA) (En,Fr,Es,Pt) | BH2E | 05047531 | 16383 | menus (generic), 2499 | oshalt | 23% | 77 / 0 / 0 / 0 % | 74% | OS_Halt pc 020f1468 lr 020ef798 (100%) | 0/0/0 |
| Tetris DS (USA, Australia) | ATRE | 03004f4d | 9878 | gameplay, 2999 | oshalt | 12% | 87 / 0 / 0 / 0 % | 93% | OS_Halt pc 0200805c lr 02005be4 (100%) | 0/0/0 |
| Trauma Center - Under the Knife (USA) | AKDE | 02017532 | 6907 | menus (generic), 2499 | oshalt | 36% | 64 / 0 / 0 / 0 % | 85% | OS_Halt pc 02007b24 lr 020062ac (100%) | 0/4/0 |
| Trauma Center - Under the Knife 2 (USA) | YYKE | 04027531 | 1271 | menus (generic), 2499 | oshalt | 41% | 59 / 0 / 0 / 0 % | 39% | OS_Halt pc 02030e14 lr 0202fbc7 (100%) | 0/0/0 |
| World Ends with You, The (USA) | AWLE | 04017532 | 4887 | gameplay, 8249 | oshalt | 14% | 86 / 0 / 0 / 0 % | 95% | OS_Halt pc 0203aae4 lr 02039208 (100%) | 0/0/0 |
| WarioWare - D.I.Y. (USA) | UORE | 04027539 | 14816 | menus (generic), 2499 | oshalt | 29% | 69 / 0 / 0 / 0 % | 73% | OS_Halt pc 02094ed4 lr 020937ac (100%) | 0/0/0 |
| WarioWare - Touched! (USA) | AZWE | 02004eeb | 1199 | menus (generic), 2499 | oshalt | 19% | 69 / 0 / 0 / 0 % | 90% | OS_Halt pc 02079f00 lr 02078394 (100%) | 0/2/0 |
| Yoshi's Island DS (USA) (Rev 1) | AYWE | 03017534 | 14786 | menus (generic), 2499 | oshalt | 18% | 82 / 0 / 0 / 0 % | 85% | OS_Halt pc 020053cc lr 02003798 (100%) | 0/0/0 |
| Legend of Zelda, The - Phantom Hourglass (USA) (En,Fr,Es) | AZEE | 04007530 | 14323 | gameplay, 1499 | oshalt | 27% | 72 / 0 / 0 / 0 % | 79% | OS_Halt pc 0200f25c lr 0200dc24 (100%) | 0/0/0 |
| Legend of Zelda, The - Spirit Tracks (USA, Australia) (En,Fr,Es) | BKIE | 04027539 | 9898 | gameplay, 7499 | oshalt | 22% | 76 / 0 / 0 / 0 % | 88% | OS_Halt pc 02028508 lr 0202701c (100%) | 0/0/0 |
