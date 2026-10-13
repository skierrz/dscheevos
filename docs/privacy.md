# DScheevos privacy policy

Applies to DScheevos 0.1.0 and later: the launcher, the achievement module and the DScheevos build of nds-bootstrap.

DScheevos is free. It has no accounts, servers, analytics, ads or tracking of its own, and it collects nothing for itself. Its only job is to talk to RetroAchievements for you, and everything it sends goes there.

## What is sent, and where

Everything goes to RetroAchievements (`retroachievements.org`) over HTTPS. Nothing is sent to anyone else.

- **Logging in.** The first time, the launcher asks for your RetroAchievements username and password and sends them to RetroAchievements once, to log in. The password is never stored. RetroAchievements answers with a login token, which DScheevos uses from then on instead of the password.
- **While you play.** Your username and login token, the RetroAchievements hash of the game (a fingerprint of the game file, used to find its achievement set), the achievements you unlock, your leaderboard entries, and rich presence (a short line such as the level you're on, sent every 2 minutes).
- **User agent.** Every request names the client and its versions: `DScheevos/<version> (Nintendo DSi) rcheevos/<version>`. RetroAchievements requires this to tell clients apart.

In spectator mode (`SPECTATOR = 1` in `dscheevos.ini`), unlocks and leaderboard entries are shown but not sent.

If you set `HOST` in `dscheevos.ini` to another server (for example a test server), the same data goes to that server instead. By default it is `https://retroachievements.org`.

What RetroAchievements does with this data, including what it shows on your public profile, is up to RetroAchievements and covered by their own terms and privacy policy: <https://retroachievements.org/terms>. RetroAchievements' servers may be reached through the network providers they use, such as their content delivery network.

## Wi-Fi

DScheevos uses the Wi-Fi settings already saved on the console (System Settings > Internet). If the connection drops during play, it joins the same network again. For WPA2 networks it reads the network key into the console's memory for that; the key is never written to the SD card and never sent anywhere. DScheevos never changes the console's saved settings.

Like any network program, the console's connection is visible to your Wi-Fi access point and goes through your internet provider. Server names are looked up with the DNS server your network gives the console.

## What is stored on the SD card

All in `sd:/_nds/dscheevos/`:

| File | What it holds |
|---|---|
| `dscheevos.ini` | your username, the login token, and your settings. Never your password. |
| `session.bin`, then `session.used` | the server's replies for the game you started: its achievement set, the login token and the network addresses the console got from your Wi-Fi network. No Wi-Fi key. Rewritten each time a game starts. |
| `pending.bin` | unlocks and leaderboard entries not yet sent (username, game hash, achievement or leaderboard, score, time). Written only at safe points: when you open the in-game menu, and before Reset Game, Exit or a soft reset. The launcher sends them at the next start and then empties the file. |
| `unsent/000.bin` to `unsent/999.bin` | copies of `pending.bin` that couldn't be sent yet: when sending failed, when a spectator session started (what was waiting is set aside for a normal session), or when they belong to another user who logged in on this console. Each holds the same data as `pending.bin`. The launcher sends a file when its user is logged in and then deletes it; until then, including another user's, it is kept. |
| `pending.spectator` | an empty marker: the last session was in spectator mode, so its `pending.bin` is emptied without being sent. Deleted at the next start. |
| `launcher.log`, if present | a log of what the launcher did, for troubleshooting. No password, token or Wi-Fi key. |

The folder also holds the program files and, if you added one, `ca.pem` (an extra trusted certificate for a test server). Nothing else is stored, and nothing is written to the console's internal memory (NAND), firmware or Wi-Fi settings.

## How long it is kept, and where

- **On the console:** everything DScheevos keeps is in the files above, on your SD card, until it is replaced or deleted. `session.bin`/`session.used` are rewritten at every game start. `pending.bin` is emptied once its entries are sent. Files in `unsent/` are kept until they are sent (when their user is logged in) or until you delete them; there is no time limit. The Wi-Fi key is kept in the console's memory only while the game runs.
- **DScheevos itself** runs no servers and keeps no data anywhere else: nothing is sent to the DScheevos project, and there is no telemetry.
- **RetroAchievements** keeps what you send it on its own servers; where they are and how long data is kept is covered by RetroAchievements' privacy policy and terms (<https://retroachievements.org/terms>).

## GDPR

DScheevos collects no personal data for itself and keeps no telemetry, so the DScheevos project holds nothing about you to access, correct or erase. The data on your SD card is under your control (see below). For data held by RetroAchievements, including requests under the GDPR, contact RetroAchievements as their privacy policy describes.

## Deleting your data

- **Log out:** in the DScheevos settings (press A on the game in TWiLight Menu++, then hold SELECT), press Y. This removes your username and login token from `dscheevos.ini`. The last game's `session.used` keeps a copy of the token, and `pending.bin` and the `unsent` folder keep your username and the unlocks not yet sent, until you delete them. Deleting `pending.bin` and the `unsent` folder throws those unlocks away.
- **Remove everything:** delete the `_nds/dscheevos` folder from the SD card (this also removes DScheevos itself).
- **On RetroAchievements:** your account and the unlocks it holds are managed on retroachievements.org.

## Contact

Questions about this policy or DScheevos: open an issue at <https://github.com/skierrz/dscheevos/issues>.

## Changes

Changes to this policy are made in the DScheevos repository, where `docs/privacy.md` keeps the full history. Each release zip includes the policy that applies to it (`privacy.txt`).
