DScheevos - RetroAchievements for DS games on a DSi
===================================================

DScheevos earns RetroAchievements while you play your own DS games on a DSi
(or a 3DS in DSi mode), through TWiLight Menu++ and nds-bootstrap. Games run
on the console itself, not in an emulator.

THIS IS A TEST BUILD. Read "What works" below before you rely on it.


What you need
-------------
- A DSi with TWiLight Menu++ installed and working on the SD card
  (games already start from it through nds-bootstrap).
- Wi-Fi settings saved on the console (DSi System Settings > Internet).
  WPA2 networks work.
- A RetroAchievements account (https://retroachievements.org).
- Your own game dumps. RetroAchievements must recognise the exact dump:
  check the game's "Supported Game Files" page on the website.


Install
-------
1. Copy everything inside the "sd" folder to the root of your SD card,
   merging folders. This replaces _nds/nds-bootstrap-release.nds and
   _nds/nds-bootstrap-nightly.nds with the DScheevos launcher. The DScheevos
   build of nds-bootstrap goes to _nds/dscheevos/nds-bootstrap.nds.
2. Start a DS game from TWiLight Menu++ as usual.
3. The first time, log in with your RetroAchievements username and
   password (touch keyboard). Only the login token is saved, in
   _nds/dscheevos/dscheevos.ini. Your password is never stored.

To uninstall, put back the nds-bootstrap files from TWiLight Menu++'s
release and delete _nds/dscheevos.


Playing
-------
- Each time a game starts, DScheevos goes online, starts a session and
  shows your progress, then the game boots.
- If the console can't get online, you can try again or play without
  achievements. Hardcore needs an online start.
- When you unlock an achievement, the Wi-Fi light blinks three times.
- In-game menu (L + DOWN + SELECT) > "Achievements..." shows the game,
  hardcore or softcore, your progress and this session's unlocks.
- Settings: press A on the game in TWiLight Menu++, then hold SELECT.
  You can turn achievements off, switch hardcore on or off, or log out.

Hardcore (on by default) follows RetroAchievements' rules. RetroAchievements
hasn't approved DScheevos yet, so it counts your hardcore unlocks as softcore.
The launcher and the in-game menu say so ("Rules on, RA counts as softcore"). While a
hardcore session runs, nds-bootstrap's cheats, RAM viewer, RAM dump and
the slow-motion refresh rates are off. RAM dump is off in softcore too,
since the dump would hold your login token.


What works and what doesn't yet
-------------------------------
- DS games in DS mode, started from the SD card. DSi-enhanced games get
  achievements only when set to run in DS mode (TWiLight Menu++ game
  settings). Flashcards aren't supported.
- Achievements are checked every frame while you play.
- Unlocks are sent to RetroAchievements during play, over the same
  Wi-Fi connection the session started on. The in-game menu shows
  "Online", how many were sent and how many are waiting.
- While a game uses its own wireless (Download Play, multiplayer),
  sending pauses and resumes afterwards.
- Unlocks still waiting are saved to the SD card (pending.bin) only at
  safe points: when you open the in-game menu, and before Reset Game,
  Exit or a soft reset. The launcher sends them at the next start.
  Turning the console off without one of those loses what was waiting.
- If the Wi-Fi drops during play, DScheevos joins it again by itself
  and sends what was waiting. On WPA2 networks it uses the network key
  the console already saved; the key never goes to the SD card. WEP
  networks don't rejoin. The in-game menu counts the rejoins.
- This build hasn't been tested on a real DSi yet. Treat it as a test
  build until it has.
- The game must give the console a little idle time each frame. In
  heavy scenes evaluation can fall behind and catch up later. The
  in-game menu shows "Frames skipped" if it ever had to skip one.


Files
-----
_nds/nds-bootstrap-release.nds   DScheevos launcher (session start, login)
_nds/nds-bootstrap-nightly.nds   the same launcher
_nds/dscheevos/nds-bootstrap.nds nds-bootstrap with DScheevos hooks
_nds/dscheevos/dscheevos9.bin    the achievement module (runs in the game)
_nds/dscheevos/load.bin          devkitPro's nds-bootloader
_nds/dscheevos/privacy.txt       privacy policy (also next to this file)
_nds/dscheevos/licenses/         licenses and source links


Cost and privacy
----------------
DScheevos is free: no paid features, no ads, no donations required.
It talks only to RetroAchievements and collects nothing for itself.
privacy.txt says what is sent, what is kept on the SD card, and how to
delete it.
