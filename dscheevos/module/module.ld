/* DScheevos ARM9 module: linked at DSCM_BASE, header first. */
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

SECTIONS {
  .header : { KEEP(*(.header)) } > image
  /* Fixed cache colours. The game's own cost depends on which cache sets our
   * hot code and data share with its own (Kirby: a 0x140-byte shift of the
   * rcheevos code cost the game 2.5% of a frame). So the hot code starts on
   * an I-cache way boundary (8 KB 4-way: 2 KB), rcheevos' evaluator first,
   * then the core: edits elsewhere can't move them. Data sections start on a
   * D-cache way boundary (4 KB 4-way: 1 KB); the low heap follows .bss. */
  .text   : {
    *(.text.irq) *(.data.irq) *(.text.entries) *(.text.wake)
    . = ALIGN(0x800) + DSCM_TEXT_COLOUR;
    *rc/runtime/*.o(.text .text.*) *rc/client/*.o(.text .text.*) *dscheevos.o(.text .text.*)
    *(.text .text.* .gnu.linkonce.t.*) *(.glue_7) *(.glue_7t) *(.vfp11_veneer) *(.v4_bx)
  } > image
  .rodata ALIGN(0x400) + DSCM_DATA_COLOUR : { *(.rodata .rodata.* .gnu.linkonce.r.*) } > image
  .ARM.exidx : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } > image
  .init_array : { KEEP(*(.init_array*)) } > image
  .data   ALIGN(0x400) + DSCM_DATA_COLOUR : { *(.data .data.* .gnu.linkonce.d.*) } > image
  . = ALIGN(4);
  __image_end = .;
  .bss ALIGN(0x400) + DSCM_DATA_COLOUR (NOLOAD) : {
    __bss_start = .;
    *(.bss .bss.* .gnu.linkonce.b.*) *(COMMON)
    . = ALIGN(4);
    __bss_end = .;
  } > image
  __image_size = __image_end - ORIGIN(image);
  __stack_top = 0x02D80000 + 0x00200000;
  __irq_stack_top = __stack_top - 0x00002000;
  /DISCARD/ : { *(.comment) *(.note*) }
}
