/* DScheevos ARM9 module: linked at DSCM_BASE, header first. Code runs at
 * 0x02D80000 (cached); the data sections are linked at the 0x0C000000 mirror
 * of the same RAM (DSCM_DATA_MIRROR, uncached on most games' MPUs), stored
 * right after the code. module-cached.ld: everything at 0x02D80000. */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(dscm_header)

MEMORY {
  image : ORIGIN = 0x02D80000, LENGTH = 0x00080000
}

/* Cache colours: offsets from the way boundaries (I-cache 2 KB, D-cache 1 KB),
 * chosen by the game's own cycles (tests/perf/gamecost.sh, melonDS cache model,
 * research/optimize/layout.md): Kirby SSU swept over wake x text (9 x 8) and the
 * pointer line (33 positions), MKDS, Black and Platinum checked at the pick.
 * Each block starts at its own colour, so a size change of one moves none of
 * the others (make CACHED_COLOUR=.. WAKE_COLOUR=.. TEXT_COLOUR=.. DATA_COLOUR=..):
 *   CACHED: the core's pointer line (.data.cached, read every wake), D-cache set;
 *   WAKE:   the wake path (.text.wake, .text.wake.core);
 *   TEXT:   the hot and warm code (hot.ld, warm.ld);
 *   DATA:   constants (hot ones first), data and bss.
 * Kirby is flat (+/-0.3k) over most pointer colours and over text 0x000-0x600
 * with wake 0x100; the wake colour is the sensitive one (17.9k-43k). */
DSCM_TEXT_COLOUR = DEFINED(DSCM_TEXT_COLOUR) ? DSCM_TEXT_COLOUR : 0x500;
DSCM_DATA_COLOUR = DEFINED(DSCM_DATA_COLOUR) ? DSCM_DATA_COLOUR : 0;
DSCM_CACHED_COLOUR = DEFINED(DSCM_CACHED_COLOUR) ? DSCM_CACHED_COLOUR : 0x140;
DSCM_WAKE_COLOUR = DEFINED(DSCM_WAKE_COLOUR) ? DSCM_WAKE_COLOUR : 0x100;
/* the hot region's size limit (hot.ld today: 0x9d88) */
DSCM_HOT_BUDGET = DEFINED(DSCM_HOT_BUDGET) ? DSCM_HOT_BUDGET : 0xB000;

SECTIONS {
  .header : { KEEP(*(.header)) } > image
  /* The layout firewall (research/optimize/layout.md). What the module costs the
   * game depends on which cache sets our hot code and data share with the game's
   * (Kirby: a 0x118-byte shift of newlib's code cost +12.5k cycles a frame). So
   * everything that runs in play sits at addresses that cold or load-time code
   * can't move, each block at its own colour (above):
   *  - the VBlank IRQ code and the entries, first, as the runner expects;
   *  - the core's pointer line (.data.cached), the wake path;
   *  - the hot code (hot.ld: called at least once per 20 frames in play;
   *    third-party first, then ours, the core last), then the warm code
   *    (warm.ld: run in play, rarely);
   *  - then everything else: load-time, TLS, cold.
   * hot.ld, warm.ld and hot-rodata.ld come from the runner's per-PC profiles of
   * real games (tests/perf/hotlist.py). The build checks the hot addresses
   * against hot-layout.ref (Makefile hotcheck) and the hot code's size against
   * DSCM_HOT_BUDGET. */
  .text   : {
    *(.text.irq) *(.text.entries)
    . = ALIGN(0x400) + DSCM_CACHED_COLOUR - (((ABSOLUTE(.) & 0x3FF) != 0 && (ABSOLUTE(.) & 0x3FF) <= DSCM_CACHED_COLOUR) ? 0x400 : 0);
    *(.data.cached)
    . = ALIGN(0x800) + DSCM_WAKE_COLOUR - (((ABSOLUTE(.) & 0x7FF) != 0 && (ABSOLUTE(.) & 0x7FF) <= DSCM_WAKE_COLOUR) ? 0x800 : 0);
    __wake_start = .;
    *(.text.wake) *(.text.wake.core)
    . = ALIGN(0x800) + DSCM_TEXT_COLOUR - (((ABSOLUTE(.) & 0x7FF) != 0 && (ABSOLUTE(.) & 0x7FF) <= DSCM_TEXT_COLOUR) ? 0x800 : 0);
    __hot_start = .;
    INCLUDE hot.ld
    __hot_end = .;
    INCLUDE warm.ld
    __warm_end = .;
    *(.text .text.* .gnu.linkonce.t.*) *(.glue_7) *(.glue_7t) *(.vfp11_veneer) *(.v4_bx)
    *(.text_cold)   /* load-time code added later: last, so it moves nothing above (DSCM_COLD) */
  } > image
  ASSERT(__hot_end - __hot_start <= DSCM_HOT_BUDGET, "module.ld: the hot code is over its budget (DSCM_HOT_BUDGET): trim it or raise the budget and re-measure the game cost")
  .ARM.exidx : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } > image
  .init_array : { KEEP(*(.init_array*)) } > image
  /* constants stay cached: rcheevos reads its tables for every operand */
  .rodata ALIGN(0x400) + DSCM_DATA_COLOUR : {
    __hot_rodata_start = .;
    INCLUDE hot-rodata.ld
    __hot_rodata_end = .;
    *(.rodata .rodata.* .gnu.linkonce.r.*)
  } > image
  __text_end = .;
  /* Data at the 0x0C000000 mirror (VMA), stored right after the code and
   * constants (LMA): the same RAM, read and written past the caches on games
   * whose MPU leaves the mirror uncached. */
  .data   (ALIGN(__text_end, 0x400) + DSCM_DATA_COLOUR + 0x0A000000) : AT(ALIGN(__text_end, 0x400) + DSCM_DATA_COLOUR) { *(.data.wake) *(.data.irq) *(.data .data.* .gnu.linkonce.d.*) }
  . = ALIGN(4);
  __image_end = . - 0x0A000000;
  .bss ALIGN(0x400) + DSCM_DATA_COLOUR (NOLOAD) : {
    __bss_start = .;
    *(.bss.S_storage)   /* the core state first: module.c's statics can't move it */
    *(.bss .bss.* .gnu.linkonce.b.*) *(COMMON)
    . = ALIGN(4);
    __bss_end = .;
  }
  __heap_start = __bss_end;
  __image_size = __image_end - ORIGIN(image);
  /* the journal's physical address, for the ARM7 (the header's other data
   * pointers are the module's own view, for the ARM9's in-game menu) */
  dscm_g_journal_phys = (dscm_g_journal & 0x00FFFFFF) | 0x02000000;
  /* the idle and init stack stays cached: rcheevos pushes and pops a lot
   * (through the mirror, Kirby's evaluation took 80% longer) */
  __stack_top = 0x02D80000 + 0x00200000;
  __irq_stack_top = __stack_top - 0x00002000;
  /DISCARD/ : { *(.comment) *(.note*) }
}
