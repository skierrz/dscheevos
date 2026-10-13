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
| `NOTICE_MODE` | 1 | Hardcore / Softcore notice at session start (the pre-boot placard is separate) |
| `NOTICE_NETWORK` | 1 | Offline / back online |
| `NOTICE_BADGES` | 1 | Badge images in notices (when the pack has them); 0 = icons |
| `NOTICE_SCREEN` | `auto` | `auto` / `top` / `bottom`: which screen (`display-library.md` 11.2) |
| `NOTICE_EDGE` | `bottom` | `bottom` / `top`. Top only where a mechanism allows it |
| `NOTICE_SIDE` | `left` | `left` / `right`: the layout's side |
| `NOTICE_SIZE` | `auto` | `auto` / `full` (32 lines) / `compact` (16 lines) |

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

## Hardcore and RA compliance

Every switch only hides or shows information. None changes evaluation, sending or the hardcore state, and RetroArch (approved) offers the same switches. One open point is for ra-compliance:
- the strict reading says Measured and Trigger indicators must show during play;
- if RA requires them in hardcore, `NOTICE_PROGRESS` and `NOTICE_CHALLENGE` would be forced on in hardcore, shown as such, the way `SEND_DURING_PLAY` is today.

Until ra-compliance answers, the design allows switching them off in both modes, as RetroArch does.

## Where it shows up later (UI/UX pass)

The launcher's settings menu lists every key with its current value. `SPECTATOR` stays ini-only. The in-game menu shows the current switches read-only; changing them in-session is not planned.
