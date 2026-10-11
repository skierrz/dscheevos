/* DScheevos ARM9 module: linked at DSCM_BASE, header first. */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(dscm_header)

MEMORY {
  image : ORIGIN = 0x02D80000, LENGTH = 0x00040000
}

SECTIONS {
  .header : { KEEP(*(.header)) } > image
  .text   : { *(.text.entries) *(.text .text.* .gnu.linkonce.t.*) *(.glue_7) *(.glue_7t) *(.vfp11_veneer) *(.v4_bx) } > image
  .rodata : { *(.rodata .rodata.* .gnu.linkonce.r.*) } > image
  .ARM.exidx : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } > image
  .init_array : { KEEP(*(.init_array*)) } > image
  .data   : { *(.data .data.* .gnu.linkonce.d.*) } > image
  . = ALIGN(4);
  __image_end = .;
  .bss (NOLOAD) : {
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
