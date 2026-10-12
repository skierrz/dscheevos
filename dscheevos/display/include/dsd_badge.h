/* Badges: the pack file the launcher writes before boot, and the small
 * module-side part that draws one slot. Design: docs/display-badges.md. */
#ifndef DSD_BADGE_H
#define DSD_BADGE_H

#include <stdint.h>
#include "dsd.h"

#define DSD_BADGE_SIZE 32             /* pixels, square */
#define DSD_BADGE_SLOT 544            /* bytes per slot: 512 of 4bpp pixels + 16 BGR555 colours */

/* One badge as stored: 4bpp, row-major, low nibble = left pixel. */
typedef struct dsd_badge_t {
  uint8_t pixels[DSD_BADGE_SIZE * DSD_BADGE_SIZE / 2];
  uint16_t palette[16];
} dsd_badge_t;

/* ---- pack file: sd:/_nds/dscheevos/badges/<game_id>.pak (little-endian) */
#define DSD_PACK_MAGIC 0x42435344u   /* "DSCB" */
#define DSD_PACK_VERSION 1
#define DSD_PACK_NAME 16              /* badge_name bytes per slot, NUL-padded */

typedef struct dsd_pack_header_t {
  uint32_t magic;
  uint16_t version;
  uint16_t slot_size;                 /* DSD_BADGE_SLOT */
  uint32_t game_id;
  uint32_t count;                     /* slots */
  uint32_t ids_offset;                /* count x uint32, ascending; slot i belongs to ids[i] */
  uint32_t names_offset;              /* count x DSD_PACK_NAME */
  uint32_t slots_offset;              /* 512-aligned; count x slot_size */
  uint32_t large_offset;              /* 64x64 section, 0 = none (reserved) */
} dsd_pack_header_t;                  /* 32 bytes */

/* Slot index of an achievement in the pack's id table, or -1. */
int dsd_pack_find(const uint32_t* ids, uint32_t count, uint32_t id);

/* The locked look: grey and darker, from the unlocked palette. */
void dsd_badge_locked_palette(const uint16_t in[16], uint16_t out[16]);

/* Draw a badge at (x, y), clipped to the surface. */
void dsd_badge_blit(const dsd_badge_t* b, int locked, dsd_surface_t* s, int x, int y);

#endif
