# RA client display: events, badges, fonts (facts only)

Sources read 2026-10-12. Emulator sources fetched from GitHub master (raw files); pages longer than ~100 KB were read only to char 100000 (DuckStation, PCSX2, PPSSPP, Dolphin handlers were all inside that window). Items marked [not in file] were not found in the fetched source.

## 1. Events

### 1.1 What RA documents
- Hardcore Compliance Requirements (docs.retroachievements.org/general/hardcore-compliance-requirements.html):
  - "Hardcore state must be visibly indicated in the UI during play."
  - "Measured and Trigger flags must work properly. Both must be visible in the achievement list as well as displayed during gameplay when appropriate conditions are met."
  - "User must be able to conveniently access the list of achievements within the emulator."
  - "Rich Presence and Leaderboards cannot be disabled in hardcore." / "Disabling leaderboard popups is okay to support for either mode."
  - The page has no text on unlock notifications, badges or sounds.
- rc_client integration wiki (github.com/RetroAchievements/rcheevos/wiki/rc_client-integration), event guidance:
  - Unlock: popup with badge and title; unofficial gets a different heading; audio cue suggested.
  - Leaderboard started/failed/submitted: "The events will always be raised, it's up to you to decide how to handle them."
  - Tracker show/update/hide: widget keyed by tracker id; "fixed-width font to eliminate jittering".
  - Challenge indicator: small unlocked icon; "Multiple challenge indicators may be shown, but only one per achievement."
  - Progress indicator: small popup with locked icon and progress text; "Only one progress indicator should be shown at a time."
  - Mastery: title "Mastered" (hardcore) / "Completed"; suggests display name, playtime, sound. Subset completed: same layout with subset title/badge.
  - Server error: "should be reported to the player, as the request will not be requeued."
  - Reset: reset the emulator.
  - Badges: no sizes or URL formats given; "The emulator is responsible for managing images." (wiki read to char 100000 of 124478.)

### 1.2 rc_client event types (external/rcheevos/include/rc_client.h lines 844-863)
1 ACHIEVEMENT_TRIGGERED, 2 LEADERBOARD_STARTED, 3 LEADERBOARD_FAILED, 4 LEADERBOARD_SUBMITTED, 5/6 ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW/HIDE, 7/8/9 ACHIEVEMENT_PROGRESS_INDICATOR_SHOW/HIDE/UPDATE, 10/11/12 LEADERBOARD_TRACKER_SHOW/HIDE/UPDATE, 13 LEADERBOARD_SCOREBOARD, 14 RESET, 15 GAME_COMPLETED, 16 SERVER_ERROR, 17 DISCONNECTED, 18 RECONNECTED, 19 SUBSET_COMPLETED.
Data sizes: leaderboard tracker `display[RC_CLIENT_LEADERBOARD_DISPLAY_SIZE]` = 24 bytes (line 669). Achievement `badge_name[8]` (line 561); `badge_url` / `badge_locked_url` pointers (576-577).
DScheevos notice kinds (dscheevos_module.h): SESSION_START, UNLOCK, MASTERY, PROGRESS, LEADERBOARD_STARTED/FAILED/SUBMITTED/RANK, OFFLINE, ONLINE; ring DSCM_NOTICE_RING = 16 x 116 B = 1856 B; indicators DSCM_INDICATOR_MAX = 8 challenges + 8 trackers, each 28 B (text[24]) = 448 B. dscm_notice_t text[48], detail[48]. List entry (dscm_list_entry_t) 176 B: title[48], description[100], measured[16].

### 1.3 Per-emulator behavior

