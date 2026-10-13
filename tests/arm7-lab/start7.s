@ arm7-lab payload entry, linked at the image base (payload.ld).
@ r0 = lab7_params_t*. Copies the fast section to ARM7 WRAM, zeroes bss,
@ runs bench7_main(r0) on the caller's (WRAM) stack and returns.
  .section .text.entry, "ax"
  .arm
  .global payload_entry
payload_entry:
  push {r4, lr}
  mov r4, r0
  @ ARM7 NWRAM windows (the ARM9 gave the ARM7 all NWRAM-B/C slots): B at
  @ 0x03100000, C at 0x03140000, 256 KB each, so .fast may live there
  ldr r0, =0x04004058
  ldr r1, =((0x20 << 3) | (3 << 12) | (0x28 << 19))
  str r1, [r0]
  ldr r1, =((0x28 << 3) | (3 << 12) | (0x30 << 19))
  str r1, [r0, #4]
  ldr r0, =__fast_vma
  ldr r1, =__fast_lma
  ldr r2, =__fast_end
1:cmp r0, r2
  ldrlo r3, [r1], #4
  strlo r3, [r0], #4
  blo 1b
  ldr r0, =__bss_start
  ldr r1, =__bss_end
  mov r3, #0
2:cmp r0, r1
  strlo r3, [r0], #4
  blo 2b
  mov r0, r4
  ldr r3, =bench7_main
  mov lr, pc
  bx r3
  pop {r4, lr}
  bx lr
  .pool
