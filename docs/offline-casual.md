# Offline casual starts: spec

Status: **spec, pending the owner's yes** (2026-10-12). Docs only. Background: the owner wants casual to be first-class, "essentially RAproxy with offline achievements" (`docs/compliance.md`, "Casual (softcore)").

## Goal and limits

Start a game with achievements when the console can't reach RetroAchievements, in **casual only**, using what the launcher fetched the last time this exact ROM started online. Unlocks are kept and sent at the next online start with their real times. Hardcore never starts offline.

RA's rule this must meet (section A): "Unlocks created while offline must be securely cached and sync to RetroAchievements when connectivity returns." Nothing in RA's list forbids starting offline. rcheevos stays unmodified: we replay real server replies, never write our own.

## How rc_client is fed

Today the launcher talks to the server and stores three real replies in the session file (`DSCM_PRELOAD_LOGIN`, `_SET`, `_SESSION`). The module answers rc_client's `login2`, `achievementsets` and `startsession` requests from them (`module/src/module.c` `http_send`). rc_client aborts a load if `startsession` fails (`rc_client_start_session_callback` calls `rc_client_load_error`), so an offline start must replay a `startsession` reply too. The plan: keep the last real reply of each kind, per ROM hash, and replay those.

## SD cache

- **Where:** `sd:/_nds/dscheevos/cache/<md5>.bin`, one file per ROM hash (the rcheevos DS hash, lowercase hex).
- **Written:** by the launcher, before boot only (never during play), each time an online start succeeds for that hash. Written to a temp name, then renamed, so a power cut can't leave a half file.
- **Format:**

  | Field | Size | Meaning |
  |---|---|---|
  | magic `DSCC`, version | 4 + 4 | format check |
  | ROM hash | 33 | must equal the file name and the ROM being started |
  | game id | 4 | from the set reply |
  | username | 32 | the account the replies belong to |
  | fetched at | 8 | Unix time of the online start (console clock, corrected to UTC) |
  | DScheevos version, rcheevos version | 16 + 16 | a cache from another build is ignored |
  | request flags | 4 | whether `f=3` was used (the "Skip unofficial achievements" setting) |
  | offsets and lengths of the 3 replies | 24 | as `dscm_boot_t` |
  | replies | variable | `login2`, `achievementsets`, `startsession`, byte for byte as received, NUL-padded |

  The `login2` reply holds the login token, like `session.used` today. `docs/privacy.md` must list the cache folder: what it holds, that it's kept until the expiry below, and that deleting it is safe.
- **Size:** about the set reply (up to 256 KB, which the session area already caps) plus a few KB. Keep at most 32 files and delete the least recently used beyond that.
- **Expiry:** 30 days after "fetched at". An older cache isn't offered (the set may have changed on the site), and it is deleted at the next launcher start. A cache for another username is never used.

## Online or offline: the launcher's decision

1. As today: connect Wi-Fi, log in, fetch the set, start the session. Any success means an online session, as now, and refreshes the cache.
2. If the failure is network-shaped (no Wi-Fi, no DNS, TLS failure, timeout, HTTP 5xx or a Cloudflare page): check for a usable cache (right hash, right user, same versions, not expired).
   - **Cache found, casual setting:** offer "Play offline (casual)" with "Try again" and "Play without achievements". Never choose for the player.
   - **Cache found, hardcore setting:** offer "Play offline in casual (hardcore needs the internet)" with the same two others. Choosing it makes this session casual; the saved setting stays hardcore.
   - **No cache:** as today ("Try again" / "Play without achievements").
3. If the server answered and refused the login (invalid or expired token): no offline option. The player must log in again (the token in the cache is the same one).
4. If the server says the hash has no set: no offline option.

## In the game

- The session file is built from the cache exactly as from live replies, with mode casual and a new flag, `DSCM_BOOT_OFFLINE`.
- **Network:** the module doesn't bring Wi-Fi up (nothing to send). rc_client's award requests fail and are retried in the background as on any network loss; rich presence pings fail and are dropped, which B allows in casual only.
- **Already unlocked:** the cached `startsession` reply lists unlocks up to the cached start. Achievements unlocked after that (in later sessions) are evaluated again and can fire again. When they do, the module checks the journal and the `unsent/` copies for this hash. A repeat is shown as "already unlocked" in the menu and not counted twice. When sent, the server's "already unlocked" answer is treated as sent (`tests/real` step 6 showed this answer).
- **Journal:** as now: every entry with `hardcore = 0`, written at safe points, sent by the launcher at the next online start with `o=` (the time since unlock). **Blocker:** the console clock is read as UTC while it holds local time ("Open" in `docs/compliance.md`). That must be fixed before offline starts ship, or every offline unlock carries a time hours off.
- **Clock trust:** award times come from the console clock, which the player can set. In casual, a wrong time misdates an unlock and gains nothing. The launcher clamps `o=` to the age of the journal file's first entry, and never sends a time in the future.

## What the player sees

- **Launcher:** "Offline: casual, achievements from <date>" in the status line, and the totals from the cached replies.
- **Placard** (before the game's first frame): "Casual · Offline · set from <date>".
- **In-game menu summary:** "Offline: N unlocks will be sent next time you're online".
- **Next online start:** "Sent N offline unlocks", as now.

## Not changing

- Hardcore stays online-only at start. A hardcore session that loses the network mid-play is unchanged: it stays hardcore and journals.
- No achievement logic is cached in any form other than the server's own reply.

## Tests (melonDS, mock server)

1. Online start (fills the cache), then a start with the mock server down: the offline option appears, the game runs, an unlock is journalled with `hardcore = 0`. The next online start sends it with `o=` > 0. Check the `events.jsonl` timestamps.
2. Cache older than 30 days (`--rtc` set ahead): no offline option, and the cache is deleted.
3. A different user's cache: not offered.
4. Hardcore setting with a cache: the offer says casual, and the session is casual (status `hardcore` 0).
5. Login refused (mock `--fail-login`, if needed): no offline option.
6. An achievement unlocked online after the cached start fires again offline: one journal entry, shown as already unlocked, the server answers "already unlocked", nothing counted twice.
7. Power off mid-session: the cache is untouched, and only the journal's safe-point rules apply.
