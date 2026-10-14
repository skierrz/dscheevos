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

/* Offsets from the way boundaries (make TEXT_COLOUR=.. DATA_COLOUR=..), from a
 * sweep of the game's own cycles (tests/perf/gamecost.sh, melonDS cache model):
 * text 0x400 / data 0 cost Kirby +67.0k cycles a frame (0x000-0x700: 67-100k)
 * and MKDS +8.1k (offset 0: +21.7k). Re-swept with the data mirror and the
 * change-driven rcheevos (AB.md "Combined build"): 0x400 / 0 kept (Kirby
 * +19.2k, all colours but text 0x500 within 3k). Re-sweep when the hot code
 * changes size a lot; see tests/perf/AB.md "Cache colours". */
DSCM_TEXT_COLOUR = DEFINED(DSCM_TEXT_COLOUR) ? DSCM_TEXT_COLOUR : 0x400;
DSCM_DATA_COLOUR = DEFINED(DSCM_DATA_COLOUR) ? DSCM_DATA_COLOUR : 0;

SECTIONS {
  .header : { KEEP(*(.header)) } > image
  /* Fixed cache colours. The game's own cost depends on which cache sets our
   * hot code and data share with its own (Kirby: a 0x140-byte shift of the
   * rcheevos code cost the game 2.5% of a frame). So the hot code starts on
   * an I-cache way boundary (8 KB 4-way: 2 KB), rcheevos' evaluator first,
   * then the core: edits elsewhere can't move them. Data sections start on a
   * D-cache way boundary (4 KB 4-way: 1 KB); the low heap follows .bss. */
  .text   : {
    *(.text.irq) *(.text.entries) *(.text.wake) *(.text.wake.core) *(.data.cached)
    . = ALIGN(0x800) + DSCM_TEXT_COLOUR;
    *rc/runtime/*.o(.text .text.*) *rc/client/*.o(.text .text.*) *dscheevos.o(.text .text.*)
    *(.text .text.* .gnu.linkonce.t.*) *(.glue_7) *(.glue_7t) *(.vfp11_veneer) *(.v4_bx)
  } > image
  .ARM.exidx : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } > image
  .init_array : { KEEP(*(.init_array*)) } > image
  /* constants stay cached: rcheevos reads its tables for every operand */
  .rodata ALIGN(0x400) + DSCM_DATA_COLOUR : { *(.rodata .rodata.* .gnu.linkonce.r.*) } > image
  __text_end = .;
  /* Data at the 0x0C000000 mirror (VMA), stored right after the code and
   * constants (LMA): the same RAM, read and written past the caches on games
   * whose MPU leaves the mirror uncached. */
  .data   (ALIGN(__text_end, 0x400) + DSCM_DATA_COLOUR + 0x0A000000) : AT(ALIGN(__text_end, 0x400) + DSCM_DATA_COLOUR) { *(.data.wake) *(.data.irq) *(.data .data.* .gnu.linkonce.d.*) }
  . = ALIGN(4);
  __image_end = . - 0x0A000000;
  .bss ALIGN(0x400) + DSCM_DATA_COLOUR (NOLOAD) : {
    __bss_start = .;
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