**RetroArch** (cheevos/cheevos.c; gfx/widgets/gfx_widget_achievement_popup.c; gfx/widgets/gfx_widget_leaderboard_display.c). Durations in cheevos.c are "N x 60" passed to runloop_msg_queue_push (unit defined elsewhere).
| Event | Display | Duration | Sound | Setting that disables |
|---|---|---|---|---|
| Triggered | Widget popup: title + subtitle "title (points)" (title line "%s - %0.2f%%" when rarity >= 10); non-widget path: message then description | widget: CHEEVO_NOTIFICATION_DURATION 4000 ms (+ MSG_QUEUE_ANIMATION_DURATION hold); message path 2x60, description 3x60 | AUDIO_MIXER_SYSTEM_SLOT_ACHIEVEMENT_UNLOCK if cheevos_unlock_sound_enable (plays even when visibility is off) | cheevos_visibility_unlock |
| Lboard started | message "Leaderboard started: title - description" | 2x60 | none | cheevos_visibility_lboard_start |
| Lboard failed | message | 2x60 | none | cheevos_visibility_lboard_cancel |
| Lboard submitted | none (silent) | - | - | - |
| Scoreboard | message: submission + score + "(rank)" or "(best)" | 2x60 | none | cheevos_visibility_lboard_submit |
| Tracker show/update/hide | widget, keyed by id | while active | none | cheevos_visibility_lboard_trackers |
| Challenge show/hide | widget icon row | while active | none | cheevos_challenge_indicators (show only) |
| Progress show/update/hide | widget, one entry | CHEEVO_PROGRESS_TRACKER_DURATION 2000 ms unless updated | none | cheevos_visibility_progress_tracker |
| Game/subset completed | placard; "Mastered" hardcore, "Completed" otherwise; optional runtime appended | 3x60 (message path) | none | cheevos_visibility_mastery |
| Server error | "<api> failed: <message>" | 4x60 | none | none seen |
| Disconnected / reconnected | message + widget "! RA !" flag | 3x60 | none | none seen |
- Popup queue: CHEEVO_QUEUE_SIZE = 8, one popup on screen at a time, a popup arriving at a full queue is dropped. Animation: slide in, unfold, hold, fold, slide out (EASING_OUT_QUAD). Popup is a square badge area of height = line_height x 4 plus text backdrop; anchor set by cheevos_appearance_anchor (top/bottom x left/center/right) with padding settings.
- Widget stack (leaderboard_display.c): bottom-right, bottom to top: trackers, challenge row, progress, disconnect box. Limits: CHEEVO_LBOARD_ARRAY_SIZE 4 trackers, CHEEVO_CHALLENGE_ARRAY_SIZE 8 challenge icons, 1 progress. Icon size = MIN(w,h)/16; spacing MIN(w,h)/64. Digits and ",-./0-9:" drawn in fixed-width cells. Badge not loaded yet: held up to CHEEVO_BADGE_HOLD_FRAMES = 30 frames then placeholder. Progress tracker keeps 4 recent badges.
- Unlock retry when badge not cached: 100 ms start, doubling (200/400/800 ms), ~1500 ms total, 4 retries.
- Locked badge local name "%s_lock"; game icons prefixed 'i'. Badge download: first pass 5 min after load, 10 s apart.

**DuckStation** (src/core/achievements.cpp):
- Unlock: shown if achievements_notifications; title = achievement title, body = description, note = trophy + points; duration = setting achievements_notification_duration; sound sounds/achievements/unlock.wav if achievements_sound_effects.
- Game complete / subset complete: key achievement_mastery, GAME_COMPLETE_NOTIFICATION_TIME = 20.0 s, no sound.
- Lboard started: "Leaderboard attempt started.", LEADERBOARD_STARTED_NOTIFICATION_TIME 3.0 s. Failed: 3.0 s. Submitted: "Your Time/Score/Value: {}", duration achievements_leaderboard_duration, min width 380, sound lbsubmit.wav. Scoreboard: submitted and best score + "Leaderboard Position: {0} of {1}", achievements_leaderboard_duration. All gated by achievements_leaderboard_notifications.
- Challenge show: mode Notification gives "Challenge Started: {}" 5.0 s (CHALLENGE_STARTED_NOTIFICATION_TIME); hide gives "Challenge Failed: {}" 5.0 s; otherwise an indicator list (fade in 0.1 s, fade out 0.5 s: INDICATOR_FADE_IN_TIME/OUT_TIME).
- Progress: single optional instance, new show replaces; mode Disabled clears. Trackers: achievements_leaderboard_trackers; list without a cap in that file.
- Server error / disconnected / reconnected: OSD messages (keys AchievementsDisconnected; reconnect replaces disconnect); no settings gate seen.
- Badge sizes in px: [not in file]. Badge prefetch: achievements_prefetch_badges (all locked badges) else next likely locked badge.

