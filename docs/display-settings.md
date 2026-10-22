# Settings model: one file, every display toggle

Status: design only, 2026-10-12. Owner ruling #5: every notice type, LED cue, badge download and overlay element can be switched on or off individually, RetroArch-style. Nothing here is built. The launcher files it touches (`config.c`, `session.c`, `main.c`) are being edited by the lockdown agent, so the orchestrator sequences the build.

## One file, read once, before boot

- **File:** `sd:/_nds/dscheevos/dscheevos.ini`, the existing settings file. The keys that exist today stay as they are: `ENABLED`, `HARDCORE`, `SEND_DURING_PLAY`, `USERNAME`, `TOKEN`, `SPECTATOR`, `HOST`.
- **Who reads it:** only the launcher, before boot. The module can't read SD and must not during play. The launcher copies the display settings into the session it writes (`session.bin`), where the module reads them.
- **The ARM7 card engine (LED cues) never needs the settings.** The module drops a cue the user switched off before publishing it, so nds-bootstrap is unchanged.
- **Unknown keys** are ignored. A missing key takes its documented default, so old files keep working and new keys need no migration.
- **Every key is unique across the file.** Section headers are for readability only; today's parser ignores them, which is fine.
- **Values:** `0` or `1` for switches; a lower-case word for choices. Anything else falls back to the default.

## The keys

### Notices on screen (the overlay)

| Key | Default | What it switches |
|---|---|---|
| `NOTICES` | 1 | Master switch for everything drawn during play. 0 = LEDs and the menu only |
| `NOTICE_UNLOCK` | 1 | Achievement unlocked |
| `NOTICE_MASTERY` | 1 | Set mastered |
| `NOTICE_PROGRESS` | 1 | Measured progress updates |
| `NOTICE_CHALLENGE` | 1 | Challenge indicators (the tray) |
| `NOTICE_LB_START` | 1 | Leaderboard attempt started |
| `NOTICE_LB_RESULT` | 1 | Leaderboard failed / submitted / rank |
| `NOTICE_LB_TRACKER` | 1 | Leaderboard tracker in the tray |
| `NOTICE_MODE` | 1 | Hardcore / Casual notice at session start (the pre-boot placard is separate). In hardcore it can't be off together with `PLACARD`; see below |
| `NOTICE_NETWORK` | 1 | Offline / back online |
| `NOTICE_BADGES` | 1 | Badge images in notices (when the pack has them); 0 = icons |
| `NOTICE_SCREEN` | `auto` | `auto` / `top` / `bottom`: which screen (`display-library.md` 11.2) |
| `NOTICE_EDGE` | `auto` | `auto` / `top` / `bottom`: the half of the game's screen the bar may use. The module learns the best spot within it (`docs/strip-lab.md`); auto prefers the bottom. Built on branch `strip-lab` (launcher reads it, passes it in the session flags) |
| `NOTICE_SIDE` | `left` | `left` / `right`: the layout's side |
| `NOTICE_SIZE` | `thin` | `thin` (16 lines: icon or 16x16 badge, one line of text) / `big` (32 lines: 32x32 badge, title and description). Owner 2026-10-22: thin by default, players choose. Built on branch `strip-lab` (session flag `DSCM_BOOT_BIG_BAR`) |
| `BADGES` | `1` | Fetch the set's badges before boot for the bar (16x16 or 32x32 to match `NOTICE_SIZE`). Built on `strip-lab` |

### Before the game

| Key | Default | What it switches |
|---|---|---|
| `PLACARD` | 1 | The pre-boot card with the title, the mode and the totals |

### LEDs

| Key | Default | What it switches |
|---|---|---|
| `LED_STYLE` | `flash` | `flash` (lit for events) / `dark` (today's inverse blinks) / `off` (no LED cues at all) |
| `LED_UNLOCK` | 1 | Blinks per unlock |
| `LED_PROGRESS` | 1 | A blink per progress update |
| `LED_LEADERBOARD` | 1 | Blinks for leaderboard start/fail/submit |
| `LED_CHALLENGE` | 1 | Held while a challenge is active |
| `LED_CAMERA` | 0 | Camera LED cues (off by default; it faces outward, owner to revisit). Details follow the LED research |

### Badges (launcher, before boot)

Privacy: the download goes to `media.retroachievements.org`, a plain GET of `/Badge/<badge_name>.png` with the launcher's user agent. It carries no username, token or other account data. The cache is `sd:/_nds/dscheevos/badges/<game_id>.pak`. ra-compliance adds both to `docs/privacy.md` when this is built.

| Key | Default | What it switches |
|---|---|---|
| `BADGES` | 1 | Download and cache badge images before boot. 0 = never download (notices use icons) |
| `BADGES_LARGE` | 0 | Also the 64x64 badges (`display-badges.md`) |

## In the session (an ABI change, coordinate with the v7 memory plan)

The launcher packs the display settings into the session's boot block: 8 bytes, a new field of `dscm_boot_t`.

```c
typedef struct dscm_display_settings_t {
  uint32_t notice_flags;   /* bit per NOTICE_* switch, NOTICES as bit 0 */
  uint16_t led_flags;      /* bit per LED_* switch, LED_STYLE in bits 8-9 */
  uint8_t  placement;      /* screen (2 bits) | edge (1) | side (1) | size (2) */
  uint8_t  misc;           /* NOTICE_BADGES, PLACARD-was-shown */
} dscm_display_settings_t;
```

The module applies the switches where each event is produced:
- the display scheduler drops switched-off kinds (`dsd_config_t` gets the flags);
- the LED cue publisher skips switched-off cues.

The pre-boot placard and badge downloads are launcher-only.

## Hardcore and RA compliance (reviewed by ra-compliance, 2026-10-12; `docs/compliance.md` "Notice settings")

Every switch only hides or shows information. None changes evaluation, sending or the hardcore state, and RetroArch (approved) offers the same switches. The rules:

1. **`NOTICE_PROGRESS` and `NOTICE_CHALLENGE` may be off in hardcore too.** RetroArch gates its challenge indicators and progress tracker with no hardcore check. Both default to 1, and resetting to defaults turns them back on. If RA objects, the fallback is to force them on in hardcore, shown as such.
2. **The mode indicator is required in hardcore** (RA section E). If a player turns off both `PLACARD` and `NOTICE_MODE`:
   - in hardcore, the session-start mode notice still shows, and the settings page says so;
   - in a Casual session, both may be off.
3. **The in-game menu always shows active challenges and measured progress** on its achievement pages, whatever the `NOTICE_*` switches say. RA requires them "visible in the achievement list", and that can't be switched off.
4. **Wording:** RA now calls the non-hardcore mode **Casual**. Notices and settings say Hardcore / Casual; the lockdown branch has already renamed the launcher's strings.

## Where it shows up later (UI/UX pass)

The launcher's settings menu lists every key with its current value. `SPECTATOR` stays ini-only. The in-game menu shows the current switches read-only; changing them in-session is not planned.
