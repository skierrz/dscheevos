/* Badge packs, launcher side: convert RetroAchievements badge PNGs to
 * 32x32 4bpp slots and write or update a game's pack (before boot only).
 * Built only with the badge pipeline enabled (make BADGES=1); not yet wired
 * into the launcher. Design: docs/display-badges.md. */
#ifndef DSD_BADGEPACK_H
#define DSD_BADGEPACK_H

#include <stddef.h>
#include <stdint.h>
#include "dsd_badge.h"

typedef struct dsdbp_entry_t {
  uint32_t id;                        /* achievement id */
  char name[DSD_PACK_NAME + 1];       /* RA badge_name, NUL-terminated */
  dsd_badge_t badge;
} dsdbp_entry_t;

/* Pixels (comp 3 = RGB, 4 = RGBA composited over black) of any size >= 1x1
 * to one slot: area-average down to 32x32, then a 16-colour median cut in
 * the DS's 15-bit colour space. Deterministic. Returns 0. */
int dsdbp_convert_pixels(const uint8_t* px, int w, int h, int comp, dsd_badge_t* out);

/* A PNG file's bytes to one slot. Returns 0, or -1 if it can't be decoded. */
int dsdbp_convert_png(const void* data, size_t len, dsd_badge_t* out);

/* The same at 16x16, for the thin bar (the launcher's session badges). */
int dsdbp_convert_pixels16(const uint8_t* px, int w, int h, int comp, dsd_badge16_t* out);
int dsdbp_convert_png16(const void* data, size_t len, dsd_badge16_t* out);

/* Writes a pack (entries in any order; sorted by id here) to path.tmp and
 * renames it over path, so a half-written pack never replaces a good one.
 * Duplicate ids keep the first. Returns 0 or -1. */
int dsdbp_write(const char* path, uint32_t game_id, const dsdbp_entry_t* e, uint32_t count);

/* Reads a whole pack for an update. Checks magic, version, sizes, game id
 * and that every table lies inside the file with ids ascending; anything
 * else counts as no pack. Returns the entry count (caller frees *out), or -1. */
int dsdbp_read(const char* path, uint32_t game_id, dsdbp_entry_t** out);

#endif
