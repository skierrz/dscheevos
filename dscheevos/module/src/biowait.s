@ BIOS-wait gate (docs/idle-coverage/decision.md 2.1-2.2).
@
@ nds-bootstrap rewrites the game's NitroSDK SVC_WaitVBlankIntr wrapper
@ (Thumb: movs r2, #0; svc 5; bx lr) to jump to header word 0, which branches
@ here (ARM, caller's mode, lr = the caller's return). r0-r3 and r12 are the
@ wrapper's scratch, as for any call.
@
@ Stage 1 of the gate, chosen by dscm_bw.mode:
@   BW_PASS (0): count the call, then the real BIOS VBlankIntrWait.
@   BW_E    (1): E, the same wait done here: IntrWait(1, 1) as the ARM9 BIOS
@                does it on the game's own call: IME = 1, discard the VBlank
@                bit of the BIOS IRQ check flags (DTCM+3FF8), halt until an IRQ
@                handler sets it, clear it and return. IRQs are taken in the
@                caller's mode, as the BIOS's SWI handler does (it runs the
@                function in SYS mode with IRQs on). The wait itself is where
@                later stages run DScheevos' work.
@ A caller in a mode other than SYS (never seen) always gets the real BIOS.
	.syntax unified
	.arch armv5te
	.arm

	.equ	BW_MODE, 0
	.equ	BW_CALLS, 4
	.equ	BW_WAITS, 8          @ calls that halted at least once (flag not already set)
	.equ	BW_HALTS, 12         @ halts in E (one per IRQ that didn't end the wait)
	.equ	BW_ENTRY_LINE, 16    @ VCOUNT at the last call
	.equ	BW_PASS, 0
	.equ	BW_E, 1

	.section .text.entries, "ax"
	.global bw_entry
	.type bw_entry, %function
bw_entry:
	ldr	r12, =dscm_bw
	ldr	r2, [r12, #BW_CALLS]
	add	r2, r2, #1
	str	r2, [r12, #BW_CALLS]
	mov	r3, #0x04000000
	ldrh	r2, [r3, #6]                @ REG_VCOUNT
	str	r2, [r12, #BW_ENTRY_LINE]
	ldr	r2, [r12, #BW_MODE]
	cmp	r2, #BW_E
	bne	bw_bios
	mrs	r0, cpsr
	and	r1, r0, #0x1F
	cmp	r1, #0x1F                   @ SYS (USR can't change its own I bit)
	bne	bw_bios
	push	{r4, r5}
	mov	r4, r0                      @ caller's CPSR, put back on return
	orr	r1, r0, #0x80
	msr	cpsr_c, r1                  @ IRQs off while testing the flag
	bic	r5, r0, #0x80               @ the caller's mode with IRQs on, for the wait
	mrc	p15, 0, r2, c9, c1, 0       @ DTCM region: base in bits 31-12
	lsr	r2, r2, #12
	lsl	r2, r2, #12
	add	r2, r2, #0x4000             @ DTCM end: check flags at -8
	mov	r1, #1
	str	r1, [r3, #0x208]            @ REG_IME = 1
	ldr	r0, [r2, #-8]
	bic	r0, r0, #1                  @ discard an old VBlank
	str	r0, [r2, #-8]
	ldr	r1, [r12, #BW_WAITS]
	add	r1, r1, #1
	str	r1, [r12, #BW_WAITS]
1:	ldr	r0, [r2, #-8]
	tst	r0, #1
	bne	2f
	mov	r0, #0
	mcr	p15, 0, r0, c7, c0, 4       @ halt until an IRQ is requested
	msr	cpsr_c, r5                  @ take it here
	orr	r1, r5, #0x80
	msr	cpsr_c, r1
	mov	r1, #1
	str	r1, [r3, #0x208]            @ the BIOS keeps IME = 1 for the wait
	ldr	r1, [r12, #BW_HALTS]
	add	r1, r1, #1
	str	r1, [r12, #BW_HALTS]
	b	1b
2:	bic	r0, r0, #1                  @ consume the VBlank
	str	r0, [r2, #-8]
	msr	cpsr_c, r4
	pop	{r4, r5}
	bx	lr

@ The real BIOS VBlankIntrWait, as the stock wrapper did (r2 = 0: the DSi
@ BIOS's extra IRQ mask; nothing).
bw_bios:
	mov	r2, #0
	svc	0x050000
	bx	lr
	.ltorg

	.bss
	.align 2
	.global dscm_bw
dscm_bw:
	.space	20
