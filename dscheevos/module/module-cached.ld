/* DScheevos ARM9 module, all of it through the cache (make DATA_MIRROR=0): linked at DSCM_BASE, header first. */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(dscm_header)

MEMORY {
  image : ORIGIN = 0x02D80000, LENGTH = 0x00080000
}

/* Offsets from the way boundaries (make TEXT_COLOUR=.. DATA_COLOUR=..), from a
 * sweep of the game's own cycles (tests/perf/gamecost.sh, melonDS cache model):
 * text 0x400 / data 0 cost Kirby +67.0k cycles a frame (0x000-0x700: 67-100k)
 * and MKDS +8.1k (offset 0: +21.7k). Re-sweep when the hot code changes size
 * a lot; see tests/perf/AB.md "Cache colours". */
DSCM_TEXT_COLOUR = DEFINED(DSCM_TEXT_COLOUR) ? DSCM_TEXT_COLOUR : 0x400;
DSCM_DATA_COLOUR = DEFINED(DSCM_DATA_COLOUR) ? DSCM_DATA_COLOUR : 0;
/* the hot region's size limit (hot.ld today: 0x9d4a) */
DSCM_HOT_BUDGET = DEFINED(DSCM_HOT_BUDGET) ? DSCM_HOT_BUDGET : 0xB000;

SECTIONS {
  .header : { KEEP(*(.header)) } > image
  /* The layout firewall (research/optimize/layout.md). What the module costs the
   * game depends on which cache sets our hot code and data share with the game's
   * (Kirby: a 0x118-byte shift of newlib's code cost +12.5k cycles a frame). So the
   * code that runs in play sits at addresses that cold or load-time code can't move:
   *  - the wake block (IRQ, entries, wake path) first, as the runner expects;
   *  - from a 2 KB I-cache way boundary plus TEXT_COLOUR, the hot code (hot.ld:
   *    called at least once per 20 frames in play; third-party first, then ours,
   *    the core last), then the warm code (warm.ld: run in play, rarely);
   *  - then everything else: load-time, TLS, cold.
   * hot.ld and warm.ld come from the runner's per-PC profiles of real games
   * (tests/perf/hotlist.py). The build checks the hot addresses against
   * hot-layout.ref (Makefile hotcheck) and the region's size against its budget. */
  .text   : {
    *(.data.wake) *(.text.irq) *(.data.irq) *(.text.entries) *(.text.wake) *(.text.wake.core)
    . = ALIGN(0x800) + DSCM_TEXT_COLOUR;
    __hot_start = .;
    INCLUDE hot.ld
    __hot_end = .;
    INCLUDE warm.ld
    __warm_end = .;
    *(.text .text.* .gnu.linkonce.t.*) *(.glue_7) *(.glue_7t) *(.vfp11_veneer) *(.v4_bx)
    *(.text_cold)
  } > image
  ASSERT(__hot_end - __hot_start <= DSCM_HOT_BUDGET, "module.ld: the hot code is over its budget (DSCM_HOT_BUDGET): trim it or raise the budget and re-measure the game cost")
  .rodata ALIGN(0x400) + DSCM_DATA_COLOUR : {
    __hot_rodata_start = .;
    INCLUDE hot-rodata.ld
    __hot_rodata_end = .;
    *(.rodata .rodata.* .gnu.linkonce.r.*)
  } > image
  .ARM.exidx : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } > image
  .init_array : { KEEP(*(.init_array*)) } > image
  .data   ALIGN(0x400) + DSCM_DATA_COLOUR : { *(.data .data.* .gnu.linkonce.d.*) } > image
  . = ALIGN(4);
  __image_end = .;
  .bss ALIGN(0x400) + DSCM_DATA_COLOUR (NOLOAD) : {
    __bss_start = .;
    *(.bss.S_storage)   /* the core state first: module.c's statics can't move it */
    *(.bss .bss.* .gnu.linkonce.b.*) *(COMMON)
    . = ALIGN(4);
    __bss_end = .;
  } > image
  __heap_start = __bss_end;
  __image_size = __image_end - ORIGIN(image);
  /* the journal's physical address, for the ARM7 (the header's other data
   * pointers are the module's own view, for the ARM9's in-game menu) */
  dscm_g_journal_phys = (dscm_g_journal & 0x00FFFFFF) | 0x02000000;
  __stack_top = 0x02D80000 + 0x00200000;
  __irq_stack_top = __stack_top - 0x00002000;
  /DISCARD/ : { *(.comment) *(.note*) }
}
