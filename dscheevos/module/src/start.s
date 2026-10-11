@ DScheevos module header and the ARM entry points the hooks call.
	.syntax unified
	.arch armv5te

	.section .header, "ax"
	.global dscm_header
	.align 2
dscm_header:
	.word 0x56435344            @ DSCM_MAGIC "DSCV"
	.word 2                     @ DSCM_VERSION
	.word __image_size
	.word init_entry
	.word vblank_entry
	.word halt_entry
	.word module_shutdown
	.word dscm_g_status
	.word dscm_g_net
dscm_vblank_count:
	.word 0                     @ bumped by the ARM7 VBlank hook

	.section .text.entries, "ax"
	.arm

@ Init: called once by nds-bootstrap's ARM9 before the game starts, with
@ the session file in r0. Its stack is tiny, so run on the module stack.
	.global init_entry
	.type init_entry, %function
init_entry:
	push	{r4, lr}
	mov	r4, sp
	ldr	sp, =__stack_top
	push	{r4, r5}
	ldr	r1, =module_init
	blx	r1
	pop	{r4, r5}
	mov	sp, r4
	pop	{r4, lr}
	bx	lr

@ VBlank: called from the ARM9 IPC sync IRQ handler. Runs module_vblank on
@ the module's IRQ stack, so the game's small DTCM IRQ stack is untouched.
	.global vblank_entry
	.type vblank_entry, %function
vblank_entry:
	push	{r4, r5, lr}
	ldr	r5, =in_vblank
	ldr	r0, [r5]
	cmp	r0, #0
	bne	1f
	mov	r0, #1
	str	r0, [r5]
	mov	r4, sp
	ldr	sp, =__irq_stack_top
	push	{r4, r5}
	ldr	r0, =module_vblank
	blx	r0
	pop	{r4, r5}
	mov	sp, r4
	mov	r0, #0
	str	r0, [r5]
1:	pop	{r4, r5, lr}
	bx	lr

@ Halt: the game's OS_Halt jumps here. Run idle work on the module stack,
@ then wait for an interrupt as OS_Halt does. A game thread woken by an IRQ
@ preempts the idle thread (and this work) as usual. If the work is already
@ running (preempted, then OS_Halt called again), just halt.
	.global halt_entry
	.type halt_entry, %function
halt_entry:
	push	{r4, r5, lr}
	ldr	r5, =in_halt
	ldr	r0, [r5]
	cmp	r0, #0
	bne	2f
	mov	r0, #1
	str	r0, [r5]
	mov	r4, sp
	ldr	sp, =__stack_top
	push	{r4, r5}
	ldr	r0, =module_idle
	blx	r0
	pop	{r4, r5}
	mov	sp, r4
	mov	r0, #0
	str	r0, [r5]
2:	mov	r0, #0
	mcr	p15, 0, r0, c7, c0, 4       @ wait for interrupt
	pop	{r4, r5, lr}
	bx	lr

@ uint32_t dscm_vblank_count_read(void): the ARM7 writes the count behind
@ the data cache, so drop the cached line first. Nothing on the ARM9 writes
@ that line (header words and code), so the invalidate loses no data.
	.global dscm_vblank_count_read
	.type dscm_vblank_count_read, %function
dscm_vblank_count_read:
	ldr	r1, =dscm_vblank_count
	mcr	p15, 0, r1, c7, c6, 1
	ldr	r0, [r1]
	bx	lr

@ uint32_t dscm_dtcm_base(void): the game's DTCM base from CP15.
	.global dscm_dtcm_base
	.type dscm_dtcm_base, %function
dscm_dtcm_base:
	mrc	p15, 0, r0, c9, c1, 0
	lsr	r0, r0, #12
	lsl	r0, r0, #12
	bx	lr

	.pool

	.bss
	.align 2
in_vblank:	.word 0
in_halt:	.word 0