**PCSX2** (pcsx2/Achievements.cpp):
- Unlock: EmuConfig.Achievements.Notifications; title + " (Unofficial)" if unofficial; summary = description; duration NotificationsDuration; key achievement_unlock_{id}; sound UnlockSound (default sounds/achievements/unlock.wav).
- Mastery: "Mastered {game}", GAME_COMPLETE_NOTIFICATION_TIME 20.0 s, no sound. Subset: key achievement_subset_mastery, 20.0 s.
- Lboard started 3.0 s, failed 3.0 s (keys leaderboard_{id}); submitted and scoreboard: LeaderboardsDuration; submitted sound LBSubmitSound outside the notification check. Gate: LeaderboardNotifications.
- Server error: OSD for Host::OSD_ERROR_DURATION; disconnect (key achievements_disconnect) Host::OSD_ERROR_DURATION; reconnect Host::OSD_INFO_DURATION.
- Overlays (DrawGameOverlays): trackers (setting LBOverlays) each a box with stopwatch icon and right-aligned text, stacked by OverlayPosition; challenge indicators (setting Overlays) square badges of LAYOUT_MENU_BUTTON_HEIGHT, repeat show resets timer; one progress indicator (std::optional), new show replaces. Fade constants 0.1 s / 0.5 s. No max counts in the file.
- Notification position: top (0.15, stacks down), center (0.5), bottom (0.85, stacks up). Pause menu lists active challenges (width up to 300 scaled, titles truncated with "...").
- Sound toggles: SoundEffects, InfoSound, UnlockSound, LBSubmitSound.

**PPSSPP** (Core/RetroAchievements.cpp, same rc_client switch):
- Unlock: g_OSD.ShowAchievementUnlocked(id), sound "achievement_unlocked"; duration inside OSD [not in file].
- Game/subset completed: g_OSD.Show(MESSAGE_INFO...), 10.0 s, sound "achievement_unlocked", "Mastered %1"/"Completed %1", body "%1 achievements, %2 points".
- Lboard started/failed: ShowLeaderboardStartEnd, "%1: Attempt started"/"%1: Attempt failed", no sound. Submitted: log only. Scoreboard: ShowLeaderboardSubmitted, "Rank: %1" or "Best: %1", sound "leaderboard_submitted".
- Challenge: ShowChallengeIndicator(id, bool), ids kept in g_activeChallenges. Progress: ShowAchievementProgress; code comment says two seconds, only one at a time, updates restart the timer. Tracker: ShowLeaderboardTracker(id, display, show).
- Server error: MESSAGE_ERROR "Server error" with RA logo. No DISCONNECTED case (unknown events are logged and ignored). Load-time connection failure: MESSAGE_ERROR 6.0 s.
- No config option gates any notification in this file.

**Dolphin** (Source/Core/Core/AchievementManager.cpp), OSD::Duration constants (numeric values defined elsewhere):
- Unlock: "Unlocked: {title} ({points})" with badge, VERY_LONG, YELLOW hardcore / CYAN softcore. No sound in file.
- Lboard started "Attempting leaderboard: {title} - {description}" (GREEN), failed "Failed leaderboard: {title}" (RED), submitted "Scored {tracker_value} on leaderboard: {title}" (YELLOW), all VERY_LONG, gated by RA_LEADERBOARD_TRACKER_ENABLED.
- Trackers/challenges: no OSD; stored in m_active_leaderboards (display count capped by MAX_DISPLAYED_LBOARDS) and m_active_challenges (also feeds rich presence).
- Progress: "{title} {measured_progress}" with badge, SHORT, GREEN, rate-limited to one per SHORT ms, gated by RA_PROGRESS_ENABLED; hide is a no-op.
- Game completed: "Congratulations, {display_name}! You have mastered|completed {title}", VERY_LONG. Server error: log only. Badge pixel size: [not in file] (Badge has width/height set by LoadPNGTexture).

**RALibretro** (src/Application.cpp): delegates to RA_* functions (RA_DoAchievementsFrame each frame, RA_IsOverlayFullyVisible, RA_NavigateOverlay). No popup or duration visible in the file; popups belong to the external RA library, not read.

### 1.4 Not found
Per-popup pixel sizes and animation constants outside the files above (MSG_QUEUE_ANIMATION_DURATION, DuckStation/PCSX2 fullscreen-UI queue, Dolphin/PPSSPP OSD classes) were not read.

## 2. Badge assets

