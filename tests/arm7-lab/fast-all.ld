/* all code and read-only data (NWRAM builds: the region is 256 KB of NWRAM) */
*(.text .text.* .gnu.linkonce.t.*)
*(.glue_7) *(.glue_7t) *(.v4_bx)
*(.rodata .rodata.* .gnu.linkonce.r.*)
