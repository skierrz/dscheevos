@ DScheevos module header and the ARM entry points the hooks call.
	.syntax unified
	.arch armv5te

	.section .header, "ax"
	.global dscm_header
	.align 2
dscm_header:
	.word 0x56435344            @ DSCM_MAGIC "DSCV"
	.word 6                     @ DSCM_VERSION (the module test checks it matches)
	.word __image_size
	.word init_entry
	.word vblank_entry
	.word halt_entry
	.word module_shutdown
	.word dscm_g_status
	.word dscm_g_net
dscm_vblank_count:
	.word 0                     @ 0x24: bumped by the ARM7 VBlank hook
	.word reset_entry           @ 0x28
	.global dscm_list_ptr
dscm_list_ptr:
	.word 0                     @ 0x2C: published achievement list (module.c swaps it)
	.word dscm_g_notices        @ 0x30
	.word dscm_g_journal        @ 0x34: written through the uncached mirror
	.space 8
	.global dscm_notify_count
dscm_notify_count:
	.word 0                     @ 0x40: unlock count for the ARM7 (own cache line)
	.global dscm_net_ring
dscm_net_ring:
	.word 0                     @ 0x44: network ring for the ARM7 SDIO shim
	.space 24

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

@ Reset: called from nds-bootstrap's reset path with IRQs off. The game's
@ threads are gone, so work that was preempted will never resume: clear
@ the re-entry guards. The achievement reset itself happens in idle time.
	.global reset_entry
	.type reset_entry, %function
reset_entry:
	ldr	r0, =in_vblank
	mov	r1, #0
	str	r1, [r0]
	ldr	r0, =in_halt
	str	r1, [r0]
	ldr	r0, =module_reset
	bx	r0

@ void dscm_publish_notify(uint32_t count): store the unlock count and clean
@ its cache line to RAM so the ARM7 sees it.
	.global dscm_publish_notify
	.type dscm_publish_notify, %function
dscm_publish_notify:
	ldr	r1, =dscm_notify_count
	str	r0, [r1]
	mcr	p15, 0, r1, c7, c10, 1      @ clean D-cache line
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4      @ drain write buffer
	bx	lr

@ void dscm_publish_net_ring(void* ring): as above, for the ring pointer.
	.global dscm_publish_net_ring
	.type dscm_publish_net_ring, %function
dscm_publish_net_ring:
	ldr	r1, =dscm_net_ring
	str	r0, [r1]
	mcr	p15, 0, r1, c7, c10, 1
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4
	bx	lr

@ void dscm_publish_list(void* list): store the list pointer (header 0x2C)
@ through the uncached mirror, then drop the cached line. The ARM9 never
@ writes that line through the cache: the vblank_count read invalidates it.
	.global dscm_publish_list
	.type dscm_publish_list, %function
dscm_publish_list:
	ldr	r1, =dscm_list_ptr
	add	r2, r1, #0x0A000000         @ uncached mirror (0x0C000000)
	str	r0, [r2]
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4      @ drain write buffer
	mcr	p15, 0, r1, c7, c6, 1       @ invalidate D-cache line
	bx	lr

@ void dscm_dc_clean(const void* addr, uint32_t len): write cached lines to RAM.
	.global dscm_dc_clean
	.type dscm_dc_clean, %function
dscm_dc_clean:
	add	r1, r0, r1
	bic	r0, r0, #31
1:	cmp	r0, r1
	bhs	2f
	mcr	p15, 0, r0, c7, c10, 1
	add	r0, r0, #32
	b	1b
2:	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4
	bx	lr

@ void dscm_dc_invalidate(const void* addr, uint32_t len): drop cached lines
@ so the next read comes from RAM. Only for lines the ARM9 never writes.
	.global dscm_dc_invalidate
	.type dscm_dc_invalidate, %function
dscm_dc_invalidate:
	add	r1, r0, r1
	bic	r0, r0, #31
1:	cmp	r0, r1
	bhs	2f
	mcr	p15, 0, r0, c7, c6, 1
	add	r0, r0, #32
	b	1b
2:	bx	lr

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