### 2.1 URL format (rcheevos src/api/rc_api_common.c, around lines 1565-1610)
- Host RETROACHIEVEMENTS_IMAGE_HOST; src/client/rc_client_external.c comment: "https://media.retroachievements.org/Badge/123456_lock.png is 58 with null terminator".
- RC_IMAGE_TYPE_GAME: `/Images/<name>.png`; ACHIEVEMENT: `/Badge/<name>.png`; ACHIEVEMENT_LOCKED: `/Badge/<name>_lock.png`; USER: `/UserPic/<name>.png`.
- Achievement badge_name is 8 chars (rc_client_achievement_t), game badge_name 16 (lines 358, 428).

### 2.2 Measured files (curl from media.retroachievements.org, 2026-10-12; 7 arbitrary badge ids 01234, 50000, 100000, 200000, 300000, 400000, 500000, each unlocked and _lock; ids 00001 and 00002 returned an HTML error, 146 B)
- All 14: PNG, 64 x 64, 8-bit/colour RGB, non-interlaced.
- Unlocked sizes (B): 6053, 5366, 2723, 7676, 8418, 6508, 2929; mean 5668, range 2723-8418.
- Locked sizes (B): 3969, 2728, 1818, 4039, 3783, 3913, 2715; mean 3281, range 1818-4039.
- Game icon (Images/000001.png): PNG 96 x 96, 8-bit grayscale, 3127 B. One sample only.
- Source PNG per set: 100 achievements = 100 x 5668 = 567 KB unlocked, +328 KB locked.

### 2.3 Conversion arithmetic (uncompressed)
Tile-ordered (8x8 tiles), 4bpp = 16 colours + 32 B palette (15-bit); 8bpp = 256 colours + 512 B palette; RGB555 = 2 B/pixel, no palette.
| Size | pixels | 4bpp + pal | 8bpp + pal | RGB555 |
|---|---|---|---|---|
| 16x16 | 256 | 128+32 = 160 B | 256+512 = 768 B | 512 B |
| 32x32 | 1024 | 512+32 = 544 B | 1024+512 = 1536 B | 2048 B |
| 64x64 | 4096 | 2048+32 = 2080 B | 4096+512 = 4608 B | 8192 B |
A locked variant that shares the tile data and only swaps palette adds 32 B (4bpp) or 512 B (8bpp) per badge; a separate locked image doubles the figures. DS 4bpp tile = 32 B, 8bpp tile = 64 B.

Per set, uncompressed, unlocked images only (KB = 1000 B; 50 / 100 / 250 achievements):
| Format | 50 | 100 | 250 |
|---|---|---|---|
| 16x16 4bpp | 8.0 | 16.0 | 40.0 |
| 16x16 8bpp | 38.4 | 76.8 | 192 |
| 16x16 RGB555 | 25.6 | 51.2 | 128 |
| 32x32 4bpp | 27.2 | 54.4 | 136 |
| 32x32 8bpp | 76.8 | 153.6 | 384 |
| 32x32 RGB555 | 102.4 | 204.8 | 512 |
| 64x64 4bpp | 104 | 208 | 520 |
| 64x64 8bpp | 230.4 | 460.8 | 1152 |
| 64x64 RGB555 | 409.6 | 819.2 | 2048 |
Comparison: DSCM heap ~1.2 MB (docs/overlay-research.md); one 100-badge set at 64x64 RGB555 is 819 KB.

### 2.4 Measured compressed sizes
Method (scratchpad/conv.py, Pillow Lanczos resize, median-cut quantise without dither, tile order; BIOS-format LZ10 via a greedy match search written for this test, and BIOS RLE). Sample = the 14 files above, locked and unlocked mixed, so these are a small-sample indication only. Palette bytes included. Bytes per badge:
| Format | raw | LZ10 mean (min-max) | RLE mean |
|---|---|---|---|
| 16x16 4bpp | 160 | 174 (150-180) | 161 |
| 16x16 8bpp | 768 | 782 (695-804) | 761 |
| 16x16 RGB555 | 512 | 351 (134-562) | 488 |
| 32x32 4bpp | 544 | 520 (347-605) | 504 |
| 32x32 8bpp | 1536 | 1448 (1020-1661) | 1425 |
| 32x32 RGB555 | 2048 | 1041 (378-1978) | 1899 |
| 64x64 4bpp | 2080 | 1505 (806-2012) | 1664 |
| 64x64 8bpp | 4608 | 3368 (1884-4789) | 3659 |
| 64x64 RGB555 | 8192 | 2711 (1236-5625) | 7445 |
Per-set at LZ10 mean (50/100/250 achievements): 16x16 4bpp 8.7/17.4/43.5 KB; 32x32 4bpp 26.0/52.0/130 KB; 64x64 4bpp 75.3/150.5/376 KB; 64x64 8bpp 168/337/842 KB; 64x64 RGB555 136/271/678 KB.
BIOS decompression facts: SWI LZ77 (LZ10) output must be written with 8-bit (WRAM) or 16-bit (VRAM) variants; Huffman and RunLength also exist (swi 0x10-0x15). Huffman not measured. Per-badge RAM when decompressed is the "raw" column.

## 3. Fonts

### 3.1 nds-bootstrap in-game menu font
- Structure: `u8 font[256 * 8]` in IgmText (retail/common/include/igm_text.h) = 2048 B: 256 glyphs, 8x8, 1bpp (IgmText total 0xF40 B, static_assert).
- Load (retail/arm9/source/conf_sd.cpp ~585-625): ascii.lz77 then a per-language extended font, both LZ77-decompressed into igmText->font. Fonts in retail/nitrofiles/fonts/ (compressed bytes): ascii 630, extended_latin 546, arabic 659, chinese 696, cyrillic 535, greek 451, hangul 410, hebrew 268, japanese 554, vietnamese 487. Choice by conf->guiLanguage (ar, zh, ru/uk, el, ko, he, ja/ry, vi, else extended Latin). IgmFont enum: arabic 0, chinese 1, cyrillic 2, extendedLatin 3, greek 4, hangul 5, hebrew 6, japanese 7, vietnamese 8. Only one 256-glyph set is resident at a time.
- Display (retail/cardenginei/arm9_igm/source/inGameMenu.c ~931): 1bpp expanded to 4bpp tiles in sub-screen BG graphics: 2048 B x 4 = 8192 B of VRAM (bgBak backs up the same 8192 B); BG map at BG_MAP_RAM_SUB(15), 32x24 tile text grid, each map entry = glyph index | palette<<12; 6 palettes (FONT_WHITE, LIGHT_GRAY, DARKER_GRAY, LIGHT_BLUE, RED, LIME). Text is single-byte (one char = one tile), fixed 8x8; print() functions take unsigned char strings. printMsg wraps at 32 columns.
- The same font is used by the pre-boot placard (docs/overlay-research.md) and by arm9_igm/dscheevosMenu.c.

### 3.2 Byte arithmetic for other bitmap fonts (1bpp, one byte per row)
- 8x8, 256 glyphs: 2048 B (as nds-bootstrap). 8x8, 128 glyphs: 1024 B. 8x8 ASCII 95 printable: 760 B.
- 6x8 (stored 1 byte/row): 95 glyphs 760 B; 191 glyphs (ASCII + Latin-1 printable) 1528 B. Same byte count as 8x8 unless packed to 6 bits/row (then 95 x 8 x 6 / 8 = 570 B).
- Proportional: add 1 width byte per glyph (+95 or +256 B).
- 4bpp tile expansion in VRAM: 32 B per 8x8 glyph; 256 glyphs = 8192 B; 95 glyphs = 3040 B.
- Pixel-width fit: 256 px / 8 = 32 columns; 256 / 6 = 42 columns; 192 / 8 = 24 rows.
- Strings in DSCM: title[48] bytes UTF-8 (cut to fit), description[100], notice text/detail 48 each, tracker text 24, measured 16. dscheevos_module.h says UTF-8; nds-bootstrap's font path is single-byte per glyph.
- Glyph counts, from general knowledge (not checked against a source here): Latin-1 Supplement printable 96, Latin Extended-A 128; kana 83 hiragana + 86 katakana approx 170; JIS X 0208 has 6879 characters; an 8x8 glyph is 8 B each, so 6879 x 8 = 55,032 B. RA title language coverage was not measured; no RA source was read on this.

## 4. Not done
Item 4 (API) dropped per coordinator. No emulators, tests or git run; no repo edits. Scratch files: scratchpad/conv.py, scratchpad/badges/*.png.
